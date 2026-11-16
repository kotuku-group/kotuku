// Optional usage counters for the software blitting paths.  Configure with -DDISPLAY_BLIT_STATS=ON to enable them;
// the totals are printed to stderr when the Display module is expunged.  Use this to learn which CopyArea() and
// CopyRawBitmap() paths a real session exercises before optimising them.  When disabled, every macro compiles to
// nothing.

#pragma once

#ifdef DISPLAY_BLIT_STATS

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

enum class BLIT : int {
   DRIVER = 0,              // CopyArea() handled by the display driver
   BLEND32_COPY,            // BAF::BLEND|BAF::COPY, sRGB, 32-bit destination
   BLEND32,                 // BAF::BLEND, Opacity 255, sRGB
   BLEND32_OPACITY,         // BAF::BLEND, Opacity < 255, sRGB
   BLEND32_LINEAR,          // Any 32-bit BAF::BLEND with BAF::LINEAR
   BLEND32_NO_SIMD,         // sRGB 32-bit blends whose channel layouts differ (counted in addition to the above)
   BLEND32_TRANSLUCENT,     // Pixels of 4-pixel groups that ran the SIMD translucent-destination kernel
   BLEND_TO16,              // BAF::BLEND onto a 15/16-bit destination
   BLEND_TO_OTHER,          // BAF::BLEND onto 8/24-bit destinations
   KEY_OPACITY,             // Colour-keyed copy with Opacity < 255
   KEY_32,                  // Colour-keyed copy, 32-bit to 32-bit
   KEY_16,                  // Colour-keyed copy, 16-bit to 16-bit
   KEY_SAME_OTHER,          // Colour-keyed copy between other matching depths
   KEY_8,                   // Colour-keyed copy from an 8-bit source
   KEY_GENERIC,             // Colour-keyed copy between other mismatched depths
   OPACITY32,               // Constant-opacity copy, 32-bit to 32-bit
   OPACITY32_NO_SIMD,       // As above, channel layouts differ (counted in addition)
   OPACITY16,               // Constant-opacity copy, 16-bit to 16-bit
   OPACITY_GENERIC,         // Constant-opacity copy between other depths
   COPY_SAME,               // Straight copy between matching depths (memcpy)
   COPY_CONVERT,            // Straight copy between mismatched depths (per-pixel converters)
   COPY_DITHER,             // Straight copy with dithering
   RAW_BLEND32,             // CopyRawBitmap() CSRF::ALPHA onto 32-bit
   RAW_BLEND32_NO_SIMD,     // As above, channel layouts differ (counted in addition)
   RAW_BLEND_OTHER,         // CopyRawBitmap() CSRF::ALPHA onto other depths
   RAW_KEY,                 // CopyRawBitmap() CSRF::TRANSPARENT
   RAW_OPACITY32,           // CopyRawBitmap() constant opacity, 32-bit to 32-bit
   RAW_OPACITY_OTHER,       // CopyRawBitmap() constant opacity, other depths
   RAW_COPY_SAME,           // CopyRawBitmap() straight copy between matching depths
   RAW_COPY_CONVERT,        // CopyRawBitmap() straight copy between mismatched depths
   PREMULTIPLY,
   DEMULTIPLY,
   END
};

struct BlitCounter {
   std::atomic<uint64_t> Calls  = 0;
   std::atomic<uint64_t> Pixels = 0;
};

inline std::array<BlitCounter, int(BLIT::END)> glBlitStats;

// Mismatched-depth conversions, indexed by [source][destination] depth class (8, 15, 16, 24, 32, other).

inline std::array<std::array<std::atomic<uint64_t>, 6>, 6> glBlitConvert;

inline int blit_depth_index(int BitsPerPixel)
{
   switch (BitsPerPixel) {
      case 8: return 0;
      case 15: return 1;
      case 16: return 2;
      case 24: return 3;
      case 32: return 4;
      default: return 5;
   }
}

inline void blit_stat(BLIT Path, int64_t Pixels)
{
   auto &counter = glBlitStats[int(Path)];
   counter.Calls.fetch_add(1, std::memory_order_relaxed);
   counter.Pixels.fetch_add(uint64_t(Pixels), std::memory_order_relaxed);
}

inline void blit_stat_pixels(BLIT Path, int64_t Pixels)
{
   glBlitStats[int(Path)].Pixels.fetch_add(uint64_t(Pixels), std::memory_order_relaxed);
}

inline void blit_stat_convert(int SourceBPP, int DestBPP, int64_t Pixels)
{
   glBlitConvert[blit_depth_index(SourceBPP)][blit_depth_index(DestBPP)].fetch_add(uint64_t(Pixels),
      std::memory_order_relaxed);
}

inline void blit_stats_report()
{
   static bool reported = false; // The module can be expunged more than once during shutdown
   if (reported) return;
   reported = true;

   static constexpr const char *names[] = {
      "driver", "blend32_copy", "blend32", "blend32_opacity", "blend32_linear", "blend32_no_simd",
      "blend32_translucent_dest", "blend_to16", "blend_to_other", "key_opacity", "key_32", "key_16",
      "key_same_other", "key_8", "key_generic", "opacity32", "opacity32_no_simd", "opacity16", "opacity_generic",
      "copy_same", "copy_convert", "copy_dither", "raw_blend32", "raw_blend32_no_simd", "raw_blend_other", "raw_key",
      "raw_opacity32", "raw_opacity_other", "raw_copy_same", "raw_copy_convert", "premultiply", "demultiply"
   };
   static_assert(std::size(names) IS size_t(BLIT::END));

   static constexpr const char *depths[] = { "8", "15", "16", "24", "32", "other" };

   std::fprintf(stderr, "Display blit statistics (path, calls, pixels):\n");
   for (int i=0; i < int(BLIT::END); i++) {
      auto calls  = glBlitStats[i].Calls.load(std::memory_order_relaxed);
      auto pixels = glBlitStats[i].Pixels.load(std::memory_order_relaxed);
      if (calls or pixels) {
         std::fprintf(stderr, "  %-26s %12llu %16llu\n", names[i], (unsigned long long)calls,
            (unsigned long long)pixels);
      }
   }

   for (int s=0; s < 6; s++) {
      for (int d=0; d < 6; d++) {
         if (auto pixels = glBlitConvert[s][d].load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "  convert %s->%s bpp: %llu pixels\n", depths[s], depths[d],
               (unsigned long long)pixels);
         }
      }
   }
}

#define BLIT_STAT(PATH, PIXELS) blit_stat(BLIT::PATH, (PIXELS))
#define BLIT_STAT_PIXELS(PATH, PIXELS) blit_stat_pixels(BLIT::PATH, (PIXELS))
#define BLIT_STAT_CONVERT(SRC, DEST, PIXELS) blit_stat_convert((SRC), (DEST), (PIXELS))
#define BLIT_STATS_REPORT() blit_stats_report()

#else

#define BLIT_STAT(PATH, PIXELS)
#define BLIT_STAT_PIXELS(PATH, PIXELS)
#define BLIT_STAT_CONVERT(SRC, DEST, PIXELS)
#define BLIT_STATS_REPORT()

#endif
