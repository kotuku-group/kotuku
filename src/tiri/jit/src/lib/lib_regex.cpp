// Compiler-owned regex callable registration.
// Copyright (C) 2026 Paul Manias

#define lib_regex_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include "lib.h"

extern int tiri_regex_new(lua_State *Lua);

#define LJLIB_MODULE_regex

LJLIB_INTRINSIC LJLIB_CF(regex_new)
{
   return tiri_regex_new(L);
}

#include "lj_libdef.h"

extern int luaopen_regex_intrinsic(lua_State *Lua)
{
   LJ_LIB_REG(Lua, nullptr, regex);
   return 1;
}
