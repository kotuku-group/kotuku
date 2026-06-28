/*********************************************************************************************************************

-CLASS-
RemapFX: Provides pixel remapping; equivalent to feComponentTransfer in SVG.

The RemapFX class provides an implementation of the `feComponentTransfer` functionality in SVG.

Internally the pixel rendering process is implemented using pixel lookup tables.  As such this particular effect
carries minimal overhead compared to most other effect classes.

-END-

*********************************************************************************************************************/

#include <algorithm>

enum RFT {
   RFT_IDENTITY=0,
   RFT_DISCRETE,
   RFT_LINEAR,
   RFT_GAMMA,
   RFT_TABLE,
   RFT_INVERT,
   RFT_MASK
};

class Component {
   public:

   Component(CSTRING pName) : Name(pName), Type(RFT_IDENTITY) {
      for (size_t i=0; i < sizeof(Lookup); i++) Lookup[i] = i;
      update_ilookup();
   }

   std::string Name;
   std::vector<double> Table; // If table; A list of function values.
   double Slope;              // If linear; the slope of the linear function.
   double Intercept;          // If linear; the intercept of the linear function.
   double Amplitude;          // If gamma; the amplitude of the gamma function.
   double Exponent;           // If gamma; the exponent of the gamma function.
   double Offset;             // If gamma; the offset of the gamma function.
   RFT    Type;               // The type of algorithm to use.
   uint8_t  Lookup[256];        // sRGB lookup
   uint8_t  ILookup[256];       // Linear RGB lookup; composes sRGB->linear conversion, the transfer function and
                                // linear->sRGB inversion so the render loop performs one lookup per channel.

   void update_ilookup() {
      for (size_t i=0; i < sizeof(ILookup); i++) ILookup[i] = glLinearRGB.invert(Lookup[glLinearRGB.convert(i)]);
   }

   void select_invert() {
      Type = RFT_INVERT;
      for (size_t i=0; i < sizeof(Lookup); i++) Lookup[i] = 255 - i;
      update_ilookup();
   }

   void select_identity() {
      Type = RFT_IDENTITY;
      for (size_t i=0; i < sizeof(Lookup); i++) Lookup[i] = i;
      update_ilookup();
   }

   void select_mask(uint8_t pMask) {
      Type = RFT_MASK;
      for (size_t i=0; i < sizeof(Lookup); i++) Lookup[i] = i & pMask;
      update_ilookup();
   }

   void select_linear(const double pSlope, const double pIntercept) {
      Type      = RFT_LINEAR;
      Slope     = pSlope;
      Intercept = pIntercept;

      for (size_t i=0; i < sizeof(Lookup); i++) {
         Lookup[i] = std::clamp(int((double(i) * pSlope) + pIntercept * 255.0), 0, 255);
      }
      update_ilookup();
   }

   void select_gamma(const double pAmplitude, const double pExponent, const double pOffset) {
      Type      = RFT_GAMMA;
      Amplitude = pAmplitude;
      Exponent  = pExponent;
      Offset    = pOffset;

      for (size_t i=0; i < sizeof(Lookup); i++) {
         double pe = pow(double(i) * (1.0/255.0), pExponent);
         Lookup[i] = std::clamp(int(((pAmplitude * pe) + pOffset) * 255.0), 0, 255);
      }
      update_ilookup();
   }

   void select_discrete(kt::vector<double> &Values) {
      Type = RFT_DISCRETE;
      Table.clear();
      Table.insert(Table.end(), Values.data(), Values.data() + Values.size());

      uint32_t n = Table.size();
      for (size_t i=0; i < sizeof(Lookup); i++) {
         auto k = uint32_t((i * n) / 255.0);
         k = std::min(k, (uint32_t)n - 1);
         auto val = 255.0 * double(Table[k]);
         val = std::max(0.0, std::min(255.0, val));
         Lookup[i] = uint8_t(val);
      }
      update_ilookup();
   }

   void select_table(kt::vector<double> &Values) {
      Type = RFT_TABLE;
      Table.clear();
      Table.insert(Table.end(), Values.data(), Values.data() + Values.size());

      uint32_t n = Table.size();
      for (size_t i=0; i < sizeof(Lookup); i++) {
          double c = double(i) / 255.0;
          auto k = uint32_t(c * (n - 1));
          double v = Table[std::min((k + 1), (n - 1))];
          int val = int(255.0 * (Table[k] + (c * (n - 1) - k) * (v - Table[k])));
          Lookup[i] = std::max(0, std::min(255, val));
      }
      update_ilookup();
   }
};

class extRemapFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::REMAPFX;
   static constexpr CSTRING CLASS_NAME = "RemapFX";
   using create = kt::Create<extRemapFX>;

   Component Red;
   Component Green;
   Component Blue;
   Component Alpha;

   extRemapFX(objMetaClass *ClassPtr, OBJECTID ObjectID) :
      extFilterEffect(ClassPtr, ObjectID), Red("Red"), Green("Green"), Blue("Blue"), Alpha("Alpha") { }

   Component * getComponent(CMP Component) {
      switch(Component) {
         case CMP::RED:   return &Red;
         case CMP::GREEN: return &Green;
         case CMP::BLUE:  return &Blue;
         case CMP::ALPHA: return &Alpha;
         default: return nullptr;
      }
   }
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR REMAPFX_Draw(extRemapFX *Self, struct acDraw *Args)
{
   if (Self->Target->BytesPerPixel != 4) return ERR::InvalidState;

   objBitmap *bmp;
   if (get_source_bitmap(Self->Filter, &bmp, Self->SourceType, Self->Input, false) != ERR::Okay) return ERR::NoData;

   int height = Self->Target->Clip.Bottom - Self->Target->Clip.Top;
   int width  = Self->Target->Clip.Right - Self->Target->Clip.Left;
   if (bmp->Clip.Right - bmp->Clip.Left < width) width = bmp->Clip.Right - bmp->Clip.Left;
   if (bmp->Clip.Bottom - bmp->Clip.Top < height) height = bmp->Clip.Bottom - bmp->Clip.Top;

   const uint8_t R = Self->Target->ColourFormat->RedPos>>3;
   const uint8_t G = Self->Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Self->Target->ColourFormat->BluePos>>3;
   const uint8_t A = Self->Target->ColourFormat->AlphaPos>>3;

   uint8_t *in = bmp->Data + (bmp->Clip.Left * 4) + (bmp->Clip.Top * bmp->LineWidth);
   uint8_t *dest = Self->Target->Data + (Self->Target->Clip.Left * 4) + (Self->Target->Clip.Top * Self->Target->LineWidth);
   for (int y=0; y < height; y++) {
      auto dp = (uint32_t *)dest;
      auto sp = in;

      if (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) {
         for (int x=0; x < width; x++) {
            if (auto a = sp[A]) {
               uint8_t out[4];
               out[R] = Self->Red.ILookup[sp[R]];
               out[G] = Self->Green.ILookup[sp[G]];
               out[B] = Self->Blue.ILookup[sp[B]];
               out[A] = Self->Alpha.Lookup[a];
               dp[0] = ((uint32_t *)out)[0];
            }
            else dp[0] = 0;
            dp++;
            sp += 4;
         }
      }
      else {
         for (int x=0; x < width; x++) {
            if (auto a = sp[A]) {
               uint8_t out[4];
               out[R] = Self->Red.Lookup[sp[R]];
               out[G] = Self->Green.Lookup[sp[G]];
               out[B] = Self->Blue.Lookup[sp[B]];
               out[A] = Self->Alpha.Lookup[a];
               dp[0] = ((uint32_t *)out)[0];
            }
            else dp[0] = 0;
            dp++;
            sp += 4;
         }
      }

      dest += Self->Target->LineWidth;
      in   += bmp->LineWidth;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SelectDiscrete: Apply the discrete function to a pixel component.

This method will apply the table function to a selected RGBA pixel component.  A list of values is required with a
minimum size of 1.

-INPUT-
int(CMP) Component: The pixel component to which the discrete function must be applied.
vector(double) Values: A list of values for the discrete function.

-RESULT-
Okay:
NullArgs:

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR REMAPFX_SelectDiscrete(extRemapFX *Self, struct rf::SelectDiscrete *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Values)) return log.warning(ERR::NullArgs);
   if ((Args->Values->empty()) or (Args->Values->size() > 1024)) return log.warning(ERR::Args);

   if (auto cmp = Self->getComponent(Args->Component)) {
      cmp->select_discrete(*Args->Values);
      log.detail("%s Values: %d", cmp->Name.c_str(), int(Args->Values->size()));
      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-METHOD-
SelectIdentity: Apply the identity function to a pixel component.

Selecting the identity function for a pixel component will render it as its original value.  By default, all pixels
will use this function if no other option is chosen.

-INPUT-
int(CMP) Component: The pixel component to which the identity function must be applied.

-RESULT-
Okay:
NullArgs:

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR REMAPFX_SelectIdentity(extRemapFX *Self, struct rf::SelectIdentity *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if (auto cmp = Self->getComponent(Args->Component)) {
      cmp->select_identity();
      log.detail("%s", cmp->Name.c_str());
      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-METHOD-
SelectGamma: Apply the gamma function to a pixel component.

This method will apply the gamma function to a selected RGBA pixel component.  The gamma function is written as
`C' = Amplitude * pow(C, Exponent) + Offset`.

-INPUT-
int(CMP) Component: The pixel component to which the gamma function must be applied.
double Amplitude: The amplitude of the gamma function.
double Offset: The offset of the gamma function.
double Exponent: The exponent of the gamma function.

-RESULT-
Okay:
NullArgs:

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR REMAPFX_SelectGamma(extRemapFX *Self, struct rf::SelectGamma *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if (auto cmp = Self->getComponent(Args->Component)) {
      cmp->select_gamma(Args->Amplitude, Args->Exponent, Args->Offset);
      log.detail("%s Amplitude: %.2f, Exponent: %.2f, Offset: %.2f", cmp->Name.c_str(), cmp->Amplitude, cmp->Exponent, cmp->Offset);
      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-METHOD-
SelectInvert: Apply the invert function to a pixel component.

This method will apply the invert function to a selected RGBA pixel component.  The function is written as
`C' = 1.0 - C`.

This feature is not compatible with SVG.

-INPUT-
int(CMP) Component: The pixel component to which the function must be applied.

-RESULT-
Okay:
Args:
NullArgs:

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR REMAPFX_SelectInvert(extRemapFX *Self, struct rf::SelectInvert *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if (auto cmp = Self->getComponent(Args->Component)) {
      cmp->select_invert();
      log.detail("%s", cmp->Name.c_str());
      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-METHOD-
SelectLinear: Apply the linear function to a pixel component.

This method will apply the linear function to a selected RGBA pixel component.  The function is written as
`C' = (Slope * C) + Intercept`.

-INPUT-
int(CMP) Component: The pixel component to which the function must be applied.
double Slope: The slope of the linear function.
double Intercept: The intercept of the linear function.

-RESULT-
Okay:
Args:
NullArgs:

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR REMAPFX_SelectLinear(extRemapFX *Self, struct rf::SelectLinear *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if (Args->Slope < 0) return log.warning(ERR::Args);

   if (auto cmp = Self->getComponent(Args->Component)) {
      cmp->select_linear(Args->Slope, Args->Intercept);
      log.detail("%s Slope: %.2f, Intercept: %.2f", cmp->Name.c_str(), cmp->Slope, Args->Intercept);
      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-METHOD-
SelectMask: Apply the mask function to a pixel component.

This method will apply the mask function to a selected RGBA pixel component.  The function is written as
`C' = C & Mask`.  This algorithm is particularly useful for lowering the bit depth of colours, e.g. a value of `0xf0`
will reduce 8 bit colours to 4 bit.

This feature is not compatible with SVG.

-INPUT-
int(CMP) Component: The pixel component to which the function must be applied.
int Mask: The bit mask to be AND'd with each value.

-RESULT-
Okay:
Args:
NullArgs:

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR REMAPFX_SelectMask(extRemapFX *Self, struct rf::SelectMask *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if (auto cmp = Self->getComponent(Args->Component)) {
      cmp->select_mask(Args->Mask);
      log.detail("%s, Mask: $%.2x", cmp->Name.c_str(), Args->Mask);
      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-METHOD-
SelectTable: Apply the table function to a pixel component.

This method will apply the table function to a selected RGBA pixel component.  A list of values is required with a
minimum size of 1.

If a single table value is supplied then the component will be output as a constant with no interpolation applied.

-INPUT-
int(CMP) Component: The pixel component to which the table function must be applied.
vector(double) Values: A list of values for the table function.

-RESULT-
Okay:
Args:
NullArgs:

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR REMAPFX_SelectTable(extRemapFX *Self, struct rf::SelectTable *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Values)) return log.warning(ERR::NullArgs);
   if ((Args->Values->empty()) or (Args->Values->size() > 1024)) return log.warning(ERR::Args);

   if (auto cmp = Self->getComponent(Args->Component)) {
      cmp->select_table(*Args->Values);
      log.detail("%s Values: %d", cmp->Name.c_str(), int(Args->Values->size()));
      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the filter.
-END-

*********************************************************************************************************************/

static ERR REMAPFX_GET_XMLDef(extRemapFX *Self, std::string_view &Value)
{
   std::stringstream stream;

   // TODO
   stream << "<feComponentTransfer>";
   stream << "<feFuncR/>";
   stream << "<feFuncG/>";
   stream << "<feFuncB/>";
   stream << "<feFuncA/>";
   stream << "</feComponentTransfer>";

   auto cppstr = stream.str();
   if (auto str = strclone(stream.str())) {
      Value = std::string_view{str, cppstr.size()};
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

//********************************************************************************************************************

#include "filter_remap_def.c"

static const FieldArray clRemapFXFields[] = {
   { "XMLDef", FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R, REMAPFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_remapfx(void)
{
   clRemapFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::REMAPFX),
      fl::Name("RemapFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clRemapFXActions),
      fl::Methods(clRemapFXMethods),
      fl::Fields(clRemapFXFields),
      fl::Size(sizeof(extRemapFX)),
      fl::Path(MOD_PATH));

   return clRemapFX ? ERR::Okay : ERR::AddClass;
}
