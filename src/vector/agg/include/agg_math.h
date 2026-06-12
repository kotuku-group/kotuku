//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.

// Bessel function (besj) was adapted for use in AGG library by Andy Wilk
// Contact: castor.vulgaris@gmail.com
//----------------------------------------------------------------------------

#ifndef AGG_MATH_INCLUDED
#define AGG_MATH_INCLUDED

#include <cmath>
#include <cstdlib>
#include <type_traits>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "agg_basics.h"

namespace agg {
    // Coinciding points maximal distance (Epsilon)
    constexpr double vertex_dist_epsilon = 1e-14;

    // See calc_intersection
    constexpr double intersection_epsilon = 1.0e-30;

    constexpr double cross_product(double x1, double y1, double x2, double y2, double x,  double y) noexcept
    {
        // Optimized for better instruction pipelining
        const double dx1 = x - x2;
        const double dy1 = y2 - y1;
        const double dx2 = x2 - x1;
        const double dy2 = y - y2;
        return dx1 * dy1 - dy2 * dx2;
    }

    constexpr bool point_in_triangle(double x1, double y1, double x2, double y2, double x3, double y3, double x,  double y) noexcept
    {
        const bool cp1 = cross_product(x1, y1, x2, y2, x, y) < 0.0;
        const bool cp2 = cross_product(x2, y2, x3, y3, x, y) < 0.0;
        const bool cp3 = cross_product(x3, y3, x1, y1, x, y) < 0.0;
        return cp1 == cp2 and cp2 == cp3 and cp3 == cp1;
    }

    inline double calc_distance(double x1, double y1, double x2, double y2) noexcept
    {
        const double dx = x2 - x1;
        const double dy = y2 - y1;
        const double dist_sq = dx * dx + dy * dy;

        // Fast path for zero distance
        if (dist_sq == 0.0) [[unlikely]] return 0.0;

        return std::sqrt(dist_sq);
    }

    constexpr double calc_sq_distance(double x1, double y1, double x2, double y2) noexcept
    {
        const double dx = x2-x1;
        const double dy = y2-y1;
        return dx * dx + dy * dy;
    }

    inline double calc_line_point_distance(double x1, double y1, double x2, double y2, double x,  double y) noexcept
    {
        const double dx = x2 - x1;
        const double dy = y2 - y1;
        const double len_sq = dx * dx + dy * dy;

        if (len_sq < vertex_dist_epsilon * vertex_dist_epsilon) [[unlikely]] {
            return calc_distance(x1, y1, x, y);
        }

        const double inv_len = 1.0 / std::sqrt(len_sq);
        return ((x - x2) * dy - (y - y2) * dx) * inv_len;
    }

    constexpr double calc_segment_point_u(double x1, double y1, double x2, double y2, double x,  double y) noexcept
    {
        const double dx = x2 - x1;
        const double dy = y2 - y1;

        if(dx == 0 and dy == 0) return 0;

        const double pdx = x - x1;
        const double pdy = y - y1;

        return (pdx * dx + pdy * dy) / (dx * dx + dy * dy);
    }

    constexpr double calc_segment_point_sq_distance(double x1, double y1, double x2, double y2, double x,  double y, double u) noexcept
    {
        if (u <= 0) return calc_sq_distance(x, y, x1, y1);
        else if (u >= 1) return calc_sq_distance(x, y, x2, y2);
        return calc_sq_distance(x, y, x1 + u * (x2 - x1), y1 + u * (y2 - y1));
    }

    constexpr double calc_segment_point_sq_distance(double x1, double y1, double x2, double y2, double x,  double y) noexcept
    {
        return calc_segment_point_sq_distance(x1, y1, x2, y2, x, y, calc_segment_point_u(x1, y1, x2, y2, x, y));
    }

    inline bool calc_intersection(double ax, double ay, double bx, double by,
                                      double cx, double cy, double dx, double dy,
                                      double* x, double* y) noexcept
    {
        const double num = (ay-cy) * (dx-cx) - (ax-cx) * (dy-cy);
        const double den = (bx-ax) * (dy-cy) - (by-ay) * (dx-cx);
        if (std::abs(den) < intersection_epsilon) return false;
        const double r = num / den;
        *x = ax + r * (bx-ax);
        *y = ay + r * (by-ay);
        return true;
    }

    constexpr bool intersection_exists(double x1, double y1, double x2, double y2,
                                        double x3, double y3, double x4, double y4) noexcept
    {
        // It's less expensive but you can't control the boundary conditions: Less or LessEqual
        const double dx1 = x2 - x1;
        const double dy1 = y2 - y1;
        const double dx2 = x4 - x3;
        const double dy2 = y4 - y3;
        return ((x3 - x2) * dy1 - (y3 - y2) * dx1 < 0.0) !=
               ((x4 - x2) * dy1 - (y4 - y2) * dx1 < 0.0) and
               ((x1 - x4) * dy2 - (y1 - y4) * dx2 < 0.0) !=
               ((x2 - x4) * dy2 - (y2 - y4) * dx2 < 0.0);

        // It's is more expensive but more flexible
        // in terms of boundary conditions.
        //--------------------
        //double den  = (x2-x1) * (y4-y3) - (y2-y1) * (x4-x3);
        //if(fabs(den) < intersection_epsilon) return false;
        //double nom1 = (x4-x3) * (y1-y3) - (y4-y3) * (x1-x3);
        //double nom2 = (x2-x1) * (y1-y3) - (y2-y1) * (x1-x3);
        //double ua = nom1 / den;
        //double ub = nom2 / den;
        //return ua >= 0.0 and ua <= 1.0 and ub >= 0.0 and ub <= 1.0;
    }

    inline void calc_orthogonal(double thickness, double x1, double y1, double x2, double y2, double* x, double* y) noexcept
    {
        const double dx = x2 - x1;
        const double dy = y2 - y1;
        const double len_sq = dx * dx + dy * dy;

        if (len_sq < 1e-20) [[unlikely]] {
            *x = *y = 0.0;
            return;
        }

        const double inv_len = thickness / std::sqrt(len_sq);
        *x =  dy * inv_len;
        *y = -dx * inv_len;
    }

    inline void dilate_triangle(double x1, double y1, double x2, double y2, double x3, double y3, double *x, double* y, double d) noexcept
    {
        double dx1=0.0;
        double dy1=0.0;
        double dx2=0.0;
        double dy2=0.0;
        double dx3=0.0;
        double dy3=0.0;
        const double loc = cross_product(x1, y1, x2, y2, x3, y3);
        if (std::abs(loc) > intersection_epsilon) {
            if (cross_product(x1, y1, x2, y2, x3, y3) > 0.0) d = -d;
            calc_orthogonal(d, x1, y1, x2, y2, &dx1, &dy1);
            calc_orthogonal(d, x2, y2, x3, y3, &dx2, &dy2);
            calc_orthogonal(d, x3, y3, x1, y1, &dx3, &dy3);
        }
        *x++ = x1 + dx1;  *y++ = y1 + dy1;
        *x++ = x2 + dx1;  *y++ = y2 + dy1;
        *x++ = x2 + dx2;  *y++ = y2 + dy2;
        *x++ = x3 + dx2;  *y++ = y3 + dy2;
        *x++ = x3 + dx3;  *y++ = y3 + dy3;
        *x++ = x1 + dx3;  *y++ = y1 + dy3;
    }

    constexpr double calc_triangle_area(double x1, double y1, double x2, double y2, double x3, double y3) noexcept
    {
        return (x1*y2 - x2*y1 + x2*y3 - x3*y2 + x3*y1 - x1*y3) * 0.5;
    }

    template<typename Storage>
    requires requires(const Storage& st) {
        st.size();
        st[0].x;
        st[0].y;
    }
    constexpr auto calc_polygon_area(const Storage& st) noexcept -> double {
        const auto size = st.size();
        if (size < 3) return 0.0;

        double sum = 0.0;
        double prev_x = st[0].x;
        double prev_y = st[0].y;
        const double first_x = prev_x;
        const double first_y = prev_y;

        // Vectorization-friendly loop with minimal dependencies
        for(unsigned i = 1; i < size; ++i) {
            const double curr_x = st[i].x;
            const double curr_y = st[i].y;
            sum += prev_x * curr_y - prev_y * curr_x;
            prev_x = curr_x;
            prev_y = curr_y;
        }

        // Close the polygon
        sum += prev_x * first_y - prev_y * first_x;
        return sum * 0.5;
    }

    // Integer square root.

    AGG_INLINE unsigned fast_sqrt(unsigned val) {
       // Compiling to a hardware sqrt instruction is expected from the compiler
       return unsigned(std::sqrt(double(val)));
    }

    // Function BESJ calculates Bessel function of first kind of order n
    // Arguments:
    //     n - an integer (>=0), the order
    //     x - value at which the Bessel function is required
    //--------------------
    // C++ Mathematical Library
    // Convereted from equivalent FORTRAN library
    // Converetd by Gareth Walker for use by course 392 computational project
    // All functions tested and yield the same results as the corresponding
    // FORTRAN versions.
    //
    // If you have any problems using these functions please report them to
    // M.Muldoon@UMIST.ac.uk
    //
    // Documentation available on the web
    // http://www.ma.umist.ac.uk/mrm/Teaching/392/libs/392.html
    // Version 1.0   8/98
    // 29 October, 1999
    //--------------------
    // Adapted for use in AGG library by Andy Wilk (castor.vulgaris@gmail.com)
    //------------------------------------------------------------------------
    constexpr double besj(double x, int n) noexcept {
        if (n < 0) {
            return 0.0;
        }
        constexpr double d = 1E-6;
        double b = 0.0;
        if (std::abs(x) <= d) {
            if (n != 0) return 0.0;
            return 1.0;
        }
        double b1 = 0.0; // b1 is the value from the previous iteration
        // Set up a starting order for recurrence
        int m1 = static_cast<int>(std::abs(x)) + 6;
        if (std::abs(x) > 5.0) {
            m1 = static_cast<int>(std::abs(1.4 * x + 60.0 / x));
        }
        int m2 = static_cast<int>(n + 2 + std::abs(x) / 4.0);
        if (m1 > m2) {
            m2 = m1;
        }

        // Apply recurrence down from curent max order
        for(;;)
        {
            double c3 = 0;
            double c2 = 1E-30;
            double c4 = 0;
            int m8 = 1;
            if (m2 / 2 * 2 == m2)
            {
                m8 = -1;
            }
            int imax = m2 - 2;
            for (int i = 1; i <= imax; i++)
            {
                double c6 = 2 * (m2 - i) * c2 / x - c3;
                c3 = c2;
                c2 = c6;
                if(m2 - i - 1 == n)
                {
                    b = c6;
                }
                m8 = -1 * m8;
                if (m8 > 0)
                {
                    c4 = c4 + 2 * c6;
                }
            }
            double c6 = 2 * c2 / x - c3;
            if(n == 0)
            {
                b = c6;
            }
            c4 += c6;
            b /= c4;
            if (std::abs(b - b1) < d) {
                return b;
            }
            b1 = b;
            m2 += 3;
        }
    }

}


#endif
