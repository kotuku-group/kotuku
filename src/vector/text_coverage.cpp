// Solid direct text drawing reuses AGG coverage.  Other fills and unsupported placements retain the outline path.

#include "text_coverage_cache.h"

static constexpr double glTextOriginLimit = 0x1000000;

static std::shared_ptr<const text_coverage> get_text_coverage(freetype_font::ft_point *Font,
   freetype_font::glyph &Glyph, int Phase)
{
   const text_coverage_cache::key key { Font, Glyph.glyph_index, Phase };
   if (auto mask = glTextCoverageCache.find(key)) return mask;
   if (Glyph.path.empty()) return glTextCoverageCache.insert(key, {});

   // Reject oversized masks before allocating rasteriser cells or pixel storage.  The original path can draw
   // these glyphs without admitting them to the cache.
   const auto bounds = get_bounds(Glyph.path);
   if (std::max({ std::abs(bounds.left), std::abs(bounds.right), std::abs(bounds.top),
         std::abs(bounds.bottom) }) > glTextOriginLimit) return {};
   const double phase = Phase * 0.25;
   const double bound_width = std::ceil(bounds.right + phase) - std::floor(bounds.left + phase) + 1;
   const double bound_height = std::ceil(bounds.bottom) - std::floor(bounds.top) + 1;
   if ((bound_width <= 0) or (bound_height <= 0) or
       (bound_width * bound_height > text_coverage_cache::BYTE_LIMIT)) return {};

   thread_local agg::rasterizer_scanline_aa<> raster;
   raster.reset();
   // AGG rounds exact half-subpixel ties away from zero.  Rasterising at a positive origin matches the bitmap
   // renderer's rounding; rasterising above a zero baseline would round some negative vertices differently.
   const int shift_x = 1 - int(std::floor(bounds.left + phase));
   const int shift_y = 1 - int(std::floor(bounds.top));
   agg::trans_affine_translation transform(phase + shift_x, shift_y);
   agg::conv_transform<agg::path_storage, agg::trans_affine> path(Glyph.path, transform);
   raster.add_path(path);
   if (not raster.rewind_scanlines()) return glTextCoverageCache.insert(key, {});

   const int left = raster.min_x();
   const int top = raster.min_y();
   const int width = raster.max_x() - left + 1;
   const int height = raster.max_y() - top + 1;
   if ((width <= 0) or (height <= 0) or
       (uint64_t(width) * height > text_coverage_cache::BYTE_LIMIT)) return {};

   std::vector<uint8_t> pixels(size_t(width) * height, 0);
   agg::scanline_u8 scanline;
   scanline.reset(raster.min_x(), raster.max_x());
   while (raster.sweep_scanline(scanline)) {
      auto span = scanline.begin();
      for (unsigned count = scanline.num_spans(); count; count--, ++span) {
         auto output = pixels.data() + size_t(scanline.y() - top) * width + span->x - left;
         if (span->len > 0) std::copy_n(span->covers, span->len, output);
         else std::fill_n(output, -span->len, *span->covers);
      }
   }

   // Store exact coverage bytes rather than blending white into grey, which would round edge coverage twice.
   // Trim zero borders so neighbouring, disjoint glyphs do not appear to overlap at an integer edge.
   int x1 = width, y1 = height, x2 = 0, y2 = 0;
   for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
         if (pixels[size_t(y) * width + x]) {
            x1 = std::min(x1, x);
            y1 = std::min(y1, y);
            x2 = std::max(x2, x + 1);
            y2 = std::max(y2, y + 1);
         }
      }
   }
   text_coverage mask;
   if ((x2 > x1) and (y2 > y1)) {
      mask.left = left + x1 - shift_x;
      mask.top = top + y1 - shift_y;
      mask.width = x2 - x1;
      mask.height = y2 - y1;
      mask.source_left = int(std::floor(bounds.left + phase));
      mask.source_top = int(std::floor(bounds.top));
      mask.pixels.resize(size_t(mask.width) * mask.height);
      for (int y = 0; y < mask.height; y++) {
         std::copy_n(pixels.data() + size_t(y + y1) * width + x1, mask.width,
            mask.pixels.data() + size_t(y) * mask.width);
      }
   }
   return glTextCoverageCache.insert(key, std::move(mask));
}

struct covered_text_glyph {
   std::shared_ptr<const text_coverage> mask;
   int x;
   int y;
};

static bool draw_covered_text(objBitmap *Bitmap, freetype_font::ft_point *Font, double X, double Y,
   const std::string_view &String, const objVectorColour *Colour, double &Advance)
{
   // Cache exact quarter-pixel X phases and whole-pixel baselines.  Preserve arbitrary fractional origins through
   // the original renderer instead of changing the public API's placement precision.
   if ((std::abs(Y) > glTextOriginLimit) or (Y != std::floor(Y))) return false;

   thread_local std::vector<covered_text_glyph> run;
   run.clear();
   auto release_masks = kt::Defer([&]() { run.clear(); });
   double pen_x = 0;
   double pen_y = 0;
   int right_edge = -0x7fffffff;
   {
      const std::lock_guard lock(glFontMutex);
      FT_Activate_Size(Font->ft_size);
      int previous = 0;
      for (size_t i = 0; i < String.size();) {
         uint32_t unicode;
         i += get_utf8(String, unicode, i);
         auto &glyph = Font->get_glyph(unicode);
         double kx, ky;
         get_kerning_xy(Font->font->face, glyph.glyph_index, previous, kx, ky);
         pen_x += kx;
         const double origin_x = X + pen_x;
         const double origin_y = Y + pen_y;
         if ((std::abs(origin_x) > glTextOriginLimit) or (std::abs(origin_y) > glTextOriginLimit) or
             (origin_x * 4 != std::round(origin_x * 4)) or (origin_y != std::floor(origin_y))) return false;
         const int whole_x = int(std::floor(origin_x));
         const int phase = int((origin_x - whole_x) * 4);
         auto mask = get_text_coverage(Font, glyph, phase);
         if (not mask) return false;
         if (mask->width and mask->height) {
            if ((whole_x + mask->source_left < 0) or (int(origin_y) + mask->source_top < 0)) return false;
            const int x = whole_x + mask->left;
            const int y = int(origin_y) + mask->top;
            const int right = x + mask->width;
            const int bottom = y + mask->height;
            if (x < right_edge) {
               // Separate alpha blends are not equivalent to a single outline union where glyphs overlap.
               // Check actual coverage in intersecting boxes, including combining marks and negative kerning.
               for (const auto &prior : run) {
                  const int x1 = std::max(x, prior.x);
                  const int x2 = std::min(right, prior.x + prior.mask->width);
                  const int y1 = std::max(y, prior.y);
                  const int y2 = std::min(bottom, prior.y + prior.mask->height);
                  if ((x1 >= x2) or (y1 >= y2)) continue;
                  for (int row = y1; row < y2; row++) {
                     const auto current = mask->pixels.data() + size_t(row - y) * mask->width;
                     const auto previous = prior.mask->pixels.data() + size_t(row - prior.y) * prior.mask->width;
                     for (int column = x1; column < x2; column++) {
                        if (current[column - x] and previous[column - prior.x]) return false;
                     }
                  }
               }
            }
            right_edge = std::max(right_edge, right);
            run.push_back({ std::move(mask), x, y });
         }
         pen_x += glyph.adv_x;
         pen_y += glyph.adv_y + ky;
         previous = glyph.glyph_index;
      }
   }

   Advance = pen_x;
   if (run.empty() or (Bitmap->Clip.Right <= Bitmap->Clip.Left) or
       (Bitmap->Clip.Bottom <= Bitmap->Clip.Top)) return true;
   agg::pixfmt_psl format;
   format.setBitmap(*Bitmap);
   agg::renderer_base<agg::pixfmt_psl> renderer(format);
   if (not renderer.clip_box(Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right - 1,
         Bitmap->Clip.Bottom - 1)) return true;
   const agg::rgba8 colour(agg::rgba(Colour->Red, Colour->Green, Colour->Blue, Colour->Alpha));
   for (const auto &glyph : run) {
      const auto &mask = *glyph.mask;
      const int first = std::max(0, renderer.ymin() - glyph.y);
      const int last = std::min(mask.height, renderer.ymax() - glyph.y + 1);
      for (int y = first; y < last; y++) {
         const auto covers = mask.pixels.data() + size_t(y) * mask.width;
         for (int x = 0; x < mask.width;) {
            while ((x < mask.width) and (not covers[x])) x++;
            const int start = x;
            while ((x < mask.width) and covers[x]) x++;
            if (x > start) renderer.blend_solid_hspan(glyph.x + start, glyph.y + y, x - start,
               colour, covers + start);
         }
      }
   }
   return true;
}
