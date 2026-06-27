#include <cmath>
#include <cstdio>

#include <kotuku/main.h>

#include "agg_conv_stroke.h"
#include "agg_path_storage.h"
#include "agg_stroke_resolve.h"

static double sine_wave_y(double Angle, double Frequency, double Amplitude)
{
   return std::sin(agg::deg2rad(Angle * Frequency)) * Amplitude;
}

static agg::path_storage make_sine_wave(double Frequency, double Length, double Amplitude)
{
   agg::path_storage path;
   constexpr double MAX_PHASE_STEP = 12.0;
   double angle_step = MAX_PHASE_STEP * (1.0 / Frequency);
   if (angle_step > 1.0) angle_step = 1.0;
   if (angle_step <= 0.0) angle_step = 1.0;

   const double xscale = Length * (1.0 / 360.0);
   path.move_to(0.0, sine_wave_y(0.0, Frequency, Amplitude));

   for (double angle=angle_step; angle < 360.0; angle += angle_step) {
      path.line_to(angle * xscale, sine_wave_y(angle, Frequency, Amplitude));
   }

   path.line_to(Length, sine_wave_y(360.0, Frequency, Amplitude));
   return path;
}

static agg::path_storage make_triangle_wave(double Frequency, double Length, double Amplitude)
{
   agg::path_storage path;
   const double xscale = Length * (1.0 / 360.0);
   const double cycle_width = 360.0 * (1.0 / Frequency);

   path.move_to(0.0, 0.0);
   for (double angle=cycle_width * 0.25; angle < 360.0; angle += cycle_width * 0.5) {
      double y = (std::fmod(angle / cycle_width, 1.0) < 0.5) ? Amplitude : -Amplitude;
      path.line_to(angle * xscale, y);
   }
   path.line_to(Length, 0.0);

   return path;
}

static agg::path_storage stroke_path(agg::path_storage &Path, double Width)
{
   agg::conv_stroke<agg::path_storage> stroke(Path);
   stroke.width(Width);
   stroke.line_join(VLJ::ROUND);
   stroke.inner_join(VIJ::ROUND);
   stroke.line_cap(VLC::ROUND);

   agg::path_storage outline;
   outline.concat_path(stroke);
   return outline;
}

static int expect_resolved(const char *Name, agg::path_storage &Path, bool ExpectIntersections)
{
   unsigned before = agg::path_self_intersection_count(Path);
   bool changed = agg::resolve_stroke_self_intersections(Path);
   unsigned after = agg::path_self_intersection_count(Path);

   std::printf("%s: before=%u after=%u changed=%s vertices=%u\n", Name, before, after, changed ? "yes" : "no",
      Path.total_vertices());

   if ((ExpectIntersections) and (before IS 0)) {
      std::fprintf(stderr, "%s: expected self-intersections before resolving\n", Name);
      return 1;
   }

   if ((!ExpectIntersections) and (before != 0)) {
      std::fprintf(stderr, "%s: did not expect self-intersections before resolving\n", Name);
      return 1;
   }

   if (after != 0) {
      std::fprintf(stderr, "%s: resolver left %u self-intersections\n", Name, after);
      return 1;
   }

   return 0;
}

int main()
{
   auto sine = make_sine_wave(20.0, 1024.0 * 0.96, 25.0);
   auto sine_stroke = stroke_path(sine, 10.0);
   if (expect_resolved("smooth-wave", sine_stroke, true) != 0) return 1;

   auto triangle = make_triangle_wave(20.0, 1024.0 * 0.96, 25.0);
   auto triangle_stroke = stroke_path(triangle, 10.0);
   if (expect_resolved("triangle-wave", triangle_stroke, false) != 0) return 1;

   return 0;
}
