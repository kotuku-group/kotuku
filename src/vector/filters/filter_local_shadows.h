// Shadows of a positioned (point or spot) light over a height field.  The header has no framework dependencies so
// that the unit test can compare the sweep with an independent ray march.
//
// Coordinates are in filter pixels: the sample of column X and row Y lies at (X, Y), and heights are in the same
// vertical units as the light's height.  The distant sweep in filter_lighting_enhanced.cpp carries the height of the
// shadow volume along parallel rays.  Here the rays converge on the light, so the volume is carried along the line from
// the light through the upstream envelope.  For a receiver at horizontal distance r from the light, whose upstream
// point at distance r_up holds the envelope S_up, the envelope projected to the receiver is
//
//    P = S_up + (S_up - Lz) * (r - r_up) / r_up
//
// The receiver is lit if P does not rise above its own height h, and max(h, P) is carried downstream.  The upstream
// point is one grid interval towards the light on the dominant axis, so (r - r_up) / r_up is 1 / (d - 1) where d is
// the receiver's distance from the light along that axis, and the envelope there is interpolated between the two
// samples that bracket the ray on the upstream grid line.
//
// Pixels are divided into four regions about the light's projection.  A pixel belongs to an X-major region if
// |dy| <= |dx|, which gives the diagonals to the X-major regions, and to an X-major region if it lies exactly at the
// projection.  Each region is visited one grid line at a time, moving away from the light.  Every sample that a pixel
// reads is strictly nearer to the light in Chebyshev distance, possibly in a neighbouring region when the light lies
// between pixel centres, so visiting lines in order of distance across all regions satisfies every dependency.
//
// A receiver whose upstream line lies at or beyond the light, or whose upstream point lies outside the cells of the
// height field, sees the light directly: there is nothing between it and the light that the field describes.
// Where one of the two bracketing samples lies outside the field, the edge sample is used for both.
//
// Several light heights are tracked so that the edge of the shadow can be softened in the same way as the distant
// sweep softens it over a range of elevations.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#ifndef IS
#define IS ==
#endif

#if (defined(__SSE2__) or defined(_M_X64)) and (not defined(LOCAL_SHADOWS_SCALAR))
#define LOCAL_SHADOWS_SSE 1
#include <emmintrin.h>
#endif

namespace local_shadows {

constexpr int LEVELS = 4;

struct light {
   double x, y;           // Projection of the light onto the field, in pixels
   std::array<float, LEVELS> z; // Ascending light heights; identical for a hard shadow
};

// The sweep orders lines by their distance from the light.  Keep enough precision for adjacent lines to have
// distinct distances, including when comparing them across a diagonal.

inline bool valid_projection(double X, double Y)
{
   constexpr double LIMIT = 0x1p51;
   return (std::abs(X) < LIMIT) and (std::abs(Y) < LIMIT);
}

// Light heights for a penumbra of Penumbra radians, sampled as the distant sweep samples elevations.  The heights are
// chosen at an anchor so that a receiver there sees the same elevations as it would from a distant light at the same
// elevation.  The penumbra is limited to half of the elevation, so that a flat surface at the anchor's height is never
// shadowed.  Above 45 degrees the anchor's horizontal distance is taken as the light's height above it, so that the
// heights remain finite and vary smoothly when the light passes over the anchor.  Lift raises the elevation seen at
// the anchor by that many radians before the penumbra is applied, as the distant sweep raises a light for wrapped
// faces.

inline double anchor_distance(double X, double Y, double Z, double AnchorX, double AnchorY, double AnchorZ)
{
   return std::max(std::hypot(X - AnchorX, Y - AnchorY), Z - AnchorZ);
}

// The light's elevation at the anchor in radians, as penumbra_light() measures it.  Zero or less if the light is not
// above the anchor.

inline double anchor_elevation(double X, double Y, double Z, double AnchorX, double AnchorY, double AnchorZ)
{
   const double rise = Z - AnchorZ;
   if (rise <= 0) return 0;
   return std::atan2(rise, anchor_distance(X, Y, Z, AnchorX, AnchorY, AnchorZ));
}

inline light penumbra_light(double X, double Y, double Z, double AnchorX, double AnchorY, double AnchorZ,
   double Penumbra, double Lift = 0)
{
   light result { X, Y, { float(Z), float(Z), float(Z), float(Z) } };
   const double rise = Z - AnchorZ;
   if ((rise <= 0) or ((Penumbra <= 0) and (Lift <= 0))) return result;

   const double distance = anchor_distance(X, Y, Z, AnchorX, AnchorY, AnchorZ);
   const double elevation = std::atan2(rise, distance) + Lift;
   const double penumbra = std::clamp(Penumbra, 0.0, elevation * 0.5);
   constexpr double CAP = 89.5 * 3.14159265358979323846 / 180.0;
   for (int k=0; k < LEVELS; ++k) {
      const double e = std::min(elevation - penumbra + (2.0 * penumbra * k / (LEVELS - 1)), CAP);
      result.z[k] = float(Z + ((std::tan(e) * distance) - rise));
   }
   return result;
}

// Elevations in radians that a receiver at (X, Y, Z) sees for each light height, for recording the angular width of
// the penumbra.

inline std::array<double, LEVELS> elevations(const light &Light, double X, double Y, double Z)
{
   std::array<double, LEVELS> result;
   const double distance = std::hypot(Light.x - X, Light.y - Y);
   for (int k=0; k < LEVELS; ++k) result[k] = std::atan2(double(Light.z[k]) - Z, distance);
   return result;
}

// A region of pixels about the light.  The along axis is the dominant one and Sign is the direction away from the
// light.  Lines are the grid lines across the along axis, visited in order of their distance from the light.  A band
// is a range of cross positions within the region, from Lo to Hi inclusive.

struct region {
   bool x_major;
   int sign;
   int along_n, cross_n;     // Extent of the field on each axis
   double la, lc;            // Light position on each axis
   int first, last;          // First and last line in the field, in visiting order, or none if line_count() <= 0

   // Distance from the light along the dominant axis of line A

   double distance(int A) const { return sign * (A - la); }

   // True if cross position C of a line at distance D belongs to the region.  The diagonals belong to the X-major
   // regions.  owner() makes the same comparisons.

   bool member(int C, double D) const {
      return x_major ? (std::abs(C - lc) <= D) : (std::abs(C - lc) < D);
   }

   // Cross range of line A that belongs to this region, clipped to the field and to Lo - Hi

   bool span(int A, int Lo, int Hi, int &C0, int &C1) const {
      const double d = distance(A);
      if (d < 0) return false;
      int c0 = int(std::clamp(std::floor(lc - d) - 1, double(Lo), double(Hi) + 1));
      int c1 = int(std::clamp(std::ceil(lc + d) + 1, double(Lo) - 1, double(Hi)));
      while ((c0 <= c1) and (not member(c0, d))) c0++;
      while ((c1 >= c0) and (not member(c1, d))) c1--;
      C0 = c0;
      C1 = c1;
      return c0 <= c1;
   }

   int line_index(int A) const { return sign * (A - first); }
};

inline int line_count(const region &R)
{
   return (R.sign > 0) ? R.last - R.first + 1 : R.first - R.last + 1;
}

// The four regions for a field of Width x Height: X-major towards +X and -X, then Y-major towards +Y and -Y.  Lines
// that hold no pixel of the region are trimmed from each end.

inline std::array<region, 4> make_regions(int Width, int Height, double LX, double LY)
{
   std::array<region, 4> result;
   int n = 0;
   for (int axis=0; axis < 2; ++axis) {
      const bool x_major = axis IS 0;
      const int along_n = x_major ? Width : Height, cross_n = x_major ? Height : Width;
      const double la = x_major ? LX : LY, lc = x_major ? LY : LX;
      for (int sign : { 1, -1 }) {
         region r { x_major, sign, along_n, cross_n, la, lc, 0, -1 };

         // The X-major region on the positive side includes the line through the light; every other region starts
         // strictly beyond it.

         int first, last;
         if (sign > 0) {
            const double start = x_major ? std::ceil(la) : std::floor(la) + 1;
            first = int(std::clamp(start, 0.0, double(along_n)));
            last = along_n - 1;
         }
         else {
            first = int(std::clamp(std::ceil(la) - 1, -1.0, double(along_n) - 1));
            last = 0;
         }

         int c0, c1;
         auto empty = [&](int A) { return not r.span(A, 0, cross_n - 1, c0, c1); };
         if (sign > 0) {
            while ((first <= last) and empty(first)) first++;
            while ((first <= last) and empty(last)) last--;
         }
         else {
            while ((first >= last) and empty(first)) first--;
            while ((first >= last) and empty(last)) last++;
         }
         r.first = first;
         r.last = last;
         result[n++] = r;
      }
   }
   return result;
}

// The region that owns pixel (X, Y), by the rule of region::member()

inline int owner(double LX, double LY, int X, int Y)
{
   const double dx = std::abs(X - LX), dy = std::abs(Y - LY);
   if (dy <= dx) return (X - LX >= 0) ? 0 : 1;
   return (Y - LY > 0) ? 2 : 3;
}

// The field and output

struct field {
   const float *heights; // Height of pixel (X, Y) at heights[Y * stride + X]
   std::ptrdiff_t stride;
   int width, height;
   float *output;        // width * height, the fraction of light reaching each pixel
};

//********************************************************************************************************************
// Arithmetic on the four light heights at once.  The SSE and scalar versions give identical results.

// Interpolates the upstream envelopes U0 and U1 by W, projects them to the receiver of height H, stores the
// receiver's envelopes in V and returns the fraction of light that reaches it.

static inline float project(const float *U0, const float *U1, float W, float Inv, const float *LZ, float H, float *V)
{
#ifdef LOCAL_SHADOWS_SSE
   const __m128 u0 = _mm_loadu_ps(U0), u1 = _mm_loadu_ps(U1);
   const __m128 s = _mm_add_ps(u0, _mm_mul_ps(_mm_set1_ps(W), _mm_sub_ps(u1, u0)));
   const __m128 p = _mm_add_ps(s, _mm_mul_ps(_mm_sub_ps(s, _mm_loadu_ps(LZ)), _mm_set1_ps(Inv)));
   const __m128 h = _mm_set1_ps(H);
   _mm_storeu_ps(V, _mm_max_ps(h, p));

   // Clearance c = h - p.  Each pair of heights contributes clamp(-c[k] / max(c[k+1] - c[k], 1e-6), 0, 1).  The
   // clearance rises with the light's height, so if the lowest light clears the envelope, every term is zero and the
   // result is exactly 1.

   const __m128 c = _mm_sub_ps(h, p);
   if (_mm_cvtss_f32(c) >= 0.0f) return 1.0f;
   const __m128 next = _mm_shuffle_ps(c, c, _MM_SHUFFLE(3, 3, 2, 1));
   const __m128 den = _mm_max_ps(_mm_sub_ps(next, c), _mm_set1_ps(1e-6f));
   const __m128 q = _mm_min_ps(_mm_max_ps(_mm_div_ps(_mm_sub_ps(_mm_setzero_ps(), c), den), _mm_setzero_ps()),
      _mm_set1_ps(1.0f));
   alignas(16) float terms[4];
   _mm_store_ps(terms, q);
   const float t = (terms[0] + terms[1] + terms[2]) * (1.0f / (LEVELS - 1));
#else
   float c[LEVELS];
   for (int k=0; k < LEVELS; ++k) {
      const float s = U0[k] + (W * (U1[k] - U0[k]));
      const float p = s + ((s - LZ[k]) * Inv);
      V[k] = std::max(H, p);
      c[k] = H - p;
   }
   const float t = (std::min(std::max(-c[0] / std::max(c[1] - c[0], 1e-6f), 0.0f), 1.0f) +
      std::min(std::max(-c[1] / std::max(c[2] - c[1], 1e-6f), 0.0f), 1.0f) +
      std::min(std::max(-c[2] / std::max(c[3] - c[2], 1e-6f), 0.0f), 1.0f)) * (1.0f / (LEVELS - 1));
#endif
   return 1.0f - (t * t * (3.0f - 2.0f * t));
}

// project() for four receivers at once.  The envelopes of each receiver are computed as in project(), and the four
// clearances are transposed so that the blend runs across the receivers.  Every element sees the same operations in
// the same order as in project(), so the results are identical.

static inline void project4(const float *const *U0, const float *W, float Inv, const float *LZ, const float *H,
   float *const *V, float *Light)
{
#ifdef LOCAL_SHADOWS_SSE
   const __m128 z = _mm_loadu_ps(LZ), inv = _mm_set1_ps(Inv);
   __m128 c[4];
   for (int j=0; j < 4; ++j) {
      const __m128 u0 = _mm_loadu_ps(U0[j]), u1 = _mm_loadu_ps(U0[j] + LEVELS);
      const __m128 s = _mm_add_ps(u0, _mm_mul_ps(_mm_set1_ps(W[j]), _mm_sub_ps(u1, u0)));
      const __m128 p = _mm_add_ps(s, _mm_mul_ps(_mm_sub_ps(s, z), inv));
      const __m128 h = _mm_set1_ps(H[j]);
      _mm_storeu_ps(V[j], _mm_max_ps(h, p));
      c[j] = _mm_sub_ps(h, p);
   }
   _MM_TRANSPOSE4_PS(c[0], c[1], c[2], c[3]); // c[k] now holds level k of the four receivers

   const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f), floor = _mm_set1_ps(1e-6f);
   if (_mm_movemask_ps(_mm_cmplt_ps(c[0], zero)) IS 0) { // Every receiver is lit
      _mm_storeu_ps(Light, one);
      return;
   }
   __m128 q[3];
   for (int k=0; k < 3; ++k) {
      const __m128 den = _mm_max_ps(_mm_sub_ps(c[k + 1], c[k]), floor);
      q[k] = _mm_min_ps(_mm_max_ps(_mm_div_ps(_mm_sub_ps(zero, c[k]), den), zero), one);
   }
   const __m128 t = _mm_mul_ps(_mm_add_ps(_mm_add_ps(q[0], q[1]), q[2]), _mm_set1_ps(1.0f / (LEVELS - 1)));
   __m128 result = _mm_sub_ps(one, _mm_mul_ps(_mm_mul_ps(t, t), _mm_sub_ps(_mm_set1_ps(3.0f),
      _mm_mul_ps(_mm_set1_ps(2.0f), t))));
   result = _mm_or_ps(_mm_and_ps(_mm_cmpge_ps(c[0], zero), one), _mm_andnot_ps(_mm_cmpge_ps(c[0], zero), result));
   _mm_storeu_ps(Light, result);
#else
   for (int j=0; j < 4; ++j) Light[j] = project(U0[j], U0[j] + LEVELS, W[j], Inv, LZ, H[j], V[j]);
#endif
}

//********************************************************************************************************************
// The sweep.  Each unit is a band of one region and keeps the envelopes of its previous and current line.  After each
// line it publishes the envelopes of the two ends of its span, which are the only samples that other units read: a
// neighbouring band of the same region reads the end beside it, and a perpendicular region reads the end at the
// diagonal.  Then it publishes the distance of its next line.  A reader of a published sample waits until the owner
// has passed it.  Every sample read lies strictly nearer to the light, so a unit with the nearest pending line never
// waits, and the result does not depend on the division into units.

class sweep {
   static constexpr int TILE = 32; // X-major lines are gathered and scattered in tiles, as in the distant sweep

   struct unit {
      int region, band, lo, hi;
      std::vector<float> prev, cur;    // (hi - lo + 1) * LEVELS
      std::vector<float> tile_h, tile_o; // X-major only: TILE lines of (hi - lo + 1)
      std::array<int, TILE> tile_c0{}, tile_c1{};
      int tile_lines = 0, next = 0;
   };

   const field &F;
   const light &L;
   std::array<region, 4> regions;
   std::array<std::vector<int>, 4> band_of;  // Band of each cross position
   std::array<std::vector<int>, 4> band_lo;  // Lowest cross position of each band
   std::array<int, 4> bands { 0, 0, 0, 0 };
   std::array<std::vector<float>, 4> published; // Per line and band: the envelopes of the two span ends
   std::array<std::unique_ptr<std::atomic<double>[]>, 4> progress; // Per band: distance of the next line
   std::vector<unit> units;
   std::vector<std::vector<int>> tasks; // Units of each task

   public:
   sweep(const field &Field, const light &Light) : F(Field), L(Light) {
      regions = make_regions(F.width, F.height, L.x, L.y);
   }

   // Divides the regions into bands and the bands among at most Workers tasks.  Each region is divided in proportion
   // to its share of the pixels, at cross positions that give its bands equal numbers of pixels.  The bands are then
   // assigned, largest first, to the task with the least work, so that a task can hold several small bands.  Returns
   // false on allocation failure.

   bool plan(int Workers) {
      constexpr int MIN_BAND = 16;
      std::array<std::vector<double>, 4> counts; // Pixels at each cross position
      std::array<double, 4> pixels { 0, 0, 0, 0 };
      double total = 0;

      for (int i=0; i < 4; ++i) {
         const auto &r = regions[i];
         if (line_count(r) <= 0) continue;
         std::vector<double> delta(r.cross_n + 1, 0.0);
         for (int a=r.first; ; a += r.sign) {
            int c0, c1;
            if (r.span(a, 0, r.cross_n - 1, c0, c1)) {
               delta[c0] += 1;
               delta[c1 + 1] -= 1;
               pixels[i] += c1 - c0 + 1;
            }
            if (a IS r.last) break;
         }
         counts[i].resize(r.cross_n);
         double running = 0;
         for (int c=0; c < r.cross_n; ++c) counts[i][c] = (running += delta[c]);
         total += pixels[i];
      }

      if (Workers < 1) Workers = 1;
      std::vector<double> work;
      for (int i=0; i < 4; ++i) {
         if (pixels[i] <= 0) continue;
         const auto &r = regions[i];
         int extent_lo = r.cross_n, extent_hi = -1;
         for (int x=0; x < r.cross_n; ++x) {
            if (counts[i][x] > 0) { extent_lo = std::min(extent_lo, x); extent_hi = x; }
         }
         const int extent = extent_hi - extent_lo + 1;
         const int share = (Workers IS 1) ? 1 :
            std::clamp(int(std::lround(Workers * pixels[i] / total)), 1, std::max(1, extent / MIN_BAND));
         bands[i] = share;
         band_of[i].assign(r.cross_n, 0);
         band_lo[i].resize(share);
         published[i].assign(size_t(line_count(r)) * share * 2 * LEVELS, 0.0f);
         progress[i].reset(new (std::nothrow) std::atomic<double>[share]);
         if (not progress[i]) return false;

         // The region's cross extent is divided into bands of equal size.  Neighbouring bands advance together, one
         // line apart at most, so the cost of a region is the sum over its lines of the largest band's share of the
         // line, which equal bands keep small.

         for (int b=0; b < share; ++b) {
            const int b_lo = (b IS 0) ? 0 : extent_lo + int(int64_t(extent) * b / share);
            const int b_hi = (b IS share - 1) ? r.cross_n - 1 : extent_lo + int(int64_t(extent) * (b + 1) / share) - 1;
            double band_pixels = 0;
            for (int x=b_lo; x <= b_hi; ++x) band_pixels += counts[i][x];

            band_lo[i][b] = b_lo;
            for (int x=b_lo; x <= b_hi; ++x) band_of[i][x] = b;
            progress[i][b].store(r.distance(r.first), std::memory_order_relaxed);

            unit u;
            u.region = i;
            u.band = b;
            u.lo = b_lo;
            u.hi = b_hi;
            const size_t n = size_t(b_hi - b_lo + 1);
            u.prev.resize(n * LEVELS);
            u.cur.resize(n * LEVELS);
            if (r.x_major) {
               u.tile_h.resize(n * TILE);
               u.tile_o.resize(n * TILE);
            }
            u.next = r.first;
            units.push_back(std::move(u));
            work.push_back(band_pixels);
         }
      }

      // Longest processing time first

      const int task_count = std::min(Workers, int(units.size()));
      tasks.assign(task_count, { });
      std::vector<double> load(task_count, 0.0);
      std::vector<int> order(units.size());
      for (size_t i=0; i < order.size(); ++i) order[i] = int(i);
      std::stable_sort(order.begin(), order.end(), [&](int A, int B) { return work[A] > work[B]; });
      for (int u : order) {
         const int t = int(std::min_element(load.begin(), load.end()) - load.begin());
         tasks[t].push_back(u);
         load[t] += work[u];
      }
      return true;
   }

   int task_count() const { return int(tasks.size()); }

   // Runs the units of task T on the calling thread, visiting their lines in order of distance.

   void run(int T) {
      const auto &list = tasks[T];
      while (true) {
         int best = -1;
         double nearest = std::numeric_limits<double>::infinity();
         for (int i : list) {
            auto &u = units[i];
            if (not pending(u)) continue;
            const double d = regions[u.region].distance(u.next);
            if (d < nearest) { nearest = d; best = i; }
         }
         if (best < 0) break;
         step(units[best]);
      }
   }

   private:
   bool pending(const unit &U) const {
      const auto &r = regions[U.region];
      return (r.sign > 0) ? U.next <= r.last : U.next >= r.last;
   }

   // The published envelopes of pixel (X, Y), which another unit owns and which lies nearer to the light than the
   // line being processed.

   const float * fetch(int X, int Y) const {
      const int i = owner(L.x, L.y, X, Y);
      const auto &r = regions[i];
      const int a = r.x_major ? X : Y, c = r.x_major ? Y : X;
      const int b = band_of[i][c];
      const double d = r.distance(a);
      while (progress[i][b].load(std::memory_order_acquire) <= d) std::this_thread::yield();
      int c0, c1;
      const int lo = band_lo[i][b], hi = (b + 1 < bands[i]) ? band_lo[i][b + 1] - 1 : r.cross_n - 1;
      r.span(a, lo, hi, c0, c1);
      const int side = (c IS c0) ? 0 : 1;
      return published[i].data() + ((size_t(r.line_index(a)) * bands[i] + b) * 2 + side) * LEVELS;
   }

   void step(unit &U) {
      const auto &r = regions[U.region];
      const int a = U.next;
      const int t = r.x_major ? (r.line_index(a) % TILE) : 0;

      if ((r.x_major) and (t IS 0)) gather(U, a);

      // Both rows start at the first cross position of the band.

      const float *heights;
      float *output;
      if (r.x_major) {
         const int n = U.hi - U.lo + 1;
         heights = U.tile_h.data() + (t * n);
         output = U.tile_o.data() + (t * n);
      }
      else {
         heights = F.heights + (a * F.stride) + U.lo;
         output = F.output + (std::ptrdiff_t(a) * F.width) + U.lo;
      }

      int c0, c1;
      const bool any = r.span(a, U.lo, U.hi, c0, c1);
      if (r.x_major) {
         U.tile_c0[t] = any ? c0 : 1;
         U.tile_c1[t] = any ? c1 : 0;
      }

      if (any) {
         line(U, a, c0, c1, heights, output);
         float *pub = published[U.region].data() + ((size_t(r.line_index(a)) * bands[U.region] + U.band) * 2) * LEVELS;
         std::copy_n(U.cur.data() + (c0 - U.lo) * LEVELS, LEVELS, pub);
         std::copy_n(U.cur.data() + (c1 - U.lo) * LEVELS, LEVELS, pub + LEVELS);
      }
      std::swap(U.prev, U.cur);

      if ((r.x_major) and ((t IS TILE - 1) or (a IS r.last))) scatter(U, a, t + 1);

      U.next += r.sign;
      progress[U.region][U.band].store(pending(U) ? r.distance(U.next) : std::numeric_limits<double>::infinity(),
         std::memory_order_release);
   }

   // Copies the heights of up to TILE X-major lines from A onwards into the unit's tile, by line.  Spans widen with
   // distance from the light, so the span of the tile's last line holds every position that the tile reads.

   void gather(unit &U, int A) {
      const auto &r = regions[U.region];
      const int count = std::min(TILE, r.sign * (r.last - A) + 1);
      const int n = U.hi - U.lo + 1;
      int c0, c1;
      if (not r.span(A + (r.sign * (count - 1)), U.lo, U.hi, c0, c1)) return;
      for (int c=c0; c <= c1; ++c) {
         const float *src = F.heights + (c * F.stride);
         float *dest = U.tile_h.data() + (c - U.lo);
         for (int t=0; t < count; ++t) dest[t * n] = src[A + (t * r.sign)];
      }
   }

   // Writes the output of the Count lines of the tile that ends at line A.  As spans widen with distance, the lines
   // of the tile that hold position C are the last ones.

   void scatter(unit &U, int A, int Count) {
      const auto &r = regions[U.region];
      const int first = A - (r.sign * (Count - 1));
      const int n = U.hi - U.lo + 1;
      const int c_lo = U.tile_c0[Count - 1], c_hi = U.tile_c1[Count - 1];
      for (int c=c_lo; c <= c_hi; ++c) {
         int t0 = 0;
         while ((c < U.tile_c0[t0]) or (c > U.tile_c1[t0])) t0++;
         float *dest = F.output + (std::ptrdiff_t(c) * F.width) + first;
         const float *src = U.tile_o.data() + (c - U.lo);
         for (int t=t0; t < Count; ++t) dest[t * r.sign] = src[t * n];
      }
   }

   // Processes cross positions C0 to C1 of line A.

   void line(unit &U, int A, int C0, int C1, const float *Heights, float *Output) {
      const auto &r = regions[U.region];
      const double d = r.distance(A);
      const int up = A - r.sign;
      float *cur = U.cur.data();

      // Seed: the upstream line lies at or beyond the light, or outside the field

      if ((d <= 1.0) or (up < 0) or (up >= r.along_n)) {
         for (int c=C0; c <= C1; ++c) {
            const int offset = c - U.lo;
            std::fill_n(cur + (offset * LEVELS), LEVELS, Heights[offset]);
            Output[offset] = 1.0f;
         }
         return;
      }

      // Cross positions of the upstream line that this unit holds in its previous line

      int p0, p1;
      if (not r.span(up, U.lo, U.hi, p0, p1)) { p0 = 1; p1 = 0; }
      const float *prev = U.prev.data();
      const double inv_d = 1.0 / d;
      const float inv = float(1.0 / (d - 1.0));
      const double edge_hi = r.cross_n - 0.5;

      // Interior cross positions read both bracketing samples from the previous line, inside the field and strictly
      // nearer to the light, so they need none of the checks of the general case.  Their results are identical to
      // those of the general case, so the division into bands does not change the output.

      const int inner_lo = int(std::clamp(std::ceil(r.lc - d) + 2, 0.0, double(r.cross_n)));
      const int inner_hi = int(std::clamp(std::floor(r.lc + d) - 2, -1.0, double(r.cross_n) - 1));
      const int i_lo = std::max({ C0, p0 + 1, 1, inner_lo });
      // The fast path loads both samples even if the weight rounds to zero at C + 1.
      const int i_hi = std::min({ C1, p1 - 2, r.cross_n - 2, inner_hi });

      auto previous = [&](int C) { return prev + ((C - U.lo) * LEVELS); };

      auto general = [&](int C) {
         const int offset = C - U.lo;
         const float h = Heights[offset];
         float *v = cur + (offset * LEVELS);
         const double cp = C + ((r.lc - C) * inv_d); // The upstream point, within a pixel of C

         if ((cp < -0.5) or (cp > edge_hi)) { // The rest of the ray lies outside the field
            for (int k=0; k < LEVELS; ++k) v[k] = h;
            Output[offset] = 1.0f;
            return;
         }

         // A sample without weight may lie as far from the light as the receiver, so it is not read.  The weight is
         // rounded first: one that rounds to 1 selects the far sample alone.

         double base = std::floor(cp);
         float w = float(cp - base);
         if (w >= 1.0f) {
            base += 1.0;
            w = 0.0f;
         }
         int i0 = std::clamp(int(base), 0, r.cross_n - 1);
         int i1 = (w IS 0.0f) ? i0 : std::clamp(int(base) + 1, 0, r.cross_n - 1);

         // Rounding can place a bracketing sample at the receiver's own distance from the light, where it is not yet
         // complete.  Only samples strictly nearer are read.

         if (std::abs(i1 - r.lc) >= d) { i1 = i0; w = 0.0f; }
         else if (std::abs(i0 - r.lc) >= d) { i0 = i1; w = 0.0f; }

         const float *u0, *u1;
         if ((i0 >= p0) and (i1 <= p1)) {
            u0 = previous(i0);
            u1 = previous(i1);
         }
         else {
            u0 = ((i0 >= p0) and (i0 <= p1)) ? previous(i0) : fetch_cross(r, up, i0);
            u1 = ((i1 >= p0) and (i1 <= p1)) ? previous(i1) : fetch_cross(r, up, i1);
         }
         Output[offset] = project(u0, u1, w, inv, L.z.data(), h, v);
      };

      if (i_lo > i_hi) {
         for (int c=C0; c <= C1; ++c) general(c);
         return;
      }

      for (int c=C0; c < i_lo; ++c) general(c);

      const float *lz = L.z.data();
      const double lc = r.lc;
      auto sample = [&](int C, float &W) -> const float * {
         const double cp = C + ((lc - C) * inv_d);
         double base = std::floor(cp);
         W = float(cp - base);
         if (W >= 1.0f) { // As in the general case; the sample beyond is then unweighted
            base += 1.0;
            W = 0.0f;
         }
         return previous(int(base));
      };

      int c = i_lo;
      for (; c + 3 <= i_hi; c += 4) {
         const int offset = c - U.lo;
         const float *u0[4];
         float w[4], h[4], light[4];
         float *v[4];
         for (int j=0; j < 4; ++j) {
            u0[j] = sample(c + j, w[j]);
            h[j] = Heights[offset + j];
            v[j] = cur + ((offset + j) * LEVELS);
         }
         project4(u0, w, inv, lz, h, v, light);
         std::copy_n(light, 4, Output + offset);
      }
      for (; c <= i_hi; ++c) {
         const int offset = c - U.lo;
         float w;
         const float *u0 = sample(c, w);
         Output[offset] = project(u0, u0 + LEVELS, w, inv, lz, Heights[offset], cur + (offset * LEVELS));
      }

      for (int c=i_hi + 1; c <= C1; ++c) general(c);
   }

   const float * fetch_cross(const region &R, int Along, int Cross) const {
      return R.x_major ? fetch(Along, Cross) : fetch(Cross, Along);
   }
};

// Sweeps every region on the calling thread.

inline bool sweep_serial(const field &F, const light &L)
{
   sweep s(F, L);
   if (not s.plan(1)) return false;
   for (int t=0; t < s.task_count(); ++t) s.run(t);
   return true;
}

// Divides the sweep into at most Workers units and runs them concurrently.  Launch(Count, Function) must start
// Function(I) for each I from 0 to Count - 1 on its own thread and wait for all of them, because a unit can wait on any
// other.  The result is identical to the serial sweep.

template <class LAUNCH>
inline bool sweep_parallel(const field &F, const light &L, int Workers, LAUNCH &&Launch)
{
   sweep s(F, L);
   if (not s.plan(Workers)) return false;
   const int count = s.task_count();
   if (count <= 1) {
      for (int t=0; t < count; ++t) s.run(t);
   }
   else Launch(count, [&s](int I) { s.run(I); });
   return true;
}

} // namespace local_shadows
