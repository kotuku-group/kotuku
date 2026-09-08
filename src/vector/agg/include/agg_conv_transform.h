// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Applies a coordinate transform to every vertex from a source path. Hooks into trans_affine, trans_perspective, and
// other transformer classes used by vertex pipelines. In the vector renderer it maps local path geometry into target
// space before clipping and rasterisation.

#pragma once

#include "agg_basics.h"
#include "agg_trans_affine.h"

namespace agg
{

template<class VertexSource, class Transformer=trans_affine> class conv_transform
{
public:
   conv_transform(VertexSource& source, const Transformer& tr) : m_source(&source), m_trans(&tr) {}
   void attach(VertexSource& source) { m_source = &source; }

   void rewind(unsigned path_id) { m_source->rewind(path_id); }

   unsigned vertex(double* x, double* y) {
      unsigned cmd = m_source->vertex(x, y);
      if (is_vertex(cmd)) m_trans->transform(x, y);
      return cmd;
   }

   void transformer(const Transformer& tr) { m_trans = &tr; }

private:
   conv_transform(const conv_transform<VertexSource>&);
   const conv_transform<VertexSource> & operator = (const conv_transform<VertexSource>&);
   VertexSource *      m_source;
   const Transformer * m_trans;
};

}
