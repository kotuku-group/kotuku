//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Concatenates two vertex sources into one stream. Hooks into render paths that need to draw combined geometry without
// materialising a new path_storage. In the vector renderer it lets related contours pass through converters and
// rasterisers as a single source.

#pragma once

#include "agg_basics.h"

namespace agg
{
    //=============================================================conv_concat
    // Concatenation of two paths. Usually used to combine lines or curves
    // with markers such as arrowheads
    template<class VS1, class VS2> class conv_concat
    {
    public:
        conv_concat(VS1& source1, VS2& source2) :
            m_source1(&source1), m_source2(&source2), m_status(2) {}
        void attach1(VS1& source) { m_source1 = &source; }
        void attach2(VS2& source) { m_source2 = &source; }


        void rewind(unsigned path_id)
        {
            m_source1->rewind(path_id);
            m_source2->rewind(0);
            m_status = 0;
        }

        unsigned vertex(double* x, double* y)
        {
            unsigned cmd;
            if(m_status == 0)
            {
                cmd = m_source1->vertex(x, y);
                if(!is_stop(cmd)) return cmd;
                m_status = 1;
            }
            if(m_status == 1)
            {
                cmd = m_source2->vertex(x, y);
                if(!is_stop(cmd)) return cmd;
                m_status = 2;
            }
            return path_cmd_stop;
        }

    private:
        conv_concat(const conv_concat<VS1, VS2>&);
        const conv_concat<VS1, VS2>&
            operator = (const conv_concat<VS1, VS2>&);

        VS1* m_source1;
        VS2* m_source2;
        int  m_status;

    };
}
