/*********************************************************************************************************************

-CLASS-
WaveFunctionFX: Generates decorative wave-function plots as colour images or scalar fields.

The WaveFunctionFX class is a source effect that plots the magnitude of a hydrogen-like atomic orbital.  It takes no
input, and the result is intended for decorative patterns and textures rather than scientific visualisation.

The quantum numbers #N, #L and #M select the shape of the orbital, and #Scale sets its size.  Drawing clamps them to
`N &gt;= 1`, `0 &lt;= L &lt; N` and `0 &lt;= M &lt;= L`, and the clamped values are written back to the object.  If the
clamped N + L exceeds 170, the formula cannot be evaluated in double precision and the field is zero.  Samples that
cannot be computed as finite values are also zero.

The plot is generated in a square domain of #Resolution pixels and fitted to the primitive subregion according to
#AspectRatio and #ResampleMethod.  #CentreX and #CentreY position the centre of the orbital within the domain.
The generated samples are retained until a field that changes them is modified, so a fixed #Resolution avoids
regenerating the plot when only the size of the target changes.

By default the magnitude is scaled so that the largest sample is one; see #Normalisation.  In `IMAGE` mode the
samples are drawn in greyscale, or through the colour ramp defined by #ColourMap or #Stops.  Set #OutputMode to
`HEIGHT` to produce a floating-point scalar field for numerical inputs, such as the height map of an enhanced
@LightingFX or the amount mask of a @CompositeFX.

SVG documents can use the effect with the `xvg:feWaveFunction` element.

-END-

*********************************************************************************************************************/

class extWaveFunctionFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::WAVEFUNCTIONFX;
   static constexpr CSTRING CLASS_NAME = "WaveFunctionFX";
   using create = kt::Create<extWaveFunctionFX>;

   // The exported fields are declared first so that their concrete offsets line up with the field table.
   // Keep the direct-access fields aligned without padding between them.
   std::string ColourMap;
   double Scale = 1.0;
   kt::vector<GradientStop> Stops;
   ARF  AspectRatio = ARF::X_MID|ARF::Y_MID|ARF::MEET; // Aspect ratio flags.
   int N = 1, L = 0, M = 1, Resolution = 0;
   FOM OutputMode = FOM::IMAGE;
   WFN Normalisation = WFN::MAXIMUM;
   VSM ResampleMethod = VSM::AUTO;
   double CentreX = 0.5, CentreY = 0.5;

   std::unique_ptr<double[]> Samples;
   std::unique_ptr<GradientColours> Colours;
   objBitmap *Bitmap = nullptr;
   double Max = 0;
   int CachedResolution = 0;
   bool Dirty = true;

   ERR compute_wavefunction(int Resolution, int Width, int Height);

   void clear_colours() {
      ColourMap.clear();
      Stops.clear();
      Colours.reset();
   }

   extWaveFunctionFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::NONE;
   }

   ~extWaveFunctionFX() {
      if (Bitmap) FreeResource(Bitmap);
   }
};

//********************************************************************************************************************
// Generate one half on each centred axis and mirror it at lookup time.

ERR extWaveFunctionFX::compute_wavefunction(int Resolution, int Width, int Height)
{
   constexpr double BOHR_RADIUS = 5.29177210903e-11 * 1e+12; // in picometres
   const size_t count = size_t(Width) * Height;
   Samples.reset();
   Samples.reset(new (std::nothrow) double[count]);
   if (not Samples) return ERR::AllocMemory;

   Max = 0;
   if ((N > 170) or (N + L > 170)) std::fill_n(Samples.get(), count, 0.0);
   else {
      const double radius = N * (Scale * BOHR_RADIUS);
      const double radial_factor = std::sqrt(std::pow(2.0 / radius, 3) * tgamma(N - L) /
         (2.0 * N * tgamma(N + L + 1)));
      const double angular_factor = std::pow(-1, M) * std::sqrt(
         (2 * L + 1) * tgamma(L - M + 1) / (4 * agg::pi * tgamma(L + M + 1)));

      // The half-sample bias retains the original mirrored grid at the default centre.
      const double centre_x = CentreX * Resolution, centre_y = CentreY * Resolution;
      for (int y = 0; y < Height; y++) {
         const double py = -2.0 * (std::abs(y + 0.5 - centre_y) + 0.5);
         for (int x = 0; x < Width; x++) {
            const double px = -2.0 * (std::abs(x + 0.5 - centre_x) + 0.5);
            const double r = std::sqrt(px * px + py * py);
            const double p = 2.0 * r / radius;
            double magnitude = 0;
            if (std::isfinite(p) and std::isfinite(radial_factor)) {
               const double laguerre = std::assoc_laguerre(N - L - 1, 2.0 * L + 1.0, p);
               const double radial = radial_factor * std::exp(-p * 0.5) * std::pow(p, L) * laguerre;
               // Retain the angular orientation of the original plot.
               const double theta = std::atan2(px, py);
               magnitude = std::abs(radial * angular_factor * std::assoc_legendre(L, M, std::cos(theta)));
               if (not std::isfinite(magnitude)) magnitude = 0;
            }
            Samples[size_t(y) * Width + x] = magnitude;
            Max = std::max(Max, magnitude);
         }
      }
   }
   CachedResolution = Resolution;
   Dirty = false;
   return ERR::Okay;
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
   const auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;
   const auto &render = *Self->Filter->Render;
   const bool scalar = step->Format IS filter_surface_format::SCALAR_F32;

   int resolution = Self->Resolution & ~1;
   if (not resolution) {
      const double size = std::min(render.FilterRegion.width(), render.FilterRegion.height());
      if (size < 2) return ERR::Okay;
      if (size > 0xffff) return ERR::OutOfRange;
      resolution = int(size) & ~1;
   }

   const int half_res = resolution / 2;
   const bool mirror_x = Self->CentreX IS 0.5, mirror_y = Self->CentreY IS 0.5;
   const int width = mirror_x ? half_res : resolution, height = mirror_y ? half_res : resolution;
   const size_t sample_bytes = size_t(width) * height * sizeof(double);
   const size_t bitmap_bytes = scalar ? 0 : size_t(resolution) * resolution * 4;
   if (sample_bytes + bitmap_bytes > MAX_FILTER_SURFACE_BYTES - render.LiveBytes) return ERR::AllocMemory;

   Self->N = std::max(Self->N, 1);
   Self->L = std::clamp(Self->L, 0, Self->N - 1);
   Self->M = std::clamp(Self->M, 0, Self->L);

   if (Self->Dirty or (resolution != Self->CachedResolution) or (not Self->Samples)) {
      if (auto error = Self->compute_wavefunction(resolution, width, height); error != ERR::Okay) return error;
   }

   const double divisor = ((Self->Normalisation IS WFN::MAXIMUM) and (Self->Max > 0.0)) ? Self->Max : 1.0;
   const auto method = Self->ResampleMethod IS VSM::AUTO ? Self->Filter->Scene->SampleMethod : Self->ResampleMethod;
   auto sample = [&](int X, int Y) {
      const int sx = mirror_x ? std::min(X, resolution - 1 - X) : X;
      const int sy = mirror_y ? std::min(Y, resolution - 1 - Y) : Y;
      return Self->Samples[size_t(sy) * width + sx] / divisor;
   };

   if (scalar) {
      if (Self->Bitmap) {
         FreeResource(Self->Bitmap);
         Self->Bitmap = nullptr;
      }
      const double maximum = (Self->Normalisation IS WFN::MAXIMUM) ? 1.0 : std::numeric_limits<float>::max();
      return render_scalar_to_filter(Self, resolution, resolution, Self->AspectRatio, method, maximum,
         [&](int X, int Y) { return std::min(sample(X, Y), maximum); });
   }

   if (not Self->Bitmap) {
      if (not (Self->Bitmap = objBitmap::create::local(fl::Name("wavefunction_bmp"),
         fl::Width(resolution),
         fl::Height(resolution),
         fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL),
         fl::BlendMode(BLM::SRGB),
         fl::ColourSpace(CS::SRGB)))) return ERR::CreateObject;
   }
   else if (Self->Bitmap->Width != resolution) {
      if (auto error = Self->Bitmap->resize(resolution, resolution); error != ERR::Okay) return error;
   }

   for (int y = 0; y < resolution; y++) {
      auto row = (uint32_t *)(Self->Bitmap->Data + std::ptrdiff_t(y) * Self->Bitmap->LineWidth);
      for (int x = 0; x < resolution; x++) {
         const auto grey = uint8_t(std::min(sample(x, y), 1.0) * 255.0);
         if (Self->Colours) {
            const auto &rgb = Self->Colours->table[grey];
            row[x] = Self->Bitmap->packPixel(rgb.r, rgb.g, rgb.b, rgb.a);
         }
         else row[x] = Self->Bitmap->packPixel(grey, grey, grey, 255);
      }
   }

   return render_to_filter(Self, Self->Bitmap, Self->AspectRatio, method);
}

/*********************************************************************************************************************

-FIELD-
AspectRatio: SVG compliant aspect ratio settings.
Lookup: ARF

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_AspectRatio(extWaveFunctionFX *Self, ARF Value)
{
   Self->AspectRatio = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CentreX: Positions the plot centre from the left of the generation domain.

The value is a finite fraction in [0, 1] and defaults to 0.5.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_CentreX(extWaveFunctionFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0.0) or (Value > 1.0)) return ERR::OutOfRange;

   if (Self->CentreX != Value) {
      Self->CentreX = Value;
      Self->Dirty = true;
      effect_modified(Self);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CentreY: Positions the plot centre from the top of the generation domain.

The value is a finite fraction in [0, 1] and defaults to 0.5.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_CentreY(extWaveFunctionFX *Self, double Value)
{
   if ((not std::isfinite(Value)) or (Value < 0.0) or (Value > 1.0)) return ERR::OutOfRange;

   if (Self->CentreY != Value) {
      Self->CentreY = Value;
      Self->Dirty = true;
      effect_modified(Self);
   }
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

Setting a colourmap replaces custom stops.  Set an empty string to clear the active ramp and draw in greyscale.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_ColourMap(extWaveFunctionFX *Self, const std::string_view &Value)
{
   if (Value.empty()) {
      Self->clear_colours();
      effect_modified(Self);
      return ERR::Okay;
   }

   if (auto it = glColourMaps.find(Value); it != glColourMaps.end()) {
      auto colours = std::unique_ptr<GradientColours>(new (std::nothrow) GradientColours(it->second, 1));
      if (not colours) return ERR::AllocMemory;
      Self->Colours = std::move(colours);
      Self->ColourMap = Value;
      Self->Stops.clear();
      effect_modified(Self);
      return ERR::Okay;
   }
   else return ERR::NotFound;
}

/*********************************************************************************************************************

-FIELD-
L: Azimuthal quantum number.

Writes accept non-negative integers.  Drawing clamps the value to the range `0 &lt;= L &lt; N`.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_L(extWaveFunctionFX *Self, int Value)
{
   if (Value >= 0) {
      Self->L = Value;
      Self->Dirty = true;
      effect_modified(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
M: Magnetic quantum number.

Writes accept any integer.  Drawing clamps the value to the range `0 &lt;= M &lt;= L`.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_M(extWaveFunctionFX *Self, int Value)
{
   Self->M = Value;
   Self->Dirty = true;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
N: Principal quantum number.

Writes accept positive integers.  For the range of the wave formula, see @WaveFunctionFX.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_N(extWaveFunctionFX *Self, int Value)
{
   if (Value >= 1) {
      Self->N = Value;
      Self->Dirty = true;
      effect_modified(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Normalisation: Controls scaling of the generated magnitude before resampling.
Lookup: WFN

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_Normalisation(extWaveFunctionFX *Self, WFN Value)
{
   if ((Value != WFN::MAXIMUM) and (Value != WFN::RAW)) return ERR::OutOfRange;
   Self->Normalisation = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputMode: Selects a colour image or a floating-point scalar surface.
Lookup: FOM

`IMAGE` is the default.  `HEIGHT` emits unquantised samples and ignores the active colour ramp.
`NATIVE` is unsupported.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_OutputMode(extWaveFunctionFX *Self, FOM Value)
{
   if ((Value != FOM::IMAGE) and (Value != FOM::HEIGHT)) return ERR::OutOfRange;

   if (Self->OutputMode != Value) {
      Self->OutputMode = Value;
      if (Self->Filter) Self->Filter->GraphRevision++;
      effect_modified(Self);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ResampleMethod: Selects the interpolation used when fitting the plot to the primitive.
Lookup: VSM

`AUTO` (the default) uses @VectorScene.SampleMethod.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_ResampleMethod(extWaveFunctionFX *Self, VSM Value)
{
   if ((Value < VSM::AUTO) or (Value > VSM::BLACKMAN)) return ERR::OutOfRange;
   Self->ResampleMethod = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Resolution: The pixel resolution of the internally rendered wave function.

Values from 2 to 65535 fix the generation resolution.  Odd values round down to an even number for drawing.
Values 0 (the default) and 1 use the smaller filter-region dimension, rounded down to an even number.
Fixed resolution allows reuse of the generated samples when the target size changes.  The render must fit the filter
surface budget.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_Resolution(extWaveFunctionFX *Self, int Value)
{
   if ((Value >= 0) and (Value <= 0xffff)) {
      Self->Resolution = Value;
      Self->Dirty = true;
      effect_modified(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Scale: A finite positive multiplier that affects the scale of the plot.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_Scale(extWaveFunctionFX *Self, double Value)
{
   if ((Value > 0) and std::isfinite(Value)) {
      Self->Scale = Value;
      Self->Dirty = true;
      effect_modified(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************

-FIELD-
Stops: Defines the colours to use for the wave function.

The colours that will be used for drawing a wave function can be defined by the Stops array.  At least two stops are
required to define a start and end point for interpolating the gradient colours.

Setting stops replaces any named colourmap.  Set an empty array to clear the active ramp and draw in greyscale.

*********************************************************************************************************************/

static ERR WAVEFUNCTIONFX_SET_Stops(extWaveFunctionFX *Self, std::span<const GradientStop> &Value)
{
   if (Value.empty()) {
      Self->clear_colours();
      effect_modified(Self);
      return ERR::Okay;
   }

   if (Value.size() < 2) return ERR::InvalidValue;

   kt::vector<GradientStop> stops;
   stops.assign(Value.begin(), Value.end());
   auto colours = std::unique_ptr<GradientColours>(new (std::nothrow)
      GradientColours(stops, VCS::SRGB, GCI::DEFAULT, GHM::SHORTER, 1.0, 1));
   if (not colours) return ERR::AllocMemory;

   Self->Stops = std::move(stops);
   Self->Colours = std::move(colours);
   Self->ColourMap.clear();
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR wavefunction_xml(const extWaveFunctionFX &Effect, filter_xml_element &Element)
{
   Element.Name = "xvg:feWaveFunction";
   Element.attribute("n", Effect.N);
   Element.attribute("l", Effect.L);
   Element.attribute("m", Effect.M);
   Element.attribute("scale", Effect.Scale);
   Element.attribute("resolution", Effect.Resolution);

   write_filter_xml_aspect(Element, Effect.AspectRatio);

   if (Effect.OutputMode IS FOM::HEIGHT) Element.attribute("xvg:outputMode", "height");
   if (Effect.Normalisation IS WFN::RAW) Element.attribute("xvg:normalisation", "raw");
   if (Effect.ResampleMethod != VSM::AUTO) Element.attribute("xvg:resampleMethod", int(Effect.ResampleMethod));
   if (Effect.CentreX != 0.5) Element.attribute("xvg:centreX", Effect.CentreX);
   if (Effect.CentreY != 0.5) Element.attribute("xvg:centreY", Effect.CentreY);

   if (not Effect.ColourMap.empty()) Element.attribute("colormap", Effect.ColourMap);
   else {
      double last_offset = 0;
      for (const auto &stop : Effect.Stops) {
         if ((stop.Offset < last_offset) or (stop.Offset > 1.0)) return ERR::NoSupport;
         auto &child = Element.child("stop");
         child.attribute("offset", stop.Offset);
         write_filter_xml_colour(child, "stop-color", stop.RGB);
         last_offset = stop.Offset;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_wavefunction_def.c"

static const FieldArray clWaveFunctionFXFields[] = {
   { "ColourMap",      FDF_CPPSTRING|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_ColourMap },
   { "Scale",          FDF_DOUBLE|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_Scale },
   { "Stops",          FDF_VECTOR|FDF_STRUCT|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_Stops, "GradientStop" },
   { "AspectRatio",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_AspectRatio, &clAspectRatio },
   { "N",              FDF_INT|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_N },
   { "L",              FDF_INT|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_L },
   { "M",              FDF_INT|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_M },
   { "Resolution",     FDF_INT|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_Resolution },
   { "OutputMode",     FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_OutputMode, &clWaveFunctionFXFOM },
   { "Normalisation",  FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_Normalisation, &clWaveFunctionFXWFN },
   { "ResampleMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_ResampleMethod, &clWaveFunctionFXVSM },
   { "CentreX",        FDF_DOUBLE|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_CentreX },
   { "CentreY",        FDF_DOUBLE|FDF_RW, nullptr, WAVEFUNCTIONFX_SET_CentreY },
   { "XMLDef",         FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
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
