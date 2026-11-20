#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <kotuku/modules/vector.h>

// Continuous bitmap coordinates put the centre of pixel i at i + 0.5.  Interpolation below uses sample indices,
// where the centre of pixel i is i.

inline double filter_pixel_centre(int Index) { return double(Index) + 0.5; }

inline int filter_edge_index(int64_t Index, int Start, int End, FEM Mode)
{
   if ((Index >= Start) and (Index < End)) return int(Index);
   if ((Mode IS FEM::TRANSPARENT) or (Start >= End)) return -1;
   if (Mode IS FEM::CLAMP) return (Index < Start) ? Start : End - 1;

   const int64_t length = int64_t(End) - Start;
   const int64_t period = (Mode IS FEM::MIRROR) ? 2 * length : length;
   auto offset = ((Index % period) - Start) % period;
   if (offset < 0) offset += period;
   if (offset >= length) offset = period - 1 - offset;
   return int(Start + offset);
}

enum class filter_interpolation { NEAREST, BILINEAR };

struct filter_rgba_sampler {
   const uint8_t *Data;
   int Stride;
   // Half-open valid pixel indices, relative to Data.
   int Left, Top, Right, Bottom;

   const uint8_t * pixel(int64_t X, int64_t Y, FEM Edge = FEM::TRANSPARENT) const {
      const int x = filter_edge_index(X, Left, Right, Edge);
      const int y = filter_edge_index(Y, Top, Bottom, Edge);
      if ((x < 0) or (y < 0)) return nullptr;
      return Data + std::ptrdiff_t(y) * Stride + std::ptrdiff_t(x) * 4;
   }

   // Interpolate the stored channels in their existing colour and alpha representations.
   template<filter_interpolation Interpolation, FEM Edge = FEM::TRANSPARENT>
   void sample(uint8_t *Output, double X, double Y) const {
      constexpr double LIMIT = 0x1p52; // Keep the adjacent sample index representable.
      if (not ((X > -LIMIT) and (X < LIMIT) and (Y > -LIMIT) and (Y < LIMIT))) {
         std::fill_n(Output, 4, uint8_t(0));
         return;
      }

      if constexpr (Interpolation IS filter_interpolation::NEAREST) {
         auto p = pixel(int64_t(std::nearbyint(X)), int64_t(std::nearbyint(Y)), Edge);
         if (p) std::copy_n(p, 4, Output);
         else std::fill_n(Output, 4, uint8_t(0));
      }
      else {
         const int64_t x0 = int64_t(std::floor(X)), y0 = int64_t(std::floor(Y));
         const double tx = X - double(x0), ty = Y - double(y0);
         const auto p00 = pixel(x0, y0, Edge), p10 = pixel(x0 + 1, y0, Edge);
         const auto p01 = pixel(x0, y0 + 1, Edge), p11 = pixel(x0 + 1, y0 + 1, Edge);

         for (int i=0; i < 4; i++) {
            const double top = ((p00 ? p00[i] : 0) * (1.0 - tx)) + ((p10 ? p10[i] : 0) * tx);
            const double bottom = ((p01 ? p01[i] : 0) * (1.0 - tx)) + ((p11 ? p11[i] : 0) * tx);
            Output[i] = uint8_t(std::lrint((top * (1.0 - ty)) + (bottom * ty)));
         }
      }
   }
};
