// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Adapts a marker generator so marker vertices can be read as a normal vertex source. Hooks into marker-aware
// converters and renderer_markers. In the vector renderer it exposes collected marker geometry through the same
// rewind()/vertex() contract as paths.

#pragma once

#include "agg_basics.h"
#include "agg_conv_adaptor_vcgen.h"
#include "agg_vcgen_vertex_sequence.h"

namespace agg
{
    template<class VertexSource, class Markers=null_markers>
    struct conv_marker_adaptor :
    public conv_adaptor_vcgen<VertexSource, vcgen_vertex_sequence, Markers>
    {
        typedef Markers marker_type;
        typedef conv_adaptor_vcgen<VertexSource, vcgen_vertex_sequence, Markers> base_type;

        conv_marker_adaptor(VertexSource& vs) :
            conv_adaptor_vcgen<VertexSource, vcgen_vertex_sequence, Markers>(vs)
        {
        }

        void shorten(double s) { base_type::generator().shorten(s); }
        double shorten() const { return base_type::generator().shorten(); }

    private:
        conv_marker_adaptor(const conv_marker_adaptor<VertexSource, Markers>&);
        const conv_marker_adaptor<VertexSource, Markers>&
            operator = (const conv_marker_adaptor<VertexSource, Markers>&);
    };


}
