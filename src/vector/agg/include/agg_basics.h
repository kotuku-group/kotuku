//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
// ---
// Defines AGG's fundamental types, path commands, rectangles, coverage constants, and small helpers. Hooks into nearly
// every AGG header as the shared vocabulary for vertex sources, rasterisers, spans, and pixels. In the vector renderer
// it is the common contract between path conversion, scanline generation, and pixel output.

#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <type_traits>
#ifdef __cpp_lib_math_constants
#include <numbers>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef AGG_CUSTOM_ALLOCATOR
#include "agg_allocator.h"
#else
namespace agg
{
    // Modern allocator with backward compatibility
    template<class T> struct pod_allocator
    {
        using pointer_type = std::unique_ptr<T[]>;

        // Modern interface returning smart pointer
        static pointer_type allocate_unique(std::size_t size) {
            return std::make_unique<T[]>(size);
        }

        // Legacy interface for backward compatibility - returns raw pointer
        static T* allocate(std::size_t size) {
            return new T[size];
        }

        static void deallocate(T* ptr, std::size_t = 0) { delete [] ptr; }
    };

    template<class T> struct obj_allocator
    {
        using pointer_type = std::unique_ptr<T>;

        // Modern interface returning smart pointer
        static pointer_type allocate_unique() {
            return std::make_unique<T>();
        }

        template<typename... Args>
        static pointer_type allocate_unique(Args&&... args) {
            return std::make_unique<T>(std::forward<Args>(args)...);
        }

        // Legacy interface for backward compatibility - returns raw pointer
        static T* allocate() { return new T; }
        static void deallocate(T* ptr) { delete ptr; }
    };
}
#endif

#ifndef AGG_INT8
using AGG_INT8 = std::int8_t;
#endif

#ifndef AGG_INT8U
using AGG_INT8U = std::uint8_t;
#endif

#ifndef AGG_INT16
using AGG_INT16 = std::int16_t;
#endif

#ifndef AGG_INT16U
using AGG_INT16U = std::uint16_t;
#endif

#ifndef AGG_INT32
using AGG_INT32 = std::int32_t;
#endif

#ifndef AGG_INT32U
using AGG_INT32U = std::uint32_t;
#endif

#ifndef AGG_INT64
using AGG_INT64 = std::int64_t;
#endif

#ifndef AGG_INT64U
using AGG_INT64U = std::uint64_t;
#endif

//------------------------------------------------ Some fixes for MS Visual C++
#if defined(_MSC_VER)
#pragma warning(disable:4786) // Identifier was truncated...
#endif

#if defined(_MSC_VER)
#define AGG_INLINE __forceinline
#else
#define AGG_INLINE inline
#endif

namespace agg
{
    using int8   = std::int8_t;
    using int8u  = std::uint8_t;
    using int16  = std::int16_t;
    using int16u = std::uint16_t;
    using int32  = std::int32_t;
    using int32u = std::uint32_t;
    using int64  = std::int64_t;
    using int64u = std::uint64_t;

#if defined(AGG_FISTP)
#pragma warning(push)
#pragma warning(disable : 4035) //Disable warning "no return value"
    inline int iround(double v)
    {
        int t;
        __asm fld   qword ptr [v]
        __asm fistp dword ptr [t]
        __asm mov eax, dword ptr [t]
    }
    inline unsigned uround(double v)
    {
        unsigned t;
        __asm fld   qword ptr [v]
        __asm fistp dword ptr [t]
        __asm mov eax, dword ptr [t]
    }
#pragma warning(pop)
    inline unsigned ufloor(double v)
    {
        return unsigned(floor(v));
    }
    inline unsigned uceil(double v)
    {
        return unsigned(ceil(v));
    }
#elif defined(AGG_QIFIST)
    inline int iround(double v)
    {
        return int(v);
    }
    inline int uround(double v)
    {
        return unsigned(v);
    }
    inline unsigned ufloor(double v)
    {
        return unsigned(floor(v));
    }
    inline unsigned uceil(double v)
    {
        return unsigned(ceil(v));
    }
#else
    inline int iround(double v)
    {
        return int((v < 0.0) ? v - 0.5 : v + 0.5);
    }
    inline int uround(double v)
    {
        return unsigned(v + 0.5);
    }
    inline unsigned ufloor(double v)
    {
        return unsigned(v);
    }
    inline unsigned uceil(double v)
    {
        return unsigned(ceil(v));
    }
#endif

    template<int Limit> struct saturation {
        static constexpr int iround(double v) noexcept {
            // Branch-free saturation optimized for modern CPUs
            const int rounded = agg::iround(v);

            // Branchless clamping using conditional moves
            const int mask_low = -(rounded < -Limit);
            const int mask_high = -(rounded > Limit);

            return (rounded & ~mask_low & ~mask_high) |
                   (-Limit & mask_low) |
                   (Limit & mask_high);
        }

        // Compile-time version for constant values
        static consteval int iround_ct(double v) noexcept {
            const int rounded = static_cast<int>(v < 0.0 ? v - 0.5 : v + 0.5);
            return rounded < -Limit ? -Limit : (rounded > Limit ? Limit : rounded);
        }
    };

    template<unsigned Shift> struct mul_one {
        static constexpr unsigned mul(unsigned a, unsigned b) noexcept {
            // Optimized fixed-point multiplication with better instruction scheduling
            const unsigned product = a * b;
            const unsigned rounded = product + (1u << (Shift - 1));
            return (rounded + (rounded >> Shift)) >> Shift;
        }

        // Compile-time version for constant multiplication
        static consteval unsigned mul_ct(unsigned a, unsigned b) noexcept {
            const unsigned product = a * b;
            const unsigned rounded = product + (1u << (Shift - 1));
            return (rounded + (rounded >> Shift)) >> Shift;
        }
    };

    using cover_type = std::uint8_t;

    constexpr int cover_shift = 8;
    constexpr int cover_size  = 1 << cover_shift;
    constexpr int cover_mask  = cover_size - 1;
    constexpr int cover_none  = 0;
    constexpr int cover_full  = cover_mask;

    // These constants determine the subpixel accuracy, to be more precise,
    // the number of bits of the fractional part of the coordinates.
    // The possible coordinate capacity in bits can be calculated by formula:
    // sizeof(int) * 8 - poly_subpixel_shift, i.e, for 32-bit integers and
    // 8-bits fractional part the capacity is 24 bits.

    constexpr int poly_subpixel_shift = 8;
    constexpr int poly_subpixel_scale = 1<<poly_subpixel_shift;
    constexpr int poly_subpixel_mask  = poly_subpixel_scale-1;

    enum filling_rule_e { fill_non_zero, fill_even_odd };

#ifdef __cpp_lib_math_constants
    constexpr double pi = std::numbers::pi;
#else
    constexpr double pi = 3.14159265358979323846;
#endif

    // Compile-time angle conversion functions
    consteval double deg2rad_ct(double deg) noexcept { return deg * pi / 180.0; }
    consteval double rad2deg_ct(double rad) noexcept { return rad * 180.0 / pi; }

    // Runtime angle conversion functions
    constexpr double deg2rad(double deg) noexcept { return deg * pi / 180.0; }
    constexpr double rad2deg(double rad) noexcept { return rad * 180.0 / pi; }

    template<typename T>
    requires std::is_arithmetic_v<T>
    struct alignas(32) rect_base {
        using value_type = T;
        using self_type = rect_base<T>;
        T x1, y1, x2, y2;

        rect_base() {}
        rect_base(T x1_, T y1_, T x2_, T y2_) : x1(x1_), y1(y1_), x2(x2_), y2(y2_) {}

        void init(T x1_, T y1_, T x2_, T y2_) {
            x1 = x1_; y1 = y1_; x2 = x2_; y2 = y2_;
        }

        constexpr self_type& normalize() noexcept {
            if (x1 > x2) std::swap(x1, x2);
            if (y1 > y2) std::swap(y1, y2);
            return *this;
        }

        constexpr bool clip(const self_type& r) noexcept {
            if (x2 > r.x2) x2 = r.x2;
            if (y2 > r.y2) y2 = r.y2;
            if (x1 < r.x1) x1 = r.x1;
            if (y1 < r.y1) y1 = r.y1;
            return x1 <= x2 and y1 <= y2;
        }

        constexpr bool is_valid() const noexcept { return x1 <= x2 and y1 <= y2; }

        constexpr bool hit_test(T x, T y) const noexcept {
            return (x >= x1 and x <= x2 and y >= y1 and y <= y2);
        }
    };

    // Consolidated rectangle operations - reduced code duplication
    template<typename Rect, bool Unite>
    constexpr auto combine_rectangles(const Rect& r1, const Rect& r2) noexcept -> Rect {
        Rect r = r1;
        if constexpr (Unite) {
            // Unite: expand bounds
            if (r.x2 < r2.x2) r.x2 = r2.x2;
            if (r.y2 < r2.y2) r.y2 = r2.y2;
            if (r.x1 > r2.x1) r.x1 = r2.x1;
            if (r.y1 > r2.y1) r.y1 = r2.y1;
        } else {
            // Intersect: shrink bounds
            if (r.x2 > r2.x2) r.x2 = r2.x2;
            if (r.y2 > r2.y2) r.y2 = r2.y2;
            if (r.x1 < r2.x1) r.x1 = r2.x1;
            if (r.y1 < r2.y1) r.y1 = r2.y1;
        }
        return r;
    }

    template<typename Rect>
    constexpr auto intersect_rectangles(const Rect& r1, const Rect& r2) noexcept -> Rect {
        return combine_rectangles<Rect, false>(r1, r2);
    }

    template<typename Rect>
    constexpr auto unite_rectangles(const Rect& r1, const Rect& r2) noexcept -> Rect {
        return combine_rectangles<Rect, true>(r1, r2);
    }

    using rect_i = rect_base<int>;
    using rect_f = rect_base<float>;
    using rect_d = rect_base<double>;

    constexpr int path_cmd_stop     = 0;
    constexpr int path_cmd_move_to  = 1;
    constexpr int path_cmd_line_to  = 2;
    constexpr int path_cmd_curve3   = 3;
    constexpr int path_cmd_curve4   = 4;
    constexpr int path_cmd_end_poly = 0x0F;
    constexpr int path_cmd_mask     = 0x0F;

    constexpr int path_flags_none  = 0;
    constexpr int path_flags_ccw   = 0x10;
    constexpr int path_flags_cw    = 0x20;
    constexpr int path_flags_close = 0x40;
    constexpr int path_flags_mask  = 0xF0;

    constexpr bool is_vertex(unsigned c) noexcept    { return c >= path_cmd_move_to and c < path_cmd_end_poly; }
    constexpr bool is_drawing(unsigned c) noexcept   { return c >= path_cmd_line_to and c < path_cmd_end_poly; }
    constexpr bool is_stop(unsigned c) noexcept      { return c == path_cmd_stop; }
    constexpr bool is_move_to(unsigned c) noexcept   { return c == path_cmd_move_to; }
    constexpr bool is_line_to(unsigned c) noexcept   { return c == path_cmd_line_to; }
    constexpr bool is_curve(unsigned c) noexcept     { return c == path_cmd_curve3 or c == path_cmd_curve4; }
    constexpr bool is_curve3(unsigned c) noexcept    { return c == path_cmd_curve3; }
    constexpr bool is_curve4(unsigned c) noexcept    { return c == path_cmd_curve4; }
    constexpr bool is_end_poly(unsigned c) noexcept  { return (c & path_cmd_mask) == path_cmd_end_poly; }
    constexpr bool is_close(unsigned c) noexcept     { return (c & ~(path_flags_cw | path_flags_ccw)) == (path_cmd_end_poly | path_flags_close); }
    constexpr bool is_next_poly(unsigned c) noexcept { return is_stop(c) or is_move_to(c) or is_end_poly(c); }
    constexpr bool is_cw(unsigned c) noexcept        { return (c & path_flags_cw) != 0; }
    constexpr bool is_ccw(unsigned c) noexcept       { return (c & path_flags_ccw) != 0; }
    constexpr bool is_oriented(unsigned c) noexcept  { return (c & (path_flags_cw | path_flags_ccw)) != 0; }
    constexpr bool is_closed(unsigned c) noexcept    { return (c & path_flags_close) != 0; }
    constexpr unsigned get_close_flag(unsigned c) noexcept    { return c & path_flags_close; }
    constexpr unsigned clear_orientation(unsigned c) noexcept { return c & ~(path_flags_cw | path_flags_ccw); }
    constexpr unsigned get_orientation(unsigned c) noexcept   { return c & (path_flags_cw | path_flags_ccw); }
    constexpr unsigned set_orientation(unsigned c, unsigned o) noexcept { return clear_orientation(c) | o; }

    template<typename T>
    requires std::is_arithmetic_v<T>
    struct alignas(16) point_base {
        using value_type = T;
        T x, y;
        point_base() {}
        point_base(T x_, T y_) : x(x_), y(y_) {}
    };

    using point_i = point_base<int>;
    using point_f = point_base<float>;
    using point_d = point_base<double>;

    template<typename T>
    requires std::is_arithmetic_v<T>
    struct alignas(16) vertex_base {
        using value_type = T;
        T x, y;
        unsigned cmd;
        vertex_base() {}
        vertex_base(T x_, T y_, unsigned cmd_ = 0) : x(x_), y(y_), cmd(cmd_) {}
    };

    using vertex_i = vertex_base<int>;
    using vertex_f = vertex_base<float>;
    using vertex_d = vertex_base<double>;

    template<typename T> struct row_info {
        int x1, x2;
        T* ptr;
        row_info() {}
        row_info(int x1_, int x2_, T* ptr_) : x1(x1_), x2(x2_), ptr(ptr_) {}
    };

    template<typename T> struct const_row_info {
        int x1, x2;
        const T* ptr;
        const_row_info() {}
        const_row_info(int x1_, int x2_, const T* ptr_) :
            x1(x1_), x2(x2_), ptr(ptr_) {}
    };

    template<typename T>
    requires std::is_floating_point_v<T>
    constexpr bool is_equal_eps(T v1, T v2, T epsilon) noexcept {
        return std::abs(v1 - v2) <= static_cast<T>(epsilon);
    }
}
