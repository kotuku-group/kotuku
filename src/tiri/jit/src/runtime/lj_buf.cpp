// Buffer handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_buf_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_strfmt.h"

static void buf_grow(SBuf* sb, MSize sz)
{
   MSize osz = sbufsz(sb), len = sbuflen(sb), nsz = osz;
   char* b;
   GCSize flag;

   if (nsz < LJ_MIN_SBUF) nsz = LJ_MIN_SBUF;
   while (nsz < sz) nsz += nsz;
   flag = sbufflag(sb);

   if ((flag & SBUF_FLAG_COW)) {  // Copy-on-write semantics.
      lj_assertG_(G(sbufL(sb)), sb->w == sb->e, "bad SBuf COW");
      b = (char*)lj_mem_new(sbufL(sb), nsz);
      setsbufflag(sb, flag & ~(GCSize)SBUF_FLAG_COW);
      setgcrefnull(sbufX(sb)->cowref);
      memcpy(b, sb->b, osz);
   }
   else {
      b = (char*)lj_mem_realloc(sbufL(sb), sb->b, osz, nsz);
   }

   if ((flag & SBUF_FLAG_EXT)) {
      sbufX(sb)->r = sbufX(sb)->r - sb->b + b;  //  Adjust read pointer, too.
   }

   // Adjust buffer pointers.

   sb->b = b;
   sb->w = b + len;
   sb->e = b + nsz;
   if ((flag & SBUF_FLAG_BORROW)) {  // Adjust borrowed buffer pointers.
      SBuf* bsb = mref<SBuf>(sbufX(sb)->bsb);
      bsb->b = b;
      bsb->w = b + len;
      bsb->e = b + nsz;
   }
}

LJ_NOINLINE char* lj_buf_need2(SBuf* sb, MSize sz)
{
   lj_assertG_(G(sbufL(sb)), sz > sbufsz(sb), "SBuf overflow");
   if (LJ_UNLIKELY(sz > LJ_MAX_BUF))
      lj_err_mem(sbufL(sb));
   buf_grow(sb, sz);
   return sb->b;
}

LJ_NOINLINE char* lj_buf_more2(SBuf* sb, MSize sz)
{
   if (sbufisext(sb)) {
      SBufExt* sbx = (SBufExt*)sb;
      MSize len = sbufxlen(sbx);
      if (LJ_UNLIKELY(sz > LJ_MAX_BUF or len + sz > LJ_MAX_BUF))
         lj_err_mem(sbufL(sbx));
      if (len + sz > sbufsz(sbx)) {  // Must grow.
         buf_grow((SBuf*)sbx, len + sz);
      }
      else if (sbufxslack(sbx) < (sbufsz(sbx) >> 3)) {
         // Also grow to avoid excessive compactions, if slack < size/8.
         buf_grow((SBuf*)sbx, sbuflen(sbx) + sz);  //  Not sbufxlen!
         return sbx->w;
      }
      if (sbx->r != sbx->b) {  // Compact by moving down.
         memmove(sbx->b, sbx->r, len);
         sbx->r = sbx->b;
         sbx->w = sbx->b + len;
         lj_assertG_(G(sbufL(sbx)), len + sz <= sbufsz(sbx), "bad SBuf compact");
      }
   }
   else {
      MSize len = sbuflen(sb);
      lj_assertG_(G(sbufL(sb)), sz > sbufleft(sb), "SBuf overflow");
      if (LJ_UNLIKELY(sz > LJ_MAX_BUF or len + sz > LJ_MAX_BUF))
         lj_err_mem(sbufL(sb));
      buf_grow(sb, len + sz);
   }
   return sb->w;
}

void lj_buf_shrink(lua_State* L, SBuf* sb)
{
   char* b = sb->b;
   MSize osz = (MSize)(sb->e - b);
   if (osz > 2 * LJ_MIN_SBUF) {
      MSize n = (MSize)(sb->w - b);
      b = (char*)lj_mem_realloc(L, b, osz, (osz >> 1));
      sb->b = b;
      sb->w = b + n;
      sb->e = b + (osz >> 1);
   }
   lj_assertG_(G(sbufL(sb)), !sbufisext(sb), "YAGNI shrink SBufExt");
}

char* lj_buf_tmp(lua_State* L, MSize sz)
{
   SBuf* sb = &G(L)->tmpbuf;
   setsbufL(sb, L);
   return lj_buf_need(sb, sz);
}

// -- Low-level buffer put operations -------------------------------------

SBuf* lj_buf_putmem(SBuf* sb, const void* q, MSize len)
{
   char* w = lj_buf_more(sb, len);
   w = lj_buf_wmem(w, q, len);
   sb->w = w;
   return sb;
}

static LJ_NOINLINE SBuf* lj_buf_putchar2(SBuf* sb, int c)
{
   char* w = lj_buf_more2(sb, 1);
   *w++ = (char)c;
   sb->w = w;
   return sb;
}

SBuf* lj_buf_putchar(SBuf* sb, int c)
{
   char* w = sb->w;
   if (LJ_LIKELY(w < sb->e)) {
      *w++ = (char)c;
      sb->w = w;
      return sb;
   }
   return lj_buf_putchar2(sb, c);
}

SBuf* lj_buf_putstr(SBuf* sb, GCstr* s)
{
   MSize len = s->len;
   char* w = lj_buf_more(sb, len);
   w = lj_buf_wmem(w, strdata(s), len);
   sb->w = w;
   return sb;
}

// High-level buffer put operations

SBuf* lj_buf_putstr_reverse(SBuf* sb, GCstr* s)
{
   MSize len = s->len;
   char* w = lj_buf_more(sb, len), * e = w + len;
   const char* q = strdata(s) + len - 1;
   while (w < e)
      *w++ = *q--;
   sb->w = w;
   return sb;
}

SBuf* lj_buf_putstr_lower(SBuf* sb, GCstr* s)
{
   MSize len = s->len;
   char* w = lj_buf_more(sb, len), * e = w + len;
   const char* q = strdata(s);
   for (; w < e; w++, q++) {
      uint32_t c = *(unsigned char*)q;
#if LJ_TARGET_PPC
      * w = c + ((c >= 'A' and c <= 'Z') << 5);
#else
      if (c >= 'A' and c <= 'Z') c += 0x20;
      *w = c;
#endif
   }
   sb->w = w;
   return sb;
}

SBuf* lj_buf_putstr_upper(SBuf* sb, GCstr* s)
{
   MSize len = s->len;
   char* w = lj_buf_more(sb, len), * e = w + len;
   const char* q = strdata(s);
   for (; w < e; w++, q++) {
      uint32_t c = *(unsigned char*)q;
#if LJ_TARGET_PPC
      * w = c - ((c >= 'a' and c <= 'z') << 5);
#else
      if (c >= 'a' and c <= 'z') c -= 0x20;
      *w = c;
#endif
   }
   sb->w = w;
   return sb;
}

//********************************************************************************************************************
// Case mapping for the Basic Latin, Latin-1 Supplement, Latin Extended-A, Greek, Cyrillic, Cyrillic Supplement and
// Latin Extended Additional blocks.  Letters without a single-letter counterpart, such as 'ß', are returned unchanged.
// The UTF-8 encoding of a result is never longer than that of the code point, which the conversion loop relies upon.

static uint32_t upper_code(uint32_t Code)
{
   if (Code < 0x80) return (Code >= 'a' and Code <= 'z') ? Code - 0x20 : Code;

   // Latin-1 Supplement and Latin Extended-A

   if (Code IS 0xb5) return 0x39c; // Micro sign to Greek capital mu
   if (Code IS 0xff) return 0x178;
   if (Code IS 0x131) return 'I'; // Dotless i
   if (Code IS 0x17f) return 'S'; // Long s
   if (Code >= 0xe0 and Code <= 0xfe) return (Code IS 0xf7) ? Code : Code - 0x20;
   if ((Code >= 0x100 and Code <= 0x137) or (Code >= 0x14a and Code <= 0x177)) return Code & ~1u;
   if ((Code >= 0x139 and Code <= 0x148) or (Code >= 0x179 and Code <= 0x17e)) return (Code & 1) ? Code : Code - 1;

   // Greek

   if ((Code >= 0x370 and Code <= 0x373) or (Code >= 0x376 and Code <= 0x377) or
       (Code >= 0x3d8 and Code <= 0x3ef)) return Code & ~1u;
   if (Code >= 0x37b and Code <= 0x37d) return Code + 0x82;
   switch (Code) {
      case 0x3d0: return 0x392; // Beta symbol
      case 0x3d1: return 0x398; // Theta symbol
      case 0x3d5: return 0x3a6; // Phi symbol
      case 0x3d6: return 0x3a0; // Pi symbol
      case 0x3d7: return 0x3cf; // Kai symbol
      case 0x3f0: return 0x39a; // Kappa symbol
      case 0x3f1: return 0x3a1; // Rho symbol
      case 0x3f2: return 0x3f9; // Lunate sigma symbol
      case 0x3f3: return 0x37f; // Yot
      case 0x3f5: return 0x395; // Lunate epsilon symbol
      case 0x3f8: return 0x3f7; // Sho
      case 0x3fb: return 0x3fa; // San
   }
   if (Code IS 0x3ac) return 0x386;
   if (Code >= 0x3ad and Code <= 0x3af) return Code - 37;
   if (Code IS 0x3c2) return 0x3a3; // Final sigma
   if (Code IS 0x3cc) return 0x38c;
   if (Code IS 0x3cd or Code IS 0x3ce) return Code - 63;
   if (Code >= 0x3b1 and Code <= 0x3cb) return Code - 32;

   // Cyrillic and Cyrillic Supplement

   if (Code >= 0x430 and Code <= 0x44f) return Code - 32;
   if (Code >= 0x450 and Code <= 0x45f) return Code - 80;
   if ((Code >= 0x460 and Code <= 0x481) or (Code >= 0x48a and Code <= 0x4bf) or (Code >= 0x4d0 and Code <= 0x52f)) {
      return Code & ~1u;
   }
   if (Code IS 0x4cf) return 0x4c0;
   if (Code >= 0x4c1 and Code <= 0x4ce) return (Code & 1) ? Code : Code - 1;

   // Latin Extended Additional, which includes the Vietnamese letters

   if (Code IS 0x1e9b) return 0x1e60; // Long s with dot above
   if ((Code >= 0x1e00 and Code <= 0x1e95) or (Code >= 0x1ea0 and Code <= 0x1eff)) return Code & ~1u;
   return Code;
}

// Capital sigma always becomes the medial form 'σ', as the final form depends on the position in a word.

static uint32_t lower_code(uint32_t Code)
{
   if (Code < 0x80) return (Code >= 'A' and Code <= 'Z') ? Code + 0x20 : Code;

   // Latin-1 Supplement and Latin Extended-A

   if (Code IS 0x130) return 'i'; // Capital I with dot above
   if (Code IS 0x178) return 0xff;
   if (Code >= 0xc0 and Code <= 0xde) return (Code IS 0xd7) ? Code : Code + 0x20;
   if ((Code >= 0x100 and Code <= 0x137) or (Code >= 0x14a and Code <= 0x177)) return Code | 1u;
   if ((Code >= 0x139 and Code <= 0x148) or (Code >= 0x179 and Code <= 0x17e)) return (Code & 1) ? Code + 1 : Code;

   // Greek

   if ((Code >= 0x370 and Code <= 0x373) or (Code >= 0x376 and Code <= 0x377) or
       (Code >= 0x3d8 and Code <= 0x3ef)) return Code | 1u;
   if (Code IS 0x37f) return 0x3f3; // Yot
   if (Code IS 0x3cf) return 0x3d7; // Kai symbol
   if (Code IS 0x3f4) return 0x3b8; // Theta symbol
   if (Code IS 0x3f7) return 0x3f8; // Sho
   if (Code IS 0x3f9) return 0x3f2; // Lunate sigma symbol
   if (Code IS 0x3fa) return 0x3fb; // San
   if (Code >= 0x3fd and Code <= 0x3ff) return Code - 0x82;
   if (Code IS 0x386) return 0x3ac;
   if (Code >= 0x388 and Code <= 0x38a) return Code + 37;
   if (Code IS 0x38c) return 0x3cc;
   if (Code IS 0x38e or Code IS 0x38f) return Code + 63;
   if (Code >= 0x391 and Code <= 0x3ab and Code != 0x3a2) return Code + 32;

   // Cyrillic and Cyrillic Supplement

   if (Code >= 0x400 and Code <= 0x40f) return Code + 80;
   if (Code >= 0x410 and Code <= 0x42f) return Code + 32;
   if ((Code >= 0x460 and Code <= 0x481) or (Code >= 0x48a and Code <= 0x4bf) or (Code >= 0x4d0 and Code <= 0x52f)) {
      return Code | 1u;
   }
   if (Code IS 0x4c0) return 0x4cf;
   if (Code >= 0x4c1 and Code <= 0x4ce) return (Code & 1) ? Code + 1 : Code;

   // Latin Extended Additional, which includes the Vietnamese letters

   if (Code IS 0x1e9e) return 0xdf; // Capital sharp s
   if ((Code >= 0x1e00 and Code <= 0x1e95) or (Code >= 0x1ea0 and Code <= 0x1eff)) return Code | 1u;
   return Code;
}

//********************************************************************************************************************
// Case conversion of UTF-8 text with a code point mapping, including accented and non-Latin letters.  Malformed and
// overlong sequences, and characters outside of the supported blocks, are copied unchanged.  The result is never
// longer than the source.

template <uint32_t (*Map)(uint32_t)>
static SBuf* putstr_case_utf8(SBuf* sb, GCstr* s)
{
   MSize len = s->len;
   char* w = lj_buf_more(sb, len);
   auto q = (const unsigned char*)strdata(s);
   auto e = q + len;
   while (q < e) {
      uint32_t c = *q;
      if (c < 0x80) {
         *w++ = char(Map(c));
         q++;
         continue;
      }

      MSize size = 0;
      uint32_t code = 0;
      if ((c & 0xe0) IS 0xc0 and (e - q) >= 2 and (q[1] & 0xc0) IS 0x80) {
         code = ((c & 0x1f) << 6) | (q[1] & 0x3f);
         if (code >= 0x80) size = 2;
      }
      else if ((c & 0xf0) IS 0xe0 and (e - q) >= 3 and (q[1] & 0xc0) IS 0x80 and (q[2] & 0xc0) IS 0x80) {
         code = ((c & 0x0f) << 12) | ((q[1] & 0x3f) << 6) | (q[2] & 0x3f);
         if (code >= 0x800) size = 3;
      }

      if (not size) { // Not a two or three byte sequence: copy the byte as it is
         *w++ = char(c);
         q++;
         continue;
      }

      uint32_t mapped = Map(code);
      if (mapped IS code) {
         memcpy(w, q, size);
         w += size;
      }
      else if (mapped < 0x80) *w++ = char(mapped);
      else if (mapped < 0x800) {
         *w++ = char(0xc0 | (mapped >> 6));
         *w++ = char(0x80 | (mapped & 0x3f));
      }
      else {
         *w++ = char(0xe0 | (mapped >> 12));
         *w++ = char(0x80 | ((mapped >> 6) & 0x3f));
         *w++ = char(0x80 | (mapped & 0x3f));
      }
      q += size;
   }
   sb->w = w;
   return sb;
}

SBuf* lj_buf_putstr_upper_utf8(SBuf* sb, GCstr* s)
{
   return putstr_case_utf8<upper_code>(sb, s);
}

SBuf* lj_buf_putstr_lower_utf8(SBuf* sb, GCstr* s)
{
   return putstr_case_utf8<lower_code>(sb, s);
}

SBuf* lj_buf_putstr_rep(SBuf* sb, GCstr* s, int32_t rep)
{
   MSize len = s->len;
   if (rep > 0 and len) {
      uint64_t tlen = (uint64_t)rep * len;
      char* w;
      if (LJ_UNLIKELY(tlen > LJ_MAX_STR))
         lj_err_mem(sbufL(sb));
      w = lj_buf_more(sb, (MSize)tlen);
      if (len == 1) {  // Optimize a common case.
         uint32_t c = strdata(s)[0];
         do { *w++ = c; } while (--rep > 0);
      }
      else {
         const char* e = strdata(s) + len;
         do {
            const char* q = strdata(s);
            do { *w++ = *q++; } while (q < e);
         } while (--rep > 0);
      }
      sb->w = w;
   }
   return sb;
}

SBuf* lj_buf_puttab(SBuf* sb, GCtab* t, GCstr* sep, int32_t i, int32_t e)
{
   MSize seplen = sep ? sep->len : 0;
   // 0-based: clamp start index to 0
   if (i < 0) i = 0;

   if (i <= e) {
      while (true) {
         cTValue *o = lj_tab_getint(t, i);
         char *w;

         if (!o) {
badtype:  //  Error: bad element type.
            sb->w = (char*)(intptr_t)i;  //  Store failing index.
            return nullptr;
         }
         else if (tvisstr(o)) {
            MSize len = strV(o)->len;
            w = lj_buf_wmem(lj_buf_more(sb, len + seplen), strVdata(o), len);
         }
         else if (tvisint(o)) {
            w = lj_strfmt_wint(lj_buf_more(sb, STRFMT_MAXBUF_INT + seplen), intV(o));
         }
         else if (tvisnum(o)) {
            w = lj_buf_more(lj_strfmt_putfnum(sb, STRFMT_G14, numV(o)), seplen);
         }
         else {
            goto badtype;
         }

         if (i++ == e) {
            sb->w = w;
            break;
         }

         if (seplen) w = lj_buf_wmem(w, strdata(sep), seplen);
         sb->w = w;
      }
   }
   return sb;
}

// Miscellaneous buffer operations

GCstr* lj_buf_tostr(SBuf* sb)
{
   return lj_str_new(sbufL(sb), sb->b, sbuflen(sb));
}

// Concatenate two strings.

GCstr* lj_buf_cat2str(lua_State* L, GCstr* s1, GCstr* s2)
{
   MSize len1 = s1->len, len2 = s2->len;
   char* buf = lj_buf_tmp(L, len1 + len2);
   memcpy(buf, strdata(s1), len1);
   memcpy(buf + len1, strdata(s2), len2);
   return lj_str_new(L, buf, len1 + len2);
}

// Read ULEB128 from buffer.

uint32_t lj_buf_ruleb128(const char** pp)
{
   const uint8_t* w = (const uint8_t*)*pp;
   uint32_t v = *w++;
   if (LJ_UNLIKELY(v >= 0x80)) {
      int sh = 0;
      v &= 0x7f;
      do { v |= ((*w & 0x7f) << (sh += 7)); } while (*w++ >= 0x80);
   }
   *pp = (const char*)w;
   return v;
}
