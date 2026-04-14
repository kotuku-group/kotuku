// Library function support.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_obj.h"
#include <kotuku/main.h>

// A fallback handler is called by the assembler VM if the fast path fails:
//
// - too few arguments:   unrecoverable.
// - wrong argument type: recoverable, if coercion succeeds.
// - bad argument value:  unrecoverable.
// - stack overflow:      recoverable, if stack reallocation succeeds.
// - extra handling:      recoverable.
//
// The unrecoverable cases throw an error with lj_err_arg(), lj_err_argtype(), lj_err_caller() or lj_err_callermsg().
// The recoverable cases return 0 or the number of results + 1.
// The assembler VM retries the fast path only if 0 is returned.
// This time the fallback must not be called again or it gets stuck in a loop.

// Return values from fallback handler.

constexpr int FFH_RETRY = 0;
#define FFH_UNREACHABLE   FFH_RETRY
#define FFH_RES(n)   ((n)+1)
constexpr int FFH_TAILCALL = (-1);

extern TValue * lj_lib_checkany(lua_State *, int);
extern GCstr * lj_lib_checkstr(lua_State *, int);
extern GCstr * lj_lib_optstr(lua_State *, int);
#if LJ_DUALNUM
extern void lj_lib_checknumber(lua_State *, int);
#else
#define lj_lib_checknumber(L, narg)   lj_lib_checknum((L), (narg))
#endif
extern lua_Number  lj_lib_checknum(lua_State *, int);
extern int32_t     lj_lib_checkint(lua_State *, int);
extern int32_t     lj_lib_optint(lua_State *, int, int32_t def);
extern GCfunc *    lj_lib_checkfunc(lua_State *, int);
extern GCtab *     lj_lib_checktab(lua_State *, int);
extern GCtab *     lj_lib_checktabornil(lua_State *, int);
extern int         lj_lib_checkopt(lua_State *, int, int def, const char* lst);
extern GCarray *   lj_lib_optarray(lua_State *L, int);
extern GCarray *   lj_lib_checkarray(lua_State *, int);
extern GCobject *  lj_lib_optobject(lua_State *L, int);
extern GCobject *  lj_lib_checkobject(lua_State *, int);

// Avoid including lj_frame.h.
#define lj_lib_upvalue(L, n) (&gcval(L->base-2)->fn.c.upvalue[(n)-1])

// Fast object retrieval - only use for +ve arguments that are CONFIRMED objects (i.e. type checked).
// Otherwise use lua_toobject()

inline GCobject * lj_get_object_fast(lua_State *L, int Arg) {
   TValue *o;
   lj_assertL(Arg > 0, "Argument %d out of range", Arg);
   o = L->base + Arg - 1;
   lj_assertL(o < L->top, "Argument %d out of range", Arg);
   return objectV(o);
}

#if LJ_TARGET_WINDOWS
#define lj_lib_checkfpu(L) do { setnumV(L->top++, (lua_Number)1437217655); if (lua_tointeger(L, -1) != 1437217655) lj_err_caller(L, ErrMsg::BADFPU); L->top--; } while (0)
#else
#define lj_lib_checkfpu(L)   UNUSED(L)
#endif

extern GCfunc* lj_lib_pushcc(lua_State *, lua_CFunction f, int id, int n);
#define lj_lib_pushcf(L, fn, id)   (lj_lib_pushcc(L, (fn), (id), 0))

// Library function declarations. Scanned by buildvm.
#define LJLIB_CF(name)      static int lj_cf_##name(lua_State *L)
#define LJLIB_ASM(name)      static int lj_ffh_##name(lua_State *L)
#define LJLIB_ASM_(name)
#define LJLIB_LUA(name)
#define LJLIB_SET(name)
#define LJLIB_PUSH(arg)
#define LJLIB_REC(handler)
#define LJLIB_NOREGUV
#define LJLIB_NOREG

#define LJ_LIB_REG(L, regname, name) lj_lib_register(L, regname, lj_lib_init_##name, lj_lib_cf_##name)

extern void lj_lib_register(lua_State *, const char* libname, const uint8_t* init, const lua_CFunction* cf);
extern void lj_lib_prereg(lua_State *, const char* name, lua_CFunction f, GCtab* env);
extern int lj_lib_postreg(lua_State *, lua_CFunction cf, int id, const char* name);

// Library init data tags.
#define LIBINIT_LENMASK   0x3f
#define LIBINIT_TAGMASK   0xc0
#define LIBINIT_CF        0x00
#define LIBINIT_ASM       0x40
#define LIBINIT_ASM_      0x80
#define LIBINIT_STRING    0xc0
#define LIBINIT_MAXSTR    0x38
#define LIBINIT_LUA       0xf9
#define LIBINIT_SET       0xfa
#define LIBINIT_NUMBER    0xfb
#define LIBINIT_COPY      0xfc
#define LIBINIT_LASTCL    0xfd
#define LIBINIT_FFID      0xfe
#define LIBINIT_END       0xff

// Conversion for arrays that originate from outside Tiri

inline AET ff_to_element(int Flags) {
   if (Flags & FD_CPP) {
      if (Flags & FD_STRING)  return AET::STR_CPP;
   }
   else {
      if (Flags & FD_BYTE)    return AET::BYTE;
      if (Flags & FD_WORD)    return AET::INT16;
      if (Flags & FD_INT)     return AET::INT32;
      if (Flags & FD_INT64)   return AET::INT64;
      if (Flags & FD_FLOAT)   return AET::FLOAT;
      if (Flags & FD_DOUBLE)  return AET::DOUBLE;
      if (Flags & FD_STRING)  return AET::CSTR;
      if (Flags & FD_POINTER) return AET::PTR;
      if (Flags & FD_STRUCT)  return AET::STRUCT;
   }
   return AET::MAX;
}
