// Native array handling.
// Copyright (C) 2025-2026 Paul Manias

#pragma once

#include "lj_obj.h"

extern GCarray * lj_array_new(lua_State *, uint32_t, AET, void *Data = nullptr, uint8_t Flags = 0,
   std::string_view StructName = {}, struct_record *StructDef = nullptr, Object *Lifecycle = nullptr);
extern void lj_array_free(global_State *, GCarray *);
[[nodiscard]] extern bool lj_array_stale(GCarray *);
extern void lj_array_check_lifecycle(lua_State *, GCarray *);
extern "C" [[nodiscard]] uint32_t lj_array_length(lua_State *, GCarray *);
[[nodiscard]] extern uint8_t lj_array_elemsize(AET);
[[nodiscard]] extern bool lj_array_struct_is_trivial(const struct_record &Def);
extern void lj_array_clear_range(GCarray *, MSize Start, MSize Count);
extern void lj_array_copy(lua_State *, GCarray *, uint32_t dstidx, GCarray *, uint32_t srcidx, uint32_t count);
extern GCtab* lj_array_to_table(lua_State *, GCarray *);
extern bool lj_array_grow(lua_State *, GCarray *, MSize MinCapacity);

enum class ArrayElementResult : uint8_t {
   OK,
   INVALID_TYPE,
   OUT_OF_RANGE,
   UNSUPPORTED_STORAGE
};

[[nodiscard]] extern ArrayElementResult lj_array_validate_element(GCarray *, cTValue *);
extern void lj_array_check_element(lua_State *, GCarray *, cTValue *);
extern void lj_array_store_validated(lua_State *, GCarray *, MSize Index, cTValue *);
extern void lj_array_store_checked(lua_State *, GCarray *, MSize Index, cTValue *);
extern void lj_array_store_new_values(lua_State *, GCarray *, cTValue *Values, MSize Count);

//********************************************************************************************************************

inline void * lj_array_index(GCarray *Array, uint32_t Idx) {
   return (int8_t*)Array->arraydata() + (Idx * Array->elemsize);
}

// Resolve a TValue known to hold an array, enforcing the lifecycle guard before any storage access.  This is the
// mandatory entry point for operator, iterator and marshalling paths that resolve arrayV() on a raw value directly
// (i.e. those that bypass lj_lib_checkarray()).  A stale view is emptied and raises before its freed storage can be
// read.  The caller must have already confirmed tvisarray(O).
inline GCarray * lj_array_checked(lua_State *L, cTValue *O) {
   GCarray *arr = &gcval(O)->arr;
   if (arr->flags & ARRAY_LIFECYCLE) lj_array_check_lifecycle(L, arr);
   return arr;
}
