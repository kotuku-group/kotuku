// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software is granted provided this copyright notice
// appears in all copies.  This software is provided "as is" without express or implied warranty, and with no
// claim as to its suitability for any purpose.
//
// This is a general purpose scanline container with *packed* spans.  It is best used in conjunction with cover
// values that are mostly continuous.  See description of scanline_u8 for details.
//
// This variant combines the int32 coordinate range of AGG's scanline32_p8 with the flat span storage of
// scanline_p8: spans live in a single contiguous array addressed through a bumped tail pointer, so appending
// and iterating avoid the block-vector double indirection of the original 32-bit container.  reset() bounds
// the worst-case span count (every span covers at least one pixel), so the array is sized once per sweep
// with no reallocation during scanline construction.

#pragma once

#include "agg_array.h"

namespace agg
{
class scanline32_p8 {
public:
   typedef scanline32_p8 self_type;
   typedef int8u cover_type;
   typedef int32 coord_type;

   struct span {
      coord_type x;
      coord_type len; // If negative, it's a solid span, covers is valid
      const cover_type* covers;
   };

   typedef const span* const_iterator;

   scanline32_p8() : m_max_len(0), m_last_x(0x7FFFFFF0), m_y(0), m_cover_ptr(0), m_cur_span(0) { }

   void reset(int min_x, int max_x) {
      unsigned max_len = max_x - min_x + 3;
      if (max_len > m_max_len) {
         m_covers.resize(max_len);
         m_spans.resize(max_len);
         m_max_len = max_len;
      }
      m_last_x    = 0x7FFFFFF0;
      m_cover_ptr = &m_covers[0];
      m_cur_span  = &m_spans[0]; // Sentinel; begin() starts at m_spans[1]
      m_cur_span->len = 0;
   }

   void add_cell(int x, unsigned cover) {
      *m_cover_ptr = cover_type(cover);
      if ((x IS m_last_x+1) and (m_cur_span->len > 0)) m_cur_span->len++;
      else {
         m_cur_span++;
         m_cur_span->x      = coord_type(x);
         m_cur_span->len    = 1;
         m_cur_span->covers = m_cover_ptr;
      }
      m_last_x = x;
      m_cover_ptr++;
   }

   void add_cells(int x, unsigned len, const cover_type *covers) {
      memcpy(m_cover_ptr, covers, len * sizeof(cover_type));
      if ((x IS m_last_x+1) and (m_cur_span->len > 0)) m_cur_span->len += coord_type(len);
      else {
         m_cur_span++;
         m_cur_span->x      = coord_type(x);
         m_cur_span->len    = coord_type(len);
         m_cur_span->covers = m_cover_ptr;
      }
      m_cover_ptr += len;
      m_last_x = x + len - 1;
   }

   void add_span(int x, unsigned len, unsigned cover) {
      if ((x IS m_last_x+1) and (m_cur_span->len < 0) and (cover IS *m_cur_span->covers)) {
         m_cur_span->len -= coord_type(len);
      }
      else {
         *m_cover_ptr = cover_type(cover);
         m_cur_span++;
         m_cur_span->x      = coord_type(x);
         m_cur_span->len    = -coord_type(len);
         m_cur_span->covers = m_cover_ptr++;
      }
      m_last_x = x + len - 1;
   }

   void finalize(int y) { m_y = y; }

   void reset_spans() {
      m_last_x    = 0x7FFFFFF0;
      m_cover_ptr = &m_covers[0];
      m_cur_span  = &m_spans[0];
      m_cur_span->len = 0;
   }

   int y() const { return m_y; }
   unsigned num_spans() const { return unsigned(m_cur_span - &m_spans[0]); }
   const_iterator begin() const { return &m_spans[1]; }

private:
   scanline32_p8(const self_type&);
   const self_type& operator = (const self_type&);

   unsigned m_max_len;
   int m_last_x;
   int m_y;
   pod_array<cover_type> m_covers;
   pod_array<span> m_spans;
   cover_type *m_cover_ptr;
   span *m_cur_span;
};

} // namespace
