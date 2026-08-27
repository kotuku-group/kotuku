// Native array handling.
// Copyright (C) 2025-2026 Paul Manias

#pragma once

#include "lj_obj.h"

struct ArrayAllocationDescriptor {
   AET storage = AET::ANY;
   struct_record *struct_def = nullptr;
   GCstr *nested_identity = nullptr;
};

extern GCarray * lj_array_new(lua_State *, uint32_t, AET, void *Data = nullptr, uint8_t Flags = 0,
   std::string_view StructName = {}, struct_record *StructDef = nullptr, GCstr *NestedIdentity = nullptr);
extern GCarray * lj_array_new(lua_State *, uint32_t, const ArrayAllocationDescriptor &, void *Data = nullptr,
   uint8_t Flags = 0);
[[nodiscard]] extern bool lj_array_identity_accepts(const GCarray *, const GCarray *);
[[nodiscard]] extern bool lj_array_identity_matches(const GCarray *, std::string_view);
[[nodiscard]] extern bool lj_array_member_identity_matches(const GCarray *, std::string_view);
[[nodiscard]] extern GCarray * lj_array_new_like(lua_State *, const GCarray *, uint32_t);
extern void lj_array_free(global_State *, GCarray *);
[[nodiscard]] extern uint8_t lj_array_elemsize(AET);
[[nodiscard]] extern CSTRING lj_array_elemtype_name(AET) noexcept;
[[nodiscard]] extern bool lj_array_struct_is_trivial(const struct_record &Def);
extern void lj_array_clear_range(GCarray *, MSize Start, MSize Count);
extern void lj_array_copy(lua_State *, GCarray *, uint32_t DstIdx, GCarray *, uint32_t SrcIdx, uint32_t Count);
extern void lj_array_copy_unchecked(lua_State *, GCarray *, uint32_t DstIdx, const GCarray *, uint32_t SrcIdx,
   uint32_t Count);
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
[[nodiscard]] extern GCarray *lj_array_new_map_result(lua_State *, MSize Length, int TypeArg);

//********************************************************************************************************************

inline void * lj_array_index(GCarray *Array, uint32_t Idx) {
   return (int8_t*)Array->arraydata() + (Idx * Array->elemsize);
}

inline const void * lj_array_index(const GCarray *Array, uint32_t Idx) {
   return (const int8_t*)Array->arraydata() + (Idx * Array->elemsize);
}
