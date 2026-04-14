// String library
//
// Copyright (C) 2025-2026 Paul Manias
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Major portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#define lib_string_c
#define LUA_LIB

#include <cctype>
#include <array>
#include <string_view>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_ff.h"
#include "lj_bcdump.h"
#include "lj_char.h"
#include "lj_strfmt.h"
#include "lj_array.h"
#include "lib.h"
#include "lib_utils.h"
#include "lib_range.h"
#include "lj_proto_registry.h"
#include "debug/error_guard.h"

#include <kotuku/modules/regex.h>

#include "../../defs.h"

#define L_ESC      '%'

// Helper to check if a TValue is a range userdata and extract it

static tiri_range * get_range_from_tvalue(lua_State *L, cTValue *tv)
{
   if (not tvisudata(tv)) return nullptr;

   GCudata* ud = udataV(tv);
   GCtab* mt = tabref(ud->metatable);
   if (not mt) return nullptr;

   // Get the expected metatable for ranges
   lua_getfield(L, LUA_REGISTRYINDEX, RANGE_METATABLE);
   if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      return nullptr;
   }
   GCtab* range_mt = tabV(L->top - 1);
   lua_pop(L, 1);

   // Compare metatables
   if (mt != range_mt) return nullptr;

   return (tiri_range*)uddata(ud);
}

// NOTE: Any string function marked with the ASM macro uses a custom assembly implementation in the
// .dasc files.  Changing the C++ code here will have no effect in such cases.

#define LJLIB_MODULE_string

LJLIB_CF(string_len)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   int32_t len = (int32_t)s->len;
   setintV(L->top - 1, len);
   return 1;
}

//********************************************************************************************************************
// NOTE: ASM version exists

LJLIB_ASM(string_byte)      LJLIB_REC(string_range 0)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   int32_t len = (int32_t)s->len;
   int32_t start = lj_lib_optint(L, 2, 0);  // 0-based: default start is 0
   int32_t stop = lj_lib_optint(L, 3, start);
   int32_t n, i;
   const unsigned char *p;
   if (stop < 0) stop += len;   // 0-based: -1 → len-1 (last char)
   if (start < 0) start += len;
   if (start < 0) start = 0;
   if (stop > len - 1) stop = len - 1;  // 0-based: max valid index is len-1
   if (start > stop) return FFH_RES(0);  //  Empty interval: return no results.
   n = stop - start + 1;
   if ((uint32_t)n > LUAI_MAXCSTACK) lj_err_caller(L, ErrMsg::STRSLC);
   lj_state_checkstack(L, (MSize)n);
   p = (const unsigned char*)strdata(s) + start;
   for (i = 0; i < n; i++) setintV(L->base + i - 1 - LJ_FR2, p[i]);
   return FFH_RES(n);
}

//********************************************************************************************************************
// NOTE: ASM version exists

LJLIB_ASM(string_char)      LJLIB_REC(.)
{
   int i, nargs = (int)(L->top - L->base);
   char* buf = lj_buf_tmp(L, (MSize)nargs);
   for (i = 1; i <= nargs; i++) {
      int32_t k = lj_lib_checkint(L, i);
      LJ_CHECK_ARG(L, i, checku8(k), ErrMsg::BADVAL);
      buf[i - 1] = (char)k;
   }
   setstrV(L, L->base - 1 - LJ_FR2, lj_str_new(L, buf, (size_t)nargs));
   return FFH_RES(1);
}

//********************************************************************************************************************
// NOTE: Backed by an ASM implementation
// string_sub:	Declares an assembly ffunc as its primary implementation. The C code that follows is the fallback (called when the ffunc jumps to ->fff_fallback).
// string_range 1: Tells the JIT recorder how to handle this function. string_range is the recorder function name, 1 is a parameter distinguishing it from other range operations.

LJLIB_ASM(string_sub)      LJLIB_REC(string_range 1)
{
   pf::Log("string.sub()").warning("Use substr()");
   lj_lib_checkstr(L, 1);
   lj_lib_checkint(L, 2);
   int32_t end_val = lj_lib_optint(L, 3, -1);
   // Convert exclusive end to inclusive by subtracting 1, but only for positive indices.
   // Negative indices already reference positions from the end, so no adjustment needed.
   if (end_val > 0) end_val--;
   setintV(L->base + 2, end_val);
   return FFH_RETRY;
}

//********************************************************************************************************************
// string.substr() is now an alias for string.sub() - both use exclusive end semantics.
// The ASM implementation jumps directly to string_sub.

LJLIB_ASM(string_substr)      LJLIB_REC(string_range 1)
{
   lj_lib_checkstr(L, 1);
   lj_lib_checkint(L, 2);
   int32_t end_val = lj_lib_optint(L, 3, -1);
   // Convert exclusive end to inclusive by subtracting 1, but only for positive indices.
   // Negative indices already reference positions from the end, so no adjustment needed.
   if (end_val > 0) end_val--;
   setintV(L->base + 2, end_val);
   return FFH_RETRY;
}

//********************************************************************************************************************

LJLIB_CF(string_rep)      LJLIB_REC(.)
{
   GCstr *s = lj_lib_optstr(L, 1);
   if (not s) s = &G(L)->strempty;
   int32_t rep = lj_lib_checkint(L, 2);
   GCstr *sep = lj_lib_optstr(L, 3);
   SBuf *sb = lj_buf_tmp_(L);
   if (sep and rep > 1) {
      GCstr *s2 = lj_buf_cat2str(L, sep, s);
      lj_buf_reset(sb);
      lj_buf_putstr(sb, s);
      s = s2;
      rep--;
   }
   sb = lj_buf_putstr_rep(sb, s, rep);
   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************
// string.alloc() is a quicker version of string.rep() for reserving space without filling it.
//
// 1. Takes a size parameter - Uses lj_lib_checkint(L, 1) to get the size from the first argument
// 2. Validates the size - Checks that size is not negative and throws an error if it is
// 3. Reserves buffer space - Uses lj_buf_need(sb, (MSize)size) to ensure the buffer has enough capacity
// 4. Advances the write pointer - Sets sb->w += size to reserve the space without filling it
// 5. Returns the string - Creates and returns the string with the reserved space

LJLIB_CF(string_alloc)
{
   int32_t size = lj_lib_checkint(L, 1);
   LJ_CHECK_ARG(L, 1, size >= 0, ErrMsg::NUMRNG);
   SBuf *sb = lj_buf_tmp_(L);
   lj_buf_reset(sb);
   (void)lj_buf_need(sb, (MSize)size);
   sb->w += size;  //  Advance write pointer to reserve space
   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************
// Helper to find the next separator in a string
// Returns pointer to separator if found, nullptr otherwise

static CSTRING find_separator(CSTRING Pos, CSTRING End, CSTRING Sep, MSize SepLen, bool IsWhitespace)
{
   if (IsWhitespace) {
      // Whitespace separators: scan the entire range, independent of SepLen.
      for (CSTRING p = Pos; p < End; p++) {
         if (*p IS ' ' or *p IS '\t' or *p IS '\n' or *p IS '\r') return p;
      }
      return nullptr;
   }

   if (SepLen IS 1) return (CSTRING)memchr(Pos, Sep[0], End - Pos);

   // Multi-character separator.
   for (CSTRING p = Pos; p <= End - SepLen; p++) {
      if (memcmp(p, Sep, SepLen) IS 0) return p;
   }
   return nullptr;
}

//********************************************************************************************************************

LJLIB_CF(string_split)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   GCstr *sep = lj_lib_optstr(L, 2);
   CSTRING str = strdata(s);
   CSTRING sepstr;
   MSize seplen;
   MSize str_len = s->len;
   bool is_whitespace = false;

   if ((not sep) or (sep->len IS 0)) {
      sepstr = " \t\n\r";  // Default whitespace separators
      seplen = 4;
      is_whitespace = true;
   }
   else {
      sepstr = strdata(sep);
      seplen = sep->len;
   }

   // Handle empty string - return empty array
   if (str_len IS 0) {
      GCarray *arr = lj_array_new(L, 0, AET::STR_GC);
      setarrayV(L, L->top++, arr);
      return 1;
   }

   CSTRING end = str + str_len;

   // First pass: count separators to determine array size
   uint32_t count = 1;  // At least one element (final segment)
   CSTRING pos = str;
   while (pos < end) {
      CSTRING found = find_separator(pos, end, sepstr, seplen, is_whitespace);
      if (found) {
         count++;
         pos = found + (is_whitespace ? 1 : seplen);
      }
      else break;
   }

   // Create array with exact size
   GCarray *arr = lj_array_new(L, count, AET::STR_GC);
   GCRef *refs = arr->get<GCRef>();

   // Second pass: populate the array
   pos = str;
   uint32_t idx = 0;
   while (pos <= end and idx < count) {
      CSTRING found = find_separator(pos, end, sepstr, seplen, is_whitespace);

      if (found) {
         GCstr *substr = lj_str_new(L, pos, found - pos);
         setgcref(refs[idx], obj2gco(substr));
         lj_gc_objbarrier(L, arr, substr);
         idx++;
         pos = found + (is_whitespace ? 1 : seplen);
      }
      else {
         // Add final substring
         GCstr *substr = lj_str_new(L, pos, end - pos);
         setgcref(refs[idx], obj2gco(substr));
         lj_gc_objbarrier(L, arr, substr);
         idx++;
         break;
      }
   }

   setarrayV(L, L->top++, arr);
   lj_gc_check(L);
   return 1;
}
//********************************************************************************************************************
// string.count(s, keyword) -> count

LJLIB_CF(string_count)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   GCstr *keyword = lj_lib_checkstr(L, 2);

   CSTRING str  = strdata(s);
   auto str_len = s->len;
   auto kw_len  = keyword->len;

   if (not kw_len) { // Handle empty search string
      setintV(L->top++, 0);
      return 1;
   }
   else if (not str_len) { // Handle empty source string
      setintV(L->top++, 0);
      return 1;
   }

   CSTRING pos = str;
   CSTRING end = str + str_len;
   CSTRING kw  = strdata(keyword);
   int32_t count = 0;

   while (pos < end) {
      if (auto found = lj_str_findsv({pos, size_t(end - pos)}, {kw, kw_len})) {
         pos = found + kw_len;
         count++;
      }
      else break;
   }

   setintV(L->top++, count);
   return 1;
}

//********************************************************************************************************************
// string.replace(s, search, replacement [, max_count]) -> string, count
// Simple literal string replacement (no pattern matching).  Returns the modified string and the replacement count.

LJLIB_CF(string_replace)
{
   GCstr *s           = lj_lib_checkstr(L, 1);
   GCstr *search      = lj_lib_checkstr(L, 2);
   GCstr *replacement = lj_lib_checkstr(L, 3);
   int32_t max_count  = lj_lib_optint(L, 4, -1);  // -1 means replace all

   CSTRING str        = strdata(s);
   CSTRING searchstr  = strdata(search);
   CSTRING replacestr = strdata(replacement);
   MSize str_len      = s->len;
   MSize searchlen    = search->len;
   MSize replacelen   = replacement->len;

   if (searchlen IS 0) { // Handle empty search string - return original string with 0 replacements
      setstrV(L, L->top++, s);
      setintV(L->top++, 0);
      return 2;
   }

   if (str_len IS 0) { // Handle empty source string
      setstrV(L, L->top++, &G(L)->strempty);
      setintV(L->top++, 0);
      return 2;
   }

   SBuf *sb = lj_buf_tmp_(L);
   lj_buf_reset(sb);

   CSTRING pos = str;
   CSTRING end = str + str_len;
   int32_t count = 0;

   while (pos < end) {
      if (max_count >= 0 and count >= max_count) { // Check if we've reached max replacements
         lj_buf_putmem(sb, pos, end - pos); // Copy remaining string
         break;
      }

      if (auto found = lj_str_findsv({pos, size_t(end - pos)}, {searchstr, searchlen})) {
         if (found > pos) lj_buf_putmem(sb, pos, found - pos);
         if (replacelen > 0) lj_buf_putmem(sb, replacestr, replacelen);
         pos = found + searchlen;
         count++;
      }
      else { // No more matches - copy remaining string
         lj_buf_putmem(sb, pos, end - pos);
         break;
      }
   }

   setstrV(L, L->top++, lj_buf_str(L, sb));
   setintV(L->top++, count);
   lj_gc_check(L);
   return 2;
}

//********************************************************************************************************************

LJLIB_CF(string_trim)
{
   GCstr *s = lj_lib_optstr(L, 1);
   if (not s) {
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   CSTRING str   = strdata(s);
   MSize len     = s->len;
   CSTRING start = str;
   CSTRING end   = str + len;

   if (len IS 0) {
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   // Skip leading whitespace
   while (start < end and (*start IS ' ' or *start IS '\t' or *start IS '\n' or *start IS '\r')) start++;

   // Skip trailing whitespace
   while (end > start and (end[-1] IS ' ' or end[-1] IS '\t' or end[-1] IS '\n' or end[-1] IS '\r')) end--;

   // If all whitespace, return empty string
   if (start >= end) {
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   // Create trimmed string
   GCstr *result = lj_str_new(L, start, end - start);
   setstrV(L, L->top - 1, result);
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(string_rtrim)
{
   GCstr *s = lj_lib_optstr(L, 1);
   if (not s) {
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   auto str = strdata(s);
   MSize len = s->len;
   auto end = str + len;

   if (len IS 0) {
      setstrV(L, L->top - 1, s);  //  Return original empty string
      return 1;
   }

   // Find end of non-whitespace
   while (end > str and (end[-1] IS ' ' or end[-1] IS '\t' or end[-1] IS '\n' or end[-1] IS '\r'))
      end--;

   // Create right-trimmed string
   GCstr *result = lj_str_new(L, str, end - str);
   setstrV(L, L->top - 1, result);
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(string_startsWith)
{
   GCstr *s        = lj_lib_checkstr(L, 1);
   GCstr *prefix   = lj_lib_checkstr(L, 2);
   auto str        = strdata(s);
   auto prefixstr  = strdata(prefix);
   MSize str_len   = s->len;
   MSize prefixlen = prefix->len;

   if (prefixlen IS 0) { // Empty prefix always matches
      setboolV(L->top - 1, 1);
      return 1;
   }
   else if (prefixlen > str_len) { // Prefix longer than string cannot match
      setboolV(L->top - 1, 0);
      return 1;
   }
   else { // Compare prefix with start of string
      int matches = (memcmp(str, prefixstr, prefixlen) IS 0);
      setboolV(L->top - 1, matches);
      return 1;
   }
}

//********************************************************************************************************************

LJLIB_CF(string_endsWith)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   GCstr *suffix = lj_lib_checkstr(L, 2);
   CSTRING str = strdata(s);
   CSTRING suffixstr = strdata(suffix);
   MSize str_len = s->len;
   MSize suffixlen = suffix->len;

   if (suffixlen IS 0) { // Empty suffix always matches
      setboolV(L->top - 1, 1);
      return 1;
   }
   else if (suffixlen > str_len) { // Suffix longer than string cannot match
      setboolV(L->top - 1, 0);
      return 1;
   }
   else { // Compare suffix with end of string
      int matches = (memcmp(str + str_len - suffixlen, suffixstr, suffixlen) IS 0);
      setboolV(L->top - 1, matches);
      return 1;
   }
}

//********************************************************************************************************************
// string.contains(s, substr) -> bool
// Returns true if substr is found within s.  Enables the `in` operator for strings, e.g. `"ell" in "hello"`.

LJLIB_CF(string_contains)
{
   GCstr *s      = lj_lib_checkstr(L, 1);
   GCstr *substr = lj_lib_checkstr(L, 2);

   if (substr->len IS 0) {
      setboolV(L->top - 1, 1);
      return 1;
   }
   else if (substr->len > s->len) {
      setboolV(L->top - 1, 0);
      return 1;
   }
   else {
      int found = lj_str_findsv({strdata(s), s->len}, {strdata(substr), substr->len}) ? 1 : 0;
      setboolV(L->top - 1, found);
      return 1;
   }
}

//********************************************************************************************************************

LJLIB_CF(string_cap)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   auto str = strdata(s);
   MSize len = s->len;

   if (len IS 0) {
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   SBuf *sb = lj_buf_tmp_(L);
   lj_buf_reset(sb);

   char first = str[0];
   if (first >= 'a' and first <= 'z') first -= 32;
   lj_buf_putb(sb, first);

   if (len > 1) lj_buf_putmem(sb, str + 1, len - 1);

   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(string_decap)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   auto str = strdata(s);
   MSize len = s->len;

   if (len IS 0) {
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   SBuf *sb = lj_buf_tmp_(L);
   lj_buf_reset(sb);

   char first = str[0];
   if (first >= 'A' and first <= 'Z') first += 32;
   lj_buf_putb(sb, first);

   if (len > 1) lj_buf_putmem(sb, str + 1, len - 1);

   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************
// string.pop(s [, count]) - Remove characters from the end of a string.
// Returns the string with 'count' characters removed from the end (default 1).
// If count >= string length, returns an empty string.

LJLIB_CF(string_pop)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   int32_t count = lj_lib_optint(L, 2, 1);
   MSize len = s->len;

   // Handle negative or zero count - return original string
   if (count <= 0) {
      setstrV(L, L->top - 1, s);
      return 1;
   }

   // Handle edge cases
   if (len IS 0 or MSize(count) >= len) {
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   // Create new string with characters removed from end
   GCstr *result = lj_str_new(L, strdata(s), len - count);
   setstrV(L, L->top - 1, result);
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(string_hash)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   int caseSensitive = 0;  //  Default: case insensitive

   // Check for optional second parameter (boolean)
   if (L->base + 1 < L->top and tvisbool(L->base + 1)) caseSensitive = boolV(L->base + 1);

   auto str = strdata(s);
   uint32_t hash;
   if (caseSensitive) hash = pf::strhash({ str, s->len });
   else hash = pf::strihash({ str, s->len });

   setintV(L->top - 1, (int32_t)hash);
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(string_unescapeXML)
{
   GCstr *s = lj_lib_optstr(L, 1);

   if (not s) {
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   static constexpr std::array<std::pair<std::string_view, char>, 5> entities = {{
      { "lt;", '<' }, { "gt;", '>' }, { "amp;", '&' }, { "quot;", '"' }, { "apos;", '\'' }
   }};

   std::string_view input(strdata(s), s->len);
   SBuf *sb = lj_buf_tmp_(L);
   lj_buf_reset(sb);

   for (size_t i = 0; i < input.size(); i++) {
      if (input[i] IS '&') {
         std::string_view remaining = input.substr(i + 1);
         bool matched = false;

         for (const auto& [entity, replacement] : entities) {
            if (remaining.starts_with(entity)) {
               lj_buf_putb(sb, replacement);
               i += entity.size();
               matched = true;
               break;
            }
         }

         if (not matched) lj_buf_putb(sb, '&');
      }
      else lj_buf_putb(sb, input[i]);
   }

   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(string_escXML)
{
   GCstr *s = lj_lib_optstr(L, 1);

   if (not s) { // Handle nil input - return empty string
      setstrV(L, L->top - 1, &G(L)->strempty);
      return 1;
   }

   auto str = strdata(s);
   MSize len = s->len;
   SBuf *sb = lj_buf_tmp_(L);
   MSize i;

   lj_buf_reset(sb);

   for (i = 0; i < len; i++) {
      char c = str[i];
      switch (c) {
         case '&': lj_buf_putmem(sb, "&amp;", 5); break;
         case '<': lj_buf_putmem(sb, "&lt;", 4); break;
         case '>': lj_buf_putmem(sb, "&gt;", 4); break;
         default: lj_buf_putb(sb, c); break;
      }
   }

   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************

LJLIB_ASM(string_reverse)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_reverse)
{
   lj_lib_checkstr(L, 1);
   return FFH_RETRY;
}
LJLIB_ASM_(string_lower)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_lower)
LJLIB_ASM_(string_upper)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_upper)

//********************************************************************************************************************

static int writer_buf(lua_State *L, const void *p, size_t size, void *sb)
{
   lj_buf_putmem((SBuf*)sb, p, (MSize)size);
   return 0;
}

//********************************************************************************************************************

LJLIB_CF(string_dump)
{
   GCfunc* fn = lj_lib_checkfunc(L, 1);
   int strip = L->base + 1 < L->top and tvistruecond(L->base + 1);
   SBuf* sb = lj_buf_tmp_(L);  //  Assumes lj_bcwrite() doesn't use tmpbuf.
   L->top = L->base + 1;
   if (not isluafunc(fn) or lj_bcwrite(L, funcproto(fn), writer_buf, sb, strip)) lj_err_caller(L, ErrMsg::STRDUMP);
   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(string_find)      LJLIB_REC(.)
{
   GCstr *s = lj_lib_checkstr(L, 1);
   GCstr *p = lj_lib_checkstr(L, 2);
   int32_t start = lj_lib_optint(L, 3, 0);  // 0-based: default start at 0

   if (start < 0) start += (int32_t)s->len;  // 0-based: -1 → len-1

   if (start < 0) start = 0;

   auto st = (MSize)start;
   if (st > s->len) {
      setnilV(L->top - 1);
      return 1;
   }

   if (auto q = lj_str_findsv({strdata(s) + st, s->len - st}, {strdata(p), p->len})) {
      setintV(L->top - 2, (int32_t)(q - strdata(s)));  // 0-based start
      setintV(L->top - 1, (int32_t)(q - strdata(s)) + (int32_t)p->len - 1);  // 0-based end (inclusive)
      return 2;
   }

   setnilV(L->top - 1);  // Not found.
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(string_match)
{
   pf::Log("string.match()").warning("DEPRECATED");
   return 0;
}

//********************************************************************************************************************

LJLIB_CF(string_gmatch)
{
   pf::Log("string.gmatch()").warning("DEPRECATED");
   return 0;
}

//********************************************************************************************************************

LJLIB_CF(string_gsub)
{
   pf::Log("string.gsub()").warning("DEPRECATED");
   return 0;
}

//********************************************************************************************************************

LJLIB_CF(string_format)      LJLIB_REC(.)
{
   int retry = 0;
   SBuf* sb;
   do {
      sb = lj_buf_tmp_(L);
      retry = lj_strfmt_putarg(L, sb, 1, -retry);
   } while (retry > 0);
   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

#include "lj_libdef.h"

//********************************************************************************************************************
// Custom __index handler for strings
// Handles numeric keys for single-character access, range userdata for substring extraction, and string keys for
// method lookups (delegated to string library table)

static int string_index_handler(lua_State *L)
{
   // Argument 1: the string
   // Argument 2: the key (number, range userdata, or string)

   if (not tvisstr(L->base)) {
      lua_pushnil(L);
      return 1;
   }

   GCstr *str = strV(L->base);
   int32_t len = (int32_t)str->len;
   cTValue *key = L->base + 1;

   // Check for numeric key (single character access)
   if (tvisnum(key) or tvisint(key)) {
      int32_t idx = lj_lib_checkint(L, 2);

      // Handle negative indices (0-based: -1 means last character)
      if (idx < 0) idx += len;

      if (idx < 0 or idx >= len) {
         lua_pushnil(L);
         return 1;
      }

      // Return single character as string
      lua_pushlstring(L, strdata(str) + idx, 1);
      return 1;
   }

   // Check for the range type (substring extraction)

   if (tiri_range *r = get_range_from_tvalue(L, key)) {
      int32_t start = r->start;
      int32_t stop = r->stop;

      // Handle negative indices (always inclusive for negative ranges)
      bool use_inclusive = r->inclusive;
      if (start < 0 or stop < 0) {
         use_inclusive = true;  // Negative indices ignore inclusive flag
         if (start < 0) start += len;
         if (stop < 0) stop += len;
      }

      // Apply exclusive semantics if not inclusive
      int32_t effective_stop = stop;
      if (not use_inclusive) effective_stop = stop - 1;

      // Bounds checking
      if (start < 0) start = 0;
      if (effective_stop >= len) effective_stop = len - 1;

      // Handle empty/invalid ranges
      if (start > effective_stop or start >= len) {
         lua_pushstring(L, "");
         return 1;
      }

      int32_t sublen = effective_stop - start + 1;
      lua_pushlstring(L, strdata(str) + start, (size_t)sublen);
      return 1;
   }

   // Check for string key (method lookup)
   if (tvisstr(key)) {
      // Get the string library table (stored in upvalue 1)
      lua_pushvalue(L, lua_upvalueindex(1));
      lua_pushvalue(L, 2);  // Push the key
      lua_rawget(L, -2);    // Get string_lib[key] without metamethods
      return 1;
   }

   // Unknown key type
   lua_pushnil(L);
   return 1;
}

extern int luaopen_string(lua_State *L)
{
   LJ_LIB_REG(L, "string", string);
   // At this point, L->top - 1 has the string library table on the Lua stack

   GCtab *mt = lj_tab_new(L, 0, 1);

   // NOBARRIER: basemt is a GC root.
   global_State *g = G(L);

   // Stores the newly created table mt as the "base metatable" for the string type. basemt_it(g, LJ_TSTR) selects
   // the slot for the string base metatable in the global state, obj2gco(mt) converts the GCtab* to a generic GC
   // object reference, and setgcref writes that GC reference into the global state (so the runtime knows the
   // canonical metatable for strings).

   setgcref(basemt_it(g, LJ_TSTR), obj2gco(mt));

   // Create a closure for string_index_handler with the string library table as upvalue.
   // This allows str[idx], str[{0..5}], and str.method() syntax.
   // Stack after LJ_LIB_REG: [..., string_lib_table] at position -1

   lua_pushvalue(L, -1);  // Push copy of string library table for upvalue
   lua_pushcclosure(L, string_index_handler, 1);  // Create closure with 1 upvalue
   // Stack: [..., string_lib_table, closure]

   // Set the closure as __index metamethod
   TValue *index_slot = lj_tab_setstr(L, mt, mmname_str(g, MM_index));
   setfuncV(L, index_slot, funcV(L->top - 1));
   lua_pop(L, 1);  // Pop the closure
   // Stack: [..., string_lib_table]

   // Update the metatable's negative‑metamethod cache (nomm). The bitwise expression clears the bit
   // corresponding to MM_index (and sets other bits), marking that this metamethod slot should not be treated as
   // "absent" by the fast metamethod check. Practically, this tells the runtime the MM_index metamethod is present
   // (so subsequent lookups/optimisations behave accordingly).
   //
   // NOTE: nomm is an 8‑bit negative cache, so MM_index must fit within those bits; the bit trick is intentional
   // to flip the absence/presence semantics used by the VM.

   mt->nomm = (uint8_t)(~(1u << MM_index));

   // Register string interface prototypes for compile-time type inference
   reg_iface_prototype("string", "alloc", { TiriType::Str }, { TiriType::Num });
   reg_iface_prototype("string", "byte", { TiriType::Num }, { TiriType::Str, TiriType::Num }, FProtoFlags::Variadic);
   reg_iface_prototype("string", "cap", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("string", "char", { TiriType::Str }, {}, FProtoFlags::Variadic);
   reg_iface_prototype("string", "contains", { TiriType::Bool }, { TiriType::Str, TiriType::Str });
   reg_iface_prototype("string", "count", { TiriType::Num }, { TiriType::Str, TiriType::Str });
   reg_iface_prototype("string", "decap", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("string", "dump", { TiriType::Str }, { TiriType::Func });
   reg_iface_prototype("string", "endsWith", { TiriType::Bool }, { TiriType::Str, TiriType::Str });
   reg_iface_prototype("string", "escXML", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("string", "find", { TiriType::Num, TiriType::Num }, { TiriType::Str, TiriType::Str }, FProtoFlags::Variadic);
   reg_iface_prototype("string", "format", { TiriType::Str }, { TiriType::Str }, FProtoFlags::Variadic);
   reg_iface_prototype("string", "hash", { TiriType::Num }, { TiriType::Str, TiriType::Bool });
   reg_iface_prototype("string", "len", { TiriType::Num }, { TiriType::Str });
   reg_iface_prototype("string", "lower", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("string", "pop", { TiriType::Str }, { TiriType::Str, TiriType::Num });
   reg_iface_prototype("string", "rep", { TiriType::Str }, { TiriType::Str, TiriType::Num });
   reg_iface_prototype("string", "replace", { TiriType::Str, TiriType::Num }, { TiriType::Str, TiriType::Str, TiriType::Str, TiriType::Num });
   reg_iface_prototype("string", "reverse", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("string", "rtrim", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("string", "split", { TiriType::Array }, { TiriType::Str, TiriType::Str });
   reg_iface_prototype("string", "startsWith", { TiriType::Bool }, { TiriType::Str, TiriType::Str });
   reg_iface_prototype("string", "substr", { TiriType::Str }, { TiriType::Str, TiriType::Num, TiriType::Num });
   reg_iface_prototype("string", "trim", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("string", "unescapeXML", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("string", "upper", { TiriType::Str }, { TiriType::Str });
   // These are implemented in translate.tiri
   reg_iface_prototype("string", "translateRefresh", { }, { });
   reg_iface_prototype("string", "translate", { TiriType::Str }, { TiriType::Str });

   return 1;
}
