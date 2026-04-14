/*********************************************************************************************************************

The num interface provides support for processing a range of numeric types other than Lua's default of DOUBLE.

   floatnum = num.float(1.2)
   intnum = num.int(3)
   dblnum = num.double(513.3982)

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>

#include "lauxlib.h"
#include "hashes.h"
#include "defs.h"

//********************************************************************************************************************
// Any Read accesses to the object will pass through here.

static int number_index(lua_State *Lua)
{
   struct fnumber *num;
   CSTRING field;

   if ((num = (struct fnumber *)luaL_checkudata(Lua, 1, "Tiri.num"))) {
      if (!(field = luaL_checkstring(Lua, 2))) return 0;

      if (HASH_VALUE IS strihash(field)) {
         switch (num->Type) {
            case NUM_DOUBLE: lua_pushnumber(Lua, num->f64); break;
            case NUM_FLOAT:  lua_pushnumber(Lua, num->f32); break;
            case NUM_INT64:  lua_pushnumber(Lua, num->i64); break;
            case NUM_INT:    lua_pushinteger(Lua, num->i32); break;
            case NUM_INT16:  lua_pushinteger(Lua, num->i16); break;
            case NUM_BYTE:   lua_pushinteger(Lua, num->i8); break;
            default: lua_pushstring(Lua, "?");
         }
         return 1;
      }
   }
   return 0;
}

/*********************************************************************************************************************
** Usage: num.[type]([Value])
*/

static int number_f64(lua_State *Lua)
{
   auto f64 = lua_tonumber(Lua, 1);
   if (auto num = (fnumber *)lua_newuserdata(Lua, sizeof(struct fnumber))) {
      luaL_getmetatable(Lua, "Tiri.num");
      lua_setmetatable(Lua, -2);
      num->f64 = f64;
      num->Type = NUM_DOUBLE;
      return 1;
   }

   luaL_error(Lua, "Failed to create a new number object.");
   return 0;
}

static int number_f32(lua_State *Lua)
{
   auto f32 = lua_tonumber(Lua, 1);
   if (auto num = (fnumber *)lua_newuserdata(Lua, sizeof(struct fnumber))) {
      luaL_getmetatable(Lua, "Tiri.num");
      lua_setmetatable(Lua, -2);
      num->f32 = f32;
      num->Type = NUM_FLOAT;
      return 1;
   }

   luaL_error(Lua, "Failed to create a new number object.");
   return 0;
}

static int number_i32(lua_State *Lua)
{
   auto i32 = lua_tointeger(Lua, 1);
   if (auto num = (fnumber *)lua_newuserdata(Lua, sizeof(struct fnumber))) {
      luaL_getmetatable(Lua, "Tiri.num");
      lua_setmetatable(Lua, -2);
      num->i32 = i32;
      num->Type = NUM_INT;
      return 1;
   }

   luaL_error(Lua, "Failed to create a new number object.");
   return 0;
}

static int number_i64(lua_State *Lua)
{
   auto i64 = lua_tointeger(Lua, 1);
   if (auto num = (fnumber *)lua_newuserdata(Lua, sizeof(struct fnumber))) {
      luaL_getmetatable(Lua, "Tiri.num");
      lua_setmetatable(Lua, -2);
      num->i64 = i64;
      num->Type = NUM_INT64;
      return 1;
   }

   luaL_error(Lua, "Failed to create a new number object.");
   return 0;
}

static int number_i16(lua_State *Lua)
{
   int i16 = lua_tointeger(Lua, 1);
   if (auto num = (fnumber *)lua_newuserdata(Lua, sizeof(struct fnumber))) {
      luaL_getmetatable(Lua, "Tiri.num");
      lua_setmetatable(Lua, -2);
      num->i16 = i16;
      num->Type = NUM_INT16;
      return 1;
   }

   luaL_error(Lua, "Failed to create a new number object.");
   return 0;
}

static int number_i8(lua_State *Lua)
{
   int i8 = lua_tointeger(Lua, 1);
   if (auto num = (fnumber *)lua_newuserdata(Lua, sizeof(struct fnumber))) {
      luaL_getmetatable(Lua, "Tiri.num");
      lua_setmetatable(Lua, -2);
      num->i8 = i8;
      num->Type = NUM_BYTE;
      return 1;
   }

   luaL_error(Lua, "Failed to create a new number object.");
   return 0;
}

/*********************************************************************************************************************
** Internal: Prints the number as a string.
*/

static int number_tostring(lua_State *Lua)
{
   if (auto num = (fnumber *)lua_touserdata(Lua, 1)) {
      switch (num->Type) {
         case NUM_DOUBLE: lua_pushfstring(Lua, std::to_string(num->f64).c_str()); return 1;
         case NUM_FLOAT:  lua_pushfstring(Lua, std::to_string(num->f32).c_str()); return 1;
         case NUM_INT64:  lua_pushstring(Lua, std::to_string(num->i64).c_str()); return 1;
         case NUM_INT:    lua_pushstring(Lua, std::to_string(num->i32).c_str()); return 1;
         case NUM_INT16:  lua_pushstring(Lua, std::to_string(num->i16).c_str()); return 1;
         case NUM_BYTE:   lua_pushstring(Lua, std::to_string(num->i8).c_str()); return 1;
      }
   }

   lua_pushstring(Lua, "?");
   return 1;
}

/*********************************************************************************************************************
** Register the number interface.
*/

void register_number_class(lua_State *Lua)
{
   static const luaL_Reg numlib_functions[] = {
      { "int",    number_i32 },
      { "int64",  number_i64 },
      { "double", number_f64 },
      { "float",  number_f32 },
      { "byte",   number_i8 },
      { "char",   number_i8 },
      { "int16",  number_i16 },
      { nullptr, nullptr }
   };

   static const luaL_Reg numlib_methods[] = {
      { "__tostring", number_tostring },
      { "__index",    number_index },
      { nullptr, nullptr }
   };

   pf::Log log(__FUNCTION__);
   log.trace("Registering number interface.");

   luaL_newmetatable(Lua, "Tiri.num");
   lua_pushstring(Lua, "Tiri.num");
   lua_setfield(Lua, -2, "__name");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable created earlier
   lua_settable(Lua, -3);   // metatable.__index = metatable

   luaL_openlib(Lua, nullptr, numlib_methods, 0);
   luaL_openlib(Lua, "num", numlib_functions, 0);
}
