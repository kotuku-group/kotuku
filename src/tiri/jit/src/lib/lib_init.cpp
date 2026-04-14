// Library initialization.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Major parts taken verbatim from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#define lib_init_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arch.h"
#include "runtime/lj_thunk.h"
#include "runtime/lj_proto_registry.h"

extern "C" int luaopen_range(lua_State* L);
extern "C" int luaopen_array(lua_State* L);
extern "C" int luaopen_object(lua_State* L);

static const luaL_Reg lj_lib_load[] = {
  { "",       luaopen_base },
  { "table",  luaopen_table },
  { "string", luaopen_string },
  { "math",   luaopen_math },
  { "debug",  luaopen_debug },
  { "bit",    luaopen_bit },
  { "jit",    luaopen_jit },
  { "range",  luaopen_range },
  { "array",  luaopen_array },
  { "obj",    luaopen_object },
  { nullptr,  nullptr }
};

static const luaL_Reg lj_lib_preload[] = {
  { nullptr,      nullptr }
};

extern void luaL_openlibs(lua_State* L)
{
   // Initialise prototype registry before loading libraries
   init_proto_registry();

   const luaL_Reg* lib;
   for (lib = lj_lib_load; lib->func; lib++) {
      lua_pushcfunction(L, lib->func);
      lua_pushstring(L, lib->name);
      lua_call(L, 1, 0);
   }

   luaL_findtable(L, LUA_REGISTRYINDEX, "_PRELOAD",
      sizeof(lj_lib_preload) / sizeof(lj_lib_preload[0]) - 1);

   for (lib = lj_lib_preload; lib->func; lib++) {
      lua_pushcfunction(L, lib->func);
      lua_setfield(L, -2, lib->name);
   }

   lua_pop(L, 1);

   // Initialize thunk metatable for deferred evaluation support
   lj_thunk_init(L);

   // Create _LIB global table for namespace registry
   lua_newtable(L);
   lua_setglobal(L, "_LIB");
}

