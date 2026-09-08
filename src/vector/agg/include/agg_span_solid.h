// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Generates spans filled with a single colour. Hooks into renderer_scanline through a span allocator and any AGG colour
// type. In the vector renderer it is the simplest fill source for solid-colour paths, strokes, and masks.

#pragma once

#include "agg_basics.h"

namespace agg
{
   template<class ColorT> class span_solid {
   public:
      typedef ColorT color_type;

      void color(const color_type& c) { m_color = c; }
      const color_type& color() const { return m_color; }

      void prepare() {}

      void generate(color_type* span, int x, int y, unsigned len) {
         do { *span++ = m_color; } while(--len);
      }

   private:
      color_type m_color;
   };
}
