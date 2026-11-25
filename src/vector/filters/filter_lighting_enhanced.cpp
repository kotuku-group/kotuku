/*********************************************************************************************************************

Enhanced lighting, used when the scene's Compliance is VSC::NONE.  The SVG algorithm loses fine detail because its
3x3 Sobel gradient attenuates short features, and 8-bit heights produce terracing on gentle slopes.  This pipeline
replaces the gradient stage only; normals, kernel units and the light models are shared with the SVG path.

1. The alpha channel is converted to a float height buffer, padded with replicated edge values so that no special
   edge kernels are required.
2. For 8-bit heights, the default dequantiser applies a weighted mean where the 3x3 neighbourhood spans one alpha
   level or less. The wide pass first estimates a slope from successive one-level transitions that bound a
   terrace; with both enabled, the default dequantiser smooths only the samples that the wide pass leaves.
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
   shadow is softened over a small angle, so the penumbra widens with distance from the occluder.  With Wrap, a
   second sweep casts shadows from the light raised by the wrap angle, for faces that turn away from the light.  Each
   distant light that casts shadows has its own sweep and shadow map; the heights and the worker pool are shared.
7. Specular highlights are anti-aliased with Toksvig's method.  Where the normals within a pixel's neighbourhood vary,
   the highlight is shaded with their mean and a reduced exponent, so that it spreads over the variation instead of
   sparkling.
8. The combined type evaluates the diffuse and specular light together and applies them to an albedo, with ambient
   light, clamping once on output.  Material terms (metalness, Fresnel, wrap and a matcap) select enhanced_material()
   for the draw; with all of them at their defaults enhanced_combined() is used, so the result is unchanged.  A matcap
   is converted once per draw to premultiplied colour in the filter's colour space, and sampled with the shading
   normal.
9. Several lights (combined type only) share the surface analysis: normals, cavity and the Toksvig rows are computed
   once per row, and enhanced_lights() accumulates the light of each one before the albedo, ambient light and matcap
   are applied and the result is encoded once.  A single light keeps the specialised path above.
10. A point or spot light with a range is attenuated by a smooth window times an inverse-square fall-off that is
   normalised to the sample nearest the light.  The nearest sample is found once per draw within the light's range;
   reach() extends the processing region by twice the range so that clipped draws see the same samples.
11. An ambient occlusion map multiplies the cavity term for indirect light only: the ambient light and the matcap.
   Each row is sampled once into a row buffer, and only when the draw has indirect light, so that an unbound map, or
   a draw without ambient light or a matcap, does the same arithmetic as before.
12. A roughness map replaces the constant specular exponent.  Each row is converted once into row buffers of
   exponents and highlight gains, and the Toksvig factor of SPECULAR_AA is computed with the exponent of each pixel.
   Without a map the gain is exactly 1 and the exponent is the Exponent field, so the arithmetic is unchanged.
13. With LOCAL_SHADOWS, point and spot lights cast shadows.  The surface is swept outwards from the light's projection
   in four regions, carrying the height of the shadow along each ray (refer to filter_local_shadows.h).  The penumbra
   is sampled with light heights fixed at the centre of the filter region, so that the shadows do not depend on the
   clip, and the effect reads its entire input because a shadow can cross the filter region.  Lights at the same
   position share one map, and Wrap adds a second sweep from the light raised by the wrap angle as for distant
   lights.

*********************************************************************************************************************/

constexpr int LIGHT_PAD = 3; // Padding on each side of the height buffer; the gradient stencil reaches 2 px.
constexpr int MAX_TERRACE = 16;

// Occlusion is 1 - Cavity * curvature for concave regions, limited to CAVITY_FLOOR.  Curvature is the Laplacian of
// the height in client units per kernel unit squared.

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
// Detail.  The added slope is compressed with s / sqrt(1 + (s / SLOPE_KNEE)^2).  The base layer's radius is
// limited to MAX_SIGMA pixels to bound the cost of the blur.  A sigma of 1 confines the boost to features of a few
// kernel units: on the harness the 4 px band gains 14% of slope over the unboosted result and the 32 px band 1%.

constexpr double DETAIL_SIGMA = 1.0;
constexpr double SLOPE_KNEE = 1.0;
constexpr double MAX_SIGMA = 16.0;

// Detail smaller than CORE_LOW alpha levels is not boosted, which leaves residual quantisation steps alone.  The boost
// rises to its full strength at CORE_HIGH.

constexpr float CORE_LOW = 0.5f;
constexpr float CORE_HIGH = 1.5f;

// The shadow edge is a smoothstep over Penumbra degrees either side of the light's elevation, limited to half
// of the elevation so that a flat surface is never shadowed.  The sweep tracks SHADOW_LEVELS elevations across the
// penumbra and interpolates between them.  With the light at 30 degrees, a block 16 units high casts a shadow 28 px
// long on the harness, against 27.7 px expected.  MAX_SHADOW_LENGTH limits the distance beyond the page edge from
// which occluders are rendered.

constexpr int SHADOW_LEVELS = 4;
constexpr double MAX_SHADOW_LENGTH = 128;

constexpr int LIGHT_ROW_BUFFERS = 27;

struct lit_surface {
   const float *heights; // Padded, dequantised heights
   const float *detail;  // Padded, cored detail layer, or nullptr if detail is not boosted
   const float *curve;   // Padded heights from which curvature is measured
   const float *shadow;  // Fraction of the light reaching each pixel, unpadded, or nullptr
   const float *raised;  // As shadow, with the light raised by the wrap angle, or nullptr if it is unshadowed
   const uint8_t *albedo; // First pixel of the albedo for the combined type, or nullptr if it is opaque white
   const float *albedo_lut; // Converts a stored albedo channel to the filter's colour space, 0 - 1
   int albedo_stride;    // Bytes per row of the albedo
   int albedo_width, albedo_height; // Pixels beyond the albedo's extent are transparent
   bool albedo_premul;   // True if the albedo's colour is premultiplied
   int stride;
   int height;           // Height of the surface in pixels, excluding the padding
   map_height normal;    // Maps height gradients to slopes
   double curvature;     // Maps the Laplacian of the height to curvature per kernel unit
   float sharpen;        // Weight of the 4th-order gradient, from 0 to 1
   std::span<const lit_light> lights; // Every light, if more than one is rendered; otherwise empty
   double spot_height;   // Converts a height in alpha levels to pixels
   int width;            // Width of the surface in pixels
   const filter_surface_view *normal_map; // Optional RGB directions in scene coordinates
   double normal_strength;
   bool flip_normal_y;
   const filter_map_view *occlusion; // Ambient occlusion in scene coordinates, or nullptr if no indirect light reads it
   double occlusion_strength;
   const filter_map_view *roughness; // Roughness in scene coordinates, or nullptr to use the Exponent field
   int scene_x, scene_y;  // Scene coordinates of the first output pixel
};

// Dequantises elements Start to End of the row R1, where R0 and R2 are the rows above and below.  A neighbourhood
// spanning one level or less is a terrace step or a flat region, so its centre is replaced with a
// [1 2 1] x [1 2 1] / 16 mean.  UnresolvedOnly preserves samples already reconstructed by the wide pass.

template<bool UnresolvedOnly>
static void dequantise_span(const float *__restrict R0, const float *__restrict R1, const float *__restrict R2,
   float *__restrict Dest, int Start, int End)
{
   for (int x=Start; x < End; ++x) {
      if constexpr (UnresolvedOnly) {
         if (Dest[x] >= 0.0f) continue;
      }
      const float lo = std::min(std::min(std::min(R0[x - 1], R0[x]), std::min(R0[x + 1], R1[x - 1])),
         std::min(std::min(R1[x], R1[x + 1]), std::min(std::min(R2[x - 1], R2[x]), R2[x + 1])));
      const float hi = std::max(std::max(std::max(R0[x - 1], R0[x]), std::max(R0[x + 1], R1[x - 1])),
         std::max(std::max(R1[x], R1[x + 1]), std::max(std::max(R2[x - 1], R2[x]), R2[x + 1])));
      const float mean = ((R0[x - 1] + R0[x + 1] + R2[x - 1] + R2[x + 1])
         + 2.0f * (R0[x] + R1[x - 1] + R1[x + 1] + R2[x]) + 4.0f * R1[x]) * (1.0f / 16.0f);
      Dest[x] = (hi - lo <= 1.0f) ? mean : R1[x];
   }
}

//********************************************************************************************************************
// Midpoints between adjacent pixel centres locate the two quantisation thresholds. Interpolate between them
// only when both steps are one level in the same direction. Each reconstructed sample stays within its bin.

static void reconstruct_terraces(const float *Source, int Length, std::ptrdiff_t Step, auto Write)
{
   int start = 0;
   while (start < Length) {
      const float height = Source[start * Step];
      int end = start + 1;
      while ((end < Length) and (Source[end * Step] IS height)) ++end;

      const int length = end - start;
      float slope = 0.0f;
      if ((length <= MAX_TERRACE) and (start > 0) and (end < Length)) {
         const float rise = height - Source[(start - 1) * Step];
         if ((std::abs(rise) IS 1.0f) and (Source[end * Step] - height IS rise)) slope = rise / float(length);
      }

      const float centre = 0.5f * float(length - 1);
      for (int i=0; i < length; ++i) Write(start + i, height + (float(i) - centre) * slope, slope != 0.0f);
      start = end;
   }
}

//********************************************************************************************************************
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

//********************************************************************************************************************
// Replaces the blurred heights in Detail with the cored difference between Heights and the blur.

static void core_detail(const float *__restrict Heights, float *__restrict Detail, int Count)
{
   for (int x=0; x < Count; ++x) {
      const float diff = Heights[x] - Detail[x];
      const float weight = std::clamp((std::abs(diff) - CORE_LOW) * (1.0f / (CORE_HIGH - CORE_LOW)), 0.0f, 1.0f);
      Detail[x] = diff * weight;
   }
}

//********************************************************************************************************************
// Separable Gaussian blur of Source into Dest, clamping its indices to the buffer.  Work receives the horizontal pass.
// Finish(Y) is called for each row once its blur is complete.

static void gaussian_blur(const float *Source, float *Dest, float *Work, int Stride, int Rows, double Sigma,
   auto ForRows, auto Finish)
{
   const int radius = std::min(int(std::ceil(Sigma * 3.0)), (Stride - 1) / 2);
   std::array<float, int(MAX_SIGMA * 3.0) + 1> weight_storage;
   auto weights = std::span(weight_storage).first(radius + 1);
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

//********************************************************************************************************************
// Detail layer: Source minus its Gaussian blur, cored to ignore quantisation residue.

static void detail_layer(const float *Source, float *Dest, float *Work, int Stride, int Rows, double Sigma,
   auto ForRows)
{
   gaussian_blur(Source, Dest, Work, Stride, Rows, Sigma, ForRows, [&](int Y) {
      core_detail(Source + Y * Stride, Dest + Y * Stride, Stride);
   });
}

//********************************************************************************************************************
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

static ERR cast_shadows(extLightingFX *Effect, const float *Heights, int Stride, int Width, int Height,
   float *Output, const point3 &Direction, double Elevation, double LevelSlope, BS::thread_pool<> &Pool)
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

   const double penumbra = std::min(Effect->Penumbra * DEG2RAD, Elevation * 0.5);
   float fall[SHADOW_LEVELS];
   for (int k=0; k < SHADOW_LEVELS; ++k) {
      const double e = std::min(Elevation - penumbra + (2.0 * penumbra * k / (SHADOW_LEVELS - 1)), 89.5 * DEG2RAD);
      fall[k] = float(tan(e) * distance / LevelSlope);
   }

   const int bands = std::clamp(cross_n / 16, 1, int(Pool.get_thread_count()));
   auto filter = Effect->Filter;
   auto &working = filter->Render->WorkingSurfaces;
   auto mark = working;
   auto release = kt::Defer([&]() { release_filter_surfaces(filter, working, mark); });
   float *edges;
   if (auto error = get_filter_scratch(Effect, size_t(bands) * along_n * SHADOW_LEVELS, edges); error != ERR::Okay) {
      return error;
   }
   auto progress = std::unique_ptr<std::atomic<int>[]>(new (std::nothrow) std::atomic<int>[bands]);
   if (not progress) return ERR::AllocMemory;
   for (int b=0; b < bands; ++b) progress[b].store(0);

   // A band is processed in tiles of up to TILE steps.  The heights of a tile are copied into a buffer ordered by step,
   // so that each step reads and writes its rows in sequence and the rows can be computed together.

   constexpr int TILE = 32;
   constexpr float NO_VOLUME = -1e30f;
   const int max_rows = (cross_n + bands - 1) / bands;
   const size_t band_size = (max_rows + 2) * SHADOW_LEVELS * 2 + max_rows * TILE * 2;
   float *band_buffers;
   if (auto error = get_filter_scratch(Effect, size_t(bands) * band_size, band_buffers); error != ERR::Okay) {
      return error;
   }

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

      auto buffer = band_buffers + Band * band_size;
      std::fill_n(buffer, (rows + 2) * SHADOW_LEVELS * 2, NO_VOLUME);
      float *prev[SHADOW_LEVELS], *cur[SHADOW_LEVELS];
      for (int k=0; k < SHADOW_LEVELS; ++k) {
         prev[k] = buffer + (rows + 2) * k;
         cur[k] = buffer + (rows + 2) * (SHADOW_LEVELS + k);
      }
      float *tile_heights = buffer + (rows + 2) * SHADOW_LEVELS * 2;
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
   return ERR::Okay;
}

//********************************************************************************************************************
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

//********************************************************************************************************************
// Adds the boosted slope of the detail layer to one row.  Only the change that the boost makes to the compressed slope
// is applied, so the slope is unaltered where there is no detail.  Project the boost onto the unboosted slope and
// discard opposing components: the detail blur must not invent slopes on flat ground beside a hard edge.  Boost is
// the slope multiplier.

static void row_boost(const float *__restrict X0, const float *__restrict X1, const float *__restrict X2,
   const float *__restrict Y, const map_height &Normal, float Centre, float Scale, double Boost,
   float *__restrict SX, float *__restrict SY, int Width)
{
   const float mx = float(Normal.x), my = float(Normal.y), mxy = float(Normal.xy);
   const float boost = float(Boost - 1.0);
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

//********************************************************************************************************************
// Converts slopes to unit normals in place.

static void row_normals(float *__restrict NX, float *__restrict NY, float *__restrict NZ, int Width)
{
   for (int x=0; x < Width; ++x) {
      const float sx = NX[x], sy = NY[x];
      const float scale = 1.0f / std::sqrt((sx * sx) + (sy * sy) + 1.0f);
      NX[x] = -sx * scale;
      NY[x] = -sy * scale;
      NZ[x] = scale;
   }
}

//********************************************************************************************************************
// Fills a table that converts an 8-bit channel to the filter's colour space, from 0 to 1.  LinearSource is true if the
// channel is linear, and Linear is true if the filter computes in linear RGB.

static void colour_table(std::array<float, 256> &Table, bool LinearSource, bool Linear)
{
   for (int i=0; i < 256; i++) {
      const float value = float(i) * (1.0f / 255.0f);
      if (Linear IS LinearSource) Table[i] = value;
      else if (Linear) Table[i] = rgb_to_linear::f_convert(value);
      else Table[i] = rgb_to_linear::f_invert(value);
   }
}

//********************************************************************************************************************
// Converts the matcap to premultiplied RGB in the filter's colour space, for sample_matcap(), with four floats per
// texel.  The bitmap is read in full on each draw, so that changes to a client's bitmap are seen.  A bitmap without
// an alpha channel is opaque.

ERR extLightingFX::prepare_matcap()
{
   MatcapTexels = nullptr;
   const auto bmp = Matcap;
   if ((not bmp) or (bmp->BytesPerPixel != 4) or (bmp->Width < 1) or (bmp->Height < 1) or (not bmp->Data)) {
      return ERR::Okay;
   }

   const int width = bmp->Width, height = bmp->Height;
   float *texels;
   if (auto error = get_filter_scratch(this, size_t(width) * height * 4, texels); error != ERR::Okay) return error;

   std::array<float, 256> lut;
   colour_table(lut, bmp->ColourSpace IS CS::LINEAR_RGB, Linear);

   const auto fmt = bmp->ColourFormat;
   const uint8_t R = fmt->RedPos>>3, G = fmt->GreenPos>>3, B = fmt->BluePos>>3, A = fmt->AlphaPos>>3;
   const bool has_alpha = (bmp->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL;
   const bool premul = (bmp->Flags & BMF::PREMUL) != BMF::NIL;

   float *out = texels;
   for (int y=0; y < height; y++) {
      const uint8_t *pixel = bmp->Data + (std::ptrdiff_t(y) * bmp->LineWidth);
      for (int x=0; x < width; x++, pixel += 4, out += 4) {
         const int alpha = has_alpha ? pixel[A] : 255;
         out[3] = 0;
         if (not alpha) {
            out[0] = out[1] = out[2] = 0;
            continue;
         }

         int r = pixel[R], g = pixel[G], b = pixel[B];
         if ((premul) and (alpha < 255)) {
            r = std::min(255, ((r * 255) + (alpha / 2)) / alpha);
            g = std::min(255, ((g * 255) + (alpha / 2)) / alpha);
            b = std::min(255, ((b * 255) + (alpha / 2)) / alpha);
         }
         const float level = float(alpha) * (1.0f / 255.0f);
         out[0] = lut[r] * level;
         out[1] = lut[g] * level;
         out[2] = lut[b] * level;
      }
   }

   MatcapTexels = texels;
   MatcapWidth  = width;
   MatcapHeight = height;
   return ERR::Okay;
}

//********************************************************************************************************************
// Reads the albedo at X from a row, or returns transparent black if X is beyond the albedo.  Without an albedo row the
// albedo is opaque white.

static inline albedo_sample read_albedo(const lit_surface &Surface, const uint8_t *Row, int X, uint8_t R, uint8_t G,
   uint8_t B, uint8_t A)
{
   if (not Surface.albedo) return { 1.0f, 1.0f, 1.0f, 1.0f };
   if ((not Row) or (X >= Surface.albedo_width)) return { 0, 0, 0, 0 };

   const uint8_t *pixel = Row + (X * 4);
   const int alpha = pixel[A];
   if (not alpha) return { 0, 0, 0, 0 };

   int r = pixel[R], g = pixel[G], b = pixel[B];
   if ((Surface.albedo_premul) and (alpha < 255)) {
      r = std::min(255, ((r * 255) + (alpha / 2)) / alpha);
      g = std::min(255, ((g * 255) + (alpha / 2)) / alpha);
      b = std::min(255, ((b * 255) + (alpha / 2)) / alpha);
   }
   const float *lut = Surface.albedo_lut;
   return { lut[r], lut[g], lut[b], float(alpha) * (1.0f / 255.0f) };
}

//********************************************************************************************************************
// Toksvig's specular anti-aliasing for one row.  N is the mean of the unit normals over a pixel's 3x3 neighbourhood,
// weighted [1 2 1] x [1 2 1] / 16.  It is shorter than a unit vector where the normals vary, and Factor receives
// ft = |N| / (|N| + e (1 - |N|)), which scales the exponent e so that the highlight widens to cover the variation.
// MX, MY and MZ receive N normalised, which is the normal that the highlight is shaded with.  Shading the pixel's own
// normal with the reduced exponent would widen every glint instead of merging them, which increases sparkle.  A box
// mean was smoother than the SVG algorithm on the coarse_paper noise, so the tent keeps more of the detail.
//
// The rows above, at and below the pixel are given for each component, with one element either side of the row.
// Where the normals are all equal, as on a flat or evenly sloped surface, N is a unit vector and the factor is 1.
// Exponents is the exponent of each pixel if VARYING is true, as given by a roughness map, and otherwise a single
// value for the row.

template <bool VARYING>
static void row_toksvig(const float *__restrict X0, const float *__restrict X1, const float *__restrict X2,
   const float *__restrict Y0, const float *__restrict Y1, const float *__restrict Y2,
   const float *__restrict Z0, const float *__restrict Z1, const float *__restrict Z2,
   const float *__restrict Exponents, float *__restrict MX, float *__restrict MY, float *__restrict MZ,
   float *__restrict Factor, int Width)
{
   for (int x=0; x < Width; ++x) {
      const float exponent = VARYING ? Exponents[x] : Exponents[0];
      const float sx = (X0[x - 1] + 2.0f * X0[x] + X0[x + 1]) + 2.0f * (X1[x - 1] + 2.0f * X1[x] + X1[x + 1])
         + (X2[x - 1] + 2.0f * X2[x] + X2[x + 1]);
      const float sy = (Y0[x - 1] + 2.0f * Y0[x] + Y0[x + 1]) + 2.0f * (Y1[x - 1] + 2.0f * Y1[x] + Y1[x + 1])
         + (Y2[x - 1] + 2.0f * Y2[x] + Y2[x + 1]);
      const float sz = (Z0[x - 1] + 2.0f * Z0[x] + Z0[x + 1]) + 2.0f * (Z1[x - 1] + 2.0f * Z1[x] + Z1[x + 1])
         + (Z2[x - 1] + 2.0f * Z2[x] + Z2[x + 1]);
      const float sum = std::sqrt((sx * sx) + (sy * sy) + (sz * sz));
      const float length = std::min(sum / 16.0f, 1.0f);
      Factor[x] = length / (length + (exponent * (1.0f - length)));
      MX[x] = sx / sum;
      MY[x] = sy / sum;
      MZ[x] = sz / sum;
   }
}

//********************************************************************************************************************
// The combined type lit by several lights, in the order in which they were added.  Each light has its own direction,
// spot cone, attenuation and shadows, and its diffuse and specular light are accumulated in floating point.  The terms
// of each light are those of enhanced_combined(), or of enhanced_material() if a material term is in use.  The
// reflectance does not depend on the light, so it is applied to the sum of the specular light.  Ambient light and the
// matcap are added once, and the result is composed and encoded once, so only the final output is clamped.  Cavity
// occludes the light of each light source, and Indirect, the cavity multiplied by the ambient occlusion map, occludes
// the ambient light and the matcap.  Gain scales the specular light of every light for a roughness map.

inline void extLightingFX::enhanced_lights(const lit_surface &Surface, const point3 &Normal, const point3 &Mean,
   double Exponent, double Gain, const albedo_sample &Albedo, int X, int Y, float Height, float Cavity, float Indirect,
   uint8_t *Output, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
   const std::ptrdiff_t offset = (std::ptrdiff_t(Y) * Surface.width) + X;
   const double surface_z = double(Height) * (1.0 / 255.0) * Surface.spot_height;
   const double indirect = Indirect;
   const double kd = Material ? Constant * (1.0 - Metalness) : Constant;
   const double ks = SpecularConstant * Gain;
   double dr = 0, dg = 0, db = 0, sr = 0, sg = 0, sb = 0;

   for (const auto &light : Surface.lights) {
      point3 stl, half;
      double falloff = 1.0;
      FRGB colour;

      if (light.kind IS LS::DISTANT) {
         stl = light.direction;
         half = light.half;
         colour = light.colour;
      }
      else {
         stl = point3(light.position.x - X, light.position.y - Y, light.position.z - surface_z);
         if (light.range > 0) {
            const double d2 = stl.dot(stl);
            falloff = light.attenuation(d2);
            if (falloff <= 0) continue;
            if (d2 < LIGHT_COINCIDENCE_SQUARED) stl = point3(0, 0, 1);
            else stl.normalise();
         }
         else stl.normalise();

         half = point3::half_vector(stl);
         colour = (light.kind IS LS::SPOT) ? light.spot_colour(stl) : light.colour;
      }

      const double occlusion = double(light.shadow ? Cavity * light.shadow[offset] : Cavity) * falloff;
      const double nl = Normal.dot(stl);
      double diffuse;

      if (Material) {
         double diffuse_occlusion = occlusion;
         if ((light.wrap_shadows) and (nl < light.wrap_blend)) {
            const double raised = double(light.raised ? Cavity * light.raised[offset] : Cavity) * falloff;
            const double turned = (nl <= 0) ? 1.0 : 1.0 - (nl / light.wrap_blend);
            diffuse_occlusion += turned * (raised - occlusion);
         }
         diffuse = kd * std::max(0.0, (nl + Wrap) * WrapScale) * diffuse_occlusion;
      }
      else diffuse = kd * std::max(0.0, nl) * occlusion;

      const double specular = ks * std::pow(std::max(0.0, Mean.dot(half)), Exponent) * occlusion;
      dr += diffuse * colour.Red;
      dg += diffuse * colour.Green;
      db += diffuse * colour.Blue;
      sr += specular * colour.Red;
      sg += specular * colour.Green;
      sb += specular * colour.Blue;
   }

   const double coverage = Albedo.a;

   if (Material) {
      const double base = (Fresnel > 0) ? Fresnel : 1.0;
      double fr = base + (Metalness * ((Albedo.r * coverage) - base));
      double fg = base + (Metalness * ((Albedo.g * coverage) - base));
      double fb = base + (Metalness * ((Albedo.b * coverage) - base));
      double grazing = 0;

      if (Fresnel > 0) {
         const double q = 1.0 - std::clamp(Mean.z, 0.0, 1.0);
         const double q2 = q * q;
         grazing = q2 * q2 * q;
         fr += (1.0 - fr) * grazing;
         fg += (1.0 - fg) * grazing;
         fb += (1.0 - fb) * grazing;
      }

      sr *= fr;
      sg *= fg;
      sb *= fb;

      if ((MatcapTexels) and (coverage > 0)) {
         alignas(16) float matcap[4];
         sample_matcap(Mean, matcap);

         const double weight = indirect * coverage;
         double cr = base + (Metalness * (Albedo.r - base));
         double cg = base + (Metalness * (Albedo.g - base));
         double cb = base + (Metalness * (Albedo.b - base));

         cr += (1.0 - cr) * grazing;
         cg += (1.0 - cg) * grazing;
         cb += (1.0 - cb) * grazing;
         sr += matcap[0] * cr * weight;
         sg += matcap[1] * cg * weight;
         sb += matcap[2] * cb * weight;
      }
   }

   const double alpha = std::max(coverage, std::min(1.0, std::max(std::max(sr, sg), sb)));
   if (alpha <= 0) {
      Output[R] = Output[G] = Output[B] = Output[A] = 0;
      return;
   }

   const double demul = 1.0 / alpha;
   Output[R] = encode_channel(((Albedo.r * coverage * ((AmbientLight.Red * indirect) + dr)) + sr) * demul, true);
   Output[G] = encode_channel(((Albedo.g * coverage * ((AmbientLight.Green * indirect) + dg)) + sg) * demul, true);
   Output[B] = encode_channel(((Albedo.b * coverage * ((AmbientLight.Blue * indirect) + db)) + sb) * demul, true);
   Output[A] = uint8_t((alpha * 255.0) + 0.5);
}

//********************************************************************************************************************
// Overwrites the height-derived normal where the map supplies a valid direction.

static void apply_normal_map_row(const filter_surface_view &Source, int SceneX, int SceneY, int Width,
   double Strength, bool FlipY, float *NX, float *NY, float *NZ)
{
   const int y = SceneY - Source.OriginY;
   if ((y < Source.Valid.top) or (y >= Source.Valid.bottom)) return;

   const int x = SceneX - Source.OriginX;
   const int first = std::max(0, Source.Valid.left - x);
   const int end = std::min(Width, Source.Valid.right - x);
   const auto row = Source.row(y);
   const auto format = Source.Bitmap->ColourFormat;
   const int r = format->RedPos >> 3, g = format->GreenPos >> 3, b = format->BluePos >> 3;
   const int a = format->AlphaPos >> 3;
   // For strengths above 1, divide the whole vector by Strength before normalising it.
   const float scale_xy = float(std::min(Strength, 1.0));
   const float scale_z = float(1.0 / std::max(Strength, 1.0));
   for (int i = first; i < end; ++i) {
      const auto pixel = row + std::ptrdiff_t(x + i) * 4;
      if (not pixel[a]) continue;
      const int raw_x = int(pixel[r]) - NORMAL_MAP_CENTRE;
      const int green = int(pixel[g]) - NORMAL_MAP_CENTRE;
      const int raw_y = FlipY ? -green : green;
      const int raw_z = int(pixel[b]) - NORMAL_MAP_CENTRE;
      if (not (raw_x or raw_y or raw_z)) continue;
      if ((Strength IS 0.0) or (not (raw_x or raw_y))) {
         NX[i] = NY[i] = 0.0f;
         NZ[i] = ((Strength > 0.0) and (raw_z < 0)) ? -1.0f : 1.0f;
         continue;
      }

      // A tangent normal keeps its direction at any positive strength, even if the strength rounds to zero.
      const float horizontal_scale = raw_z ? scale_xy : 1.0f;
      const float nx = float(raw_x) * horizontal_scale;
      const float ny = float(raw_y) * horizontal_scale;
      const float nz = float(raw_z) * scale_z;
      const float inv_length = 1.0f / std::sqrt((nx * nx) + (ny * ny) + (nz * nz));
      NX[i] = nx * inv_length;
      NY[i] = ny * inv_length;
      NZ[i] = nz * inv_length;
   }
}

//********************************************************************************************************************
// Multiplies a row of cavity terms by the ambient occlusion map, for indirect light.  The map is sampled at the centre
// of each output pixel, and its value ao is limited to 0 - 1 and applied as 1 + strength * (ao - 1).

static void occlusion_row(const lit_surface &Surface, int Y, const float *Cavity, float *Indirect, int Width)
{
   const auto &map = *Surface.occlusion;
   const double strength = Surface.occlusion_strength;
   const double scene_y = Surface.scene_y + Y + 0.5;
   for (int x=0; x < Width; ++x) {
      const double ao = std::clamp(double(map.sample(Surface.scene_x + x + 0.5, scene_y)[0]), 0.0, 1.0);
      Indirect[x] = Cavity[x] * float(std::max(0.0, 1.0 + (strength * (ao - 1.0))));
   }
}

//********************************************************************************************************************
// Converts a row of the roughness map to specular exponents and gains.  The map is sampled at the centre of each output
// pixel, and its roughness r is limited to 0 - 1 and converted with roughness_exponent().  Reference is the Exponent
// field, at which SpecularConstant is calibrated, and the gain (n + 2) / (Reference + 2) makes the highlights of smooth
// areas brighter as they narrow.  A sample equal to the map's fallback, which is the roughness of the Exponent field,
// uses that field with a gain of exactly 1, so areas the map does not cover keep the constant highlight.

static void roughness_row(const lit_surface &Surface, int Y, double Reference, float *Exponents, float *Gains,
   int Width)
{
   const auto &map = *Surface.roughness;
   const float fallback = map.Contract.Fallback[0];
   const double inverse = 1.0 / (Reference + 2.0);
   const double scene_y = Surface.scene_y + Y + 0.5;
   for (int x=0; x < Width; ++x) {
      const float r = map.sample(Surface.scene_x + x + 0.5, scene_y)[0];
      if ((r IS fallback) or (not std::isfinite(r))) {
         Exponents[x] = float(Reference);
         Gains[x] = 1.0f;
         continue;
      }

      const double n = roughness_exponent(std::clamp(double(r), 0.0, 1.0));
      Exponents[x] = float(n);
      Gains[x] = float((n + 2.0) * inverse);
   }
}

//********************************************************************************************************************
// Renders rows StartY to EndY from the padded height buffer.  Delta(x, y, height) returns the surface-to-light unit
// vector and Tint(stl) returns the colour of the light arriving from that direction.  Falloff(x, y, height) returns
// the attenuation of a ranged light, or exactly 1.  With several lights, the shading of each pixel is delegated to
// enhanced_lights() and the three functions are not used.

template <class DELTA, class COLOUR, class FALLOFF>
void extLightingFX::render_enhanced(int StartY, int EndY, const lit_surface &Surface, int Width, float *Buffer,
   DELTA Delta, COLOUR Tint, FALLOFF Falloff)
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
   float *hx[3] = { Buffer, Buffer + span, Buffer + span * 2 };
   float *hy = Buffer + span * 3;
   float *dx[3] = { Buffer + span * 4, Buffer + span * 5, Buffer + span * 6 };
   float *dy = Buffer + span * 7;
   float *factor = Buffer + span * 8; // Toksvig factor for each pixel of the row
   float *mean_x = Buffer + span * 21; // Normalised mean normal for each pixel of the row
   float *mean_y = Buffer + span * 22;
   float *mean_z = Buffer + span * 23;
   float *indirect_row = Buffer + span * 24; // Cavity multiplied by ambient occlusion, if a map is read
   float *exponent_row = Buffer + span * 25; // Specular exponent for each pixel, if a roughness map is read
   float *gain_row = Buffer + span * 26;     // Specular gain for each pixel, if a roughness map is read

   // Surface normal and occlusion for each pixel, for three rows indexed by row number modulo 3.  Element 0 is the
   // pixel before the row, which replicates the first pixel, and element Width + 1 replicates the last.  Specular light
   // reads the rows above and below for the Toksvig factor; diffuse light needs the current row only.

   struct normal_row { float *x, *y, *z, *occlusion; };
   normal_row slots[3];
   for (int s=0; s < 3; ++s) {
      float *base = Buffer + span * (9 + (s * 4));
      slots[s] = { base, base + span, base + span * 2, base + span * 3 };
   }

   const bool normal_map = Type IS LT::NORMAL;
   const bool toksvig = (not normal_map) and (Type != LT::DIFFUSE) and ((Features & LFX::SPECULAR_AA) != LFX::NIL);
   const bool ambient = (Type IS LT::DIFFUSE) and has_ambient();
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

   const float cavity = ((not normal_map) and ((Features & LFX::CAVITY) != LFX::NIL)) ?
      float(Cavity * Surface.curvature) : 0.0f;

   // Computes the normals of each row up to and including Y, in sequence.  A row shared with a neighbouring
   // chunk uses the same heights and normal map samples.

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
         if (Surface.detail) row_boost(dx[0] + 1, dx[1] + 1, dx[2] + 1, dy + 1, m, centre, smooth, Detail, nx, ny,
            Width);
         row_normals(nx, ny, nz, Width);
         if (Surface.normal_map) {
            apply_normal_map_row(*Surface.normal_map, Surface.scene_x, Surface.scene_y + ry, Width,
               Surface.normal_strength, Surface.flip_normal_y, nx, ny, nz);
         }

         for (float *n : { slot.x, slot.y, slot.z }) {
            n[0] = n[1];
            n[Width + 1] = n[Width];
         }
      }
   };

   // With a roughness map, each pixel has its own exponent and gain.  Otherwise every pixel uses the Exponent field
   // and a gain of 1.  The result of pixel_exponent() includes the Toksvig factor.  The lambdas capture by value: the
   // output is written through a byte pointer, which would otherwise force every captured value to be reloaded for
   // each pixel.

   const bool rough = Surface.roughness and (not normal_map) and (Type != LT::DIFFUSE);
   const float exponent = float(SpecularExponent);
   auto pixel_exponent = [rough, toksvig, exponent_row, factor, reference = SpecularExponent](int X) {
      const double base = rough ? double(exponent_row[X]) : reference;
      return toksvig ? base * double(factor[X]) : base;
   };
   auto pixel_gain = [rough, gain_row](int X) { return rough ? double(gain_row[X]) : 1.0; };

   for (int y=StartY; y < EndY; ++y) {
      const auto &cur = slots[y % 3];
      if (rough) roughness_row(Surface, y, SpecularExponent, exponent_row, gain_row, Width);
      if (toksvig) {
         compute_to(std::min(y + 1, last_row));
         const auto &above = slots[std::max(y - 1, 0) % 3];
         const auto &below = slots[std::min(y + 1, last_row) % 3];
         if (rough) {
            row_toksvig<true>(above.x + 1, cur.x + 1, below.x + 1, above.y + 1, cur.y + 1, below.y + 1,
               above.z + 1, cur.z + 1, below.z + 1, exponent_row, mean_x, mean_y, mean_z, factor, Width);
         }
         else {
            row_toksvig<false>(above.x + 1, cur.x + 1, below.x + 1, above.y + 1, cur.y + 1, below.y + 1,
               above.z + 1, cur.z + 1, below.z + 1, &exponent, mean_x, mean_y, mean_z, factor, Width);
         }
      }
      else compute_to(y);

      const float *nx = cur.x + 1, *ny = cur.y + 1, *nz = cur.z + 1;
      auto dptr = dest_base + y * Target->LineWidth;
      if (normal_map) {
         auto encode = [](float Component, float Fallback) -> uint8_t {
            const float value = std::isfinite(Component) ? std::clamp(Component, -1.0f, 1.0f) : Fallback;
            return uint8_t(NORMAL_MAP_CENTRE + NORMAL_MAP_RANGE * value + 0.5f);
         };
         for (int x=0; x < Width; ++x, dptr += bpp) {
            dptr[R] = encode(nx[x], 0);
            dptr[G] = encode(ny[x], 0);
            dptr[B] = encode(nz[x], 1);
            dptr[A] = 255;
         }
         continue;
      }

      // Cavity is the occlusion from curvature alone.  Light is reduced by both cavity and shadow.  Indirect light,
      // which is the ambient light and the matcap, is reduced by the cavity and the ambient occlusion map.

      const float *h = row_of(Surface.heights, y);
      const float *cavity = cur.occlusion + 1;
      const float *indirect = cavity;
      if (Surface.occlusion) {
         occlusion_row(Surface, y, cavity, indirect_row, Width);
         indirect = indirect_row;
      }
      const float *shadow = Surface.shadow ? Surface.shadow + (y * Width) : nullptr;
      const float *raised = Surface.raised ? Surface.raised + (y * Width) : nullptr;
      const uint8_t *albedo = ((Surface.albedo) and (y < Surface.albedo_height)) ?
         Surface.albedo + (std::ptrdiff_t(y) * Surface.albedo_stride) : nullptr;

      if (not Surface.lights.empty()) { // Several lights; only the COMBINED type accepts them
         for (int x=0; x < Width; ++x, dptr += bpp) {
            const point3 normal(nx[x], ny[x], nz[x]);
            const auto sample = read_albedo(Surface, albedo, x, R, G, B, A);
            const point3 mean = toksvig ? point3(mean_x[x], mean_y[x], mean_z[x]) : normal;
            enhanced_lights(Surface, normal, mean, pixel_exponent(x), pixel_gain(x), sample, x, y, h[x], cavity[x],
               indirect[x], dptr, R, G, B, A);
         }
         continue;
      }

      for (int x=0; x < Width; ++x, dptr += bpp) {
         const point3 normal(nx[x], ny[x], nz[x]);
         const double falloff = Falloff(x, y, h[x]); // Exactly 1 for an unlimited light
         const double occlusion = double(shadow ? cavity[x] * shadow[x] : cavity[x]) * falloff;
         point3 stl = Delta(x, y, h[x]);
         if (Type IS LT::DIFFUSE) {
            if (ambient) enhanced_ambient(normal, stl, Tint(stl), dptr, R, G, B, A, occlusion, indirect[x]);
            else enhanced_diffuse(normal, stl, Tint(stl), dptr, R, G, B, A, occlusion);
         }
         else if (Type IS LT::COMBINED) {
            const auto sample = read_albedo(Surface, albedo, x, R, G, B, A);
            const point3 mean = toksvig ? point3(mean_x[x], mean_y[x], mean_z[x]) : normal;
            const double exponent = pixel_exponent(x), gain = pixel_gain(x);
            if (Material) {
               const double lifted = double(raised ? cavity[x] * raised[x] : cavity[x]) * falloff;
               enhanced_material(normal, mean, exponent, gain, stl, Tint(stl), sample, dptr, R, G, B, A, occlusion,
                  indirect[x], lifted);
            }
            else {
               enhanced_combined(normal, mean, exponent, gain, stl, Tint(stl), sample, dptr, R, G, B, A, occlusion,
                  indirect[x]);
            }
         }
         else if (toksvig) {
            enhanced_specular(point3(mean_x[x], mean_y[x], mean_z[x]), stl, Tint(stl), dptr, R, G, B, A,
               occlusion, pixel_exponent(x), pixel_gain(x));
         }
         else enhanced_specular(normal, stl, Tint(stl), dptr, R, G, B, A, occlusion, pixel_exponent(x),
            pixel_gain(x));
      }
   }
}

//********************************************************************************************************************

bool extLightingFX::boost_detail(double Sigma) const
{
   return ((Features & LFX::DETAIL) != LFX::NIL) and (Detail != 1.0) and (Sigma >= 0.5);
}

// Sigma in pixels of the blur applied to the heights before curvature is measured, or 0 if no blur is required.

double extLightingFX::cavity_sigma(double UnitSize) const
{
   if (Type IS LT::NORMAL) return 0;
   if (((Features & LFX::CAVITY) IS LFX::NIL) or (Cavity <= 0) or (UnitSize <= 1.0)) return 0;
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
// gradient reaches 2 px into heights that are usually dequantised over 1 px, and the specular Toksvig factor averages the
// normals over a further 1 px.  The detail layer and the curvature blur add the larger of their blur radii.  Each
// addition applies only if its feature is enabled.
//
// Beyond that shared support, shadows read up to the length of the longest shadow of a distant light, limited to
// MAX_SHADOW_LENGTH, and a ranged light reads twice its range.  The nearest sample to the light normalises its
// attenuation, and any two samples within the light's sphere are no further apart than its diameter, so heights within
// that distance of a lit pixel are required.  The larger of the two reaches is added.

double extLightingFX::reach(filter_surface_format PrimaryFormat) const
{
   if (not enhanced(PrimaryFormat)) return 2;

   const double scale = Filter->Render->ToScene.scale();
   const double unit_size = unit_length(kernel_unit_pixels(scale));
   const double sigma = std::min(MAX_SIGMA, DETAIL_SIGMA * unit_size);
   double blur = cavity_sigma(unit_size);
   if (boost_detail(sigma)) blur = std::max(blur, sigma);
   const int dequantise_reach = ((Features & LFX::DEQUANTISE_WIDE) != LFX::NIL) ? MAX_TERRACE : 1;
   const double result = 3 + dequantise_reach + std::ceil(blur * 3.0);
   if (Type IS LT::NORMAL) return result;

   double shadow_reach = 0, range_reach = 0;
   const bool shadows = (Features & LFX::SHADOWS) != LFX::NIL;
   for (size_t i=0; i < light_count(); i++) {
      const auto &light = Lights[i];
      if (light.Kind IS LS::DISTANT) {
         if ((not shadows) or (not light.Shadows)) continue;
         const double elevation = light.Elevation * DEG2RAD;
         const double lowest = elevation - std::min(Penumbra * DEG2RAD, elevation * 0.5);
         if ((elevation > 0) and (elevation < 89.0 * DEG2RAD)) {
            shadow_reach = std::max(shadow_reach,
               std::ceil(std::min(MAX_SHADOW_LENGTH, std::abs(MapHeight) * scale / tan(lowest))));
         }
      }
      else if (light.Range > 0) {
         const double range = std::min(scaled_range(light.Range), std::numeric_limits<double>::max() * 0.5);
         range_reach = std::max(range_reach, std::ceil(2.0 * range));
      }
   }

   return result + std::max(shadow_reach, range_reach);
}

//********************************************************************************************************************
// Load a row of native heights in alpha-level units, aligned to the lighting output in scene coordinates.

static void load_lighting_height_row(const filter_surface_view &Source, float *Dest, int SceneX, int SceneY, int Width)
{
   const int y = SceneY - Source.OriginY;
   const int x = SceneX - Source.OriginX;
   std::fill_n(Dest, Width, 0.0f);
   if ((y < Source.Valid.top) or (y >= Source.Valid.bottom)) return;
   const auto row = Source.row(y);
   const int first = std::max(0, Source.Valid.left - x);
   const int end = std::min(Width, Source.Valid.right - x);
   for (int i=first; i < end; ++i) Dest[i] = 255.0f * filter_height_sample<true>(row, x + i, 0);
}

//********************************************************************************************************************

ERR extLightingFX::draw_enhanced(const filter_surface_view &Source, const filter_surface_view *Albedo,
   const filter_surface_view *Normals, const filter_map_view *Occlusion, const filter_map_view *Roughness,
   std::span<lit_light> Prepared, double SpotHeight, const map_height &NormalHeight, double UnitSize, int Width,
   int Height)
{
   const int stride = Width + (LIGHT_PAD * 2);
   const int rows   = Height + (LIGHT_PAD * 2);
   const size_t size = size_t(stride) * rows;
   const bool scalar = Source.Format IS filter_surface_format::SCALAR_F32;
   const bool wide = (not scalar) and ((Features & LFX::DEQUANTISE_WIDE) != LFX::NIL);
   const bool legacy = (not scalar) and ((Features & LFX::DEQUANTISE) != LFX::NIL);
   const bool dequantise = wide or legacy;

   // The diffuse and specular output functions expect each channel of the light in the range 0 - 1, and apply the
   // intensity to the light's strength.  A spot light only scales the colour down, so limiting it here is sufficient.
   // The combined type takes the light at its full intensity and clamps on output.  A light without a colour of its
   // own uses the effect's colour.

   for (size_t i=0; i < Prepared.size(); i++) {
      const auto &record = Lights[i];
      auto &light = Prepared[i];
      light.colour = record.HasColour ? (Linear ? record.LinearColour : record.Colour) : LightColour;

      if (Type IS LT::COMBINED) {
         light.colour.Red   = std::max(0.0f, float(light.colour.Red * record.Intensity));
         light.colour.Green = std::max(0.0f, float(light.colour.Green * record.Intensity));
         light.colour.Blue  = std::max(0.0f, float(light.colour.Blue * record.Intensity));
         light.strength = Constant;
      }
      else {
         light.colour.Red   = std::clamp(light.colour.Red, 0.0f, 1.0f);
         light.colour.Green = std::clamp(light.colour.Green, 0.0f, 1.0f);
         light.colour.Blue  = std::clamp(light.colour.Blue, 0.0f, 1.0f);
         light.strength = Constant * record.Intensity;
      }
   }

   const lit_light no_light;
   const auto &first = Prepared.empty() ? no_light : Prepared.front();
   const bool multiple = Prepared.size() > 1;
   Strength = first.strength;
   Material = has_material();
   WrapScale = 1.0 / (1.0 + Wrap);

   // Ambient light in the filter's colour space, scaled by its alpha.

   AmbientLight = { 0, 0, 0, 0 };
   if (has_ambient()) {
      FRGB ambient = { std::max(0.0f, Ambient.Red), std::max(0.0f, Ambient.Green), std::max(0.0f, Ambient.Blue), 1 };
      if (Linear) glLinearRGB.convert(ambient);
      const float level = std::min(Ambient.Alpha, 1.0f);
      AmbientLight = { ambient.Red * level, ambient.Green * level, ambient.Blue * level, 1 };
   }

   // The albedo's channels are converted to the filter's colour space through a table.

   std::array<float, 256> albedo_lut;
   if (Albedo) colour_table(albedo_lut, Albedo->Encoding IS CS::LINEAR_RGB, Linear);

   // The matcap's colour, for the combined type.

   MatcapTexels = nullptr;
   if ((Material) and (Matcap)) {
      if (auto error = prepare_matcap(); error != ERR::Okay) return error;
   }

   float *raw;
   if (auto error = get_filter_scratch(this, size, raw); error != ERR::Okay) return error;

   float *heights = raw;
   if (dequantise) {
      if (auto error = get_filter_scratch(this, size, heights); error != ERR::Okay) return error;
   }

   const auto bitmap = Source.Bitmap;
   const auto &origin = Filter->Render->VectorClip;
   const int scene_x = origin.left + Target->Clip.Left;
   const int scene_y = origin.top + Target->Clip.Top;

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

   // Load heights in alpha-level units, replicating the processing boundary into the padding.

   for_rows(rows, [&](int Start, int End) {
      for (int py=Start; py < End; ++py) {
         const int y = std::clamp(py - LIGHT_PAD, 0, Height - 1);
         float *dest = raw + py * stride;
         float *interior = dest + LIGHT_PAD;

         if (scalar) load_lighting_height_row(Source, interior, scene_x, scene_y + y, Width);
         else {
            const int alpha = bitmap->ColourFormat->AlphaPos >> 3;
            const int bpp = bitmap->BytesPerPixel;
            const auto src = Source.row(bitmap->Clip.Top + y) + std::ptrdiff_t(bitmap->Clip.Left) * bpp + alpha;
            for (int x=0; x < Width; ++x) interior[x] = src[x * bpp];
         }

         std::fill_n(dest, LIGHT_PAD, interior[0]);
         std::fill_n(interior + Width, LIGHT_PAD, interior[Width - 1]);
      }
   });

   // Reconstruct terraces across both axes.  When legacy dequantisation is enabled, leave unresolved samples marked
   // for the local smoothing pass.

   if (wide) {
      for_rows(rows, [&](int Start, int End) {
         for (int y=Start; y < End; ++y) {
            float *dest = heights + y * stride;
            reconstruct_terraces(raw + y * stride, stride, 1, [&](int X, float Value, bool Reconstructed) {
               dest[X] = Reconstructed ? Value : -1.0f; // A negative value marks an unresolved sample.
            });
         }
      });

      for_rows(stride, [&](int Start, int End) {
         for (int x=Start; x < End; ++x) {
            reconstruct_terraces(raw + x, rows, stride, [&](int Y, float Value, bool Reconstructed) {
               auto &dest = heights[std::ptrdiff_t(Y) * stride + x];
               if (dest < 0.0f) {
                  if (Reconstructed or (not legacy)) dest = Value;
               }
               else if (Reconstructed) dest = (dest + Value) * 0.5f;
            });
         }
      });
   }

   if (legacy) for_rows(rows, [&](int Start, int End) {
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

         if ((not wide) or (dest[0] < 0.0f)) dest[0] = dequantise_edge(0, 0, 1);

         if (wide) dequantise_span<true>(r0, r1, r2, dest, 1, stride - 1);
         else dequantise_span<false>(r0, r1, r2, dest, 1, stride - 1);

         if ((not wide) or (dest[stride - 1] < 0.0f)) {
            dest[stride - 1] = dequantise_edge(stride - 1, stride - 2, stride - 1);
         }
      }
   });

   // Surface positions for point and spot lights are measured from the dequantised height.

   auto surface_delta = [&first, SpotHeight](double X, double Y, float Alpha) -> point3 {
      const auto &lt = first.position;
      point3 direction(lt.x - X, lt.y - Y, lt.z - (double(Alpha) * (1.0 / 255.0) * SpotHeight));
      direction.normalise();
      return direction;
   };

   // A ranged light lights a sample that coincides with it from directly above.

   auto ranged_delta = [&first, SpotHeight](double X, double Y, float Alpha) -> point3 {
      const auto &lt = first.position;
      point3 direction(lt.x - X, lt.y - Y, lt.z - (double(Alpha) * (1.0 / 255.0) * SpotHeight));
      if (direction.dot(direction) < LIGHT_COINCIDENCE_SQUARED) return point3(0, 0, 1);
      direction.normalise();
      return direction;
   };

   auto ranged_falloff = [&first, SpotHeight](double X, double Y, float Alpha) -> double {
      const auto &lt = first.position;
      const point3 delta(lt.x - X, lt.y - Y, lt.z - (double(Alpha) * (1.0 / 255.0) * SpotHeight));
      return first.attenuation(delta.dot(delta));
   };

   // Detail layer for the boost.  The raw buffer is no longer needed once it has been dequantised, so it serves as the
   // blur's work space.

   float *detail = nullptr;
   const double sigma = std::min(MAX_SIGMA, DETAIL_SIGMA * UnitSize);
   const bool boost = boost_detail(sigma);
   const double curve_sigma = cavity_sigma(UnitSize);
   float *work = dequantise ? raw : nullptr;

   if ((boost or (curve_sigma > 0)) and (not work)) {
      if (auto error = get_filter_scratch(this, size, work); error != ERR::Okay) return error;
   }

   if (boost) {
      if (auto error = get_filter_scratch(this, size, detail); error != ERR::Okay) return error;
      detail_layer(heights, detail, work, stride, rows, sigma, for_rows);
   }

   // Heights for measuring curvature, blurred to the scale of the kernel unit if it covers more than a pixel.

   const float *curve = heights;
   if (curve_sigma > 0) {
      float *blurred;
      if (auto error = get_filter_scratch(this, size, blurred); error != ERR::Okay) return error;
      gaussian_blur(heights, blurred, work, stride, rows, curve_sigma, for_rows, [](int) { });
      curve = blurred;
   }

   // Shadows for distant lights, one map per light that casts them.  Heights are converted to pixels with the same
   // scale that positions the surface relative to point and spot lights.  A negative surface scale inverts the
   // surface, which is not supported.  The maps remain in scratch memory until the draw completes.
   //
   // With Wrap, faces that turn away from a light take their shadows from the light raised by the wrap angle, which
   // their own slope does not block.  The raised light is unshadowed within 1 degree of overhead.

   const bool shadows = ((Features & LFX::SHADOWS) != LFX::NIL) and (SpotHeight > 0);
   const float *surface_heights = heights + (LIGHT_PAD * stride) + LIGHT_PAD;

   for (size_t i=0; i < Prepared.size(); i++) {
      auto &light = Prepared[i];
      const double elevation = light.elevation;
      if ((light.kind != LS::DISTANT) or (not shadows) or (not Lights[i].Shadows) or (elevation <= 0) or
          (elevation >= 89.0 * DEG2RAD)) continue;

      float *shadow;
      if (auto error = get_filter_scratch(this, size_t(Width) * Height, shadow); error != ERR::Okay) return error;
      auto error = cast_shadows(this, surface_heights, stride, Width, Height, shadow, light.direction, elevation,
         SpotHeight * (1.0 / 255.0), pool);
      if (error != ERR::Okay) return error;
      light.shadow = shadow;

      light.wrap_shadows = (Material) and (Wrap > 0);
      if (light.wrap_shadows) {
         const double lifted = elevation + asin(Wrap);
         if (lifted < 89.0 * DEG2RAD) {
            float *raised;
            if (auto error = get_filter_scratch(this, size_t(Width) * Height, raised); error != ERR::Okay) return error;

            auto error = cast_shadows(this, surface_heights, stride, Width, Height, raised, light.direction, lifted,
               SpotHeight * (1.0 / 255.0), pool);
            if (error != ERR::Okay) return error;
            light.raised = raised;
         }
         light.wrap_blend = sin(std::min(Penumbra * DEG2RAD, elevation * 0.5));
      }
   }

   // Shadows for point and spot lights.  The light's height is converted to alpha levels.  The penumbra's light
   // heights are chosen at the centre of the filter region, at height 0, so that they do not depend on the clip.
   // Lights at the same position share their maps.

   const bool local_shadows = ((Features & LFX::LOCAL_SHADOWS) != LFX::NIL) and (SpotHeight > 0);
   if (local_shadows) {
      const auto &render = *Filter->Render;
      double anchor_x = (render.FilterRegion.left + render.FilterRegion.right) * 0.5;
      double anchor_y = (render.FilterRegion.top + render.FilterRegion.bottom) * 0.5;
      render.Transform.transform(&anchor_x, &anchor_y);
      anchor_x -= Target->Clip.Left;
      anchor_y -= Target->Clip.Top;
      const double to_levels = 255.0 / SpotHeight;

      auto launch = [&pool](int Count, auto &&Function) {
         for (int t=0; t < Count; ++t) pool.detach_task([&Function, t]() { Function(t); });
         pool.wait();
      };

      // Sweeps one map for Source, which is in client units on the vertical axis

      auto sweep = [&](local_shadows::light Source, float *&Map) -> ERR {
         for (auto &z : Source.z) z = float(z * to_levels);
         if (auto error = get_filter_scratch(this, size_t(Width) * Height, Map); error != ERR::Okay) return error;
         const local_shadows::field field { surface_heights, stride, Width, Height, Map };
         if (not local_shadows::sweep_parallel(field, Source, int(pool.get_thread_count()), launch)) {
            return ERR::AllocMemory;
         }
         return ERR::Okay;
      };

      for (size_t i=0; i < Prepared.size(); i++) {
         auto &light = Prepared[i];
         if ((light.kind IS LS::DISTANT) or (not Lights[i].Shadows)) continue;

         bool shared = false;
         for (size_t j=0; j < i; j++) {
            const auto &other = Prepared[j];
            if ((other.kind != LS::DISTANT) and (other.shadow) and (other.position.x IS light.position.x) and
                (other.position.y IS light.position.y) and (other.position.z IS light.position.z)) {
               light.shadow = other.shadow;
               light.raised = other.raised;
               light.wrap_shadows = other.wrap_shadows;
               light.wrap_blend = other.wrap_blend;
               shared = true;
               break;
            }
         }
         if (shared) continue;

         const auto &lt = light.position;
         if (not local_shadows::valid_projection(lt.x, lt.y)) return ERR::OutOfRange;
         const double penumbra = Penumbra * DEG2RAD;
         float *shadow;
         if (auto error = sweep(local_shadows::penumbra_light(lt.x, lt.y, lt.z, anchor_x, anchor_y, 0, penumbra),
            shadow); error != ERR::Okay) return error;
         light.shadow = shadow;

         // As for distant lights, with the elevation seen at the anchor

         const double elevation = local_shadows::anchor_elevation(lt.x, lt.y, lt.z, anchor_x, anchor_y, 0);
         light.wrap_shadows = (Material) and (Wrap > 0) and (elevation > 0);
         if (light.wrap_shadows) {
            const double lift = asin(Wrap);
            if (elevation + lift < 89.0 * DEG2RAD) {
               float *raised;
               if (auto error = sweep(local_shadows::penumbra_light(lt.x, lt.y, lt.z, anchor_x, anchor_y, 0,
                  penumbra, lift), raised); error != ERR::Okay) return error;
               light.raised = raised;
            }
            light.wrap_blend = sin(std::min(penumbra, elevation * 0.5));
         }
      }
   }

   WrapShadows = first.wrap_shadows;
   WrapBlend = first.wrap_blend;

   // The nearest sample to each ranged light normalises its attenuation.  It is found among the samples of the
   // surface, excluding the padding, within the light's range.  Rows are reduced in chunks and the chunk minima are
   // combined, so the result does not depend on the order in which the chunks complete.

   std::vector<double> minima;
   for (auto &light : Prepared) {
      if ((light.kind IS LS::DISTANT) or (not (light.range > 0))) continue;

      const auto &lt = light.position;
      const double range = light.range;
      const int x0 = int(std::clamp(std::floor(lt.x - range), 0.0, double(Width)));
      const int x1 = int(std::clamp(std::ceil(lt.x + range) + 1.0, 0.0, double(Width)));
      const int y0 = int(std::clamp(std::floor(lt.y - range), 0.0, double(Height)));
      const int y1 = int(std::clamp(std::ceil(lt.y + range) + 1.0, 0.0, double(Height)));
      double nearest = std::numeric_limits<double>::infinity();

      if ((x0 < x1) and (y0 < y1)) {
         minima.assign((Height + chunk_size - 1) / chunk_size, std::numeric_limits<double>::infinity());
         for_rows(Height, [&](int Start, int End) {
            double result = std::numeric_limits<double>::infinity();
            for (int y=std::max(Start, y0); y < std::min(End, y1); ++y) {
               const float *h = heights + ((y + LIGHT_PAD) * stride) + LIGHT_PAD;
               const double dy = lt.y - y;
               for (int x=x0; x < x1; ++x) {
                  const double dx = lt.x - x;
                  const double dz = lt.z - (double(h[x]) * (1.0 / 255.0) * SpotHeight);
                  result = std::min(result, (dx * dx) + (dy * dy) + (dz * dz));
               }
            }
            minima[Start / chunk_size] = result;
         });
         for (double value : minima) nearest = std::min(nearest, value);
      }

      light.nearest = sqrt(nearest);
      light.reaches = light.nearest < range;
      light.nearest_term = std::max(nearest, LIGHT_COINCIDENCE_SQUARED);
   }

   lit_surface surface = {
      .heights = heights, .detail = detail, .curve = curve, .shadow = first.shadow, .raised = first.raised,
      .albedo = nullptr, .albedo_lut = albedo_lut.data(), .albedo_stride = 0, .albedo_width = 0, .albedo_height = 0,
      .albedo_premul = false, .stride = stride, .height = Height, .normal = NormalHeight,
      .curvature = MapHeight * (1.0 / 255.0) * UnitSize * UnitSize, .sharpen = sharpen_weight(UnitSize),
      .lights = multiple ? std::span<const lit_light>(Prepared) : std::span<const lit_light>(),
      .spot_height = SpotHeight, .width = Width, .normal_map = Normals,
      .normal_strength = NormalStrength, .flip_normal_y = NormalConvention IS NMC::Y_UP,
      // The map is skipped without indirect light to occlude.  With a strength of zero it would change nothing.
      .occlusion = ((has_ambient() or MatcapTexels) and (OcclusionStrength > 0)) ? Occlusion : nullptr,
      .occlusion_strength = OcclusionStrength, .roughness = Roughness,
      .scene_x = scene_x, .scene_y = scene_y
   };

   if (Albedo) { // The albedo is aligned with the height input, as for the other dual-input effects
      const auto mix = Albedo->Bitmap;
      surface.albedo = mix->Data + (mix->Clip.Left * 4) + (mix->Clip.Top * mix->LineWidth);
      surface.albedo_stride = mix->LineWidth;
      surface.albedo_width  = std::max(0, mix->Clip.Right - mix->Clip.Left);
      surface.albedo_height = std::max(0, mix->Clip.Bottom - mix->Clip.Top);
      surface.albedo_premul = Albedo->Alpha IS filter_alpha::PREMULTIPLIED;
   }

   const size_t chunk_floats = size_t(Width + 2) * LIGHT_ROW_BUFFERS;
   const size_t chunks = (Height + chunk_size - 1) / chunk_size;
   float *row_buffers;

   if (auto error = get_filter_scratch(this, chunks * chunk_floats, row_buffers); error != ERR::Okay) return error;

   // A single light is rendered with functions specialised for its kind.  Several lights are rendered by
   // enhanced_lights(), which does not use the functions.

   auto unlimited = [](double, double, float) -> double { return 1.0; };
   auto colour = [&first](const point3 &) -> const FRGB & { return first.colour; };
   auto spot_colour = [&first](const point3 &STL) { return first.spot_colour(STL); };

   for_rows(Height, [&](int Start, int End) {
      auto buffer = row_buffers + (Start / chunk_size) * chunk_floats;
      if ((multiple) or (first.kind IS LS::DISTANT)) {
         render_enhanced(Start, End, surface, Width, buffer,
            [&first](int, int, float) { return first.direction; }, colour, unlimited);
      }
      else if (first.range > 0) {
         if (first.kind IS LS::SPOT) {
            render_enhanced(Start, End, surface, Width, buffer, ranged_delta, spot_colour, ranged_falloff);
         }
         else render_enhanced(Start, End, surface, Width, buffer, ranged_delta, colour, ranged_falloff);
      }
      else if (first.kind IS LS::SPOT) {
         render_enhanced(Start, End, surface, Width, buffer, surface_delta, spot_colour, unlimited);
      }
      else render_enhanced(Start, End, surface, Width, buffer, surface_delta, colour, unlimited);
   });

   return ERR::Okay;
}
