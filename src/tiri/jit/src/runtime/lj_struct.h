// Native struct handling.
// Copyright (C) 2026 Paul Manias

#pragma once

#include "lj_obj.h"
#include "lj_bc.h"

struct struct_field;

extern GCstruct * lj_struct_new(lua_State *, struct struct_record &);
extern GCstruct * lj_struct_new_external(lua_State *, struct struct_record &, void *Data, uint8_t Flags,
   struct Object *Lifecycle = nullptr, GCstruct *Parent = nullptr);
extern void lj_struct_free(global_State *, GCstruct *);
extern bool lj_struct_stale(GCstruct *);

extern void lj_struct_check_lifecycle(lua_State *, GCstruct *, const char *);
extern void lj_struct_getfield_core(lua_State *, GCstruct *, struct_field &, void *);
extern void lj_struct_setfield_core(lua_State *, GCstruct *, struct_field &, void *);
extern void lj_struct_push_size_closure(lua_State *, GCstruct *);

// Fast path bytecode handlers for BC_STGETF and BC_STSETF.
// Ins points to the current instruction for inline caching (nullptr disables caching in JIT traces).

extern "C" void bc_struct_getfield(lua_State *, GCstruct *, GCstr *, TValue *, BCIns *);
extern "C" void bc_struct_setfield(lua_State *, GCstruct *, GCstr *, TValue *, BCIns *);
