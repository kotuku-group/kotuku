// Metamethod handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_obj.h"

// Metamethod handling
LJ_FUNC void lj_meta_init(lua_State* L);
LJ_FUNC [[nodiscard]] cTValue* lj_meta_cache(GCtab* mt, MMS mm, GCstr* name);
LJ_FUNC [[nodiscard]] cTValue* lj_meta_lookup(lua_State* L, cTValue* o, MMS mm);
LJ_FUNC [[nodiscard]] GCstr* lj_meta_type_name(lua_State* L, cTValue* Value) noexcept;
LJ_FUNC [[nodiscard]] const char* lj_meta_display_name(lua_State* L, cTValue* Value) noexcept;
LJ_FUNC void lj_meta_raw_type_text(lua_State *L, cTValue *Value, char *Buffer, size_t Size) noexcept;
LJ_FUNC [[nodiscard]] GCstr* lj_meta_raw_type_name(lua_State *L, cTValue *Value) noexcept;

[[nodiscard]] inline cTValue* lj_meta_fastg(global_State* g, GCtab* mt, MMS mm) noexcept
{
   return mt IS nullptr ? nullptr : ((mt)->nomm & (1u << (mm))) ? nullptr :
      lj_meta_cache(mt, mm, mmname_str(g, mm));
}

[[nodiscard]] inline cTValue* lj_meta_fast(lua_State* L, GCtab* mt, MMS mm) noexcept
{
   return lj_meta_fastg(G(L), mt, mm);
}

// C helpers for some instructions, called from assembler VM.
extern "C" [[nodiscard]] cTValue* lj_meta_tget(lua_State* L, cTValue* o, cTValue* k);
extern "C" [[nodiscard]] TValue* lj_meta_tset(lua_State* L, cTValue* o, cTValue* k);
extern "C" [[nodiscard]] TValue* lj_meta_arith(lua_State* L, TValue* ra, cTValue* rb, cTValue* rc, BCREG op);
extern "C" [[nodiscard]] TValue* lj_meta_cat(lua_State* L, TValue* top, int left);
extern "C" [[nodiscard]] TValue* lj_meta_len(lua_State* L, cTValue* o);
extern "C" [[nodiscard]] TValue* lj_meta_len_result(lua_State* L, TValue* Result);
extern "C" [[nodiscard]] TValue* lj_meta_equal(lua_State* L, GCobj* o1, GCobj* o2, int ne);
extern "C" [[nodiscard]] TValue* lj_meta_equal_cd(lua_State* L, BCIns ins);
extern "C" [[nodiscard]] TValue* lj_meta_equal_thunk(lua_State* L, BCIns ins);
extern "C" [[nodiscard]] int lj_meta_isfalsey(lua_State* L, BCIns Ins);
extern "C" [[nodiscard]] TValue* lj_meta_comp(lua_State* L, cTValue* o1, cTValue* o2, int op);
extern "C" [[nodiscard]] TValue* lj_meta_contains(lua_State* L, cTValue* Candidate, cTValue* Target, int Inverted);
extern "C" void lj_meta_istype(lua_State* L, BCREG ra, BCREG tp);
extern "C" void lj_meta_contract(lua_State *, TValue *, uint32_t, GCstr *);
extern "C" void lj_meta_contract_pc(lua_State *, const BCIns *, uint32_t);
extern "C" void lj_meta_type_test_pc(lua_State *, const BCIns *);
[[nodiscard]] bool lj_meta_object_class_matches(const GCobject *, CLASSID) noexcept;
LJ_FUNC void lj_contract_build_cache(lua_State *, GCproto *);
extern "C" void lj_env_check(lua_State *, GCtab *, GCstr *, cTValue *);
extern "C" void lj_env_store(lua_State *, GCtab *, GCstr *, cTValue *);
extern "C" GCstr * lj_vm_envcheck(lua_State *, GCtab *, GCstr *, TValue *);
extern "C" int lj_meta_call(lua_State *L, TValue *func, TValue *top, bool TailTransfer);
extern "C" void lj_meta_for(lua_State* L, TValue* o);

// Helper for __close metamethod during scope exit. Returns error code (0 = success).
extern "C" int lj_meta_close(lua_State* L, TValue* o, TValue* err);
LJ_FUNC int lj_meta_defer(lua_State *L, TValue *OwnerBase, const DeferRegistration &Registration);
extern "C" void lj_meta_multres_save(lua_State *L, TValue *Base, uint32_t Count);
extern "C" uint32_t lj_meta_multres_restore(lua_State *L, TValue *Base);
extern "C" void lj_meta_multres_unwind(lua_State *L, TValue *TargetBase);
