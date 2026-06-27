// Copyright (C) 2026 Paul Manias
//
// Permission to copy, use, modify, sell and distribute this software is granted provided this copyright notice
// appears in all copies.  This software is provided "as is" without express or implied warranty, and with no
// claim as to its suitability for any purpose.
// ---
// Materialises stroked outlines and removes simple self-intersection loops. Hooks into conv_stroke output before the
// result is reused as a fillable path. In the vector renderer it prevents internal stroke loops from being traced when
// a generated thick path is subsequently filled and stroked.
//
// TODO: Code is functional for the situations where it is needed, but as a solution (if it is the right solution) is
// not yet fast enough

#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "agg_basics.h"
#include "agg_path_storage.h"

namespace agg {

struct stroke_resolve_point {
   double x;
   double y;
};

struct stroke_resolve_intersection {
   unsigned a;
   unsigned b;
   double t;
   double u;
   stroke_resolve_point point;
};

struct stroke_resolve_loop {
   unsigned a;
   unsigned b;
   stroke_resolve_point point;
};

struct stroke_resolve_options {
   double epsilon = 1.0e-8;
   unsigned max_passes = 256;
};

namespace detail {

   inline bool almost_same(const stroke_resolve_point &A, const stroke_resolve_point &B, double Epsilon) noexcept
   {
      return (std::abs(A.x - B.x) <= Epsilon) and (std::abs(A.y - B.y) <= Epsilon);
   }

   inline double signed_area(const std::vector<stroke_resolve_point> &Points) noexcept
   {
      double area = 0.0;
      const unsigned total = unsigned(Points.size());
      if (total < 3) return 0.0;

      for (unsigned i=0; i < total; i++) {
         const auto &a = Points[i];
         const auto &b = Points[(i + 1) % total];
         area += (a.x * b.y) - (b.x * a.y);
      }

      return area * 0.5;
   }

   inline bool line_intersection(const stroke_resolve_point &A, const stroke_resolve_point &B,
      const stroke_resolve_point &C, const stroke_resolve_point &D, stroke_resolve_intersection &Out,
      double Epsilon) noexcept
   {
      const double ab_x = B.x - A.x;
      const double ab_y = B.y - A.y;
      const double cd_x = D.x - C.x;
      const double cd_y = D.y - C.y;
      const double den = (ab_x * cd_y) - (ab_y * cd_x);
      if (std::abs(den) <= Epsilon) return false;

      const double ac_x = C.x - A.x;
      const double ac_y = C.y - A.y;
      const double t = ((ac_x * cd_y) - (ac_y * cd_x)) / den;
      const double u = ((ac_x * ab_y) - (ac_y * ab_x)) / den;

      if ((t <= Epsilon) or (t >= 1.0 - Epsilon) or (u <= Epsilon) or (u >= 1.0 - Epsilon)) return false;

      Out.t = t;
      Out.u = u;
      Out.point = { A.x + (ab_x * t), A.y + (ab_y * t) };
      return true;
   }

   inline bool boxes_overlap(const stroke_resolve_point &A, const stroke_resolve_point &B,
      const stroke_resolve_point &C, const stroke_resolve_point &D, double Epsilon) noexcept
   {
      const double ab_min_x = std::min(A.x, B.x) - Epsilon;
      const double ab_max_x = std::max(A.x, B.x) + Epsilon;
      const double ab_min_y = std::min(A.y, B.y) - Epsilon;
      const double ab_max_y = std::max(A.y, B.y) + Epsilon;

      const double cd_min_x = std::min(C.x, D.x) - Epsilon;
      const double cd_max_x = std::max(C.x, D.x) + Epsilon;
      const double cd_min_y = std::min(C.y, D.y) - Epsilon;
      const double cd_max_y = std::max(C.y, D.y) + Epsilon;

      return (ab_min_x <= cd_max_x) and (cd_min_x <= ab_max_x) and
         (ab_min_y <= cd_max_y) and (cd_min_y <= ab_max_y);
   }

   inline bool adjacent_edges(unsigned A, unsigned B, unsigned Total) noexcept
   {
      if (A IS B) return true;
      if (A + 1 IS B) return true;
      if (B + 1 IS A) return true;
      return ((A IS 0) and (B + 1 IS Total)) or ((B IS 0) and (A + 1 IS Total));
   }

   inline bool find_self_intersection(const std::vector<stroke_resolve_point> &Points,
      stroke_resolve_intersection &Out, double Epsilon) noexcept
   {
      const unsigned total = unsigned(Points.size());
      if (total < 4) return false;

      for (unsigned i=0; i < total; i++) {
         const auto &a1 = Points[i];
         const auto &a2 = Points[(i + 1) % total];

         for (unsigned j=i + 1; j < total; j++) {
            if (adjacent_edges(i, j, total)) continue;

            const auto &b1 = Points[j];
            const auto &b2 = Points[(j + 1) % total];
            if (!boxes_overlap(a1, a2, b1, b2, Epsilon)) continue;

            stroke_resolve_intersection intersection;
            if (line_intersection(a1, a2, b1, b2, intersection, Epsilon)) {
               intersection.a = i;
               intersection.b = j;
               Out = intersection;
               return true;
            }
         }
      }

      return false;
   }

   inline std::vector<stroke_resolve_point> remove_inner_loop(const std::vector<stroke_resolve_point> &Points,
      const stroke_resolve_intersection &Intersection)
   {
      std::vector<stroke_resolve_point> candidate;
      const unsigned total = unsigned(Points.size());
      candidate.reserve(total);

      for (unsigned i=0; i <= Intersection.a; i++) candidate.push_back(Points[i]);
      candidate.push_back(Intersection.point);
      for (unsigned i=Intersection.b + 1; i < total; i++) candidate.push_back(Points[i]);

      std::vector<stroke_resolve_point> loop;
      loop.reserve(Intersection.b - Intersection.a + 2);
      loop.push_back(Intersection.point);
      for (unsigned i=Intersection.a + 1; i <= Intersection.b; i++) loop.push_back(Points[i]);

      if (std::abs(signed_area(loop)) > std::abs(signed_area(candidate))) return loop;
      return candidate;
   }

   inline unsigned count_self_intersections(const std::vector<stroke_resolve_point> &Points, double Epsilon) noexcept
   {
      const unsigned total = unsigned(Points.size());
      if (total < 4) return 0;

      unsigned intersections = 0;

      for (unsigned i=0; i < total; i++) {
         const auto &a1 = Points[i];
         const auto &a2 = Points[(i + 1) % total];

         for (unsigned j=i + 1; j < total; j++) {
            if (adjacent_edges(i, j, total)) continue;

            const auto &b1 = Points[j];
            const auto &b2 = Points[(j + 1) % total];
            if (!boxes_overlap(a1, a2, b1, b2, Epsilon)) continue;

            stroke_resolve_intersection intersection;
            if (line_intersection(a1, a2, b1, b2, intersection, Epsilon)) intersections++;
         }
      }

      return intersections;
   }

   inline std::vector<stroke_resolve_loop> find_removable_loops(
      const std::vector<stroke_resolve_point> &Points, double Epsilon)
   {
      std::vector<stroke_resolve_loop> loops;
      const unsigned total = unsigned(Points.size());
      if (total < 4) return loops;

      for (unsigned i=0; i < total; i++) {
         const auto &a1 = Points[i];
         const auto &a2 = Points[(i + 1) % total];

         for (unsigned j=i + 1; j < total; j++) {
            if (adjacent_edges(i, j, total)) continue;

            const auto &b1 = Points[j];
            const auto &b2 = Points[(j + 1) % total];
            if (!boxes_overlap(a1, a2, b1, b2, Epsilon)) continue;

            stroke_resolve_intersection intersection;
            if (!line_intersection(a1, a2, b1, b2, intersection, Epsilon)) continue;

            const unsigned span = j - i;
            if (span <= total / 2) loops.push_back({ i, j, intersection.point });
         }
      }

      return loops;
   }

   inline bool remove_loop_batch(std::vector<stroke_resolve_point> &Points, const std::vector<stroke_resolve_loop> &Loops)
   {
      if (Loops.empty()) return false;

      std::vector<stroke_resolve_point> resolved;
      resolved.reserve(Points.size());

      unsigned cursor = 0;
      bool changed = false;

      for (auto &loop : Loops) {
         if (loop.a < cursor) continue;

         for (unsigned i=cursor; i <= loop.a; i++) resolved.push_back(Points[i]);
         resolved.push_back(loop.point);
         cursor = loop.b + 1;
         changed = true;
      }

      for (unsigned i=cursor; i < Points.size(); i++) resolved.push_back(Points[i]);

      if ((changed) and (resolved.size() >= 3)) {
         Points = std::move(resolved);
         return true;
      }

      return false;
   }

   inline bool remove_self_intersections(std::vector<stroke_resolve_point> &Points, const stroke_resolve_options &Options)
   {
      bool changed = false;

      for (unsigned pass=0; pass < Options.max_passes; pass++) {
         auto loops = find_removable_loops(Points, Options.epsilon);
         if (remove_loop_batch(Points, loops)) {
            changed = true;
            continue;
         }

         stroke_resolve_intersection intersection;
         if (!find_self_intersection(Points, intersection, Options.epsilon)) break;

         auto resolved = remove_inner_loop(Points, intersection);
         if (resolved.size() < 3) break;

         Points = std::move(resolved);
         changed = true;
      }

      return changed;
   }
}

inline unsigned path_self_intersection_count(path_storage &Path, double Epsilon = 1.0e-8)
{
   std::vector<std::vector<stroke_resolve_point>> contours;
   contours.emplace_back();

   double x = 0.0;
   double y = 0.0;

   Path.rewind(0);
   while (true) {
      unsigned cmd = Path.vertex(&x, &y);
      if (is_stop(cmd)) break;

      if (is_move_to(cmd)) {
         if (!contours.back().empty()) contours.emplace_back();
         contours.back().push_back({ x, y });
      }
      else if (is_vertex(cmd)) {
         if (contours.empty()) contours.emplace_back();
         contours.back().push_back({ x, y });
      }
   }

   unsigned intersections = 0;
   for (auto &contour : contours) {
      if (contour.size() > 1 and detail::almost_same(contour.front(), contour.back(), Epsilon)) {
         contour.pop_back();
      }

      intersections += detail::count_self_intersections(contour, Epsilon);
   }

   return intersections;
}

inline bool resolve_stroke_self_intersections(path_storage &Path, const stroke_resolve_options &Options = stroke_resolve_options())
{
   std::vector<std::vector<stroke_resolve_point>> contours;
   std::vector<bool> closed;
   contours.emplace_back();
   closed.push_back(false);

   double x = 0.0;
   double y = 0.0;

   Path.rewind(0);
   while (true) {
      unsigned cmd = Path.vertex(&x, &y);
      if (is_stop(cmd)) break;

      if (is_move_to(cmd)) {
         if (!contours.back().empty()) {
            contours.emplace_back();
            closed.push_back(false);
         }
         contours.back().push_back({ x, y });
      }
      else if (is_vertex(cmd)) {
         if (contours.empty()) {
            contours.emplace_back();
            closed.push_back(false);
         }
         contours.back().push_back({ x, y });
      }
      else if (is_end_poly(cmd) and (get_close_flag(cmd) != 0)) {
         if (!closed.empty()) closed.back() = true;
      }
   }

   bool changed = false;
   path_storage resolved;

   for (unsigned i=0; i < contours.size(); i++) {
      auto &contour = contours[i];
      if (contour.empty()) continue;

      if (contour.size() > 1 and detail::almost_same(contour.front(), contour.back(), Options.epsilon)) {
         contour.pop_back();
         closed[i] = true;
      }

      if ((closed[i]) and (detail::remove_self_intersections(contour, Options))) changed = true;

      if (contour.empty()) continue;

      resolved.move_to(contour[0].x, contour[0].y);
      for (unsigned j=1; j < contour.size(); j++) resolved.line_to(contour[j].x, contour[j].y);
      if (closed[i]) resolved.close_polygon();
   }

   if (changed) {
      Path.remove_all();
      Path.concat_path(resolved);
   }

   return changed;
}

} // namespace
