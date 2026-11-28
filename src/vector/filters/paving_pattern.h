// Procedural paving layouts, as used by PavingFX.
//
// A paving partitions the pattern plane into rectangular elements separated by joints.  Every layout maps a pattern
// position to the element that owns it in O(1) time, returning the position local to that element and an integer
// element key.  Joint coverage, the bevelled height field, seeded per-element variation and OKLCH colour adjustment
// are all evaluated from that local position, so results depend only on the sampled position and never on the extent
// of the area being drawn.
//
// All lengths are in pattern units, which are isotropic primitive units after translation by the origin and rotation
// by the pattern angle.  Mapping a filter primitive into pattern units is the caller's responsibility.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <kotuku/modules/vector.h>
#include "cellular_noise.h"
#include "../easing.h"
#include "../oklab.h"

// Layout values match the public LAY enumeration.

enum class paving_layout : int { RUNNING = 0, STACK = 1, BASKET_WEAVE = 2, HERRINGBONE = 3, CHECKER = 4 };

static_assert(int(paving_layout::RUNNING)      IS int(LAY::RUNNING));
static_assert(int(paving_layout::STACK)        IS int(LAY::STACK));
static_assert(int(paving_layout::BASKET_WEAVE) IS int(LAY::BASKET_WEAVE));
static_assert(int(paving_layout::HERRINGBONE)  IS int(LAY::HERRINGBONE));
static_assert(int(paving_layout::CHECKER)      IS int(LAY::CHECKER));

constexpr int PAVING_MAX_PERIOD = 1<<20;

// Element indices stay exact while pattern positions, measured in element units, remain below 2^52.

constexpr double PAVING_LATTICE_LIMIT = 4503599627370496.0;

// BASKET_WEAVE and HERRINGBONE require ElementWidth / ElementHeight to lie this close to an integer k >= 2.

constexpr double PAVING_RATIO_TOLERANCE = 1e-4;

// RUNNING with a vertical period requires PeriodY * RowOffset to lie this close to an integer.

constexpr double PAVING_ROW_TOLERANCE = 1e-9;

struct paving_params {
   paving_layout Layout = paving_layout::RUNNING;
   double ElementWidth = 0;       // Resolved, > 0
   double ElementHeight = 0;      // Resolved, > 0
   double JointWidth = 0;         // Resolved, >= 0
   double BevelWidth = 0;         // Resolved, >= 0
   double RowOffset = 0.5;        // RUNNING only; fraction of ElementWidth per row
   double Angle = 0;              // Degrees; only checked against the periods here
   double ElementHeightValue = 1.0;
   double JointHeightValue = 0.0;
   double HueVariation = 0;       // Degrees, [0, 180]
   double ChromaVariation = 0;    // OKLCH chroma, [0, 0.4]
   double LightVariation = 0;     // OKLCH lightness, [0, 1]
   double HeightVariation = 0;    // Height units, [0, 100]
   GEZ BevelProfile = GEZ::LINEAR;
   int64_t Seed = 0;
   int PeriodX = 0;               // Repeat in layout cells (see paving_sampler); 0 disables
   int PeriodY = 0;

   bool colour_varied() const {
      return (HueVariation != 0.0) or (ChromaVariation != 0.0) or (LightVariation != 0.0);
   }
};

// The element that owns a pattern position.  LX runs along the element's length (U) and LY across its thickness (V);
// both rotate with the element, so for a Vertical element LX follows the pattern Y axis.  The key is the element's
// unwrapped identity; paving_sampler::element_key() wraps and hashes it.

struct paving_sample {
   double QX, QY;     // Position in the pattern plane
   double LX, LY;     // In [0, Length) x [0, Thickness)
   double U, V;       // LX / Length, LY / Thickness, in [0, 1)
   int64_t KeyX, KeyY, KeyZ;
   int Parity;        // CHECKER cell parity, otherwise 0
   bool Vertical;     // True if the element is rotated by 90 degrees relative to the pattern axes
};

// Seeded per-element offsets, each scaled by its variation field.

struct paving_variation {
   double Id;         // [0, 1)
   double Hue;        // Degrees, [-HueVariation, HueVariation]
   double Chroma;     // [-ChromaVariation, ChromaVariation]
   double Light;      // [-LightVariation, LightVariation]
   double Height;     // [-HeightVariation, HeightVariation]
   double HueUnit;    // The unscaled hue variate in [0, 1), used to pick a hue for neutral colours
};

// A colour in polar OKLab (OKLCH).  H is in radians and is meaningless when Neutral is true.

struct paving_lch {
   double L, C, H;
   bool Neutral;
};

//********************************************************************************************************************

inline int64_t paving_floor_div(int64_t A, int64_t B)
{
   const int64_t q = A / B;
   return ((A % B) != 0 and ((A < 0) != (B < 0))) ? q - 1 : q;
}

inline int64_t paving_floor_mod(int64_t A, int64_t B)
{
   const int64_t r = A % B;
   return (r < 0) ? r + B : r;
}

// Hashes an element key with the seed.  Variates are drawn from the result with cellular_hash().

inline uint64_t paving_element_key(int64_t Seed, int64_t X, int64_t Y, int64_t Z)
{
   const uint64_t seeded = cellular_mix(uint64_t(Seed) + CELLULAR_GOLDEN);
   return cellular_mix(cellular_mix(seeded + uint64_t(X) * 0xd1b54a32d192ed03ULL) + uint64_t(Y) * 0xaef17502108ef2d9ULL)
      + uint64_t(Z) * 0xcb24d0a5c88c35b3ULL;
}

inline double paving_ease(GEZ Profile, double Value)
{
   return gez_ease(Profile, Value);
}

// Integer element ratio k = ElementWidth / ElementHeight for BASKET_WEAVE and HERRINGBONE, or 0 if the ratio is not
// within PAVING_RATIO_TOLERANCE of an integer k >= 2.

inline int paving_ratio(double Width, double Height)
{
   if (not ((Width > 0) and (Height > 0))) return 0;
   const double ratio = Width / Height;
   if (not std::isfinite(ratio) or (ratio > double(PAVING_MAX_PERIOD))) return 0;
   const double k = std::round(ratio);
   if ((k < 2) or (std::abs(ratio - k) > PAVING_RATIO_TOLERANCE)) return 0;
   return int(k);
}

//********************************************************************************************************************
// Checks combinations of parameters that individual field setters cannot, as required before drawing.  The caller
// must skip drawing without error if either element dimension is not positive (e.g. a collapsed filter region), and
// is responsible for the Mode and OutputMode combination.

inline ERR paving_check(const paving_params &Params)
{
   if (not (std::isfinite(Params.ElementWidth) and (Params.ElementWidth > 0))) return ERR::InvalidValue;
   if (not (std::isfinite(Params.ElementHeight) and (Params.ElementHeight > 0))) return ERR::InvalidValue;
   if (not (std::isfinite(Params.JointWidth) and (Params.JointWidth >= 0))) return ERR::InvalidValue;
   if (not (std::isfinite(Params.BevelWidth) and (Params.BevelWidth >= 0))) return ERR::InvalidValue;
   if (not std::isfinite(Params.RowOffset)) return ERR::InvalidValue;
   if ((Params.PeriodX < 0) or (Params.PeriodX > PAVING_MAX_PERIOD)) return ERR::OutOfRange;
   if ((Params.PeriodY < 0) or (Params.PeriodY > PAVING_MAX_PERIOD)) return ERR::OutOfRange;

   if ((Params.Layout IS paving_layout::BASKET_WEAVE) or (Params.Layout IS paving_layout::HERRINGBONE)) {
      if (not paving_ratio(Params.ElementWidth, Params.ElementHeight)) return ERR::InvalidValue;
   }

   const bool periodic = (Params.PeriodX > 0) or (Params.PeriodY > 0);
   if (periodic) {
      if (not std::isfinite(Params.Angle) or (std::fmod(Params.Angle, 360.0) != 0.0)) return ERR::InvalidValue;

      if ((Params.Layout IS paving_layout::RUNNING) and (Params.PeriodY > 0)) {
         const double shift = double(Params.PeriodY) * Params.RowOffset;
         if (std::abs(shift - std::round(shift)) > PAVING_ROW_TOLERANCE) return ERR::InvalidValue;
      }

      if ((Params.Layout IS paving_layout::CHECKER) or (Params.Layout IS paving_layout::BASKET_WEAVE)) {
         if ((Params.PeriodX & 1) or (Params.PeriodY & 1)) return ERR::InvalidValue;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// OKLCH conversion of a straight linear RGB colour.

inline paving_lch paving_lch_from_linear(double R, double G, double B)
{
   const auto lab = oklab::from_linear_rgb(R, G, B);
   const double c = std::hypot(lab.a, lab.b);
   if (c < 1e-6) return { lab.l, 0.0, 0.0, true };
   return { lab.l, c, std::atan2(lab.b, lab.a), false };
}

inline bool paving_in_gamut(const oklab::LinearRGB &Colour)
{
   return (Colour.r >= 0.0) and (Colour.r <= 1.0) and (Colour.g >= 0.0) and (Colour.g <= 1.0) and
      (Colour.b >= 0.0) and (Colour.b <= 1.0);
}

// Applies additive OKLCH offsets to Base and returns straight linear RGB within [0, 1].  Lightness is clamped to
// [0, 1] and chroma to >= 0.  Neutral colours keep zero chroma unless HueVaried, in which case they take the seeded
// hue HueUnit * 2pi so that tinted greys scatter evenly.  Out-of-gamut results reduce chroma at fixed lightness and
// hue by bisection.

inline oklab::LinearRGB paving_adjust_oklch(const paving_lch &Base, double DL, double DC, double DH, double HueUnit,
   bool HueVaried)
{
   constexpr double TWO_PI = 6.283185307179586;
   const double l = std::clamp(Base.L + DL, 0.0, 1.0);
   double c, h;
   if (Base.Neutral) {
      if (HueVaried) {
         c = std::max(0.0, Base.C + DC);
         h = HueUnit * TWO_PI;
      }
      else c = h = 0.0;
   }
   else {
      c = std::max(0.0, Base.C + DC);
      h = Base.H + (DH * (TWO_PI / 360.0));
   }

   const double ca = std::cos(h), sa = std::sin(h);
   auto result = oklab::to_linear_rgb({ l, c * ca, c * sa });
   if (not paving_in_gamut(result)) {
      double lo = 0.0, hi = c;
      for (int i=0; i < 8; i++) {
         const double mid = (lo + hi) * 0.5;
         if (paving_in_gamut(oklab::to_linear_rgb({ l, mid * ca, mid * sa }))) lo = mid;
         else hi = mid;
      }
      result = oklab::to_linear_rgb({ l, lo * ca, lo * sa });
      result.r = std::clamp(result.r, 0.0, 1.0);
      result.g = std::clamp(result.g, 0.0, 1.0);
      result.b = std::clamp(result.b, 0.0, 1.0);
   }
   return result;
}

//********************************************************************************************************************
// Locates the element that owns the pattern position (QX, QY).  Length and Thickness are the element dimensions; for
// BASKET_WEAVE and HERRINGBONE, Thickness must equal Length / K exactly.

inline paving_sample paving_sample_point(paving_layout Layout, double Length, double Thickness, int K, double RowOffset,
   double QX, double QY)
{
   paving_sample s { .QX = QX, .QY = QY, .Parity = 0, .Vertical = false };

   switch (Layout) {
      case paving_layout::RUNNING: {
         const double fy = std::floor(QY / Thickness);
         const double xs = QX - (fy * RowOffset * Length);
         const double fx = std::floor(xs / Length);
         s.LX = xs - (fx * Length);
         s.LY = QY - (fy * Thickness);
         s.KeyX = int64_t(fx);
         s.KeyY = int64_t(fy);
         s.KeyZ = 0;
         break;
      }

      case paving_layout::BASKET_WEAVE: {
         // Square macro-cells of side Length, alternating between horizontal and vertical packs of K elements.
         const double mx = std::floor(QX / Length), my = std::floor(QY / Length);
         const double fx = QX - (mx * Length), fy = QY - (my * Length);
         s.KeyX = int64_t(mx);
         s.KeyY = int64_t(my);
         s.Vertical = ((s.KeyX + s.KeyY) & 1) != 0;
         const double across = s.Vertical ? fx : fy;
         const int64_t strip = std::clamp(int64_t(std::floor(across / Thickness)), int64_t(0), int64_t(K - 1));
         s.LX = s.Vertical ? fy : fx;
         s.LY = across - (double(strip) * Thickness);
         s.KeyZ = strip;
         break;
      }

      case paving_layout::HERRINGBONE: {
         // A unit grid of spacing Thickness.  Horizontal elements are anchored where (gx - gy) mod 2K is 0, vertical
         // elements where it is 2K - 1.
         const double ux = std::floor(QX / Thickness), uy = std::floor(QY / Thickness);
         const double fx = QX - (ux * Thickness), fy = QY - (uy * Thickness);
         const int64_t gx = int64_t(ux), gy = int64_t(uy);
         const int64_t d = paving_floor_mod(gx - gy, 2 * K);
         if (d < K) {
            s.KeyX = gx - d;
            s.KeyY = gy;
            s.KeyZ = 0;
            s.LX = (double(d) * Thickness) + fx;
            s.LY = fy;
         }
         else {
            const int64_t j = (2 * K) - 1 - d;
            s.KeyX = gx;
            s.KeyY = gy - j;
            s.KeyZ = 1;
            s.LX = (double(j) * Thickness) + fy;
            s.LY = fx;
            s.Vertical = true;
         }
         break;
      }

      case paving_layout::STACK:
      case paving_layout::CHECKER:
      default: {
         const double fx = std::floor(QX / Length), fy = std::floor(QY / Thickness);
         s.LX = QX - (fx * Length);
         s.LY = QY - (fy * Thickness);
         s.KeyX = int64_t(fx);
         s.KeyY = int64_t(fy);
         s.KeyZ = 0;
         if (Layout IS paving_layout::CHECKER) s.Parity = int((s.KeyX + s.KeyY) & 1);
         break;
      }
   }

   // Guard against rounding at the upper cell boundary, e.g. QX = -1e-17 giving LX = Length.

   s.LX = std::clamp(s.LX, 0.0, std::nextafter(Length, 0.0));
   s.LY = std::clamp(s.LY, 0.0, std::nextafter(Thickness, 0.0));
   s.U = std::min(s.LX / Length, std::nextafter(1.0, 0.0));
   s.V = std::min(s.LY / Thickness, std::nextafter(1.0, 0.0));
   return s;
}

//********************************************************************************************************************
// Evaluates a validated paving.  AAX and AAY are the pattern-unit lengths of one output pixel along the pattern X and
// Y axes, which control the width of the anti-aliased joint edge.
//
// Periods count layout cells: elements for RUNNING, STACK and CHECKER, square macro-cells of side ElementWidth for
// BASKET_WEAVE, and 2 x ElementWidth fundamental tiles for HERRINGBONE.  period_vector() returns the corresponding
// pattern-unit repeat.

class paving_sampler {
   paving_params m_params;
   double m_length, m_thickness;
   double m_half_joint;
   double m_aa_x, m_aa_y;
   int m_ratio = 1;
   int64_t m_row_shift = 0;   // RUNNING: whole elements shifted per vertical period
   int64_t m_wrap_x = 0, m_wrap_y = 0;
   bool m_solid_joint;

public:
   paving_sampler(const paving_params &Params, double AAX = 1.0, double AAY = 1.0) : m_params(Params) {
      m_length = Params.ElementWidth;
      m_thickness = Params.ElementHeight;
      m_aa_x = std::max(std::abs(AAX), 1e-12);
      m_aa_y = std::max(std::abs(AAY), 1e-12);

      if ((Params.Layout IS paving_layout::BASKET_WEAVE) or (Params.Layout IS paving_layout::HERRINGBONE)) {
         m_ratio = std::max(paving_ratio(Params.ElementWidth, Params.ElementHeight), 1);
         m_thickness = m_length / double(m_ratio);
      }

      m_half_joint = Params.JointWidth * 0.5;
      m_solid_joint = Params.JointWidth >= std::min(m_length, m_thickness);

      if (Params.Layout IS paving_layout::HERRINGBONE) {
         m_wrap_x = int64_t(Params.PeriodX) * 2 * m_ratio;
         m_wrap_y = int64_t(Params.PeriodY) * 2 * m_ratio;
      }
      else {
         m_wrap_x = Params.PeriodX;
         m_wrap_y = Params.PeriodY;
      }

      if ((Params.Layout IS paving_layout::RUNNING) and (Params.PeriodY > 0)) {
         m_row_shift = std::llround(double(Params.PeriodY) * Params.RowOffset);
      }
   }

   const paving_params & params() const { return m_params; }
   double length() const { return m_length; }
   double thickness() const { return m_thickness; }
   int ratio() const { return m_ratio; }

   // The pattern-unit translations over which a periodic paving repeats, or zero on an aperiodic axis.

   void period_vector(double &X, double &Y) const {
      double cell_x, cell_y;
      switch (m_params.Layout) {
         case paving_layout::BASKET_WEAVE: cell_x = cell_y = m_length; break;
         case paving_layout::HERRINGBONE:  cell_x = cell_y = 2.0 * m_length; break;
         default: cell_x = m_length; cell_y = m_thickness; break;
      }
      X = double(m_params.PeriodX) * cell_x;
      Y = double(m_params.PeriodY) * cell_y;
   }

   // Returns true if every pattern position within the given bounds keeps exact element indices.

   bool lattice_fits(double MinX, double MinY, double MaxX, double MaxY) const {
      const double ex = std::max(std::abs(MinX), std::abs(MaxX));
      const double ey = std::max(std::abs(MinY), std::abs(MaxY));
      const double unit = std::min(m_length, m_thickness);
      double extent = std::max(ex, ey) / unit;
      if (m_params.Layout IS paving_layout::RUNNING) {
         extent = std::max(extent, (ex + ((ey / m_thickness) * std::abs(m_params.RowOffset) * m_length)) / m_length);
      }
      return std::isfinite(extent) and (extent < PAVING_LATTICE_LIMIT);
   }

   paving_sample locate(double QX, double QY) const {
      return paving_sample_point(m_params.Layout, m_length, m_thickness, m_ratio, m_params.RowOffset, QX, QY);
   }

   // The seeded hash of the element, with its key wrapped to the period so that repeats share appearance.

   uint64_t element_key(const paving_sample &Sample) const {
      int64_t x = Sample.KeyX, y = Sample.KeyY;
      if (m_wrap_y > 0) {
         const int64_t turns = paving_floor_div(y, m_wrap_y);
         y -= turns * m_wrap_y;
         x += turns * m_row_shift;
      }
      if (m_wrap_x > 0) x = paving_floor_mod(x, m_wrap_x);
      return paving_element_key(m_params.Seed, x, y, Sample.KeyZ);
   }

   paving_variation variation(uint64_t Key) const {
      const double hue_unit = cellular_hash(Key, 1);
      return {
         .Id      = cellular_hash(Key, 0),
         .Hue     = ((hue_unit * 2.0) - 1.0) * m_params.HueVariation,
         .Chroma  = ((cellular_hash(Key, 2) * 2.0) - 1.0) * m_params.ChromaVariation,
         .Light   = ((cellular_hash(Key, 3) * 2.0) - 1.0) * m_params.LightVariation,
         .Height  = ((cellular_hash(Key, 4) * 2.0) - 1.0) * m_params.HeightVariation,
         .HueUnit = hue_unit
      };
   }

   // Mitered distance from the joint edge into the element; zero or less inside the joint.

   double interior(const paving_sample &Sample) const {
      const double dx = std::min(Sample.LX, m_length - Sample.LX);
      const double dy = std::min(Sample.LY, m_thickness - Sample.LY);
      return std::min(dx, dy) - m_half_joint;
   }

   // Limit the footprint to one shortest element dimension per axis.  Every rectangle it overlaps then
   // contains at least one footprint corner, even at a staggered joint or where the orientation changes.

   std::pair<double, double> coverage_radius() const {
      const double x = std::min(m_aa_x, std::min(m_length, m_thickness));
      const double y = std::min(m_aa_y, std::min(m_length, m_thickness));
      if ((m_half_joint <= 0) or (m_params.JointWidth >= std::max(x, y))) return { 0, 0 };
      return { x * 0.5, y * 0.5 };
   }

   // Report each contributing element and its coverage.  Wide joints only need the element at the pixel centre.

   template<class Visitor>
   double visit_coverage(const paving_sample &Sample, Visitor Visit) const {
      if (m_params.JointWidth <= 0) {
         Visit(Sample, 1.0);
         return 1.0;
      }
      if (m_solid_joint) return 0.0;

      const auto [rx, ry] = coverage_radius();
      const double dx = std::min(Sample.LX, m_length - Sample.LX);
      const double dy = std::min(Sample.LY, m_thickness - Sample.LY);
      const double ru = Sample.Vertical ? ry : rx;
      const double rv = Sample.Vertical ? rx : ry;
      if (rx > 0) {
         if ((dx - m_half_joint >= ru) and (dy - m_half_joint >= rv)) {
            Visit(Sample, 1.0);
            return 1.0;
         }

         auto overlap = [this](double Centre, double Size, double Radius) {
            const double first = std::max((m_half_joint - Centre) / Radius, -1.0);
            const double last = std::min(((Size - Centre) - m_half_joint) / Radius, 1.0);
            return std::max(0.0, (last - first) * 0.5);
         };

         if ((dx + m_half_joint >= ru) and (dy + m_half_joint >= rv)) {
            const double coverage = overlap(Sample.LX, m_length, ru) * overlap(Sample.LY, m_thickness, rv);
            if (coverage > 0.0) Visit(Sample, coverage);
            return coverage;
         }

         paving_sample seen[4];
         int count = 0;
         double coverage = 0.0;
         for (double y : { -ry, ry }) {
            for (double x : { -rx, rx }) {
               auto element = locate(Sample.QX + x, Sample.QY + y);
               auto same = [&](const paving_sample &Other) {
                  return (Other.KeyX IS element.KeyX) and (Other.KeyY IS element.KeyY) and
                     (Other.KeyZ IS element.KeyZ);
               };
               if (std::any_of(seen, seen + count, same)) continue;
               seen[count++] = element;

               // Move from the corner that found this element to the pixel centre in its local coordinates.

               const double u = element.LX - (element.Vertical ? y : x);
               const double v = element.LY - (element.Vertical ? x : y);
               const double weight = overlap(u, m_length, element.Vertical ? ry : rx) *
                  overlap(v, m_thickness, element.Vertical ? rx : ry);
               if (weight <= 0.0) continue;
               element.U = std::clamp(u / m_length, 0.0, 1.0);
               element.V = std::clamp(v / m_thickness, 0.0, 1.0);
               Visit(element, weight);
               coverage += weight;
            }
         }
         return std::min(coverage, 1.0);
      }

      const double aa_u = Sample.Vertical ? m_aa_y : m_aa_x;
      const double aa_v = Sample.Vertical ? m_aa_x : m_aa_y;
      const double u = std::clamp(((dx - m_half_joint) / aa_u) + 0.5, 0.0, 1.0);
      const double v = std::clamp(((dy - m_half_joint) / aa_v) + 0.5, 0.0, 1.0);
      const double coverage = std::min(u, v);
      if (coverage > 0.0) Visit(Sample, coverage);
      return coverage;
   }

   double coverage(const paving_sample &Sample) const {
      return visit_coverage(Sample, [](const paving_sample &, double) {});
   }

   // Bevelled height.  HeightOffset is the element's seeded variation.Height.

   double height(const paving_sample &Sample, double HeightOffset) const {
      const double s = interior(Sample);
      if (s <= 0) return m_params.JointHeightValue;
      const double element = m_params.ElementHeightValue + HeightOffset;
      if (m_params.BevelWidth <= 0) return element;
      const double t = paving_ease(m_params.BevelProfile, std::min(s / m_params.BevelWidth, 1.0));
      return std::lerp(m_params.JointHeightValue, element, t);
   }

   // Discrete element ID: the element hash inside elements, zero in joints.

   double id(const paving_sample &Sample, double ElementId) const {
      if (m_solid_joint and (m_params.JointWidth > 0)) return 0.0;
      return (interior(Sample) >= 0) ? ElementId : 0.0;
   }
};
