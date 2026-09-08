// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software is granted provided this copyright notice
// appears in all copies.  This software is provided "as is" without express or implied warranty, and with no
// claim as to its suitability for any purpose.
// ---
// Computes bounds for one or more vertex-source paths. Hooks into any source that implements rewind() and vertex(),
// including path_storage and converters. In the vector renderer it provides quick geometry extents for clipping,
// culling, and layout decisions.

#pragma once

#include "agg_basics.h"

namespace agg
{
template<class VertexSource, class GetId, class V>
bool bounding_rect(VertexSource& vs, GetId& gi, unsigned start, unsigned num, V* x1, V* y1, V* x2, V* y2)
{
   double x, y;
   bool first = true;

   *x1 = V(1);
   *y1 = V(1);
   *x2 = V(0);
   *y2 = V(0);

   for (unsigned i = 0; i < num; i++) {
      vs.rewind(gi[start + i]);
      unsigned cmd;
      while (!is_stop(cmd = vs.vertex(&x, &y))) {
         if (is_vertex(cmd)) {
            if (first) {
               *x1 = V(x);
               *y1 = V(y);
               *x2 = V(x);
               *y2 = V(y);
               first = false;
            }
            else {
               if (V(x) < *x1) *x1 = V(x);
               if (V(y) < *y1) *y1 = V(y);
               if (V(x) > *x2) *x2 = V(x);
               if (V(y) > *y2) *y2 = V(y);
            }
         }
      }
   }
   return (*x1 <= *x2) and (*y1 <= *y2);
}

template<class VertexSource, class V>
bool bounding_rect_single(VertexSource& vs, unsigned path_id, V* x1, V* y1, V* x2, V* y2)
{
   double x, y;
   bool first = true;

   *x1 = V(1);
   *y1 = V(1);
   *x2 = V(0);
   *y2 = V(0);

   vs.rewind(path_id);
   unsigned cmd;
   while (!is_stop(cmd = vs.vertex(&x, &y))) {
      if (is_vertex(cmd)) {
         if (first) {
            *x1 = V(x);
            *y1 = V(y);
            *x2 = V(x);
            *y2 = V(y);
            first = false;
         }
         else {
            if (V(x) < *x1) *x1 = V(x);
            if (V(y) < *y1) *y1 = V(y);
            if (V(x) > *x2) *x2 = V(x);
            if (V(y) > *y2) *y2 = V(y);
         }
      }
   }
   return (*x1 <= *x2) and (*y1 <= *y2);
}

} // namespace
