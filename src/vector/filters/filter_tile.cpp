/*********************************************************************************************************************

-CLASS-
TileFX: A filter effect that repeats its input to fill the primitive subregion.

TileFX takes the primitive subregion of its @FilterEffect.Input as a tile and repeats it across its own primitive
subregion, as for the SVG `feTile` element.  The tile lattice is anchored at the top-left of the input's subregion,
so the tile appears at its original position and the copies extend from it in every direction.

The input's subregion is typically set with @FilterEffect.X, @FilterEffect.Y, @FilterEffect.Width and
@FilterEffect.Height on the producer, e.g. a @FloodFX or @OffsetFX limited to a small rectangle.  A standard source
such as `SourceGraphic` covers the filter region, so tiling it has no visible effect.

Unlike other effects, the default primitive subregion of a TileFX is the full filter region rather than the subregion
of its input, so that a TileFX without an explicit subregion fills the filter region.

The result has the same data kind as the input.  Colour images are copied without colour conversion, so a tiled
normal map produced by a @LightingFX of type `NORMAL` is still recognised as a normal map.  Scalar heights produced
in the `HEIGHT` output mode of @ImageFX, @TurbulenceFX or @BlurFX remain floating-point values, so they can be tiled
at full precision before they are lit.

If the tile covers a whole number of device pixels, every pixel of the output is an exact copy of a pixel in the tile.
Otherwise, for example when the client is scaled or rotated, the tile is resampled with bilinear interpolation.
Interpolation wraps across the edges of the tile, so the copies join without seams.  Repetition does not repair
discontinuities that are already present between opposite edges of the tile.

-END-

*********************************************************************************************************************/

class extTileFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TILEFX;
   static constexpr CSTRING CLASS_NAME = "TileFX";
   using create = kt::Create<extTileFX>;

   extTileFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }
};

//********************************************************************************************************************
// Up to four weighted source pixels that contribute to an output pixel.  A single tap is an exact copy.

struct tile_taps {
   std::array<int, 4> X, Y;
   std::array<double, 4> Weight;
   int Count;
};

// Fractional positions within this distance of a pixel centre are treated as exact, so that rounding in the
// coordinate transforms cannot turn a whole-pixel tile into an interpolated one.

constexpr double TILE_SNAP = 1e-6;

// Tiles beyond this coordinate magnitude in device pixels are not repeated.

constexpr double TILE_LIMIT = 0x1p40;

//********************************************************************************************************************
// Returns Value modulo Period in the range [0, Period).

static double tile_wrap(double Value, double Period)
{
   auto result = std::fmod(Value, Period);
   if (result < 0) result += Period;
   return (result < Period) ? result : 0.0;
}

//********************************************************************************************************************
// Returns a pixel index as an int, or -1 if it is out of range.  Valid surface indexes are never negative.

static int tile_index(int64_t Index)
{
   return ((Index >= 0) and (Index <= std::numeric_limits<int>::max())) ? int(Index) : -1;
}

//********************************************************************************************************************
// Computes the source taps for one axis of an axis-aligned transform.  Start is included and End is excluded in
// device space, even for a reflected axis.  Sample the original pixel grid and wrap taps across the tile edges.

struct tile_axis {
   std::vector<int> First, Second;
   std::vector<double> Weight;
};

static bool tile_axis_taps(double Start, double End, int From, int To, tile_axis &Axis)
{
   if ((not (std::abs(Start) < TILE_LIMIT)) or (not (std::abs(End) < TILE_LIMIT))) return false;
   const bool reflected = Start > End;
   const int64_t first = reflected ? int64_t(std::floor(End - 0.5)) + 1 : int64_t(std::ceil(Start - 0.5));
   const int64_t last = reflected ? int64_t(std::floor(Start - 0.5)) : int64_t(std::ceil(End - 0.5)) - 1;
   if (last < first) return false;

   const double direction = reflected ? -1.0 : 1.0;
   const double period = std::abs(End - Start);
   auto wrap = [&](double Position) {
      return Start + direction * tile_wrap((Position - Start) * direction, period);
   };
   auto wrapped_tap = [&](int64_t Index) {
      if ((Index >= first) and (Index <= last)) return tile_index(Index);
      const auto wrapped = int64_t(std::floor(wrap(double(Index) + 0.5)));
      return tile_index(std::clamp(wrapped, first, last));
   };

   const auto size = size_t(To - From);
   Axis.First.resize(size);
   Axis.Second.resize(size);
   Axis.Weight.resize(size);

   for (int i = From; i < To; i++) {
      const auto offset = i - From;
      if ((i >= first) and (i <= last)) {
         Axis.First[offset] = Axis.Second[offset] = i;
         Axis.Weight[offset] = 0;
         continue;
      }

      const double position = wrap(filter_pixel_centre(i)) - 0.5;
      auto index = int64_t(std::floor(position));
      double weight = position - double(index);
      if (weight < TILE_SNAP) weight = 0;
      else if (weight > 1.0 - TILE_SNAP) {
         weight = 0;
         index++;
      }

      Axis.First[offset] = wrapped_tap(index);
      Axis.Second[offset] = weight > 0 ? wrapped_tap(index + 1) : Axis.First[offset];
      Axis.Weight[offset] = weight;
   }

   return true;
}

//********************************************************************************************************************
// Writes interpolated RGBA8 pixels.  Colour is weighted by alpha so that transparent taps do not darken the result.

static void tile_rgba_pixel(const filter_surface_view &Source, const ColourFormat &Format, uint8_t *Dest,
   const tile_taps &Taps)
{
   auto read = [&](int X, int Y) -> const uint8_t * {
      if ((X < Source.Valid.left) or (X >= Source.Valid.right) or
          (Y < Source.Valid.top) or (Y >= Source.Valid.bottom)) return nullptr;
      return Source.row(Y) + std::ptrdiff_t(X) * 4;
   };

   if (Taps.Count IS 1) {
      if (auto pixel = read(Taps.X[0], Taps.Y[0]); pixel) copymem(pixel, Dest, 4);
      else std::memset(Dest, 0, 4);
      return;
   }

   const int alpha_index = Format.AlphaPos >> 3;
   double alpha = 0, colour[4] = { 0, 0, 0, 0 };
   for (int i = 0; i < Taps.Count; i++) {
      if (Taps.Weight[i] <= 0) continue;
      const auto pixel = read(Taps.X[i], Taps.Y[i]);
      if (not pixel) continue;
      const double weight = Taps.Weight[i] * pixel[alpha_index];
      alpha += weight;
      for (int c = 0; c < 4; c++) colour[c] += weight * pixel[c];
   }

   for (int c = 0; c < 4; c++) {
      if (c IS alpha_index) Dest[c] = uint8_t(std::clamp(std::lround(alpha), 0L, 255L));
      else Dest[c] = (alpha > 0) ? uint8_t(std::clamp(std::lround(colour[c] / alpha), 0L, 255L)) : 0;
   }
}

//********************************************************************************************************************
// Writes interpolated numerical samples.  Invalid taps are excluded and the remaining weights renormalised.  If no
// valid tap remains, the output keeps its invalid initial value.

static void tile_numeric_pixel(const filter_surface_view &Source, int Components, float *Dest, const tile_taps &Taps)
{
   auto read = [&](int X, int Y) -> const float * {
      if ((X < Source.Valid.left) or (X >= Source.Valid.right) or
          (Y < Source.Valid.top) or (Y >= Source.Valid.bottom)) return nullptr;
      return (const float *)Source.row(Y) + std::ptrdiff_t(X) * Components;
   };

   if (Taps.Count IS 1) {
      if (auto sample = read(Taps.X[0], Taps.Y[0]); sample) std::copy_n(sample, Components, Dest);
      return;
   }

   double total = 0, value[2] = { 0, 0 };
   for (int i = 0; i < Taps.Count; i++) {
      if (Taps.Weight[i] <= 0) continue;
      const auto sample = read(Taps.X[i], Taps.Y[i]);
      if (not sample) continue;
      bool finite = true;
      for (int c = 0; c < Components; c++) finite = finite and std::isfinite(sample[c]);
      if (not finite) continue;
      total += Taps.Weight[i];
      for (int c = 0; c < Components; c++) value[c] += Taps.Weight[i] * sample[c];
   }

   if (total > 0) {
      for (int c = 0; c < Components; c++) Dest[c] = float(value[c] / total);
   }
}

//********************************************************************************************************************
// Fills the output region with taps for each pixel.  Tiling preserves the input format, so the writer is selected by
// the format of the source.

template<class TapFunction>
static void tile_region(const filter_surface_view &Source, const TClipRectangle<int> &Region, uint8_t *Data,
   int Stride, const ColourFormat *Format, TapFunction &&GetTaps)
{
   tile_taps taps;
   const int components = (Source.Format IS filter_surface_format::RGBA8) ? 4 : filter_components(Source.Format);
   for (int y = Region.top; y < Region.bottom; y++) {
      auto row = Data + std::ptrdiff_t(y) * Stride;
      for (int x = Region.left; x < Region.right; x++) {
         GetTaps(x, y, taps);
         if (Format) tile_rgba_pixel(Source, *Format, row + std::ptrdiff_t(x) * 4, taps);
         else tile_numeric_pixel(Source, components, (float *)row + std::ptrdiff_t(x) * components, taps);
      }
   }
}

//********************************************************************************************************************

static ERR TILEFX_Draw(extTileFX *Self, struct acDraw *Args)
{
   const auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;
   const auto &render = *Self->Filter->Render;

   // The tile is the primitive subregion of the producer.  Standard sources cover the filter region.

   const auto &input = step->Inputs.front();
   const auto tile = input.Producer ? input.Producer->Subregion : render.FilterRegion;
   if ((not tile.valid()) or (not finite_region(tile))) return ERR::Okay;

   filter_surface_view input_view, source;
   if (auto error = get_source_view(Self, input_view, filter_input_role::INPUT); error != ERR::Okay) return error;
   if (input_view.Format != step->Format) return ERR::NoSupport;

   // The output is bound to the effect's subregion, which clip_to_subregion() applies precisely after drawing.

   uint8_t *data;
   int stride;
   TClipRectangle<int> valid;
   const ColourFormat *format = nullptr;

   if (input_view.Format IS filter_surface_format::RGBA8) {
      // Results are stored as straight sRGB.  Standard sources are converted if necessary.
      if (auto error = working_input(Self, input_view, CS::SRGB, filter_alpha::STRAIGHT, source);
          error != ERR::Okay) return error;
      const auto target = Self->Target;
      data = target->Data;
      stride = target->LineWidth;
      valid = { target->Clip.Left, target->Clip.Top, target->Clip.Right, target->Clip.Bottom };
      format = target->ColourFormat;
   }
   else {
      source = input_view;
      filter_surface_view layout;
      float *floats;
      if (auto error = get_numeric_target(Self, layout, floats); error != ERR::Okay) return error;
      data = (uint8_t *)floats;
      stride = layout.Stride;
      valid = layout.Valid;
   }

   auto bounds = step->Subregion;
   if (not transform_filter_region(bounds, render.Transform)) return ERR::Okay;
   const TClipRectangle<int> region = {
      int(std::clamp(std::floor(bounds.left), double(valid.left), double(valid.right))),
      int(std::clamp(std::floor(bounds.top), double(valid.top), double(valid.bottom))),
      int(std::clamp(std::ceil(bounds.right), double(valid.left), double(valid.right))),
      int(std::clamp(std::ceil(bounds.bottom), double(valid.top), double(valid.bottom)))
   };
   if (not region.valid()) return ERR::Okay;

   const auto &transform = render.Transform;
   const bool axis_aligned = (std::abs(transform.shx) < 1e-9) and (std::abs(transform.shy) < 1e-9);

   if (axis_aligned) {
      // Each axis is independent, so the taps are computed once per column and once per row.

      tile_axis columns, rows;
      if (not tile_axis_taps(transform.sx * tile.left + transform.tx, transform.sx * tile.right + transform.tx,
          region.left, region.right, columns)) return ERR::Okay;
      if (not tile_axis_taps(transform.sy * tile.top + transform.ty, transform.sy * tile.bottom + transform.ty,
          region.top, region.bottom, rows)) return ERR::Okay;

      tile_region(source, region, data, stride, format, [&](int X, int Y, tile_taps &Taps) {
         const auto cx = size_t(X - region.left), cy = size_t(Y - region.top);
         const double wx = columns.Weight[cx], wy = rows.Weight[cy];
         const int x0 = columns.First[cx], x1 = columns.Second[cx];
         const int y0 = rows.First[cy], y1 = rows.Second[cy];
         if ((wx IS 0.0) and (wy IS 0.0)) {
            Taps.X[0] = x0;
            Taps.Y[0] = y0;
            Taps.Weight[0] = 1.0;
            Taps.Count = 1;
         }
         else {
            Taps.X = { x0, x1, x0, x1 };
            Taps.Y = { y0, y0, y1, y1 };
            Taps.Weight = { (1.0 - wx) * (1.0 - wy), wx * (1.0 - wy), (1.0 - wx) * wy, wx * wy };
            Taps.Count = 4;
         }
      });
      return ERR::Okay;
   }

   // Rotated and skewed clients.  Each output pixel centre is wrapped into the tile in user space and sampled
   // bilinearly.  A tap whose centre lies outside of the tile is wrapped in the same way and read from the nearest
   // pixel, so that interpolation continues across the tile edge.

   const double tile_width = tile.width(), tile_height = tile.height();
   auto wrap = [&](double &X, double &Y) {
      X = tile.left + tile_wrap(X - tile.left, tile_width);
      Y = tile.top + tile_wrap(Y - tile.top, tile_height);
   };

   auto inside_tile = [&](int64_t X, int64_t Y) {
      double ux = double(X) + 0.5, uy = double(Y) + 0.5;
      render.Inverse.transform(&ux, &uy);
      return tile.hit_test(ux, uy);
   };

   // The tile's pixels are those whose centres lie inside it, which under rotation is not the pixel containing every
   // point of the tile.  A wrapped tap reads the nearest pixel with its centre in the tile.

   auto wrapped_pixel = [&](int64_t X, int64_t Y, int &PX, int &PY) {
      if (not inside_tile(X, Y)) {
         double ux = double(X) + 0.5, uy = double(Y) + 0.5;
         render.Inverse.transform(&ux, &uy);
         wrap(ux, uy);
         transform.transform(&ux, &uy);
         const auto cx = int64_t(std::floor(ux)), cy = int64_t(std::floor(uy));
         X = cx;
         Y = cy;
         double nearest = std::numeric_limits<double>::max();
         for (int64_t ny = cy - 1; ny <= cy + 1; ny++) {
            for (int64_t nx = cx - 1; nx <= cx + 1; nx++) {
               const double ex = double(nx) + 0.5 - ux, ey = double(ny) + 0.5 - uy;
               const double distance = (ex * ex) + (ey * ey);
               if ((distance < nearest) and inside_tile(nx, ny)) {
                  nearest = distance;
                  X = nx;
                  Y = ny;
               }
            }
         }
      }
      PX = tile_index(X);
      PY = tile_index(Y);
   };

   tile_region(source, region, data, stride, format, [&](int X, int Y, tile_taps &Taps) {
      double ux, uy;
      map_filter_pixel_centre(render.Inverse, X, Y, ux, uy);
      wrap(ux, uy);
      transform.transform(&ux, &uy);
      if ((not (std::abs(ux) < TILE_LIMIT)) or (not (std::abs(uy) < TILE_LIMIT))) {
         Taps.X[0] = Taps.Y[0] = -1;
         Taps.Weight[0] = 1.0;
         Taps.Count = 1;
         return;
      }

      const double sx = ux - 0.5, sy = uy - 0.5;
      const auto ix = int64_t(std::floor(sx)), iy = int64_t(std::floor(sy));
      double wx = sx - double(ix), wy = sy - double(iy);
      if (wx < TILE_SNAP) wx = 0;
      if (wy < TILE_SNAP) wy = 0;

      if ((wx IS 0.0) and (wy IS 0.0)) {
         wrapped_pixel(ix, iy, Taps.X[0], Taps.Y[0]);
         Taps.Weight[0] = 1.0;
         Taps.Count = 1;
         return;
      }

      for (int i = 0; i < 4; i++) wrapped_pixel(ix + (i & 1), iy + (i >> 1), Taps.X[i], Taps.Y[i]);
      Taps.Weight = { (1.0 - wx) * (1.0 - wy), wx * (1.0 - wy), (1.0 - wx) * wy, wx * wy };
      Taps.Count = 4;
   });

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/



//********************************************************************************************************************

#include "filter_tile_def.c"

static const FieldArray clTileFXFields[] = {
   { "XMLDef", FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_tilefx(void)
{
   clTileFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::TILEFX),
      fl::Name("TileFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clTileFXActions),
      fl::Fields(clTileFXFields),
      fl::Size(sizeof(extTileFX)),
      fl::Path(MOD_PATH));

   return clTileFX ? ERR::Okay : ERR::AddClass;
}
