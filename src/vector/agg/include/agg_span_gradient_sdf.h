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

// Exterior glow colour mode (see span_gradient_sdf below).  Uncomment to make the exterior walk the upper half of
// the colour ramp (edge -> last stop) while still fading in alpha.  Left commented, the exterior holds the outline
// colour (ramp midpoint) and fades only in alpha — the classic glow/halo look.

//#define SDF_GLOW_RAMP_COLOUR

// Exterior glow alpha falloff curve.  The exterior coverage 'a' runs 1.0 at the path outline to 0.0 at the padding
// edge; the selected curve reshapes how quickly the glow fades over that distance.  Define exactly one of the
// following (quadratic ease-out is the default).  A curve that drops faster near the outline (quadratic/cubic)
// produces a tighter, less bright halo; smoothstep holds the inner half bright and reads as a wider, brighter glow.
//
//   SDF_FALLOFF_LINEAR     : a            (even taper)
//   SDF_FALLOFF_QUADRATIC  : a^2          (fast drop, thin tail — natural glow, default)
//   SDF_FALLOFF_CUBIC      : a^3          (very tight glow hugging the outline)
//   SDF_FALLOFF_SMOOTHSTEP : 3a^2 - 2a^3  (S-curve; brightest, holds the inner half)

#if !defined(SDF_FALLOFF_LINEAR) and !defined(SDF_FALLOFF_QUADRATIC) and !defined(SDF_FALLOFF_CUBIC) and !defined(SDF_FALLOFF_SMOOTHSTEP)
   #define SDF_FALLOFF_SMOOTHSTEP
#endif

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
      std::vector<int8u> m_alpha; // Exterior alpha coverage: 255 at the outline, falling to 0 at the padding edge
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

      // Returns the raw signed-distance byte at a pixel coordinate (x,y are in gradient subpixel units, as passed
      // to calculate()).  127/128 is the outline (signed zero); < 128 is interior, > 128 is exterior.  Used by the
      // glow span generator to derive both the colour index and the distance-based alpha falloff.

      int8u raw(int x, int y) const {
         if (m_buffer.empty()) return 128;
         int px = (x >> agg::gradient_subpixel_shift) % m_width;
         int py = (y >> agg::gradient_subpixel_shift) % m_height;
         if (px < 0) px += m_width;
         if (py < 0) py += m_height;
         return m_buffer[(py * m_width) + px];
      }

      // Returns the LUT (gradient ramp index) value for a raw signed-distance byte.  Lets the glow span generator
      // map a specific distance to a colour index without going through a pixel coordinate (used to hold the
      // outline colour across the exterior).
      int lut_at(int Byte) const { return m_lut[Byte & 0xff]; }

      // Returns the exterior alpha coverage at a pixel coordinate (gradient subpixel units).  255 at the path
      // outline, falling linearly to 0 at the padding edge; the interior is always fully opaque (255).  This is
      // normalised against the padding distance independently of the colour LUT, so the glow always fades out
      // exactly at the coverage boundary regardless of the shape's interior extent.
      int8u alpha(int x, int y) const {
         if (m_alpha.empty()) return 255;
         int px = (x >> agg::gradient_subpixel_shift) % m_width;
         int py = (y >> agg::gradient_subpixel_shift) % m_height;
         if (px < 0) px += m_width;
         if (py < 0) py += m_height;
         return m_alpha[(py * m_width) + px];
      }

      bool empty() const { return m_buffer.empty(); }
   };

   // Exterior glow alpha falloff: reshapes the linear coverage 'a' (1.0 at the outline, 0.0 at the padding edge)
   // into the final alpha multiplier.

   static inline double sdf_falloff(double a) {
      if (a <= 0.0) return 0.0;
      if (a >= 1.0) return 1.0;
#if defined(SDF_FALLOFF_LINEAR)
         return a;
#elif defined(SDF_FALLOFF_CUBIC)
         return a * a * a;
#elif defined(SDF_FALLOFF_SMOOTHSTEP)
         return a * a * (3.0 - (2.0 * a));
#else // SDF_FALLOFF_QUADRATIC (default)
         return a * a;
#endif
   }

   // Span generator for SDF glow fills.  Unlike the stock span_gradient, it modulates per-pixel alpha by a
   // distance-based falloff so the exterior half of the field fades smoothly to transparent over the padding
   // distance, and contributes nothing once alpha reaches zero.  Two exterior colour modes are selectable at
   // compile time via SDF_GLOW_RAMP_COLOUR:
   //   - undefined (default): the exterior holds the outline colour (ramp midpoint), fading only in alpha.
   //   - defined: the exterior walks the upper half of the colour ramp and also fades in alpha.
   // The interior half is always rendered at full opacity through the colour ramp, exactly as a normal SDF fill.

   template<class ColorT, class Interpolator, class ColorF>
   class span_gradient_sdf {
   public:
      typedef Interpolator interpolator_type;
      typedef ColorT color_type;

      const int downscale_shift = interpolator_type::subpixel_shift - gradient_subpixel_shift;
      static const int reciprocal_shift = 24;

      span_gradient_sdf() : m_interpolator(nullptr), m_gradient(nullptr), m_color_function(nullptr), m_d1(0), m_d2(0) { }

      span_gradient_sdf(interpolator_type &Inter, const gradient_sdf &Gradient, const ColorF &ColorFunction,
         double D1, double D2) :
         m_interpolator(&Inter), m_gradient(&Gradient), m_color_function(&ColorFunction),
         m_d1(iround(D1 * gradient_subpixel_scale)), m_d2(iround(D2 * gradient_subpixel_scale)) { }

      void prepare() { }

      void generate(color_type *Span, int x, int y, unsigned Len) {
         int dd = m_d2 - m_d1;
         if (dd < 1) dd = 1;
         const int colour_count = int(m_color_function->size());
         const int64_t scale = (int64_t(colour_count) << reciprocal_shift) / dd;

         m_interpolator->begin(x + 0.5, y + 0.5, Len);
         do {
            int ix, iy;
            m_interpolator->coordinates(&ix, &iy);

            const int gx = ix >> downscale_shift;
            const int gy = iy >> downscale_shift;
            const int raw = m_gradient->raw(gx, gy);

            // Exterior alpha is read from the gradient's margin-normalised coverage buffer (255 at the outline,
            // 0 at the padding edge) and softened with a smoothstep curve.  Normalising against the padding
            // distance (rather than the colour LUT range) guarantees the glow reaches zero alpha exactly at the
            // coverage boundary, so there is no hard rectangular cut-off.

            const double alpha_mul = sdf_falloff(double(m_gradient->alpha(gx, gy)) / 255.0);

            if (alpha_mul <= 0.0) { // Past the glow edge: contribute nothing.
               *Span++ = color_type(0, 0, 0, 0);
            }
            else {
               // The colour index is derived through the gradient's own LUT (which applies d1/d2), then scaled into
               // the colour table exactly as the stock span_gradient does.  In outline-hold mode the exterior is
               // forced to the ramp midpoint by clamping the sampled distance to the outline value.

#ifdef SDF_GLOW_RAMP_COLOUR
                  const int lut_value = m_gradient->lut_at(raw); // Exterior walks the upper ramp half
#else
                  // Exterior holds the outline colour (ramp midpoint); interior uses its true distance.
                  const int lut_value = m_gradient->lut_at((raw > 128) ? 128 : raw);
#endif

               int d = int((int64_t(lut_value - m_d1) * scale) >> reciprocal_shift);
               if (d < 0) d = 0;
               if (d >= colour_count) d = colour_count - 1;

               color_type c = (*m_color_function)[d];
               c.a = int8u((int(c.a) * int(alpha_mul * 256.0)) >> 8);
               *Span++ = c;
            }

            ++(*m_interpolator);
         } while (--Len);
      }

   private:
      interpolator_type *m_interpolator;
      const gradient_sdf *m_gradient;
      const ColorF       *m_color_function;
      int m_d1;
      int m_d2;
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

      if (max_abs <= 0.0f) { m_buffer.clear(); m_alpha.clear(); }
      else {
         // Map signed_d in [-max_abs, +max_abs] linearly onto [0, 255].  signed_d == 0 (the outline) lands at
         // 127.5, so the colour ramp midpoint tracks the path edge regardless of shape.

         const float scale = 127.5f / max_abs;

         // Exterior alpha is normalised against the padding distance (margin), independently of the colour
         // mapping.  This guarantees the glow fades to fully transparent exactly at the padding edge no matter how
         // large the interior distances are; otherwise a shape with a deep interior would clip the exterior glow
         // before its alpha reached zero, leaving a hard rectangular edge at the coverage boundary.

         m_alpha.resize(width * height);
         const float inv_margin = (margin > 0) ? (1.0f / float(margin)) : 0.0f;

         for (int l=0, total=width * height; l < total; l++) {
            const float mag = sqrtf(image[l]);
            const float signed_d = inside[l] ? -mag : mag;
            int v = int(127.5f + (signed_d * scale));
            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            m_buffer[l] = int8u(v);

            if (inside[l]) m_alpha[l] = 255; // Interior is fully opaque
            else {
               float a = 1.0f - (mag * inv_margin); // 1 at the outline, 0 at the padding edge
               if (a < 0.0f) a = 0.0f;
               else if (a > 1.0f) a = 1.0f;
               m_alpha[l] = int8u(a * 255.0f);
            }
         }
      }

      m_width    = width;
      m_height   = height;
      m_origin_x = margin;
      m_origin_y = margin;

      return m_buffer.data();
   }
}
