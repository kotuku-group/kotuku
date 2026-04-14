/*
** Table handling.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lj_obj.h"

// Hash constants. Tuned using a brute force search.
inline constexpr int32_t HASH_BIAS = (-0x04c11db7);
inline constexpr int HASH_ROT1 = 14;
inline constexpr int HASH_ROT2 = 5;
inline constexpr int HASH_ROT3 = 13;

// Scramble the bits of numbers and pointers.
[[nodiscard]] inline constexpr uint32_t hashrot(uint32_t lo, uint32_t hi) noexcept
{
#if LJ_TARGET_X86ORX64
   // Prefer variant that compiles well for a 2-operand CPU.
   lo ^= hi; hi = lj_rol(hi, HASH_ROT1);
   lo -= hi; hi = lj_rol(hi, HASH_ROT2);
   hi ^= lo; hi -= lj_rol(lo, HASH_ROT3);
#else
   lo ^= hi;
   lo = lo - lj_rol(hi, HASH_ROT1);
   hi = lo ^ lj_rol(hi, HASH_ROT1 + HASH_ROT2);
   hi = hi - lj_rol(lo, HASH_ROT3);
#endif
   return hi;
}

// Hash values are masked with the table hash mask and used as an index.
[[nodiscard]] inline constexpr Node* hashmask(const GCtab* t, uint32_t hash) noexcept
{
   Node* n = noderef(t->node);
   return &n[hash & t->hmask];
}

// String IDs are generated when a string is interned.
[[nodiscard]] inline constexpr Node* hashstr(const GCtab* t, const GCstr* s) noexcept
{
   return hashmask(t, s->sid);
}

[[nodiscard]] inline constexpr Node* hashlohi(const GCtab* t, uint32_t lo, uint32_t hi) noexcept
{
   return hashmask(t, hashrot(lo, hi));
}

[[nodiscard]] inline constexpr Node* hashnum(const GCtab* t, const TValue* o) noexcept
{
   return hashlohi(t, o->u32.lo, (o->u32.hi << 1));
}

// Hash GC reference - 64-bit only
[[nodiscard]] inline constexpr Node* hashgcref(const GCtab* t, GCRef r) noexcept
{
   return hashlohi(t, (uint32_t)gcrefu(r), (uint32_t)(gcrefu(r) >> 32));
}

#define hsize2hbits(s)   ((s) ? ((s)==1 ? 1 : 1+lj_fls((uint32_t)((s)-1))) : 0)

LJ_FUNCA GCtab* lj_tab_new(lua_State* L, uint32_t asize, uint32_t hbits);
LJ_FUNC GCtab* lj_tab_new_ah(lua_State* L, int32_t a, int32_t h);
#if LJ_HASJIT
LJ_FUNC GCtab* lj_tab_new1(lua_State* L, uint32_t ahsize);
#endif
LJ_FUNCA GCtab* lj_tab_dup(lua_State* L, const GCtab* kt);
LJ_FUNC void lj_tab_clear(GCtab* t);
LJ_FUNC void lj_tab_free(global_State* g, GCtab* t);
LJ_FUNC void lj_tab_resize(lua_State* L, GCtab* t, uint32_t asize, uint32_t hbits);
LJ_FUNCA void lj_tab_reasize(lua_State* L, GCtab* t, uint32_t nasize);

// Caveat: all getters except lj_tab_get() can return NULL!

LJ_FUNCA [[nodiscard]] cTValue* lj_tab_getinth(GCtab* t, int32_t key);
LJ_FUNC [[nodiscard]] cTValue* lj_tab_getstr(GCtab* t, const GCstr* key);
LJ_FUNCA [[nodiscard]] cTValue* lj_tab_get(lua_State* L, GCtab* t, cTValue* key);

// Caveat: all setters require a write barrier for the stored value.

LJ_FUNCA TValue* lj_tab_newkey(lua_State* L, GCtab* t, cTValue* key);
LJ_FUNCA TValue* lj_tab_setinth(lua_State* L, GCtab* t, int32_t key);
LJ_FUNC TValue* lj_tab_setstr(lua_State* L, GCtab* t, const GCstr* key);
LJ_FUNC TValue* lj_tab_set(lua_State* L, GCtab* t, cTValue* key);

// 0-based indexing: valid array indices are [0, asize)
#define inarray(t, key)      ((MSize)(key) < (MSize)(t)->asize)
#define arrayslot(t, i)      (&tvref((t)->array)[(i)])
#define lj_tab_getint(t, key) \
  (inarray((t), (key)) ? arrayslot((t), (key)) : lj_tab_getinth((t), (key)))
#define lj_tab_setint(L, t, key) \
  (inarray((t), (key)) ? arrayslot((t), (key)) : lj_tab_setinth(L, (t), (key)))

LJ_FUNC uint32_t lj_tab_keyindex(GCtab* t, cTValue* key);
LJ_FUNCA int lj_tab_next(GCtab* t, cTValue* key, TValue* o);
LJ_FUNCA MSize lj_tab_len(GCtab* t);
#if LJ_HASJIT
LJ_FUNC MSize lj_tab_len_hint(GCtab* t, size_t hint);
#endif
