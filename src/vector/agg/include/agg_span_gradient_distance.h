// For Anti-Grain Geometry - Version 2.4
// http://www.antigrain.org
//
// Contribution Created By:
//  Milan Marusinec alias Milano
//  milan@marusinec.sk
//  Copyright (c) 2007-2008
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Shared distance-transform helpers used by the contour and SDF gradient generators.  The 2D Euclidean distance
// transform is separable into 1D passes (Felzenszwalb), so both gradients reuse the same row pass plus a
// cache-blocked transpose to handle columns cheaply.

#pragma once

#include "agg_basics.h"

namespace agg
{
   constexpr double infinity = 1E20;

   static constexpr int square(int x) { return x * x; }

   // 1D distance transform by Pedro Felzenszwalb.  Operates in-place on a contiguous row; fq, spang and
   // spann are caller-provided scratch buffers of at least length, length + 1 and length entries.

   static void dt(float *Row, int Length, float *Fq, float *SpanG, int *SpanN)
   {
      for (int q=0; q < Length; q++) Fq[q] = Row[q] + float(square(q));

      SpanN[0] = 0;
      SpanG[0] = -float(infinity);
      SpanG[1] = +float(infinity);

      int k = 0;
      for (int q = 1; q <= Length - 1; q++) {
         float s;
         for (;;) {
            int p = SpanN[k];
            s = (Fq[q] - Fq[p]) / float(2 * (q - p));
            if (s > SpanG[k]) break;
            k--;
         }

         k++;
         SpanN[k] = q;
         SpanG[k] = s;
         SpanG[k + 1] = +float(infinity);
      }

      int j = 0;
      int p = SpanN[0];
      float fp = Fq[p] - float(square(p)); // Recovers the original row value at p
      for (int q = 0; q <= Length - 1; q++) {
         while (SpanG[j + 1] < q) {
            j++;
            p = SpanN[j];
            fp = Fq[p] - float(square(p));
         }
         Row[q] = float(square(q - p)) + fp;
      }
   }

   // Cache-blocked transpose of a src_width * src_height row-major image into dst (src_height * src_width).

   static void transpose_image(const float *Src, float *Dst, int SrcWidth, int SrcHeight)
   {
      constexpr int block_size = 16;
      for (int yb=0; yb < SrcHeight; yb += block_size) {
         const int ymax = (yb + block_size < SrcHeight) ? yb + block_size : SrcHeight;
         for (int xb=0; xb < SrcWidth; xb += block_size) {
            const int xmax = (xb + block_size < SrcWidth) ? xb + block_size : SrcWidth;
            for (int y=yb; y < ymax; y++) {
               for (int x=xb; x < xmax; x++) Dst[(x * SrcHeight) + y] = Src[(y * SrcWidth) + x];
            }
         }
      }
   }
}
