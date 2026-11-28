// TODO: Support ParameterMaps (a filter input that varies a field per sample) for Jitter, HeightMin, HeightMax and
// Persistence, once a binding facility shared by the filter classes has been designed.  cellular_sampler fixes these
// values at construction (m_half_jitter, the height limits and the octave weights), so they must become per-sample
// arguments.  The exact search stops on bounds derived from Jitter and the height range, so it must use the largest
// value that each map can supply in the drawn area, or the worst case of Jitter 1 and the full [-4, 4] height range.

/*********************************************************************************************************************

-CLASS-
CellularFX: Generates cellular (Worley) noise for stone, foam, cracks, cells and similar textures.

CellularFX is a source effect that takes no input.  It places one feature point in every cell of an unbounded
lattice and derives each output sample from the distances between the sample and its nearest features.  The
result fills the primitive subregion, and every sample depends only on its position in primitive units.  Clipped,
partial and full redraws therefore produce identical values.

The lattice is defined by #CellWidth and #CellHeight, positioned by #OriginX and #OriginY, and rotated by #Angle.
Percentages in these fields refer to the filter region, so `CellWidth = 12.5%` gives eight cells across it.  Each
feature is displaced from its cell centre by up to #Jitter, and the positions, heights and cell values are chosen by
#Seed.  Distances are measured in lattice units, in which one cell measures 1 &times; 1, and #WorleyMetric selects
the distance function.

#WorleyMode selects how the distances are converted to a value.  `F1`, `F2` and `F2_F1` produce distances to the
nearest features, `WEIGHTED` and `PEAKS` combine distances with the seeded feature heights from #HeightMin to
#HeightMax, and `CELL` gives each cell a constant seeded value in [0, 1).  Values are not normalised against the
drawn area, and the range of each mode is described in #WorleyMode.

#Octaves sums independently seeded layers, each sampled #Lacunarity times more finely than the last and weighted by
#Persistence.  The sum is divided by the total weight, so several octaves stay within the range of a single octave.
#PeriodX and #PeriodY make the pattern repeat after a whole number of cells.

In `IMAGE` mode, values are clamped to [0, 1] and drawn as opaque greyscale, or through the colour ramp of
#ColourMap or #Stops.  Set #OutputMode to `HEIGHT` to produce an unclamped floating-point scalar field for numerical
inputs, such as the height map of an enhanced @LightingFX or the amount mask of a @CompositeFX.  Prefer `HEIGHT` for
lighting, because the 8-bit greyscale of `IMAGE` output produces visible terraces.

Greyscale output is written in the effect's colour space.  In a linear RGB filter, which is the SVG default, values
are therefore treated as linear light and appear brighter once they are converted for display.  For example, 0.25
is displayed as 135 rather than 64.  Draw the effect in an sRGB filter to display values directly.  Colour ramps are
drawn as authored in either colour space.

Some combinations of fields can only be checked when the effect is drawn, because fields may be set in any order.
`CELL` with more than one octave, a period with an #Angle that is not a multiple of 360, and a periodic octave
stack with a non-integer #Lacunarity each cause the effect to draw nothing.  `Draw` then logs a warning and returns
`ERR::InvalidValue`.  `Draw` also returns `ERR::OutOfRange` if the lattice coordinates of the drawn area would
exceed 2^52, beyond which cell positions are not exact.

Samples are taken once at each pixel centre, so the hard edges of `CELL` and `F2_F1` output are not antialiased.

SVG documents can use the effect with the `xvg:feCellular` element.  Refer to the @SVG class for its attributes.

-END-

*********************************************************************************************************************/

#include "cellular_noise.h"
#include "filter_colour_ramp.h"

class extCellularFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CELLULARFX;
   static constexpr CSTRING CLASS_NAME = "CellularFX";
   using create = kt::Create<extCellularFX>;

   // The exported fields are declared first, in the order of the field table, so that their offsets match it.
   // Eight-byte fields precede four-byte fields so that no padding is required between them.
   std::string ColourMap;
   kt::vector<GradientStop> Stops;
   int64_t Seed = 0;
   Unit CellWidth = Unit(0.125, FD_SCALED);
   Unit CellHeight = Unit(0.125, FD_SCALED);
   Unit OriginX = Unit(0.0);
   Unit OriginY = Unit(0.0);
   double Angle = 0;
   double Jitter = 1.0;
   double HeightMin = 0;
   double HeightMax = 1.0;
   double Persistence = 0.5;
   double Lacunarity = 2.0;
   WLF WorleyMode = WLF::F1;
   WLM WorleyMetric = WLM::EUCLIDEAN;
   int Octaves = 1;
   int PeriodX = 0;
   int PeriodY = 0;
   FOM OutputMode = FOM::IMAGE;

   std::unique_ptr<GradientColours> Colours;
   objBitmap *Bitmap = nullptr;
   bool Dirty = true;
   agg::trans_affine CacheTransform;
   TClipRectangle<double> CacheRegion = {};
   TClipRectangle<double> CacheDomain = {};
   VCS CacheColourSpace = VCS::INHERIT;

   cellular_params params() const {
      return {
         .Mode        = cellular_mode(int(WorleyMode)),
         .Metric      = WorleyMetric,
         .Seed        = Seed,
         .Jitter      = Jitter,
         .HeightMin   = HeightMin,
         .HeightMax   = HeightMax,
         .Octaves     = Octaves,
         .Persistence = Persistence,
         .Lacunarity  = Lacunarity,
         .PeriodX     = PeriodX,
         .PeriodY     = PeriodY
      };
   }

   extCellularFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::NONE;
   }

   ~extCellularFX() {
      if (Bitmap) FreeResource(Bitmap);
   }
};

//********************************************************************************************************************
// Builds the transform from primitive units to base lattice units.  Domain is the filter region in primitive units.
// Returns ERR::NothingDone if the domain collapses a percentage cell size to zero.

static ERR cellular_lattice_transform(const extCellularFX *Self, const TClipRectangle<double> &Domain,
   double Angle, agg::trans_affine &Lattice)
{
   const double width = Domain.width(), height = Domain.height();
   auto resolve = [](const Unit &Length, double Reference) {
      return Length.scaled() ? Length.Value * Reference : Length.Value;
   };

   const double cell_width = resolve(Self->CellWidth, width);
   const double cell_height = resolve(Self->CellHeight, height);
   const double origin_x = Domain.left + resolve(Self->OriginX, width);
   const double origin_y = Domain.top + resolve(Self->OriginY, height);
   if ((not std::isfinite(cell_width)) or (not std::isfinite(cell_height)) or (not std::isfinite(origin_x)) or
       (not std::isfinite(origin_y))) return ERR::InvalidValue;
   if ((cell_width <= 0) or (cell_height <= 0)) return ERR::NothingDone;

   Lattice.reset();
   Lattice.translate(-origin_x, -origin_y);
   if (Angle != 0) Lattice.rotate(-Angle * agg::pi / 180.0);
   Lattice.scale(1.0 / cell_width, 1.0 / cell_height);
   return finite_transform(Lattice) ? ERR::Okay : ERR::InvalidValue;
}

//********************************************************************************************************************
// Checks the combinations of fields that individual setters cannot, because fields may be set in any order.

static ERR validate_cellular(const extCellularFX *Self, const cellular_params &Params, double Angle)
{
   kt::Log log("CellularFX");

   if (cellular_check(Params) != ERR::Okay) {
      if ((Params.Mode IS cellular_mode::CELL) and (Params.Octaves > 1)) {
         log.warning("#%d: The CELL mode supports one octave only.", Self->UID);
      }
      else log.warning("#%d: A periodic octave stack requires an integer Lacunarity.", Self->UID);
      return ERR::InvalidValue;
   }

   if (((Params.PeriodX > 0) or (Params.PeriodY > 0)) and (Angle != 0)) {
      log.warning("#%d: A periodic pattern cannot be rotated.", Self->UID);
      return ERR::InvalidValue;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.

Draw fails with `ERR::InvalidValue` if `CELL` mode is combined with more than one octave, if a periodic pattern is
rotated, or if a periodic pattern of several octaves has a fractional #Lacunarity.  It fails with `ERR::OutOfRange`
if the drawn area lies so far from the origin, at the finest octave, that cell indices can no longer be represented
exactly.
-END-
*********************************************************************************************************************/

static ERR CELLULARFX_Draw(extCellularFX *Self, struct acDraw *Args)
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

   const auto params = Self->params();
   const double angle = std::fmod(Self->Angle, 360.0);
   if (auto error = validate_cellular(Self, params, angle); error != ERR::Okay) return error;

   const auto &render = *Self->Filter->Render;
   const auto clip = render.scratch_clip();
   const int width = clip.right;
   const int height = clip.bottom;
   if ((width <= 0) or (height <= 0)) return ERR::Okay;

   // Express the primitive subregion, the filter region and the inverse sampling transform in primitiveUnits.

   double origin_x = 0, origin_y = 0, unit_width = 1, unit_height = 1;
   if (render.PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      const auto &bounds = render.ClientBounds;
      origin_x = bounds.left;
      origin_y = bounds.top;
      unit_width = bounds.width();
      unit_height = bounds.height();
      if ((unit_width <= 0) or (unit_height <= 0)) return ERR::Okay;
   }

   auto to_primitive = [&](const TClipRectangle<double> &Area) -> TClipRectangle<double> {
      return { (Area.left - origin_x) / unit_width, (Area.top - origin_y) / unit_height,
         (Area.right - origin_x) / unit_width, (Area.bottom - origin_y) / unit_height };
   };
   const auto region = to_primitive(step->Subregion);
   const auto domain = to_primitive(render.FilterRegion);

   agg::trans_affine sampling;
   sampling.scale(unit_width, unit_height);
   sampling.translate(origin_x, origin_y);
   sampling *= render.Transform;
   TClipRectangle<double> samples = { filter_pixel_centre(0), filter_pixel_centre(0),
      filter_pixel_centre(width - 1), filter_pixel_centre(height - 1) };
   if ((not finite_region(region)) or (not finite_region(domain)) or (not invert_filter_transform(sampling)) or
       (not transform_filter_region(samples, sampling))) return ERR::InvalidValue;

   // Only pixel centres inside both the processing bitmap and the primitive can reach the lattice.
   if ((not region.valid()) or (samples.right < region.left) or (samples.bottom < region.top) or
       (samples.left >= region.right) or (samples.top >= region.bottom)) return ERR::Okay;
   samples.shrinking(region);

   agg::trans_affine lattice;
   if (auto error = cellular_lattice_transform(Self, domain, angle, lattice); error != ERR::Okay) {
      return (error IS ERR::NothingDone) ? ERR::Okay : error;
   }

   const cellular_sampler sampler(params);
   double extent = 0;
   for (auto [x, y] : { std::pair(samples.left, samples.top), std::pair(samples.right, samples.top),
         std::pair(samples.left, samples.bottom), std::pair(samples.right, samples.bottom) }) {
      lattice.transform(&x, &y);
      extent = std::max({ extent, std::abs(x), std::abs(y) });
   }
   if (not sampler.lattice_fits(extent)) return ERR::OutOfRange;

   if (not scalar) {
      if (not Self->Bitmap) {
         Self->Dirty = true;
         if (not (Self->Bitmap = objBitmap::create::local(fl::Name("cellular_bmp"),
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
   }

   const bool linear_rgb = effect_colour_space(Self) IS VCS::LINEAR_RGB;
   const auto colours = Self->Colours.get();

   int thread_count = std::thread::hardware_concurrency();
   if (thread_count > height / 4) thread_count = height / 4;
   if (thread_count < 1) thread_count = 1;
   BS::thread_pool pool(thread_count);

   for (int i=0; i < thread_count; i++) {
      pool.detach_task([=, &sampler, start = (height * i) / thread_count, end = (height * (i + 1)) / thread_count]() {
         // Calls WritePixel(X, Y, Covered, Value) for each pixel in the band.  Value is meaningless when uncovered.
         auto draw_band = [&](auto WritePixel) {
            for (int y=start; y < end; ++y) {
               for (int x=0; x < width; ++x) {
                  double px, py;
                  map_filter_pixel_centre(sampling, x, y, px, py);
                  const bool covered = (px >= region.left) and (px < region.right) and
                     (py >= region.top) and (py < region.bottom);
                  double value = 0;
                  if (covered) {
                     lattice.transform(&px, &py);
                     value = sampler.sample(px, py);
                  }
                  WritePixel(x, y, covered, value);
               }
            }
         };

         if (scalar) {
            draw_band([&](int X, int Y, bool Covered, double Value) {
               auto row = heights + std::ptrdiff_t(Y) * (layout.Stride / sizeof(float));
               row[X] = Covered ? float(Value) : 0.0f;
            });
         }
         else {
            const auto bitmap = Self->Bitmap;
            const uint8_t a = bitmap->ColourFormat->AlphaPos>>3;
            const uint8_t r = bitmap->ColourFormat->RedPos>>3;
            const uint8_t g = bitmap->ColourFormat->GreenPos>>3;
            const uint8_t b = bitmap->ColourFormat->BluePos>>3;
            draw_band([&](int X, int Y, bool Covered, double Value) {
               auto pixel = bitmap->Data + std::ptrdiff_t(Y) * bitmap->LineWidth + std::ptrdiff_t(X) * 4;
               if (not Covered) {
                  pixel[r] = pixel[g] = pixel[b] = pixel[a] = 0;
                  return;
               }

               if (colours) write_colour_ramp_pixel(*colours, Value, pixel, r, g, b, a);
               else {
                  const auto level = uint8_t(std::lround(std::clamp(Value, 0.0, 1.0) * 255.0));
                  pixel[r] = pixel[g] = pixel[b] = linear_rgb ? glLinearRGB.invert(level) : level;
                  pixel[a] = 255;
               }
            });
         }
      });
   }
   pool.wait();

   if (scalar) return ERR::Okay;

   Self->CacheTransform = sampling;
   Self->CacheRegion = region;
   Self->CacheDomain = domain;
   Self->CacheColourSpace = effect_colour_space(Self);
   Self->Dirty = false;
   return gfx::CopyArea(Self->Bitmap, Self->Target, BAF::NIL, 0, 0, width, height, 0, 0);
}

//********************************************************************************************************************
// Field updates invalidate the cached image and notify the scene only when the value changes.

static void cellular_modified(extCellularFX *Self)
{
   Self->Dirty = true;
   effect_modified(Self);
}

template <class T> static ERR update_cellular(extCellularFX *Self, T &Field, T Value)
{
   if (Field != Value) {
      Field = Value;
      cellular_modified(Self);
   }
   return ERR::Okay;
}

static ERR update_cellular_range(extCellularFX *Self, double &Field, double Value, double Min, double Max)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   if ((Value < Min) or (Value > Max)) return ERR::OutOfRange;
   return update_cellular(Self, Field, Value);
}

static ERR update_cellular_unit(extCellularFX *Self, Unit &Field, const Unit &Value, bool Positive)
{
   if (not std::isfinite(Value.Value)) return ERR::InvalidValue;
   if (Positive and (Value.Value <= 0)) return ERR::OutOfRange;
   if ((Field.Value != Value.Value) or (Field.scaled() != Value.scaled())) {
      Field = Unit(Value.Value, Value.scaled() ? FD_SCALED : 0);
      cellular_modified(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Angle: Rotates the lattice clockwise about its origin, in degrees.

The rotation is applied about the point given by #OriginX and #OriginY.  The value must be finite and defaults to 0.
A periodic pattern cannot be rotated: if #PeriodX or #PeriodY is set, the Angle must be a multiple of 360 or the
effect will fail to draw.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_Angle(extCellularFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   return update_cellular(Self, Self->Angle, Value);
}

/*********************************************************************************************************************

-FIELD-
CellHeight: The vertical spacing of the base lattice.

The value must be finite and greater than zero.  A percentage refers to the height of the filter region.  The
default is 12.5%, which gives eight rows of cells within the filter region.

Distances are measured in cell units, so cells of different width and height stretch the distance metric with them.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_CellHeight(extCellularFX *Self, Unit &Value)
{
   return update_cellular_unit(Self, Self->CellHeight, Value, true);
}

/*********************************************************************************************************************

-FIELD-
CellWidth: The horizontal spacing of the base lattice.

The value must be finite and greater than zero.  A percentage refers to the width of the filter region.  The default
is 12.5%, which gives eight columns of cells within the filter region.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_CellWidth(extCellularFX *Self, Unit &Value)
{
   return update_cellular_unit(Self, Self->CellWidth, Value, true);
}

/*********************************************************************************************************************

-FIELD-
ColourMap: Draws `IMAGE` output through a pre-defined colour ramp.

The clamped value of each sample selects a colour from the named colourmap, from the first colour at 0 to the last at
1.  The following colourmaps from the matplotlib and seaborn projects are supported: `cmap:crest`, `cmap:flare`,
`cmap:icefire`, `cmap:inferno`, `cmap:magma`, `cmap:mako`, `cmap:plasma`, `cmap:rocket`, `cmap:viridis`.

Setting a colourmap replaces custom #Stops.  Set an empty string to clear the ramp and draw in greyscale.  `HEIGHT`
output ignores the ramp.  Unknown names fail with `ERR::NotFound`.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_ColourMap(extCellularFX *Self, const std::string_view &Value)
{
   if (auto error = set_colour_ramp_map(*Self, Value); error != ERR::Okay) return error;
   cellular_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
HeightMax: The highest seeded feature height, for the `WEIGHTED` and `PEAKS` modes.

Each feature is given a height between #HeightMin and HeightMax, in cell units.  The value must lie in the range
[-4, 4] and defaults to 1.  If HeightMax is lower than #HeightMin, the two are swapped when drawing.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_HeightMax(extCellularFX *Self, double Value)
{
   return update_cellular_range(Self, Self->HeightMax, Value, -CELLULAR_MAX_HEIGHT, CELLULAR_MAX_HEIGHT);
}

/*********************************************************************************************************************

-FIELD-
HeightMin: The lowest seeded feature height, for the `WEIGHTED` and `PEAKS` modes.

The value must lie in the range [-4, 4] and defaults to 0.  See #HeightMax.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_HeightMin(extCellularFX *Self, double Value)
{
   return update_cellular_range(Self, Self->HeightMin, Value, -CELLULAR_MAX_HEIGHT, CELLULAR_MAX_HEIGHT);
}

/*********************************************************************************************************************

-FIELD-
Jitter: The maximum displacement of each feature from its cell centre, as a fraction of the cell.

A value of 0 places every feature at its cell centre, giving a regular grid of square or rectangular cells.  A value
of 1, the default, allows a feature to lie anywhere within its cell.  The value must lie in the range [0, 1].

*********************************************************************************************************************/

static ERR CELLULARFX_SET_Jitter(extCellularFX *Self, double Value)
{
   return update_cellular_range(Self, Self->Jitter, Value, 0.0, 1.0);
}

/*********************************************************************************************************************

-FIELD-
Lacunarity: The frequency multiplier between octaves.

Each octave samples the lattice Lacunarity times more finely than the previous one.  The value must lie in the range
[1, 8] and defaults to 2.  A periodic pattern of more than one octave requires an integer value, or the effect will
fail to draw.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_Lacunarity(extCellularFX *Self, double Value)
{
   return update_cellular_range(Self, Self->Lacunarity, Value, 1.0, 8.0);
}

/*********************************************************************************************************************

-FIELD-
Octaves: The number of independently seeded layers.

Each octave is sampled at #Lacunarity times the frequency of the previous one and weighted by #Persistence relative
to it.  The weighted sum is divided by the total weight, so several octaves stay within the range of one.  The value
must lie in the range [1, 12] and defaults to 1.  The `CELL` mode supports one octave only.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_Octaves(extCellularFX *Self, int Value)
{
   if ((Value < 1) or (Value > CELLULAR_MAX_OCTAVES)) return ERR::OutOfRange;
   return update_cellular(Self, Self->Octaves, Value);
}

/*********************************************************************************************************************

-FIELD-
OriginX: The horizontal origin of the lattice, relative to the left edge of the filter region.

A percentage refers to the width of the filter region.  The value must be finite and defaults to 0.  Moving the
origin translates the pattern without changing it, and #Angle rotates the lattice about this point.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_OriginX(extCellularFX *Self, Unit &Value)
{
   return update_cellular_unit(Self, Self->OriginX, Value, false);
}

/*********************************************************************************************************************

-FIELD-
OriginY: The vertical origin of the lattice, relative to the top edge of the filter region.

A percentage refers to the height of the filter region.  The value must be finite and defaults to 0.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_OriginY(extCellularFX *Self, Unit &Value)
{
   return update_cellular_unit(Self, Self->OriginY, Value, false);
}

/*********************************************************************************************************************

-FIELD-
OutputMode: Selects an image or a floating-point scalar result.
Lookup: FOM

`IMAGE`, the default, draws clamped greyscale or colour-mapped pixels.  `HEIGHT` writes the unclamped field values
to a scalar surface for numerical inputs and ignores the colour ramp.  A height cannot be the final result of a
filter, so a `HEIGHT` effect must be read by a later effect.  `NATIVE` is unsupported.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_OutputMode(extCellularFX *Self, FOM Value)
{
   if ((Value != FOM::IMAGE) and (Value != FOM::HEIGHT)) return ERR::OutOfRange;

   if (Self->OutputMode != Value) {
      Self->OutputMode = Value;
      if (Self->Filter) Self->Filter->GraphRevision++;
      cellular_modified(Self);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PeriodX: Repeats the pattern horizontally after this number of base cells.

The pattern repeats exactly every `PeriodX * CellWidth`, with no seam.  The value must lie in the range [0, 1048576],
and 0, the default, disables horizontal repetition.  A periodic pattern cannot be rotated, and a periodic pattern of
several octaves requires an integer #Lacunarity.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_PeriodX(extCellularFX *Self, int Value)
{
   if ((Value < 0) or (Value > CELLULAR_MAX_PERIOD)) return ERR::OutOfRange;
   return update_cellular(Self, Self->PeriodX, Value);
}

/*********************************************************************************************************************

-FIELD-
PeriodY: Repeats the pattern vertically after this number of base cells.

The value must lie in the range [0, 1048576], and 0, the default, disables vertical repetition.  See #PeriodX.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_PeriodY(extCellularFX *Self, int Value)
{
   if ((Value < 0) or (Value > CELLULAR_MAX_PERIOD)) return ERR::OutOfRange;
   return update_cellular(Self, Self->PeriodY, Value);
}

/*********************************************************************************************************************

-FIELD-
Persistence: The weight of each octave relative to the previous one.

Octave `k` is weighted by `Persistence^k`.  The value must lie in the range [0, 1] and defaults to 0.5.  A value of 0
leaves only the first octave.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_Persistence(extCellularFX *Self, double Value)
{
   return update_cellular_range(Self, Self->Persistence, Value, 0.0, 1.0);
}

/*********************************************************************************************************************

-FIELD-
Seed: Selects the feature positions, heights and cell values.

Any 64-bit value is accepted, including 0, which is the default.  The same seed always produces the same pattern.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_Seed(extCellularFX *Self, int64_t Value)
{
   return update_cellular(Self, Self->Seed, Value);
}

/*********************************************************************************************************************

-FIELD-
Stops: Draws `IMAGE` output through a custom colour ramp.

The clamped value of each sample selects a colour from the gradient defined by the stops.  At least two stops are
required, or the setter fails with `ERR::InvalidValue`.

Setting stops replaces any named #ColourMap.  Set an empty array to clear the ramp and draw in greyscale.  `HEIGHT`
output ignores the ramp.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_Stops(extCellularFX *Self, std::span<const GradientStop> &Value)
{
   if (auto error = set_colour_ramp_stops(*Self, Value); error != ERR::Okay) return error;
   cellular_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
WorleyMetric: Selects the distance metric.
Lookup: WLM

`EUCLIDEAN` (the default) gives rounded cells, `MANHATTAN` gives diamond-shaped contours and `CHEBYSHEV` gives square
contours.  Distances are measured in cell units.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_WorleyMetric(extCellularFX *Self, WLM Value)
{
   if ((int(Value) < int(WLM::EUCLIDEAN)) or (int(Value) > int(WLM::CHEBYSHEV))) return ERR::OutOfRange;
   return update_cellular(Self, Self->WorleyMetric, Value);
}

/*********************************************************************************************************************

-FIELD-
WorleyMode: Selects how distances to the nearest features are converted to a value.
Lookup: WLF

Distances are in cell units and are not normalised against the drawn area.  With `F1` the distance to the nearest
feature is used, and with `F2` the distance to the second-nearest.  `F2_F1` is their difference, which is zero on
the boundaries between cells and suits cracks and mortar.  `WEIGHTED` is the minimum over all features of the
distance less the feature's height, and `PEAKS` is the maximum of the height less the distance.  `CELL` gives every
sample the seeded value of its nearest feature, which is constant within each cell and lies in [0, 1).

With the default #Jitter of 1, `F1` rarely exceeds 0.8 under the Euclidean metric, and `F2` can exceed 1.  On a
regular grid (Jitter 0), `F1` reaches about 0.707, 1 or 0.5 at cell corners under the Euclidean, Manhattan and Chebyshev
metrics, and `F2` is 1 at a cell centre under every metric.  `WEIGHTED` and `PEAKS` may be negative.  `IMAGE` output
clamps every mode to [0, 1]; use a colour ramp, @RemapFX or `HEIGHT` output to work with other ranges.

The `CELL` mode supports one octave only.

*********************************************************************************************************************/

static ERR CELLULARFX_SET_WorleyMode(extCellularFX *Self, WLF Value)
{
   if ((int(Value) < int(WLF::F1)) or (int(Value) > int(WLF::CELL))) return ERR::OutOfRange;
   return update_cellular(Self, Self->WorleyMode, Value);
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR cellular_xml(const extCellularFX &Effect, filter_xml_element &Element)
{
   auto length = [&](const Unit &Value) {
      // Invert the percentage conversion in Unit::read() so that the value reloads unchanged.
      return Value.scaled() ? Element.number(Value.Value / 0.01) + '%' : Element.number(Value.Value);
   };

   auto same_length = [](const Unit &A, const Unit &B) {
      return (A.Value IS B.Value) and (A.scaled() IS B.scaled());
   };

   Element.Name = "xvg:feCellular";
   if (Effect.Seed) Element.attribute("seed", Effect.Seed);

   const Unit default_cell(0.125, FD_SCALED);
   if ((not same_length(Effect.CellWidth, default_cell)) or (not same_length(Effect.CellHeight, default_cell))) {
      if (same_length(Effect.CellWidth, Effect.CellHeight)) Element.attribute("cellSize", length(Effect.CellWidth));
      else Element.attribute("cellSize", length(Effect.CellWidth) + ' ' + length(Effect.CellHeight));
   }

   if ((Effect.OriginX.Value != 0) or (Effect.OriginY.Value != 0)) {
      Element.attribute("origin", length(Effect.OriginX) + ' ' + length(Effect.OriginY));
   }

   if (Effect.Angle != 0) Element.attribute("angle", Effect.Angle);
   if (Effect.Jitter != 1.0) Element.attribute("jitter", Effect.Jitter);

   static constexpr std::string_view modes[] = { "f1", "f2-f1", "f2", "weighted", "peaks", "cell" };
   if (Effect.WorleyMode != WLF::F1) Element.attribute("mode", modes[int(Effect.WorleyMode)]);

   static constexpr std::string_view metrics[] = { "euclidean", "manhattan", "chebyshev" };
   if (Effect.WorleyMetric != WLM::EUCLIDEAN) Element.attribute("metric", metrics[int(Effect.WorleyMetric)]);

   if (Effect.HeightMin != 0) Element.attribute("heightMin", Effect.HeightMin);
   if (Effect.HeightMax != 1.0) Element.attribute("heightMax", Effect.HeightMax);
   if (Effect.Octaves != 1) Element.attribute("numOctaves", Effect.Octaves);
   if (Effect.Persistence != 0.5) Element.attribute("persistence", Effect.Persistence);
   if (Effect.Lacunarity != 2.0) Element.attribute("lacunarity", Effect.Lacunarity);

   if ((Effect.PeriodX != 0) or (Effect.PeriodY != 0)) {
      if (Effect.PeriodX IS Effect.PeriodY) Element.attribute("period", Effect.PeriodX);
      else Element.attribute("period", Element.number(Effect.PeriodX) + ' ' + Element.number(Effect.PeriodY));
   }

   if (Effect.OutputMode IS FOM::HEIGHT) Element.attribute("xvg:outputMode", "height");

   return colour_ramp_xml(Effect, Element);
}

//********************************************************************************************************************

#include "filter_cellular_def.c"

static const FieldArray clCellularFXFields[] = {
   { "ColourMap",    FDF_CPPSTRING|FDF_RW, nullptr, CELLULARFX_SET_ColourMap },
   { "Stops",        FDF_VECTOR|FDF_STRUCT|FDF_RW, nullptr, CELLULARFX_SET_Stops, "GradientStop" },
   { "Seed",         FDF_INT64|FDF_RW, nullptr, CELLULARFX_SET_Seed },
   { "CellWidth",    FDF_UNIT|FDF_RW, nullptr, CELLULARFX_SET_CellWidth },
   { "CellHeight",   FDF_UNIT|FDF_RW, nullptr, CELLULARFX_SET_CellHeight },
   { "OriginX",      FDF_UNIT|FDF_RW, nullptr, CELLULARFX_SET_OriginX },
   { "OriginY",      FDF_UNIT|FDF_RW, nullptr, CELLULARFX_SET_OriginY },
   { "Angle",        FDF_DOUBLE|FDF_RW, nullptr, CELLULARFX_SET_Angle },
   { "Jitter",       FDF_DOUBLE|FDF_RW, nullptr, CELLULARFX_SET_Jitter },
   { "HeightMin",    FDF_DOUBLE|FDF_RW, nullptr, CELLULARFX_SET_HeightMin },
   { "HeightMax",    FDF_DOUBLE|FDF_RW, nullptr, CELLULARFX_SET_HeightMax },
   { "Persistence",  FDF_DOUBLE|FDF_RW, nullptr, CELLULARFX_SET_Persistence },
   { "Lacunarity",   FDF_DOUBLE|FDF_RW, nullptr, CELLULARFX_SET_Lacunarity },
   { "WorleyMode",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, CELLULARFX_SET_WorleyMode, &clCellularFXWLF },
   { "WorleyMetric", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, CELLULARFX_SET_WorleyMetric, &clCellularFXWLM },
   { "Octaves",      FDF_INT|FDF_RW, nullptr, CELLULARFX_SET_Octaves },
   { "PeriodX",      FDF_INT|FDF_RW, nullptr, CELLULARFX_SET_PeriodX },
   { "PeriodY",      FDF_INT|FDF_RW, nullptr, CELLULARFX_SET_PeriodY },
   { "OutputMode",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, CELLULARFX_SET_OutputMode, &clCellularFXFOM },
   { "XMLDef",       FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_cellularfx(void)
{
   clCellularFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::CELLULARFX),
      fl::Name("CellularFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clCellularFXActions),
      fl::Fields(clCellularFXFields),
      fl::Size(sizeof(extCellularFX)),
      fl::Path(MOD_PATH));

   return clCellularFX ? ERR::Okay : ERR::AddClass;
}
