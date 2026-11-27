// Seeded cellular (Worley) noise over an unbounded jittered lattice, as used by CellularFX.
//
// Every integer cell (i, j) holds one feature, whose position, height and cell value are derived from a hash of the
// octave seed and the cell index.  Fields are evaluated per sample by an exact nearest-feature search, so the result
// depends only on the sampled lattice position and never on the extent of the area being drawn.
//
// All positions are in base lattice units, in which one base cell measures 1 x 1.  Mapping a filter primitive into
// lattice units is the caller's responsibility.

#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <kotuku/modules/vector.h>

// Field modes share their values with WLF so that WorleyMode can be passed through unchanged.  CELL returns the
// seeded value of the nearest feature, which is constant within each cell.

enum class cellular_mode : int { F1 = 0, F2_F1 = 1, F2 = 2, WEIGHTED = 3, PEAKS = 4, CELL = 5 };

static_assert(int(cellular_mode::F1) IS int(WLF::F1));
static_assert(int(cellular_mode::F2_F1) IS int(WLF::F2_F1));
static_assert(int(cellular_mode::F2) IS int(WLF::F2));
static_assert(int(cellular_mode::WEIGHTED) IS int(WLF::WEIGHTED));
static_assert(int(cellular_mode::PEAKS) IS int(WLF::PEAKS));
static_assert(int(cellular_mode::CELL) IS int(WLF::CELL));

constexpr int CELLULAR_MAX_OCTAVES = 12;
constexpr int CELLULAR_MAX_PERIOD = 1<<20;
constexpr double CELLULAR_MAX_HEIGHT = 4.0;

// Cell indices stay exact while lattice positions remain below 2^52.

constexpr double CELLULAR_LATTICE_LIMIT = 4503599627370496.0;

// The search never needs to visit more rings than this while heights lie within +/- CELLULAR_MAX_HEIGHT.  The
// worst case is WEIGHTED or PEAKS under the Manhattan metric with the full height range, which completes in ring 10.

constexpr int CELLULAR_MAX_RINGS = 12;

struct cellular_params {
   cellular_mode Mode = cellular_mode::F1;
   WLM Metric = WLM::EUCLIDEAN;
   int64_t Seed = 0;
   double Jitter = 1.0;      // [0, 1]; 0 places every feature at its cell centre
   double HeightMin = 0.0;   // [-4, 4]; swapped with HeightMax if greater
   double HeightMax = 1.0;   // [-4, 4]
   int Octaves = 1;          // [1, 12]
   double Persistence = 0.5; // [0, 1]
   double Lacunarity = 2.0;  // [1, 8]
   int PeriodX = 0;          // Horizontal repeat in base cells; 0 disables
   int PeriodY = 0;          // Vertical repeat in base cells; 0 disables
};

struct cellular_feature {
   double X, Y;   // Lattice position, always within the feature's own cell
   double Height; // In [HeightMin, HeightMax]
   double Value;  // In [0, 1)
};

// Per-octave state.  A period of zero disables wrapping on that axis.

struct cellular_layer {
   uint64_t Seed;
   double Frequency;
   double Amplitude;
   int64_t PeriodX, PeriodY;
};

//********************************************************************************************************************
// The splitmix64 finaliser.  Successive splitmix64 outputs from a state S are cellular_mix(S + n * golden), n >= 1.

constexpr uint64_t CELLULAR_GOLDEN = 0x9e3779b97f4a7c15ULL;

inline uint64_t cellular_mix(uint64_t Z)
{
   Z = (Z ^ (Z >> 30)) * 0xbf58476d1ce4e5b9ULL;
   Z = (Z ^ (Z >> 27)) * 0x94d049bb133111ebULL;
   return Z ^ (Z >> 31);
}

// Converts a hash to a uniform double in [0, 1).

inline double cellular_unit(uint64_t Hash)
{
   return double(Hash >> 11) * (1.0 / 9007199254740992.0);
}

inline uint64_t cellular_octave_seed(int64_t Seed, int Octave)
{
   return cellular_mix(uint64_t(Seed) + (uint64_t(Octave) + 1) * CELLULAR_GOLDEN);
}

inline uint64_t cellular_cell_key(uint64_t Seed, int64_t I, int64_t J)
{
   return cellular_mix(cellular_mix(Seed + uint64_t(I) * 0xd1b54a32d192ed03ULL) + uint64_t(J) * 0xaef17502108ef2d9ULL);
}

// Output N (0-based) of the splitmix64 sequence seeded by Key.

inline double cellular_hash(uint64_t Key, int N)
{
   return cellular_unit(cellular_mix(Key + (uint64_t(N) + 1) * CELLULAR_GOLDEN));
}

// Positive modulo for periodic cell keys.

inline int64_t cellular_wrap(int64_t Index, int64_t Period)
{
   if (Period <= 0) return Index;
   const int64_t result = Index % Period;
   return (result < 0) ? result + Period : result;
}

//********************************************************************************************************************
// Distance metrics.  Dx and Dy must be non-negative.

template <WLM Metric> inline double cellular_distance(double Dx, double Dy)
{
   if constexpr (Metric IS WLM::MANHATTAN) return Dx + Dy;
   else if constexpr (Metric IS WLM::CHEBYSHEV) return (Dx > Dy) ? Dx : Dy;
   else return std::sqrt((Dx * Dx) + (Dy * Dy));
}

inline double cellular_distance(WLM Metric, double Dx, double Dy)
{
   if (Metric IS WLM::MANHATTAN) return cellular_distance<WLM::MANHATTAN>(Dx, Dy);
   else if (Metric IS WLM::CHEBYSHEV) return cellular_distance<WLM::CHEBYSHEV>(Dx, Dy);
   else return cellular_distance<WLM::EUCLIDEAN>(Dx, Dy);
}

//********************************************************************************************************************
// Checks combinations of parameters that individual field setters cannot.  Ranges of single values are the
// responsibility of the setters.  Rotation of a periodic domain is not represented here and must be checked by the
// caller.

inline ERR cellular_check(const cellular_params &Params)
{
   if ((Params.Mode IS cellular_mode::CELL) and (Params.Octaves > 1)) return ERR::InvalidValue;

   if (((Params.PeriodX > 0) or (Params.PeriodY > 0)) and (Params.Octaves > 1)) {
      if (Params.Lacunarity != std::trunc(Params.Lacunarity)) return ERR::InvalidValue;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

class cellular_sampler {
   using eval_fn = double (*)(const cellular_sampler &, const cellular_layer &, double, double);

   cellular_params m_params;
   cellular_layer m_layers[CELLULAR_MAX_OCTAVES];
   int m_octaves;
   double m_height_min, m_height_max;
   double m_half_jitter;
   double m_amplitude_scale;
   eval_fn m_eval;

   // Lower bound of the distance from (X, Y) to any feature of cell (I, J), using the box that contains every
   // possible feature position.

   template <WLM Metric> double cell_bound(double X, double Y, int64_t I, int64_t J) const {
      const double cx = double(I) + 0.5;
      const double cy = double(J) + 0.5;
      const double gx = std::max(0.0, std::abs(X - cx) - m_half_jitter);
      const double gy = std::max(0.0, std::abs(Y - cy) - m_half_jitter);
      return cellular_distance<Metric>(gx, gy);
   }

   // Lower bound of the distance from a sample in cell (I0, J0) to any feature in ring Ring or beyond.  Every metric
   // is at least the Chebyshev distance, which is at least the gap on the axis that is Ring cells away.

   double ring_bound(int Ring) const {
      return double(Ring) - 0.5 - m_half_jitter;
   }

   // Calls Visit(I, J) for every cell in the square ring Ring cells away from (I0, J0).

   template <class T> static void visit_ring(int64_t I0, int64_t J0, int Ring, T &&Visit) {
      if (Ring IS 0) {
         Visit(I0, J0);
         return;
      }

      for (int64_t i = I0 - Ring; i <= I0 + Ring; i++) {
         Visit(i, J0 - Ring);
         Visit(i, J0 + Ring);
      }

      for (int64_t j = J0 - Ring + 1; j < J0 + Ring; j++) {
         Visit(I0 - Ring, j);
         Visit(I0 + Ring, j);
      }
   }

   template <WLM Metric, cellular_mode Mode>
   static double evaluate(const cellular_sampler &Self, const cellular_layer &Layer, double X, double Y) {
      const auto i0 = int64_t(std::floor(X));
      const auto j0 = int64_t(std::floor(Y));

      [[maybe_unused]] double f1 = std::numeric_limits<double>::infinity();
      [[maybe_unused]] double f2 = std::numeric_limits<double>::infinity();
      [[maybe_unused]] double best;
      [[maybe_unused]] uint64_t best_key = 0;

      if constexpr (Mode IS cellular_mode::PEAKS) best = -std::numeric_limits<double>::infinity();
      else best = std::numeric_limits<double>::infinity();

      // The smallest lower bound at which a cell can no longer change the result.

      auto threshold = [&]() -> double {
         if constexpr ((Mode IS cellular_mode::F1) or (Mode IS cellular_mode::CELL)) return f1;
         else if constexpr ((Mode IS cellular_mode::F2) or (Mode IS cellular_mode::F2_F1)) return f2;
         else if constexpr (Mode IS cellular_mode::WEIGHTED) return best + Self.m_height_max;
         else return Self.m_height_max - best;
      };

      auto visit = [&](int64_t I, int64_t J) {
         if (Self.cell_bound<Metric>(X, Y, I, J) >= threshold()) return;

         const uint64_t key = cellular_cell_key(Layer.Seed, cellular_wrap(I, Layer.PeriodX),
            cellular_wrap(J, Layer.PeriodY));
         const double fx = double(I) + 0.5 + Self.m_params.Jitter * (cellular_hash(key, 0) - 0.5);
         const double fy = double(J) + 0.5 + Self.m_params.Jitter * (cellular_hash(key, 1) - 0.5);
         const double dist = cellular_distance<Metric>(std::abs(X - fx), std::abs(Y - fy));

         if constexpr ((Mode IS cellular_mode::WEIGHTED) or (Mode IS cellular_mode::PEAKS)) {
            const double height = Self.m_height_min +
               ((Self.m_height_max - Self.m_height_min) * cellular_hash(key, 2));
            if constexpr (Mode IS cellular_mode::WEIGHTED) best = std::min(best, dist - height);
            else best = std::max(best, height - dist);
         }
         else if (dist < f1) {
            f2 = f1;
            f1 = dist;
            if constexpr (Mode IS cellular_mode::CELL) best_key = key;
         }
         else if (dist < f2) f2 = dist;
      };

      int ring = 0;
      do {
         visit_ring(i0, j0, ring, visit);
         ring++;
         assert(ring < CELLULAR_MAX_RINGS);
      } while ((Self.ring_bound(ring) < threshold()) and (ring < CELLULAR_MAX_RINGS));

      if constexpr (Mode IS cellular_mode::F1) return f1;
      else if constexpr (Mode IS cellular_mode::F2) return f2;
      else if constexpr (Mode IS cellular_mode::F2_F1) return f2 - f1;
      else if constexpr (Mode IS cellular_mode::CELL) return cellular_hash(best_key, 3);
      else return best;
   }

   template <WLM Metric> static eval_fn select_mode(cellular_mode Mode) {
      switch (Mode) {
         case cellular_mode::F2_F1:    return &evaluate<Metric, cellular_mode::F2_F1>;
         case cellular_mode::F2:       return &evaluate<Metric, cellular_mode::F2>;
         case cellular_mode::WEIGHTED: return &evaluate<Metric, cellular_mode::WEIGHTED>;
         case cellular_mode::PEAKS:    return &evaluate<Metric, cellular_mode::PEAKS>;
         case cellular_mode::CELL:     return &evaluate<Metric, cellular_mode::CELL>;
         default:                      return &evaluate<Metric, cellular_mode::F1>;
      }
   }

public:
   // Params must satisfy the documented ranges and cellular_check().  Out-of-range octave counts are clamped.

   explicit cellular_sampler(const cellular_params &Params) : m_params(Params) {
      m_octaves = std::clamp(Params.Octaves, 1, CELLULAR_MAX_OCTAVES);
      m_height_min = std::min(Params.HeightMin, Params.HeightMax);
      m_height_max = std::max(Params.HeightMin, Params.HeightMax);
      m_half_jitter = Params.Jitter * 0.5;

      double frequency = 1.0;
      double amplitude = 1.0;
      double total = 0;
      for (int k=0; k < m_octaves; k++) {
         // Integer lacunarity is required for periodic octaves above 0, so the period products are exact.
         const double scale = std::pow(Params.Lacunarity, k);
         m_layers[k] = {
            .Seed      = cellular_octave_seed(Params.Seed, k),
            .Frequency = frequency,
            .Amplitude = amplitude,
            .PeriodX   = (Params.PeriodX > 0) ? int64_t(std::llround(double(Params.PeriodX) * scale)) : 0,
            .PeriodY   = (Params.PeriodY > 0) ? int64_t(std::llround(double(Params.PeriodY) * scale)) : 0
         };
         total += amplitude;
         frequency *= Params.Lacunarity;
         amplitude *= Params.Persistence;
      }
      m_amplitude_scale = 1.0 / total;

      if (Params.Metric IS WLM::MANHATTAN) m_eval = select_mode<WLM::MANHATTAN>(Params.Mode);
      else if (Params.Metric IS WLM::CHEBYSHEV) m_eval = select_mode<WLM::CHEBYSHEV>(Params.Mode);
      else m_eval = select_mode<WLM::EUCLIDEAN>(Params.Mode);
   }

   const cellular_params & params() const { return m_params; }
   int octaves() const { return m_octaves; }
   const cellular_layer & layer(int Octave) const { return m_layers[Octave]; }

   // Returns true if every lattice position within Extent base units of the origin keeps exact cell indices at the
   // highest octave.

   bool lattice_fits(double Extent) const {
      const double extent = std::abs(Extent) * std::pow(m_params.Lacunarity, m_octaves - 1);
      return std::isfinite(extent) and (extent < CELLULAR_LATTICE_LIMIT);
   }

   // The feature of cell (I, J) in octave-local lattice units.

   cellular_feature feature(int Octave, int64_t I, int64_t J) const {
      const auto &layer = m_layers[Octave];
      const uint64_t key = cellular_cell_key(layer.Seed, cellular_wrap(I, layer.PeriodX),
         cellular_wrap(J, layer.PeriodY));
      return {
         .X      = double(I) + 0.5 + m_params.Jitter * (cellular_hash(key, 0) - 0.5),
         .Y      = double(J) + 0.5 + m_params.Jitter * (cellular_hash(key, 1) - 0.5),
         .Height = m_height_min + ((m_height_max - m_height_min) * cellular_hash(key, 2)),
         .Value  = cellular_hash(key, 3)
      };
   }

   // The raw field value of one octave at base lattice position (X, Y).

   double sample_octave(int Octave, double X, double Y) const {
      const auto &layer = m_layers[Octave];
      return m_eval(*this, layer, X * layer.Frequency, Y * layer.Frequency);
   }

   // The amplitude-weighted mean of all octaves at base lattice position (X, Y).

   double sample(double X, double Y) const {
      if (m_octaves IS 1) return m_eval(*this, m_layers[0], X, Y);

      double total = 0;
      for (int k=0; k < m_octaves; k++) {
         const auto &layer = m_layers[k];
         if (layer.Amplitude IS 0.0) break;
         total += layer.Amplitude * m_eval(*this, layer, X * layer.Frequency, Y * layer.Frequency);
      }
      return total * m_amplitude_scale;
   }
};
