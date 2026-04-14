// String handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_str_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_char.h"
#include "lj_prng.h"
#include <kotuku/strings.hpp>
#include <algorithm>

//********************************************************************************************************************
// Ordered compare of strings. Assumes string data is 4-byte aligned.

int32_t lj_str_cmp(GCstr *a, GCstr *b)
{
   auto n = std::min(a->len, b->len);
   for (MSize i = 0; i < n; i += 4) {
      // Note: innocuous access up to end of string + 3.
      auto va = *(const uint32_t *)(strdata(a) + i);
      auto vb = *(const uint32_t *)(strdata(b) + i);
      if (va != vb) {
#if LJ_LE
         va = lj_bswap(va); vb = lj_bswap(vb);
#endif
         // Check if the mismatch is within the valid string region.
         auto overshoot = int32_t(i) - int32_t(n);
         if (overshoot >= -3) {
            va >>= 32 + (overshoot << 3); vb >>= 32 + (overshoot << 3);
            if (va IS vb) break;
         }
         return va < vb ? -1 : 1;
      }
   }
   return int32_t(a->len - b->len);
}

//********************************************************************************************************************
// Find fixed string p inside string s.

const char * lj_str_find(const char *s, const char *p, MSize slen, MSize plen)
{
   if (plen <= slen) {
      if (plen IS 0) {
         return s;
      }
      else {
         int c = *(const uint8_t *)p++;
         plen--; slen -= plen;
         while (slen) {
            auto q = (const char *)memchr(s, c, slen);
            if (!q) break;
            if (memcmp(q + 1, p, plen) IS 0) return q;
            q++; slen -= MSize(q - s); s = q;
         }
      }
   }
   return nullptr;
}

//********************************************************************************************************************
// Check whether a string contains a pattern matching character.

bool lj_str_haspattern(GCstr *s)
{
   constexpr std::string_view pattern_chars("^$*+?.([%-");
   std::string_view str(strdata(s), s->len);
   for (auto c : str) {
      if (lj_char_ispunct((uint8_t)c) and pattern_chars.find(c) != std::string_view::npos)
         return true;
   }
   return false;
}

//********************************************************************************************************************
// String interning

// Resize the string interning hash table (grow and shrink).

void lj_str_resize(lua_State *L, MSize newmask)
{
   auto g = G(L);

   // No resizing during GC traversal or if already too big.
   if (gc(g).phase() IS GCPhase::SweepString or newmask >= LJ_MAX_STRTAB - 1)
      return;

   auto *oldtab = g->str.tab;
   auto *newtab = lj_mem_newvec(L, newmask + 1, GCRef);
   memset(newtab, 0, (newmask + 1) * sizeof(GCRef));

   // Reinsert all strings from the old table into the new table.
   for (MSize i = g->str.mask; i != ~MSize(0); i--) {
      auto *o = (GCobj *)(gcrefu(oldtab[i]) & ~uintptr_t(1));
      while (o) {
         auto *next = gcnext(o);
         auto *s = gco_to_string(o);
         MSize hash = s->hash & newmask;
         // NOBARRIER: The string table is a GC root.
         setgcrefr(o->gch.nextgc, newtab[hash]);
         setgcref(newtab[hash], o);
         o = next;
      }
   }

   // Free old table and replace with new table.
   lj_str_freetab(g);
   g->str.tab = newtab;
   g->str.mask = newmask;
}

//********************************************************************************************************************
// Allocate a new string and add to string interning table.  Throws on failure.

static GCstr * lj_str_alloc(lua_State *L, const char *str, MSize len, uint32_t hash)
{
   auto *s = lj_mem_newt(L, lj_str_size(len), GCstr);
   auto *g = G(L);

   newwhite(g, s);

   s->gct      = ~LJ_TSTR;
   s->len      = len;
   s->hash     = hash;
   s->sid      = g->str.id++;
   s->reserved = 0;
   s->flags    = 0;

   // Clear last 4 bytes of allocated memory. Implies zero-termination, too.
   *(uint32_t *)(strdatawr(s) + (len & ~MSize(3))) = 0;
   memcpy(strdatawr(s), str, len);

   // Add to string hash table.
   MSize slot = hash & g->str.mask;
   auto u = gcrefu(g->str.tab[slot]);
   setgcrefp(s->nextgc, (u & ~uintptr_t(1)));

   // NOBARRIER: The string table is a GC root.
   setgcrefp(g->str.tab[slot], (uintptr_t(s) | (u & 1)));
   if (g->str.num++ > g->str.mask) { // Allow a 100% load factor.
      lj_str_resize(L, (g->str.mask << 1) + 1); // Grow string table.
   }
   return s;
}

//********************************************************************************************************************
// Intern a string and return string object.  Throws on failure.

GCstr * lj_str_new(lua_State *L, const char *str, size_t lenx)
{
   auto *g = G(L);
   if (lenx - 1 < LJ_MAX_STR - 1) {
      auto len = MSize(lenx);
      auto hash = pf::strhash(std::string_view(str, lenx));
      MSize coll = 0;
      // Check if the string has already been interned.
      auto *o = gcref(g->str.tab[hash & g->str.mask]);

      while (o != nullptr) {
         auto *sx = gco_to_string(o);
         if (sx->hash IS hash and sx->len IS len) {
            if (memcmp(str, strdata(sx), len) IS 0) {
               if (isdead(g, o)) flipwhite(o); // Resurrect if dead.
               return sx;
            }
            coll++;
         }
         coll++;
         o = gcnext(o);
      }

      // Otherwise allocate a new string.
      return lj_str_alloc(L, str, len, hash);
   }
   else {
      if (lenx) lj_err_msg(L, ErrMsg::STROV);
      return &g->strempty;
   }
}

//********************************************************************************************************************

void lj_str_free(global_State *g, GCstr *s)
{
   g->str.num--;
   lj_mem_free(g, s, lj_str_size(s->len));
}

//********************************************************************************************************************

void lj_str_init(lua_State *L)
{
   lj_str_resize(L, LJ_MIN_STRTAB - 1);
}
