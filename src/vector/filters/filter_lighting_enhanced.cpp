/*********************************************************************************************************************

Enhanced lighting, used when the scene's Compliance is VSC::NONE.  The SVG algorithm loses fine detail because its
3x3 Sobel gradient attenuates short features, and 8-bit heights produce terracing on gentle slopes.  This pipeline
replaces the gradient stage only; normals, kernel units and the light models are shared with the SVG path.

1. The alpha channel is converted to a float height buffer, padded with replicated edge values so that no special
   edge kernels are required.
2. Pixels whose 3x3 neighbourhood spans no more than one alpha level are replaced with a Gaussian-weighted mean,
   which removes quantisation terraces without softening real edges.
3. Gradients use a 4th-order central difference with light cross-axis smoothing.  The result is doubled to match the
   slope gain of the SVG interior kernel, so broad shapes are lit identically in both modes.  Where a kernel unit
   covers more than a pixel the stencil blends back to the SVG kernel, because magnified features are already wide
   enough for it and the 4th-order stencil would only amplify the quantisation steps.
4. Concave regions are darkened in proportion to their curvature, measured with an isotropic Laplacian.  This reveals
   creases and pits at the scale of a single pixel, where the gradient is zero at the centre of the feature.  Where a
   kernel unit covers more than a pixel the heights are first blurred to the scale of the kernel unit, so that the
   curvature is measured at the scale on which it is defined rather than on the quantisation steps.
5. Fine detail is given more slope than broad shapes.  A Gaussian base layer is subtracted from the height to give the
   detail, and only the component of its slope that reinforces the existing slope is boosted.  The boost is softly
   compressed so that it cannot tilt the surface far past the light, while the base slope is left exactly as it was.
6. Distant lights cast shadows.  The surface is swept from the side nearest the light, carrying the height of the
   shadow from pixel to pixel, so the cost per pixel does not depend on the length of the shadows.  The edge of the
   shadow is softened over a small angle, so the penumbra widens with distance from the occluder.
7. Specular highlights are anti-aliased with Toksvig's method.  Where the normals within a pixel's neighbourhood vary,
   the highlight is shaded with their mean and a reduced exponent, so that it spreads over the variation instead of
   sparkling.

*********************************************************************************************************************/

constexpr int LIGHT_PAD = 3; // Padding on each side of the height buffer; the gradient stencil reaches 2 px.

// Occlusion is 1 - CAVITY_STRENGTH * curvature for concave regions, limited to CAVITY_FLOOR.  Curvature is the
// Laplacian of the height in client units per kernel unit squared.

constexpr double CAVITY_STRENGTH = 0.25;
constexpr double CAVITY_FLOOR = 0.4;

// Curvature is scaled by the square of the kernel unit's length in pixels, which also scales the Laplacian of the
// 8-bit quantisation steps.  On magnified input that turned each step into a dark speckle.  Where a kernel unit is U
// pixels long, the Laplacian is measured on heights blurred with a sigma of CAVITY_SIGMA * sqrt(U^2 - 1) pixels.  The
// blur is zero at 1:1 and approaches half of a kernel unit as U grows, which smooths steps up to a kernel unit apart.

constexpr double CAVITY_SIGMA = 0.5;
constexpr double MIN_CAVITY_SIGMA = 0.25; // Smaller blurs are skipped

// The 4th-order gradient is used in full where a kernel unit is no longer than SHARPEN_FULL pixels, and is blended
// linearly into the SVG kernel by SHARPEN_NONE pixels.  At that scale the narrowest features already span several
// pixels and the 2nd-order kernel measures them equally well, while the 4th-order kernel would amplify the
// quantisation steps of the magnified heights.

constexpr double SHARPEN_FULL = 1.0;
constexpr double SHARPEN_NONE = 2.0;

// Gradient along the stencil axis at H, where Step is the distance between consecutive taps.  The weights are
// [1, -8, 0, 8, -1] / 12, multiplied by the SVG slope gain of 2.  The outer taps oppose the inner ones, so beside a
// hard edge the stencil reports a slope in the wrong direction and draws a bright or dark halo.  Where the result
// disagrees in sign with the 2nd-order difference [-1, 0, 1] / 2, the 2nd-order value is used instead.  Weight
// blends the result from the 2nd-order difference (0) to the 4th-order difference (1).

static inline float central_diff(const float *H, ptrdiff_t Step, float Weight)
{
   const float near = H[Step] - H[-Step];
   const float wide = (H[-2 * Step] - H[2 * Step] + 8.0f * near) * (2.0f / 12.0f);
   return (near * wide > 0.0f) ? near + Weight * (wide - near) : near;
}

// Detail is the height minus a Gaussian base layer of DETAIL_SIGMA kernel units, and its slope is multiplied by
// DETAIL_BOOST.  The added slope is compressed with s / sqrt(1 + (s / SLOPE_KNEE)^2).  The base layer's radius is
// limited to MAX_SIGMA pixels to bound the cost of the blur.  A sigma of 1 confines the boost to features of a few
// kernel units: on the harness the 4 px band gains 14% of slope over the unboosted result and the 32 px band 1%.

constexpr double DETAIL_BOOST = 1.5;
constexpr double DETAIL_SIGMA = 1.0;
constexpr double SLOPE_KNEE = 1.0;
constexpr double MAX_SIGMA = 16.0;

// Detail smaller than CORE_LOW alpha levels is not boosted, which leaves residual quantisation steps alone.  The boost
// rises to its full strength at CORE_HIGH.

constexpr float CORE_LOW = 0.5f;
constexpr float CORE_HIGH = 1.5f;

// The shadow edge is a smoothstep over SHADOW_PENUMBRA degrees either side of the light's elevation, limited to half
// of the elevation so that a flat surface is never shadowed.  The sweep tracks SHADOW_LEVELS elevations across the
// penumbra and interpolates between them.  With the light at 30 degrees, a block 16 units high casts a shadow 28 px
// long on the harness, against 27.7 px expected.  MAX_SHADOW_LENGTH limits the distance beyond the page edge from
// which occluders are rendered.

constexpr double SHADOW_PENUMBRA = 2.0;
constexpr int SHADOW_LEVELS = 4;
constexpr double MAX_SHADOW_LENGTH = 128;

// Working buffers are kept between draws by each drawing thread, unless they grow beyond SCRATCH_LIMIT floats.

constexpr size_t SCRATCH_LIMIT = 8 * 1024 * 1024;

struct light_scratch {
   std::vector<float> raw, heights, detail, curve, shadow;

   static float * reserve(std::vector<float> &Buffer, size_t Size) {
      if (Buffer.size() < Size) Buffer.resize(Size);
      return Buffer.data();
   }

   void trim() {
      if (raw.size() + heights.size() + detail.size() + curve.size() + shadow.size() > SCRATCH_LIMIT) {
         for (auto *buffer : { &raw, &heights, &detail, &curve, &shadow }) {
            buffer->clear();
            buffer->shrink_to_fit();
         }
      }
   }
};

static thread_local light_scratch glLightScratch;

struct lit_surface {
   const float *heights; // Padded, dequantised heights
   const float *detail;  // Padded, cored detail layer, or nullptr if detail is not boosted
   const float *curve;   // Padded heights from which curvature is measured
   const float *shadow;  // Fraction of a distant light reaching each pixel, unpadded, or nullptr
   int stride;
   int height;           // Height of the surface in pixels, excluding the padding
   map_height normal;    // Maps height gradients to slopes
   double curvature;     // Maps the Laplacian of the height to curvature per kernel unit
   float sharpen;        // Weight of the 4th-order gradient, from 0 to 1
};

// Dequantises elements Start to End of the row R1, where R0 and R2 are the rows above and below.  A neighbourhood
// spanning one level or less is a terrace step or a flat region, so its centre is replaced with a
// [1 2 1] x [1 2 1] / 16 mean.

static void dequantise_span(const float *__restrict R0, const float *__restrict R1, const float *__restrict R2,
   float *__restrict Dest, int Start, int End)
{
   for (int x=Start; x < End; ++x) {
      const float lo = std::min(std::min(std::min(R0[x - 1], R0[x]), std::min(R0[x + 1], R1[x - 1])),
         std::min(std::min(R1[x], R1[x + 1]), std::min(std::min(R2[x - 1], R2[x]), R2[x + 1])));
      const float hi = std::max(std::max(std::max(R0[x - 1], R0[x]), std::max(R0[x + 1], R1[x - 1])),
         std::max(std::max(R1[x], R1[x + 1]), std::max(std::max(R2[x - 1], R2[x]), R2[x + 1])));
      const float mean = ((R0[x - 1] + R0[x + 1] + R2[x - 1] + R2[x + 1])
         + 2.0f * (R0[x] + R1[x - 1] + R1[x + 1] + R2[x]) + 4.0f * R1[x]) * (1.0f / 16.0f);
      Dest[x] = (hi - lo <= 1.0f) ? mean : R1[x];
   }
}

// Weighted sum of Radius taps either side of each element from Start to End, with taps Step elements apart.

static void blur_span(const float *__restrict Source, float *__restrict Dest, const float *__restrict Weights,
   int Radius, int Start, int End, int Step)
{
   const float w0 = Weights[0];
   for (int x=Start; x < End; ++x) Dest[x] = w0 * Source[x];
   for (int i=1; i <= Radius; ++i) {
      const float wi = Weights[i];
      const float *__restrict before = Source - (i * Step), *__restrict after = Source + (i * Step);
      for (int x=Start; x < End; ++x) Dest[x] += wi * (before[x] + after[x]);
   }
}

// Replaces the blurred heights in Detail with the cored difference between Heights and the blur.

static void core_detail(const float *__restrict Heights, float *__restrict Detail, int Count)
{
   for (int x=0; x < Count; ++x) {
      const float diff = Heights[x] - Detail[x];
      const float weight = std::clamp((std::abs(diff) - CORE_LOW) * (1.0f / (CORE_HIGH - CORE_LOW)), 0.0f, 1.0f);
      Detail[x] = diff * weight;
   }
}

// Separable Gaussian blur of Source into Dest, clamping its indices to the buffer.  Work receives the horizontal pass.
// Finish(Y) is called for each row once its blur is complete.

static void gaussian_blur(const float *Source, float *Dest, float *Work, int Stride, int Rows, double Sigma,
   auto ForRows, auto Finish)
{
   const int radius = std::min(int(std::ceil(Sigma * 3.0)), (Stride - 1) / 2);
   std::vector<float> weights(radius + 1);
   double total = 0;
   for (int i=0; i <= radius; ++i) {
      weights[i] = float(exp(-(i * i) / (2.0 * Sigma * Sigma)));
      total += (i ? 2.0 : 1.0) * weights[i];
   }
   for (auto &w : weights) w = float(w / total);

   // The module is compiled without strict aliasing, so loop bounds and weights are copied to locals that stores
   // cannot affect, and buffers are marked as unaliased.  Otherwise the loops are not vectorised.

   ForRows(Rows, [&](int Start, int End) {
      const float *w = weights.data();
      const int stride = Stride, r = radius;
      for (int y=Start; y < End; ++y) {
         const float *__restrict src = Source + y * Stride;
         float *__restrict dest = Work + y * Stride;
         auto clamped = [&](int X) {
            float sum = w[0] * src[X];
            for (int i=1; i <= radius; ++i) {
               sum += w[i] * (src[std::max(X - i, 0)] + src[std::min(X + i, Stride - 1)]);
            }
            return sum;
         };
         for (int x=0; x < r; ++x) dest[x] = clamped(x);
         for (int x=stride - r; x < stride; ++x) dest[x] = clamped(x);
         blur_span(src, dest, w, r, r, stride - r, 1);
      }
   });

   ForRows(Rows, [&](int Start, int End) {
      const float *w = weights.data();
      const int stride = Stride, rows = Rows, r = radius;
      for (int y=Start; y < End; ++y) {
         if ((y >= r) and (y < rows - r)) blur_span(Work + y * stride, Dest + y * stride, w, r, 0, stride, stride);
         else { // Rows near the edge clamp to the buffer
            float *dest = Dest + y * stride;
            for (int x=0; x < stride; ++x) {
               float sum = w[0] * Work[y * stride + x];
               for (int i=1; i <= r; ++i) {
                  sum += w[i] * (Work[std::max(y - i, 0) * stride + x] + Work[std::min(y + i, rows - 1) * stride + x]);
               }
               dest[x] = sum;
            }
         }
         Finish(y);
      }
   });
}

// Detail layer: Source minus its Gaussian blur, cored to ignore quantisation residue.

static void detail_layer(const float *Source, float *Dest, float *Work, int Stride, int Rows, double Sigma,
   auto ForRows)
{
   gaussian_blur(Source, Dest, Work, Stride, Rows, Sigma, ForRows, [&](int Y) {
      core_detail(Source + Y * Stride, Dest + Y * Stride, Stride);
   });
}

// One step of the shadow sweep for Rows rows.  Prev and Cur hold the volumes for the previous and current step at
// each elevation, offset so that index -1 and Rows are the slots beside the band.  Heights are the rows' heights at
// this step, and Light receives the fraction of light reaching each row.  The parameters are restricted so that the
// loop can be vectorised; the module is compiled without strict aliasing.

static_assert(SHADOW_LEVELS IS 4);

static void sweep_rows(const float *__restrict Heights, const float *__restrict N0, const float *__restrict N1,
   const float *__restrict N2, const float *__restrict N3, float *__restrict V0, float *__restrict V1,
   float *__restrict V2, float *__restrict V3, float *__restrict Light, int Rows, int Side, float Frac,
   float Fall0, float Fall1, float Fall2, float Fall3)
{
   constexpr float RANGE = 1.0f / (SHADOW_LEVELS - 1);
   for (int r=0; r < Rows; ++r) {
      const float height = Heights[r];
      const float f0 = N0[r] + Frac * (N0[r + Side] - N0[r]) - Fall0;
      const float f1 = N1[r] + Frac * (N1[r + Side] - N1[r]) - Fall1;
      const float f2 = N2[r] + Frac * (N2[r + Side] - N2[r]) - Fall2;
      const float f3 = N3[r] + Frac * (N3[r + Side] - N3[r]) - Fall3;
      V0[r] = std::max(height, f0);
      V1[r] = std::max(height, f1);
      V2[r] = std::max(height, f2);
      V3[r] = std::max(height, f3);

      // Clearance above the fallen volume grows with elevation.  Each pair of elevations contributes the part of its
      // interval that lies in shadow, which interpolates the elevation of the shadow's edge.

      const float c0 = height - f0, c1 = height - f1, c2 = height - f2, c3 = height - f3;
      const float t = (std::clamp(-c0 / std::max(c1 - c0, 1e-6f), 0.0f, 1.0f) +
         std::clamp(-c1 / std::max(c2 - c1, 1e-6f), 0.0f, 1.0f) +
         std::clamp(-c2 / std::max(c3 - c2, 1e-6f), 0.0f, 1.0f)) * RANGE;
      Light[r] = 1.0f - (t * t * (3.0f - 2.0f * t));
   }
}

static inline void sweep_step(const float *Heights, float *const *Prev, float *const *Cur, float *Light, int Rows,
   int Side, float Frac, const float *Fall)
{
   sweep_rows(Heights, Prev[0] + 1, Prev[1] + 1, Prev[2] + 1, Prev[3] + 1, Cur[0] + 1, Cur[1] + 1, Cur[2] + 1,
      Cur[3] + 1, Light, Rows, Side, Frac, Fall[0], Fall[1], Fall[2], Fall[3]);
}

//********************************************************************************************************************
// Computes the fraction of a distant light that reaches each pixel of the Width x Height surface at Heights.  Pixels
// are visited along the axis that is closest to the light's direction, starting from the side nearest the light.  The
// upstream neighbour of a pixel is one step towards the light along that axis and a fraction of a pixel across it.
//
// For each of the tracked elevations, S is the height of the shadow volume above a pixel: the greater of the surface
// and the upstream volume after it has fallen by one step.  A pixel is lit at an elevation if the fallen volume is
// below it.  The light fraction interpolates the elevation at which the pixel passes into shadow.
//
// The image is divided into bands across the sweep.  Each band reads one row of its upstream neighbour, which the
// neighbour publishes as it goes, so the bands run concurrently as a wavefront.

static void cast_shadows(const float *Heights, int Stride, int Width, int Height, float *Output,
   const point3 &Direction, double Elevation, double LevelSlope, BS::thread_pool<> &Pool)
{
   const bool x_major = std::abs(Direction.x) >= std::abs(Direction.y);
   const int along_n = x_major ? Width : Height;
   const int cross_n = x_major ? Height : Width;
   const ptrdiff_t h_along = x_major ? 1 : Stride, h_cross = x_major ? Stride : 1;
   const ptrdiff_t o_along = x_major ? 1 : Width, o_cross = x_major ? Width : 1;
   const double light_along = x_major ? Direction.x : Direction.y;
   const double offset = (x_major ? Direction.y : Direction.x) / std::abs(light_along);
   const int side = (offset > 0) ? 1 : -1;  // Direction of the upstream row across the sweep
   const float frac = float(std::abs(offset));
   const double distance = sqrt(1.0 + (offset * offset));

   // Height lost by the shadow volume per step at each elevation, in alpha levels

   const double penumbra = std::min(SHADOW_PENUMBRA * DEG2RAD, Elevation * 0.5);
   float fall[SHADOW_LEVELS];
   for (int k=0; k < SHADOW_LEVELS; ++k) {
      const double e = std::min(Elevation - penumbra + (2.0 * penumbra * k / (SHADOW_LEVELS - 1)), 89.5 * DEG2RAD);
      fall[k] = float(tan(e) * distance / LevelSlope);
   }

   const int bands = std::clamp(cross_n / 16, 1, int(Pool.get_thread_count()));
   std::vector<float> edges(size_t(bands) * along_n * SHADOW_LEVELS);
   std::unique_ptr<std::atomic<int>[]> progress(new std::atomic<int>[bands]);
   for (int b=0; b < bands; ++b) progress[b].store(0);

   // A band is processed in tiles of up to TILE steps.  The heights of a tile are copied into a buffer ordered by step,
   // so that each step reads and writes its rows in sequence and the rows can be computed together.

   constexpr int TILE = 32;
   constexpr float NO_VOLUME = -1e30f;

   auto sweep_band = [&](int Band) {
      const int j0 = int(int64_t(cross_n) * Band / bands), j1 = int(int64_t(cross_n) * (Band + 1) / bands);
      const int rows = j1 - j0;

      // The neighbour that holds this band's upstream rows, and the row that this band publishes for its own
      // dependant.  A band at the edge of the image treats its edge row as its own upstream row.

      const int upstream = ((frac > 0.0f) and (Band + side >= 0) and (Band + side < bands)) ? Band + side : -1;
      const int published = (side > 0) ? 0 : rows - 1;
      const int extra = (side > 0) ? rows : -1;
      const int edge_row = (side > 0) ? rows - 1 : 0;

      // Volumes for the previous and current step at each elevation, with a slot either side for the neighbouring
      // rows.  Index 0 is the slot before row 0.

      std::vector<float> buffer(size_t(rows + 2) * SHADOW_LEVELS * 2 + size_t(rows) * TILE * 2, NO_VOLUME);
      float *prev[SHADOW_LEVELS], *cur[SHADOW_LEVELS];
      for (int k=0; k < SHADOW_LEVELS; ++k) {
         prev[k] = buffer.data() + (rows + 2) * k;
         cur[k] = buffer.data() + (rows + 2) * (SHADOW_LEVELS + k);
      }
      float *tile_heights = buffer.data() + (rows + 2) * SHADOW_LEVELS * 2;
      float *tile_light = tile_heights + rows * TILE;

      for (int p0=0; p0 < along_n; p0 += TILE) {
         const int count = std::min(TILE, along_n - p0);
         auto index = [&](int C) { return (light_along > 0) ? along_n - 1 - (p0 + C) : p0 + C; };

         for (int r=0; r < rows; ++r) {
            const float *src = Heights + ((j0 + r) * h_cross);
            for (int c=0; c < count; ++c) tile_heights[(c * rows) + r] = src[index(c) * h_along];
         }

         if (upstream >= 0) {
            while (progress[upstream].load(std::memory_order_acquire) < p0 + count - 1) std::this_thread::yield();
         }

         for (int c=0; c < count; ++c) {
            const int p = p0 + c;

            // Fill the slot beside the band with the upstream row from the previous step

            for (int k=0; k < SHADOW_LEVELS; ++k) {
               if (upstream < 0) prev[k][extra + 1] = prev[k][edge_row + 1];
               else if (p IS 0) prev[k][extra + 1] = NO_VOLUME;
               else prev[k][extra + 1] = edges[(size_t(upstream) * along_n + p - 1) * SHADOW_LEVELS + k];
            }

            sweep_step(tile_heights + (c * rows), prev, cur, tile_light + (c * rows), rows, side, frac, fall);

            for (int k=0; k < SHADOW_LEVELS; ++k) {
               edges[(size_t(Band) * along_n + p) * SHADOW_LEVELS + k] = cur[k][published + 1];
               std::swap(prev[k], cur[k]);
            }
         }

         progress[Band].store(p0 + count, std::memory_order_release);

         for (int r=0; r < rows; ++r) {
            float *dest = Output + ((j0 + r) * o_cross);
            for (int c=0; c < count; ++c) dest[index(c) * o_along] = tile_light[(c * rows) + r];
         }
      }
   };

   // Upstream bands are queued first, so that a band never waits on one that cannot start.

   for (int n=0; n < bands; ++n) {
      const int band = (side > 0) ? bands - 1 - n : n;
      Pool.detach_task([&sweep_band, band]() { sweep_band(band); });
   }
   Pool.wait();
}

// Slopes and occlusion for one row of Width pixels.  H is the row of heights from which curvature is measured, and
// the gradient is taken from the 1D differences across rows (X0 to X2, for the rows above, at and below) and along
// the row (Y, including the pixel either side).  The differences are smoothed across their axis with weights
// [1, Centre, 1] * Scale.  The parameters are restricted so that the loop can be vectorised.

static void row_slopes(const float *__restrict H, int Stride, const float *__restrict X0, const float *__restrict X1,
   const float *__restrict X2, const float *__restrict Y, const map_height &Normal, float Centre, float Scale,
   float Cavity, float *__restrict SX, float *__restrict SY, float *__restrict Occlusion, int Width)
{
   const float mx = float(Normal.x), my = float(Normal.y), mxy = float(Normal.xy);
   const float floor = float(CAVITY_FLOOR);
   const float *__restrict up = H - Stride, *__restrict down = H + Stride;
   for (int x=0; x < Width; ++x) {
      const float gx = (X0[x] + Centre * X1[x] + X2[x]) * Scale;
      const float gy = (Y[x - 1] + Centre * Y[x] + Y[x + 1]) * Scale;
      SX[x] = (gx * mx) + (gy * mxy);
      SY[x] = (gx * mxy) + (gy * my);

      // Isotropic Laplacian (1/6) [1 4 1; 4 -20 4; 1 4 1].  It is positive where the surface is concave.

      const float laplacian = ((up[x - 1] + up[x + 1] + down[x - 1] + down[x + 1])
         + 4.0f * (up[x] + down[x] + H[x - 1] + H[x + 1]) - 20.0f * H[x]) * (1.0f / 6.0f);
      Occlusion[x] = std::max(floor, 1.0f - (Cavity * std::max(laplacian, 0.0f)));
   }
}

// Adds the boosted slope of the detail layer to one row.  Only the change that the boost makes to the compressed slope
// is applied, so the slope is unaltered where there is no detail.  Project the boost onto the unboosted slope and
// discard opposing components: the detail blur must not invent slopes on flat ground beside a hard edge.

static void row_boost(const float *__restrict X0, const float *__restrict X1, const float *__restrict X2,
   const float *__restrict Y, const map_height &Normal, float Centre, float Scale, float *__restrict SX,
   float *__restrict SY, int Width)
{
   const float mx = float(Normal.x), my = float(Normal.y), mxy = float(Normal.xy);
   const float boost = float(DETAIL_BOOST - 1.0);
   const float knee = float(1.0 / (SLOPE_KNEE * SLOPE_KNEE));
   for (int x=0; x < Width; ++x) {
      const float ddx = boost * (X0[x] + Centre * X1[x] + X2[x]) * Scale;
      const float ddy = boost * (Y[x - 1] + Centre * Y[x] + Y[x + 1]) * Scale;
      const float sx = SX[x], sy = SY[x];
      const float dx = (ddx * mx) + (ddy * mxy), dy = (ddx * mxy) + (ddy * my);
      const float slope2 = (sx * sx) + (sy * sy);
      const float along = std::max(0.0f, ((dx * sx) + (dy * sy)) / std::max(slope2, 1e-12f));
      const float bsx = sx + (along * sx), bsy = sy + (along * sy);
      const float boosted = 1.0f / std::sqrt(1.0f + ((bsx * bsx) + (bsy * bsy)) * knee);
      const float plain = 1.0f / std::sqrt(1.0f + ((sx * sx) + (sy * sy)) * knee);
      SX[x] = sx + (bsx * boosted) - (sx * plain);
      SY[x] = sy + (bsy * boosted) - (sy * plain);
   }
}

// Converts slopes to unit normals in place, and applies the shadow to the occlusion if there is one.

static void row_normals(float *__restrict NX, float *__restrict NY, float *__restrict NZ,
   float *__restrict Occlusion, const float *__restrict Shadow, int Width)
{
   for (int x=0; x < Width; ++x) {
      const float sx = NX[x], sy = NY[x];
      const float scale = 1.0f / std::sqrt((sx * sx) + (sy * sy) + 1.0f);
      NX[x] = -sx * scale;
      NY[x] = -sy * scale;
      NZ[x] = scale;
   }
   if (Shadow) {
      for (int x=0; x < Width; ++x) Occlusion[x] *= Shadow[x];
   }
}

// Toksvig's specular anti-aliasing for one row.  N is the mean of the unit normals over a pixel's 3x3 neighbourhood,
// weighted [1 2 1] x [1 2 1] / 16.  It is shorter than a unit vector where the normals vary, and Factor receives
// ft = |N| / (|N| + e (1 - |N|)), which scales the exponent e so that the highlight widens to cover the variation.
// MX, MY and MZ receive N normalised, which is the normal that the highlight is shaded with.  Shading the pixel's own
// normal with the reduced exponent would widen every glint instead of merging them, which increases sparkle.  A box
// mean was smoother than the SVG algorithm on the coarse_paper noise, so the tent keeps more of the detail.
//
// The rows above, at and below the pixel are given for each component, with one element either side of the row.
// Where the normals are all equal, as on a flat or evenly sloped surface, N is a unit vector and the factor is 1.

static void row_toksvig(const float *__restrict X0, const float *__restrict X1, const float *__restrict X2,
   const float *__restrict Y0, const float *__restrict Y1, const float *__restrict Y2,
   const float *__restrict Z0, const float *__restrict Z1, const float *__restrict Z2, float Exponent,
   float *__restrict MX, float *__restrict MY, float *__restrict MZ, float *__restrict Factor, int Width)
{
   for (int x=0; x < Width; ++x) {
      const float sx = (X0[x - 1] + 2.0f * X0[x] + X0[x + 1]) + 2.0f * (X1[x - 1] + 2.0f * X1[x] + X1[x + 1])
         + (X2[x - 1] + 2.0f * X2[x] + X2[x + 1]);
      const float sy = (Y0[x - 1] + 2.0f * Y0[x] + Y0[x + 1]) + 2.0f * (Y1[x - 1] + 2.0f * Y1[x] + Y1[x + 1])
         + (Y2[x - 1] + 2.0f * Y2[x] + Y2[x + 1]);
      const float sz = (Z0[x - 1] + 2.0f * Z0[x] + Z0[x + 1]) + 2.0f * (Z1[x - 1] + 2.0f * Z1[x] + Z1[x + 1])
         + (Z2[x - 1] + 2.0f * Z2[x] + Z2[x + 1]);
      const float sum = std::sqrt((sx * sx) + (sy * sy) + (sz * sz));
      const float length = std::min(sum / 16.0f, 1.0f);
      Factor[x] = length / (length + (Exponent * (1.0f - length)));
      MX[x] = sx / sum;
      MY[x] = sy / sum;
      MZ[x] = sz / sum;
   }
}

//********************************************************************************************************************
// Renders rows StartY to EndY from the padded height buffer.  Delta(x, y, height) returns the surface-to-light unit
// vector and Tint(stl) returns the colour of the light arriving from that direction.

template <class DELTA, class COLOUR>
void extLightingFX::render_enhanced(int StartY, int EndY, const lit_surface &Surface, int Width, DELTA Delta,
   COLOUR Tint)
{
   const uint8_t R = Target->ColourFormat->RedPos>>3;
   const uint8_t G = Target->ColourFormat->GreenPos>>3;
   const uint8_t B = Target->ColourFormat->BluePos>>3;
   const uint8_t A = Target->ColourFormat->AlphaPos>>3;
   const auto bpp = Target->BytesPerPixel;
   auto dest_base = (uint8_t *)(Target->Data + (Target->Clip.Left * bpp) + (Target->Clip.Top * Target->LineWidth));

   const int stride = Surface.stride;
   const auto &m = Surface.normal;

   // Each pixel's gradient smooths the 1D differences of its neighbours, so the differences are computed once per
   // row.  Across-row differences (dx) are kept for the three rows that the current row reads, and along-column
   // differences (dy) for the current row, including one pixel either side.

   const int span = Width + 2;
   std::vector<float> buffer(size_t(span) * 24);
   float *hx[3] = { buffer.data(), buffer.data() + span, buffer.data() + span * 2 };
   float *hy = buffer.data() + span * 3;
   float *dx[3] = { buffer.data() + span * 4, buffer.data() + span * 5, buffer.data() + span * 6 };
   float *dy = buffer.data() + span * 7;
   float *factor = buffer.data() + span * 8; // Toksvig factor for each pixel of the row
   float *mean_x = buffer.data() + span * 21; // Normalised mean normal for each pixel of the row
   float *mean_y = buffer.data() + span * 22;
   float *mean_z = buffer.data() + span * 23;

   // Surface normal and occlusion for each pixel, for three rows indexed by row number modulo 3.  Element 0 is the
   // pixel before the row, which replicates the first pixel, and element Width + 1 replicates the last.  Specular light
   // reads the rows above and below for the Toksvig factor; diffuse light needs the current row only.

   struct normal_row { float *x, *y, *z, *occlusion; };
   normal_row slots[3];
   for (int s=0; s < 3; ++s) {
      float *base = buffer.data() + span * (9 + (s * 4));
      slots[s] = { base, base + span, base + span * 2, base + span * 3 };
   }

   const bool toksvig = (Type IS LT::SPECULAR) and ((Features & LFX::SPECULAR_AA) != LFX::NIL);
   const int last_row = Surface.height - 1;
   const int first = toksvig ? std::max(StartY - 1, 0) : StartY;

   // Row points at the first pixel of an image row; Out[0] is the pixel before it.

   // Without SHARPEN, the 2nd-order difference [-1, 0, 1] and cross-axis smoothing [1, 2, 1] / 4 reproduce the
   // Sobel kernel of the SVG algorithm, including its slope gain of 2.  With the full weight of SHARPEN the smoothing
   // is [1, 6, 1] / 8, and partial weights blend the difference and the smoothing between the two.

   const float weight = Surface.sharpen;
   const bool sharpen = weight > 0.0f;
   const float centre = 2.0f + (4.0f * weight);
   const float smooth = 1.0f / (centre + 2.0f);

   auto row_dx = [span, sharpen, weight](const float *__restrict Row, float *__restrict Out) {
      if (sharpen) for (int x=0; x < span; ++x) Out[x] = central_diff(Row + x - 1, 1, weight);
      else for (int x=0; x < span; ++x) Out[x] = Row[x] - Row[x - 2];
   };
   auto row_dy = [stride, span, sharpen, weight](const float *__restrict Row, float *__restrict Out) {
      if (sharpen) for (int x=0; x < span; ++x) Out[x] = central_diff(Row + x - 1, stride, weight);
      else for (int x=0; x < span; ++x) Out[x] = Row[x + stride - 1] - Row[x - stride - 1];
   };

   auto row_of = [&](const float *Base, int Y) { return Base + ((Y + LIGHT_PAD) * stride) + LIGHT_PAD; };

   for (int i=0; i < 2; ++i) {
      row_dx(row_of(Surface.heights, first - 1 + i), hx[i + 1]);
      if (Surface.detail) row_dx(row_of(Surface.detail, first - 1 + i), dx[i + 1]);
   }

   const float cavity = ((Features & LFX::CAVITY) != LFX::NIL) ? float(CAVITY_STRENGTH * Surface.curvature) : 0.0f;

   // Computes the normals of each row up to and including Y, in sequence.  A row's normals depend only on the
   // heights, so a row that is shared with a neighbouring chunk is identical in both.

   int computed = first - 1;
   auto compute_to = [&](int Y) {
      while (computed < Y) {
         const int ry = ++computed;
         std::rotate(hx, hx + 1, hx + 3);
         row_dx(row_of(Surface.heights, ry + 1), hx[2]);
         row_dy(row_of(Surface.heights, ry), hy);
         if (Surface.detail) {
            std::rotate(dx, dx + 1, dx + 3);
            row_dx(row_of(Surface.detail, ry + 1), dx[2]);
            row_dy(row_of(Surface.detail, ry), dy);
         }

         const auto &slot = slots[ry % 3];
         float *nx = slot.x + 1, *ny = slot.y + 1, *nz = slot.z + 1;
         row_slopes(row_of(Surface.curve, ry), stride, hx[0] + 1, hx[1] + 1, hx[2] + 1, hy + 1, m, centre, smooth,
            cavity, nx, ny, slot.occlusion + 1, Width);
         if (Surface.detail) row_boost(dx[0] + 1, dx[1] + 1, dx[2] + 1, dy + 1, m, centre, smooth, nx, ny, Width);
         row_normals(nx, ny, nz, slot.occlusion + 1, Surface.shadow ? Surface.shadow + (ry * Width) : nullptr,
            Width);

         for (float *n : { slot.x, slot.y, slot.z }) {
            n[0] = n[1];
            n[Width + 1] = n[Width];
         }
      }
   };

   const float exponent = float(SpecularExponent);
   for (int y=StartY; y < EndY; ++y) {
      const auto &cur = slots[y % 3];
      if (toksvig) {
         compute_to(std::min(y + 1, last_row));
         const auto &above = slots[std::max(y - 1, 0) % 3];
         const auto &below = slots[std::min(y + 1, last_row) % 3];
         row_toksvig(above.x + 1, cur.x + 1, below.x + 1, above.y + 1, cur.y + 1, below.y + 1,
            above.z + 1, cur.z + 1, below.z + 1, exponent, mean_x, mean_y, mean_z, factor, Width);
      }
      else compute_to(y);

      const float *h = row_of(Surface.heights, y);
      const float *nx = cur.x + 1, *ny = cur.y + 1, *nz = cur.z + 1, *occlusion = cur.occlusion + 1;
      auto dptr = dest_base + y * Target->LineWidth;
      for (int x=0; x < Width; ++x, dptr += bpp) {
         const point3 normal(nx[x], ny[x], nz[x]);
         point3 stl = Delta(x, y, h[x]);
         if (Type IS LT::DIFFUSE) diffuse_light(normal, stl, Tint(stl), dptr, R, G, B, A, occlusion[x]);
         else if (toksvig) {
            specular_light(point3(mean_x[x], mean_y[x], mean_z[x]), stl, Tint(stl), dptr, R, G, B, A, occlusion[x],
               SpecularExponent * double(factor[x]));
         }
         else specular_light(normal, stl, Tint(stl), dptr, R, G, B, A, occlusion[x]);
      }
   }
}

//********************************************************************************************************************

bool extLightingFX::boost_detail(double Sigma) const
{
   return ((Features & LFX::DETAIL) != LFX::NIL) and (DETAIL_BOOST != 1.0) and (Sigma >= 0.5);
}

// Sigma in pixels of the blur applied to the heights before curvature is measured, or 0 if no blur is required.

double extLightingFX::cavity_sigma(double UnitSize) const
{
   if (((Features & LFX::CAVITY) IS LFX::NIL) or (UnitSize <= 1.0)) return 0;
   const double sigma = std::min(MAX_SIGMA, CAVITY_SIGMA * sqrt((UnitSize * UnitSize) - 1.0));
   return (sigma >= MIN_CAVITY_SIGMA) ? sigma : 0;
}

// Weight of the 4th-order gradient, from 0 for the SVG kernel to 1.

float extLightingFX::sharpen_weight(double UnitSize) const
{
   if ((Features & LFX::SHARPEN) IS LFX::NIL) return 0;
   return float(std::clamp((SHARPEN_NONE - UnitSize) / (SHARPEN_NONE - SHARPEN_FULL), 0.0, 1.0));
}

//********************************************************************************************************************
// Distance in pixels over which the lighting samples neighbouring pixels.  The SVG kernel is 3x3.  The enhanced
// gradient reaches 2 px into heights that are dequantised over 1 px, and the specular Toksvig factor averages the
// normals over a further 1 px.  The detail layer and the curvature blur add the larger of their blur radii, and
// shadows add the length of the longest shadow, limited to MAX_SHADOW_LENGTH.  Each addition applies only if its
// feature is enabled.

double extLightingFX::reach() const
{
   if (not enhanced()) return 2;

   const double scale = Filter->ClientVector ? Filter->ClientVector->Transform.scale() : 1.0;
   const double unit_size = unit_length(kernel_unit_pixels(scale));
   const double sigma = std::min(MAX_SIGMA, DETAIL_SIGMA * unit_size);
   double blur = cavity_sigma(unit_size);
   if (boost_detail(sigma)) blur = std::max(blur, sigma);
   double result = 4 + std::ceil(blur * 3.0);

   if ((LightSource IS LS::DISTANT) and ((Features & LFX::SHADOWS) != LFX::NIL)) {
      const double elevation = Elevation * DEG2RAD;
      const double lowest = elevation - std::min(SHADOW_PENUMBRA * DEG2RAD, elevation * 0.5);
      if ((elevation > 0) and (elevation < 89.0 * DEG2RAD)) {
         result += std::ceil(std::min(MAX_SHADOW_LENGTH, std::abs(MapHeight) * scale / tan(lowest)));
      }
   }
   return result;
}

//********************************************************************************************************************

void extLightingFX::draw_enhanced(objBitmap *Bitmap, const point3 &Light, double SpotHeight,
   const map_height &NormalHeight, double UnitSize, int Width, int Height)
{
   const int stride = Width + (LIGHT_PAD * 2);
   const int rows   = Height + (LIGHT_PAD * 2);
   const size_t size = size_t(stride) * rows;
   auto &scratch = glLightScratch;
   const bool dequantise = (Features & LFX::DEQUANTISE) != LFX::NIL;
   float *raw = light_scratch::reserve(scratch.raw, size);
   float *heights = dequantise ? light_scratch::reserve(scratch.heights, size) : raw;

   const uint8_t A = Target->ColourFormat->AlphaPos>>3;
   const auto bpp = Bitmap->BytesPerPixel;
   auto input_base = (uint8_t *)(Bitmap->Data + (Bitmap->Clip.Left * bpp) + (Bitmap->Clip.Top * Bitmap->LineWidth));

   int num_threads = std::min(int(std::thread::hardware_concurrency()), Height);
   if (num_threads < 1) num_threads = 1;
   const int chunk_size = std::max(4, Height / num_threads);
   BS::thread_pool pool(num_threads);

   auto for_rows = [&](int Total, auto Function) {
      for (int start=0; start < Total; start += chunk_size) {
         pool.detach_task([&Function, start, end = std::min(start + chunk_size, Total)]() { Function(start, end); });
      }
      pool.wait();
   };

   // Convert alpha to heights, replicating the edge pixels into the padding.

   for_rows(rows, [&](int Start, int End) {
      for (int py=Start; py < End; ++py) {
         const int y = std::clamp(py - LIGHT_PAD, 0, Height - 1);
         const uint8_t *src = input_base + (y * Bitmap->LineWidth) + A;
         float *dest = raw + py * stride;
         std::fill_n(dest, LIGHT_PAD, float(src[0]));
         for (int x=0; x < Width; ++x) dest[LIGHT_PAD + x] = src[x * bpp];
         std::fill_n(dest + LIGHT_PAD + Width, LIGHT_PAD, float(src[(Width - 1) * bpp]));
      }
   });

   // Dequantise.  Neighbours are clamped to the buffer, which matches the replicated edges.  Without DEQUANTISE, the
   // raw heights are used as they are.

   if (dequantise) for_rows(rows, [&](int Start, int End) {
      for (int py=Start; py < End; ++py) {
         const float *r0 = raw + std::max(py - 1, 0) * stride;
         const float *r1 = raw + py * stride;
         const float *r2 = raw + std::min(py + 1, rows - 1) * stride;
         float *dest = heights + py * stride;
         auto dequantise_edge = [&](int X, int L, int R) { // Edge columns clamp their neighbours to the buffer
            const float lo = std::min({ r0[L], r0[X], r0[R], r1[L], r1[X], r1[R], r2[L], r2[X], r2[R] });
            const float hi = std::max({ r0[L], r0[X], r0[R], r1[L], r1[X], r1[R], r2[L], r2[X], r2[R] });
            const float mean = ((r0[L] + r0[R] + r2[L] + r2[R]) + 2.0f * (r0[X] + r1[L] + r1[R] + r2[X])
               + 4.0f * r1[X]) * (1.0f / 16.0f);
            return (hi - lo <= 1.0f) ? mean : r1[X];
         };
         dest[0] = dequantise_edge(0, 0, 1);
         dequantise_span(r0, r1, r2, dest, 1, stride - 1);
         dest[stride - 1] = dequantise_edge(stride - 1, stride - 2, stride - 1);
      }
   });

   // Surface positions for point and spot lights are measured from the dequantised height.

   auto surface_delta = [&Light, SpotHeight](double X, double Y, float Alpha) -> point3 {
      point3 direction(Light.x - X, Light.y - Y, Light.z - (double(Alpha) * (1.0 / 255.0) * SpotHeight));
      direction.normalise();
      return direction;
   };

   // Detail layer for the boost.  The raw buffer is no longer needed once it has been dequantised, so it serves as the
   // blur's work space.

   float *detail = nullptr;
   const double sigma = std::min(MAX_SIGMA, DETAIL_SIGMA * UnitSize);
   if (boost_detail(sigma)) {
      detail = light_scratch::reserve(scratch.detail, size);
      float *work = dequantise ? raw : light_scratch::reserve(scratch.heights, size);
      detail_layer(heights, detail, work, stride, rows, sigma, for_rows);
   }

   // Heights for measuring curvature, blurred to the scale of the kernel unit if it covers more than a pixel.

   const float *curve = heights;
   if (const double curve_sigma = cavity_sigma(UnitSize); curve_sigma > 0) {
      float *blurred = light_scratch::reserve(scratch.curve, size);
      float *work = dequantise ? raw : light_scratch::reserve(scratch.heights, size);
      gaussian_blur(heights, blurred, work, stride, rows, curve_sigma, for_rows, [](int) { });
      curve = blurred;
   }

   // Shadows for distant lights.  Heights are converted to pixels with the same scale that positions the surface
   // relative to point and spot lights.  A negative surface scale inverts the surface, which is not supported.

   float *shadow = nullptr;
   const double elevation = Elevation * DEG2RAD;
   if ((LightSource IS LS::DISTANT) and ((Features & LFX::SHADOWS) != LFX::NIL) and (SpotHeight > 0) and
       (elevation > 0) and (elevation < 89.0 * DEG2RAD)) {
      shadow = light_scratch::reserve(scratch.shadow, size_t(Width) * Height);
      cast_shadows(heights + (LIGHT_PAD * stride) + LIGHT_PAD, stride, Width, Height, shadow, Direction, elevation,
         SpotHeight * (1.0 / 255.0), pool);
   }

   const lit_surface surface = {
      heights, detail, curve, shadow, stride, Height, NormalHeight, MapHeight * (1.0 / 255.0) * UnitSize * UnitSize,
      sharpen_weight(UnitSize)
   };

   for_rows(Height, [&](int Start, int End) {
      if (LightSource IS LS::DISTANT) {
         render_enhanced(Start, End, surface, Width,
            [this](int, int, float) { return Direction; },
            [this](const point3 &) -> const FRGB & { return LightColour; });
      }
      else if (LightSource IS LS::SPOT) {
         render_enhanced(Start, End, surface, Width, surface_delta,
            [this](point3 &STL) { return colour_spot_light(STL); });
      }
      else if (LightSource IS LS::POINT) {
         render_enhanced(Start, End, surface, Width, surface_delta,
            [this](const point3 &) -> const FRGB & { return LightColour; });
      }
   });

   scratch.trim();
}
