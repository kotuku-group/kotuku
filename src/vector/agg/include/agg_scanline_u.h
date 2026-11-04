// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software is granted provided this copyright notice
// appears in all copies.  This software is provided "as is" without express or implied warranty, and with no
// claim as to its suitability for any purpose.
// ---
// Defines unpacked anti-aliased scanlines with one coverage span per run. Hooks into rasterizer_scanline_aa and
// renderer_scanline. In the vector renderer it favours straightforward coverage access when scanline memory compaction
// is less important.
//
// This is a general purpose scanline container with *packed* spans.  It is best used in conjunction with cover
// values that mostly continuous.  See description of scanline_u8 for details.
//
//********************************************************************************************************************
//
// One of the key concepts in AGG is the scanline. The scanline is a container that consist of a number of horizontal
// spans that can carry Anti-Aliasing information. The scanline renderer decomposes provided scanline into a number
// of spans and in simple cases (like solid fill) calls basic renderer. In more complex cases it can call span
// generator.
//
// Unpacked scanline container class
// =================================
// This class is used to transfer data from a scanline rasterizer to the rendering buffer. It's organized very simple.
// The class stores information of horizontal spans to render it into a pixel-map buffer.  Each span has staring X,
// length, and an array of bytes that determine the cover-values for each pixel.
//
// Before using this class you should know the minimal and maximal pixel coordinates of your scanline. The protocol
// of using is:
//
// 1. reset(min_x, max_x)
// 2. add_cell() / add_span() - accumulate scanline.
//    When forming one scanline the next X coordinate must be always greater
//    than the last stored one, i.e. it works only with ordered coordinates.
// 3. Call finalize(y) and render the scanline.
// 3. Call reset_spans() to prepare for the new scanline.
// 4. Rendering:
//
// Scanline provides an iterator class that allows you to extract the spans and the cover values for each pixel. Be
// aware that clipping has not been done yet, so you should perform it yourself.  Use scanline_u8::iterator to render
// spans:
//
// int y = sl.y(); // Y-coordinate of the scanline
//
// ************************************
// ...Perform vertical clipping here...
// ************************************
//
// scanline_u8::const_iterator span = sl.begin();
//
// unsigned char* row = m_rbuf->row(y); // The the address of the beginning of the current row
// unsigned num_spans = sl.num_spans(); // Number of spans. It's guaranteed that num_spans is always greater than 0.
//
// do {
//     const scanline_u8::cover_type* covers = span->covers; // The array of the cover values
//     int num_pix = span->len; // Number of pixels of the span
//     int x = span->x;
//
//     **************************************
//     ...Perform horizontal clipping here...
//     ...you have x, covers, and pix_count..
//     **************************************
//
//     unsigned char* dst = row + x;  // Calculate the start address of the row.  In this case we assume a simple
//                                    // grayscale image 1-byte per pixel.
//     do {
//         *dst++ = *covers++;        // Hypothetical rendering.
//     } while(--num_pix);
//     ++span;
// } while(--num_spans);  // num_spans cannot be 0, so this loop is quite safe
//
// The question is: why should we accumulate the whole scanline when we could render just separate spans when they're
// ready? That's because using the scanline is generally faster. When is consists of more than one span the
// conditions for the processor cache system are better, because switching between two different areas of memory
// (that can be very large) occurs less frequently.

#pragma once

#include "agg_array.h"

namespace agg {

//********************************************************************************************************************

class scanline_u8 {
public:
   typedef scanline_u8 self_type;
   typedef int8u cover_type;
   typedef int32 coord_type;

   struct span {
      span() {}
      span(coord_type x_, coord_type len_, cover_type* covers_) : x(x_), len(len_), covers(covers_) {}

      coord_type  x;
      coord_type  len;
      cover_type* covers;
   };

   typedef pod_bvector<span, 4> span_array_type;

   class const_iterator {
   public:
      const_iterator(const span_array_type& spans) : m_spans(spans), m_span_idx(0) { }

      const span& operator*()  const { return m_spans[m_span_idx]; }
      const span* operator->() const { return &m_spans[m_span_idx]; }

      void operator ++ () { ++m_span_idx; }

   private:
      const span_array_type &m_spans;
      unsigned m_span_idx;
   };

   class iterator {
   public:
      iterator(span_array_type& spans) : m_spans(spans), m_span_idx(0) { }

      span& operator*()  { return m_spans[m_span_idx];  }
      span* operator->() { return &m_spans[m_span_idx]; }
      void operator ++ () { ++m_span_idx; }

   private:
      span_array_type& m_spans;
      unsigned         m_span_idx;
   };

   scanline_u8() : m_min_x(0), m_last_x(0x7FFFFFF0), m_covers() { }

   inline void reset(int min_x, int max_x) {
      unsigned max_len = max_x - min_x + 2;
      if (max_len > m_covers.size()) m_covers.resize(max_len);
      m_last_x = 0x7FFFFFF0;
      m_min_x  = min_x;
      m_spans.remove_all();
   }

   inline void add_cell(int x, unsigned cover) {
      x -= m_min_x;
      m_covers[x] = cover_type(cover);
      if (x == m_last_x+1) m_spans.last().len++;
      else m_spans.add(span(coord_type(x + m_min_x), 1, &m_covers[x]));
      m_last_x = x;
   }

   inline void add_cells(int x, unsigned len, const cover_type* covers) {
      x -= m_min_x;
      memcpy(&m_covers[x], covers, len * sizeof(cover_type));
      if (x == m_last_x+1) m_spans.last().len += coord_type(len);
      else m_spans.add(span(coord_type(x + m_min_x), coord_type(len), &m_covers[x]));
      m_last_x = x + len - 1;
   }

   inline void add_span(int x, unsigned len, unsigned cover) {
      x -= m_min_x;
      memset(&m_covers[x], cover, len);
      if (x == m_last_x+1) m_spans.last().len += coord_type(len);
      else m_spans.add(span(coord_type(x + m_min_x), coord_type(len), &m_covers[x]));
      m_last_x = x + len - 1;
   }

   inline void finalize(int y) {
      m_y = y;
   }

   inline void reset_spans() {
      m_last_x = 0x7FFFFFF0;
      m_spans.remove_all();
   }

   constexpr int y() const { return m_y; }
   inline unsigned num_spans() const { return m_spans.size(); }
   const_iterator begin() const { return const_iterator(m_spans); }
   iterator begin() { return iterator(m_spans); }

private:
   scanline_u8(const self_type&);
   const self_type& operator = (const self_type&);

private:
   int m_min_x;
   int m_last_x;
   int m_y;
   pod_array<cover_type> m_covers;
   span_array_type m_spans;
};

// The scanline container with alpha-masking.  It is viable to initialise with a NULL mask, in which case behaviour
// will revert to the non-masked default without a performance penalty.

template<class AlphaMask>
class scanline_u8_am : public scanline_u8 {
public:
   typedef scanline_u8 base_type;
   typedef AlphaMask alpha_mask_type;
   typedef base_type::cover_type cover_type;
   typedef base_type::coord_type coord_type;

   scanline_u8_am() : base_type(), m_alpha_mask(0) {}
   scanline_u8_am(const AlphaMask& am) : base_type(), m_alpha_mask(&am) {}

   void finalize(int span_y) {
      base_type::finalize(span_y);
      if (m_alpha_mask) {
         typename base_type::iterator span = base_type::begin();
         unsigned count = base_type::num_spans();
         do {
            m_alpha_mask->combine_hspan(span->x, base_type::y(), span->covers, span->len);
            ++span;
         } while(--count);
      }
   }

private:
   const AlphaMask *m_alpha_mask;
};

} // namespace
