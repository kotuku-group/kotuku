/*********************************************************************************************************************

-CLASS-
GradientDiffusion: Diffusion curves paint server [BETA].

This class is in beta and is not yet fully supported.  It is available for experimentation and feedback.

GradientDiffusion implements diffusion curves, a vector representation for smooth-shaded images (Orzan et al.,
SIGGRAPH 2008).  The client supplies a sparse set of cubic Bezier curves through #Curves, with an independent colour
assigned to each side of every curve.  The colours diffuse outward from the curves until they meet, producing smooth,
organic shading across the fill region; each curve acts simultaneously as a colour source and as a barrier that
prevents the colours of other curves from bleeding across it.

"Left" and "right" are relative to the direction of travel from the curve's start point to its end point, i.e. the
left side is the side pointed to by the normal `(-dy, dx)`.  Colours are interpolated linearly along the length of
each curve from its start colour to its end colour.  Multi-segment curves are expressed as consecutive records that
share endpoints and colours.

The diffusion is evaluated by relaxing a Laplace equation over a cached raster grid.  The grid is re-solved only when
the curve set or solve parameters change, so static gradients render at full speed after the first frame.  The
@Gradient.Resolution field scales the solve grid's resolution: at `0` the longest axis receives 128 texels, rising to
512 texels at `1.0`.

The inherited colour-ramp fields `Stops`, `ColourMap`, `SpreadMethod` and `Colour` are not supported by this class.
All colour information is supplied through #Curves.

-END-

*********************************************************************************************************************/

static void invalidate_diffusion(extGradientDiffusion *Self) noexcept
{
   Self->FieldHash = 0;
   if (Self->initialised()) Self->modified();
}

//********************************************************************************************************************
// Drop fields that are irrelevant to diffusion gradients.

static ERR GRADIENTDIFFUSION_SET_Colour(extGradientDiffusion *, const std::span<const float> &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTDIFFUSION_SET_ColourMap(extGradientDiffusion *, const std::string_view &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTDIFFUSION_SET_SpreadMethod(extGradientDiffusion *, VSPREAD)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

static ERR GRADIENTDIFFUSION_SET_Stops(extGradientDiffusion *, const std::span<const GradientStop> &)
{
   return kt::Log().warning(ERR::UnsupportedField);
}

//********************************************************************************************************************

static FRGB diffusion_clamp_colour(const FRGB &Colour)
{
   return FRGB(std::clamp(Colour.Red, 0.0f, 1.0f), std::clamp(Colour.Green, 0.0f, 1.0f),
      std::clamp(Colour.Blue, 0.0f, 1.0f), std::clamp(Colour.Alpha, 0.0f, 1.0f));
}

static DiffusionCurveRecord diffusion_curve_to_record(const DiffusionCurve &Curve)
{
   DiffusionCurveRecord record = {};

   record.Curve.StartX = Curve.p0.x;  record.Curve.StartY = Curve.p0.y;
   record.Curve.X1     = Curve.c0.x;  record.Curve.Y1     = Curve.c0.y;
   record.Curve.X2     = Curve.c1.x;  record.Curve.Y2     = Curve.c1.y;
   record.Curve.EndX   = Curve.p1.x;  record.Curve.EndY   = Curve.p1.y;

   record.LeftStart  = Curve.left_start;
   record.LeftEnd    = Curve.left_end;
   record.RightStart = Curve.right_start;
   record.RightEnd   = Curve.right_end;

   return record;
}

static DiffusionCurve record_to_diffusion_curve(const DiffusionCurveRecord &Record)
{
   DiffusionCurve curve;

   curve.p0 = { Record.Curve.StartX, Record.Curve.StartY };
   curve.c0 = { Record.Curve.X1, Record.Curve.Y1 };
   curve.c1 = { Record.Curve.X2, Record.Curve.Y2 };
   curve.p1 = { Record.Curve.EndX, Record.Curve.EndY };

   curve.left_start  = diffusion_clamp_colour(Record.LeftStart);
   curve.left_end    = diffusion_clamp_colour(Record.LeftEnd);
   curve.right_start = diffusion_clamp_colour(Record.RightStart);
   curve.right_end   = diffusion_clamp_colour(Record.RightEnd);

   return curve;
}

/*********************************************************************************************************************

-FIELD-
Curves: Defines the diffusion curves and their per-side colours.

The Curves array defines one or more diffusion curves.  Each !DiffusionCurveRecord carries the cubic Bezier geometry
of one curve segment in its `Curve` member, plus a start and end colour for each side of the curve: `LeftStart`,
`LeftEnd`, `RightStart` and `RightEnd`.  Colours are interpolated linearly along the curve's arc length, and the left
side is the side pointed to by the normal `(-dy, dx)` when travelling from the curve's start to its end.

The coordinate system for curve positions is determined by @Gradient.Units.  Under `BOUNDING_BOX`, normalised curve
coordinates in the range `0 - 1.0` are scaled into the target path's bounds.  Under `USERSPACE`, positions are taken
directly in the viewport's coordinate system and the colour field is solved over the target path's bounds.

Colour components are clamped to the range `0 - 1.0` at assignment.  Alpha participates in the diffusion, so
transparent constraints are legal.  Degenerate (zero-length) curve segments are skipped during rendering.

-END-
*********************************************************************************************************************/

static ERR GRADIENTDIFFUSION_GET_Curves(extGradientDiffusion *Self, std::span<DiffusionCurveRecord> &Array)
{
   thread_local std::vector<DiffusionCurveRecord> records;
   records.clear();

   records.reserve(Self->Curves.size());
   for (auto &curve : Self->Curves) records.push_back(diffusion_curve_to_record(curve));

   Array = std::span<DiffusionCurveRecord>(records.data(), records.size());
   return ERR::Okay;
}

static ERR GRADIENTDIFFUSION_SET_Curves(extGradientDiffusion *Self, std::span<const DiffusionCurveRecord> &Array)
{
   if ((not Array.data()) and (not Array.empty())) return kt::Log().warning(ERR::InvalidValue);

   Self->Curves.clear();
   Self->Curves.reserve(Array.size());
   for (auto &record : Array) Self->Curves.push_back(record_to_diffusion_curve(record));

   invalidate_diffusion(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-
*********************************************************************************************************************/

static ERR GRADIENTDIFFUSION_GET_XMLDef(extGradientDiffusion *, std::string &Value)
{
   Value = "diffusionGradient";
   return ERR::Okay;
}

//********************************************************************************************************************
// Multigrid Laplace solver (Orzan et al. SIGGRAPH 2008, section 4, adapted to a cached CPU solve).
//
// The colour constraints are rasterised into the finest grid as premultiplied float RGBA deposits (linearised when
// the gradient's colour space is LINEAR_RGB), then a coarse-to-fine pyramid carries the low frequencies so only a
// handful of Jacobi relaxation sweeps are needed per level.  Curves act as barriers implicitly: their texels are
// constrained, so relaxation never propagates colour across a curve's centre line.
//
// All state lives in locals or in the owning gradient object, so concurrent solves on different gradient objects
// are safe.

namespace {

struct DiffusionLevel {
   int width = 0, height = 0;
   std::vector<float> colour;      // Premultiplied RGBA, 4 floats per texel
   std::vector<uint8_t> mask;      // Non-zero if the texel is constrained

   void resize(int Width, int Height) {
      width = Width;
      height = Height;
      colour.assign(size_t(Width) * size_t(Height) * 4, 0.0f);
      mask.assign(size_t(Width) * size_t(Height), 0);
   }
};

inline agg::point_d diffusion_bezier_eval(const DiffusionCurve &Curve, double T)
{
   const double mt = 1.0 - T;
   const double b0 = mt * mt * mt;
   const double b1 = 3.0 * mt * mt * T;
   const double b2 = 3.0 * mt * T * T;
   const double b3 = T * T * T;

   return agg::point_d((Curve.p0.x * b0) + (Curve.c0.x * b1) + (Curve.c1.x * b2) + (Curve.p1.x * b3),
      (Curve.p0.y * b0) + (Curve.c0.y * b1) + (Curve.c1.y * b2) + (Curve.p1.y * b3));
}

// Deposit a premultiplied colour into the accumulation grid.  Samples outside the solve domain are ignored; samples
// exactly on its right or bottom boundary map to the final texel.  Overlapping deposits average via their weight.

inline void diffusion_deposit(std::vector<float> &Accum, std::vector<float> &Weight, int Width, int Height,
   double X, double Y, const float (&Colour)[4])
{
   if ((X < 0.0) or (X > double(Width)) or (Y < 0.0) or (Y > double(Height))) return;

   const int ix = std::clamp(int(std::floor(X)), 0, Width - 1);
   const int iy = std::clamp(int(std::floor(Y)), 0, Height - 1);
   const size_t idx = (size_t(iy) * size_t(Width)) + size_t(ix);

   Accum[idx * 4]     += Colour[0];
   Accum[idx * 4 + 1] += Colour[1];
   Accum[idx * 4 + 2] += Colour[2];
   Accum[idx * 4 + 3] += Colour[3];
   Weight[idx] += 1.0f;
}

// Convert an FRGB constraint colour to premultiplied float RGBA, linearising first when requested.

inline void diffusion_prepare_colour(const FRGB &Colour, bool Linear, float (&Out)[4])
{
   FRGB c = Colour;
   if (Linear) glLinearRGB.convert(c);
   Out[0] = c.Red * c.Alpha;
   Out[1] = c.Green * c.Alpha;
   Out[2] = c.Blue * c.Alpha;
   Out[3] = c.Alpha;
}

// Rasterise every curve's colour constraints into the finest pyramid level.  Each curve is flattened at grid
// resolution and per-sample colours are deposited half a texel to either side of the centre line.

void diffusion_rasterise_constraints(const std::vector<DiffusionCurve> &Curves, const TClipRectangle<double> &Rect,
   bool Linear, DiffusionLevel &Level)
{
   const double scale_x = double(Level.width) / Rect.width();
   const double scale_y = double(Level.height) / Rect.height();

   std::vector<float> accum(Level.colour.size(), 0.0f);
   std::vector<float> weight(size_t(Level.width) * size_t(Level.height), 0.0f);

   std::vector<agg::point_d> points;
   std::vector<double> arc_length;

   for (auto &curve : Curves) {
      // Map control points into grid space.

      DiffusionCurve grid_curve = curve;
      grid_curve.p0 = { (curve.p0.x - Rect.left) * scale_x, (curve.p0.y - Rect.top) * scale_y };
      grid_curve.c0 = { (curve.c0.x - Rect.left) * scale_x, (curve.c0.y - Rect.top) * scale_y };
      grid_curve.c1 = { (curve.c1.x - Rect.left) * scale_x, (curve.c1.y - Rect.top) * scale_y };
      grid_curve.p1 = { (curve.p1.x - Rect.left) * scale_x, (curve.p1.y - Rect.top) * scale_y };

      // The cubic's arc length never exceeds its control polygon length, so sampling the polygon length at
      // <= 0.4 texel spacing guarantees gap-free constraint lines at the finest level.

      auto seg_len = [](const agg::point_d &A, const agg::point_d &B) {
         const double dx = B.x - A.x, dy = B.y - A.y;
         return std::sqrt((dx * dx) + (dy * dy));
      };

      const double poly_len = seg_len(grid_curve.p0, grid_curve.c0) + seg_len(grid_curve.c0, grid_curve.c1) +
         seg_len(grid_curve.c1, grid_curve.p1);

      if (poly_len < 1e-6) continue; // Degenerate zero-length segment; skipped, not rejected.

      const int samples = std::clamp(int(std::ceil(poly_len / 0.4)) + 1, 2, 65536);

      points.clear();
      arc_length.clear();
      points.reserve(samples);
      arc_length.reserve(samples);

      double total = 0;
      for (int i=0; i < samples; i++) {
         auto p = diffusion_bezier_eval(grid_curve, double(i) / double(samples - 1));
         if (not points.empty()) total += seg_len(points.back(), p);
         points.push_back(p);
         arc_length.push_back(total);
      }

      if (total < 1e-6) continue;

      float left_start[4], left_end[4], right_start[4], right_end[4];
      diffusion_prepare_colour(curve.left_start, Linear, left_start);
      diffusion_prepare_colour(curve.left_end, Linear, left_end);
      diffusion_prepare_colour(curve.right_start, Linear, right_start);
      diffusion_prepare_colour(curve.right_end, Linear, right_end);

      for (int i=0; i < samples; i++) {
         // The unit tangent is estimated from the neighbouring samples; the left normal is (-dy, dx).

         const auto &prev = points[(i > 0) ? i - 1 : 0];
         const auto &next = points[(i < samples - 1) ? i + 1 : samples - 1];
         const double dx = next.x - prev.x;
         const double dy = next.y - prev.y;
         const double len = std::sqrt((dx * dx) + (dy * dy));
         if (len < 1e-12) continue;

         const double nx = -dy / len;
         const double ny = dx / len;
         const double t = arc_length[i] / total;

         float left[4], right[4];
         for (int c=0; c < 4; c++) {
            left[c]  = float(left_start[c] + ((left_end[c] - left_start[c]) * t));
            right[c] = float(right_start[c] + ((right_end[c] - right_start[c]) * t));
         }

         diffusion_deposit(accum, weight, Level.width, Level.height,
            points[i].x + (nx * 0.5), points[i].y + (ny * 0.5), left);
         diffusion_deposit(accum, weight, Level.width, Level.height,
            points[i].x - (nx * 0.5), points[i].y - (ny * 0.5), right);
      }
   }

   for (size_t i=0; i < weight.size(); i++) {
      if (weight[i] > 0.0f) {
         const float inv = 1.0f / weight[i];
         Level.colour[i * 4]     = accum[i * 4] * inv;
         Level.colour[i * 4 + 1] = accum[i * 4 + 1] * inv;
         Level.colour[i * 4 + 2] = accum[i * 4 + 2] * inv;
         Level.colour[i * 4 + 3] = accum[i * 4 + 3] * inv;
         Level.mask[i] = 1;
      }
   }
}

// Restriction: build the next-coarser constraint level by averaging constrained child texels only.  A coarse texel
// is constrained if any of its (up to four) children is.

void diffusion_restrict(const DiffusionLevel &Fine, DiffusionLevel &Coarse)
{
   Coarse.resize(std::max(1, (Fine.width + 1) / 2), std::max(1, (Fine.height + 1) / 2));

   for (int y=0; y < Coarse.height; y++) {
      for (int x=0; x < Coarse.width; x++) {
         float sum[4] = { 0, 0, 0, 0 };
         int count = 0;

         for (int cy=0; cy < 2; cy++) {
            for (int cx=0; cx < 2; cx++) {
               const int fx = (x * 2) + cx;
               const int fy = (y * 2) + cy;
               if ((fx >= Fine.width) or (fy >= Fine.height)) continue;

               const size_t fidx = (size_t(fy) * size_t(Fine.width)) + size_t(fx);
               if (not Fine.mask[fidx]) continue;

               for (int c=0; c < 4; c++) sum[c] += Fine.colour[fidx * 4 + c];
               count++;
            }
         }

         if (count) {
            const size_t idx = (size_t(y) * size_t(Coarse.width)) + size_t(x);
            const float inv = 1.0f / float(count);
            for (int c=0; c < 4; c++) Coarse.colour[idx * 4 + c] = sum[c] * inv;
            Coarse.mask[idx] = 1;
         }
      }
   }
}

// Jacobi relaxation: each unconstrained texel becomes the average of its four neighbours, with clamped reads
// providing a Neumann boundary at the grid edges.  Constrained texels are held fixed.

void diffusion_relax(std::vector<float> &Solution, const DiffusionLevel &Constraints, int Iterations)
{
   const int width = Constraints.width;
   const int height = Constraints.height;
   if ((width < 2) and (height < 2)) return;

   std::vector<float> next(Solution.size());

   for (int iter=0; iter < Iterations; iter++) {
      for (int y=0; y < height; y++) {
         const int y0 = (y > 0) ? y - 1 : 0;
         const int y1 = (y < height - 1) ? y + 1 : height - 1;

         for (int x=0; x < width; x++) {
            const size_t idx = (size_t(y) * size_t(width)) + size_t(x);

            if (Constraints.mask[idx]) {
               for (int c=0; c < 4; c++) next[idx * 4 + c] = Solution[idx * 4 + c];
               continue;
            }

            const int x0 = (x > 0) ? x - 1 : 0;
            const int x1 = (x < width - 1) ? x + 1 : width - 1;

            const size_t left   = (size_t(y) * size_t(width)) + size_t(x0);
            const size_t right  = (size_t(y) * size_t(width)) + size_t(x1);
            const size_t top    = (size_t(y0) * size_t(width)) + size_t(x);
            const size_t bottom = (size_t(y1) * size_t(width)) + size_t(x);

            for (int c=0; c < 4; c++) {
               next[idx * 4 + c] = (Solution[left * 4 + c] + Solution[right * 4 + c] +
                  Solution[top * 4 + c] + Solution[bottom * 4 + c]) * 0.25f;
            }
         }
      }

      Solution.swap(next);
   }
}

// Prolongation: bilinearly upsample a coarse solution onto a finer grid.

void diffusion_prolongate(const std::vector<float> &Coarse, int CoarseWidth, int CoarseHeight,
   std::vector<float> &Fine, int FineWidth, int FineHeight)
{
   Fine.resize(size_t(FineWidth) * size_t(FineHeight) * 4);

   const double scale_x = double(CoarseWidth) / double(FineWidth);
   const double scale_y = double(CoarseHeight) / double(FineHeight);

   for (int y=0; y < FineHeight; y++) {
      const double sy = ((double(y) + 0.5) * scale_y) - 0.5;
      const int cy0 = std::clamp(int(std::floor(sy)), 0, CoarseHeight - 1);
      const int cy1 = std::min(cy0 + 1, CoarseHeight - 1);
      const double fy = std::clamp(sy - double(cy0), 0.0, 1.0);

      for (int x=0; x < FineWidth; x++) {
         const double sx = ((double(x) + 0.5) * scale_x) - 0.5;
         const int cx0 = std::clamp(int(std::floor(sx)), 0, CoarseWidth - 1);
         const int cx1 = std::min(cx0 + 1, CoarseWidth - 1);
         const double fx = std::clamp(sx - double(cx0), 0.0, 1.0);

         const size_t i00 = ((size_t(cy0) * size_t(CoarseWidth)) + size_t(cx0)) * 4;
         const size_t i10 = ((size_t(cy0) * size_t(CoarseWidth)) + size_t(cx1)) * 4;
         const size_t i01 = ((size_t(cy1) * size_t(CoarseWidth)) + size_t(cx0)) * 4;
         const size_t i11 = ((size_t(cy1) * size_t(CoarseWidth)) + size_t(cx1)) * 4;

         const size_t out = ((size_t(y) * size_t(FineWidth)) + size_t(x)) * 4;
         for (int c=0; c < 4; c++) {
            const double top = Coarse[i00 + c] + ((Coarse[i10 + c] - Coarse[i00 + c]) * fx);
            const double bottom = Coarse[i01 + c] + ((Coarse[i11 + c] - Coarse[i01 + c]) * fx);
            Fine[out + c] = float(top + ((bottom - top) * fy));
         }
      }
   }
}

// FNV-1a fingerprint over the curve set and solve parameters.

uint64_t diffusion_fingerprint(const std::vector<DiffusionCurve> &Curves, int GridWidth, int GridHeight,
   const TClipRectangle<double> &Rect, VCS ColourSpace)
{
   uint64_t hash = 0xcbf29ce484222325ULL;
   auto mix = [&hash](uint64_t Value) {
      hash ^= Value;
      hash *= 0x100000001b3ULL;
   };

   mix(Curves.size());
   for (auto &curve : Curves) {
      mix(std::bit_cast<uint64_t>(curve.p0.x)); mix(std::bit_cast<uint64_t>(curve.p0.y));
      mix(std::bit_cast<uint64_t>(curve.c0.x)); mix(std::bit_cast<uint64_t>(curve.c0.y));
      mix(std::bit_cast<uint64_t>(curve.c1.x)); mix(std::bit_cast<uint64_t>(curve.c1.y));
      mix(std::bit_cast<uint64_t>(curve.p1.x)); mix(std::bit_cast<uint64_t>(curve.p1.y));

      for (auto colour : { &curve.left_start, &curve.left_end, &curve.right_start, &curve.right_end }) {
         mix(std::bit_cast<uint32_t>(colour->Red));
         mix(std::bit_cast<uint32_t>(colour->Green));
         mix(std::bit_cast<uint32_t>(colour->Blue));
         mix(std::bit_cast<uint32_t>(colour->Alpha));
      }
   }

   mix(uint64_t(GridWidth));
   mix(uint64_t(GridHeight));
   mix(std::bit_cast<uint64_t>(Rect.left));
   mix(std::bit_cast<uint64_t>(Rect.top));
   mix(std::bit_cast<uint64_t>(Rect.width()));
   mix(std::bit_cast<uint64_t>(Rect.height()));
   mix(uint64_t(int(ColourSpace)));

   return hash;
}

} // namespace

//********************************************************************************************************************
// Ensure FieldBuffer holds the solved colour field for the given grid dimensions and solve rectangle.  The solve
// runs in premultiplied float space (linearised for VCS::LINEAR_RGB) and the result is stored as straight-alpha
// rgba8 texels ready for bilinear sampling by the renderer.

void extGradientDiffusion::refresh_field(int GridWidth, int GridHeight, const TClipRectangle<double> &SolveRect)
{
   const uint64_t hash = diffusion_fingerprint(Curves, GridWidth, GridHeight, SolveRect, ColourSpace);
   if ((hash IS FieldHash) and (FieldWidth IS GridWidth) and (FieldHeight IS GridHeight)) return;

   const bool linear = (ColourSpace IS VCS::LINEAR_RGB);

   // Step 1: rasterise the constraints into the finest level.

   std::vector<DiffusionLevel> pyramid;
   pyramid.emplace_back();
   pyramid[0].resize(GridWidth, GridHeight);
   diffusion_rasterise_constraints(Curves, SolveRect, linear, pyramid[0]);

   // Step 2: restriction - constraint pyramid down to <= 4x4.

   while ((pyramid.back().width > 4) or (pyramid.back().height > 4)) {
      DiffusionLevel coarse;
      diffusion_restrict(pyramid.back(), coarse);
      pyramid.push_back(std::move(coarse));
   }

   // Step 3: coarsest level - constrained texels keep their colour, unconstrained texels start from the average
   // constraint colour.

   auto &coarsest = pyramid.back();
   float average[4] = { 0, 0, 0, 0 };
   int constrained = 0;
   for (size_t i=0; i < coarsest.mask.size(); i++) {
      if (coarsest.mask[i]) {
         for (int c=0; c < 4; c++) average[c] += coarsest.colour[i * 4 + c];
         constrained++;
      }
   }
   if (constrained) {
      for (auto &value : average) value /= float(constrained);
   }

   std::vector<float> solution(coarsest.colour.size());
   for (size_t i=0; i < coarsest.mask.size(); i++) {
      for (int c=0; c < 4; c++) {
         solution[i * 4 + c] = coarsest.mask[i] ? coarsest.colour[i * 4 + c] : average[c];
      }
   }
   diffusion_relax(solution, coarsest, 8);

   // Step 4: coarse-to-fine - prolongate, re-impose constraints, relax.  The pyramid carries the low frequencies,
   // so only a few sweeps are needed at each level; the two finest levels receive extra sweeps for edge quality.

   for (int level = int(pyramid.size()) - 2; level >= 0; level--) {
      auto &constraints = pyramid[level];
      std::vector<float> fine;
      diffusion_prolongate(solution, pyramid[level + 1].width, pyramid[level + 1].height,
         fine, constraints.width, constraints.height);
      solution.swap(fine);

      for (size_t i=0; i < constraints.mask.size(); i++) {
         if (constraints.mask[i]) {
            for (int c=0; c < 4; c++) solution[i * 4 + c] = constraints.colour[i * 4 + c];
         }
      }

      diffusion_relax(solution, constraints, (level <= 1) ? 16 : 8);
   }

   // Step 5: quantise the premultiplied float field.  Sampling remains premultiplied; the renderer converts each
   // filtered span to straight alpha afterwards so translucent boundaries do not develop dark fringes.

   FieldBuffer.resize(size_t(GridWidth) * size_t(GridHeight));
   for (size_t i=0; i < FieldBuffer.size(); i++) {
      const float alpha = std::clamp(solution[i * 4 + 3], 0.0f, 1.0f);
      const float red   = std::clamp(solution[i * 4], 0.0f, alpha);
      const float green = std::clamp(solution[i * 4 + 1], 0.0f, alpha);
      const float blue  = std::clamp(solution[i * 4 + 2], 0.0f, alpha);

      FieldBuffer[i] = agg::rgba8(uint8_t((red * 255.0f) + 0.5f), uint8_t((green * 255.0f) + 0.5f),
         uint8_t((blue * 255.0f) + 0.5f), uint8_t((alpha * 255.0f) + 0.5f));
   }

   FieldWidth  = GridWidth;
   FieldHeight = GridHeight;
   FieldHash   = hash;
}

//********************************************************************************************************************

#include "gradient_diffusion_def.cpp"

static const FieldArray clGradientDiffusionFields[] = {
   // Mark these fields as unsupported
   { "Colour",       FDF_SYSTEM|FDF_VIRTUAL|FD_FLOAT|FDF_ARRAY|FD_RW|FDF_PURE, nullptr, GRADIENTDIFFUSION_SET_Colour },
   { "ColourMap",    FDF_SYSTEM|FDF_VIRTUAL|FDF_CPPSTRING|FDF_W|FDF_PURE, nullptr, GRADIENTDIFFUSION_SET_ColourMap },
   { "SpreadMethod", FDF_SYSTEM|FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENTDIFFUSION_SET_SpreadMethod, &clGradientSpreadMethod },
   { "Stops",        FDF_SYSTEM|FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_RW|FDF_PURE, nullptr, GRADIENTDIFFUSION_SET_Stops, "GradientStop" },
   // Diffusion fields
   { "Curves",       FDF_VIRTUAL|FDF_VECTOR|FDF_STRUCT|FDF_RW|FDF_PURE, GRADIENTDIFFUSION_GET_Curves, GRADIENTDIFFUSION_SET_Curves, "DiffusionCurveRecord" },
   { "XMLDef",       FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, GRADIENTDIFFUSION_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_diffusion(void)
{
   clGradientDiffusion = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTDIFFUSION),
      fl::Name("GradientDiffusion"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientDiffusionActions),
      fl::Fields(clGradientDiffusionFields),
      fl::Size(sizeof(extGradientDiffusion)),
      fl::Path(MOD_PATH));

   return clGradientDiffusion ? ERR::Okay : ERR::AddClass;
}
