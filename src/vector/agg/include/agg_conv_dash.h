// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Applies dash patterns to streamed path geometry. Hooks into conv_adaptor_vcgen with vcgen_dash and then into stroke
// or rasteriser stages. In the vector renderer it turns styled strokes into separate dash subpaths before outline
// generation.

#pragma once

#include "agg_basics.h"
#include "agg_vcgen_dash.h"
#include "agg_conv_adaptor_vcgen.h"

namespace agg
{
   template<class VertexSource, class Markers=null_markers>
   struct conv_dash : public conv_adaptor_vcgen<VertexSource, vcgen_dash, Markers>
   {
      typedef Markers marker_type;
      typedef conv_adaptor_vcgen<VertexSource, vcgen_dash, Markers> base_type;

      conv_dash(VertexSource& vs) :
         conv_adaptor_vcgen<VertexSource, vcgen_dash, Markers>(vs) { }

      constexpr void remove_all_dashes() {
         base_type::generator().remove_all_dashes();
      }

      constexpr void add_dash(double dash_len, double gap_len) {
         base_type::generator().add_dash(dash_len, gap_len);
      }

      constexpr void dash_start(double ds) {
         base_type::generator().dash_start(ds);
      }

      constexpr double dash_length() {
         return base_type::generator().dash_length();
      }

      constexpr void shorten(double s) { base_type::generator().shorten(s); }
      constexpr double shorten() const { return base_type::generator().shorten(); }

   private:
      conv_dash(const conv_dash<VertexSource, Markers>&);
      const conv_dash<VertexSource, Markers>&
         operator = (const conv_dash<VertexSource, Markers>&);
   };
}
