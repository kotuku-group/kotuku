//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Wraps a span generator with a post-generation span converter. Hooks into renderer_scanline pipelines that need
// colour-space or alpha adjustments after generation. In the vector renderer it allows generated spans to be
// transformed before they reach the pixel blender.

#pragma once

#include "agg_basics.h"

namespace agg
{
    //----------------------------------------------------------span_converter
    template<class SpanGenerator, class SpanConverter> class span_converter
    {
    public:
        typedef typename SpanGenerator::color_type color_type;

        span_converter(SpanGenerator& span_gen, SpanConverter& span_cnv) :
            m_span_gen(&span_gen), m_span_cnv(&span_cnv) {}

        void attach_generator(SpanGenerator& span_gen) { m_span_gen = &span_gen; }
        void attach_converter(SpanConverter& span_cnv) { m_span_cnv = &span_cnv; }

        //--------------------------------------------------------------------
        void prepare()
        {
            m_span_gen->prepare();
            m_span_cnv->prepare();
        }

        //--------------------------------------------------------------------
        void generate(color_type* span, int x, int y, unsigned len)
        {
            m_span_gen->generate(span, x, y, len);
            m_span_cnv->generate(span, x, y, len);
        }

    private:
        SpanGenerator* m_span_gen;
        SpanConverter* m_span_cnv;
    };

}
