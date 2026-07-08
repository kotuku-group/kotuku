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
   using create = kt::Create<extWaveFunctionFX>;

   // The exported fields are declared first so that their concrete offsets line up with the field table.
   // The 8-byte aligned members precede the narrower int fields to avoid padding before a direct-access field.
   std::string ColourMap;
   double Scale = 1.0;
   kt::vector<GradientStop> Stops;
   ARF  AspectRatio = ARF::X_MID|ARF::Y_MID|ARF::MEET; // Aspect ratio flags.
   int N = 1, L = 0, M = 1, Resolution = 0;

   std::vector<std::vector<double>> psi;
   std::unique_ptr<GradientColours> Colours;
   objBitmap *Bitmap = nullptr;
   double Max = 0;
   bool Dirty = true;

   void compute_wavefunction(int);

   extWaveFunctionFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::NONE;
   }

   ~extWaveFunctionFX() {
      if (Bitmap) FreeResource(Bitmap);
   }
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
   if (Self->Scale <= 0.0) return ERR::InvalidValue;

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
         uint8_t grey = 0;
         if (Self->Max > 0.0) grey = int(Self->psi[x][y] / Self->Max * 255.0);
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

/*********************************************************************************************************************

-FIELD-
AspectRatio: SVG compliant aspect ratio settings.
Lookup: ARF

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_AspectRatio(extWaveFunctionFX *Self, ARF Value)
{
   Self->AspectRatio = Value;
   Self->Dirty = true;
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

static ERR WAVEFUNCTIONFX_SET_ColourMap(extWaveFunctionFX *Self, const std::string_view &Value)
{
   if (Value.empty()) return ERR::NoData;

   if (auto it = glColourMaps.find(Value); it != glColourMaps.end()) {
      Self->Colours.reset(new (std::nothrow) GradientColours(it->second, 1));
      if (not Self->Colours) return ERR::AllocMemory;
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

static ERR WAVEFUNCTIONFX_SET_Scale(extWaveFunctionFX *Self, double Value)
{
   if (Value > 0) {
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

static ERR WAVEFUNCTIONFX_SET_Stops(extWaveFunctionFX *Self, std::span<const GradientStop> &Value)
{
   Self->Stops.clear();

   if (Value.size() >= 2) {
      Self->Stops.assign(Value.begin(), Value.end());
      Self->Colours.reset(new (std::nothrow) GradientColours(Self->Stops, /*Self->Filter->ColourSpace*/ VCS::SRGB, 1.0, 1));
      if (!Self->Colours) return ERR::AllocMemory;
      return ERR::Okay;
   }
   else {
      kt::Log log;
      log.warning("Array size %d < 2", int(Value.size()));
      return ERR::InvalidValue;
   }
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_GET_XMLDef(extWaveFunctionFX *Self, std::string &Value)
{
   std::stringstream stream;

   stream << "feWaveFunction";

   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_wavefunction_def.c"

static const FieldArray clWaveFunctionFXFields[] = {
   { "ColourMap",   FDF_CPPSTRING|FDF_RW,        nullptr, WAVEFUNCTIONFX_SET_ColourMap },
   { "Scale",       FDF_DOUBLE|FDF_RW,           nullptr, WAVEFUNCTIONFX_SET_Scale },
   { "Stops",       FDF_VECTOR|FDF_STRUCT|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_Stops, "GradientStop" },
   { "AspectRatio", FDF_INT|FDF_LOOKUP|FDF_RW,   nullptr, WAVEFUNCTIONFX_SET_AspectRatio, &clAspectRatio },
   { "N",           FDF_INT|FDF_RW,              nullptr, WAVEFUNCTIONFX_SET_N },
   { "L",           FDF_INT|FDF_RW,              nullptr, WAVEFUNCTIONFX_SET_L },
   { "M",           FDF_INT|FDF_RW,              nullptr, WAVEFUNCTIONFX_SET_M },
   { "Resolution",  FDF_INT|FDF_RW,              nullptr, WAVEFUNCTIONFX_SET_Resolution },
   { "XMLDef",      FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R|FDF_PURE, WAVEFUNCTIONFX_GET_XMLDef },
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
