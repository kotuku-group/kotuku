// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
//
// Adaptation for high precision colors has been sponsored by Liberty Technology Systems, Inc., visit http://lib-sys.com
//
// Liberty Technology Systems, Inc. is the provider of PostScript and PDF technology for software developers.
// ---
// Generates RGBA Gouraud-shaded spans for triangles. Hooks into span_gouraud interpolation and RGBA renderer_scanline
// paths. In the vector renderer it produces smoothly interpolated colours across triangular vector primitives.

#pragma once

#include "agg_basics.h"
#include "agg_color_rgba.h"
#include "agg_span_gouraud.h"

namespace agg {

template<class ColorT> class span_gouraud_rgba : public span_gouraud<ColorT> {
public:
    typedef ColorT color_type;
    typedef typename ColorT::value_type value_type;
    typedef span_gouraud<color_type> base_type;
    typedef typename base_type::coord_type coord_type;
    enum subpixel_scale_e
    {
        subpixel_shift = 4,
        subpixel_scale = 1 << subpixel_shift
    };

private:
    struct rgba_calc
    {
        void init(const coord_type& c1, const coord_type& c2)
        {
            m_x1  = c1.x - 0.5;
            m_y1  = c1.y - 0.5;
            m_dx  = c2.x - c1.x;
            double dy = c2.y - c1.y;
            m_1dy = (dy < 1e-5) ? 1e5 : 1.0 / dy;
            m_r1  = c1.color.r;
            m_g1  = c1.color.g;
            m_b1  = c1.color.b;
            m_a1  = c1.color.a;
            m_dr  = c2.color.r - m_r1;
            m_dg  = c2.color.g - m_g1;
            m_db  = c2.color.b - m_b1;
            m_da  = c2.color.a - m_a1;
        }

        void calc(double y)
        {
            double k = (y - m_y1) * m_1dy;
            if(k < 0.0) k = 0.0;
            if(k > 1.0) k = 1.0;
            m_r = m_r1 + iround(m_dr * k);
            m_g = m_g1 + iround(m_dg * k);
            m_b = m_b1 + iround(m_db * k);
            m_a = m_a1 + iround(m_da * k);
            m_x = iround((m_x1 + m_dx * k) * int(subpixel_scale));
        }

        double m_x1;
        double m_y1;
        double m_dx;
        double m_1dy;
        int    m_r1;
        int    m_g1;
        int    m_b1;
        int    m_a1;
        int    m_dr;
        int    m_dg;
        int    m_db;
        int    m_da;
        int    m_r;
        int    m_g;
        int    m_b;
        int    m_a;
        int    m_x;
    };

public:
    span_gouraud_rgba() {}
    span_gouraud_rgba(const color_type& c1, const color_type& c2, const color_type& c3, double x1, double y1,
       double x2, double y2, double x3, double y3, double d = 0) :
       base_type(c1, c2, c3, x1, y1, x2, y2, x3, y3, d)
    {}

    void prepare() {
        coord_type coord[3];
        base_type::arrange_vertices(coord);

        m_y2 = int(coord[1].y);

        m_swap = cross_product(coord[0].x, coord[0].y, coord[2].x, coord[2].y, coord[1].x, coord[1].y) < 0.0;

        m_rgba1.init(coord[0], coord[2]);
        m_rgba2.init(coord[0], coord[1]);
        m_rgba3.init(coord[1], coord[2]);
    }

    template<class Output, class Convert>
    void generate(Output *Span, int X, int Y, unsigned Length, Convert ConvertColour)
    {
        m_rgba1.calc(Y);
        const rgba_calc *left = &m_rgba1;
        const rgba_calc *right = &m_rgba2;

        if (Y <= m_y2) m_rgba2.calc(Y + m_rgba2.m_1dy);
        else {
            m_rgba3.calc(Y - m_rgba3.m_1dy);
            right = &m_rgba3;
        }

        if (m_swap) std::swap(left, right);

        // Keep fractional precision for shallow slopes without rounding at each horizontal step.
        const double width = std::max(int64_t(1), std::abs(int64_t(right->m_x) - left->m_x));
        const double start = double(X) * subpixel_scale - left->m_x;
        const int first[4] = { left->m_r, left->m_g, left->m_b, left->m_a };
        const int last[4] = { right->m_r, right->m_g, right->m_b, right->m_a };
        double position[4], step[4];
        for (int channel=0; channel < 4; ++channel) {
            const double slope = (last[channel] - first[channel]) / width;
            position[channel] = first[channel] + slope * start;
            step[channel] = slope * subpixel_scale;
        }

        for (unsigned i=0; i < Length; i++) {
            int64_t red = int64_t(position[0] + 0.5);
            int64_t green = int64_t(position[1] + 0.5);
            int64_t blue = int64_t(position[2] + 0.5);
            int64_t alpha = int64_t(position[3] + 0.5);

            // Antialiasing can extend the span beyond the interpolated edges.
            constexpr int64_t limit = color_type::base_mask;
            if ((red | green | blue | alpha) & ~limit) {
                red   = std::clamp(red,   int64_t(0), limit);
                green = std::clamp(green, int64_t(0), limit);
                blue  = std::clamp(blue,  int64_t(0), limit);
                alpha = std::clamp(alpha, int64_t(0), limit);
            }

            Span[i] = ConvertColour(value_type(red), value_type(green), value_type(blue), value_type(alpha));
            position[0] += step[0];
            position[1] += step[1];
            position[2] += step[2];
            position[3] += step[3];
        }
    }

private:
    bool      m_swap;
    int       m_y2;
    rgba_calc m_rgba1;
    rgba_calc m_rgba2;
    rgba_calc m_rgba3;
};

} // namespace
