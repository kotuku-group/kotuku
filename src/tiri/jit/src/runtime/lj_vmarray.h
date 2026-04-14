// Array helper functions for assembler VM.
// Copyright (C) 2025-2026 Paul Manias

#pragma once

#include "lj_obj.h"

// C helpers for array bytecodes, called from assembler VM.
// Returns TValue* result or nullptr if metamethod needs to be called.

extern "C" [[nodiscard]] cTValue * lj_arr_get(lua_State *, cTValue *, cTValue *);

// lj_arr_set returns 1 on success, 0 if metamethod needs to be called

extern "C" [[nodiscard]] int lj_arr_set(lua_State *, cTValue *, cTValue *, cTValue *);

// Direct array access helpers (no metamethod support, used after type check passes)

extern "C" void lj_arr_getidx(lua_State *, GCarray *, int32_t idx, TValue *);
extern "C" void lj_arr_setidx(lua_State *, GCarray *, int32_t idx, cTValue *);

// Safe array get - returns nil for out-of-bounds instead of throwing error
// Used by safe navigation operator (?[]) on arrays

extern "C" void lj_arr_safe_getidx(lua_State *, GCarray *, int32_t, TValue *);
