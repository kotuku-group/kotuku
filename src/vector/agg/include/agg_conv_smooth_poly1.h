//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Smooths polygonal paths using AGG's first-order smoothing generator. Hooks into conv_adaptor_vcgen with
// vcgen_smooth_poly1. In the vector renderer it reduces sharp polygonal artefacts before curves or scanline
// rasterisation are emitted.

#pragma once

#include "agg_basics.h"
#include "agg_vcgen_smooth_poly1.h"
#include "agg_conv_adaptor_vcgen.h"
#include "agg_conv_curve.h"


namespace agg
{
    template<class VertexSource>
    struct conv_smooth_poly1 :
    public conv_adaptor_vcgen<VertexSource, vcgen_smooth_poly1>
    {
        typedef conv_adaptor_vcgen<VertexSource, vcgen_smooth_poly1> base_type;

        conv_smooth_poly1(VertexSource& vs) :
            conv_adaptor_vcgen<VertexSource, vcgen_smooth_poly1>(vs)
        {
        }

        void   smooth_value(double v) { base_type::generator().smooth_value(v); }
        double smooth_value() const { return base_type::generator().smooth_value(); }

    private:
        conv_smooth_poly1(const conv_smooth_poly1<VertexSource>&);
        const conv_smooth_poly1<VertexSource>&
            operator = (const conv_smooth_poly1<VertexSource>&);
    };

    template<class VertexSource>
    struct conv_smooth_poly1_curve :
    public conv_curve<conv_smooth_poly1<VertexSource> >
    {
        conv_smooth_poly1_curve(VertexSource& vs) :
            conv_curve<conv_smooth_poly1<VertexSource> >(m_smooth),
            m_smooth(vs)
        {
        }

        void   smooth_value(double v) { m_smooth.generator().smooth_value(v); }
        double smooth_value() const { return m_smooth.generator().smooth_value(); }

    private:
        conv_smooth_poly1_curve(const conv_smooth_poly1_curve<VertexSource>&);
        const conv_smooth_poly1_curve<VertexSource>&
            operator = (const conv_smooth_poly1_curve<VertexSource>&);

        conv_smooth_poly1<VertexSource> m_smooth;
    };

}
