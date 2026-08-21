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

// Append a single value to an array (array.append() semantics), called from JIT traces

extern "C" void lj_arr_push1(lua_State *, GCarray *, cTValue *);

extern "C" void lj_arr_putstr(lua_State *, GCarray *, GCstr *);

extern "C" void lj_arr_putsbuf(lua_State *, GCarray *, SBuf *);

extern "C" void lj_arr_putnumtv(lua_State *, GCarray *, cTValue *);

// Clear an array from JIT traces.

extern "C" void lj_arr_clear(lua_State *, GCarray *);

// Resize an array from JIT traces.

extern "C" int32_t lj_arr_resize(lua_State *, GCarray *, int32_t);

// Extract a string from a byte array in JIT traces.

extern "C" GCstr * lj_arr_getstring(lua_State *, GCarray *, int32_t, int32_t);

// Create an array from JIT traces.

extern "C" GCarray * lj_arr_new_jit(lua_State *, uint32_t, uint32_t);

// Test whether a resolved __contains value is the canonical array provider.

extern "C" [[nodiscard]] int32_t lj_arr_is_contains_handler(cTValue *);

// Search an array using the canonical membership rules. Returns exactly zero or one.

extern "C" [[nodiscard]] int32_t lj_arr_contains(lua_State *, GCarray *, cTValue *);
extern "C" [[nodiscard]] int32_t lj_arr_contains_num(GCarray *, lua_Number);
extern "C" [[nodiscard]] int32_t lj_arr_contains_str(GCarray *, GCstr *);
LJ_NOAPI [[nodiscard]] int32_t lj_arr_find_num(GCarray *, lua_Number, int32_t, int32_t, int32_t);
LJ_NOAPI [[nodiscard]] int32_t lj_arr_find_str(GCarray *, GCstr *, int32_t, int32_t, int32_t);
LJ_NOAPI [[nodiscard]] int32_t lj_arr_find_object(GCarray *, OBJECTID, int32_t, int32_t, int32_t);

// Safe array get - returns nil for out-of-bounds instead of throwing error
// Used by safe navigation operator (?[]) on arrays

extern "C" void lj_arr_safe_getidx(lua_State *, GCarray *, int32_t, TValue *);
