/*
** $Id: lua.h,v 1.218.1.5 2008/08/06 13:30:12 roberto Exp $
** Lua - An Extensible Extension Language
** Lua.org, PUC-Rio, Brazil (https://www.lua.org)
** See Copyright Notice at the end of this file
*/

#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <cstdint>
#include <string_view>

#include "luaconf.h"

// mark for precompiled code (`<esc>Lua')
#define   LUA_SIGNATURE   "\033Lua"

// option for multiple returns in `lua_pcall' and `lua_call'
constexpr int LUA_MULTRET = -1;

// pseudo-indices

constexpr int LUA_REGISTRYINDEX = -10000;
constexpr int LUA_ENVIRONINDEX = -10001;
constexpr int LUA_GLOBALSINDEX = -10002;

constexpr int lua_upvalueindex(int I) {
   return LUA_GLOBALSINDEX - I;
}

// thread status

constexpr int LUA_OK = 0;
constexpr int LUA_YIELD = 1;
constexpr int LUA_ERRRUN = 2;
constexpr int LUA_ERRSYNTAX = 3;
constexpr int LUA_ERRMEM = 4;
constexpr int LUA_ERRERR = 5;

typedef struct lua_State lua_State;

using lua_CFunction = int(*)(lua_State *L);

// functions that read/write blocks when loading/dumping Lua chunks

using lua_Reader = const char*(*)(lua_State *L, void *ud, size_t *sz);
using lua_Writer = int(*)(lua_State *L, const void* p, size_t sz, void* ud);

// prototype for memory-allocation functions

using lua_Alloc = void*(*)(void *ud, void *ptr, size_t osize, size_t nsize);

// Basic types. These also exist as LJ_* types in lj_obj.h but are not perfectly mirrored.

constexpr int LUA_TNONE = -1;
constexpr int LUA_TNIL = 0;
constexpr int LUA_TBOOLEAN = 1;
constexpr int LUA_TLIGHTUSERDATA = 2;
constexpr int LUA_TNUMBER = 3;
constexpr int LUA_TSTRING = 4;
constexpr int LUA_TTABLE = 5;
constexpr int LUA_TFUNCTION = 6;
constexpr int LUA_TUSERDATA = 7;
constexpr int LUA_TTHREAD = 8;
constexpr int LUA_TPROTO = 9;
constexpr int LUA_TOBJECT = 10;
constexpr int LUA_TARRAY = 11;

// minimum Lua stack available to a C function
constexpr int LUA_MINSTACK = 20;

// generic extra include file

#if defined(LUA_USER_H)
#include LUA_USER_H
#endif

// type of numbers in Lua
using lua_Number = LUA_NUMBER;

// type for integer functions
using lua_Integer = LUA_INTEGER;

// state manipulation

extern lua_State *(lua_newstate) (lua_Alloc f, void *ud);
extern void       (lua_close) (lua_State *L);

extern lua_CFunction (lua_atpanic) (lua_State *L, lua_CFunction panicf);

// basic stack manipulation

extern int   (lua_gettop) (lua_State *L);
extern void  (lua_settop) (lua_State *L, int idx);
extern void  (lua_pushvalue) (lua_State *L, int idx);
extern void  (lua_remove) (lua_State *L, int idx);
extern void  (lua_insert) (lua_State *L, int idx);
extern void  (lua_rotate) (lua_State *L, int idx, int n);
extern void  (lua_replace) (lua_State *L, int idx);
extern int   (lua_checkstack) (lua_State *L, int sz);
extern void  (lua_xmove) (lua_State *from, lua_State *to, int n);

// access functions (stack -> C)

extern int (lua_isnumber) (lua_State *L, int idx);
extern int (lua_isstring) (lua_State *L, int idx);
extern int (lua_iscfunction) (lua_State *L, int idx);
extern int (lua_isuserdata) (lua_State *L, int idx);
extern int (lua_type) (lua_State *L, int idx);
extern const char *(lua_typename) (lua_State *L, int tp);

extern int (lua_equal) (lua_State *L, int idx1, int idx2);
extern int (lua_rawequal) (lua_State *L, int idx1, int idx2);
extern int (lua_lessthan) (lua_State *L, int idx1, int idx2);

struct GCarray;
struct GCobject;

extern GCarray *     lua_toarray(lua_State *, int);
extern lua_Number    lua_tonumber(lua_State *, int);
extern lua_Integer   lua_tointeger(lua_State *, int);
extern int           lua_toboolean(lua_State *, int);
extern const char *  lua_tolstring(lua_State *, int, size_t *);
extern GCobject *    lua_toobject(lua_State *, int);
extern GCobject *    lua_optobject(lua_State *, int);
extern size_t        lua_objlen(lua_State *, int);
extern lua_CFunction lua_tocfunction(lua_State *, int);
extern void *        lua_touserdata(lua_State *, int);
extern lua_State *   lua_tothread(lua_State *, int);
extern const void *  lua_topointer(lua_State *, int);

// push functions (C -> stack)

extern void  (lua_pushnil) (lua_State *L);
extern void  (lua_pushnumber) (lua_State *L, lua_Number n);
extern void  (lua_pushinteger) (lua_State *L, lua_Integer n);
extern void  (lua_pushlstring) (lua_State *L, const char *s, size_t l);
extern void  (lua_pushstring) (lua_State *L, const char *s);
extern const char *(lua_pushvfstring) (lua_State *L, const char *fmt, va_list argp);
extern const char *(lua_pushfstring) (lua_State *L, const char *fmt, ...);
extern void  (lua_pushcclosure) (lua_State *L, lua_CFunction fn, int n);
extern void  (lua_pushboolean) (lua_State *L, int b);
extern void  (lua_pushlightuserdata) (lua_State *L, void *p);
extern int   (lua_pushthread) (lua_State *L);

// get functions (Lua -> stack)

enum class AET : uint8_t;

extern void   lua_gettable(lua_State *L, int idx);
extern void   lua_getfield(lua_State *L, int idx, const char *k);
extern void   lua_rawget(lua_State *L, int idx);
extern void   lua_rawgeti(lua_State *L, int idx, int n);
extern void   lua_createtable(lua_State *L, int narr, int nrec);
extern void   lua_createarray(lua_State *L, uint32_t Length, AET Type, void *Data = nullptr, uint8_t Flags = 0, std::string_view StructName = {});
extern void * lua_newuserdata(lua_State *L, size_t sz);

// Native Kōtuku object support
struct GCobject;
struct Object;
class objMetaClass;
extern GCobject * lua_pushobject(lua_State *L, int32_t UID, Object *Ptr, objMetaClass *ClassPtr, uint8_t Flags);
extern int    lua_getmetatable(lua_State *L, int objindex);
extern void   lua_getfenv(lua_State *L, int idx);

// set functions (stack -> Lua)

extern void  (lua_settable) (lua_State *L, int idx);
extern void  (lua_setfield) (lua_State *L, int idx, const char *k);
extern void  (lua_rawset) (lua_State *L, int idx);
extern void  (lua_rawseti) (lua_State *L, int idx, int n);
extern int   (lua_setmetatable) (lua_State *L, int objindex);
extern void  (lua_setbasemetatable) (lua_State *L, uint32_t itype);  // Set base metatable for a type
extern int   (lua_setfenv) (lua_State *L, int idx);

// `load' and `call' functions (load and run Lua code)

extern void lua_call(lua_State *L, int nargs, int nresults);
extern int  lua_pcall(lua_State *L, int nargs, int nresults, int errfunc);
extern int  lua_cpcall(lua_State *L, lua_CFunction func, void *ud);
extern int  lua_load(lua_State *L, std::string_view, const char *chunk_name);
extern int  lua_dump(lua_State *L, lua_Writer writer, void *data);

// Garbage collection function and options

constexpr int LUA_GCSTOP = 0;
constexpr int LUA_GCRESTART = 1;
constexpr int LUA_GCCOLLECT = 2;
constexpr int LUA_GCCOUNT = 3;
constexpr int LUA_GCCOUNTB = 4;
constexpr int LUA_GCSTEP = 5;
constexpr int LUA_GCSETPAUSE = 6;
constexpr int LUA_GCSETSTEPMUL = 7;
constexpr int LUA_GCISRUNNING = 9;

extern int (lua_gc) (lua_State *L, int what, int data = 0);

extern int   (lua_error) (lua_State *L);
extern int   (lua_next) (lua_State *L, int idx);
extern void  (lua_concat) (lua_State *L, int n);
extern lua_Alloc (lua_getallocf) (lua_State *L, void **ud);
extern void lua_setallocf (lua_State *L, lua_Alloc f, void *ud);

inline void lua_pop(lua_State *L, int N) { lua_settop(L, -(N)-1); }
inline void lua_newtable(lua_State *L) { lua_createtable(L, 0, 0); }
inline void lua_pushcfunction(lua_State *L, lua_CFunction F) { lua_pushcclosure(L, F, 0); }
inline void lua_setglobal(lua_State *L, const char *S) { lua_setfield(L, LUA_GLOBALSINDEX, S); }

inline void lua_register(lua_State *L, const char *N, lua_CFunction F) {
   lua_pushcfunction(L, F);
   lua_setglobal(L, N);
}

inline size_t lua_strlen(lua_State *L, int I) { return lua_objlen(L, I); }
inline bool lua_isfunction(lua_State *L, int N) { return lua_type(L, N) == LUA_TFUNCTION; }
inline bool lua_istable(lua_State *L, int N) { return lua_type(L, N) == LUA_TTABLE; }
inline bool lua_isobject(lua_State *L, int N) { return lua_type(L, N) == LUA_TOBJECT; }
inline bool lua_islightuserdata(lua_State *L, int N) { return lua_type(L, N) == LUA_TLIGHTUSERDATA; }
inline bool lua_isnil(lua_State *L, int N) { return lua_type(L, N) == LUA_TNIL; }
inline bool lua_isboolean(lua_State *L, int N) { return lua_type(L, N) == LUA_TBOOLEAN; }
inline bool lua_isthread(lua_State *L, int N) { return lua_type(L, N) == LUA_TTHREAD; }
inline bool lua_isarray(lua_State *L, int N) { return lua_type(L, N) == LUA_TARRAY; }
inline bool lua_isnone(lua_State *L, int N) { return lua_type(L, N) == LUA_TNONE; }
inline bool lua_isnoneornil(lua_State *L, int N) { return lua_type(L, N) <= 0; }

template<size_t N>
inline void lua_pushliteral(lua_State *L, const char (&S)[N]) {
   lua_pushlstring(L, S, N - 1);
}

inline void lua_getglobal(lua_State *L, const char *S) {
   lua_getfield(L, LUA_GLOBALSINDEX, S);
}

inline const char *lua_tostring(lua_State *L, int I) {
   return lua_tolstring(L, I, nullptr);
}

// compatibility macros and inline functions

#define lua_open()   luaL_newstate()

inline void lua_getregistry(lua_State *L) {
   lua_pushvalue(L, LUA_REGISTRYINDEX);
}

inline int lua_getgccount(lua_State *L) {
   return lua_gc(L, LUA_GCCOUNT, 0);
}

#define lua_Chunkreader      lua_Reader
#define lua_Chunkwriter      lua_Writer

// hack
extern void lua_setlevel   (lua_State *from, lua_State *to);

// Event codes

#define LUA_HOOKCALL   0
#define LUA_HOOKRET   1
#define LUA_HOOKLINE   2
#define LUA_HOOKCOUNT   3
#define LUA_HOOKTAILRET 4

// Event masks
#define LUA_MASKCALL   (1 << LUA_HOOKCALL)
#define LUA_MASKRET   (1 << LUA_HOOKRET)
#define LUA_MASKLINE   (1 << LUA_HOOKLINE)
#define LUA_MASKCOUNT   (1 << LUA_HOOKCOUNT)

// Activation record
using lua_Debug = struct lua_Debug;

// Functions to be called by the debuger in specific events
using lua_Hook = void(*)(lua_State *L, lua_Debug *ar);

extern int lua_getstack (lua_State *L, int level, lua_Debug *ar);
extern int lua_getinfo (lua_State *L, const char *what, lua_Debug *ar);
extern const char *lua_getlocal (lua_State *L, const lua_Debug *ar, int n);
extern const char *lua_setlocal (lua_State *L, const lua_Debug *ar, int n);
extern const char *lua_getupvalue (lua_State *L, int funcindex, int n);
extern const char *lua_setupvalue (lua_State *L, int funcindex, int n);
extern int lua_sethook (lua_State *L, lua_Hook func, int mask, int count);
extern lua_Hook lua_gethook (lua_State *L);
extern int lua_gethookmask (lua_State *L);
extern int lua_gethookcount (lua_State *L);
extern void *lua_upvalueid (lua_State *L, int idx, int n);
extern void lua_upvaluejoin (lua_State *L, int idx1, int n1, int idx2, int n2);
extern void lua_copy (lua_State *L, int fromidx, int toidx);
extern lua_Number lua_tonumberx (lua_State *L, int idx, int *isnum);
extern lua_Integer lua_tointegerx (lua_State *L, int idx, int *isnum);

struct lua_Debug {
  int event;
  const char *name;      // (n)
  const char *namewhat;  // (n) `global', `local', `field', `method'
  const char *what;      // (S) `Lua', `C', `main', `tail'
  const char *source;    // (S)
  int currentline;       // (l)
  int nups;              // (u) number of upvalues
  int linedefined;       // (S)
  int lastlinedefined;   // (S)
  char short_src[LUA_IDSIZE]; //  (S)
  // private part
  int i_ci;  //  active function
};

/******************************************************************************
* Copyright (C) 1994-2008 Lua.org, PUC-Rio.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/
