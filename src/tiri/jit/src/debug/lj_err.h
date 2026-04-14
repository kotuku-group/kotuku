// Error handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include <stdarg.h>
#include "lj_obj.h"

enum class ErrMsg : unsigned int {
#define ERRDEF(name, msg) \
  name, name##_ = name + sizeof(msg)-1,
#include "lj_errmsg.h"
  _MAX
};

LJ_DATA const char *lj_err_allmsg;

[[nodiscard]] inline const char* err2msg(ErrMsg em) noexcept {
   return lj_err_allmsg + int(em);
}

LJ_FUNC GCstr *lj_err_str(lua_State *L, ErrMsg em);
LJ_FUNCA_NORET void  lj_err_throw(lua_State *L, int errcode);
LJ_FUNC_NORET void lj_err_mem(lua_State *L);
LJ_FUNC_NORET void lj_err_run(lua_State *L);
LJ_FUNCA_NORET void lj_err_trace(lua_State *L, int errcode);
LJ_FUNC_NORET void lj_err_msg(lua_State *L, ErrMsg em);
LJ_FUNC_NORET void lj_err_msgv(lua_State *L, ErrMsg em, ...);
LJ_FUNC_NORET void lj_err_lex(lua_State *L, GCstr *src, const char *tok, BCLine line, ErrMsg em, va_list argp);
LJ_FUNC_NORET void lj_err_optype(lua_State *L, cTValue *o, ErrMsg opm);
LJ_FUNC_NORET void lj_err_comp(lua_State *L, cTValue *o1, cTValue *o2);
LJ_FUNC_NORET void lj_err_optype_call(lua_State *L, TValue *o);
LJ_FUNC_NORET void lj_err_callermsg(lua_State *L, const char *msg);
LJ_FUNC_NORET void lj_err_callerv(lua_State *L, ErrMsg em, ...);
LJ_FUNC_NORET void lj_err_caller(lua_State *L, ErrMsg em);
LJ_FUNC_NORET void lj_err_arg(lua_State *L, int narg, ErrMsg em);
LJ_FUNC_NORET void lj_err_argv(lua_State *L, int narg, ErrMsg em, ...);
LJ_FUNC_NORET void lj_err_argtype(lua_State *L, int narg, const char *xname);
LJ_FUNC_NORET void lj_err_argt(lua_State *L, int narg, int tt);
LJ_FUNC_NORET void lj_err_assigntype(lua_State *L, int slot, const char *expected_type);

#if LJ_UNWIND_JIT and not LJ_ABI_WIN
LJ_FUNC uint8_t *lj_err_register_mcode(void *base, size_t sz, uint8_t *info);
LJ_FUNC void lj_err_deregister_mcode(void *base, size_t sz, uint8_t *info);
#else
[[nodiscard]] inline uint8_t *lj_err_register_mcode([[maybe_unused]] void *base, [[maybe_unused]] size_t sz, uint8_t *info) noexcept {
   return info;
}
inline void lj_err_deregister_mcode([[maybe_unused]] void *base, [[maybe_unused]] size_t sz, [[maybe_unused]] uint8_t *info) noexcept {}
#endif

#if LJ_UNWIND_EXT && !LJ_ABI_WIN && defined(LUA_USE_ASSERT)
LJ_FUNC void lj_err_verify();
#else
inline void lj_err_verify() noexcept {}
#endif

// Try-except exception handling runtime functions (defined in tiri_functions.cpp)
// Note: lj_try_enter and lj_try_leave are called from VM bytecode handlers and JIT code.
// Other functions (lj_try_find_handler, lj_try_build_exception_table) are internal
// to lj_err.cpp and use Kotuku's ERR type, so they're declared there.
extern "C" void lj_try_enter(lua_State *L, GCfunc *Func, TValue *Base, uint16_t TryBlockIndex);
extern "C" void lj_try_leave(lua_State *L);
extern "C" void lj_try_cleanup_to_base(lua_State *L, TValue *TargetBase);
