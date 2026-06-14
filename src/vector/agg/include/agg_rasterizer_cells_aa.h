// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
//
// The author gratefully acknowleges the support of David Turner,
// Robert Wilhelm, and Werner Lemberg - the authors of the FreeType
// libray - in producing this work. See http://www.freetype.org for details.
// ---
// Stores and sorts anti-aliased rasteriser cells. Hooks into rasterizer_scanline_aa and compound rasterisers. In the
// vector renderer it accumulates edge coverage before scanlines are emitted for blending.

#pragma once

#include <string.h>
#include <math.h>
#include <algorithm>
#include <cstdint>
#include "agg_math.h"
#include "agg_array.h"

namespace agg
{
    // An internal class that implements the main rasterization algorithm.
    // Used in the rasterizer. Should not be used direcly.
    template<class Cell> class rasterizer_cells_aa {
        enum cell_block_scale_e {
            cell_block_shift = 12,
            cell_block_size  = 1 << cell_block_shift,
            cell_block_mask  = cell_block_size - 1,
            cell_block_pool  = 256,
            cell_block_limit = 1024
        };

        struct sorted_y {
            unsigned start = 0;
            unsigned num = 0;
        };

    public:
        typedef Cell cell_type; // Refer agg_rasterizer_scanline_aa::cell_aa
        typedef rasterizer_cells_aa<Cell> self_type;

        rasterizer_cells_aa();

        void reset();
        void style(const cell_type& style_cell);
        void line(int x1, int y1, int x2, int y2);

        int min_x() const { return m_min_x; }
        int min_y() const { return m_min_y; }
        int max_x() const { return m_max_x; }
        int max_y() const { return m_max_y; }

        void sort_cells();

        unsigned total_cells() const { return m_num_cells; }

        unsigned scanline_num_cells(unsigned y) const {
            return m_sorted_y[y - m_min_y].num;
        }

        const cell_type* scanline_cells(unsigned y) const {
            return m_sorted_cells.data() + m_sorted_y[y - m_min_y].start;
        }

        bool sorted() const { return m_sorted; }

    private:
        rasterizer_cells_aa(const self_type&);
        const self_type& operator = (const self_type&);

        void set_curr_cell(int x, int y);
        void add_curr_cell();
        void render_hline(int ey, int x1, int y1, int x2, int y2);
        void allocate_block();

    private:
        unsigned                m_num_blocks;
        unsigned                m_curr_block;
        unsigned                m_num_cells;
        std::vector<std::unique_ptr<cell_type[]>> m_cells;
        cell_type*              m_curr_cell_ptr;
        std::vector<cell_type>  m_sorted_cells;
        std::vector<sorted_y>   m_sorted_y;
        std::vector<uint64_t>   m_sort_keys;    // Scratch for sort_cells' per-scanline key sort
        std::vector<cell_type>  m_cell_scratch; // Scratch for sort_cells' gather pass
        cell_type m_curr_cell;
        cell_type m_style_cell;
        int m_min_x;
        int m_min_y;
        int m_max_x;
        int m_max_y;
        bool m_sorted;
    };

    template<class Cell>
    rasterizer_cells_aa<Cell>::rasterizer_cells_aa() :
        m_num_blocks(0),
        m_curr_block(0),
        m_num_cells(0),
        m_curr_cell_ptr(0),
        m_min_x(0x7FFFFFFF),
        m_min_y(0x7FFFFFFF),
        m_max_x(-0x7FFFFFFF),
        m_max_y(-0x7FFFFFFF),
        m_sorted(false)
    {
        m_style_cell.initial();
        m_curr_cell.initial();
    }

    template<class Cell>
    void rasterizer_cells_aa<Cell>::reset()
    {
        m_num_cells  = 0;
        m_curr_block = 0;
        m_curr_cell.initial();
        m_style_cell.initial();
        m_sorted = false;
        m_min_x  = 0x7FFFFFFF;
        m_min_y  = 0x7FFFFFFF;
        m_max_x  = -0x7FFFFFFF;
        m_max_y  = -0x7FFFFFFF;
    }

    template<class Cell>
    AGG_INLINE void rasterizer_cells_aa<Cell>::add_curr_cell() {
        if (m_curr_cell.area | m_curr_cell.cover) {
            if ((m_num_cells & cell_block_mask) == 0) {
                if (m_num_blocks >= cell_block_limit) return;
                allocate_block();
            }
            *m_curr_cell_ptr++ = m_curr_cell;
            ++m_num_cells;
        }
    }

    template<class Cell>
    AGG_INLINE void rasterizer_cells_aa<Cell>::set_curr_cell(int x, int y)
    {
        if (m_curr_cell.not_equal(x, y, m_style_cell)) {
            add_curr_cell();
            m_curr_cell.style(m_style_cell);
            m_curr_cell.x     = x;
            m_curr_cell.y     = y;
            m_curr_cell.cover = 0;
            m_curr_cell.area  = 0;
        }
    }

    template<class Cell>
    AGG_INLINE void rasterizer_cells_aa<Cell>::render_hline(int ey, int x1, int y1, int x2, int y2)
    {
        int ex1 = x1 >> poly_subpixel_shift;
        int ex2 = x2 >> poly_subpixel_shift;
        int fx1 = x1 & poly_subpixel_mask;
        int fx2 = x2 & poly_subpixel_mask;

        int delta, p, first, dx;
        int incr, lift, mod, rem;

        if (y1 == y2) { // trivial case. Happens often
            set_curr_cell(ex2, ey);
            return;
        }

        // everything is located in a single cell.  That is easy!
        if (ex1 == ex2) {
            delta = y2 - y1;
            m_curr_cell.cover += delta;
            m_curr_cell.area  += (fx1 + fx2) * delta;
            return;
        }

        // ok, we'll have to render a run of adjacent cells on the same hline...
        p     = (poly_subpixel_scale - fx1) * (y2 - y1);
        first = poly_subpixel_scale;
        incr  = 1;

        dx = x2 - x1;

        if (dx < 0) {
            p     = fx1 * (y2 - y1);
            first = 0;
            incr  = -1;
            dx    = -dx;
        }

        delta = p / dx;
        mod   = p % dx;

        if (mod < 0) {
           delta--;
           mod += dx;
        }

        m_curr_cell.cover += delta;
        m_curr_cell.area  += (fx1 + first) * delta;

        ex1 += incr;
        set_curr_cell(ex1, ey);
        y1  += delta;

        if (ex1 != ex2) {
            p     = poly_subpixel_scale * (y2 - y1 + delta);
            lift  = p / dx;
            rem   = p % dx;

            if (rem < 0) {
                lift--;
                rem += dx;
            }

            mod -= dx;

            while (ex1 != ex2) {
                delta = lift;
                mod  += rem;
                if (mod >= 0) {
                   mod -= dx;
                   delta++;
                }

                m_curr_cell.cover += delta;
                m_curr_cell.area  += poly_subpixel_scale * delta;
                y1  += delta;
                ex1 += incr;
                set_curr_cell(ex1, ey);
            }
        }
        delta = y2 - y1;
        m_curr_cell.cover += delta;
        m_curr_cell.area  += (fx2 + poly_subpixel_scale - first) * delta;
    }

    template<class Cell>
    AGG_INLINE void rasterizer_cells_aa<Cell>::style(const cell_type& style_cell)
    {
        m_style_cell.style(style_cell);
    }

    template<class Cell>
    void rasterizer_cells_aa<Cell>::line(int x1, int y1, int x2, int y2)
    {
        enum dx_limit_e { dx_limit = 16384 << poly_subpixel_shift };

        int dx = x2 - x1;

        if (dx >= dx_limit or dx <= -dx_limit) {
            int cx = (x1 + x2) >> 1;
            int cy = (y1 + y2) >> 1;
            line(x1, y1, cx, cy);
            line(cx, cy, x2, y2);
            return;
        }

        int dy = y2 - y1;
        int ex1 = x1 >> poly_subpixel_shift;
        int ex2 = x2 >> poly_subpixel_shift;
        int ey1 = y1 >> poly_subpixel_shift;
        int ey2 = y2 >> poly_subpixel_shift;
        int fy1 = y1 & poly_subpixel_mask;
        int fy2 = y2 & poly_subpixel_mask;

        int x_from, x_to;
        int p, rem, mod, lift, delta, first, incr;

        if (ex1 < m_min_x) m_min_x = ex1;
        if (ex1 > m_max_x) m_max_x = ex1;
        if (ey1 < m_min_y) m_min_y = ey1;
        if (ey1 > m_max_y) m_max_y = ey1;
        if (ex2 < m_min_x) m_min_x = ex2;
        if (ex2 > m_max_x) m_max_x = ex2;
        if (ey2 < m_min_y) m_min_y = ey2;
        if (ey2 > m_max_y) m_max_y = ey2;

        set_curr_cell(ex1, ey1);

        if (ey1 == ey2) { // Everything is on a single hline
            render_hline(ey1, x1, fy1, x2, fy2);
            return;
        }

        //Vertical line - we have to calculate start and end cells, and then - the common values of the area and coverage for
        //all cells of the line. We know exactly there's only one cell, so, we don't have to call render_hline().

        incr  = 1;
        if (dx == 0) {
            int ex = x1 >> poly_subpixel_shift;
            int two_fx = (x1 - (ex << poly_subpixel_shift)) << 1;
            int area;

            first = poly_subpixel_scale;
            if (dy < 0) {
               first = 0;
               incr  = -1;
            }

            x_from = x1;

            //render_hline(ey1, x_from, fy1, x_from, first);
            delta = first - fy1;
            m_curr_cell.cover += delta;
            m_curr_cell.area  += two_fx * delta;

            ey1 += incr;
            set_curr_cell(ex, ey1);

            delta = first + first - poly_subpixel_scale;
            area = two_fx * delta;
            while (ey1 != ey2) {
                //render_hline(ey1, x_from, poly_subpixel_scale - first, x_from, first);
                m_curr_cell.cover = delta;
                m_curr_cell.area  = area;
                ey1 += incr;
                set_curr_cell(ex, ey1);
            }
            //render_hline(ey1, x_from, poly_subpixel_scale - first, x_from, fy2);
            delta = fy2 - poly_subpixel_scale + first;
            m_curr_cell.cover += delta;
            m_curr_cell.area  += two_fx * delta;
            return;
        }

        //ok, we have to render several hlines
        p     = (poly_subpixel_scale - fy1) * dx;
        first = poly_subpixel_scale;

        if (dy < 0) {
            p     = fy1 * dx;
            first = 0;
            incr  = -1;
            dy    = -dy;
        }

        delta = p / dy;
        mod   = p % dy;

        if (mod < 0) {
           delta--;
           mod += dy;
        }

        x_from = x1 + delta;
        render_hline(ey1, x1, fy1, x_from, first);

        ey1 += incr;
        set_curr_cell(x_from >> poly_subpixel_shift, ey1);

        if (ey1 != ey2) {
            p     = poly_subpixel_scale * dx;
            lift  = p / dy;
            rem   = p % dy;

            if (rem < 0) {
                lift--;
                rem += dy;
            }
            mod -= dy;

            while (ey1 != ey2) {
                delta = lift;
                mod  += rem;
                if (mod >= 0) {
                    mod -= dy;
                    delta++;
                }

                x_to = x_from + delta;
                render_hline(ey1, x_from, poly_subpixel_scale - first, x_to, first);
                x_from = x_to;

                ey1 += incr;
                set_curr_cell(x_from >> poly_subpixel_shift, ey1);
            }
        }
        render_hline(ey1, x_from, poly_subpixel_scale - first, x2, fy2);
    }

    template<class Cell>
    void rasterizer_cells_aa<Cell>::allocate_block() {
        if (m_curr_block >= m_num_blocks) {
            if (m_num_blocks >= m_cells.size()) {
                m_cells.resize(m_cells.size() + cell_block_pool);
            }
            m_cells[m_num_blocks++] = std::make_unique<cell_type[]>(cell_block_size);
        }

        m_curr_cell_ptr = m_cells[m_curr_block++].get();
    }

    template<class Cell>
    void rasterizer_cells_aa<Cell>::sort_cells() {
        if (m_sorted) return; // Perform sort only the first time.

        add_curr_cell();
        m_curr_cell.x     = 0x7FFFFFFF;
        m_curr_cell.y     = 0x7FFFFFFF;
        m_curr_cell.cover = 0;
        m_curr_cell.area  = 0;

        if (m_num_cells == 0) return;

        // DBG: Check to see if min/max works well.
        //for(unsigned nc = 0; nc < m_num_cells; nc++)
        //{
        //    cell_type* cell = m_cells[nc >> cell_block_shift] + (nc & cell_block_mask);
        //    if(cell->x < m_min_x ||
        //       cell->y < m_min_y ||
        //       cell->x > m_max_x ||
        //       cell->y > m_max_y)
        //    {
        //        cell = cell; // Breakpoint here
        //    }
        //}

        m_sorted_cells.resize(m_num_cells);

        m_sorted_y.resize(m_max_y - m_min_y + 1);
        memset(m_sorted_y.data(), 0, m_sorted_y.size() * sizeof(sorted_y));

        // Create the Y-histogram (count the numbers of cells for each Y)
        std::unique_ptr<cell_type[]> *block_ptr = m_cells.data();
        cell_type*  cell_ptr;
        unsigned nb = m_num_cells >> cell_block_shift;
        unsigned i;
        while(nb--) {
            cell_ptr = block_ptr->get();
            block_ptr++;
            i = cell_block_size;
            while(i--) {
                m_sorted_y[cell_ptr->y - m_min_y].start++;
                ++cell_ptr;
            }
        }

        cell_ptr = block_ptr->get();
        block_ptr++;
        i = m_num_cells & cell_block_mask;
        while(i--) {
            m_sorted_y[cell_ptr->y - m_min_y].start++;
            ++cell_ptr;
        }

        // Convert the Y-histogram into the array of starting indexes
        unsigned start = 0;
        for (i = 0; i < m_sorted_y.size(); i++) {
            unsigned v = m_sorted_y[i].start;
            m_sorted_y[i].start = start;
            start += v;
        }

        // Copy the cells into the contiguous array sorted by Y, so each scanline's cells can be swept linearly.
        block_ptr = m_cells.data();
        nb = m_num_cells >> cell_block_shift;
        while(nb--) {
            cell_ptr = block_ptr->get();
            block_ptr++;
            i = cell_block_size;
            while(i--) {
                sorted_y& curr_y = m_sorted_y[cell_ptr->y - m_min_y];
                m_sorted_cells[curr_y.start + curr_y.num] = *cell_ptr;
                ++curr_y.num;
                ++cell_ptr;
            }
        }

        cell_ptr = block_ptr->get();
        block_ptr++;
        i = m_num_cells & cell_block_mask;
        while(i--) {
            sorted_y& curr_y = m_sorted_y[cell_ptr->y - m_min_y];
            m_sorted_cells[curr_y.start + curr_y.num] = *cell_ptr;
            ++curr_y.num;
            ++cell_ptr;
        }

        // Finally arrange the X-arrays.  Relative order of equal-x cells is irrelevant: the sweep accumulates
        // their area/cover sums commutatively.
        //
        // Longer scanlines sort packed 64-bit keys (x offset in the high half, source index in the low half)
        // rather than the 16-byte cells themselves: comparisons become branchless integer compares and each
        // swap moves half the bytes, with a single gather pass afterwards.  Short scanlines stay on a direct
        // std::sort, where the key-build and gather overhead would outweigh the saving.
        constexpr unsigned key_sort_threshold = 32;
        for (i=0; i < m_sorted_y.size(); i++) {
            const sorted_y &curr_y = m_sorted_y[i];
            if (curr_y.num < 2) continue;
            cell_type *base = m_sorted_cells.data() + curr_y.start;

            if (curr_y.num < key_sort_threshold) {
                std::sort(base, base + curr_y.num, [](const cell_type &a, const cell_type &b) { return a.x < b.x; });
            }
            else {
                if (m_sort_keys.size() < curr_y.num) {
                    m_sort_keys.resize(curr_y.num);
                    m_cell_scratch.resize(curr_y.num);
                }
                for (unsigned n=0; n < curr_y.num; n++) {
                    m_sort_keys[n] = (uint64_t(uint32_t(base[n].x - m_min_x)) << 32) | n;
                }
                std::sort(m_sort_keys.begin(), m_sort_keys.begin() + curr_y.num);
                for (unsigned n=0; n < curr_y.num; n++) {
                    m_cell_scratch[n] = base[uint32_t(m_sort_keys[n])];
                }
                memcpy(base, m_cell_scratch.data(), curr_y.num * sizeof(cell_type));
            }
        }
        m_sorted = true;
    }

    class scanline_hit_test {
    public:
        scanline_hit_test(int x) : m_x(x), m_hit(false) {}

        void reset_spans() {}
        void finalize(int) {}
        void add_cell(int x, int) {
            if (m_x == x) m_hit = true;
        }

        void add_span(int x, int len, int) {
            if (m_x >= x and m_x < x+len) m_hit = true;
        }
        unsigned num_spans() const { return 1; }
        bool hit() const { return m_hit; }

    private:
        int  m_x;
        bool m_hit;
    };
} // namespace
