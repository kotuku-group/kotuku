// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Stores path commands and floating-point vertices in AGG's standard path container. Hooks into converters, bounding-
// box helpers, rasterisers, and renderer_scanline entry points. In the vector renderer it is the main in-memory
// representation for constructed vector paths.

#pragma once

#include <cstring>
#include <cmath>
#include <vector>
#include <type_traits>

#include "agg_math.h"
#include "agg_array.h"
#include "agg_bezier_arc.h"
#include "agg_curves.h"

namespace agg
{
   template<typename T>
   requires std::is_arithmetic_v<T>
   class poly_plain_adaptor {
   public:
      using value_type = T;

      constexpr poly_plain_adaptor() noexcept :
         m_data(nullptr),
         m_ptr(nullptr),
         m_end(nullptr),
         m_closed(false),
         m_stop(false)
      {}

      constexpr poly_plain_adaptor(const T* data, unsigned num_points, bool closed) noexcept :
         m_data(data),
         m_ptr(data),
         m_end(data + num_points * 2),
         m_closed(closed),
         m_stop(false)
      {}

      constexpr void init(const T* data, unsigned num_points, bool closed) noexcept {
         m_data = data;
         m_ptr = data;
         m_end = data + num_points * 2;
         m_closed = closed;
         m_stop = false;
      }

      constexpr void rewind(unsigned) noexcept {
         m_ptr = m_data;
         m_stop = false;
      }

      constexpr unsigned vertex(double* x, double* y) noexcept {
         if (m_ptr < m_end) {
            const bool first = m_ptr == m_data;
            *x = static_cast<double>(*m_ptr++);
            *y = static_cast<double>(*m_ptr++);
            return first ? path_cmd_move_to : path_cmd_line_to;
         }
         *x = *y = 0.0;
         if (m_closed and !m_stop) {
            m_stop = true;
            return path_cmd_end_poly | path_flags_close;
         }
         return path_cmd_stop;
      }

    private:
        const T* m_data;
        const T* m_ptr;
        const T* m_end;
        bool     m_closed;
        bool     m_stop;
    };

   template<typename Container>
   requires requires(const Container& c) {
       c.size();
       c[0];
   }
   class poly_container_adaptor {
   public:
        using vertex_type = typename Container::value_type;

        constexpr poly_container_adaptor() noexcept :
            m_container(nullptr),
            m_index(0),
            m_closed(false),
            m_stop(false)
        {}

        constexpr poly_container_adaptor(const Container& data, bool closed) noexcept :
            m_container(&data),
            m_index(0),
            m_closed(closed),
            m_stop(false)
        {}

        void init(const Container& data, bool closed) {
            m_container = &data;
            m_index = 0;
            m_closed = closed;
            m_stop = false;
        }

        void rewind(unsigned) {
            m_index = 0;
            m_stop = false;
        }

        unsigned vertex(double* x, double* y) {
            if(m_index < m_container->size()) {
                bool first = m_index == 0;
                const vertex_type& v = (*m_container)[m_index++];
                *x = v.x;
                *y = v.y;
                return first ? path_cmd_move_to : path_cmd_line_to;
            }
            *x = *y = 0.0;
            if(m_closed and !m_stop) {
                m_stop = true;
                return path_cmd_end_poly | path_flags_close;
            }
            return path_cmd_stop;
        }

   private:
      const Container *m_container;
      unsigned m_index;
      bool     m_closed;
      bool     m_stop;
   };

   //-----------------------------------------poly_container_reverse_adaptor
   template<class Container> class poly_container_reverse_adaptor {
   public:
      typedef typename Container::value_type vertex_type;

      poly_container_reverse_adaptor() :
         m_container(0),
         m_index(-1),
         m_closed(false),
         m_stop(false)
      {}

      poly_container_reverse_adaptor(const Container& data, bool closed) :
         m_container(&data),
         m_index(-1),
         m_closed(closed),
         m_stop(false)
      {}

        void init(const Container& data, bool closed) {
            m_container = &data;
            m_index = m_container->size() - 1;
            m_closed = closed;
            m_stop = false;
        }

        void rewind(unsigned) {
            m_index = m_container->size() - 1;
            m_stop = false;
        }

        unsigned vertex(double* x, double* y) {
            if (m_index >= 0) {
               bool first = m_index == int(m_container->size() - 1);
               const vertex_type& v = (*m_container)[m_index--];
               *x = v.x;
               *y = v.y;
               return first ? path_cmd_move_to : path_cmd_line_to;
            }
            *x = *y = 0.0;
            if (m_closed and !m_stop) {
               m_stop = true;
               return path_cmd_end_poly | path_flags_close;
            }
            return path_cmd_stop;
        }

    private:
        const Container* m_container;
        int  m_index;
        bool m_closed;
        bool m_stop;
    };

    class line_adaptor {
    public:
        typedef double value_type;

        line_adaptor() : m_line(m_coord, 2, false) {}
        line_adaptor(double x1, double y1, double x2, double y2) : m_line(m_coord, 2, false) {
            m_coord[0] = x1;
            m_coord[1] = y1;
            m_coord[2] = x2;
            m_coord[3] = y2;
        }

        void init(double x1, double y1, double x2, double y2) {
            m_coord[0] = x1;
            m_coord[1] = y1;
            m_coord[2] = x2;
            m_coord[3] = y2;
            m_line.rewind(0);
        }

        void rewind(unsigned) {
            m_line.rewind(0);
        }

        unsigned vertex(double* x, double* y) {
            return m_line.vertex(x, y);
        }

    private:
        double m_coord[4];
        poly_plain_adaptor<double> m_line;
    };

    // A container to store vertices with their flags.
    // A path consists of a number of contours separated with "move_to"
    // commands. The path storage can keep and maintain more than one
    // path.
    // To navigate to the beginning of a particular path, use rewind(path_id);
    // Where path_id is what start_new_path() returns. So, when you call
    // start_new_path() you need to store its return value somewhere else
    // to navigate to the path afterwards.
    //
    // See also: vertex_source concept

    template<class VertexContainer> class path_base {
    public:
        typedef VertexContainer            container_type;
        typedef path_base<VertexContainer> self_type;

        path_base() : m_last_x(0), m_last_y(0), m_vertices(), m_iterator(0) {}
        void remove_all() { m_vertices.remove_all(); m_iterator = 0; }
        void free_all()   { m_vertices.free_all();   m_iterator = 0; }
        void reserve(unsigned Total) { m_vertices.reserve(Total); }

        // Make path functions

        unsigned start_new_path();
        void rect(double width, double height);
        void move_to(const point_d &);
        void move_rel(point_d);
        void line_to(const point_d &);
        void line_rel(point_d);
        void move_to(double x, double y);
        void move_rel(double dx, double dy);
        void line_to(double x, double y);
        void line_rel(double dx, double dy);
        void hline_to(double x);
        void hline_rel(double dx);
        void vline_to(double y);
        void vline_rel(double dy);
        void arc_to(double rx, double ry, double angle, bool large_arc_flag, bool sweep_flag, double x, double y);
        void arc_rel(double rx, double ry, double angle, bool large_arc_flag, bool sweep_flag, double dx, double dy);
        void curve3(double x_ctrl, double y_ctrl, double x_to,   double y_to);
        void curve3_rel(double dx_ctrl, double dy_ctrl, double dx_to,   double dy_to);
        void curve3(double x_to, double y_to);
        void curve3_rel(double dx_to, double dy_to);
        void curve4(double x_ctrl1, double y_ctrl1, double x_ctrl2, double y_ctrl2, double x_to,    double y_to);
        void curve4_rel(double dx_ctrl1, double dy_ctrl1, double dx_ctrl2, double dy_ctrl2, double dx_to,    double dy_to);
        void curve4(double x_ctrl2, double y_ctrl2, double x_to,    double y_to);
        void curve4_rel(double x_ctrl2, double y_ctrl2, double x_to,    double y_to);
        void end_poly(unsigned flags = path_flags_close);
        void close_polygon(unsigned flags = path_flags_none);

        // conv_curve functions

        double m_last_x;
        double m_last_y;
        curve3_div m_curve3;
        curve4_div m_curve4;

        void approximation_method(curve_approximation_method_e v) {
            m_curve3.approximation_method(v);
            m_curve4.approximation_method(v);
        }

        curve_approximation_method_e approximation_method() const {
            return m_curve4.approximation_method();
        }

        void approximation_scale(double s) {
            m_curve3.approximation_scale(s);
            m_curve4.approximation_scale(s);
        }

        double approximation_scale() const {
            return m_curve4.approximation_scale();
        }

        void angle_tolerance(double v) {
            m_curve3.angle_tolerance(v);
            m_curve4.angle_tolerance(v);
        }

        double angle_tolerance() const {
            return m_curve4.angle_tolerance();
        }

        void cusp_limit(double v) {
            m_curve3.cusp_limit(v);
            m_curve4.cusp_limit(v);
        }

        double cusp_limit() const {
            return m_curve4.cusp_limit();
        }

        bool empty() const { return total_vertices() == 0; }

        const container_type& vertices() const { return m_vertices; }
              container_type& vertices()       { return m_vertices; }

        unsigned total_vertices() const;

        void rel_to_abs(double* x, double* y) const;

        unsigned last_vertex(double* x, double* y) const;
        unsigned prev_vertex(double* x, double* y) const;

        double last_x() const;
        double last_y() const;

        unsigned vertex(unsigned idx, double* x, double* y) const;
        unsigned command(unsigned idx) const;

        void modify_vertex(unsigned idx, double x, double y);
        void modify_vertex(unsigned idx, double x, double y, unsigned cmd);
        void modify_command(unsigned idx, unsigned cmd);

        void     rewind(unsigned path_id);
        unsigned vertex(double* x, double* y);

        // Arrange the orientation of a polygon, all polygons in a path,
        // or in all paths. After calling arrange_orientations() or
        // arrange_orientations_all_paths(), all the polygons will have
        // the same orientation, i.e. path_flags_cw or path_flags_ccw

        unsigned arrange_polygon_orientation(unsigned start, int orientation);
        unsigned arrange_orientations(unsigned path_id, int orientation);
        void     arrange_orientations_all_paths(int orientation);
        void     invert_polygon(unsigned start);

        // Flip all vertices horizontally or vertically, between x1 and x2, or between y1 and y2 respectively

        void flip_x(double x1, double x2);
        void flip_y(double y1, double y2);

        // Concatenate path. The path is added as is.

        template<class VertexSource> void concat_path(VertexSource& vs, unsigned path_id = 0) {
            double x, y;
            unsigned cmd;
            vs.rewind(path_id);
            while(!is_stop(cmd = vs.vertex(&x, &y))) {
                m_vertices.add_vertex(x, y, cmd);
            }
        }

        // Copy a path as-is, bypassing add_vertex()

        template<class VertexSource> void copy_path(VertexSource& vs) {
           m_vertices = vs.vertices();
        }

        // Join path. The path is joined with the existing one, that is, it behaves as if the pen of a plotter was always down (drawing)
        template<class VertexSource> void join_path(VertexSource& vs, unsigned path_id = 0)
        {
           double x, y;
           vs.rewind(path_id);
           auto cmd = vs.vertex(&x, &y);
           if (!is_stop(cmd)) {
              if (is_vertex(cmd)) {
                 double x0, y0;
                 auto cmd0 = last_vertex(&x0, &y0);
                 if (is_vertex(cmd0)) {
                    if (calc_distance(x, y, x0, y0) > vertex_dist_epsilon) {
                       if (is_move_to(cmd)) cmd = path_cmd_line_to;
                       m_vertices.add_vertex(x, y, cmd);
                    }
                 }
                 else {
                     if (is_stop(cmd0)) cmd = path_cmd_move_to;
                     else if (is_move_to(cmd)) cmd = path_cmd_line_to;
                     m_vertices.add_vertex(x, y, cmd);
                 }
              }

              while (!is_stop(cmd = vs.vertex(&x, &y))) {
                 m_vertices.add_vertex(x, y, is_move_to(cmd) ? unsigned(path_cmd_line_to) : cmd);
              }
           }
        }

        // Concatenate polygon/polyline.

        template<class T> void concat_poly(const T* data, unsigned num_points, bool closed)
        {
            poly_plain_adaptor<T> poly(data, num_points, closed);
            concat_path(poly);
        }

        // Join polygon/polyline continuously.

        template<class T> void join_poly(const T* data, unsigned num_points, bool closed)
        {
            poly_plain_adaptor<T> poly(data, num_points, closed);
            join_path(poly);
        }

        void translate(double dx, double dy, unsigned path_id=0);
        void translate_all_paths(double dx, double dy);

        template<class Trans>
        void transform(const Trans& trans, unsigned path_id=0) {
            auto num_ver = m_vertices.total_vertices();
            for(; path_id < num_ver; path_id++) {
                double x, y;
                auto cmd = m_vertices.vertex(path_id, &x, &y);
                if (is_stop(cmd)) break;
                if (is_vertex(cmd)) {
                    trans.transform(&x, &y);
                    m_vertices.modify_vertex(path_id, x, y);
                }
            }
        }

        template<class Trans>
        void transform_all_paths(const Trans& trans) {
            unsigned idx;
            auto num_ver = m_vertices.total_vertices();
            for(idx = 0; idx < num_ver; idx++) {
                double x, y;
                if(is_vertex(m_vertices.vertex(idx, &x, &y))) {
                    trans.transform(&x, &y);
                    m_vertices.modify_vertex(idx, x, y);
                }
            }
        }



    private:
        unsigned perceive_polygon_orientation(unsigned start, unsigned end);
        void     invert_polygon(unsigned start, unsigned end);

        VertexContainer m_vertices;
        unsigned        m_iterator;
    };

    template<class VC>
    unsigned path_base<VC>::start_new_path()
    {
        if (!is_stop(m_vertices.last_command())) {
           m_vertices.add_vertex(0.0, 0.0, path_cmd_stop);
        }
        return m_vertices.total_vertices();
    }

   template<class VC>
   inline void path_base<VC>::rel_to_abs(double* x, double* y) const {
      if (m_vertices.total_vertices()) {
         double x2, y2;
         if (is_vertex(m_vertices.last_vertex(&x2, &y2))) {
            *x += x2;
            *y += y2;
         }
      }
   }

   template<class VC>
   inline void path_base<VC>::rect(double width, double height) {
      m_vertices.free_all();
      m_vertices.add_vertex(0.0, 0.0, path_cmd_move_to);
      m_vertices.add_vertex(width, 0.0, path_cmd_line_to);
      m_vertices.add_vertex(width, height, path_cmd_line_to);
      m_vertices.add_vertex(0.0, height, path_cmd_line_to);
      m_vertices.add_vertex(0.0, 0.0, path_cmd_end_poly | path_flags_close);
   }


   template<class VC>
   inline void path_base<VC>::move_to(const point_d &Point) {
      m_vertices.add_vertex(Point.x, Point.y, path_cmd_move_to);
   }

   template<class VC>
   inline void path_base<VC>::move_rel(point_d Point) {
      rel_to_abs(&Point.x, &Point.y);
      m_vertices.add_vertex(Point.x, Point.y, path_cmd_move_to);
   }

   template<class VC>
   inline void path_base<VC>::line_to(const point_d &Point) {
      m_vertices.add_vertex(Point.x, Point.y, path_cmd_line_to);
   }

   template<class VC>
   inline void path_base<VC>::line_rel(point_d Point) {
      rel_to_abs(&Point.x, &Point.y);
      m_vertices.add_vertex(Point.x, Point.y, path_cmd_line_to);
   }

   template<class VC>
   inline void path_base<VC>::move_to(double x, double y) {
      m_vertices.add_vertex(x, y, path_cmd_move_to);
   }

   template<class VC>
   inline void path_base<VC>::move_rel(double dx, double dy) {
      rel_to_abs(&dx, &dy);
      m_vertices.add_vertex(dx, dy, path_cmd_move_to);
   }

   template<class VC>
   inline void path_base<VC>::line_to(double x, double y) {
      m_vertices.add_vertex(x, y, path_cmd_line_to);
   }

   template<class VC>
   inline void path_base<VC>::line_rel(double dx, double dy) {
      rel_to_abs(&dx, &dy);
      m_vertices.add_vertex(dx, dy, path_cmd_line_to);
   }

   template<class VC>
   inline void path_base<VC>::hline_to(double x) {
      m_vertices.add_vertex(x, last_y(), path_cmd_line_to);
   }

   template<class VC>
   inline void path_base<VC>::hline_rel(double dx) {
      double dy = 0;
      rel_to_abs(&dx, &dy);
      m_vertices.add_vertex(dx, dy, path_cmd_line_to);
   }

   template<class VC>
   inline void path_base<VC>::vline_to(double y) {
      m_vertices.add_vertex(last_x(), y, path_cmd_line_to);
   }

   template<class VC>
   inline void path_base<VC>::vline_rel(double dy) {
      double dx = 0;
      rel_to_abs(&dx, &dy);
      m_vertices.add_vertex(dx, dy, path_cmd_line_to);
   }

   template<class VC>
   void path_base<VC>::arc_to(double rx, double ry, double angle, bool large_arc_flag,
      bool sweep_flag, double x, double y) {

      if (m_vertices.total_vertices() and is_vertex(m_vertices.last_command())) {
         const double epsilon = 1e-30;
         double x0 = 0.0;
         double y0 = 0.0;
         m_vertices.last_vertex(&x0, &y0);

         rx = fabs(rx);
         ry = fabs(ry);

         // Ensure radii are valid

         if (rx < epsilon or ry < epsilon) {
            line_to(x, y);
            return;
         }

         if (calc_distance(x0, y0, x, y) < epsilon) {
            // If the endpoints (x, y) and (x0, y0) are identical, then this
            // is equivalent to omitting the elliptical arc segment entirely.
            return;
         }

         bezier_arc_svg a(x0, y0, rx, ry, angle, large_arc_flag, sweep_flag, x, y);
         if (a.radii_ok()) join_path(a);
         else line_to(x, y);
      }
      else move_to(x, y);
   }

   template<class VC>
   void path_base<VC>::arc_rel(double rx, double ry, double angle, bool large_arc_flag,
      bool sweep_flag, double dx, double dy) {

      rel_to_abs(&dx, &dy);
      arc_to(rx, ry, angle, large_arc_flag, sweep_flag, dx, dy);
   }

   template<class VC>
   void path_base<VC>::curve3(double x_ctrl, double y_ctrl, double x_to, double y_to) {
      m_vertices.add_vertex(x_ctrl, y_ctrl, path_cmd_curve3);
      m_vertices.add_vertex(x_to, y_to, path_cmd_curve3);
   }

   template<class VC>
   void path_base<VC>::curve3_rel(double dx_ctrl, double dy_ctrl, double dx_to, double dy_to) {
      rel_to_abs(&dx_ctrl, &dy_ctrl);
      rel_to_abs(&dx_to,   &dy_to);
      m_vertices.add_vertex(dx_ctrl, dy_ctrl, path_cmd_curve3);
      m_vertices.add_vertex(dx_to, dy_to, path_cmd_curve3);
   }

   template<class VC>
   void path_base<VC>::curve3(double x_to, double y_to) {
      double x0, y0;

      if (is_vertex(m_vertices.last_vertex(&x0, &y0))) {
         double x_ctrl;
         double y_ctrl;
         unsigned cmd = m_vertices.prev_vertex(&x_ctrl, &y_ctrl);
         if (is_curve(cmd)) {
            x_ctrl = x0 + x0 - x_ctrl;
            y_ctrl = y0 + y0 - y_ctrl;
         }
         else {
            x_ctrl = x0;
            y_ctrl = y0;
         }
         curve3(x_ctrl, y_ctrl, x_to, y_to);
      }
   }

   template<class VC> void path_base<VC>::curve3_rel(double dx_to, double dy_to) {
      rel_to_abs(&dx_to, &dy_to);
      curve3(dx_to, dy_to);
   }

   template<class VC>
   void path_base<VC>::curve4(double x_ctrl1, double y_ctrl1, double x_ctrl2, double y_ctrl2, double x_to, double y_to) {
      m_vertices.add_vertex(x_ctrl1, y_ctrl1, path_cmd_curve4);
      m_vertices.add_vertex(x_ctrl2, y_ctrl2, path_cmd_curve4);
      m_vertices.add_vertex(x_to,    y_to,    path_cmd_curve4);
   }

   template<class VC>
   void path_base<VC>::curve4_rel(double dx_ctrl1, double dy_ctrl1, double dx_ctrl2, double dy_ctrl2, double dx_to, double dy_to) {
      rel_to_abs(&dx_ctrl1, &dy_ctrl1);
      rel_to_abs(&dx_ctrl2, &dy_ctrl2);
      rel_to_abs(&dx_to,    &dy_to);
      m_vertices.add_vertex(dx_ctrl1, dy_ctrl1, path_cmd_curve4);
      m_vertices.add_vertex(dx_ctrl2, dy_ctrl2, path_cmd_curve4);
      m_vertices.add_vertex(dx_to,    dy_to,    path_cmd_curve4);
   }

   template<class VC> void path_base<VC>::curve4(double x_ctrl2, double y_ctrl2, double x_to, double y_to) {
      double x0, y0;
      if (is_vertex(last_vertex(&x0, &y0))) {
         double x_ctrl1, y_ctrl1;
         unsigned cmd = prev_vertex(&x_ctrl1, &y_ctrl1);
         if (is_curve(cmd)) {
            x_ctrl1 = x0 + x0 - x_ctrl1;
            y_ctrl1 = y0 + y0 - y_ctrl1;
         }
         else {
            x_ctrl1 = x0;
            y_ctrl1 = y0;
         }
         curve4(x_ctrl1, y_ctrl1, x_ctrl2, y_ctrl2, x_to, y_to);
      }
   }

   template<class VC> void path_base<VC>::curve4_rel(double dx_ctrl2, double dy_ctrl2, double dx_to, double dy_to) {
      rel_to_abs(&dx_ctrl2, &dy_ctrl2);
      rel_to_abs(&dx_to,    &dy_to);
      curve4(dx_ctrl2, dy_ctrl2, dx_to, dy_to);
   }

   template<class VC> inline void path_base<VC>::end_poly(unsigned flags) {
      if (is_vertex(m_vertices.last_command())) {
         m_vertices.add_vertex(0.0, 0.0, path_cmd_end_poly | flags);
      }
   }

   template<class VC> inline void path_base<VC>::close_polygon(unsigned flags) {
      end_poly(path_flags_close | flags);
   }

   template<class VC> inline unsigned path_base<VC>::total_vertices() const {
      return m_vertices.total_vertices();
   }

    template<class VC> inline unsigned path_base<VC>::last_vertex(double* x, double* y) const
    {
        return m_vertices.last_vertex(x, y);
    }

    template<class VC> inline unsigned path_base<VC>::prev_vertex(double* x, double* y) const
    {
        return m_vertices.prev_vertex(x, y);
    }

    template<class VC> inline double path_base<VC>::last_x() const
    {
        return m_vertices.last_x();
    }

    template<class VC> inline double path_base<VC>::last_y() const
    {
        return m_vertices.last_y();
    }

    template<class VC> inline unsigned path_base<VC>::vertex(unsigned idx, double* x, double* y) const
    {
        return m_vertices.vertex(idx, x, y);
    }

    template<class VC> inline unsigned path_base<VC>::command(unsigned idx) const
    {
        return m_vertices.command(idx);
    }

    template<class VC> void path_base<VC>::modify_vertex(unsigned idx, double x, double y)
    {
        m_vertices.modify_vertex(idx, x, y);
    }

    template<class VC> void path_base<VC>::modify_vertex(unsigned idx, double x, double y, unsigned cmd)
    {
        m_vertices.modify_vertex(idx, x, y, cmd);
    }

    template<class VC> void path_base<VC>::modify_command(unsigned idx, unsigned cmd)
    {
        m_vertices.modify_command(idx, cmd);
    }

    template<class VC> inline void path_base<VC>::rewind(unsigned path_id)
    {
        m_iterator = path_id;
        m_last_x = 0.0;
        m_last_y = 0.0;
        m_curve3.reset();
        m_curve4.reset();
    }

   template<class VC>
   inline unsigned path_base<VC>::vertex(double* x, double* y)
   {
      if (!is_stop(m_curve3.vertex(x, y))) {
         m_last_x = *x;
         m_last_y = *y;
         return path_cmd_line_to;
      }

      if (!is_stop(m_curve4.vertex(x, y))) {
         m_last_x = *x;
         m_last_y = *y;
         return path_cmd_line_to;
      }

      double ct2_x, ct2_y, end_x, end_y;
      unsigned cmd;
      if (m_iterator >= m_vertices.total_vertices()) cmd = path_cmd_stop;
      else cmd = m_vertices.vertex(m_iterator++, x, y);

      switch (cmd) {
         case path_cmd_curve3:
            m_vertices.vertex(m_iterator++, &end_x, &end_y);
            m_curve3.init(m_last_x, m_last_y, *x, *y, end_x, end_y);
            m_curve3.vertex(x, y);    // First call returns path_cmd_move_to
            m_curve3.vertex(x, y);    // This is the first vertex of the curve
            cmd = path_cmd_line_to;
            break;

         case path_cmd_curve4:
            m_vertices.vertex(m_iterator++, &ct2_x, &ct2_y);
            m_vertices.vertex(m_iterator++, &end_x, &end_y);
            m_curve4.init(m_last_x, m_last_y, *x, *y, ct2_x, ct2_y, end_x, end_y);

            m_curve4.vertex(x, y);    // First call returns path_cmd_move_to
            m_curve4.vertex(x, y);    // This is the first vertex of the curve
            cmd = path_cmd_line_to;
            break;
      }
      m_last_x = *x;
      m_last_y = *y;
      return cmd;
   }

    template<class VC>
    unsigned path_base<VC>::perceive_polygon_orientation(unsigned start, unsigned end)
    {
        // Calculate signed area (double area to be exact)
        unsigned np = end - start;
        double area = 0.0;
        unsigned i;
        for(i = 0; i < np; i++)
        {
            double x1, y1, x2, y2;
            m_vertices.vertex(start + i,            &x1, &y1);
            m_vertices.vertex(start + (i + 1) % np, &x2, &y2);
            area += x1 * y2 - y1 * x2;
        }
        return (area < 0.0) ? path_flags_cw : path_flags_ccw;
    }

    template<class VC>
    void path_base<VC>::invert_polygon(unsigned start, unsigned end)
    {
        unsigned i;
        unsigned tmp_cmd = m_vertices.command(start);

        --end; // Make "end" inclusive

        // Shift all commands to one position
        for(i = start; i < end; i++)
        {
            m_vertices.modify_command(i, m_vertices.command(i + 1));
        }

        // Assign starting command to the ending command
        m_vertices.modify_command(end, tmp_cmd);

        // Reverse the polygon
        while(end > start)
        {
            m_vertices.swap_vertices(start++, end--);
        }
    }

    template<class VC>
    void path_base<VC>::invert_polygon(unsigned start) {
        // Skip all non-vertices at the beginning
        while(start < m_vertices.total_vertices() and !is_vertex(m_vertices.command(start))) ++start;

        // Skip all insignificant move_to
        while (start+1 < m_vertices.total_vertices() and is_move_to(m_vertices.command(start)) and
              is_move_to(m_vertices.command(start+1))) ++start;

        // Find the last vertex
        unsigned end = start + 1;
        while(end < m_vertices.total_vertices() and !is_next_poly(m_vertices.command(end))) ++end;

        invert_polygon(start, end);
    }

    template<class VC>
    unsigned path_base<VC>::arrange_polygon_orientation(unsigned start, int orientation)
    {
        if(orientation == path_flags_none) return start;

        // Skip all non-vertices at the beginning
        while(start < m_vertices.total_vertices() and
              !is_vertex(m_vertices.command(start))) ++start;

        // Skip all insignificant move_to
        while(start+1 < m_vertices.total_vertices() and
              is_move_to(m_vertices.command(start)) and
              is_move_to(m_vertices.command(start+1))) ++start;

        // Find the last vertex
        unsigned end = start + 1;
        while(end < m_vertices.total_vertices() and
              !is_next_poly(m_vertices.command(end))) ++end;

        if(end - start > 2) {
            if(perceive_polygon_orientation(start, end) != unsigned(orientation)) {
                // Invert polygon, set orientation flag, and skip all end_poly
                invert_polygon(start, end);
                unsigned cmd;
                while(end < m_vertices.total_vertices() and is_end_poly(cmd = m_vertices.command(end))) {
                    m_vertices.modify_command(end++, set_orientation(cmd, orientation));
                }
            }
        }
        return end;
    }

   template<class VC>
   unsigned path_base<VC>::arrange_orientations(unsigned start, int orientation)
   {
      if (orientation != path_flags_none) {
         while(start < m_vertices.total_vertices()) {
            start = arrange_polygon_orientation(start, orientation);
            if (is_stop(m_vertices.command(start))) {
               ++start;
               break;
            }
         }
      }
      return start;
   }

    template<class VC>
    void path_base<VC>::arrange_orientations_all_paths(int orientation) {
        if (orientation != path_flags_none) {
            unsigned start = 0;
            while(start < m_vertices.total_vertices()) {
                start = arrange_orientations(start, orientation);
            }
        }
    }

    template<class VC>
    void path_base<VC>::flip_x(double x1, double x2) {
        unsigned i;
        double x, y;
        for(i = 0; i < m_vertices.total_vertices(); i++) {
            unsigned cmd = m_vertices.vertex(i, &x, &y);
            if (is_vertex(cmd)) {
                m_vertices.modify_vertex(i, x2 - x + x1, y);
            }
        }
    }

    template<class VC>
    void path_base<VC>::flip_y(double y1, double y2) {
        unsigned i;
        double x, y;
        for(i = 0; i < m_vertices.total_vertices(); i++) {
            unsigned cmd = m_vertices.vertex(i, &x, &y);
            if (is_vertex(cmd)) {
                m_vertices.modify_vertex(i, x, y2 - y + y1);
            }
        }
    }

    template<class VC>
    void path_base<VC>::translate(double dx, double dy, unsigned path_id) {
        unsigned num_ver = m_vertices.total_vertices();
        for(; path_id < num_ver; path_id++)
        {
            double x, y;
            unsigned cmd = m_vertices.vertex(path_id, &x, &y);
            if(is_stop(cmd)) break;
            if(is_vertex(cmd))
            {
                x += dx;
                y += dy;
                m_vertices.modify_vertex(path_id, x, y);
            }
        }
    }

    //------------------------------------------------------------------------
    template<class VC>
    void path_base<VC>::translate_all_paths(double dx, double dy) {
        unsigned idx;
        unsigned num_ver = m_vertices.total_vertices();
        for (idx = 0; idx < num_ver; idx++) {
            double x, y;
            if (is_vertex(m_vertices.vertex(idx, &x, &y))) {
               x += dx;
               y += dy;
               m_vertices.modify_vertex(idx, x, y);
            }
        }
    }

    template<class Container> class vertex_stl_storage {
    public:
        typedef typename Container::value_type vertex_type;
        typedef typename vertex_type::value_type value_type;

        void remove_all() { m_vertices.clear(); }
        void free_all()   { m_vertices.clear(); }
        void reserve(unsigned Total) { m_vertices.reserve(Total); }

        void add_vertex(double x, double y, unsigned cmd) {
            m_vertices.push_back(vertex_type(value_type(x), value_type(y), int8u(cmd)));
        }

        void modify_vertex(unsigned idx, double x, double y) {
            vertex_type& v = m_vertices[idx];
            v.x = value_type(x);
            v.y = value_type(y);
        }

        void modify_vertex(unsigned idx, double x, double y, unsigned cmd) {
            vertex_type& v = m_vertices[idx];
            v.x   = value_type(x);
            v.y   = value_type(y);
            v.cmd = int8u(cmd);
        }

        void modify_command(unsigned idx, unsigned cmd) {
            m_vertices[idx].cmd = int8u(cmd);
        }

        void swap_vertices(unsigned v1, unsigned v2) {
            vertex_type t = m_vertices[v1];
            m_vertices[v1] = m_vertices[v2];
            m_vertices[v2] = t;
        }

        unsigned last_command() const {
            return m_vertices.size() ?
                m_vertices[m_vertices.size() - 1].cmd :
                path_cmd_stop;
        }

        unsigned last_vertex(double* x, double* y) const {
            if(m_vertices.size() == 0) {
                *x = *y = 0.0;
                return path_cmd_stop;
            }
            return vertex(m_vertices.size() - 1, x, y);
        }

        unsigned prev_vertex(double* x, double* y) const {
            if (m_vertices.size() < 2) {
                *x = *y = 0.0;
                return path_cmd_stop;
            }
            return vertex(m_vertices.size() - 2, x, y);
        }

        double last_x() const {
            return m_vertices.size() ? m_vertices[m_vertices.size() - 1].x : 0.0;
        }

        double last_y() const {
            return m_vertices.size() ? m_vertices[m_vertices.size() - 1].y : 0.0;
        }

        constexpr unsigned total_vertices() const {
            return m_vertices.size();
        }

        unsigned vertex(unsigned idx, double* x, double* y) const {
            const vertex_type& v = m_vertices[idx];
            *x = v.x;
            *y = v.y;
            return v.cmd;
        }

        unsigned command(unsigned idx) const {
            return m_vertices[idx].cmd;
        }

    private:
        Container m_vertices;
    };

    // Vertices are stored using std::vector.  The AGG default was to use a custom vertex_block_storage implementation,
    // however tests showed no difference in efficiency and a major penalty in memory usage.  For this reason it was
    // eliminated in favour of std::vector.

    typedef path_base<vertex_stl_storage<std::vector<vertex_d> > > path_storage;
}
