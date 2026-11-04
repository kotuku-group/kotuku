// Copyright (C) 2026 Paul Manias
//
// Permission to copy, use, modify, sell and distribute this software is granted provided this copyright notice
// appears in all copies.  This software is provided "as is" without express or implied warranty, and with no
// claim as to its suitability for any purpose.
// ---
// Materialises stroked outlines and removes simple self-intersection loops. Hooks into conv_stroke output before the
// result is reused as a fillable path. In the vector renderer it prevents internal stroke loops from being traced when
// a generated thick path is subsequently filled and stroked.

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

   struct stroke_resolve_edge_box {
      unsigned index;
      double min_x;
      double max_x;
      double min_y;
      double max_y;
   };

   struct stroke_resolve_scan {
      std::vector<stroke_resolve_loop> loops;
      stroke_resolve_intersection fallback;
      bool has_fallback = false;
   };

   struct stroke_resolve_contours {
      std::vector<std::vector<stroke_resolve_point>> contours;
      std::vector<bool> closed;
   };

   inline bool almost_same(const stroke_resolve_point &A, const stroke_resolve_point &B, double Epsilon) noexcept
   {
      return (std::abs(A.x - B.x) <= Epsilon) and (std::abs(A.y - B.y) <= Epsilon);
   }

   inline void add_signed_area_edge(double &Area, const stroke_resolve_point &A,
      const stroke_resolve_point &B) noexcept
   {
      Area += (A.x * B.y) - (B.x * A.y);
   }

   inline double signed_area_without_loop(const std::vector<stroke_resolve_point> &Points,
      const stroke_resolve_intersection &Intersection) noexcept
   {
      double area = 0.0;
      stroke_resolve_point first = { 0.0, 0.0 };
      stroke_resolve_point previous = { 0.0, 0.0 };
      bool has_previous = false;
      const unsigned total = unsigned(Points.size());

      auto add_point = [&](const stroke_resolve_point &Point) {
         if (not has_previous) {
            first = Point;
            previous = Point;
            has_previous = true;
         }
         else {
            add_signed_area_edge(area, previous, Point);
            previous = Point;
         }
      };

      for (unsigned i=0; i <= Intersection.a; i++) add_point(Points[i]);
      add_point(Intersection.point);
      for (unsigned i=Intersection.b + 1; i < total; i++) add_point(Points[i]);

      if (has_previous) add_signed_area_edge(area, previous, first);
      return area * 0.5;
   }

   inline double signed_area_loop(const std::vector<stroke_resolve_point> &Points,
      const stroke_resolve_intersection &Intersection) noexcept
   {
      double area = 0.0;
      stroke_resolve_point first = Intersection.point;
      stroke_resolve_point previous = Intersection.point;

      for (unsigned i=Intersection.a + 1; i <= Intersection.b; i++) {
         add_signed_area_edge(area, previous, Points[i]);
         previous = Points[i];
      }

      add_signed_area_edge(area, previous, first);
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

   inline stroke_resolve_edge_box edge_box(const std::vector<stroke_resolve_point> &Points, unsigned Index,
      double Epsilon) noexcept
   {
      const unsigned total = unsigned(Points.size());
      const auto &a = Points[Index];
      const auto &b = Points[(Index + 1) % total];

      return {
         Index,
         std::min(a.x, b.x) - Epsilon,
         std::max(a.x, b.x) + Epsilon,
         std::min(a.y, b.y) - Epsilon,
         std::max(a.y, b.y) + Epsilon
      };
   }

   inline std::vector<stroke_resolve_edge_box> sorted_edge_boxes(
      const std::vector<stroke_resolve_point> &Points, double Epsilon)
   {
      std::vector<stroke_resolve_edge_box> boxes;
      const unsigned total = unsigned(Points.size());
      boxes.reserve(total);

      for (unsigned i=0; i < total; i++) boxes.push_back(edge_box(Points, i, Epsilon));

      std::sort(boxes.begin(), boxes.end(), [](const stroke_resolve_edge_box &A,
         const stroke_resolve_edge_box &B) {
         if (A.min_x < B.min_x) return true;
         if (B.min_x < A.min_x) return false;
         return A.index < B.index;
      });

      return boxes;
   }

   inline bool boxes_overlap(const stroke_resolve_edge_box &A, const stroke_resolve_edge_box &B) noexcept
   {
      return (A.min_x <= B.max_x) and (B.min_x <= A.max_x) and
         (A.min_y <= B.max_y) and (B.min_y <= A.max_y);
   }

   inline bool adjacent_edges(unsigned A, unsigned B, unsigned Total) noexcept
   {
      if (A IS B) return true;
      if (A + 1 IS B) return true;
      if (B + 1 IS A) return true;
      return ((A IS 0) and (B + 1 IS Total)) or ((B IS 0) and (A + 1 IS Total));
   }

   inline void sort_removable_loops(std::vector<stroke_resolve_loop> &Loops)
   {
      std::sort(Loops.begin(), Loops.end(), [](const stroke_resolve_loop &A, const stroke_resolve_loop &B) {
         if (A.a < B.a) return true;
         if (B.a < A.a) return false;
         return A.b < B.b;
      });
   }

   inline void discard_overlapping_loops(std::vector<stroke_resolve_loop> &Loops)
   {
      if (Loops.empty()) return;

      sort_removable_loops(Loops);

      std::vector<stroke_resolve_loop> filtered;
      filtered.reserve(Loops.size());

      unsigned cursor = 0;
      for (auto &loop : Loops) {
         if (loop.a < cursor) continue;
         filtered.push_back(loop);
         cursor = loop.b + 1;
      }

      Loops = std::move(filtered);
   }

   inline stroke_resolve_scan scan_self_intersections(const std::vector<stroke_resolve_point> &Points,
      double Epsilon)
   {
      stroke_resolve_scan result;
      const unsigned total = unsigned(Points.size());
      if (total < 4) return result;

      auto boxes = sorted_edge_boxes(Points, Epsilon);

      for (unsigned i=0; i < total; i++) {
         const auto &box_a = boxes[i];
         const auto &a1 = Points[box_a.index];
         const auto &a2 = Points[(box_a.index + 1) % total];

         for (unsigned j=i + 1; j < total; j++) {
            const auto &box_b = boxes[j];
            if (box_b.min_x > box_a.max_x) break;
            if (adjacent_edges(box_a.index, box_b.index, total)) continue;
            if (not boxes_overlap(box_a, box_b)) continue;

            const auto &b1 = Points[box_b.index];
            const auto &b2 = Points[(box_b.index + 1) % total];

            stroke_resolve_intersection intersection;
            if (not line_intersection(a1, a2, b1, b2, intersection, Epsilon)) continue;

            if (box_a.index < box_b.index) {
               intersection.a = box_a.index;
               intersection.b = box_b.index;
            }
            else {
               intersection.a = box_b.index;
               intersection.b = box_a.index;
            }

            if (not result.has_fallback) {
               result.fallback = intersection;
               result.has_fallback = true;
            }

            const unsigned span = intersection.b - intersection.a;
            if (span <= total / 2) {
               result.loops.push_back({ intersection.a, intersection.b, intersection.point });
            }
         }
      }

      discard_overlapping_loops(result.loops);
      return result;
   }

   inline std::vector<stroke_resolve_point> remove_inner_loop(const std::vector<stroke_resolve_point> &Points,
      const stroke_resolve_intersection &Intersection)
   {
      const unsigned total = unsigned(Points.size());
      std::vector<stroke_resolve_point> resolved;

      if (std::abs(signed_area_loop(Points, Intersection)) > std::abs(signed_area_without_loop(Points, Intersection))) {
         resolved.reserve(Intersection.b - Intersection.a + 2);
         resolved.push_back(Intersection.point);
         for (unsigned i=Intersection.a + 1; i <= Intersection.b; i++) resolved.push_back(Points[i]);
      }
      else {
         resolved.reserve(total);
         for (unsigned i=0; i <= Intersection.a; i++) resolved.push_back(Points[i]);
         resolved.push_back(Intersection.point);
         for (unsigned i=Intersection.b + 1; i < total; i++) resolved.push_back(Points[i]);
      }

      return resolved;
   }

   inline unsigned count_self_intersections(const std::vector<stroke_resolve_point> &Points, double Epsilon)
   {
      const unsigned total = unsigned(Points.size());
      if (total < 4) return 0;

      unsigned intersections = 0;
      auto boxes = sorted_edge_boxes(Points, Epsilon);

      for (unsigned i=0; i < total; i++) {
         const auto &box_a = boxes[i];
         const auto &a1 = Points[box_a.index];
         const auto &a2 = Points[(box_a.index + 1) % total];

         for (unsigned j=i + 1; j < total; j++) {
            const auto &box_b = boxes[j];
            if (box_b.min_x > box_a.max_x) break;
            if (adjacent_edges(box_a.index, box_b.index, total)) continue;
            if (not boxes_overlap(box_a, box_b)) continue;

            const auto &b1 = Points[box_b.index];
            const auto &b2 = Points[(box_b.index + 1) % total];

            stroke_resolve_intersection intersection;
            if (line_intersection(a1, a2, b1, b2, intersection, Epsilon)) intersections++;
         }
      }

      return intersections;
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
         auto scan = scan_self_intersections(Points, Options.epsilon);
         if (remove_loop_batch(Points, scan.loops)) {
            changed = true;
            continue;
         }

         if (not scan.has_fallback) break;

         auto resolved = remove_inner_loop(Points, scan.fallback);
         if (resolved.size() < 3) break;

         Points = std::move(resolved);
         changed = true;
      }

      return changed;
   }

   template <class VertexSource> inline stroke_resolve_contours extract_contours(VertexSource &Source)
   {
      stroke_resolve_contours extracted;
      extracted.contours.emplace_back();
      extracted.closed.push_back(false);

      double x = 0.0;
      double y = 0.0;

      Source.rewind(0);
      while (true) {
         unsigned cmd = Source.vertex(&x, &y);
         if (is_stop(cmd)) break;

         if (is_move_to(cmd)) {
            if (not extracted.contours.back().empty()) {
               extracted.contours.emplace_back();
               extracted.closed.push_back(false);
            }
            extracted.contours.back().push_back({ x, y });
         }
         else if (is_vertex(cmd)) {
            if (extracted.contours.empty()) {
               extracted.contours.emplace_back();
               extracted.closed.push_back(false);
            }
            extracted.contours.back().push_back({ x, y });
         }
         else if (is_end_poly(cmd) and (get_close_flag(cmd) != 0)) {
            if (not extracted.closed.empty()) extracted.closed.back() = true;
         }
      }

      return extracted;
   }

   inline bool clean_contours(stroke_resolve_contours &Contours, const stroke_resolve_options &Options)
   {
      bool changed = false;

      for (unsigned i=0; i < Contours.contours.size(); i++) {
         auto &contour = Contours.contours[i];
         if (contour.empty()) continue;

         if (contour.size() > 1 and detail::almost_same(contour.front(), contour.back(), Options.epsilon)) {
            contour.pop_back();
            Contours.closed[i] = true;
         }

         if ((Contours.closed[i]) and (detail::remove_self_intersections(contour, Options))) changed = true;
      }

      return changed;
   }

   inline void append_contours(path_storage &Path, const stroke_resolve_contours &Contours)
   {
      for (unsigned i=0; i < Contours.contours.size(); i++) {
         auto &contour = Contours.contours[i];
         if (contour.empty()) continue;

         Path.move_to(contour[0].x, contour[0].y);
         for (unsigned j=1; j < contour.size(); j++) Path.line_to(contour[j].x, contour[j].y);
         if (Contours.closed[i]) Path.close_polygon();
      }
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
   auto contours = detail::extract_contours(Path);
   bool changed = detail::clean_contours(contours, Options);

   if (changed) {
      Path.remove_all();
      detail::append_contours(Path, contours);
   }

   return changed;
}

template <class VertexSource> inline bool resolve_stroke_self_intersections(path_storage &Target, VertexSource &Source,
   const stroke_resolve_options &Options = stroke_resolve_options())
{
   auto contours = detail::extract_contours(Source);
   bool changed = detail::clean_contours(contours, Options);

   Target.remove_all();
   detail::append_contours(Target, contours);

   return changed;
}

} // namespace
