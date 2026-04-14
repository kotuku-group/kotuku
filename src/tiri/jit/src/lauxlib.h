// $Id: lauxlib.h,v 1.88.1.1 2007/12/27 13:02:25 roberto Exp $
// Auxiliary functions for building Lua libraries
// See Copyright Notice in lua.h

#pragma once

#include <cstddef>
#include <cstdio>
#include <kotuku/system/errors.h>
#include "lua.h"
#include "lj_obj.h"

// extra error code for `luaL_load'
constexpr int LUA_ERRFILE = (LUA_ERRERR+1);

struct luaL_Reg {
  const char *name;
  lua_CFunction func;
};

extern void luaL_openlib(lua_State *, const char *, const luaL_Reg *, int);
extern void luaL_register(lua_State *, const char *, const luaL_Reg *);
extern int luaL_getmetafield(lua_State *, int, const char *);
extern int luaL_callmeta(lua_State *, int, const char *);
extern int luaL_typerror(lua_State *, int, const char *);
extern int luaL_argerror(lua_State *, int, const char *);
extern const char * luaL_checklstring(lua_State *, int, size_t * = nullptr);
extern uint32_t luaL_checkstringhash(lua_State *, int);
extern const char * luaL_optlstring(lua_State *, int, const char *, size_t * = nullptr);
extern lua_Number luaL_checknumber(lua_State *, int);
extern lua_Number luaL_optnumber(lua_State *, int, lua_Number);
extern lua_Integer luaL_checkinteger(lua_State *, int);
extern lua_Integer luaL_optinteger(lua_State *, int, lua_Integer);
extern void luaL_checkstack(lua_State *, int, const char *);
extern void luaL_checktype(lua_State *, int, int);
extern void luaL_checkany(lua_State *, int);
extern int  luaL_newmetatable(lua_State *, const char *);
extern void * luaL_checkudata(lua_State *, int ud, const char *);
extern void luaL_where(lua_State *, int lvl);
[[noreturn]] extern void luaL_error(lua_State *, const char *fmt, ...);
[[noreturn]] extern void luaL_error(lua_State *, ERR);
[[noreturn]] extern void luaL_error(lua_State *, ERR, const char *, ...);
extern int luaL_checkoption(lua_State *, int, const char *, const char *const lst[]);
extern TValue * resolve_index(lua_State *L, int);

// pre-defined references
constexpr int LUA_NOREF = -2;
constexpr int LUA_REFNIL = -1;

extern int luaL_ref(lua_State *, int t);
extern void luaL_unref(lua_State *, int t, int ref);
extern lua_State * luaL_newstate(class objScript *);
extern const char * luaL_gsub(lua_State *, const char *s, const char *p, const char *r);
extern const char * luaL_findtable(lua_State *, int idx, const char *fname, int szhint);
extern void luaL_traceback (lua_State *, lua_State *L1, const char *msg, int level);
extern void luaL_setfuncs(lua_State *, const luaL_Reg *l, int nup);
extern void luaL_pushmodule(lua_State *, const char *modname, int sizehint);
extern void *luaL_testudata(lua_State *, int ud, const char *tname);
extern void luaL_setmetatable(lua_State *, const char *tname);

inline void luaL_argcheck(lua_State *L, bool Cond, int NumArg, const char *ExtraMsg) {
   if (not Cond) luaL_argerror(L, NumArg, ExtraMsg);
}

inline const char * luaL_checkstring(lua_State *L, int N) { return luaL_checklstring(L, N, nullptr); }
inline const char * luaL_optstring(lua_State *L, int N, const char *D) { return luaL_optlstring(L, N, D, nullptr); }
inline int luaL_checkint(lua_State *L, int N) { return int(luaL_checkinteger(L, N)); }
inline int luaL_optint(lua_State *L, int N, int D) { return int(luaL_optinteger(L, N, D)); }
inline long luaL_checklong(lua_State *L, int N) { return long(luaL_checkinteger(L, N)); }
inline long luaL_optlong(lua_State *L, int N, long D) { return long(luaL_optinteger(L, N, D)); }
inline const char * luaL_typename(lua_State *L, int I) { return lua_typename(L, lua_type(L, I)); }
inline void luaL_getmetatable(lua_State *L, const char *N) { lua_getfield(L, LUA_REGISTRYINDEX, N); }

template<typename F, typename T>
inline auto luaL_opt(lua_State *L, F Func, int N, T Default) { return lua_isnoneornil(L, N) ? Default : Func(L, N); }

template<std::size_t N>
inline void luaL_newlibtable(lua_State *L, const luaL_Reg (&Lib)[N]) { lua_createtable(L, 0, N - 1); }

template<std::size_t N>
inline void luaL_newlib(lua_State *L, const luaL_Reg (&Lib)[N]) {
   luaL_newlibtable(L, Lib);
   luaL_setfuncs(L, Lib, 0);
}

struct luaL_Buffer {
  char *p;  // current position in buffer
  int lvl;  // number of strings in the stack (level)
  lua_State *L;
  char buffer[LUAL_BUFFERSIZE];
};

LUALIB_API void luaL_buffinit(lua_State *, luaL_Buffer *B);
LUALIB_API char * luaL_prepbuffer(luaL_Buffer *B);

inline void luaL_addchar(luaL_Buffer *B, char C) {
   if (B->p >= (B->buffer + LUAL_BUFFERSIZE)) luaL_prepbuffer(B);
   *(B->p++) = C;
}

// compatibility only

inline void luaL_putchar(luaL_Buffer *B, char C) { luaL_addchar(B, C); }
inline void luaL_addsize(luaL_Buffer *B, int N) { B->p += N; }

extern void luaL_addlstring(luaL_Buffer *B, const char *s, std::size_t l);
extern void luaL_addstring(luaL_Buffer *B, const char *s);
extern void luaL_addvalue(luaL_Buffer *B);
extern void luaL_pushresult(luaL_Buffer *B);
