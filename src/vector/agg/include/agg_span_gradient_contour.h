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

#pragma once

#include "agg_basics.h"
#include "agg_trans_affine.h"
#include "agg_path_storage.h"
#include "agg_pixfmt_gray.h"
#include "agg_conv_transform.h"
#include "agg_conv_curve.h"
#include "agg_bounding_rect.h"
#include "agg_renderer_base.h"
#include "agg_renderer_primitives.h"
#include "agg_rasterizer_outline.h"
#include "agg_span_gradient.h"

constexpr double infinity = 1E20;

namespace agg
{
   class gradient_contour {
   private:
      std::vector<int8u> m_buffer;
      int m_lut[256]; // Maps DT greyscale values directly to gradient results
      int m_width, m_height;
      double m_d1; // Ranges from 0 to 254
      double m_d2; // Ranges from 0.001 to 1.0

      void build_lut() {
         for (int i=0; i < 256; i++) m_lut[i] = iround((i * m_d2) + m_d1) << gradient_subpixel_shift;
      }

   public:
      gradient_contour() : m_width(0), m_height(0), m_d1(0), m_d2(1.0) { build_lut(); }
      gradient_contour(double d1, double d2) : m_width(0), m_height(0), m_d2(d2) {
         m_d1 = (d1 > 254) ? 254 : d1;
         build_lut();
      }

      int8u* contour_create(path_storage &ps);

      int    contour_width() { return m_width; }
      int    contour_height() { return m_height; }

      void   d1(double d) { m_d1 = d; build_lut(); }
      void   d2(double d) { m_d2 = d; build_lut(); }

      int calculate(int x, int y, int d) const {
         if (!m_buffer.empty()) {
            int px = (x >> agg::gradient_subpixel_shift) % m_width;
            int py = (y >> agg::gradient_subpixel_shift) % m_height;

            if (px < 0) px += m_width;
            if (py < 0) py += m_height;
            return m_lut[m_buffer[(py * m_width) + px]];
         }
         else return 0;
      }
   };

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

   int8u * gradient_contour::contour_create(path_storage &ps) {
      // Flatten the curves once so that bounding rect computation and both rasterisation passes do not
      // each repeat the curve subdivision.

      agg::conv_curve<agg::path_storage> conv(ps);
      agg::path_storage flat;
      flat.concat_path(conv, 0);

      double x1, y1, x2, y2;
      if (!agg::bounding_rect_single(flat, 0, &x1, &y1, &x2, &y2)) return nullptr;

      const auto width  = int(ceil(x2 - x1)) + 1;
      const auto height = int(ceil(y2 - y1)) + 1;
      m_buffer.resize(width * height);
      std::fill(m_buffer.begin(), m_buffer.end(), 255);

      agg::rendering_buffer rb(m_buffer.data(), width, height, width);

      agg::pixfmt_gray8 pf(rb);
      agg::renderer_base<agg::pixfmt_gray8> renb(pf);
      agg::renderer_primitives<agg::renderer_base<agg::pixfmt_gray8>> prim(renb);
      agg::rasterizer_outline<renderer_primitives<agg::renderer_base<agg::pixfmt_gray8>>> ras(prim);

      agg::trans_affine mtx;
      mtx.translate(-x1, -y1);

      agg::conv_transform<agg::path_storage> trans(flat, mtx);

      // Render a filled version of the path to create a mask defined by 0x01

      agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_gray8>> solid(renb);
      agg::rasterizer_scanline_aa<> rasterizer;
      agg::scanline32_p8 sl;
      rasterizer.reset();
      rasterizer.add_path(trans);
      solid.color(agg::gray8(0x01, 0xff));
      agg::render_scanlines(rasterizer, sl, solid);

      // Render the path as a stroke with colour index 0x00

      prim.line_color(agg::gray8(0x00, 0xff));
      ras.add_path(trans);

      // Distance Transform
      // Create float buffer; 0 = POI, Infinity = Undefined

      std::vector<float> image(width * height);

      for (int l=0, total=width * height; l < total; l++) {
         image[l] = (m_buffer[l] IS 0) ? 0.0f : float(infinity);
      }

      // The 2D transform is separable into 1D passes.  Each pass runs on contiguous rows; columns are
      // handled by transposing, transforming and transposing back, which is far cheaper than strided access.

      int length = width;
      if (height > length) length = height;

      std::vector<float> fq(length), spang(length + 1);
      std::vector<int> spann(length);

      // Transform along columns (rows of the transposed image)

      std::vector<float> transposed(width * height);
      transpose_image(image.data(), transposed.data(), width, height);

      for (int x=0; x < width; x++) dt(transposed.data() + (x * height), height, fq.data(), spang.data(), spann.data());

      transpose_image(transposed.data(), image.data(), height, width);

      // Transform along rows

      for (int y=0; y < height; y++) dt(image.data() + (y * width), width, fq.data(), spang.data(), spann.data());

      // Find min & max of the squared distances; sqrt() is monotonic so the order is preserved and only
      // the two extremes need their root taken.  Pixel values only contribute to min/max if unmasked.

      float min_sq = std::numeric_limits<float>::max();
      float max_sq = std::numeric_limits<float>::min();

      for (int l=0, total=width * height; l < total; l++) {
         if (m_buffer[l] <= 0x01) {
            if (min_sq > image[l]) min_sq = image[l];
            if (max_sq < image[l]) max_sq = image[l];
         }
      }

      // Convert to greyscale in a single fused sqrt + scale pass.  Masked (OOB) pixels get the same
      // treatment; ideally they would be zero but anti-aliasing at the edges doesn't like that.

      if (min_sq IS max_sq) m_buffer.clear();
      else {
         const float min = sqrtf(min_sq);
         const float max = sqrtf(max_sq);
         const float scale = 255.0f / (max - min);
         for (int l=0, total=width * height; l < total; l++) {
            m_buffer[l] = int8u(int((sqrtf(image[l]) - min) * scale));
         }
      }

      m_width  = width;
      m_height = height;

      return m_buffer.data();
   }
}
