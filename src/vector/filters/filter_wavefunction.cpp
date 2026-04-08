/*********************************************************************************************************************

-CLASS-
WaveFunctionFX: A filter effect that plots the probability distribution of a quantum wave function.

This filter effect uses a quantum wave function algorithm to generate a plot of electron probability density.
Ignoring its scientific value, the formula can be exploited for its aesthetic qualities.  It can be
used as an alternative to the radial gradient for generating more interesting shapes for example.

The rendering of the wave function is controlled by its parameters #N, #L and #M.  A #Scale is also provided to deal
with situations where the generated plot would otherwise be too large for its bounds.

The parameter values are clamped according to the rules `N &gt;= 1`, `0 &lt;= L &lt; N`, `0 &lt;= M &lt;= L`.
Check that the values are assigned and clamped correctly if the wave function is not rendering as expected.

-END-

*********************************************************************************************************************/

#include <complex>

class extWaveFunctionFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::WAVEFUNCTIONFX;
   static constexpr CSTRING CLASS_NAME = "WaveFunctionFX";
   using create = pf::Create<extWaveFunctionFX>;

   std::vector<std::vector<double>> psi;
   std::vector<GradientStop> Stops;
   std::string ColourMap;
   GradientColours *Colours;
   objBitmap *Bitmap;
   ARF  AspectRatio;     // Aspect ratio flags.
   double Scale, Max;
   int N, L, M, Resolution;
   bool Dirty;

   void compute_wavefunction(int);
};

//********************************************************************************************************************
// Wave function generator.  Note that only the top-left quadrant is generated for max efficiency.

inline int plot(int X, int W) { return -W + (X<<1); }

void extWaveFunctionFX::compute_wavefunction(int Resolution)
{
   constexpr double BOHR_RADIUS = 5.29177210903e-11 * 1e+12; // in picometers

   Max = 0;
   Dirty = false;
   psi = std::vector<std::vector<double>>(Resolution>>1, std::vector<double>(Resolution>>1));

   for (int dy = 0; dy < Resolution>>1; ++dy) {
      for (int dx = 0; dx < Resolution>>1; ++dx) {
         // Compute the radius value

         const double r = std::sqrt(plot(dy, Resolution) * plot(dy, Resolution) + plot(dx, Resolution) * plot(dx, Resolution));
         const double p = 2.0 * r / (N * (Scale * BOHR_RADIUS));
         const double constant_factor = std::sqrt(
            std::pow(2.0 / (N * (Scale * BOHR_RADIUS)), 3) * tgamma(N - L) / (2.0 * N * tgamma(N + L + 1))
         );
         const double laguerre = std::assoc_laguerre(N - L - 1, 2.0 * L + 1.0, p);
         const double rv = constant_factor * std::exp(-p * 0.5) * std::pow(p, L) * laguerre;

         // Angular function

         const double theta = std::atan2(plot(dy, Resolution), plot(dx, Resolution)); // Polar angle
         const double af_constant_factor = std::pow(-1, M) * std::sqrt(
            (2 * L + 1) * tgamma(L - std::abs(M) + 1) / (4 * agg::pi * tgamma(L + std::abs(M) + 1))
         );

         psi[dy][dx] = std::abs(rv * af_constant_factor * std::assoc_legendre(L, M, std::cos(theta)));
         if (psi[dy][dx] > Max) Max = psi[dy][dx];
      }
   }
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.

Note that drawing the wave function will result in the N, L and M parameters being clamped to their valid ranges and
this will be reflected in the object once the method returns.

-END-
*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_Draw(extWaveFunctionFX *Self, struct acDraw *Args)
{
   int resolution = Self->Resolution & (~1);
   if (!resolution) resolution = std::min(int(Self->Filter->TargetWidth), int(Self->Filter->TargetHeight)) & (~1);
   const int half_res = resolution>>1;

   if (Self->N < 1) Self->N = 1;
   Self->L = std::clamp(Self->L, 0, Self->N - 1);
   Self->M = std::clamp(Self->M, 0, Self->L);

   if (!Self->Bitmap) {
      if (!(Self->Bitmap = objBitmap::create::local(fl::Name("wavefunction_bmp"),
         fl::Width(resolution),
         fl::Height(resolution),
         fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL),
         fl::BlendMode(BLM::SRGB),
         fl::ColourSpace(CS::SRGB)))) return ERR::CreateObject;
   }
   else if (Self->Bitmap->Width != resolution) Self->Bitmap->resize(resolution, resolution);

   // The wave function is symmetrical on all four corners, so the top-left quadrant is duplicated to the others.

   if ((Self->Dirty) or (resolution>>1 != std::ssize(Self->psi))) Self->compute_wavefunction(resolution);

   for (int y = 0; y < half_res; ++y) {
      auto top          = (uint32_t *)(Self->Bitmap->Data + (Self->Bitmap->LineWidth * y));
      auto bottom       = (uint32_t *)(Self->Bitmap->Data + (((half_res<<1) - y - 1) * Self->Bitmap->LineWidth));
      auto top_right    = (uint32_t *)(top + (((half_res<<1) - 1)));
      auto bottom_right = (uint32_t *)(bottom + (((half_res<<1) - 1)));

      for (int x = 0; x < half_res; ++x, top++, bottom++, top_right--, bottom_right--) {
         uint8_t grey = int(Self->psi[x][y] / Self->Max * 255.0);
         uint32_t col;
         if (Self->Colours) {
            auto &rgb = Self->Colours->table[grey];
            col = Self->Bitmap->packPixel(rgb.r, rgb.g, rgb.b, rgb.a);
         }
         else col = Self->Bitmap->packPixel(grey, grey, grey, 255);

         top[0] = col;
         bottom[0] = col;
         top_right[0] = col;
         bottom_right[0] = col;
      }
   }

   render_to_filter(Self, Self->Bitmap, Self->AspectRatio, Self->Filter->Scene->SampleMethod);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR WAVEFUNCTIONFX_Free(extWaveFunctionFX *Self)
{
   Self->psi.~vector<std::vector<double>>();
   Self->Stops.~vector<GradientStop>();
   Self->ColourMap.~basic_string();
   if (Self->Colours) { delete Self->Colours; Self->Colours = nullptr; }
   if (Self->Bitmap) { FreeResource(Self->Bitmap); Self->Bitmap = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR WAVEFUNCTIONFX_Init(extWaveFunctionFX *Self)
{
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR WAVEFUNCTIONFX_NewObject(extWaveFunctionFX *Self)
{
   new (&Self->psi) std::vector<std::vector<double>>;
   new (&Self->Stops) std::vector<GradientStop>;
   new (&Self->ColourMap) std::string;
   Self->AspectRatio = ARF::X_MID|ARF::Y_MID|ARF::MEET;
   Self->N = 1;
   Self->L = 0;
   Self->M = 1;
   Self->Scale = 1.0;
   Self->Dirty = true;
   Self->SourceType = VSF::NONE;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AspectRatio: SVG compliant aspect ratio settings.
Lookup: ARF

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_AspectRatio(extWaveFunctionFX *Self, ARF *Value)
{
   *Value = Self->AspectRatio;
   return ERR::Okay;
}

static ERR WAVEFUNCTIONFX_SET_AspectRatio(extWaveFunctionFX *Self, ARF Value)
{
   Self->AspectRatio = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ColourMap: Assigns a pre-defined colourmap to the wave function.

An alternative to defining colour #Stops in a wave function is available in the form of named colourmaps.
Declaring a colourmap in this field will automatically populate the wave function's gradient with the colours defined
in the map.

We currently support the following established colourmaps from the matplotlib and seaborn projects: `cmap:crest`,
`cmap:flare`, `cmap:icefire`, `cmap:inferno`, `cmap:magma`, `cmap:mako`, `cmap:plasma`, `cmap:rocket`,
`cmap:viridis`.

The use of colourmaps and custom stops are mutually exclusive.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_ColourMap(extWaveFunctionFX *Self, CSTRING *Value)
{
   if (Self->ColourMap.empty()) *Value = nullptr;
   else *Value = Self->ColourMap.c_str();
   return ERR::Okay;
}

static ERR WAVEFUNCTIONFX_SET_ColourMap(extWaveFunctionFX *Self, CSTRING Value)
{
   if (!Value) return ERR::NoData;

   if (glColourMaps.contains(Value)) {
      if (Self->Colours) delete Self->Colours;
      Self->Colours = new (std::nothrow) GradientColours(glColourMaps[Value], 1);
      if (!Self->Colours) return ERR::AllocMemory;
      Self->ColourMap = Value;
      return ERR::Okay;
   }
   else return ERR::NotFound;
}

/*********************************************************************************************************************

-FIELD-
L: Azimuthal quantum number.

This value is clamped by `0 &lt;= L &lt; N`.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_L(extWaveFunctionFX *Self, int *Value)
{
   *Value = Self->L;
   return ERR::Okay;
}

static ERR WAVEFUNCTIONFX_SET_L(extWaveFunctionFX *Self, int Value)
{
   if (Value >= 0) {
      Self->L = Value;
      Self->Dirty = true;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
M: Magnetic quantum number.

This value is clamped by `0 &lt;= M &lt;= L`.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_M(extWaveFunctionFX *Self, int *Value)
{
   *Value = Self->M;
   return ERR::Okay;
}

static ERR WAVEFUNCTIONFX_SET_M(extWaveFunctionFX *Self, int Value)
{
   Self->M = Value;
   Self->Dirty = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
N: Principal quantum number.

This value is clamped by `N &gt;= 1`.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_N(extWaveFunctionFX *Self, int *Value)
{
   *Value = Self->N;
   return ERR::Okay;
}

static ERR WAVEFUNCTIONFX_SET_N(extWaveFunctionFX *Self, int Value)
{
   if (Value >= 1) {
      Self->N = Value;
      Self->Dirty = true;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Resolution: The pixel resolution of the internally rendered wave function.

By default the resolution of the wave function will match the smallest dimension of the filter target region, which
gives the best looking result at the cost of performance.

Setting the Resolution field will instead fix the resolution to that size permanently, and the final result will be
scaled to fit the target region.  This can give a considerable performance increase, especially when the filter is
redrawn it will not be necessary to redraw the wave function if its parameters are constant.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_Resolution(extWaveFunctionFX *Self, int *Value)
{
   *Value = Self->Resolution;
   return ERR::Okay;
}

static ERR WAVEFUNCTIONFX_SET_Resolution(extWaveFunctionFX *Self, int Value)
{
   if (Value >= 0) {
      Self->Resolution = Value;
      Self->Dirty = true;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Scale: Multiplier that affects the scale of the plot.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_Scale(extWaveFunctionFX *Self, double *Value)
{
   *Value = Self->Scale;
   return ERR::Okay;
}

static ERR WAVEFUNCTIONFX_SET_Scale(extWaveFunctionFX *Self, double Value)
{
   if (Value >= 0) {
      Self->Scale = Value;
      Self->Dirty = true;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Stops: Defines the colours to use for the wave function.

The colours that will be used for drawing a wave function can be defined by the Stops array.  At least two stops are
required to define a start and end point for interpolating the gradient colours.

If no stops are defined, the wave function will be drawn in greyscale.
-END-
*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_Stops(extWaveFunctionFX *Self, GradientStop **Value, int *Elements)
{
   *Value    = Self->Stops.data();
   *Elements = Self->Stops.size();
   return ERR::Okay;
}

static ERR WAVEFUNCTIONFX_SET_Stops(extWaveFunctionFX *Self, GradientStop *Value, int Elements)
{
   Self->Stops.clear();

   if (Elements >= 2) {
      Self->Stops.insert(Self->Stops.end(), &Value[0], &Value[Elements]);
      if (Self->Colours) delete Self->Colours;
      Self->Colours = new (std::nothrow) GradientColours(Self->Stops, /*Self->Filter->ColourSpace*/ VCS::SRGB, 1.0, 1);
      if (!Self->Colours) return ERR::AllocMemory;
      return ERR::Okay;
   }
   else {
      pf::Log log;
      log.warning("Array size %d < 2", Elements);
      return ERR::InvalidValue;
   }
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_XMLDef(extWaveFunctionFX *Self, STRING *Value)
{
   std::stringstream stream;

   stream << "feWaveFunction";

   *Value = strclone(stream.str());
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_wavefunction_def.c"

static const FieldArray clWaveFunctionFXFields[] = {
   { "AspectRatio", FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW,   WAVEFUNCTIONFX_GET_AspectRatio, WAVEFUNCTIONFX_SET_AspectRatio, &clAspectRatio },
   { "ColourMap",   FDF_VIRTUAL|FDF_STRING|FDF_RW,           WAVEFUNCTIONFX_GET_ColourMap,   WAVEFUNCTIONFX_SET_ColourMap },
   { "N",           FDF_VIRTUAL|FDF_INT|FDF_RW,              WAVEFUNCTIONFX_GET_N,           WAVEFUNCTIONFX_SET_N },
   { "L",           FDF_VIRTUAL|FDF_INT|FDF_RW,              WAVEFUNCTIONFX_GET_L,           WAVEFUNCTIONFX_SET_L },
   { "M",           FDF_VIRTUAL|FDF_INT|FDF_RW,              WAVEFUNCTIONFX_GET_M,           WAVEFUNCTIONFX_SET_M },
   { "Resolution",  FDF_VIRTUAL|FDF_INT|FDF_RW,              WAVEFUNCTIONFX_GET_Resolution,  WAVEFUNCTIONFX_SET_Resolution },
   { "Scale",       FDF_VIRTUAL|FDF_DOUBLE|FDF_RW,           WAVEFUNCTIONFX_GET_Scale,       WAVEFUNCTIONFX_SET_Scale },
   { "Stops",       FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW, WAVEFUNCTIONFX_GET_Stops,       WAVEFUNCTIONFX_SET_Stops, "GradientStop" },
   { "XMLDef",      FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R,  WAVEFUNCTIONFX_GET_XMLDef,      nullptr },
   END_FIELD
};

//********************************************************************************************************************

ERR init_wavefunctionfx(void)
{
   clWaveFunctionFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::WAVEFUNCTIONFX),
      fl::Name("WaveFunctionFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clWaveFunctionFXActions),
      fl::Fields(clWaveFunctionFXFields),
      fl::Size(sizeof(extWaveFunctionFX)),
      fl::Path(MOD_PATH));

   return clWaveFunctionFX ? ERR::Okay : ERR::AddClass;
}
