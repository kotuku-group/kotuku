// Function handling (prototypes, functions and upvalues).
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_obj.h"

// Prototypes.
LJ_FUNC void lj_func_freeproto(global_State *g, GCproto *pt);

// Upvalues.
LJ_FUNCA void lj_func_closeuv(lua_State *L, TValue *level);
LJ_FUNC void lj_func_freeuv(global_State *g, GCupval *uv);

// Functions (closures).
LJ_FUNC [[nodiscard]] GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env);
LJ_FUNC [[nodiscard]] GCfunc *lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env);
LJ_FUNC [[nodiscard]] GCfunc *lj_func_newL_zero(lua_State *L, GCproto *Proto, GCtab *Environment);
LJ_FUNC [[nodiscard]] GCfunc *lj_func_newL_inherited(lua_State *L, GCproto *Proto, GCfuncL *Parent);
LJ_FUNC [[nodiscard]] GCfunc *lj_func_newL_local(lua_State *L, GCproto *Proto, GCfuncL *Parent, TValue *Base);
LJ_FUNCA [[nodiscard]] GCfunc *lj_func_newL_gc(lua_State *L, GCproto *pt, GCfuncL *parent);
LJ_FUNC void lj_func_free(global_State *g, GCfunc *c);

#ifdef UNIT_TESTS
// State-confined observation only; the allocator owns injection and the protected caller resets this after unwind.
struct ClosureAllocationProbe {
   lua_State *state = nullptr;
   GCproto *prototype = nullptr;
   int helper = 0; // 1: zero, 2: inherited, 3: local, 4: interpreter.
   int site = -1; // -1: function, otherwise the capture descriptor index.
   bool active = false;
};
LJ_FUNC ClosureAllocationProbe &lj_func_allocation_probe();
#endif
