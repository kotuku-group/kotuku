// Copyright (C) 2026 Paul Manias
//
// Permission to copy, use, modify, sell and distribute this software is granted provided this copyright notice
// appears in all copies.  This software is provided "as is" without express or implied warranty, and with no claim
// as to its suitability for any purpose.
// ---
// Computes seeded Worley / Voronoi field values for cellular gradients.  The generated scalar field is cached as a
// greyscale buffer and sampled through the standard AGG span_gradient interface, matching the contour gradient model.

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

namespace agg
{
   struct worley_feature {
      double x, y, height;
   };

   class splitmix64_generator {
   private:
      uint64_t m_state;

   public:
      explicit splitmix64_generator(uint64_t Seed) : m_state(Seed) { }

      uint64_t next() {
         uint64_t z = (m_state += 0x9e3779b97f4a7c15ULL);
         z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
         z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
         return z ^ (z >> 31);
      }

      double unit() {
         return double(next() >> 11) * (1.0 / 9007199254740992.0);
      }

      double range(double Min, double Max) {
         return Min + ((Max - Min) * unit());
      }
   };

   class gradient_worley {
   private:
      std::vector<int8u> m_buffer;
      int m_lut[256]; // Maps field greyscale values directly to gradient results
      int m_width, m_height;
      double m_d1; // Ranges from 0 to 254
      double m_d2; // Ranges from 0.001 to 1.0

      void build_lut() {
         for (int i=0; i < 256; i++) m_lut[i] = iround((i * m_d2) + m_d1) << gradient_subpixel_shift;
      }

      static void store_field_value(std::vector<float> &Field, int Pos, double Value, float &MinValue,
         float &MaxValue)
      {
         const float value = float(Value);
         Field[Pos] = value;
         if (value < MinValue) MinValue = value;
         if (value > MaxValue) MaxValue = value;
      }

      template <WLM Metric> static double distance_from_delta(double Dx, double Dy) {
         if constexpr (Metric IS WLM::MANHATTAN) return Dx + Dy;
         else if constexpr (Metric IS WLM::CHEBYSHEV) return (Dx > Dy) ? Dx : Dy;
         else return sqrt((Dx * Dx) + (Dy * Dy));
      }

      template <WLM Metric, WLF Mode> static void build_field(const std::vector<worley_feature> &Features,
         const std::vector<int8u> &Mask, int Width, int Height, std::vector<float> &Field, float &MinValue,
         float &MaxValue)
      {
         for (int y=0; y < Height; y++) {
            for (int x=0; x < Width; x++) {
               const int pos = (y * Width) + x;
               if (Mask[pos] > 0x01) continue;

               const double px = x + 0.5;
               const double py = y + 0.5;

               if constexpr ((Mode IS WLF::F1) or (Mode IS WLF::F2) or (Mode IS WLF::F2_F1)) {
                  double f1 = std::numeric_limits<double>::max();
                  double f2 = std::numeric_limits<double>::max();

                  for (auto &point : Features) {
                     const double dx = (point.x > px) ? point.x - px : px - point.x;
                     const double dy = (point.y > py) ? point.y - py : py - point.y;

                     double dist;
                     if constexpr (Metric IS WLM::EUCLIDEAN) dist = (dx * dx) + (dy * dy);
                     else dist = distance_from_delta<Metric>(dx, dy);

                     if (dist < f1) {
                        f2 = f1;
                        f1 = dist;
                     }
                     else if (dist < f2) f2 = dist;
                  }

                  if (f2 IS std::numeric_limits<double>::max()) f2 = f1;

                  double value;
                  if constexpr (Metric IS WLM::EUCLIDEAN) {
                     if constexpr (Mode IS WLF::F2_F1) value = sqrt(f2) - sqrt(f1);
                     else if constexpr (Mode IS WLF::F2) value = sqrt(f2);
                     else value = sqrt(f1);
                  }
                  else {
                     if constexpr (Mode IS WLF::F2_F1) value = f2 - f1;
                     else if constexpr (Mode IS WLF::F2) value = f2;
                     else value = f1;
                  }

                  store_field_value(Field, pos, value, MinValue, MaxValue);
               }
               else if constexpr (Mode IS WLF::WEIGHTED) {
                  double weighted = std::numeric_limits<double>::max();

                  for (auto &point : Features) {
                     const double dx = (point.x > px) ? point.x - px : px - point.x;
                     const double dy = (point.y > py) ? point.y - py : py - point.y;
                     const double dominance = distance_from_delta<Metric>(dx, dy) - point.height;
                     if (dominance < weighted) weighted = dominance;
                  }

                  store_field_value(Field, pos, weighted, MinValue, MaxValue);
               }
               else {
                  double peaks = std::numeric_limits<double>::lowest();

                  for (auto &point : Features) {
                     const double dx = (point.x > px) ? point.x - px : px - point.x;
                     const double dy = (point.y > py) ? point.y - py : py - point.y;
                     const double peak = point.height - distance_from_delta<Metric>(dx, dy);
                     if (peak > peaks) peaks = peak;
                  }

                  store_field_value(Field, pos, peaks, MinValue, MaxValue);
               }
            }
         }
      }

      template <WLM Metric> static void build_field_for_metric(WLF Mode, const std::vector<worley_feature> &Features,
         const std::vector<int8u> &Mask, int Width, int Height, std::vector<float> &Field, float &MinValue,
         float &MaxValue)
      {
         if (Mode IS WLF::F2_F1) build_field<Metric, WLF::F2_F1>(Features, Mask, Width, Height, Field,
            MinValue, MaxValue);
         else if (Mode IS WLF::F2) build_field<Metric, WLF::F2>(Features, Mask, Width, Height, Field,
            MinValue, MaxValue);
         else if (Mode IS WLF::WEIGHTED) build_field<Metric, WLF::WEIGHTED>(Features, Mask, Width, Height, Field,
            MinValue, MaxValue);
         else if (Mode IS WLF::PEAKS) build_field<Metric, WLF::PEAKS>(Features, Mask, Width, Height, Field,
            MinValue, MaxValue);
         else build_field<Metric, WLF::F1>(Features, Mask, Width, Height, Field, MinValue, MaxValue);
      }

      static void build_worley_field(WLF Mode, WLM Metric, const std::vector<worley_feature> &Features,
         const std::vector<int8u> &Mask, int Width, int Height, std::vector<float> &Field, float &MinValue,
         float &MaxValue)
      {
         if (Metric IS WLM::MANHATTAN) build_field_for_metric<WLM::MANHATTAN>(Mode, Features, Mask, Width,
            Height, Field, MinValue, MaxValue);
         else if (Metric IS WLM::CHEBYSHEV) build_field_for_metric<WLM::CHEBYSHEV>(Mode, Features, Mask, Width,
            Height, Field, MinValue, MaxValue);
         else build_field_for_metric<WLM::EUCLIDEAN>(Mode, Features, Mask, Width, Height, Field, MinValue,
            MaxValue);
      }

      static bool inside_mask(const std::vector<int8u> &Mask, int Width, int Height, int X, int Y) {
         if ((X < 0) or (Y < 0) or (X >= Width) or (Y >= Height)) return false;
         return Mask[(Y * Width) + X] <= 0x01;
      }

      static void add_uniform_point(std::vector<worley_feature> &Features, splitmix64_generator &Prng,
         const std::vector<int8u> &Mask, int Width, int Height, double HeightMin, double HeightMax)
      {
         double x = 0;
         double y = 0;
         bool accepted = false;

         for (int tries=0; tries < 128; tries++) {
            x = Prng.range(0, Width - 1);
            y = Prng.range(0, Height - 1);
            if (inside_mask(Mask, Width, Height, int(x), int(y))) {
               accepted = true;
               break;
            }
         }

         if (!accepted) {
            for (int row=0; row < Height; row++) {
               for (int col=0; col < Width; col++) {
                  if (inside_mask(Mask, Width, Height, col, row)) {
                     x = col + 0.5;
                     y = row + 0.5;
                     accepted = true;
                     break;
                  }
               }
               if (accepted) break;
            }
         }

         if (accepted) Features.push_back({ x, y, Prng.range(HeightMin, HeightMax) });
      }

      static void create_uniform_features(std::vector<worley_feature> &Features, splitmix64_generator &Prng,
         const std::vector<int8u> &Mask, int Width, int Height, int PointCount, double HeightMin, double HeightMax)
      {
         Features.reserve(PointCount);
         for (int i=0; i < PointCount; i++) {
            add_uniform_point(Features, Prng, Mask, Width, Height, HeightMin, HeightMax);
         }
      }

      static void create_jittered_features(std::vector<worley_feature> &Features, splitmix64_generator &Prng,
         const std::vector<int8u> &Mask, int Width, int Height, int PointCount, double HeightMin, double HeightMax,
         double Jitter)
      {
         const double aspect = (Height > 0) ? double(Width) / double(Height) : 1.0;
         int cols = int(ceil(sqrt(double(PointCount) * aspect)));
         if (cols < 1) cols = 1;
         int rows = (PointCount + cols - 1) / cols;
         if (rows < 1) rows = 1;

         const double cell_w = double(Width) / double(cols);
         const double cell_h = double(Height) / double(rows);
         const double jitter = std::clamp(Jitter, 0.0, 1.0);

         Features.reserve(PointCount);
         for (int i=0; i < PointCount; i++) {
            const int col = i % cols;
            const int row = i / cols;
            double x = (double(col) + 0.5) * cell_w;
            double y = (double(row) + 0.5) * cell_h;
            x += (Prng.unit() - 0.5) * cell_w * jitter;
            y += (Prng.unit() - 0.5) * cell_h * jitter;

            if (x < 0) x = 0;
            else if (x > Width - 1) x = Width - 1;
            if (y < 0) y = 0;
            else if (y > Height - 1) y = Height - 1;

            if (inside_mask(Mask, Width, Height, int(x), int(y))) {
               Features.push_back({ x, y, Prng.range(HeightMin, HeightMax) });
            }
            else add_uniform_point(Features, Prng, Mask, Width, Height, HeightMin, HeightMax);
         }
      }

   public:
      gradient_worley() : m_width(0), m_height(0), m_d1(0), m_d2(1.0) { build_lut(); }
      gradient_worley(double d1, double d2) : m_width(0), m_height(0), m_d2(d2) {
         m_d1 = (d1 > 254) ? 254 : d1;
         build_lut();
      }

      int8u* worley_create(path_storage &ps, uint64_t Seed, int PointCount, WLF Mode, WLM Metric,
         double HeightMin, double HeightMax, double Jitter);

      int worley_width() { return m_width; }
      int worley_height() { return m_height; }

      void d1(double d) { m_d1 = d; build_lut(); }
      void d2(double d) { m_d2 = d; build_lut(); }

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

   int8u * gradient_worley::worley_create(path_storage &ps, uint64_t Seed, int PointCount, WLF Mode, WLM Metric,
      double HeightMin, double HeightMax, double Jitter)
   {
      agg::conv_curve<agg::path_storage> conv(ps);
      agg::path_storage flat;
      flat.concat_path(conv, 0);

      double x1, y1, x2, y2;
      if (!agg::bounding_rect_single(flat, 0, &x1, &y1, &x2, &y2)) return nullptr;

      const auto width  = int(ceil(x2 - x1)) + 1;
      const auto height = int(ceil(y2 - y1)) + 1;
      if ((width <= 0) or (height <= 0)) return nullptr;

      m_buffer.resize(width * height);
      std::fill(m_buffer.begin(), m_buffer.end(), 255);

      agg::rendering_buffer rb(m_buffer.data(), width, height, width);
      agg::pixfmt_gray8 pf(rb);
      agg::renderer_base<agg::pixfmt_gray8> renb(pf);

      agg::trans_affine mtx;
      mtx.translate(-x1, -y1);
      agg::conv_transform<agg::path_storage> trans(flat, mtx);

      agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_gray8>> solid(renb);
      agg::rasterizer_scanline_aa<> rasterizer;
      agg::scanline32_p8 sl;
      rasterizer.reset();
      rasterizer.add_path(trans);
      solid.color(agg::gray8(0x01, 0xff));
      agg::render_scanlines(rasterizer, sl, solid);

      std::vector<int8u> mask(m_buffer);

      if (PointCount < 1) PointCount = 1;
      else if (PointCount > 4096) PointCount = 4096;

      if (HeightMin > HeightMax) std::swap(HeightMin, HeightMax);

      splitmix64_generator prng(Seed ? Seed : 0x6a09e667f3bcc909ULL);
      std::vector<worley_feature> features;
      if (Jitter > 0.0) {
         create_jittered_features(features, prng, mask, width, height, PointCount, HeightMin, HeightMax, Jitter);
      }
      else create_uniform_features(features, prng, mask, width, height, PointCount, HeightMin, HeightMax);

      if (features.empty()) {
         m_buffer.clear();
         m_width = width;
         m_height = height;
         return nullptr;
      }

      std::vector<float> field(width * height, 0.0f);
      float min_value = std::numeric_limits<float>::max();
      float max_value = std::numeric_limits<float>::lowest();

      build_worley_field(Mode, Metric, features, mask, width, height, field, min_value, max_value);

      if (min_value IS max_value) m_buffer.clear();
      else {
         const float scale = 255.0f / (max_value - min_value);
         for (int l=0, total=width * height; l < total; l++) {
            if (mask[l] <= 0x01) {
               int value = int((field[l] - min_value) * scale);
               if (value < 0) value = 0;
               else if (value > 255) value = 255;
               m_buffer[l] = int8u(value);
            }
            else m_buffer[l] = 0;
         }
      }

      m_width  = width;
      m_height = height;

      return m_buffer.data();
   }
}
