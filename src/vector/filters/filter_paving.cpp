/*********************************************************************************************************************

-CLASS-
PavingFX: Generates structured masonry, paving, tile and checker arrangements.

PavingFX is a source effect that takes no primary input.  It partitions the pattern plane into rectangular elements
separated by joints according to #Layout (`RUNNING`, `STACK`, `BASKET_WEAVE`, `HERRINGBONE` or `CHECKER`), and
evaluates each sample solely from its position in primitive units.  Clipped, partial and full redraws therefore
produce identical values.

Element dimensions are given by #ElementWidth and #ElementHeight, positioned by #OriginX and #OriginY, and rotated
about the origin by #Angle.  Percentages in #ElementWidth and #OriginX refer to the width of the filter region, and
percentages in #ElementHeight and #OriginY refer to its height.  Percentages in #JointWidth and #BevelWidth refer to
the normalised diagonal of the filter region (`sqrt((width^2 + height^2) / 2)`), so horizontal and vertical joints
and bevels share the same thickness on non-square regions.

`BASKET_WEAVE` and `HERRINGBONE` require the resolved `ElementWidth / ElementHeight` ratio to lie within `1e-4` of
an integer `k >= 2`.  With the default `ElementWidth = 25%` and `ElementHeight = 12.5%`, this holds automatically on
square filter regions and in `BOUNDING_BOX` primitive units.

#Mode selects the generated field:

<list type="bullet">
<li>`COLOUR`: Draws elements using #ElementColour (and #AlternateColour for odd-parity cells in `CHECKER`) or an
upstream #ElementMap, separated by joints using #JointColour or an upstream #JointMap.  Per-element perceptual OKLCH
variation is controlled by #HueVariation, #ChromaVariation, #LightVariation and #Seed.  Edges between elements and
joints are analytically anti-aliased when #JointWidth is positive.  For thin joints, coverage includes the elements
on either side of the joint and limits the sampling footprint to the shortest element dimension per axis.</li>
<li>`HEIGHT`: Produces a bevelled relief field rising from #JointHeightValue in joints to `#ElementHeightValue` plus
seeded #HeightVariation on element interiors, shaped across #BevelWidth by #BevelProfile.</li>
<li>`MASK`: Produces element coverage in [0, 1], with analytic edge anti-aliasing when #JointWidth is positive.</li>
<li>`ID`: Produces a discrete seeded hash in [0, 1) per element, and 0 in joints.</li>
</list>

In `IMAGE` #OutputMode, scalar modes (`HEIGHT`, `MASK` and `ID`) clamp values to [0, 1] and draw opaque greyscale in
the effect's colour space, or through #ColourMap or #Stops.  Set #OutputMode to `HEIGHT` to emit an unclamped
floating-point scalar surface for numerical consumers such as @LightingFX or @CompositeFX.  `Mode = COLOUR` cannot
be combined with `OutputMode = HEIGHT`.

#PeriodX and #PeriodY wrap element keys so that seeded variation and `ID` output repeat without seams.  Natural repeat
vectors in pattern units are `(W, 0)` and `(RowOffset * W, H)` for `RUNNING`, `(W, 0)` and `(0, H)` for `STACK`,
`(2W, 0)` and `(0, 2H)` for `CHECKER`, `(2W, 0)` and `(0, 2W)` for `BASKET_WEAVE`, and `(2W, 0)` and `(0, 2W)`
(axis-aligned) for `HERRINGBONE`.  A periodic pattern cannot be rotated (`Angle` must be a multiple of 360), a
periodic `RUNNING` layout requires `PeriodY * RowOffset` to be an integer, and periodic `CHECKER` and `BASKET_WEAVE`
layouts require even non-zero periods.

SVG documents can use the effect with the `xvg:fePaving` element.  Refer to the @SVG class for its attributes.

-END-

// TODO: Support ParameterMaps (a filter input that varies a field per sample) for JointWidth, BevelWidth,
// ElementHeightValue, JointHeightValue, HueVariation, ChromaVariation, LightVariation and HeightVariation, once a
// binding facility shared by the filter classes has been designed.

*********************************************************************************************************************/

#include "paving_pattern.h"
#include "filter_colour_ramp.h"

constexpr FRGB DEFAULT_ELEMENT_COLOUR   = { 168.0f / 255.0f, 155.0f / 255.0f, 140.0f / 255.0f, 1.0f };
constexpr FRGB DEFAULT_JOINT_COLOUR     = { 69.0f / 255.0f,  65.0f / 255.0f,  60.0f / 255.0f,  1.0f };
constexpr FRGB DEFAULT_ALTERNATE_COLOUR = { 107.0f / 255.0f, 99.0f / 255.0f,  90.0f / 255.0f,  1.0f };

class extPavingFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::PAVINGFX;
   static constexpr CSTRING CLASS_NAME = "PavingFX";
   using create = kt::Create<extPavingFX>;

   // Exported fields are declared first, in the order of the field table, so that their concrete offsets match it.
   // Eight-byte aligned fields precede four-byte aligned fields to avoid padding between them.
   std::string ColourMap;
   kt::vector<GradientStop> Stops;
   int64_t Seed = 0;
   Unit ElementWidth = Unit(0.25, FD_SCALED);
   Unit ElementHeight = Unit(0.125, FD_SCALED);
   Unit OriginX = Unit(0.0);
   Unit OriginY = Unit(0.0);
   Unit JointWidth = Unit(0.01, FD_SCALED);
   Unit BevelWidth = Unit(0.01, FD_SCALED);
   double Angle = 0;
   double RowOffset = 0.5;
   double ElementHeightValue = 1.0;
   double JointHeightValue = 0.0;
   double HueVariation = 0.0;
   double ChromaVariation = 0.0;
   double LightVariation = 0.0;
   double HeightVariation = 0.0;
   FRGB ElementColour = DEFAULT_ELEMENT_COLOUR;
   FRGB JointColour = DEFAULT_JOINT_COLOUR;
   FRGB AlternateColour = DEFAULT_ALTERNATE_COLOUR;
   LAY Layout = LAY::RUNNING;
   LMD Mode = LMD::COLOUR;
   LFM FillMode = LFM::LOCAL;
   GEZ BevelProfile = GEZ::LINEAR;
   FOM OutputMode = FOM::IMAGE;
   VSM ResampleMethod = VSM::BILINEAR;
   int PeriodX = 0;
   int PeriodY = 0;

   // Named input slots.  ElementMap reads its entire producer when FillMode is LOCAL.
   std::array<filter_named_input, 2> MapInputs = {{
      { "ElementMap", { .ContributesRegion = false, .Optional = true, .WholeProducer = true } },
      { "JointMap",   { .ContributesRegion = false, .Optional = true } }
   }};
   filter_named_input &ElementMap = MapInputs[0];
   filter_named_input &JointMap   = MapInputs[1];
   filter_effect_extension Extension = { std::span(MapInputs), filter_surface_format::RGBA8, &slot_active };

   static bool slot_active(const extFilterEffect *Effect, size_t Slot) {
      return ((const extPavingFX *)Effect)->Mode IS LMD::COLOUR;
   }

   std::unique_ptr<GradientColours> Colours;
   objBitmap *Bitmap = nullptr;
   bool Dirty = true;
   agg::trans_affine CacheTransform;
   TClipRectangle<double> CacheRegion = {};
   TClipRectangle<double> CacheDomain = {};
   VCS CacheColourSpace = VCS::INHERIT;

   extPavingFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::NONE;
      FilterExtension = &Extension;
   }

   ~extPavingFX() {
      if (Bitmap) FreeResource(Bitmap);
   }
};

//********************************************************************************************************************

static double paving_srgb_to_linear(double Value)
{
   const double v = std::clamp(Value, 0.0, 1.0);
   return (v <= 0.04045) ? (v / 12.92) : std::pow((v + 0.055) / 1.055, 2.4);
}

static double paving_linear_to_srgb(double Value)
{
   const double v = std::clamp(Value, 0.0, 1.0);
   return (v <= 0.0031308) ? (v * 12.92) : ((std::pow(v, 1.0 / 2.4) * 1.055) - 0.055);
}

struct paving_rgba {
   double r, g, b, a;
};

static paving_rgba paving_working_colour(const FRGB &Colour, bool LinearRGB)
{
   if (LinearRGB) {
      return { paving_srgb_to_linear(Colour.Red), paving_srgb_to_linear(Colour.Green),
         paving_srgb_to_linear(Colour.Blue), double(Colour.Alpha) };
   }
   return { double(Colour.Red), double(Colour.Green), double(Colour.Blue), double(Colour.Alpha) };
}

static void paving_write_pixel(uint8_t *Pixel, const paving_rgba &Colour, bool LinearRGB,
   uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
   if (LinearRGB) {
      Pixel[R] = glLinearRGB.invert16(uint16_t(std::lround(std::clamp(Colour.r, 0.0, 1.0) * 65535.0)));
      Pixel[G] = glLinearRGB.invert16(uint16_t(std::lround(std::clamp(Colour.g, 0.0, 1.0) * 65535.0)));
      Pixel[B] = glLinearRGB.invert16(uint16_t(std::lround(std::clamp(Colour.b, 0.0, 1.0) * 65535.0)));
   }
   else {
      Pixel[R] = uint8_t(std::lround(std::clamp(Colour.r, 0.0, 1.0) * 255.0));
      Pixel[G] = uint8_t(std::lround(std::clamp(Colour.g, 0.0, 1.0) * 255.0));
      Pixel[B] = uint8_t(std::lround(std::clamp(Colour.b, 0.0, 1.0) * 255.0));
   }
   Pixel[A] = uint8_t(std::lround(std::clamp(Colour.a, 0.0, 1.0) * 255.0));
}

//********************************************************************************************************************
// Prepares a named RGBA8 texture input in straight alpha with byte lookup tables to the working space and linear RGB.

struct paving_map_source {
   filter_surface_view View = {};
   filter_rgba_sampler Sampler = {};
   TClipRectangle<int> Frame = {};
   std::array<double, 256> ToWorking = {};
   std::array<double, 256> ToLinear = {};
   uint8_t R = 0, G = 0, B = 0, A = 0;
   bool Bound = false;
   bool HasPixels = false;

   void fill_luts(bool SourceLinear, bool WorkingLinear) {
      for (int i=0; i < 256; i++) {
         const double v = double(i) * (1.0 / 255.0);
         const double lin = SourceLinear ? v : paving_srgb_to_linear(v);
         ToLinear[i] = lin;
         if (WorkingLinear) ToWorking[i] = lin;
         else ToWorking[i] = SourceLinear ? paving_linear_to_srgb(v) : v;
      }
   }

   paving_rgba decode_working(const uint8_t *Pixel) const {
      const uint8_t a = Pixel[A];
      if (not a) return { 0.0, 0.0, 0.0, 0.0 };
      return { ToWorking[Pixel[R]], ToWorking[Pixel[G]], ToWorking[Pixel[B]], double(a) * (1.0 / 255.0) };
   }

   paving_rgba sample_continuous(int X, int Y) const {
      if (not HasPixels) return { 0.0, 0.0, 0.0, 0.0 };
      if ((X < View.Valid.left) or (X >= View.Valid.right) or (Y < View.Valid.top) or (Y >= View.Valid.bottom)) {
         return { 0.0, 0.0, 0.0, 0.0 };
      }
      return decode_working(View.row(Y) + std::ptrdiff_t(X) * 4);
   }

   bool read_local_bytes(double U, double V, VSM Method, uint8_t Output[4]) const {
      if (not HasPixels) {
         Output[0] = Output[1] = Output[2] = Output[3] = 0;
         return false;
      }
      const double sx = double(Frame.left) + (U * double(Frame.width())) - 0.5;
      const double sy = double(Frame.top) + (V * double(Frame.height())) - 0.5;
      if (Method IS VSM::NEIGHBOUR) {
         Sampler.sample<filter_interpolation::NEAREST, FEM::CLAMP>(Output, sx, sy, FEM::CLAMP, int(A));
      }
      else {
         Sampler.sample<filter_interpolation::BILINEAR, FEM::CLAMP>(Output, sx, sy, FEM::CLAMP, int(A));
      }
      return Output[A] != 0;
   }

   bool read_continuous_bytes(int X, int Y, uint8_t Output[4]) const {
      if (not HasPixels) {
         Output[0] = Output[1] = Output[2] = Output[3] = 0;
         return false;
      }
      if ((X < View.Valid.left) or (X >= View.Valid.right) or (Y < View.Valid.top) or (Y >= View.Valid.bottom)) {
         Output[0] = Output[1] = Output[2] = Output[3] = 0;
         return false;
      }
      const auto p = View.row(Y) + std::ptrdiff_t(X) * 4;
      std::copy_n(p, 4, Output);
      return Output[A] != 0;
   }
};

static ERR prepare_paving_map(extPavingFX *Self, const filter_named_input &Slot, bool LocalFrame, bool WorkingLinear,
   paving_map_source &Source)
{
   Source = {};
   if (Slot.Source IS VSF::NONE) return ERR::Okay;
   Source.Bound = true;

   filter_surface_view raw_view;
   if (auto error = get_named_filter_bitmap(Self, Slot.Name, raw_view); error != ERR::Okay) return error;
   if ((not raw_view.Bitmap) or (raw_view.Bitmap->BytesPerPixel != 4)) return ERR::InvalidState;

   filter_surface_view view;
   if (auto error = working_input(Self, raw_view, raw_view.Encoding, filter_alpha::STRAIGHT, view);
       error != ERR::Okay) {
      return error;
   }
   view.Valid.shrinking(raw_view.Valid);

   const auto fmt = view.Bitmap->ColourFormat;
   Source.View = view;
   Source.R = fmt->RedPos>>3;
   Source.G = fmt->GreenPos>>3;
   Source.B = fmt->BluePos>>3;
   Source.A = fmt->AlphaPos>>3;
   Source.fill_luts(view.Encoding IS CS::LINEAR_RGB, WorkingLinear);

   if (LocalFrame) {
      const auto input = planned_named_input(Self, Slot.Name);
      if ((not input) or (not input->Producer)) return ERR::InvalidState;
      auto bounds = input->Producer->Subregion;
      if ((not bounds.valid()) or (not transform_filter_region(bounds, Self->Filter->Render->Transform))) {
         return ERR::Okay;
      }

      const auto &valid = view.Valid;
      const auto clamp_x = [&](double X) { return int(std::clamp(X, double(valid.left), double(valid.right))); };
      const auto clamp_y = [&](double Y) { return int(std::clamp(Y, double(valid.top), double(valid.bottom))); };
      const TClipRectangle<int> frame = { clamp_x(std::floor(bounds.left)), clamp_y(std::floor(bounds.top)),
         clamp_x(std::ceil(bounds.right)), clamp_y(std::ceil(bounds.bottom)) };
      if (not frame.valid()) return ERR::Okay;

      Source.Frame = frame;
      Source.Sampler = { view.Data, view.Stride, frame.left, frame.top, frame.right, frame.bottom };
      Source.HasPixels = true;
   }
   else {
      Source.HasPixels = view.Valid.valid();
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Checks combinations of fields that individual setters cannot, because fields may be set in any order or depend on
// resolved domain dimensions.

static ERR validate_paving(const extPavingFX *Self, const paving_params &Params)
{
   kt::Log log("PavingFX");

   if (auto error = paving_check(Params); error != ERR::Okay) {
      if (((Params.Layout IS paving_layout::BASKET_WEAVE) or (Params.Layout IS paving_layout::HERRINGBONE)) and
          (not paving_ratio(Params.ElementWidth, Params.ElementHeight))) {
         const CSTRING name = (Params.Layout IS paving_layout::BASKET_WEAVE) ? "BASKET_WEAVE" : "HERRINGBONE";
         log.warning("#%d: %s requires ElementWidth / ElementHeight to be an integer >= 2 (got %.6g / %.6g).",
            Self->UID, name, Params.ElementWidth, Params.ElementHeight);
         return ERR::InvalidValue;
      }

      const bool periodic = (Params.PeriodX > 0) or (Params.PeriodY > 0);
      if (periodic and ((not std::isfinite(Params.Angle)) or (std::fmod(Params.Angle, 360.0) != 0.0))) {
         log.warning("#%d: A periodic pattern cannot be rotated.", Self->UID);
         return ERR::InvalidValue;
      }

      if (periodic and (Params.Layout IS paving_layout::RUNNING) and (Params.PeriodY > 0)) {
         const double shift = double(Params.PeriodY) * Params.RowOffset;
         if (std::abs(shift - std::round(shift)) > PAVING_ROW_TOLERANCE) {
            log.warning("#%d: Periodic RUNNING layout requires PeriodY * RowOffset to be an integer.", Self->UID);
            return ERR::InvalidValue;
         }
      }

      if (periodic and ((Params.Layout IS paving_layout::CHECKER) or (Params.Layout IS paving_layout::BASKET_WEAVE)) and
          ((Params.PeriodX & 1) or (Params.PeriodY & 1))) {
         const CSTRING name = (Params.Layout IS paving_layout::CHECKER) ? "CHECKER" : "BASKET_WEAVE";
         log.warning("#%d: Periodic %s layout requires even non-zero periods.", Self->UID, name);
         return ERR::InvalidValue;
      }

      log.warning("#%d: Invalid paving parameters.", Self->UID);
      return error;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.

Draw fails with `ERR::InvalidValue` if `Mode` is `COLOUR` with `OutputMode` set to `HEIGHT`, if `BASKET_WEAVE` or
`HERRINGBONE` is drawn with a resolved `ElementWidth / ElementHeight` ratio that is not within `1e-4` of an integer
`>= 2`, if a periodic pattern is rotated, if a periodic `RUNNING` layout has a non-integer `PeriodY * RowOffset`, or
if a periodic `CHECKER` or `BASKET_WEAVE` layout has an odd period.  It fails with `ERR::OutOfRange` if pattern
coordinates across the drawn area would exceed 2^52, or if the possible `HEIGHT` mode values exceed the finite
range of a 32-bit float when #OutputMode is `HEIGHT`.
-END-
*********************************************************************************************************************/

static ERR PAVINGFX_Draw(extPavingFX *Self, struct acDraw *Args)
{
   const auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;
   const bool scalar = step->Format IS filter_surface_format::SCALAR_F32;
   filter_surface_view layout;
   float *heights = nullptr;
   if (scalar) {
      if (auto error = get_numeric_target(Self, layout, heights); error != ERR::Okay) return error;
   }
   else if ((not Self->Target) or (Self->Target->BytesPerPixel != 4)) return ERR::InvalidState;

   if ((Self->Mode IS LMD::COLOUR) and (Self->OutputMode IS FOM::HEIGHT)) {
      kt::Log("PavingFX").warning("#%d: OutputMode HEIGHT cannot be used with Mode COLOUR.", Self->UID);
      return ERR::InvalidValue;
   }

   if (scalar and (Self->Mode IS LMD::HEIGHT)) {
      constexpr double limit = std::numeric_limits<float>::max();
      if ((std::abs(Self->JointHeightValue) > limit) or
          (std::abs(Self->ElementHeightValue) > limit - Self->HeightVariation)) return ERR::OutOfRange;
   }

   const auto &render = *Self->Filter->Render;
   const auto clip = render.scratch_clip();
   const int width = clip.right;
   const int height = clip.bottom;
   if ((width <= 0) or (height <= 0)) return ERR::Okay;

   // Express the primitive subregion, filter region and inverse sampling transform in primitiveUnits.

   double unit_ox = 0, unit_oy = 0, unit_width = 1, unit_height = 1;
   if (render.PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      const auto &bounds = render.ClientBounds;
      unit_ox = bounds.left;
      unit_oy = bounds.top;
      unit_width = bounds.width();
      unit_height = bounds.height();
      if ((unit_width <= 0) or (unit_height <= 0)) return ERR::Okay;
   }

   auto to_primitive = [&](const TClipRectangle<double> &Area) -> TClipRectangle<double> {
      return { (Area.left - unit_ox) / unit_width, (Area.top - unit_oy) / unit_height,
         (Area.right - unit_ox) / unit_width, (Area.bottom - unit_oy) / unit_height };
   };
   const auto region = to_primitive(step->Subregion);
   const auto domain = to_primitive(render.FilterRegion);

   agg::trans_affine sampling;
   sampling.scale(unit_width, unit_height);
   sampling.translate(unit_ox, unit_oy);
   sampling *= render.Transform;
   TClipRectangle<double> samples = { filter_pixel_centre(0), filter_pixel_centre(0),
      filter_pixel_centre(width - 1), filter_pixel_centre(height - 1) };
   if ((not finite_region(region)) or (not finite_region(domain)) or (not invert_filter_transform(sampling)) or
       (not transform_filter_region(samples, sampling))) return ERR::InvalidValue;

   // Resolve Unit fields against the filter region in primitive units.

   const double domain_w = domain.width(), domain_h = domain.height();
   const double diagonal = filter_render_context::normalised_diagonal(domain_w, domain_h);
   auto resolve = [](const Unit &Length, double Reference) {
      return Length.scaled() ? Length.Value * Reference : Length.Value;
   };

   const double elem_w  = resolve(Self->ElementWidth, domain_w);
   const double elem_h  = resolve(Self->ElementHeight, domain_h);
   const double orig_x  = domain.left + resolve(Self->OriginX, domain_w);
   const double orig_y  = domain.top + resolve(Self->OriginY, domain_h);
   const double joint_w = resolve(Self->JointWidth, diagonal);
   const double bevel_w = resolve(Self->BevelWidth, diagonal);
   if ((not std::isfinite(elem_w)) or (not std::isfinite(elem_h)) or (not std::isfinite(orig_x)) or
       (not std::isfinite(orig_y)) or (not std::isfinite(joint_w)) or (not std::isfinite(bevel_w))) {
      return ERR::InvalidValue;
   }
   if ((elem_w <= 0) or (elem_h <= 0)) return ERR::Okay;

   const double angle = std::fmod(Self->Angle, 360.0);
   const paving_params params {
      .Layout             = paving_layout(int(Self->Layout)),
      .ElementWidth       = elem_w,
      .ElementHeight      = elem_h,
      .JointWidth         = joint_w,
      .BevelWidth         = bevel_w,
      .RowOffset          = Self->RowOffset,
      .Angle              = angle,
      .ElementHeightValue = Self->ElementHeightValue,
      .JointHeightValue   = Self->JointHeightValue,
      .HueVariation       = Self->HueVariation,
      .ChromaVariation    = Self->ChromaVariation,
      .LightVariation     = Self->LightVariation,
      .HeightVariation    = Self->HeightVariation,
      .BevelProfile       = Self->BevelProfile,
      .Seed               = Self->Seed,
      .PeriodX            = Self->PeriodX,
      .PeriodY            = Self->PeriodY
   };
   if (auto error = validate_paving(Self, params); error != ERR::Okay) return error;

   // Only pixel centres inside both the processing bitmap and the primitive subregion reach the pattern.

   if ((not region.valid()) or (samples.right < region.left) or (samples.bottom < region.top) or
       (samples.left >= region.right) or (samples.top >= region.bottom)) return ERR::Okay;
   samples.shrinking(region);

   agg::trans_affine pattern;
   pattern.translate(-orig_x, -orig_y);
   if (angle != 0.0) pattern.rotate(-angle * agg::pi / 180.0);
   if (not finite_transform(pattern)) return ERR::InvalidValue;

   // Compute directional 1-pixel anti-aliasing footprints from the combined pixel-to-pattern Jacobian.

   agg::trans_affine to_pattern = sampling;
   to_pattern *= pattern;
   if (not finite_transform(to_pattern)) return ERR::InvalidValue;
   const double aa_x = std::hypot(to_pattern.sx, to_pattern.shx);
   const double aa_y = std::hypot(to_pattern.shy, to_pattern.sy);

   const paving_sampler sampler(params, aa_x, aa_y);

   double min_qx = DBL_MAX, min_qy = DBL_MAX, max_qx = -DBL_MAX, max_qy = -DBL_MAX;
   for (auto [x, y] : { std::pair(samples.left, samples.top), std::pair(samples.right, samples.top),
         std::pair(samples.left, samples.bottom), std::pair(samples.right, samples.bottom) }) {
      pattern.transform(&x, &y);
      min_qx = std::min(min_qx, x);
      min_qy = std::min(min_qy, y);
      max_qx = std::max(max_qx, x);
      max_qy = std::max(max_qy, y);
   }
   if ((Self->Mode IS LMD::COLOUR) or (Self->Mode IS LMD::MASK)) {
      const auto [rx, ry] = sampler.coverage_radius();
      min_qx -= rx;
      max_qx += rx;
      min_qy -= ry;
      max_qy += ry;
   }
   if (not sampler.lattice_fits(min_qx, min_qy, max_qx, max_qy)) return ERR::OutOfRange;

   const bool linear_rgb = effect_colour_space(Self) IS VCS::LINEAR_RGB;
   const bool colour_mode = Self->Mode IS LMD::COLOUR;
   const bool has_upstream = (not scalar) and colour_mode and
      ((Self->ElementMap.Source != VSF::NONE) or (Self->JointMap.Source != VSF::NONE));

   paving_map_source elem_map, joint_map;
   if (has_upstream) {
      if (auto error = prepare_paving_map(Self, Self->ElementMap, Self->FillMode IS LFM::LOCAL, linear_rgb, elem_map);
          error != ERR::Okay) return error;
      if (auto error = prepare_paving_map(Self, Self->JointMap, false, linear_rgb, joint_map);
          error != ERR::Okay) return error;
      Self->Dirty = true;
   }

   objBitmap *dest_bitmap = Self->Target;
   const bool use_cache = (not scalar) and (not has_upstream);

   if (use_cache) {
      if (not Self->Bitmap) {
         Self->Dirty = true;
         if (not (Self->Bitmap = objBitmap::create::local(fl::Name("paving_bmp"),
            fl::Width(width),
            fl::Height(height),
            fl::BitsPerPixel(32),
            fl::Flags(BMF::ALPHA_CHANNEL),
            fl::BlendMode(BLM::NONE),
            fl::ColourSpace(CS::SRGB)))) return ERR::CreateObject;
      }
      else if ((Self->Bitmap->Width != width) or (Self->Bitmap->Height != height)) {
         Self->Dirty = true;
         if (auto error = Self->Bitmap->resize(width, height); error != ERR::Okay) return error;
      }

      auto same_area = [](const TClipRectangle<double> &A, const TClipRectangle<double> &B) {
         return (A.left IS B.left) and (A.top IS B.top) and (A.right IS B.right) and (A.bottom IS B.bottom);
      };
      if ((not Self->CacheTransform.is_equal(sampling, 0.0)) or (Self->CacheColourSpace != effect_colour_space(Self)) or
          (not same_area(Self->CacheRegion, region)) or (not same_area(Self->CacheDomain, domain))) {
         Self->Dirty = true;
      }

      if (not Self->Dirty) return gfx::CopyArea(Self->Bitmap, Self->Target, BAF::NIL, 0, 0, width, height, 0, 0);
      dest_bitmap = Self->Bitmap;
   }

   const auto colours = Self->Colours.get();
   const bool varied = params.colour_varied();
   const bool hue_varied = params.HueVariation != 0.0;
   const bool height_varied = params.HeightVariation != 0.0;
   const bool local_fill = Self->FillMode IS LFM::LOCAL;
   const VSM resample = Self->ResampleMethod;
   const LMD mode = Self->Mode;

   const paving_rgba elem_solid  = paving_working_colour(Self->ElementColour, linear_rgb);
   const paving_rgba alt_solid   = paving_working_colour(Self->AlternateColour, linear_rgb);
   const paving_rgba joint_solid = paving_working_colour(Self->JointColour, linear_rgb);

   const paving_lch elem_lch = paving_lch_from_linear(paving_srgb_to_linear(Self->ElementColour.Red),
      paving_srgb_to_linear(Self->ElementColour.Green), paving_srgb_to_linear(Self->ElementColour.Blue));
   const paving_lch alt_lch = paving_lch_from_linear(paving_srgb_to_linear(Self->AlternateColour.Red),
      paving_srgb_to_linear(Self->AlternateColour.Green), paving_srgb_to_linear(Self->AlternateColour.Blue));

   int thread_count = std::thread::hardware_concurrency();
   if (thread_count > height / 4) thread_count = height / 4;
   if (thread_count < 1) thread_count = 1;
   BS::thread_pool pool(thread_count);

   for (int i=0; i < thread_count; i++) {
      pool.detach_task([=, &sampler, &elem_map, &joint_map,
         start = (height * i) / thread_count, end = (height * (i + 1)) / thread_count]() {

         auto eval_scalar = [&](const paving_sample &Sample) -> double {
            switch (mode) {
               case LMD::HEIGHT: {
                  const double dz = height_varied ? sampler.variation(sampler.element_key(Sample)).Height : 0.0;
                  return sampler.height(Sample, dz);
               }
               case LMD::MASK:
                  return sampler.coverage(Sample);
               case LMD::ID:
                  return (sampler.interior(Sample) >= 0.0) ?
                     sampler.id(Sample, cellular_hash(sampler.element_key(Sample), 0)) : 0.0;
               default:
                  return 0.0;
            }
         };

         if (scalar) {
            for (int y=start; y < end; ++y) {
               auto row = heights + std::ptrdiff_t(y) * (layout.Stride / sizeof(float));
               for (int x=0; x < width; ++x) {
                  double px, py;
                  map_filter_pixel_centre(sampling, x, y, px, py);
                  const bool covered = (px >= region.left) and (px < region.right) and
                     (py >= region.top) and (py < region.bottom);
                  if (not covered) {
                     row[x] = 0.0f;
                     continue;
                  }
                  pattern.transform(&px, &py);
                  row[x] = float(eval_scalar(sampler.locate(px, py)));
               }
            }
            return;
         }

         const uint8_t a = dest_bitmap->ColourFormat->AlphaPos>>3;
         const uint8_t r = dest_bitmap->ColourFormat->RedPos>>3;
         const uint8_t g = dest_bitmap->ColourFormat->GreenPos>>3;
         const uint8_t b = dest_bitmap->ColourFormat->BluePos>>3;

         if (not colour_mode) {
            for (int y=start; y < end; ++y) {
               auto row = dest_bitmap->Data + std::ptrdiff_t(y) * dest_bitmap->LineWidth;
               for (int x=0; x < width; ++x) {
                  auto pixel = row + std::ptrdiff_t(x) * 4;
                  double px, py;
                  map_filter_pixel_centre(sampling, x, y, px, py);
                  if ((px < region.left) or (px >= region.right) or (py < region.top) or (py >= region.bottom)) {
                     pixel[r] = pixel[g] = pixel[b] = pixel[a] = 0;
                     continue;
                  }
                  pattern.transform(&px, &py);
                  const double value = eval_scalar(sampler.locate(px, py));
                  if (colours) write_colour_ramp_pixel(*colours, value, pixel, r, g, b, a);
                  else {
                     const auto level = uint8_t(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
                     pixel[r] = pixel[g] = pixel[b] = linear_rgb ? glLinearRGB.invert(level) : level;
                     pixel[a] = 255;
                  }
               }
            }
            return;
         }

         auto eval_element = [&](const paving_sample &Sample, int X, int Y) -> paving_rgba {
            if (elem_map.Bound) {
               uint8_t bytes[4];
               const bool opaque = local_fill ?
                  elem_map.read_local_bytes(Sample.U, Sample.V, resample, bytes) :
                  elem_map.read_continuous_bytes(X, Y, bytes);
               if (not opaque) return { 0.0, 0.0, 0.0, 0.0 };
               if (not varied) return elem_map.decode_working(bytes);

               const auto var = sampler.variation(sampler.element_key(Sample));
               const auto base_lch = paving_lch_from_linear(elem_map.ToLinear[bytes[elem_map.R]],
                  elem_map.ToLinear[bytes[elem_map.G]], elem_map.ToLinear[bytes[elem_map.B]]);
               const auto lin = paving_adjust_oklch(base_lch, var.Light, var.Chroma, var.Hue, var.HueUnit, hue_varied);
               const double alpha = double(bytes[elem_map.A]) * (1.0 / 255.0);
               if (linear_rgb) return { lin.r, lin.g, lin.b, alpha };
               return { paving_linear_to_srgb(lin.r), paving_linear_to_srgb(lin.g),
                  paving_linear_to_srgb(lin.b), alpha };
            }

            if (not varied) return Sample.Parity ? alt_solid : elem_solid;

            const auto var = sampler.variation(sampler.element_key(Sample));
            const auto &base_lch = Sample.Parity ? alt_lch : elem_lch;
            const auto lin = paving_adjust_oklch(base_lch, var.Light, var.Chroma, var.Hue, var.HueUnit, hue_varied);
            const double alpha = Sample.Parity ? alt_solid.a : elem_solid.a;
            if (linear_rgb) return { lin.r, lin.g, lin.b, alpha };
            return { paving_linear_to_srgb(lin.r), paving_linear_to_srgb(lin.g),
               paving_linear_to_srgb(lin.b), alpha };
         };

         auto eval_joint = [&](int X, int Y) -> paving_rgba {
            return joint_map.Bound ? joint_map.sample_continuous(X, Y) : joint_solid;
         };

         for (int y=start; y < end; ++y) {
            auto row = dest_bitmap->Data + std::ptrdiff_t(y) * dest_bitmap->LineWidth;
            for (int x=0; x < width; ++x) {
               auto pixel = row + std::ptrdiff_t(x) * 4;
               double px, py;
               map_filter_pixel_centre(sampling, x, y, px, py);
               if ((px < region.left) or (px >= region.right) or (py < region.top) or (py >= region.bottom)) {
                  pixel[r] = pixel[g] = pixel[b] = pixel[a] = 0;
                  continue;
               }

               pattern.transform(&px, &py);
               const auto sample = sampler.locate(px, py);
               paving_rgba out = {};
               auto accumulate = [&](const paving_rgba &Colour, double Weight) {
                  const double alpha = Colour.a * Weight;
                  out.r += Colour.r * alpha;
                  out.g += Colour.g * alpha;
                  out.b += Colour.b * alpha;
                  out.a += alpha;
               };

               bool full_element = false;
               const double coverage = sampler.visit_coverage(sample, [&](const paving_sample &Element, double Weight) {
                  const auto colour = eval_element(Element, x, y);
                  if (Weight IS 1.0) {
                     out = colour;
                     full_element = true;
                  }
                  else accumulate(colour, Weight);
               });

               if (coverage <= 0.0) out = eval_joint(x, y);
               else if (not full_element) {
                  if (coverage < 1.0) accumulate(eval_joint(x, y), 1.0 - coverage);
                  if (out.a > 0.0) {
                     out.r /= out.a;
                     out.g /= out.a;
                     out.b /= out.a;
                  }
               }

               paving_write_pixel(pixel, out, linear_rgb, r, g, b, a);
            }
         }
      });
   }
   pool.wait();

   if (scalar or (not use_cache)) return ERR::Okay;

   Self->CacheTransform = sampling;
   Self->CacheRegion = region;
   Self->CacheDomain = domain;
   Self->CacheColourSpace = effect_colour_space(Self);
   Self->Dirty = false;
   return gfx::CopyArea(Self->Bitmap, Self->Target, BAF::NIL, 0, 0, width, height, 0, 0);
}

//********************************************************************************************************************
// Field updates invalidate the cached image and notify the scene only when the value changes.

static void paving_modified(extPavingFX *Self)
{
   Self->Dirty = true;
   effect_modified(Self);
}

template <class T> static ERR update_paving(extPavingFX *Self, T &Field, T Value)
{
   if (Field != Value) {
      Field = Value;
      paving_modified(Self);
   }
   return ERR::Okay;
}

static ERR update_paving_range(extPavingFX *Self, double &Field, double Value, double Min, double Max)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   if ((Value < Min) or (Value > Max)) return ERR::OutOfRange;
   return update_paving(Self, Field, Value);
}

static ERR update_paving_unit(extPavingFX *Self, Unit &Field, const Unit &Value, bool Positive, bool NonNegative)
{
   if (not std::isfinite(Value.Value)) return ERR::InvalidValue;
   if (Positive and (Value.Value <= 0)) return ERR::OutOfRange;
   if (NonNegative and (Value.Value < 0)) return ERR::OutOfRange;
   if ((Field.Value != Value.Value) or (Field.scaled() != Value.scaled())) {
      Field = Unit(Value.Value, Value.scaled() ? FD_SCALED : 0);
      paving_modified(Self);
   }
   return ERR::Okay;
}

static bool same_colour(const FRGB &A, const FRGB &B)
{
   return (A.Red IS B.Red) and (A.Green IS B.Green) and (A.Blue IS B.Blue) and (A.Alpha IS B.Alpha);
}

static ERR update_paving_colour(extPavingFX *Self, FRGB &Field, const FRGB *Value)
{
   FRGB clamped;
   if (Value) {
      if ((not std::isfinite(Value->Red)) or (not std::isfinite(Value->Green)) or
          (not std::isfinite(Value->Blue)) or (not std::isfinite(Value->Alpha))) {
         return ERR::InvalidValue;
      }
      clamped = {
         std::clamp(Value->Red, 0.0f, 1.0f),
         std::clamp(Value->Green, 0.0f, 1.0f),
         std::clamp(Value->Blue, 0.0f, 1.0f),
         std::clamp(Value->Alpha, 0.0f, 1.0f)
      };
   }
   else clamped = { Field.Red, Field.Green, Field.Blue, 0.0f };

   if (not same_colour(Field, clamped)) {
      Field = clamped;
      paving_modified(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AlternateColour: The colour of odd-parity cells in the `CHECKER` layout.

Stored in straight sRGB RGBA format with components clamped to [0, 1].  Defaults to `#6b635a` (opaque).  Ignored by
layouts other than `CHECKER`, and replaced by #ElementMap when bound.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_AlternateColour(extPavingFX *Self, FRGB *Value)
{
   return update_paving_colour(Self, Self->AlternateColour, Value);
}

/*********************************************************************************************************************

-FIELD-
Angle: Rotates the pattern clockwise about its origin, in degrees.

The rotation is applied about the point given by #OriginX and #OriginY.  The value must be finite and defaults to 0.
A periodic pattern cannot be rotated: if #PeriodX or #PeriodY is set, Angle must be a multiple of 360 or the effect
will fail to draw.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_Angle(extPavingFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_paving(Self, Self->Angle, Value);
}

/*********************************************************************************************************************

-FIELD-
BevelProfile: Shapes the height transition across #BevelWidth.
Lookup: GEZ

Selects the easing curve used to interpolate from #JointHeightValue to the element interior height in `HEIGHT` mode.
Defaults to `LINEAR`.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_BevelProfile(extPavingFX *Self, GEZ Value)
{
   if ((int(Value) < int(GEZ::LINEAR)) or (int(Value) > int(GEZ::CIRCLE_IN_OUT))) return ERR::OutOfRange;
   return update_paving(Self, Self->BevelProfile, Value);
}

/*********************************************************************************************************************

-FIELD-
BevelWidth: Width of the bevel transition inside each element edge, in pattern units.

The value must be finite and non-negative.  A percentage refers to the normalised diagonal of the filter region.
Defaults to `1%`.  A value of 0 produces a vertical step from #JointHeightValue to the element height.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_BevelWidth(extPavingFX *Self, Unit &Value)
{
   return update_paving_unit(Self, Self->BevelWidth, Value, false, true);
}

/*********************************************************************************************************************

-FIELD-
ChromaVariation: Maximum seeded per-element OKLCH chroma offset.

Each element receives a deterministic offset in `[-ChromaVariation, +ChromaVariation]`.  The value must lie in the
range [0, 0.4] and defaults to 0.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_ChromaVariation(extPavingFX *Self, double Value)
{
   return update_paving_range(Self, Self->ChromaVariation, Value, 0.0, 0.4);
}

/*********************************************************************************************************************

-FIELD-
ColourMap: Draws scalar `IMAGE` output through a pre-defined colour ramp.

Applies when #Mode is `HEIGHT`, `MASK` or `ID` and #OutputMode is `IMAGE`.  The clamped value selects a colour from
the named colourmap, from 0 to 1.  Setting a colourmap replaces custom #Stops.  Set an empty string to restore
greyscale output.  Unknown names fail with `ERR::NotFound`.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_ColourMap(extPavingFX *Self, const std::string_view &Value)
{
   if (auto error = set_colour_ramp_map(*Self, Value); error != ERR::Okay) return error;
   paving_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ElementColour: The base colour of elements when #ElementMap is not bound.

Stored in straight sRGB RGBA format with components clamped to [0, 1].  Defaults to `#a89b8c` (opaque).

*********************************************************************************************************************/

static ERR PAVINGFX_SET_ElementColour(extPavingFX *Self, FRGB *Value)
{
   return update_paving_colour(Self, Self->ElementColour, Value);
}

/*********************************************************************************************************************

-FIELD-
ElementHeight: The nominal element thickness, before rotation.

The value must be finite and greater than zero.  A percentage refers to the height of the filter region.  Defaults
to `12.5%`.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_ElementHeight(extPavingFX *Self, Unit &Value)
{
   return update_paving_unit(Self, Self->ElementHeight, Value, true, false);
}

/*********************************************************************************************************************

-FIELD-
ElementHeightValue: The nominal scalar height of element interiors in `HEIGHT` mode.

The value must be finite and defaults to 1.0.  Each element's interior height is offset by up to #HeightVariation.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_ElementHeightValue(extPavingFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_paving(Self, Self->ElementHeightValue, Value);
}

/*********************************************************************************************************************

-FIELD-
ElementMap: Optional upstream effect supplying a colour texture for elements.

Bind an earlier image-producing effect in the same filter to texture elements in `COLOUR` mode.  #FillMode selects
whether the texture is mapped into each element's local coordinates (`LOCAL`) or sampled continuously in output
coordinates (`CONTINUOUS`).  Ignored when #Mode is not `COLOUR`.  Set to `nullptr` to restore solid #ElementColour.

*********************************************************************************************************************/

static ERR PAVINGFX_GET_ElementMap(extPavingFX *Self, objFilterEffect **Value)
{
   *Value = Self->ElementMap.Reference;
   return ERR::Okay;
}

static ERR PAVINGFX_SET_ElementMap(extPavingFX *Self, objFilterEffect *Value)
{
   if (auto error = set_named_filter_input(Self, Self->ElementMap.Name, Value ? VSF::REFERENCE : VSF::NONE, Value);
       error != ERR::Okay) return error;
   Self->Dirty = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ElementWidth: The nominal element length, before rotation.

The value must be finite and greater than zero.  A percentage refers to the width of the filter region.  Defaults to
`25%`.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_ElementWidth(extPavingFX *Self, Unit &Value)
{
   return update_paving_unit(Self, Self->ElementWidth, Value, true, false);
}

/*********************************************************************************************************************

-FIELD-
FillMode: Selects how #ElementMap is mapped onto elements.
Lookup: LFM

`LOCAL`, the default, maps the producer's subregion across each element in its own `(u, v)` coordinates and rotates
with vertical elements in `BASKET_WEAVE` and `HERRINGBONE`.  `CONTINUOUS` samples #ElementMap directly at output
coordinates.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_FillMode(extPavingFX *Self, LFM Value)
{
   if ((int(Value) < int(LFM::LOCAL)) or (int(Value) > int(LFM::CONTINUOUS))) return ERR::OutOfRange;
   if (Self->FillMode != Value) {
      Self->FillMode = Value;
      Self->ElementMap.Contract.WholeProducer = (Value IS LFM::LOCAL);
      if (Self->Filter) Self->Filter->GraphRevision++;
      paving_modified(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
HeightVariation: Maximum seeded per-element height offset in `HEIGHT` mode.

Each element's interior height is offset by a deterministic value in `[-HeightVariation, +HeightVariation]`.  The
value must lie in the range [0, 100] and defaults to 0.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_HeightVariation(extPavingFX *Self, double Value)
{
   return update_paving_range(Self, Self->HeightVariation, Value, 0.0, 100.0);
}

/*********************************************************************************************************************

-FIELD-
HueVariation: Maximum seeded per-element OKLCH hue shift, in degrees.

Each element receives a deterministic hue offset in `[-HueVariation, +HueVariation]`.  For achromatic base colours,
a positive HueVariation assigns a uniformly distributed seeded hue when combined with #ChromaVariation.  The value
must lie in the range [0, 180] and defaults to 0.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_HueVariation(extPavingFX *Self, double Value)
{
   return update_paving_range(Self, Self->HueVariation, Value, 0.0, 180.0);
}

/*********************************************************************************************************************

-FIELD-
JointColour: The base colour of joints when #JointMap is not bound.

Stored in straight sRGB RGBA format with components clamped to [0, 1].  Defaults to `#45413c` (opaque).

*********************************************************************************************************************/

static ERR PAVINGFX_SET_JointColour(extPavingFX *Self, FRGB *Value)
{
   return update_paving_colour(Self, Self->JointColour, Value);
}

/*********************************************************************************************************************

-FIELD-
JointHeightValue: The scalar height of joints in `HEIGHT` mode.

The value must be finite and defaults to 0.0.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_JointHeightValue(extPavingFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_paving(Self, Self->JointHeightValue, Value);
}

/*********************************************************************************************************************

-FIELD-
JointMap: Optional upstream effect supplying a colour texture for joints.

Bind an earlier image-producing effect in the same filter to texture joints continuously in output coordinates when
#Mode is `COLOUR`.  Ignored when #Mode is not `COLOUR`.  Set to `nullptr` to restore solid #JointColour.

*********************************************************************************************************************/

static ERR PAVINGFX_GET_JointMap(extPavingFX *Self, objFilterEffect **Value)
{
   *Value = Self->JointMap.Reference;
   return ERR::Okay;
}

static ERR PAVINGFX_SET_JointMap(extPavingFX *Self, objFilterEffect *Value)
{
   if (auto error = set_named_filter_input(Self, Self->JointMap.Name, Value ? VSF::REFERENCE : VSF::NONE, Value);
       error != ERR::Okay) return error;
   Self->Dirty = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
JointWidth: Width of the gap between adjacent elements, in pattern units.

The value must be finite and non-negative.  A percentage refers to the normalised diagonal of the filter region.
Defaults to `1%`.  A value of 0 removes joints completely without mortar halos.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_JointWidth(extPavingFX *Self, Unit &Value)
{
   return update_paving_unit(Self, Self->JointWidth, Value, false, true);
}

/*********************************************************************************************************************

-FIELD-
Layout: Selects the tiling arrangement of elements.
Lookup: LAY

Supported layouts are `RUNNING` (the default), `STACK`, `BASKET_WEAVE`, `HERRINGBONE` and `CHECKER`.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_Layout(extPavingFX *Self, LAY Value)
{
   if ((int(Value) < int(LAY::RUNNING)) or (int(Value) > int(LAY::CHECKER))) return ERR::OutOfRange;
   return update_paving(Self, Self->Layout, Value);
}

/*********************************************************************************************************************

-FIELD-
LightVariation: Maximum seeded per-element OKLCH lightness offset.

Each element receives a deterministic lightness offset in `[-LightVariation, +LightVariation]`, clamped to [0, 1].
The value must lie in the range [0, 1] and defaults to 0.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_LightVariation(extPavingFX *Self, double Value)
{
   return update_paving_range(Self, Self->LightVariation, Value, 0.0, 1.0);
}

/*********************************************************************************************************************

-FIELD-
Mode: Selects the content produced by the effect.
Lookup: LMD

`COLOUR`, the default, draws coloured or textured elements and joints.  `HEIGHT` emits the bevelled surface height
field, `MASK` emits element coverage in [0, 1], and `ID` emits a discrete seeded hash in [0, 1) per element and 0 in
joints.  `COLOUR` cannot be combined with #OutputMode `HEIGHT`.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_Mode(extPavingFX *Self, LMD Value)
{
   if ((int(Value) < int(LMD::COLOUR)) or (int(Value) > int(LMD::ID))) return ERR::OutOfRange;
   if (Self->Mode != Value) {
      Self->Mode = Value;
      if (Self->Filter) Self->Filter->GraphRevision++;
      paving_modified(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OriginX: The horizontal origin of the pattern, relative to the left edge of the filter region.

A percentage refers to the width of the filter region.  The value must be finite and defaults to 0.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_OriginX(extPavingFX *Self, Unit &Value)
{
   return update_paving_unit(Self, Self->OriginX, Value, false, false);
}

/*********************************************************************************************************************

-FIELD-
OriginY: The vertical origin of the pattern, relative to the top edge of the filter region.

A percentage refers to the height of the filter region.  The value must be finite and defaults to 0.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_OriginY(extPavingFX *Self, Unit &Value)
{
   return update_paving_unit(Self, Self->OriginY, Value, false, false);
}

/*********************************************************************************************************************

-FIELD-
OutputMode: Selects an image or a floating-point scalar result.
Lookup: FOM

`IMAGE`, the default, writes RGBA8 pixels (coloured in `COLOUR` mode, or greyscale/colour-mapped for scalar modes).
`HEIGHT` writes unclamped floating-point scalars for `HEIGHT`, `MASK` and `ID` modes, and is rejected at draw time
when #Mode is `COLOUR`.  A `HEIGHT` effect cannot be the final step in a filter.  `NATIVE` is unsupported.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_OutputMode(extPavingFX *Self, FOM Value)
{
   if ((Value != FOM::IMAGE) and (Value != FOM::HEIGHT)) return ERR::OutOfRange;
   if (Self->OutputMode != Value) {
      Self->OutputMode = Value;
      Self->Extension.Format = (Value IS FOM::HEIGHT) ?
         filter_surface_format::SCALAR_F32 : filter_surface_format::RGBA8;
      if (Self->Filter) Self->Filter->GraphRevision++;
      paving_modified(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PeriodX: Repeats the pattern horizontally after this number of layout cells.

Periods count elements for `RUNNING`, `STACK` and `CHECKER`, square macro-cells of side `ElementWidth` for
`BASKET_WEAVE`, and `2 * ElementWidth` fundamental tiles for `HERRINGBONE`.  The value must lie in [0, 1048576],
and 0, the default, disables horizontal repetition.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_PeriodX(extPavingFX *Self, int Value)
{
   if ((Value < 0) or (Value > PAVING_MAX_PERIOD)) return ERR::OutOfRange;
   return update_paving(Self, Self->PeriodX, Value);
}

/*********************************************************************************************************************

-FIELD-
PeriodY: Repeats the pattern vertically after this number of layout cells.

The value must lie in [0, 1048576], and 0, the default, disables vertical repetition.  See #PeriodX.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_PeriodY(extPavingFX *Self, int Value)
{
   if ((Value < 0) or (Value > PAVING_MAX_PERIOD)) return ERR::OutOfRange;
   return update_paving(Self, Self->PeriodY, Value);
}

/*********************************************************************************************************************

-FIELD-
ResampleMethod: Selects interpolation for #ElementMap in `LOCAL` fill mode.
Lookup: VSM

Accepts `NEIGHBOUR` and `BILINEAR` (the default).

*********************************************************************************************************************/

static ERR PAVINGFX_SET_ResampleMethod(extPavingFX *Self, VSM Value)
{
   if ((Value != VSM::NEIGHBOUR) and (Value != VSM::BILINEAR)) return ERR::OutOfRange;
   return update_paving(Self, Self->ResampleMethod, Value);
}

/*********************************************************************************************************************

-FIELD-
RowOffset: Horizontal shift between consecutive courses in `RUNNING` layout, as a fraction of #ElementWidth.

The value must be finite and defaults to 0.5 (half-brick running bond).  Ignored by other layouts.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_RowOffset(extPavingFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_paving(Self, Self->RowOffset, Value);
}

/*********************************************************************************************************************

-FIELD-
Seed: Selects deterministic per-element appearance and height variation.

Any 64-bit integer is accepted, including 0, which is the default.  The same seed always produces the same pattern.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_Seed(extPavingFX *Self, int64_t Value)
{
   return update_paving(Self, Self->Seed, Value);
}

/*********************************************************************************************************************

-FIELD-
Stops: Draws scalar `IMAGE` output through a custom colour ramp.

Applies when #Mode is `HEIGHT`, `MASK` or `ID` and #OutputMode is `IMAGE`.  At least two stops are required, or the
setter fails with `ERR::InvalidValue`.  Setting stops replaces any named #ColourMap; set an empty array to clear the
ramp and restore greyscale output.

*********************************************************************************************************************/

static ERR PAVINGFX_SET_Stops(extPavingFX *Self, std::span<const GradientStop> &Value)
{
   if (auto error = set_colour_ramp_stops(*Self, Value); error != ERR::Okay) return error;
   paving_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR paving_xml(const extPavingFX &Effect, filter_xml_element &Element)
{
   auto length = [&](const Unit &Value) {
      return Value.scaled() ? Element.number(Value.Value / 0.01) + '%' : Element.number(Value.Value);
   };

   auto same_length = [](const Unit &A, const Unit &B) {
      return (A.Value IS B.Value) and (A.scaled() IS B.scaled());
   };

   Element.Name = "xvg:fePaving";

   static constexpr std::string_view layouts[] = { "running", "stack", "basketWeave", "herringbone", "checker" };
   if (Effect.Layout != LAY::RUNNING) Element.attribute("layout", layouts[int(Effect.Layout)]);

   static constexpr std::string_view modes[] = { "colour", "height", "mask", "id" };
   if (Effect.Mode != LMD::COLOUR) Element.attribute("mode", modes[int(Effect.Mode)]);

   if (Effect.Seed) Element.attribute("seed", Effect.Seed);

   const Unit default_w(0.25, FD_SCALED), default_h(0.125, FD_SCALED);
   if ((not same_length(Effect.ElementWidth, default_w)) or (not same_length(Effect.ElementHeight, default_h))) {
      if (same_length(Effect.ElementWidth, Effect.ElementHeight)) {
         Element.attribute("elementSize", length(Effect.ElementWidth));
      }
      else {
         Element.attribute("elementSize", length(Effect.ElementWidth) + ' ' + length(Effect.ElementHeight));
      }
   }

   if ((Effect.OriginX.Value != 0) or (Effect.OriginY.Value != 0)) {
      Element.attribute("origin", length(Effect.OriginX) + ' ' + length(Effect.OriginY));
   }

   const Unit default_joint(0.01, FD_SCALED), default_bevel(0.01, FD_SCALED);
   if (not same_length(Effect.JointWidth, default_joint)) Element.attribute("jointWidth", length(Effect.JointWidth));
   if (not same_length(Effect.BevelWidth, default_bevel)) Element.attribute("bevelWidth", length(Effect.BevelWidth));

   static constexpr std::string_view profiles[] = {
      "linear", "in", "out", "inOut", "cubicIn", "cubicOut", "cubicInOut", "circleIn", "circleOut", "circleInOut"
   };
   if (Effect.BevelProfile != GEZ::LINEAR) Element.attribute("bevelProfile", profiles[int(Effect.BevelProfile)]);

   if (Effect.Angle != 0) Element.attribute("angle", Effect.Angle);
   if (Effect.RowOffset != 0.5) Element.attribute("rowOffset", Effect.RowOffset);
   if (Effect.ElementHeightValue != 1.0) Element.attribute("elementHeight", Effect.ElementHeightValue);
   if (Effect.JointHeightValue != 0.0) Element.attribute("jointHeight", Effect.JointHeightValue);
   if (Effect.HueVariation != 0.0) Element.attribute("hueVariation", Effect.HueVariation);
   if (Effect.ChromaVariation != 0.0) Element.attribute("chromaVariation", Effect.ChromaVariation);
   if (Effect.LightVariation != 0.0) Element.attribute("lightVariation", Effect.LightVariation);
   if (Effect.HeightVariation != 0.0) Element.attribute("heightVariation", Effect.HeightVariation);

   if (not same_colour(Effect.ElementColour, DEFAULT_ELEMENT_COLOUR)) {
      write_filter_xml_colour(Element, "elementColor", Effect.ElementColour);
   }
   if (not same_colour(Effect.JointColour, DEFAULT_JOINT_COLOUR)) {
      write_filter_xml_colour(Element, "jointColor", Effect.JointColour);
   }
   if (not same_colour(Effect.AlternateColour, DEFAULT_ALTERNATE_COLOUR)) {
      write_filter_xml_colour(Element, "alternateColor", Effect.AlternateColour);
   }

   if (Effect.FillMode IS LFM::CONTINUOUS) Element.attribute("fillMode", "continuous");
   if (Effect.ResampleMethod IS VSM::NEIGHBOUR) Element.attribute("resampleMethod", "neighbour");

   if ((Effect.PeriodX != 0) or (Effect.PeriodY != 0)) {
      if (Effect.PeriodX IS Effect.PeriodY) Element.attribute("period", Effect.PeriodX);
      else Element.attribute("period", Element.number(Effect.PeriodX) + ' ' + Element.number(Effect.PeriodY));
   }

   if (Effect.OutputMode IS FOM::HEIGHT) Element.attribute("xvg:outputMode", "height");

   return colour_ramp_xml(Effect, Element);
}

//********************************************************************************************************************

#include "filter_paving_def.c"

static const FieldArray clPavingFXFields[] = {
   { "ColourMap",          FDF_CPPSTRING|FDF_RW, nullptr, PAVINGFX_SET_ColourMap },
   { "Stops",              FDF_VECTOR|FDF_STRUCT|FDF_RW, nullptr, PAVINGFX_SET_Stops, "GradientStop" },
   { "Seed",               FDF_INT64|FDF_RW, nullptr, PAVINGFX_SET_Seed },
   { "ElementWidth",       FDF_UNIT|FDF_RW, nullptr, PAVINGFX_SET_ElementWidth },
   { "ElementHeight",      FDF_UNIT|FDF_RW, nullptr, PAVINGFX_SET_ElementHeight },
   { "OriginX",            FDF_UNIT|FDF_RW, nullptr, PAVINGFX_SET_OriginX },
   { "OriginY",            FDF_UNIT|FDF_RW, nullptr, PAVINGFX_SET_OriginY },
   { "JointWidth",         FDF_UNIT|FDF_RW, nullptr, PAVINGFX_SET_JointWidth },
   { "BevelWidth",         FDF_UNIT|FDF_RW, nullptr, PAVINGFX_SET_BevelWidth },
   { "Angle",              FDF_DOUBLE|FDF_RW, nullptr, PAVINGFX_SET_Angle },
   { "RowOffset",          FDF_DOUBLE|FDF_RW, nullptr, PAVINGFX_SET_RowOffset },
   { "ElementHeightValue", FDF_DOUBLE|FDF_RW, nullptr, PAVINGFX_SET_ElementHeightValue },
   { "JointHeightValue",   FDF_DOUBLE|FDF_RW, nullptr, PAVINGFX_SET_JointHeightValue },
   { "HueVariation",       FDF_DOUBLE|FDF_RW, nullptr, PAVINGFX_SET_HueVariation },
   { "ChromaVariation",    FDF_DOUBLE|FDF_RW, nullptr, PAVINGFX_SET_ChromaVariation },
   { "LightVariation",     FDF_DOUBLE|FDF_RW, nullptr, PAVINGFX_SET_LightVariation },
   { "HeightVariation",    FDF_DOUBLE|FDF_RW, nullptr, PAVINGFX_SET_HeightVariation },
   { "ElementColour",      FDF_STRUCT|FDF_RW, nullptr, PAVINGFX_SET_ElementColour, "FRGB" },
   { "JointColour",        FDF_STRUCT|FDF_RW, nullptr, PAVINGFX_SET_JointColour, "FRGB" },
   { "AlternateColour",    FDF_STRUCT|FDF_RW, nullptr, PAVINGFX_SET_AlternateColour, "FRGB" },
   { "Layout",             FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, PAVINGFX_SET_Layout, &clPavingFXLAY },
   { "Mode",               FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, PAVINGFX_SET_Mode, &clPavingFXLMD },
   { "FillMode",           FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, PAVINGFX_SET_FillMode, &clPavingFXLFM },
   { "BevelProfile",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, PAVINGFX_SET_BevelProfile, &clPavingFXGEZ },
   { "OutputMode",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, PAVINGFX_SET_OutputMode, &clPavingFXFOM },
   { "ResampleMethod",     FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, PAVINGFX_SET_ResampleMethod, &clPavingFXVSM },
   { "PeriodX",            FDF_INT|FDF_RW, nullptr, PAVINGFX_SET_PeriodX },
   { "PeriodY",            FDF_INT|FDF_RW, nullptr, PAVINGFX_SET_PeriodY },
   // Virtual fields
   { "ElementMap",         FDF_VIRTUAL|FDF_OBJECT|FDF_RW|FDF_PURE, PAVINGFX_GET_ElementMap, PAVINGFX_SET_ElementMap, CLASSID::FILTEREFFECT },
   { "JointMap",           FDF_VIRTUAL|FDF_OBJECT|FDF_RW|FDF_PURE, PAVINGFX_GET_JointMap, PAVINGFX_SET_JointMap, CLASSID::FILTEREFFECT },
   { "XMLDef",             FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_pavingfx(void)
{
   clPavingFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::PAVINGFX),
      fl::Name("PavingFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clPavingFXActions),
      fl::Fields(clPavingFXFields),
      fl::Size(sizeof(extPavingFX)),
      fl::Path(MOD_PATH));

   return clPavingFX ? ERR::Okay : ERR::AddClass;
}
