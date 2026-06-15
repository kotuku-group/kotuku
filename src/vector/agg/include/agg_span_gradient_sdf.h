// Copyright (C) 2026 Paul Manias
//
// Permission to copy, use, modify, sell and distribute this software is granted provided this copyright notice
// appears in all copies.  This software is provided "as is" without express or implied warranty, and with no claim
// as to its suitability for any purpose.
// ---
// Computes signed distance field values for SDF-style gradients.  Where the contour gradient maps the unsigned
// distance to the path outline, this gradient is signed: the outline is pinned to the centre of the colour ramp,
// the interior shades through one half of the ramp and the exterior through the other.  This is the natural
// primitive for glow, halo, inner-shadow, bevel and outline-band effects.

#pragma once

// Exterior glow colour mode (see span_gradient_sdf below).  Uncomment to make the exterior walk the upper half of
// the colour ramp (edge -> last stop) while still fading in alpha.  Left commented, the exterior holds the outline
// colour (ramp midpoint) and fades only in alpha — the classic glow/halo look.

#define SDF_GLOW_RAMP_COLOUR

// Alpha fall-off curve.  The coverage 'a' runs 1.0 at the path outline to 0.0 at the fade boundary (the padding edge
// for the exterior, or InnerRadius for the interior); the selected curve reshapes how quickly the alpha fades over
// that distance.  A curve that drops faster near the outline (quadratic/cubic) produces a tighter, less bright fade;
// smoothstep holds the inner half bright and reads as a wider, brighter fade.  The interior and exterior curves are
// selected independently at runtime via the GradientDistal InnerFall and OuterFall fields.
//
//   OPAQUE     : 0            (no fade; the half is fully opaque up to its boundary then cut)
//   LINEAR     : a            (even taper)
//   QUADRATIC  : a^2          (fast drop, thin tail — natural glow)
//   CUBIC      : a^3          (very tight glow hugging the outline)
//   SMOOTHSTEP : 3a^2 - 2a^3  (S-curve; brightest, holds the inner half)

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
   // Alpha fall-off curve selector.  Values mirror the Kotuku GFALL enumeration so a GradientDistal field can be
   // forwarded directly without translation.

   enum class sdf_falloff_curve {
      opaque     = 1,
      linear     = 2,
      quadratic  = 3,
      cubic      = 4,
      smoothstep = 5
   };

   // Exterior spread mode.  Controls what happens to the exterior alpha fade beyond the first fall-off cycle (one
   // cycle spans the padding Radius).  Values mirror the Kotuku VSPREAD enumeration for the subset that the SDF
   // honours, so a GradientDistal SpreadMethod can be forwarded after collapsing the unsupported variants to pad.
   //
   //   pad     : a single fade from the outline to the padding edge, then transparent (the classic glow).
   //   repeat  : the fade restarts at full opacity each Radius cycle (sawtooth), tiling out to the fill extent.
   //   reflect : the fade alternates direction each cycle (triangle), ramping 255->0 then 0->255 and so on.

   enum class sdf_spread {
      pad     = 0,
      repeat  = 1,
      reflect = 2
   };

   // Reshapes the linear coverage 'a' (1.0 at the outline, 0.0 at the fade boundary) into the final alpha multiplier
   // using the selected curve.

   static inline double sdf_falloff(double a, sdf_falloff_curve Curve) {
      if (a <= 0.0) return 0.0;
      if (a >= 1.0) return 1.0;
      switch (Curve) {
         case sdf_falloff_curve::opaque:     return 1.0; // Opaque up to the boundary, then cut
         case sdf_falloff_curve::linear:     return a;
         case sdf_falloff_curve::quadratic:  return a * a;
         case sdf_falloff_curve::cubic:      return a * a * a;
         case sdf_falloff_curve::smoothstep: return a * a * (3.0 - (2.0 * a));
         default:                            return a * a;
      }
   }

   class gradient_sdf {
   private:
      std::vector<int8u> m_buffer;
      std::vector<int8u> m_alpha; // Per-pixel alpha with the InnerFall/OuterFall curve already baked in (255 at the
                                  // outline, falling to 0 at the interior InnerRadius and the exterior padding edge)
      int m_lut[256]; // Maps signed DT greyscale values directly to gradient results
      int m_width, m_height;
      int m_origin_x, m_origin_y; // Pixel offset of the path bounds within the padded buffer
      double m_padding; // Margin (in path units) added around the bounds so the exterior ramp half is visible
      double m_inner_radius; // Interior fade distance from the outline; zero keeps the interior fully opaque
      sdf_falloff_curve m_inner_fall; // Alpha fall-off curve baked into the interior half of m_alpha
      sdf_falloff_curve m_outer_fall; // Alpha fall-off curve baked into the exterior half of m_alpha
      sdf_spread m_spread; // Exterior spread mode baked into m_alpha (pad / repeat / reflect)
      double m_fill_extent; // Exterior extent (path units) for repeat/reflect; <= padding falls back to a single cycle
      double m_resolution; // Gradient Resolution stepping baked into m_alpha (before the fall-off curve)
      double m_d1; // Ranges from 0 to 254
      double m_d2; // Ranges from 0.001 to 1.0

      void build_lut() {
         for (int i=0; i < 256; i++) m_lut[i] = iround((i * m_d2) + m_d1) << gradient_subpixel_shift;
      }

   public:
      gradient_sdf() : m_width(0), m_height(0), m_origin_x(0), m_origin_y(0), m_padding(0), m_inner_radius(0),
         m_inner_fall(sdf_falloff_curve::smoothstep), m_outer_fall(sdf_falloff_curve::smoothstep),
         m_spread(sdf_spread::pad), m_fill_extent(0), m_resolution(1.0), m_d1(0), m_d2(1.0) {
         build_lut();
      }

      gradient_sdf(double d1, double d2) : m_width(0), m_height(0), m_origin_x(0), m_origin_y(0), m_padding(0),
         m_inner_radius(0), m_inner_fall(sdf_falloff_curve::smoothstep), m_outer_fall(sdf_falloff_curve::smoothstep),
         m_spread(sdf_spread::pad), m_fill_extent(0), m_resolution(1.0), m_d2(d2) {
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
      void   inner_radius(double Value) { m_inner_radius = (Value < 0) ? 0 : Value; }
      double inner_radius() const { return m_inner_radius; }

      void inner_fall(sdf_falloff_curve Value) { m_inner_fall = Value; }
      void outer_fall(sdf_falloff_curve Value) { m_outer_fall = Value; }
      sdf_falloff_curve inner_fall() const { return m_inner_fall; }
      sdf_falloff_curve outer_fall() const { return m_outer_fall; }

      void   spread(sdf_spread Value) { m_spread = Value; }
      sdf_spread spread() const { return m_spread; }

      // The exterior extent (path units) the repeat/reflect cycle should tile across.  For pad this is ignored; for
      // repeat/reflect it is clamped up to at least the padding so a single cycle always fits.
      void   fill_extent(double Value) { m_fill_extent = (Value < 0) ? 0 : Value; }
      double fill_extent() const { return m_fill_extent; }

      void   resolution(double Value) { m_resolution = Value; }
      double resolution() const { return m_resolution; }

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

      // Returns the alpha coverage at a pixel coordinate (gradient subpixel units), with the per-side fall-off curve
      // already baked in.  The exterior falls from the outline to the padding edge; if inner_radius is non-zero the
      // interior also fades inward from the outline.  Both fades are normalised independently of the colour LUT.
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

   // Match the Gradient Resolution field for SDF alpha ramps.  Colour resolution is baked into the 256-entry
   // ramp table; SDF alpha is independent of that table, so it needs equivalent stepping here.

   static inline int8u sdf_apply_resolution(int Value, double Resolution) {
      if (Resolution >= 1.0) return int8u(Value);

      const int colour_block_size = int((1.0 - Resolution) * 256.0);
      if (colour_block_size <= 1) return int8u(Value);

      if (Value <= 0) return 0;
      if (Value >= 255) return 255;

      int exterior_steps = (128 + colour_block_size - 1) / colour_block_size;
      if (exterior_steps < 1) exterior_steps = 1;
      const int alpha_block_size = (255 + exterior_steps - 1) / exterior_steps;
      int stepped = ((Value + alpha_block_size - 1) / alpha_block_size) * alpha_block_size;
      if (stepped < 0) stepped = 0;
      else if (stepped > 255) stepped = 255;
      return int8u(stepped);
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

      span_gradient_sdf() : m_interpolator(nullptr), m_gradient(nullptr), m_color_function(nullptr), m_d1(0),
         m_d2(0) { }

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

            // Alpha is read straight from the gradient's coverage buffer, which already has resolution stepping and
            // the per-side InnerFall / OuterFall curve baked in by sdf_create (255 at the outline, fading inward when
            // InnerRadius is non-zero and outward to the padding edge).  The hot loop is therefore just a buffer read.

            const double alpha_mul = double(m_gradient->alpha(gx, gy)) / 255.0;

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
      if (pad <= 0) pad = 0.01;

      // 'margin' is the colour/alpha cycle length: one fall-off cycle spans the padding Radius.  In pad mode the
      // buffer is padded by exactly one cycle (the exterior fades once, then transparent).  In repeat/reflect mode
      // the cycle tiles outward, so the buffer is padded out to the requested fill extent while the cycle length
      // stays at 'margin'.

      const int margin = int(ceil(pad));
      int outer_margin = margin;
      if ((m_spread != sdf_spread::pad) and (m_fill_extent > pad)) outer_margin = int(ceil(m_fill_extent));

      const auto width  = int(ceil(bound_w)) + 1 + (outer_margin * 2);
      const auto height = int(ceil(bound_h)) + 1 + (outer_margin * 2);
      m_buffer.resize(width * height);
      std::fill(m_buffer.begin(), m_buffer.end(), 255);

      agg::rendering_buffer rb(m_buffer.data(), width, height, width);

      agg::pixfmt_gray8 pf(rb);
      agg::renderer_base<agg::pixfmt_gray8> renb(pf);
      agg::renderer_primitives<agg::renderer_base<agg::pixfmt_gray8>> prim(renb);
      agg::rasterizer_outline<renderer_primitives<agg::renderer_base<agg::pixfmt_gray8>>> ras(prim);

      // Translate the path into the padded buffer, leaving a margin of empty space on all sides.

      agg::trans_affine mtx;
      mtx.translate(-x1 + outer_margin, -y1 + outer_margin);

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

      // Apply the sign from the interior/exterior mask: inside negative, outside positive.  image[] holds squared
      // distances, so the signed magnitude is sign * sqrt(image).  Track the interior extent independently from
      // the padded exterior so the target shape always receives the full lower half of the colour ramp.

      float max_inside = 0.0f;  // Largest |d| found inside the shape
      float max_outside = 0.0f; // Largest |d| found outside the shape

      for (int l=0, total=width * height; l < total; l++) {
         const float mag = sqrtf(image[l]);
         if (inside[l]) { if (mag > max_inside) max_inside = mag; }
         else { if (mag > max_outside) max_outside = mag; }
      }

      if ((max_inside <= 0.0f) and (max_outside <= 0.0f)) { m_buffer.clear(); m_alpha.clear(); }
      else {
         // Map the interior and exterior independently around the outline midpoint.  The deepest interior reaches
         // the first stop, while the exterior reaches the final stop at the padding boundary.  This avoids a large
         // glow radius compressing all target-shape pixels into a narrow section of the colour ramp.

         const float inside_scale = (max_inside > 0.0f) ? (127.5f / max_inside) : 0.0f;

         // Exterior alpha is normalised against the padding distance (margin), independently of the colour
         // mapping.  This guarantees the glow fades to fully transparent exactly at the padding edge no matter how
         // large the interior distances are; otherwise a shape with a deep interior would clip the exterior glow
         // before its alpha reached zero, leaving a hard rectangular edge at the coverage boundary.

         m_alpha.resize(width * height);
         const float inv_margin = (margin > 0) ? (1.0f / float(margin)) : 0.0f;
         const float inv_inner_radius = (m_inner_radius > 0) ? (1.0f / float(m_inner_radius)) : 0.0f;

         // Resolves an exterior distance into a per-cycle phase 'a' (1.0 at the cycle start, 0.0 at the cycle end)
         // for both the colour mapping and the alpha fall-off.  In pad mode the single cycle clamps to 0 beyond the
         // padding edge; repeat restarts each cycle (sawtooth); reflect alternates direction each cycle (triangle).

         auto exterior_phase = [&](float Mag) -> float {
            float a = 1.0f - (Mag * inv_margin); // 1 at the outline, 0 at the padding edge (one cycle)
            switch (m_spread) {
               case sdf_spread::repeat: {
                  const float cycles = Mag * inv_margin;
                  a = 1.0f - (cycles - floorf(cycles)); // Sawtooth: restart at full opacity each cycle
                  break;
               }
               case sdf_spread::reflect: {
                  const float cycles = Mag * inv_margin;
                  float m2 = fmodf(cycles, 2.0f); // 0..2 within a reflected pair of cycles
                  a = (m2 < 1.0f) ? (1.0f - m2) : (m2 - 1.0f); // Triangle: 255->0 then 0->255
                  break;
               }
               default: break; // pad: linear single fade
            }
            if (a < 0.0f) a = 0.0f;
            else if (a > 1.0f) a = 1.0f;
            return a;
         };

         for (int l=0, total=width * height; l < total; l++) {
            const float mag = sqrtf(image[l]);
            int v;
            if (inside[l]) v = int(127.5f - (mag * inside_scale));
            else {
               // The exterior colour walks the upper ramp half over one cycle.  For repeat/reflect the colour
               // cycles in lock-step with the alpha phase so each tile shows the full exterior ramp again.
               const float phase = exterior_phase(mag); // 1 at the cycle start (outline), 0 at the cycle end
               v = int(127.5f + ((1.0f - phase) * 127.5f));
            }

            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            m_buffer[l] = int8u(v);

            // Resolution stepping and the InnerFall / OuterFall curve are both baked in here so the span generator
            // only has to read the byte.  The order matches the original per-pixel pipeline exactly: the linear
            // coverage is resolution-stepped first (keeping the alpha bands spatially even), then reshaped by the
            // fall-off curve.  The interior uses InnerFall, the exterior uses OuterFall.

            if (inside[l]) {
               if (m_inner_radius <= 0) m_alpha[l] = 255; // Interior is fully opaque by default
               else {
                  float a = 1.0f - (mag * inv_inner_radius); // 1 at the outline, 0 at InnerRadius
                  if (a < 0.0f) a = 0.0f;
                  else if (a > 1.0f) a = 1.0f;
                  const int stepped = sdf_apply_resolution(int(a * 255.0f), m_resolution);
                  m_alpha[l] = int8u(sdf_falloff(double(stepped) / 255.0, m_inner_fall) * 255.0);
               }
            }
            else if ((m_outer_fall IS sdf_falloff_curve::opaque) and (m_spread != sdf_spread::pad)) {
               // OPAQUE holds the alpha at full strength across the whole tiled field.  This is handled here rather
               // than via sdf_falloff because the repeat/reflect phase touches 0 at every cycle seam (the reflect
               // valley lands squarely on a pixel row), and sdf_falloff cuts a==0 to transparent.  That cut is correct
               // for PAD (the padding edge) but would punch a one-pixel transparent seam through a seamless tile.
               m_alpha[l] = 255;
            }
            else {
               // The exterior phase encodes the spread: PAD fades once and clamps to 0 past the padding edge (where
               // OPAQUE correctly cuts to transparent), while repeat/reflect keep the phase above 0 except at the
               // instantaneous cycle seams.

               const float a = exterior_phase(mag);
               const int stepped = sdf_apply_resolution(int(a * 255.0f), m_resolution);
               m_alpha[l] = int8u(sdf_falloff(double(stepped) / 255.0, m_outer_fall) * 255.0);
            }
         }
      }

      m_width    = width;
      m_height   = height;
      m_origin_x = outer_margin;
      m_origin_y = outer_margin;

      return m_buffer.data();
   }
}
