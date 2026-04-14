// Native array handling.
// Copyright (C) 2025-2026 Paul Manias

#pragma once

#include "lj_obj.h"

extern GCarray * lj_array_new(lua_State *, uint32_t, AET, void *Data = nullptr, uint8_t Flags = 0, std::string_view StructName = {});
extern void lj_array_free(global_State *, GCarray *);
[[nodiscard]] extern uint8_t lj_array_elemsize(AET);
extern void lj_array_copy(lua_State *, GCarray *, uint32_t dstidx, GCarray *, uint32_t srcidx, uint32_t count);
extern GCtab* lj_array_to_table(lua_State *, GCarray *);
extern bool lj_array_grow(lua_State *, GCarray *, MSize MinCapacity);

//********************************************************************************************************************

inline void * lj_array_index(GCarray *Array, uint32_t Idx) {
   return (int8_t*)Array->arraydata() + (Idx * Array->elemsize);
}
