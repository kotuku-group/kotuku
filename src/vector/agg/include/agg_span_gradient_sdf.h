// For Anti-Grain Geometry - Version 2.4
// http://www.antigrain.org
//
// Derived from the contour-distance gradient contributed by Milan Marusinec.
// Copyright (c) 2007-2008
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Computes signed distance field values for SDF-style gradients.  Where the contour gradient maps the unsigned
// distance to the path outline, this gradient is signed: the outline is pinned to the centre of the colour ramp,
// the interior shades through one half of the ramp and the exterior through the other.  This is the natural
// primitive for glow, halo, inner-shadow, bevel and outline-band effects.

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
#include "agg_span_gradient_distance.h"

namespace agg
{
   class gradient_sdf {
   private:
      std::vector<int8u> m_buffer;
      int m_lut[256]; // Maps signed DT greyscale values directly to gradient results
      int m_width, m_height;
      int m_origin_x, m_origin_y; // Pixel offset of the path bounds within the padded buffer
      double m_padding; // Margin (in path units) added around the bounds so the exterior ramp half is visible
      double m_d1; // Ranges from 0 to 254
      double m_d2; // Ranges from 0.001 to 1.0

      void build_lut() {
         for (int i=0; i < 256; i++) m_lut[i] = iround((i * m_d2) + m_d1) << gradient_subpixel_shift;
      }

   public:
      gradient_sdf() : m_width(0), m_height(0), m_origin_x(0), m_origin_y(0), m_padding(0), m_d1(0), m_d2(1.0) {
         build_lut();
      }

      gradient_sdf(double d1, double d2) : m_width(0), m_height(0), m_origin_x(0), m_origin_y(0), m_padding(0), m_d2(d2) {
         m_d1 = (d1 > 254) ? 254 : d1;
         build_lut();
      }

      int8u* sdf_create(path_storage &ps);

      int    sdf_width() { return m_width; }
      int    sdf_height() { return m_height; }
      int    sdf_origin_x() { return m_origin_x; }
      int    sdf_origin_y() { return m_origin_y; }

      void   padding(double Value) { m_padding = (Value < 0) ? 0 : Value; }
      double padding() const { return m_padding; }

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

   int8u * gradient_sdf::sdf_create(path_storage &ps) {
      // Flatten the curves once so that bounding rect computation and both rasterisation passes do not
      // each repeat the curve subdivision.

      agg::conv_curve<agg::path_storage> conv(ps);
      agg::path_storage flat;
      flat.concat_path(conv, 0);

      double x1, y1, x2, y2;
      if (!agg::bounding_rect_single(flat, 0, &x1, &y1, &x2, &y2)) return nullptr;

      // Pad the bounding box so the exterior distance has room to grow.  Without a margin the "outside" ramp
      // half collapses, because the unpadded contour buffer is clipped to the path bounds.

      const double bound_w = x2 - x1;
      const double bound_h = y2 - y1;
      double pad = m_padding;
      if (pad <= 0) pad = 0.25 * ((bound_w > bound_h) ? bound_w : bound_h); // Default: 25% of the larger bound

      const int margin = int(ceil(pad));

      const auto width  = int(ceil(bound_w)) + 1 + (margin * 2);
      const auto height = int(ceil(bound_h)) + 1 + (margin * 2);
      m_buffer.resize(width * height);
      std::fill(m_buffer.begin(), m_buffer.end(), 255);

      agg::rendering_buffer rb(m_buffer.data(), width, height, width);

      agg::pixfmt_gray8 pf(rb);
      agg::renderer_base<agg::pixfmt_gray8> renb(pf);
      agg::renderer_primitives<agg::renderer_base<agg::pixfmt_gray8>> prim(renb);
      agg::rasterizer_outline<renderer_primitives<agg::renderer_base<agg::pixfmt_gray8>>> ras(prim);

      // Translate the path into the padded buffer, leaving a margin of empty space on all sides.

      agg::trans_affine mtx;
      mtx.translate(-x1 + margin, -y1 + margin);

      agg::conv_transform<agg::path_storage> trans(flat, mtx);

      // Render a filled version of the path to create the interior mask, marked with colour index 0x01.
      // Pixels left at 0xff are exterior.

      agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_gray8>> solid(renb);
      agg::rasterizer_scanline_aa<> rasterizer;
      agg::scanline32_p8 sl;
      rasterizer.reset();
      rasterizer.add_path(trans);
      solid.color(agg::gray8(0x01, 0xff));
      agg::render_scanlines(rasterizer, sl, solid);

      // Capture the interior/exterior flag per pixel before the outline overwrites edge pixels with 0x00.
      // A pixel is interior when its mask value is <= 0x01 (filled interior or stroked outline).

      std::vector<uint8_t> inside(width * height);
      for (int l=0, total=width * height; l < total; l++) inside[l] = (m_buffer[l] <= 0x01) ? 1 : 0;

      // Render the path as a stroke with colour index 0x00 to seed the distance transform along the outline.

      prim.line_color(agg::gray8(0x00, 0xff));
      ras.add_path(trans);

      // Distance Transform
      // Create float buffer; 0 = point-of-interest (outline), Infinity = undefined.

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

      // Apply the sign from the interior/exterior mask: inside negative, outside positive.  image[] holds
      // squared distances, so the signed magnitude is sign * sqrt(image).  Track the extreme magnitude in
      // each direction to build a symmetric normalisation that pins the outline (signed 0) to the centre.

      float max_inside = 0.0f;  // Largest |d| found inside the shape
      float max_outside = 0.0f; // Largest |d| found outside the shape

      for (int l=0, total=width * height; l < total; l++) {
         const float mag = sqrtf(image[l]);
         if (inside[l]) { if (mag > max_inside) max_inside = mag; }
         else { if (mag > max_outside) max_outside = mag; }
      }

      const float max_abs = (max_inside > max_outside) ? max_inside : max_outside;

      if (max_abs <= 0.0f) m_buffer.clear();
      else {
         // Map signed_d in [-max_abs, +max_abs] linearly onto [0, 255].  signed_d == 0 (the outline) lands at
         // 127.5, so the colour ramp midpoint tracks the path edge regardless of shape.

         const float scale = 127.5f / max_abs;
         for (int l=0, total=width * height; l < total; l++) {
            const float mag = sqrtf(image[l]);
            const float signed_d = inside[l] ? -mag : mag;
            int v = int(127.5f + (signed_d * scale));
            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            m_buffer[l] = int8u(v);
         }
      }

      m_width    = width;
      m_height   = height;
      m_origin_x = margin;
      m_origin_y = margin;

      return m_buffer.data();
   }
}
