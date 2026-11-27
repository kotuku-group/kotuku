/*********************************************************************************************************************

-CLASS-
RemapFX: Provides pixel remapping; equivalent to feComponentTransfer in SVG.

The RemapFX class provides an implementation of the `feComponentTransfer` functionality in SVG.

Internally the pixel rendering process is implemented using pixel lookup tables.  As such this particular effect
carries minimal overhead compared to most other effect classes.

-END-

*********************************************************************************************************************/

#include <algorithm>

class Component {
   public:
   Component() { configure({ .Type = RFT::IDENTITY }); }

   RemapTransfer Transfer;
   uint8_t Lookup[256], ILookup[256];

   ERR configure(const RemapTransfer &Value) {
      RemapTransfer next { .Type = Value.Type };
      switch (Value.Type) {
         case RFT::IDENTITY:
         case RFT::INVERT: break;
         case RFT::MASK:
            if ((Value.Mask < 0) or (Value.Mask > 255)) return ERR::InvalidValue;
            next.Mask = Value.Mask;
            break;

         case RFT::LINEAR:
            if ((not std::isfinite(Value.Slope)) or (not std::isfinite(Value.Intercept))) return ERR::InvalidValue;
            next.Slope = Value.Slope;
            next.Intercept = Value.Intercept;
            break;

         case RFT::GAMMA:
            if ((not std::isfinite(Value.Amplitude)) or (not std::isfinite(Value.Exponent)) or
                (not std::isfinite(Value.Offset))) return ERR::InvalidValue;
            next.Amplitude = Value.Amplitude;
            next.Exponent  = Value.Exponent;
            next.Offset    = Value.Offset;
            break;

         case RFT::DISCRETE:
         case RFT::TABLE:
            if ((Value.Count < 1) or (Value.Count > std::ssize(Value.Values))) return ERR::InvalidValue;
            for (int i = 0; i < Value.Count; i++) {
               if (not std::isfinite(Value.Values[i])) return ERR::InvalidValue;
               next.Values[i] = Value.Values[i];
            }
            next.Count = Value.Count;
            break;

         default: return ERR::InvalidValue;
      }

      for (int i = 0; i < 256; i++) {
         double value = i;
         const double x = double(i) / 255.0;
         switch (next.Type) {
            case RFT::INVERT: value = 255 - i; break;
            case RFT::MASK:   value = i & next.Mask; break;
            case RFT::LINEAR:
               value = (double(i) * next.Slope) + next.Intercept * 255.0;
               if (not std::isfinite(value)) value = 255.0 * std::fma(x, next.Slope, next.Intercept);
               break;

            case RFT::GAMMA:
               value = next.Offset;
               if (next.Amplitude != 0) value += next.Amplitude * std::pow(double(i) * (1.0/255.0), next.Exponent);
               value *= 255.0;
               break;

            case RFT::DISCRETE:
               value = 255.0 * next.Values[std::min(int((i * next.Count) / 255.0), next.Count - 1)];
               break;

            case RFT::TABLE: {
               const double position = x * (next.Count - 1);
               const int index       = int(position);
               const double start    = next.Values[index], end = next.Values[std::min(index + 1, next.Count - 1)];
               const double fraction = position - index;
               value = 255.0 * (start + fraction * (end - start));
               if (not std::isfinite(value)) value = 255.0 * std::lerp(start, end, fraction);
               break;
            }

            default: break;
         }

         Lookup[i] = uint8_t(std::fmax(0.0, std::fmin(255.0, value)));
      }

      for (int i = 0; i < 256; i++) {
         ILookup[i] = glLinearRGB.invert(Lookup[glLinearRGB.convert(i)]);
      }

      Transfer = next;
      return ERR::Okay;
   }
};

//********************************************************************************************************************

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
      extFilterEffect(ClassPtr, ObjectID) { }

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

//********************************************************************************************************************

static ERR set_remap_transfer(extRemapFX *Self, Component *Channel, const RemapTransfer &Value)
{
   if (not Channel) return ERR::Args;
   if (auto error = Channel->configure(Value); error != ERR::Okay) return error;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR select_remap_table(extRemapFX *Self, CMP Channel, RFT Type, const kt::vector<double> *Values)
{
   if (not Values) return ERR::NullArgs;
   RemapTransfer transfer { .Type = Type };
   if (Values->empty() or (Values->size() > std::size(transfer.Values))) return ERR::Args;
   transfer.Count = int(Values->size());
   std::ranges::copy(*Values, transfer.Values);
   return set_remap_transfer(Self, Self->getComponent(Channel), transfer);
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR REMAPFX_Draw(extRemapFX *Self, struct acDraw *Args)
{
   if ((not Self->Target) or (Self->Target->BytesPerPixel != 4)) return ERR::InvalidState;

   const objBitmap *bmp;
   if (auto error = get_source_bitmap(Self, &bmp, filter_input_role::INPUT); error != ERR::Okay) return error;

   int height = Self->Target->Clip.Bottom - Self->Target->Clip.Top;
   int width  = Self->Target->Clip.Right - Self->Target->Clip.Left;
   if (bmp->Clip.Right - bmp->Clip.Left < width) width = bmp->Clip.Right - bmp->Clip.Left;
   if (bmp->Clip.Bottom - bmp->Clip.Top < height) height = bmp->Clip.Bottom - bmp->Clip.Top;

   const uint8_t R = Self->Target->ColourFormat->RedPos>>3;
   const uint8_t G = Self->Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Self->Target->ColourFormat->BluePos>>3;
   const uint8_t A = Self->Target->ColourFormat->AlphaPos>>3;

   const bool linear_rgb = effect_colour_space(Self) IS VCS::LINEAR_RGB;

   // The transfer functions apply to every pixel, including transparent ones, so that functions such as an inverted
   // alpha can raise empty areas to full opacity.  The colour of a transparent pixel is undefined, so it is treated
   // as black (consistent with ColourMatrixFX and premultiplied pipelines) and its output can be computed once.

   uint8_t empty[4];
   empty[R] = linear_rgb ? Self->Red.ILookup[0] : Self->Red.Lookup[0];
   empty[G] = linear_rgb ? Self->Green.ILookup[0] : Self->Green.Lookup[0];
   empty[B] = linear_rgb ? Self->Blue.ILookup[0] : Self->Blue.Lookup[0];
   empty[A] = Self->Alpha.Lookup[0];
   const uint32_t empty_pixel = std::bit_cast<uint32_t>(empty);

   const uint8_t *in = bmp->Data + (bmp->Clip.Left * 4) + (bmp->Clip.Top * bmp->LineWidth);
   uint8_t *dest = Self->Target->Data + (Self->Target->Clip.Left * 4) + (Self->Target->Clip.Top * Self->Target->LineWidth);
   for (int y=0; y < height; y++) {
      auto dp = (uint32_t *)dest;
      auto sp = in;

      if (linear_rgb) {
         for (int x=0; x < width; x++) {
            if (auto a = sp[A]) {
               uint8_t out[4];
               out[R] = Self->Red.ILookup[sp[R]];
               out[G] = Self->Green.ILookup[sp[G]];
               out[B] = Self->Blue.ILookup[sp[B]];
               out[A] = Self->Alpha.Lookup[a];
               dp[0] = std::bit_cast<uint32_t>(out);
            }
            else dp[0] = empty_pixel;
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
               dp[0] = std::bit_cast<uint32_t>(out);
            }
            else dp[0] = empty_pixel;
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
InvalidValue: A coefficient or table value is not finite.
NullArgs:

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR REMAPFX_SelectDiscrete(extRemapFX *Self, struct rf::SelectDiscrete *Args)
{
   if (not Args) return ERR::NullArgs;
   return select_remap_table(Self, Args->Component, RFT::DISCRETE, Args->Values);
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
   if (not Args) return ERR::NullArgs;
   return set_remap_transfer(Self, Self->getComponent(Args->Component), { .Type = RFT::IDENTITY });
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
InvalidValue: A coefficient or table value is not finite.
NullArgs:

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR REMAPFX_SelectGamma(extRemapFX *Self, struct rf::SelectGamma *Args)
{
   if (not Args) return ERR::NullArgs;
   return set_remap_transfer(Self, Self->getComponent(Args->Component),
      { .Type = RFT::GAMMA, .Amplitude = Args->Amplitude, .Exponent = Args->Exponent, .Offset = Args->Offset });
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
   if (not Args) return ERR::NullArgs;

   return set_remap_transfer(Self, Self->getComponent(Args->Component), { .Type = RFT::INVERT });
}

/*********************************************************************************************************************

-METHOD-
SelectLinear: Apply the linear function to a pixel component.

This method will apply the linear function to a selected RGBA pixel component.  The function is written as
`C' = (Slope * C) + Intercept`.  Any slope is permitted; for example a slope of `-1` with an intercept of `1` inverts
the component.  Results are clamped to the valid range of `0` to `1`.

-INPUT-
int(CMP) Component: The pixel component to which the function must be applied.
double Slope: The slope of the linear function.
double Intercept: The intercept of the linear function.

-RESULT-
Okay:
Args:
InvalidValue: A coefficient or table value is not finite.
NullArgs:

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR REMAPFX_SelectLinear(extRemapFX *Self, struct rf::SelectLinear *Args)
{
   if (not Args) return ERR::NullArgs;

   return set_remap_transfer(Self, Self->getComponent(Args->Component),
      { .Type = RFT::LINEAR, .Slope = Args->Slope, .Intercept = Args->Intercept });
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
   if (not Args) return ERR::NullArgs;

   return set_remap_transfer(Self, Self->getComponent(Args->Component),
      { .Type = RFT::MASK, .Mask = uint8_t(Args->Mask) });
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
InvalidValue: A coefficient or table value is not finite.
NullArgs:

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR REMAPFX_SelectTable(extRemapFX *Self, struct rf::SelectTable *Args)
{
   if (not Args) return ERR::NullArgs;

   return select_remap_table(Self, Args->Component, RFT::TABLE, Args->Values);
}

/*********************************************************************************************************************

-FIELD-
RedTransfer: The red channel's transfer configuration.

Each channel of a RemapFX has a transfer function that maps the channel's input value to an output value.  The
RedTransfer, #GreenTransfer, #BlueTransfer and #AlphaTransfer fields read and write the complete configuration of
that function as a RemapTransfer structure.  The function of every channel is `IDENTITY` by default.

!RemapTransfer

The Type determines which of the other fields are used.  In the following, `C` is the input value and `C'` is the
output value, both in the range 0 - 1.

<list type="bullet">
<li>`IDENTITY`: `C' = C`.</li>
<li>`LINEAR`: `C' = Slope * C + Intercept`.</li>
<li>`GAMMA`: `C' = Amplitude * pow(C, Exponent) + Offset`.</li>
<li>`TABLE`: The Count entries of Values define a piecewise linear function over equal intervals, between which
`C` is interpolated.</li>
<li>`DISCRETE`: The Count entries of Values define a step function of equal intervals, and `C'` is the entry of the
interval containing `C`.</li>
<li>`INVERT`: `C' = 1 - C`.</li>
<li>`MASK`: The 8-bit channel value is combined with Mask using a bitwise AND.</li>
</list>

The result is limited to the range 0 - 1.  The function applies to non-premultiplied channel values.  When the
effect's colour space is linear RGB, the red, green and blue functions apply to linear values; the alpha function is
never converted.  Fully transparent pixels are treated as transparent black before the functions are applied, so an
alpha function can make them visible.

Writing this field validates the members used by the Type and discards the rest, so reading the field back returns
zero for unused members.  Each Values entry and each parameter must be finite, Count must be between 1 and 1024 for
`TABLE` and `DISCRETE`, and Mask must be between 0 and 255.  An invalid configuration leaves the channel unchanged.
Reading the field returns a copy of the configuration that is allocated as a resource.

The #SelectIdentity(), #SelectLinear() and other Select methods offer an alternative means of configuring a channel.

-ERRORS-
Okay:
NullArgs:
InvalidValue: The Type is unrecognised or a parameter used by the Type is invalid.

-TAGS-
caller-owns-result

-FIELD-
GreenTransfer: The green channel's transfer configuration. See #RedTransfer.

-ERRORS-
Okay:
NullArgs:
InvalidValue: The Type is unrecognised or a parameter used by the Type is invalid.

-TAGS-
caller-owns-result

-FIELD-
BlueTransfer: The blue channel's transfer configuration. See #RedTransfer.

-ERRORS-
Okay:
NullArgs:
InvalidValue: The Type is unrecognised or a parameter used by the Type is invalid.

-TAGS-
caller-owns-result

-FIELD-
AlphaTransfer: The alpha channel's transfer configuration. See #RedTransfer.

-ERRORS-
Okay:
NullArgs:
InvalidValue: The Type is unrecognised or a parameter used by the Type is invalid.

-TAGS-
caller-owns-result

-END-

*********************************************************************************************************************/

template<Component extRemapFX::*Channel>
static ERR REMAPFX_GET_Transfer(extRemapFX *Self, RemapTransfer **Value)
{
   APTR memory = nullptr;
   *Value = nullptr;

   if (auto error = AllocResource(sizeof(RemapTransfer), MEM::NIL, &memory, nullptr); error != ERR::Okay) return error;

   *Value = new (memory) RemapTransfer((Self->*Channel).Transfer);
   return ERR::Okay;
}

template<Component extRemapFX::*Channel>
static ERR REMAPFX_SET_Transfer(extRemapFX *Self, const RemapTransfer *Value)
{
   if (not Value) return ERR::NullArgs;
   return set_remap_transfer(Self, &(Self->*Channel), *Value);
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR remap_xml(const extRemapFX &Effect, filter_xml_element &Element)
{
   Element.Name = "feComponentTransfer";
   Element.Complete = true;
   const std::pair<std::string_view, const Component *> channels[] = {
      { "feFuncR", &Effect.Red }, { "feFuncG", &Effect.Green },
      { "feFuncB", &Effect.Blue }, { "feFuncA", &Effect.Alpha }
   };

   for (const auto &[name, component] : channels) {
      auto &child = Element.child(name);
      const auto &transfer = component->Transfer;
      switch (transfer.Type) {
         case RFT::IDENTITY:
            child.attribute("type", "identity");
            break;

         case RFT::DISCRETE:
         case RFT::TABLE:
            child.attribute("type", transfer.Type IS RFT::TABLE ? "table" : "discrete");
            child.list("tableValues", std::span(transfer.Values, size_t(transfer.Count)));
            break;

         case RFT::LINEAR:
            child.attribute("type", "linear");
            child.attribute("slope", transfer.Slope);
            child.attribute("intercept", transfer.Intercept);
            break;

         case RFT::GAMMA:
            child.attribute("type", "gamma");
            child.attribute("amplitude", transfer.Amplitude);
            child.attribute("exponent", transfer.Exponent);
            child.attribute("offset", transfer.Offset);
            break;

         case RFT::INVERT:
            child.attribute("type", "linear");
            child.attribute("slope", -1);
            child.attribute("intercept", 1);
            child.attribute("kotuku:type", "invert");
            break;

         case RFT::MASK:
            child.attribute("kotuku:type", "mask");
            child.attribute("kotuku:mask", transfer.Mask);
            break;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_remap_def.c"

static const FieldArray clRemapFXFields[] = {
   { "RedTransfer",   FDF_VIRTUAL|FDF_STRUCT|FDF_RW|FDF_RESOURCE|FDF_STORE, REMAPFX_GET_Transfer<&extRemapFX::Red>, REMAPFX_SET_Transfer<&extRemapFX::Red>, "RemapTransfer" },
   { "GreenTransfer", FDF_VIRTUAL|FDF_STRUCT|FDF_RW|FDF_RESOURCE|FDF_STORE, REMAPFX_GET_Transfer<&extRemapFX::Green>, REMAPFX_SET_Transfer<&extRemapFX::Green>, "RemapTransfer" },
   { "BlueTransfer",  FDF_VIRTUAL|FDF_STRUCT|FDF_RW|FDF_RESOURCE|FDF_STORE, REMAPFX_GET_Transfer<&extRemapFX::Blue>, REMAPFX_SET_Transfer<&extRemapFX::Blue>, "RemapTransfer" },
   { "AlphaTransfer", FDF_VIRTUAL|FDF_STRUCT|FDF_RW|FDF_RESOURCE|FDF_STORE, REMAPFX_GET_Transfer<&extRemapFX::Alpha>, REMAPFX_SET_Transfer<&extRemapFX::Alpha>, "RemapTransfer" },
   { "XMLDef",        FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
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
