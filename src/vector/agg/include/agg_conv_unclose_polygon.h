// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Wraps a vertex source and suppresses close-polygon commands. Hooks into converter chains that need open contours,
// usually before stroke or marker processing. In the vector renderer it prevents fill-style closure from affecting line
// and marker geometry.

#pragma once

#include "agg_basics.h"

namespace agg
{
    template<class VertexSource> class conv_unclose_polygon
    {
    public:
        explicit conv_unclose_polygon(VertexSource& vs) : m_source(&vs) {}
        void attach(VertexSource& source) { m_source = &source; }

        void rewind(unsigned path_id)
        {
            m_source->rewind(path_id);
        }

        unsigned vertex(double* x, double* y)
        {
            unsigned cmd = m_source->vertex(x, y);
            if(is_end_poly(cmd)) cmd &= ~path_flags_close;
            return cmd;
        }

    private:
        conv_unclose_polygon(const conv_unclose_polygon<VertexSource>&);
        const conv_unclose_polygon<VertexSource>&
            operator = (const conv_unclose_polygon<VertexSource>&);

        VertexSource* m_source;
    };

}
