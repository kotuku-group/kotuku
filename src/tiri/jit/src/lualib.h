/*
** Standard library header.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lua.h"

#define LUA_FILEHANDLE   "FILE*"

constexpr const char * LUA_MATHLIBNAME = "math";
constexpr const char * LUA_STRLIBNAME  = "string";
constexpr const char * LUA_TABLIBNAME  = "table";
constexpr const char * LUA_DBLIBNAME   = "debug";
constexpr const char * LUA_BITLIBNAME  = "bit";
constexpr const char * LUA_JITLIBNAME  = "jit";
constexpr const char * LUA_FFILIBNAME  = "ffi";

extern int luaopen_base(lua_State *L);
extern int luaopen_math(lua_State *L);
extern int luaopen_string(lua_State *L);
extern int luaopen_table(lua_State *L);
extern int luaopen_debug(lua_State *L);
extern int luaopen_bit(lua_State *L);
extern int luaopen_jit(lua_State *L);
extern int luaopen_ffi(lua_State *L);
extern void luaL_openlibs(lua_State *L);

#ifndef lua_assert
#define lua_assert(x)   ((void)0)
#endif
