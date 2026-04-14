/*
** State and stack handling.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lj_obj.h"

#define incr_top(L) (++L->top >= tvref(L->maxstack) and (lj_state_growstack1(L), 0))

[[nodiscard]] inline ptrdiff_t savestack(lua_State* L, const TValue* p) noexcept
{
   return (const char*)(p) - mref<char>(L->stack);
}

[[nodiscard]] inline TValue* restorestack(lua_State* L, ptrdiff_t n) noexcept
{
   return (TValue*)(mref<char>(L->stack) + n);
}

LJ_FUNC void lj_state_relimitstack(lua_State* L);
LJ_FUNC void lj_state_shrinkstack(lua_State* L, MSize used);
LJ_FUNCA void lj_state_growstack(lua_State* L, MSize need);
LJ_FUNC void lj_state_growstack1(lua_State* L);

static LJ_AINLINE void lj_state_checkstack(lua_State* L, MSize need)
{
   if ((mref<char>(L->maxstack) - (char*)L->top) <=
      (ptrdiff_t)need * (ptrdiff_t)sizeof(TValue))
      lj_state_growstack(L, need);
}

LJ_FUNC void lj_state_free(global_State* g, lua_State* L);

// Function name registry for tostring() support on named functions.
LJ_FUNC void lj_funcname_register(global_State* g, const GCproto* pt, const char* name, size_t len);
LJ_FUNC const char* lj_funcname_lookup(global_State* g, const GCproto* pt, size_t* len);

#define LJ_ALLOCF_INTERNAL   ((lua_Alloc)(void *)(uintptr_t)(1237<<4))
