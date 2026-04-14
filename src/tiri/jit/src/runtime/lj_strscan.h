// String scanning.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_obj.h"

// Options for accepted/returned formats.
enum class StrScanOpt : uint32_t {
   None   = 0x00,
   ToInt  = 0x01,  // Convert to int32_t, if possible.
   ToNum  = 0x02,  // Always convert to double.
   Imag   = 0x04,
   LL     = 0x08,
   C      = 0x10
};

// Flag operators for StrScanOpt
[[nodiscard]] static constexpr StrScanOpt operator|(StrScanOpt Left, StrScanOpt Right) noexcept {
   return StrScanOpt(uint32_t(Left) | uint32_t(Right));
}

[[nodiscard]] static constexpr StrScanOpt operator&(StrScanOpt Left, StrScanOpt Right) noexcept {
   return StrScanOpt(uint32_t(Left) & uint32_t(Right));
}

[[nodiscard]] static constexpr bool has_opt(StrScanOpt Opts, StrScanOpt Mask) noexcept {
   return (uint32_t(Opts) & uint32_t(Mask)) != 0;
}

// Returned format.
enum class StrScanFmt : uint8_t {
   Error,
   Num, Imag,
   Int, U32, I64, U64
};

// Comparison helpers for StrScanFmt range checks
[[nodiscard]] static constexpr bool operator==(StrScanFmt Left, StrScanFmt Right) noexcept {
   return uint8_t(Left) == uint8_t(Right);
}

[[nodiscard]] static constexpr bool operator!=(StrScanFmt Left, StrScanFmt Right) noexcept {
   return uint8_t(Left) != uint8_t(Right);
}

[[nodiscard]] static constexpr bool operator>=(StrScanFmt Left, StrScanFmt Right) noexcept {
   return uint8_t(Left) >= uint8_t(Right);
}

[[nodiscard]] static constexpr bool operator>(StrScanFmt Left, StrScanFmt Right) noexcept {
   return uint8_t(Left) > uint8_t(Right);
}

// Arithmetic operators for StrScanFmt (used for format conversions)
[[nodiscard]] static constexpr StrScanFmt operator+(StrScanFmt Left, StrScanFmt Right) noexcept {
   return StrScanFmt(uint8_t(Left) + uint8_t(Right));
}

[[nodiscard]] static constexpr StrScanFmt operator-(StrScanFmt Left, StrScanFmt Right) noexcept {
   return StrScanFmt(uint8_t(Left) - uint8_t(Right));
}

// Backward compatibility macros (to be removed once all callers updated)
#define STRSCAN_OPT_TOINT  uint32_t(StrScanOpt::ToInt)
#define STRSCAN_OPT_TONUM  uint32_t(StrScanOpt::ToNum)
#define STRSCAN_OPT_IMAG   uint32_t(StrScanOpt::Imag)
#define STRSCAN_OPT_LL     uint32_t(StrScanOpt::LL)
#define STRSCAN_OPT_C      uint32_t(StrScanOpt::C)

#define STRSCAN_ERROR  StrScanFmt::Error
#define STRSCAN_NUM    StrScanFmt::Num
#define STRSCAN_IMAG   StrScanFmt::Imag
#define STRSCAN_INT    StrScanFmt::Int
#define STRSCAN_U32    StrScanFmt::U32
#define STRSCAN_I64    StrScanFmt::I64
#define STRSCAN_U64    StrScanFmt::U64

LJ_FUNC StrScanFmt lj_strscan_scan(const uint8_t *p, MSize len, TValue *o, uint32_t opt);
LJ_FUNC int lj_strscan_num(GCstr *str, TValue *o);
#if LJ_DUALNUM
LJ_FUNC int lj_strscan_number(GCstr *str, TValue *o);
#else
#define lj_strscan_number(s, o)      lj_strscan_num((s), (o))
#endif

// Check for number or convert string to number/int in-place (!).
static LJ_AINLINE int lj_strscan_numberobj(TValue *o)
{
  return tvisnumber(o) or (tvisstr(o) and lj_strscan_number(strV(o), o));
}
