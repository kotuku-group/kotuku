// String scanning.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#include <kotuku/main.h>
#include <cctype>
#include <cmath>
#include <charconv>

#define lj_strscan_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_char.h"
#include "lj_strscan.h"

// Definitions for circular decimal digit buffer (base 100 = 2 digits/byte).
static constexpr int STRSCAN_DIG    = 1024;
static constexpr int STRSCAN_MAXDIG = 800;      //  772 + extra are sufficient.
static constexpr int STRSCAN_DDIG   = STRSCAN_DIG / 2;
static constexpr int STRSCAN_DMASK  = STRSCAN_DDIG - 1;
static constexpr int STRSCAN_MAXEXP = 1 << 20;

// Helpers for circular buffer.
[[nodiscard]] static constexpr uint32_t DNEXT(uint32_t a) noexcept {
   return (a + 1) & STRSCAN_DMASK;
}

[[nodiscard]] static constexpr uint32_t DPREV(uint32_t a) noexcept {
   return (a - 1) & STRSCAN_DMASK;
}

[[nodiscard]] static constexpr int32_t DLEN(uint32_t lo, uint32_t hi) noexcept {
   return int32_t((lo - hi) & STRSCAN_DMASK);
}

[[nodiscard]] static constexpr bool casecmp(int c, int k) noexcept {
   return (c | 0x20) IS k;
}

//********************************************************************************************************************
// Final conversion to double.

static void strscan_double(uint64_t x, TValue* o, int32_t ex2, int32_t neg)
{
   double n;

   // Avoid double rounding for denormals.
   if (LJ_UNLIKELY(ex2 <= -1075 and x != 0)) {
      // NYI: all of this generates way too much code on 32 bit CPUs.
#if (defined(__GNUC__) or defined(__clang__)) and LJ_64
      int32_t b = (int32_t)(__builtin_clzll(x) ^ 63);
#else
      int32_t b = (x >> 32) ? 32 + (int32_t)lj_fls((uint32_t)(x >> 32)) :
         (int32_t)lj_fls((uint32_t)x);
#endif
      if ((int32_t)b + ex2 <= -1023 and (int32_t)b + ex2 >= -1075) {
         uint64_t rb = (uint64_t)1 << (-1075 - ex2);
         if ((x & rb) and ((x & (rb + rb + rb - 1)))) x += rb + rb;
         x = (x & ~(rb + rb - 1));
      }
   }

   // Convert to double using a signed int64_t conversion, then rescale.
   lj_assertX((int64_t)x >= 0, "bad double conversion");
   n = (double)(int64_t)x;
   if (neg) n = -n;
   if (ex2) n = ldexp(n, ex2);
   o->n = n;
}

//********************************************************************************************************************
// Parse hexadecimal number.

[[nodiscard]] static StrScanFmt strscan_hex(const uint8_t* p, TValue* o,
   StrScanFmt fmt, uint32_t opt, int32_t ex2, int32_t neg, uint32_t dig)
{
   uint64_t x = 0;

   // Fast path: use std::from_chars for integer formats with ≤16 digits and no exponent
   if (dig <= 16 and ex2 IS 0 and fmt != STRSCAN_NUM) {
      auto [ptr, ec] = std::from_chars(
         CSTRING(p),
         CSTRING(p + dig),
         x, 16);

      if (ec IS std::errc{}) {
         // Format-specific handling.
         switch (fmt) {
         case STRSCAN_INT:
            if (!(opt & STRSCAN_OPT_TONUM) and x < 0x80000000u + neg and
               !(x IS 0 and neg)) {
               o->i = neg ? -(int32_t)x : (int32_t)x;
               return STRSCAN_INT;  //  Fast path for 32 bit integers.
            }
            if (!(opt & STRSCAN_OPT_C)) { fmt = STRSCAN_NUM; break; }
            // fallthrough
         case STRSCAN_U32:
            if (dig > 8) return STRSCAN_ERROR;
            o->i = neg ? -(int32_t)x : (int32_t)x;
            return STRSCAN_U32;
         case STRSCAN_I64:
         case STRSCAN_U64:
            o->u64 = neg ? (uint64_t)-(int64_t)x : x;
            return fmt;
         default:
            break;
         }
      }
   }

   // Fallback: custom implementation for floats, excess digits, and edge cases.
   x = 0;
   uint32_t i;

   // Scan hex digits.
   for (i = dig > 16 ? 16 : dig; i; i--, p++) {
      uint32_t d = (*p != '.' ? *p : *++p); if (d > '9') d += 9;
      x = (x << 4) + (d & 15);
   }

   // Summarize rounding-effect of excess digits.
   for (i = 16; i < dig; i++, p++)
      x |= ((*p != '.' ? *p : *++p) != '0'), ex2 += 4;

   // Format-specific handling.
   switch (fmt) {
      case STRSCAN_INT:
         if (!(opt & STRSCAN_OPT_TONUM) and x < 0x80000000u + neg &&
            !(x IS 0 and neg)) {
            o->i = neg ? -(int32_t)x : (int32_t)x;
            return STRSCAN_INT;  //  Fast path for 32 bit integers.
         }
         if (!(opt & STRSCAN_OPT_C)) { fmt = STRSCAN_NUM; break; }
         // fallthrough
      case STRSCAN_U32:
         if (dig > 8) return STRSCAN_ERROR;
         o->i = neg ? -(int32_t)x : (int32_t)x;
         return STRSCAN_U32;
      case STRSCAN_I64:
      case STRSCAN_U64:
         if (dig > 16) return STRSCAN_ERROR;
         o->u64 = neg ? (uint64_t)-(int64_t)x : x;
         return fmt;
      default:
         break;
   }

   // Reduce range, then convert to double.
   if ((x & U64x(c0000000, 0000000))) { x = (x >> 2) | (x & 3); ex2 += 2; }
   strscan_double(x, o, ex2, neg);
   return fmt;
}

//********************************************************************************************************************
// Parse octal number.

[[nodiscard]] static StrScanFmt strscan_oct(const uint8_t* p, TValue* o, StrScanFmt fmt, int32_t neg, uint32_t dig)
{
   uint64_t x = 0;

   // Validate maximum octal digits (22 digits = 64 bits).
   if (dig > 22 or (dig IS 22 and *p > '1')) return STRSCAN_ERROR;

   // Use std::from_chars for octal parsing (C++17).
   auto [ptr, ec] = std::from_chars(
      CSTRING(p),
      CSTRING(p + dig),
      x, 8);

   if (ec != std::errc{}) return STRSCAN_ERROR;

   // Format-specific handling.
   switch (fmt) {
      case STRSCAN_INT:
         if (x >= 0x80000000u + neg) fmt = STRSCAN_U32;
         // fallthrough
      case STRSCAN_U32:
         if ((x >> 32)) return STRSCAN_ERROR;
         o->i = neg ? -(int32_t)x : (int32_t)x;
         break;
      default:
      case STRSCAN_I64:
      case STRSCAN_U64:
         o->u64 = neg ? (uint64_t)-(int64_t)x : x;
         break;
   }
   return fmt;
}

//********************************************************************************************************************
// Parse decimal number.

[[nodiscard]] static StrScanFmt strscan_dec(const uint8_t* p, TValue* o, StrScanFmt fmt, uint32_t opt, int32_t ex10, int32_t neg, uint32_t dig)
{
   uint8_t xi[STRSCAN_DDIG], * xip = xi;

   if (dig) {
      uint32_t i = dig;
      if (i > STRSCAN_MAXDIG) {
         ex10 += (int32_t)(i - STRSCAN_MAXDIG);
         i = STRSCAN_MAXDIG;
      }
      // Scan unaligned leading digit.
      if (((ex10 ^ i) & 1))
         *xip++ = ((*p != '.' ? *p : *++p) & 15), i--, p++;
      // Scan aligned double-digits.
      for (; i > 1; i -= 2) {
         uint32_t d = 10 * ((*p != '.' ? *p : *++p) & 15); p++;
         *xip++ = d + ((*p != '.' ? *p : *++p) & 15); p++;
      }
      // Scan and realign trailing digit.
      if (i) *xip++ = 10 * ((*p != '.' ? *p : *++p) & 15), ex10--, dig++, p++;

      // Summarize rounding-effect of excess digits.
      if (dig > STRSCAN_MAXDIG) {
         do {
            if ((*p != '.' ? *p : *++p) != '0') { xip[-1] |= 1; break; }
            p++;
         } while (--dig > STRSCAN_MAXDIG);
         dig = STRSCAN_MAXDIG;
      }
      else {  // Simplify exponent.
         while (ex10 > 0 and dig <= 18) *xip++ = 0, ex10 -= 2, dig += 2;
      }
   }
   else {  // Only got zeros.
      ex10 = 0;
      xi[0] = 0;
   }

   // Fast path for numbers in integer format (but handles e.g. 1e6, too).
   if (dig <= 20 and ex10 IS 0) {
      uint8_t* xis;
      uint64_t x = xi[0];
      double n;
      for (xis = xi + 1; xis < xip; xis++) x = x * 100 + *xis;
      if (!(dig IS 20 and (xi[0] > 18 or (int64_t)x >= 0))) {  // No overflow?
         // Format-specific handling.
         switch (fmt) {
         case STRSCAN_INT:
            if (!(opt & STRSCAN_OPT_TONUM) and x < 0x80000000u + neg) {
               o->i = neg ? -(int32_t)x : (int32_t)x;
               return STRSCAN_INT;  //  Fast path for 32 bit integers.
            }
            if (!(opt & STRSCAN_OPT_C)) { fmt = STRSCAN_NUM; goto plainnumber; }
            // fallthrough
         case STRSCAN_U32:
            if ((x >> 32) != 0) return STRSCAN_ERROR;
            o->i = neg ? -(int32_t)x : (int32_t)x;
            return STRSCAN_U32;
         case STRSCAN_I64:
         case STRSCAN_U64:
            o->u64 = neg ? (uint64_t)-(int64_t)x : x;
            return fmt;
         default:
         plainnumber:  //  Fast path for plain numbers < 2^63.
            if ((int64_t)x < 0) break;
            n = (double)(int64_t)x;
            if (neg) n = -n;
            o->n = n;
            return fmt;
         }
      }
   }

   // Slow non-integer path.
   if (fmt IS STRSCAN_INT) {
      if ((opt & STRSCAN_OPT_C)) return STRSCAN_ERROR;
      fmt = STRSCAN_NUM;
   }
   else if (fmt > STRSCAN_INT) {
      return STRSCAN_ERROR;
   }
   {
      uint32_t hi = 0, lo = (uint32_t)(xip - xi);
      int32_t ex2 = 0, idig = (int32_t)lo + (ex10 >> 1);

      lj_assertX(lo > 0 and (ex10 & 1) IS 0, "bad lo %d ex10 %d", lo, ex10);

      // Handle simple overflow/underflow.
      if (idig > 310 / 2) { if (neg) setminfV(o); else setpinfV(o); return fmt; }
      else if (idig < -326 / 2) { o->n = neg ? -0.0 : 0.0; return fmt; }

      // Scale up until we have at least 17 or 18 integer part digits.
      while (idig < 9 and idig < DLEN(lo, hi)) {
         uint32_t i, cy = 0;
         ex2 -= 6;
         for (i = DPREV(lo); ; i = DPREV(i)) {
            uint32_t d = (xi[i] << 6) + cy;
            cy = (((d >> 2) * 5243) >> 17); d = d - cy * 100;  //  Div/mod 100.
            xi[i] = (uint8_t)d;
            if (i IS hi) break;
            if (d IS 0 and i IS DPREV(lo)) lo = i;
         }
         if (cy) {
            hi = DPREV(hi);
            if (xi[DPREV(lo)] IS 0) lo = DPREV(lo);
            else if (hi IS lo) { lo = DPREV(lo); xi[DPREV(lo)] |= xi[lo]; }
            xi[hi] = (uint8_t)cy; idig++;
         }
      }

      // Scale down until no more than 17 or 18 integer part digits remain.
      while (idig > 9) {
         uint32_t i = hi, cy = 0;
         ex2 += 6;
         do {
            cy += xi[i];
            xi[i] = (cy >> 6);
            cy = 100 * (cy & 0x3f);
            if (xi[i] IS 0 and i IS hi) hi = DNEXT(hi), idig--;
            i = DNEXT(i);
         } while (i != lo);
         while (cy) {
            if (hi IS lo) { xi[DPREV(lo)] |= 1; break; }
            xi[lo] = (cy >> 6); lo = DNEXT(lo);
            cy = 100 * (cy & 0x3f);
         }
      }

      // Collect integer part digits and convert to rescaled double.
      {
         uint64_t x = xi[hi];
         uint32_t i;
         for (i = DNEXT(hi); --idig > 0 and i != lo; i = DNEXT(i))
            x = x * 100 + xi[i];
         if (i IS lo) {
            while (--idig >= 0) x = x * 100;
         }
         else {  // Gather round bit from remaining digits.
            x <<= 1; ex2--;
            do {
               if (xi[i]) { x |= 1; break; }
               i = DNEXT(i);
            } while (i != lo);
         }
         strscan_double(x, o, ex2, neg);
      }
   }
   return fmt;
}

//********************************************************************************************************************
// Parse binary number for lj_strscan_scan()

[[nodiscard]] static StrScanFmt strscan_bin(const uint8_t* p, TValue* o, StrScanFmt fmt, uint32_t opt, int32_t ex2, int32_t neg, uint32_t dig)
{
   uint64_t x = 0;

   if (ex2 or dig > 64) return STRSCAN_ERROR;

   // Use std::from_chars for binary parsing
   auto [ptr, ec] = std::from_chars(CSTRING(p), CSTRING(p + dig), x, 2);

   if (ec != std::errc{} or ptr != CSTRING(p + dig)) return STRSCAN_ERROR;

   // Format-specific handling.
   switch (fmt) {
      case STRSCAN_INT:
         if (!(opt & STRSCAN_OPT_TONUM) and x < 0x80000000u + neg) {
            o->i = neg ? -(int32_t)x : (int32_t)x;
            return STRSCAN_INT;  //  Fast path for 32 bit integers.
         }
         if (!(opt & STRSCAN_OPT_C)) { fmt = STRSCAN_NUM; break; }
         // fallthrough
      case STRSCAN_U32:
         if (dig > 32) return STRSCAN_ERROR;
         o->i = neg ? -(int32_t)x : (int32_t)x;
         return STRSCAN_U32;
      case STRSCAN_I64:
      case STRSCAN_U64:
         o->u64 = neg ? (uint64_t)-(int64_t)x : x;
         return fmt;
      default:
         break;
   }

   // Reduce range, then convert to double.
   if ((x & U64x(c0000000, 0000000))) { x = (x >> 2) | (x & 3); ex2 += 2; }
   strscan_double(x, o, ex2, neg);
   return fmt;
}

//********************************************************************************************************************
// Scan string containing a number. Returns format. Returns value in o.  Used directly by the lexer and indirectly
// via lj_strscan_num()

StrScanFmt lj_strscan_scan(const uint8_t* p, MSize len, TValue* o, uint32_t opt)
{
   int32_t neg = 0;
   const uint8_t* pe = p + len;

   // Remove leading space, parse sign and non-numbers.
   if (LJ_UNLIKELY(!std::isdigit(*p))) {
      while (std::isspace(*p)) p++;
      if (*p IS '+' or *p IS '-') neg = (*p++ IS '-');
      if (LJ_UNLIKELY(*p >= 'A')) {  // Parse "inf", "infinity" or "nan".
         TValue tmp;
         setnanV(&tmp);
         if (casecmp(p[0], 'i') and casecmp(p[1], 'n') and casecmp(p[2], 'f')) {
            if (neg) setminfV(&tmp); else setpinfV(&tmp);
            p += 3;
            if (casecmp(p[0], 'i') and casecmp(p[1], 'n') and casecmp(p[2], 'i') and
               casecmp(p[3], 't') and casecmp(p[4], 'y')) p += 5;
         }
         else if (casecmp(p[0], 'n') and casecmp(p[1], 'a') and casecmp(p[2], 'n')) {
            p += 3;
         }
         while (std::isspace(*p)) p++;
         if (*p or p < pe) return STRSCAN_ERROR;
         o->u64 = tmp.u64;
         return STRSCAN_NUM;
      }
   }

   // Parse regular number.
   {
      StrScanFmt fmt = STRSCAN_INT;
      int cmask = LJ_CHAR_DIGIT;
      int base = (opt & STRSCAN_OPT_C) and *p IS '0' ? 0 : 10;
      const uint8_t* sp, * dp = nullptr;
      uint32_t dig = 0, hasdig = 0, x = 0;
      int32_t ex = 0;

      // Determine base and skip leading zeros.
      if (*p <= '0') [[unlikely]] {
         if (*p IS '0') [[likely]] {
            if (casecmp(p[1], 'x')) base = 16, cmask = LJ_CHAR_XDIGIT, p += 2;
            else if (casecmp(p[1], 'b')) base = 2, cmask = LJ_CHAR_DIGIT, p += 2;
         }
         for (; ; p++) {
            if (*p IS '0') hasdig = 1;
            else if (*p IS '.') {
               if (dp) return STRSCAN_ERROR;
               dp = p;
            }
            else break;
         }
      }

      // Preliminary digit and decimal point scan.
      for (sp = p; ; p++) {
         if (LJ_LIKELY(lj_char_isa(*p, cmask))) {
            x = x * 10 + (*p & 15);  //  For fast path below.
            dig++;
         }
         else if (*p IS '.') {
            if (dp) return STRSCAN_ERROR;
            dp = p;
         }
         else break;
      }
      if (!(hasdig | dig)) return STRSCAN_ERROR;

      // Handle decimal point.

      if (dp) {
         if (base IS 2) return STRSCAN_ERROR;
         fmt = STRSCAN_NUM;
         if (dig) {
            ex = (int32_t)(dp - (p - 1)); dp = p - 1;
            while (ex < 0 and *dp-- IS '0') ex++, dig--;  //  Skip trailing zeros.
            if (ex <= -STRSCAN_MAXEXP) return STRSCAN_ERROR;
            if (base IS 16) ex *= 4;
         }
      }

      // Parse exponent.
      if (base >= 10 and casecmp(*p, (uint32_t)(base IS 16 ? 'p' : 'e'))) {
         uint32_t xx;
         int negx = 0;
         fmt = STRSCAN_NUM; p++;
         if (*p IS '+' or *p IS '-') negx = (*p++ IS '-');
         if (!std::isdigit(*p)) return STRSCAN_ERROR;
         xx = (*p++ & 15);
         while (std::isdigit(*p)) {
            xx = xx * 10 + (*p & 15);
            if (xx >= STRSCAN_MAXEXP) return STRSCAN_ERROR;
            p++;
         }
         ex += negx ? -(int32_t)xx : (int32_t)xx;
      }

      // Parse suffix.
      if (*p) {
         // I (IMAG), U (U32), LL (I64), ULL/LLU (U64), L (long), UL/LU (ulong).
         // NYI: f (float). Not needed until cp_number() handles non-integers.
         if (casecmp(*p, 'i')) {
            if (!(opt & STRSCAN_OPT_IMAG)) return STRSCAN_ERROR;
            p++; fmt = STRSCAN_IMAG;
         }
         else if (fmt IS STRSCAN_INT) {
            if (casecmp(*p, 'u')) p++, fmt = STRSCAN_U32;
            if (casecmp(*p, 'l')) {
               p++;
               if (casecmp(*p, 'l')) p++, fmt = (StrScanFmt)(fmt + STRSCAN_I64 - STRSCAN_INT);
               else if (!(opt & STRSCAN_OPT_C)) return STRSCAN_ERROR;
               else if (sizeof(long) IS 8) fmt = (StrScanFmt)(fmt + STRSCAN_I64 - STRSCAN_INT);
            }
            if (casecmp(*p, 'u') and (fmt IS STRSCAN_INT or fmt IS STRSCAN_I64))
               p++, fmt = (StrScanFmt)(fmt + STRSCAN_U32 - STRSCAN_INT);
            if ((fmt IS STRSCAN_U32 and !(opt & STRSCAN_OPT_C)) or
               (fmt >= STRSCAN_I64 and !(opt & STRSCAN_OPT_LL)))
               return STRSCAN_ERROR;
         }
         while (std::isspace(*p)) p++;
         if (*p) return STRSCAN_ERROR;
      }
      if (p < pe) return STRSCAN_ERROR;

      // Fast path for decimal 32 bit integers.
      if (fmt IS STRSCAN_INT and base IS 10 &&
         (dig < 10 or (dig IS 10 and *sp <= '2' and x < 0x80000000u + neg))) {
         if ((opt & STRSCAN_OPT_TONUM)) {
            o->n = neg ? -(double)x : (double)x;
            return STRSCAN_NUM;
         }
         else if (x IS 0 and neg) {
            o->n = -0.0;
            return STRSCAN_NUM;
         }
         else {
            o->i = neg ? -(int32_t)x : (int32_t)x;
            return STRSCAN_INT;
         }
      }

      // Dispatch to base-specific parser.
      if (base IS 0 and !(fmt IS STRSCAN_NUM or fmt IS STRSCAN_IMAG)) return strscan_oct(sp, o, fmt, neg, dig);

      if (base IS 16) fmt = strscan_hex(sp, o, fmt, opt, ex, neg, dig);
      else if (base IS 2) fmt = strscan_bin(sp, o, fmt, opt, ex, neg, dig);
      else fmt = strscan_dec(sp, o, fmt, opt, ex, neg, dig);

      // Try to convert number to integer, if requested.
      if (fmt IS STRSCAN_NUM and (opt & STRSCAN_OPT_TOINT) and !tvismzero(o)) {
         double n = o->n;
         int32_t i = lj_num2int(n);
         if (n IS (lua_Number)i) { o->i = i; return STRSCAN_INT; }
      }
      return fmt;
   }
}

int lj_strscan_num(GCstr* str, TValue* o)
{
   StrScanFmt fmt = lj_strscan_scan((const uint8_t*)strdata(str), str->len, o, STRSCAN_OPT_TONUM);
   lj_assertX(fmt IS STRSCAN_ERROR or fmt IS STRSCAN_NUM, "bad scan format");
   return (fmt != STRSCAN_ERROR);
}

#if LJ_DUALNUM
int lj_strscan_number(GCstr* str, TValue* o)
{
   StrScanFmt fmt = lj_strscan_scan((const uint8_t*)strdata(str), str->len, o, STRSCAN_OPT_TOINT);
   lj_assertX(fmt IS STRSCAN_ERROR or fmt IS STRSCAN_NUM or fmt IS STRSCAN_INT, "bad scan format");
   if (fmt IS STRSCAN_INT) setitype(o, LJ_TISNUM);
   return (fmt != STRSCAN_ERROR);
}
#endif
