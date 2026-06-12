//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.

//
// Adaptation for high precision colors has been sponsored by
// Liberty Technology Systems, Inc., visit http://lib-sys.com
//
// Liberty Technology Systems, Inc. is the provider of
// PostScript and PDF technology for software developers.
//
//----------------------------------------------------------------------------

#ifndef AGG_SPAN_IMAGE_FILTER_RGBA_INCLUDED
#define AGG_SPAN_IMAGE_FILTER_RGBA_INCLUDED

#include <cstring>

#include "agg_basics.h"
#include "agg_color_rgba.h"
#include "agg_span_image_filter.h"
#include "../../../link/simd.h"

namespace agg
{
   // span_image_filter_rgba_nn
   template<class Source, class Interpolator>
   class span_image_filter_rgba_nn : public span_image_filter<Source, Interpolator> {
   public:
      typedef Source source_type;
      typedef typename source_type::color_type color_type;
      typedef Interpolator in_type;
      typedef span_image_filter<source_type, in_type> base_type;
      typedef typename color_type::value_type value_type;
      unsigned char oR, oG, oB, oA;

      enum base_scale_e {
          base_shift = color_type::base_shift,
          base_mask  = color_type::base_mask
      };

      span_image_filter_rgba_nn(source_type &src, in_type &inter) : base_type(src, inter, 0) {
         oR = src.m_src->mPixelOrder.Red;
         oG = src.m_src->mPixelOrder.Green;
         oB = src.m_src->mPixelOrder.Blue;
         oA = src.m_src->mPixelOrder.Alpha;
      }

      void generate(color_type *span, int x, int y, unsigned len) {
         base_type::interpolator().begin(x + base_type::filter_dx_dbl(), y + base_type::filter_dy_dbl(), len);
         do {
            base_type::interpolator().coordinates(&x, &y);
            auto fg_ptr = (const value_type *)base_type::source().span(x >> image_subpixel_shift, y >> image_subpixel_shift, 1);
            span->r = fg_ptr[oR];
            span->g = fg_ptr[oG];
            span->b = fg_ptr[oB];
            span->a = fg_ptr[oA];
            ++span;
            ++base_type::interpolator();

         } while(--len);
      }
   };

   // span_image_filter_rgba_bilinear

   template<class Source, class Interpolator>
   class span_image_filter_rgba_bilinear :  public span_image_filter<Source, Interpolator> {
   public:
      typedef Source source_type;
      typedef typename source_type::color_type color_type;
      typedef typename source_type::order_type order_type;
      typedef Interpolator in_type;
      typedef span_image_filter<source_type, in_type> base_type;
      typedef typename color_type::value_type value_type;
      typedef typename color_type::calc_type calc_type;

      enum base_scale_e {
         base_shift = color_type::base_shift,
         base_mask  = color_type::base_mask
      };

      span_image_filter_rgba_bilinear() {}
      span_image_filter_rgba_bilinear(source_type &src, in_type &inter) :
         base_type(src, inter, 0)
      {}

      void generate(color_type *span, int x, int y, unsigned len) {
         base_type::interpolator().begin(x + base_type::filter_dx_dbl(), y + base_type::filter_dy_dbl(), len);
         calc_type fg[4];
         const value_type *fg_ptr;

         do {
            int x_hr, y_hr;
            base_type::interpolator().coordinates(&x_hr, &y_hr);

            x_hr -= base_type::filter_dx_int();
            y_hr -= base_type::filter_dy_int();

            int x_lr = x_hr >> image_subpixel_shift;
            int y_lr = y_hr >> image_subpixel_shift;

            unsigned weight;

            fg[0] =
            fg[1] =
            fg[2] =
            fg[3] = image_subpixel_scale * image_subpixel_scale / 2;

            x_hr &= image_subpixel_mask;
            y_hr &= image_subpixel_mask;

            fg_ptr = (const value_type*)base_type::source().span(x_lr, y_lr, 2);
            weight = (image_subpixel_scale - x_hr) * (image_subpixel_scale - y_hr);
            fg[0] += weight * *fg_ptr++;
            fg[1] += weight * *fg_ptr++;
            fg[2] += weight * *fg_ptr++;
            fg[3] += weight * *fg_ptr;

            fg_ptr = (const value_type*)base_type::source().next_x();
            weight = x_hr * (image_subpixel_scale - y_hr);
            fg[0] += weight * *fg_ptr++;
            fg[1] += weight * *fg_ptr++;
            fg[2] += weight * *fg_ptr++;
            fg[3] += weight * *fg_ptr;

            fg_ptr = (const value_type*)base_type::source().next_y();
            weight = (image_subpixel_scale - x_hr) * y_hr;
            fg[0] += weight * *fg_ptr++;
            fg[1] += weight * *fg_ptr++;
            fg[2] += weight * *fg_ptr++;
            fg[3] += weight * *fg_ptr;

            fg_ptr = (const value_type*)base_type::source().next_x();
            weight = x_hr * y_hr;
            fg[0] += weight * *fg_ptr++;
            fg[1] += weight * *fg_ptr++;
            fg[2] += weight * *fg_ptr++;
            fg[3] += weight * *fg_ptr;

            span->r = value_type(fg[order_type::R] >> (image_subpixel_shift * 2));
            span->g = value_type(fg[order_type::G] >> (image_subpixel_shift * 2));
            span->b = value_type(fg[order_type::B] >> (image_subpixel_shift * 2));
            span->a = value_type(fg[order_type::A] >> (image_subpixel_shift * 2));

            ++span;
            ++base_type::interpolator();

         } while(--len);
      }
   };

   // span_image_filter_rgba_bilinear_clip
   template<class Source, class Interpolator>
   class span_image_filter_rgba_bilinear_clip : public span_image_filter<Source, Interpolator> {
   public:
      typedef Source source_type;
      typedef typename source_type::color_type color_type;
      typedef typename source_type::order_type order_type;
      typedef Interpolator in_type;
      typedef span_image_filter<source_type, in_type> base_type;
      typedef typename color_type::value_type value_type;
      typedef typename color_type::calc_type calc_type;

      enum base_scale_e {
          base_shift = color_type::base_shift,
          base_mask  = color_type::base_mask
      };

      span_image_filter_rgba_bilinear_clip() {}
      span_image_filter_rgba_bilinear_clip(source_type& src, const color_type& back_color, in_type& inter) :
          base_type(src, inter, 0),
          m_back_color(back_color)
      {}
      const color_type& background_color() const { return m_back_color; }
      void background_color(const color_type& v)   { m_back_color = v; }

      void generate(color_type* span, int x, int y, unsigned len) {
         base_type::interpolator().begin(x + base_type::filter_dx_dbl(), y + base_type::filter_dy_dbl(), len);

         calc_type fg[4];
         value_type back_r = m_back_color.r;
         value_type back_g = m_back_color.g;
         value_type back_b = m_back_color.b;
         value_type back_a = m_back_color.a;

         const value_type *fg_ptr;
         int maxx = base_type::source().width() - 1;
         int maxy = base_type::source().height() - 1;

         do {
            int x_hr;
            int y_hr;

            base_type::interpolator().coordinates(&x_hr, &y_hr);

            x_hr -= base_type::filter_dx_int();
            y_hr -= base_type::filter_dy_int();

            int x_lr = x_hr >> image_subpixel_shift;
            int y_lr = y_hr >> image_subpixel_shift;

            unsigned weight;

            if(x_lr >= 0 and y_lr >= 0 and x_lr <  maxx and y_lr <  maxy) {
               fg[0] =
               fg[1] =
               fg[2] =
               fg[3] = image_subpixel_scale * image_subpixel_scale / 2;

               x_hr &= image_subpixel_mask;
               y_hr &= image_subpixel_mask;

               fg_ptr = (const value_type*) base_type::source().row_ptr(y_lr) + (x_lr << 2);

               weight = (image_subpixel_scale - x_hr) * (image_subpixel_scale - y_hr);
               fg[0] += weight * *fg_ptr++;
               fg[1] += weight * *fg_ptr++;
               fg[2] += weight * *fg_ptr++;
               fg[3] += weight * *fg_ptr++;

               weight = x_hr * (image_subpixel_scale - y_hr);
               fg[0] += weight * *fg_ptr++;
               fg[1] += weight * *fg_ptr++;
               fg[2] += weight * *fg_ptr++;
               fg[3] += weight * *fg_ptr++;

               ++y_lr;
               fg_ptr = (const value_type*)base_type::source().row_ptr(y_lr) + (x_lr << 2);

               weight = (image_subpixel_scale - x_hr) * y_hr;
               fg[0] += weight * *fg_ptr++;
               fg[1] += weight * *fg_ptr++;
               fg[2] += weight * *fg_ptr++;
               fg[3] += weight * *fg_ptr++;

               weight = x_hr * y_hr;
               fg[0] += weight * *fg_ptr++;
               fg[1] += weight * *fg_ptr++;
               fg[2] += weight * *fg_ptr++;
               fg[3] += weight * *fg_ptr++;

               fg[0] >>= image_subpixel_shift * 2;
               fg[1] >>= image_subpixel_shift * 2;
               fg[2] >>= image_subpixel_shift * 2;
               fg[3] >>= image_subpixel_shift * 2;
            }
            else {
               if (x_lr < -1 or y_lr < -1 or x_lr > maxx or y_lr > maxy) {
                  fg[order_type::R] = back_r;
                  fg[order_type::G] = back_g;
                  fg[order_type::B] = back_b;
                  fg[order_type::A] = back_a;
               }
               else {
                  fg[0] =
                  fg[1] =
                  fg[2] =
                  fg[3] = image_subpixel_scale * image_subpixel_scale / 2;

                  x_hr &= image_subpixel_mask;
                  y_hr &= image_subpixel_mask;

                  weight = (image_subpixel_scale - x_hr) * (image_subpixel_scale - y_hr);
                  if (x_lr >= 0 and y_lr >= 0 and x_lr <= maxx and y_lr <= maxy) {
                     fg_ptr = (const value_type*)base_type::source().row_ptr(y_lr) + (x_lr << 2);

                     fg[0] += weight * *fg_ptr++;
                     fg[1] += weight * *fg_ptr++;
                     fg[2] += weight * *fg_ptr++;
                     fg[3] += weight * *fg_ptr++;
                  }
                  else {
                     fg[order_type::R] += back_r * weight;
                     fg[order_type::G] += back_g * weight;
                     fg[order_type::B] += back_b * weight;
                     fg[order_type::A] += back_a * weight;
                  }

                  x_lr++;

                  weight = x_hr * (image_subpixel_scale - y_hr);
                  if (x_lr >= 0 and y_lr >= 0 and x_lr <= maxx and y_lr <= maxy) {
                     fg_ptr = (const value_type*)base_type::source().row_ptr(y_lr) + (x_lr << 2);

                     fg[0] += weight * *fg_ptr++;
                     fg[1] += weight * *fg_ptr++;
                     fg[2] += weight * *fg_ptr++;
                     fg[3] += weight * *fg_ptr++;
                  }
                  else {
                     fg[order_type::R] += back_r * weight;
                     fg[order_type::G] += back_g * weight;
                     fg[order_type::B] += back_b * weight;
                     fg[order_type::A] += back_a * weight;
                  }

                  x_lr--;
                  y_lr++;

                  weight = (image_subpixel_scale - x_hr) * y_hr;
                  if (x_lr >= 0 and y_lr >= 0 and x_lr <= maxx and y_lr <= maxy) {
                     fg_ptr = (const value_type*)base_type::source().row_ptr(y_lr) + (x_lr << 2);

                     fg[0] += weight * *fg_ptr++;
                     fg[1] += weight * *fg_ptr++;
                     fg[2] += weight * *fg_ptr++;
                     fg[3] += weight * *fg_ptr++;
                  }
                  else {
                     fg[order_type::R] += back_r * weight;
                     fg[order_type::G] += back_g * weight;
                     fg[order_type::B] += back_b * weight;
                     fg[order_type::A] += back_a * weight;
                  }

                  x_lr++;

                  weight = x_hr * y_hr;
                  if (x_lr >= 0 and y_lr >= 0 and x_lr <= maxx and y_lr <= maxy) {
                     fg_ptr = (const value_type*)base_type::source().row_ptr(y_lr) + (x_lr << 2);

                     fg[0] += weight * *fg_ptr++;
                     fg[1] += weight * *fg_ptr++;
                     fg[2] += weight * *fg_ptr++;
                     fg[3] += weight * *fg_ptr++;
                  }
                  else {
                     fg[order_type::R] += back_r * weight;
                     fg[order_type::G] += back_g * weight;
                     fg[order_type::B] += back_b * weight;
                     fg[order_type::A] += back_a * weight;
                  }

                  fg[0] >>= image_subpixel_shift * 2;
                  fg[1] >>= image_subpixel_shift * 2;
                  fg[2] >>= image_subpixel_shift * 2;
                  fg[3] >>= image_subpixel_shift * 2;
               }
            }

            span->r = (value_type)fg[order_type::R];
            span->g = (value_type)fg[order_type::G];
            span->b = (value_type)fg[order_type::B];
            span->a = (value_type)fg[order_type::A];
            ++span;
            ++base_type::interpolator();

         } while(--len);
      }
   private:
      color_type m_back_color;
   };

    // span_image_filter_rgba_2x2

    template<class Source, class Interpolator>
    class span_image_filter_rgba_2x2 : public span_image_filter<Source, Interpolator> {
    public:
        typedef Source source_type;
        typedef typename source_type::color_type color_type;
        typedef typename source_type::order_type order_type;
        typedef Interpolator in_type;
        typedef span_image_filter<source_type, in_type> base_type;
        typedef typename color_type::value_type value_type;
        typedef typename color_type::calc_type calc_type;

        enum base_scale_e {
            base_shift = color_type::base_shift,
            base_mask  = color_type::base_mask
        };

        span_image_filter_rgba_2x2() {}
        span_image_filter_rgba_2x2(source_type& src, in_type& inter, const image_filter_lut& filter) :
            base_type(src, inter, &filter)
        {}

        void generate(color_type* span, int x, int y, unsigned len) {
            base_type::interpolator().begin(x + base_type::filter_dx_dbl(), y + base_type::filter_dy_dbl(), len);

            calc_type fg[4];

            const value_type *fg_ptr;
            const int16* weight_array = base_type::filter().weight_array() + ((base_type::filter().diameter()/2 - 1) << image_subpixel_shift);

            do {
                int x_hr;
                int y_hr;

                base_type::interpolator().coordinates(&x_hr, &y_hr);

                x_hr -= base_type::filter_dx_int();
                y_hr -= base_type::filter_dy_int();

                int x_lr = x_hr >> image_subpixel_shift;
                int y_lr = y_hr >> image_subpixel_shift;

                unsigned weight;
                fg[0] = fg[1] = fg[2] = fg[3] = image_filter_scale / 2;

                x_hr &= image_subpixel_mask;
                y_hr &= image_subpixel_mask;

                fg_ptr = (const value_type*)base_type::source().span(x_lr, y_lr, 2);
                weight = (weight_array[x_hr + image_subpixel_scale] * weight_array[y_hr + image_subpixel_scale] +
                          image_filter_scale / 2) >> image_filter_shift;
                fg[0] += weight * *fg_ptr++;
                fg[1] += weight * *fg_ptr++;
                fg[2] += weight * *fg_ptr++;
                fg[3] += weight * *fg_ptr;

                fg_ptr = (const value_type*)base_type::source().next_x();
                weight = (weight_array[x_hr] * weight_array[y_hr + image_subpixel_scale] +
                          image_filter_scale / 2) >> image_filter_shift;
                fg[0] += weight * *fg_ptr++;
                fg[1] += weight * *fg_ptr++;
                fg[2] += weight * *fg_ptr++;
                fg[3] += weight * *fg_ptr;

                fg_ptr = (const value_type*)base_type::source().next_y();
                weight = (weight_array[x_hr + image_subpixel_scale] * weight_array[y_hr] +
                          image_filter_scale / 2) >> image_filter_shift;
                fg[0] += weight * *fg_ptr++;
                fg[1] += weight * *fg_ptr++;
                fg[2] += weight * *fg_ptr++;
                fg[3] += weight * *fg_ptr;

                fg_ptr = (const value_type*)base_type::source().next_x();
                weight = (weight_array[x_hr] * weight_array[y_hr] +
                          image_filter_scale / 2) >> image_filter_shift;
                fg[0] += weight * *fg_ptr++;
                fg[1] += weight * *fg_ptr++;
                fg[2] += weight * *fg_ptr++;
                fg[3] += weight * *fg_ptr;

                fg[0] >>= image_filter_shift;
                fg[1] >>= image_filter_shift;
                fg[2] >>= image_filter_shift;
                fg[3] >>= image_filter_shift;

                if(fg[order_type::A] > base_mask)         fg[order_type::A] = base_mask;
                if(fg[order_type::R] > fg[order_type::A]) fg[order_type::R] = fg[order_type::A];
                if(fg[order_type::G] > fg[order_type::A]) fg[order_type::G] = fg[order_type::A];
                if(fg[order_type::B] > fg[order_type::A]) fg[order_type::B] = fg[order_type::A];

                span->r = (value_type)fg[order_type::R];
                span->g = (value_type)fg[order_type::G];
                span->b = (value_type)fg[order_type::B];
                span->a = (value_type)fg[order_type::A];
                ++span;
                ++base_type::interpolator();

            } while(--len);
        }
    };

    // span_image_filter_rgba

   template<class Source, class Interpolator>
   class span_image_filter_rgba : public span_image_filter<Source, Interpolator> {
   private:
      span_image_filter_rgba() {}
   public:
      typedef Source source_type;
      typedef typename source_type::color_type color_type;
      typedef Interpolator in_type;
      typedef span_image_filter<source_type, in_type> base_type;
      typedef typename color_type::value_type value_type;
      unsigned char oR, oG, oB, oA;
      bool alpha_limit;

      enum base_scale_e {
         base_shift = color_type::base_shift,
         base_mask  = color_type::base_mask
      };

      span_image_filter_rgba(source_type& src, in_type& inter, const image_filter_lut& filter, bool pAlphaLimit) :
          base_type(src, inter, &filter), alpha_limit(pAlphaLimit)
      {
         oR = src.m_src->mPixelOrder.Red;
         oG = src.m_src->mPixelOrder.Green;
         oB = src.m_src->mPixelOrder.Blue;
         oA = src.m_src->mPixelOrder.Alpha;
      }

      void generate(color_type* span, int x, int y, unsigned len) {
         base_type::interpolator().begin(x + base_type::filter_dx_dbl(), y + base_type::filter_dy_dbl(), len);

         const value_type *fg_ptr;

         unsigned     diameter     = base_type::filter().diameter();
         int          start        = base_type::filter().start();
         const int16* weight_array = base_type::filter().weight_array();

#ifdef KOTUKU_SSE2
         if (diameter IS 2) { // Bilinear: the 2x2 kernel maps onto two _mm_madd_epi16 ops per pixel.
            generate_2x2(span, len, start, weight_array);
            return;
         }
         else if (diameter IS 4) { // Spline16/bicubic/mitchell etc: 4x4 kernel, two madds per row.
            generate_4x4(span, len, start, weight_array);
            return;
         }
#endif

         int x_count, weight_y;

         do {
            base_type::interpolator().coordinates(&x, &y);

            x -= base_type::filter_dx_int();
            y -= base_type::filter_dy_int();

            int x_hr = x;
            int y_hr = y;

            int x_lr = x_hr >> image_subpixel_shift;
            int y_lr = y_hr >> image_subpixel_shift;

            int fg[4];
            fg[0] = fg[1] = fg[2] = fg[3] = image_filter_scale / 2;

            int x_fract = x_hr & image_subpixel_mask;
            unsigned y_count = diameter;

            y_hr = image_subpixel_mask - (y_hr & image_subpixel_mask);
            fg_ptr = (const value_type*)base_type::source().span(x_lr + start, y_lr + start, diameter);
            while (true) {
                x_count  = diameter;
                weight_y = weight_array[y_hr];
                x_hr = image_subpixel_mask - x_fract;
                while (true) {
                    int weight = (weight_y * weight_array[x_hr] + image_filter_scale / 2) >> image_filter_shift;

                    fg[0] += weight * *fg_ptr++;
                    fg[1] += weight * *fg_ptr++;
                    fg[2] += weight * *fg_ptr++;
                    fg[3] += weight * *fg_ptr;

                    if (--x_count == 0) break;
                    x_hr  += image_subpixel_scale;
                    fg_ptr = (const value_type*)base_type::source().next_x();
                }

                if (--y_count == 0) break;
                y_hr  += image_subpixel_scale;
                fg_ptr = (const value_type*)base_type::source().next_y();
            }

            fg[0] >>= image_filter_shift;
            fg[1] >>= image_filter_shift;
            fg[2] >>= image_filter_shift;
            fg[3] >>= image_filter_shift;

            store_clamped(span, fg);
            ++span;
            ++base_type::interpolator();
         } while(--len);
      }

   private:
      // Clamp the accumulated channel values (already shifted down to pixel range) and write them to the span
      // in destination colour order.

      inline void store_clamped(color_type *span, int (&fg)[4]) const {
         if (fg[0] < 0) fg[0] = 0;
         if (fg[1] < 0) fg[1] = 0;
         if (fg[2] < 0) fg[2] = 0;
         if (fg[3] < 0) fg[3] = 0;

         if (fg[oA] > base_mask) fg[oA] = base_mask;

         if (alpha_limit) { // Enable only if pipeline is blending with background color
            if (fg[oR] > fg[oA]) fg[oR] = fg[oA];
            if (fg[oG] > fg[oA]) fg[oG] = fg[oA];
            if (fg[oB] > fg[oA]) fg[oB] = fg[oA];
         }
         else { // For copy-only, non-blending pipelines
            if (fg[oR] > base_mask) fg[oR] = base_mask;
            if (fg[oG] > base_mask) fg[oG] = base_mask;
            if (fg[oB] > base_mask) fg[oB] = base_mask;
         }

         span->r = (value_type)fg[oR];
         span->g = (value_type)fg[oG];
         span->b = (value_type)fg[oB];
         span->a = (value_type)fg[oA];
      }

#ifdef KOTUKU_SSE2
      // SSE2 specialisation of the 2x2 (bilinear) kernel for 4-byte pixels.  Bit-exact with the generic loop:
      // the four combined weights are computed with the same fixed-point arithmetic, and bilinear LUT weights
      // are non-negative and bounded by image_filter_scale (16384) after normalisation, so each fits a signed
      // 16-bit lane for _mm_madd_epi16.  Texel fetches stay on the source accessor so edge wrapping semantics
      // are unchanged.

      void generate_2x2(color_type *span, unsigned len, int start, const int16 *weight_array) {
         const __m128i zero     = _mm_setzero_si128();
         const __m128i rounding = _mm_set1_epi32(image_filter_scale / 2);

         do {
            int x, y;
            base_type::interpolator().coordinates(&x, &y);

            x -= base_type::filter_dx_int();
            y -= base_type::filter_dy_int();

            const int x_lr = x >> image_subpixel_shift;
            const int y_lr = y >> image_subpixel_shift;

            const int x_hr = image_subpixel_mask - (x & image_subpixel_mask);
            const int y_hr = image_subpixel_mask - (y & image_subpixel_mask);

            const int wx0 = weight_array[x_hr];
            const int wx1 = weight_array[x_hr + image_subpixel_scale];
            const int wy0 = weight_array[y_hr];
            const int wy1 = weight_array[y_hr + image_subpixel_scale];

            const int w00 = (wy0 * wx0 + image_filter_scale / 2) >> image_filter_shift;
            const int w01 = (wy0 * wx1 + image_filter_scale / 2) >> image_filter_shift;
            const int w10 = (wy1 * wx0 + image_filter_scale / 2) >> image_filter_shift;
            const int w11 = (wy1 * wx1 + image_filter_scale / 2) >> image_filter_shift;

            // Texel fetch order matches the generic loop: (0,0), (1,0), then next_y resets x for (0,1), (1,1).

            int32_t t00, t01, t10, t11;
            std::memcpy(&t00, base_type::source().span(x_lr + start, y_lr + start, 2), 4);
            std::memcpy(&t01, base_type::source().next_x(), 4);
            std::memcpy(&t10, base_type::source().next_y(), 4);
            std::memcpy(&t11, base_type::source().next_x(), 4);

            // Interleave row pairs per channel so madd produces w_left*left + w_right*right per 32-bit lane.

            const __m128i top = _mm_unpacklo_epi16(
               _mm_unpacklo_epi8(_mm_cvtsi32_si128(t00), zero),
               _mm_unpacklo_epi8(_mm_cvtsi32_si128(t01), zero));
            const __m128i bottom = _mm_unpacklo_epi16(
               _mm_unpacklo_epi8(_mm_cvtsi32_si128(t10), zero),
               _mm_unpacklo_epi8(_mm_cvtsi32_si128(t11), zero));

            __m128i acc = _mm_add_epi32(
               _mm_madd_epi16(top, _mm_set1_epi32((w01 << 16) | (w00 & 0xffff))),
               _mm_madd_epi16(bottom, _mm_set1_epi32((w11 << 16) | (w10 & 0xffff))));
            acc = _mm_srai_epi32(_mm_add_epi32(acc, rounding), image_filter_shift);

            alignas(16) int fg[4];
            _mm_store_si128((__m128i *)fg, acc);

            store_clamped(span, fg);
            ++span;
            ++base_type::interpolator();
         } while (--len);
      }

      // SSE2 specialisation of the 4x4 kernel (spline16, bicubic, mitchell, quadric, gaussian) for 4-byte
      // pixels.  Bit-exact with the generic loop: each row's four combined weights use the same fixed-point
      // arithmetic and the row is accumulated with two _mm_madd_epi16 ops, replacing 64 scalar multiply-adds
      // per pixel with 8 madds.  Diameter-4 kernels peak at ~1.0 so combined weights are bounded by
      // ~image_filter_scale (16384) and fit a signed 16-bit lane.  Negative lobes are handled by the signed
      // madd and the negative clamp in store_clamped().  Texel fetches stay on the source accessor so edge
      // wrapping semantics are unchanged.

      void generate_4x4(color_type *span, unsigned len, int start, const int16 *weight_array) {
         const __m128i zero     = _mm_setzero_si128();
         const __m128i rounding = _mm_set1_epi32(image_filter_scale / 2);

         do {
            int x, y;
            base_type::interpolator().coordinates(&x, &y);

            x -= base_type::filter_dx_int();
            y -= base_type::filter_dy_int();

            const int x_lr = x >> image_subpixel_shift;
            const int y_lr = y >> image_subpixel_shift;

            const int x_hr = image_subpixel_mask - (x & image_subpixel_mask);
            const int y_hr = image_subpixel_mask - (y & image_subpixel_mask);

            const int wx0 = weight_array[x_hr];
            const int wx1 = weight_array[x_hr + image_subpixel_scale];
            const int wx2 = weight_array[x_hr + image_subpixel_scale * 2];
            const int wx3 = weight_array[x_hr + image_subpixel_scale * 3];

            __m128i acc = rounding;

            for (int row = 0; row < 4; row++) {
               const int wy = weight_array[y_hr + (row << image_subpixel_shift)];

               const int w0 = (wy * wx0 + image_filter_scale / 2) >> image_filter_shift;
               const int w1 = (wy * wx1 + image_filter_scale / 2) >> image_filter_shift;
               const int w2 = (wy * wx2 + image_filter_scale / 2) >> image_filter_shift;
               const int w3 = (wy * wx3 + image_filter_scale / 2) >> image_filter_shift;

               // Texel fetch order matches the generic loop: row start (span or next_y), then next_x x3.

               int32_t t0, t1, t2, t3;
               if (row IS 0) std::memcpy(&t0, base_type::source().span(x_lr + start, y_lr + start, 4), 4);
               else std::memcpy(&t0, base_type::source().next_y(), 4);
               std::memcpy(&t1, base_type::source().next_x(), 4);
               std::memcpy(&t2, base_type::source().next_x(), 4);
               std::memcpy(&t3, base_type::source().next_x(), 4);

               const __m128i pair01 = _mm_unpacklo_epi16(
                  _mm_unpacklo_epi8(_mm_cvtsi32_si128(t0), zero),
                  _mm_unpacklo_epi8(_mm_cvtsi32_si128(t1), zero));
               const __m128i pair23 = _mm_unpacklo_epi16(
                  _mm_unpacklo_epi8(_mm_cvtsi32_si128(t2), zero),
                  _mm_unpacklo_epi8(_mm_cvtsi32_si128(t3), zero));

               acc = _mm_add_epi32(acc, _mm_madd_epi16(pair01, _mm_set1_epi32((w1 << 16) | (w0 & 0xffff))));
               acc = _mm_add_epi32(acc, _mm_madd_epi16(pair23, _mm_set1_epi32((w3 << 16) | (w2 & 0xffff))));
            }

            acc = _mm_srai_epi32(acc, image_filter_shift);

            alignas(16) int fg[4];
            _mm_store_si128((__m128i *)fg, acc);

            store_clamped(span, fg);
            ++span;
            ++base_type::interpolator();
         } while (--len);
      }
#endif
   };

   // span_image_resample_rgba_affine
   template<class Source> class span_image_resample_rgba_affine : public span_image_resample_affine<Source> {
   public:
      typedef Source source_type;
      typedef typename source_type::color_type color_type;
      typedef typename source_type::order_type order_type;
      typedef span_image_resample_affine<source_type> base_type;
      typedef typename base_type::in_type in_type;
      typedef typename color_type::value_type value_type;
      typedef typename color_type::long_type long_type;
      enum base_scale_e {
          base_shift      = color_type::base_shift,
          base_mask       = color_type::base_mask,
          downscale_shift = image_filter_shift
      };

      span_image_resample_rgba_affine() {}
      span_image_resample_rgba_affine(source_type& src, in_type& inter, const image_filter_lut& filter) :
          base_type(src, inter, filter)
      {}

      void generate(color_type* span, int x, int y, unsigned len) {
         base_type::interpolator().begin(x + base_type::filter_dx_dbl(), y + base_type::filter_dy_dbl(), len);

         long_type fg[4];

         int diameter     = base_type::filter().diameter();
         int filter_scale = diameter << image_subpixel_shift;
         int radius_x     = (diameter * base_type::m_rx) >> 1;
         int radius_y     = (diameter * base_type::m_ry) >> 1;
         int len_x_lr     = (diameter * base_type::m_rx + image_subpixel_mask) >> image_subpixel_shift;

         const int16* weight_array = base_type::filter().weight_array();

         do {
            base_type::interpolator().coordinates(&x, &y);

            x += base_type::filter_dx_int() - radius_x;
            y += base_type::filter_dy_int() - radius_y;

            fg[0] = fg[1] = fg[2] = fg[3] = image_filter_scale / 2;

            int y_lr = y >> image_subpixel_shift;
            int y_hr = ((image_subpixel_mask - (y & image_subpixel_mask)) * base_type::m_ry_inv) >> image_subpixel_shift;
            int total_weight = 0;
            int x_lr = x >> image_subpixel_shift;
            int x_hr = ((image_subpixel_mask - (x & image_subpixel_mask)) * base_type::m_rx_inv) >> image_subpixel_shift;

            int x_hr2 = x_hr;
            auto fg_ptr = (const value_type*)base_type::source().span(x_lr, y_lr, len_x_lr);
            for(;;) {
               int weight_y = weight_array[y_hr];
               x_hr = x_hr2;
               for(;;) {
                  int weight = (weight_y * weight_array[x_hr] + image_filter_scale / 2) >> downscale_shift;

                  fg[0] += *fg_ptr++ * weight;
                  fg[1] += *fg_ptr++ * weight;
                  fg[2] += *fg_ptr++ * weight;
                  fg[3] += *fg_ptr++ * weight;
                  total_weight += weight;
                  x_hr  += base_type::m_rx_inv;
                  if(x_hr >= filter_scale) break;
                  fg_ptr = (const value_type*)base_type::source().next_x();
               }
               y_hr += base_type::m_ry_inv;
               if(y_hr >= filter_scale) break;
               fg_ptr = (const value_type*)base_type::source().next_y();
            }

            fg[0] /= total_weight;
            fg[1] /= total_weight;
            fg[2] /= total_weight;
            fg[3] /= total_weight;

            if (fg[0] < 0) fg[0] = 0;
            if (fg[1] < 0) fg[1] = 0;
            if (fg[2] < 0) fg[2] = 0;
            if (fg[3] < 0) fg[3] = 0;

            if (fg[order_type::A] > base_mask)         fg[order_type::A] = base_mask;
            if (fg[order_type::R] > fg[order_type::A]) fg[order_type::R] = fg[order_type::A];
            if (fg[order_type::G] > fg[order_type::A]) fg[order_type::G] = fg[order_type::A];
            if (fg[order_type::B] > fg[order_type::A]) fg[order_type::B] = fg[order_type::A];

            span->r = (value_type)fg[order_type::R];
            span->g = (value_type)fg[order_type::G];
            span->b = (value_type)fg[order_type::B];
            span->a = (value_type)fg[order_type::A];

            ++span;
            ++base_type::interpolator();
         } while(--len);
      }
   };

   // span_image_resample_rgba
   template<class Source, class Interpolator>
   class span_image_resample_rgba : public span_image_resample<Source, Interpolator> {
   public:
      typedef Source source_type;
      typedef typename source_type::color_type color_type;
      typedef typename source_type::order_type order_type;
      typedef Interpolator in_type;
      typedef span_image_resample<source_type, in_type> base_type;
      typedef typename color_type::value_type value_type;
      typedef typename color_type::long_type long_type;
      enum base_scale_e {
          base_shift = color_type::base_shift,
          base_mask  = color_type::base_mask,
          downscale_shift = image_filter_shift
      };

      span_image_resample_rgba() {}
      span_image_resample_rgba(source_type& src, in_type& inter, const image_filter_lut& filter) :
          base_type(src, inter, filter)
      {}

      void generate(color_type* span, int x, int y, unsigned len) {
         base_type::interpolator().begin(x + base_type::filter_dx_dbl(), y + base_type::filter_dy_dbl(), len);
         long_type fg[4];

         int diameter = base_type::filter().diameter();
         int filter_scale = diameter << image_subpixel_shift;

         const int16* weight_array = base_type::filter().weight_array();
         do {
            int rx;
            int ry;
            int rx_inv = image_subpixel_scale;
            int ry_inv = image_subpixel_scale;
            base_type::interpolator().coordinates(&x,  &y);
            base_type::interpolator().local_scale(&rx, &ry);
            base_type::adjust_scale(&rx, &ry);

            rx_inv = image_subpixel_scale * image_subpixel_scale / rx;
            ry_inv = image_subpixel_scale * image_subpixel_scale / ry;

            int radius_x = (diameter * rx) >> 1;
            int radius_y = (diameter * ry) >> 1;
            int len_x_lr = (diameter * rx + image_subpixel_mask) >> image_subpixel_shift;

            x += base_type::filter_dx_int() - radius_x;
            y += base_type::filter_dy_int() - radius_y;

            fg[0] = fg[1] = fg[2] = fg[3] = image_filter_scale / 2;

            int y_lr = y >> image_subpixel_shift;
            int y_hr = ((image_subpixel_mask - (y & image_subpixel_mask)) * ry_inv) >> image_subpixel_shift;
            int total_weight = 0;
            int x_lr  = x >> image_subpixel_shift;
            int x_hr  = ((image_subpixel_mask - (x & image_subpixel_mask)) * rx_inv) >> image_subpixel_shift;
            int x_hr2 = x_hr;
            auto fg_ptr = (const value_type*)base_type::source().span(x_lr, y_lr, len_x_lr);

            for(;;) {
               int weight_y = weight_array[y_hr];
               x_hr = x_hr2;
               for(;;) {
                  int weight = (weight_y * weight_array[x_hr] + image_filter_scale / 2) >> downscale_shift;
                  fg[0] += *fg_ptr++ * weight;
                  fg[1] += *fg_ptr++ * weight;
                  fg[2] += *fg_ptr++ * weight;
                  fg[3] += *fg_ptr++ * weight;
                  total_weight += weight;
                  x_hr  += rx_inv;
                  if(x_hr >= filter_scale) break;
                  fg_ptr = (const value_type*)base_type::source().next_x();
               }
               y_hr += ry_inv;
               if (y_hr >= filter_scale) break;
               fg_ptr = (const value_type*)base_type::source().next_y();
            }

            fg[0] /= total_weight;
            fg[1] /= total_weight;
            fg[2] /= total_weight;
            fg[3] /= total_weight;

            if (fg[0] < 0) fg[0] = 0;
            if (fg[1] < 0) fg[1] = 0;
            if (fg[2] < 0) fg[2] = 0;
            if (fg[3] < 0) fg[3] = 0;

            if (fg[order_type::A] > base_mask)         fg[order_type::A] = base_mask;
            if (fg[order_type::R] > fg[order_type::R]) fg[order_type::R] = fg[order_type::R];
            if (fg[order_type::G] > fg[order_type::G]) fg[order_type::G] = fg[order_type::G];
            if (fg[order_type::B] > fg[order_type::B]) fg[order_type::B] = fg[order_type::B];

            span->r = (value_type)fg[order_type::R];
            span->g = (value_type)fg[order_type::G];
            span->b = (value_type)fg[order_type::B];
            span->a = (value_type)fg[order_type::A];

            ++span;
            ++base_type::interpolator();
         } while(--len);
      }
   };
}

#endif
