// Library function utilities
// Copyright (C) 2025-2026 Paul Manias.

#pragma once

#include "lj_obj.h"
#include "runtime/lj_strscan.h"
#include <optional>

//********************************************************************************************************************
// Conditional copy helper - replaces repetitive if/else copyTV/setnilV patterns
//
// Usage:
//    copy_or_nil(L, dst, src);  // Copies src to dst if src is non-null, otherwise sets dst to nil

[[maybe_unused]] static inline void copy_or_nil(lua_State* L, TValue* dst, const TValue* src) noexcept
{
   if (src) copyTV(L, dst, src);
   else setnilV(dst);
}

//********************************************************************************************************************
// Optional-based number coercion - provides type-safe conversion with clear failure semantics
//
// Usage:
//    TValue tmp;
//    if (auto num = try_to_number(o, &tmp)) {
//       // Use *num
//    } else {
//       // Handle conversion failure
//    }

[[nodiscard, maybe_unused]] static inline std::optional<lua_Number> try_to_number(const TValue* o, TValue* tmp) noexcept
{
   if (tvisnumber(o)) return numberVnum(o);
   if (tvisstr(o) and lj_strscan_num(strV(o), tmp)) return numV(tmp);
   return std::nullopt;
}

//********************************************************************************************************************
// Optional-based integer coercion - handles both direct integers and numeric conversions
//
// Usage:
//    TValue tmp;
//    if (auto i = try_to_integer(o, &tmp)) {
//       // Use *i
//    }

[[nodiscard, maybe_unused]] static inline std::optional<lua_Integer> try_to_integer(const TValue* o, TValue* tmp) noexcept
{
   if (tvisint(o)) return intV(o);
   if (tvisnum(o)) return lua_Integer(numV(o));
   if (tvisstr(o) and lj_strscan_number(strV(o), tmp)) {
      if (tvisint(tmp)) return intV(tmp);
      return lua_Integer(numV(tmp));
   }
   return std::nullopt;
}

//********************************************************************************************************************
// Variadic type checking - check if a TValue matches any of the specified type tags
//
// Usage:
//    if (is_any_type<LJ_TSTR, LJ_TNUMX, LJ_TTAB>(o)) { ... }
//
// Replaces patterns like:
//    if (tvisstr(o) or tvisnumber(o) or tvistab(o)) { ... }

template<uint32_t... Tags>
[[nodiscard]] constexpr bool is_any_type(const TValue* o) noexcept
{
   if constexpr (sizeof...(Tags) IS 0) return false;
   else {
      uint32_t it = itype(o);
      return ((it IS Tags) or ...);
   }
}

//********************************************************************************************************************
// Bulk nil setting - efficiently set a range of TValues to nil
//
// Usage:
//    set_range_nil(array, count);
//
// Replaces loops like:
//    for (i = 0; i < count; i++) setnilV(&array[i]);

[[maybe_unused]] static inline void set_range_nil(TValue* start, size_t count) noexcept
{
   for (size_t i = 0; i < count; ++i) setnilV(&start[i]);
}

//********************************************************************************************************************
// Bulk copy - copy a range of TValues
//
// Usage:
//    copy_range(L, dst, src, count);
//
// Replaces loops like:
//    for (i = 0; i < count; i++) copyTV(L, &dst[i], &src[i]);

[[maybe_unused]] static inline void copy_range(lua_State* L, TValue* dst, const TValue* src, size_t count) noexcept
{
   for (size_t i = 0; i < count; ++i) copyTV(L, &dst[i], &src[i]);
}
