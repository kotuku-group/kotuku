// Native Tiri struct library.
// Copyright (C) 2026 Paul Manias.

#define lib_struct_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_struct.h"
#include "lj_proto_registry.h"
#include "lib.h"

#include <format>
#include <optional>
#include <kotuku/main.h>

#include "../../defs.h"

#define LJLIB_MODULE_struct

static bool write_primitive_field(lua_State *L, APTR Address, int Type, int StackIndex, int ElementIndex = 0)
{
   if (Type & FD_FLOAT)       ((float *)Address)[ElementIndex]   = lua_tonumber(L, StackIndex);
   else if (Type & FD_DOUBLE) ((double *)Address)[ElementIndex]  = lua_tonumber(L, StackIndex);
   else if (Type & FD_INT64)  ((int64_t *)Address)[ElementIndex] = lua_tonumber(L, StackIndex);
   else if (Type & FD_INT)    ((int *)Address)[ElementIndex]     = lua_tointeger(L, StackIndex);
   else if (Type & FD_WORD)   ((int16_t *)Address)[ElementIndex] = lua_tointeger(L, StackIndex);
   else if (Type & FD_BYTE)   ((uint8_t *)Address)[ElementIndex] = lua_tointeger(L, StackIndex);
   else return false;
   return true;
}

static void write_field(lua_State *L, APTR Address, const struct_field &Field, int StackIndex, CSTRING FieldName)
{
   if (Field.Type & FD_STRING) {
      if ((Field.Type & FD_CPP) and (not (Field.Type & FD_ARRAY))) {
         size_t length;
         auto value = luaL_checklstring(L, StackIndex, &length);
         ((std::string *)Address)[0].assign(value, length);
         return;
      }
      luaL_error(L, ERR::InvalidType, "Field '%s' does not support string assignment.", FieldName);
   }
   else if (Field.Type & FD_OBJECT) {
      if (lua_isnil(L, StackIndex)) ((OBJECTPTR *)Address)[0] = nullptr;
      else if (lua_isobject(L, StackIndex)) {
         auto object = lua_toobject(L, StackIndex);
         if (object_is_dead(object)) {
            luaL_error(L, ERR::DoesNotExist, "Cannot assign detached object to field '%s'.", FieldName);
         }
         ((OBJECTPTR *)Address)[0] = object->ptr;
      }
      else if (lua_islightuserdata(L, StackIndex)) {
         ((OBJECTPTR *)Address)[0] = (OBJECTPTR)lua_touserdata(L, StackIndex);
      }
      else luaL_error(L, ERR::InvalidType, "Field '%s' requires an object, lightuserdata or nil.", FieldName);
      return;
   }
   else if (Field.Type & FD_POINTER) {
      if ((not lua_isnil(L, StackIndex)) and (not lua_islightuserdata(L, StackIndex))) {
         luaL_error(L, ERR::InvalidType, "Field '%s' requires lightuserdata or nil.", FieldName);
      }
      ((APTR *)Address)[0] = lua_touserdata(L, StackIndex);
      return;
   }
   else if (write_primitive_field(L, Address, Field.Type, StackIndex)) return;

   luaL_error(L, ERR::InvalidType, "Field '%s' does not support assignment (type %x).", FieldName, Field.Type);
}

static bool read_primitive_field(lua_State *L, APTR Address, int Type, int ArraySize)
{
   if (not (Type & (FD_FLOAT|FD_DOUBLE|FD_INT64|FD_INT|FD_WORD|FD_BYTE))) return false;

   if (Type & FD_ARRAY) {
      if ((Type & FD_CUSTOM) and (Type & FD_BYTE)) lua_pushstring(L, (CSTRING)Address);
      else lua_createarray(L, ArraySize, ff_to_aet(Type), (APTR *)Address, ARRAY_CACHED);
   }
   else if (Type & FD_FLOAT)  lua_pushnumber(L, ((float *)Address)[0]);
   else if (Type & FD_DOUBLE) lua_pushnumber(L, ((double *)Address)[0]);
   else if (Type & FD_INT64)  lua_pushnumber(L, ((int64_t *)Address)[0]);
   else if (Type & FD_INT)    lua_pushinteger(L, ((int *)Address)[0]);
   else if (Type & FD_WORD)   lua_pushinteger(L, ((int16_t *)Address)[0]);
   else if (Type & FD_BYTE)   lua_pushinteger(L, ((uint8_t *)Address)[0]);
   return true;
}

[[nodiscard]] static std::optional<std::reference_wrapper<struct_field>> find_field(GCstruct *Struct,
   CSTRING FieldName)
{
   if (auto def = Struct->def) {
      auto field_hash = strihash(FieldName);
      for (auto &field : def->Fields) {
         if (field.nameHash() IS field_hash) return std::ref(field);
      }
   }
   return std::nullopt;
}

static GCstruct * push_external_struct(lua_State *L, APTR Address, std::string_view StructName,
   Object *Lifecycle = nullptr)
{
   if (auto def = glStructs.find(StructName); def != glStructs.end()) {
      // This view does not anchor its parent.  It must not outlive the struct whose payload contains Address.
      // Sub-views of a lifecycle-bound view inherit the binding (each takes its own weak pin on the object).
      return lua_pushstruct(L, def->second, Address, 0, Lifecycle);
   }
   return nullptr;
}

LJLIB_CF(struct_size)
{
   if (auto name = lua_tostring(L, 1)) {
      if (auto def = glStructs.find(struct_name(name)); def != glStructs.end()) {
         lua_pushnumber(L, def->second.Size);
         return 1;
      }
      luaL_argerror(L, 1, "The requested structure is not defined.");
   }
   else luaL_argerror(L, 1, "Structure name required.");
   return 0;
}

LJLIB_CF(struct_new)
{
   auto struct_name_value = luaL_checkstring(L, 1);
   auto def = glStructs.find(struct_name(struct_name_value));
   if (def IS glStructs.end()) {
      luaL_argerror(L, 1, "The requested structure is not defined.");
      return 0;
   }

   auto *result = lua_pushstruct(L, def->second);
   if (not result) {
      luaL_error(L, ERR::Memory, "Failed to create new struct.");
      return 0;
   }

   if (lua_istable(L, 2)) {
      lua_pushnil(L);
      while (lua_next(L, 2) != 0) {
         auto field_name = luaL_checkstring(L, -2);
         if (auto field_opt = find_field(result, field_name)) {
            auto &field = field_opt->get();
            APTR address = (int8_t *)result->data + field.Offset;
            write_field(L, address, field, -1, field_name);
         }
         else {
            lua_pop(L, 2);
            break;
         }
         lua_pop(L, 1);
      }
   }
   return 1;
}

static int struct_struct_size(lua_State *L)
{
   auto *value = lua_isstruct(L, lua_upvalueindex(1)) ? lua_tostruct(L, lua_upvalueindex(1)) : nullptr;
   if (not value) {
      luaL_argerror(L, 1, "Expected struct.");
      return 0;
   }
   lua_pushnumber(L, value->structsize);
   return 1;
}

static int struct_len(lua_State *L)
{
   auto *value = lj_lib_checkstruct(L, 1);
   lua_pushnumber(L, value->def->Fields.size());
   return 1;
}

// Lifecycle staleness guard for interpreted struct accesses.  The check applies only to structs that carry a
// dependency on a Kotuku object's lifecycle (STRUCT_LIFECYCLE); all other structs pass through untouched.  A JIT
// fast path for struct fields would follow the same rule, compiling the guard out entirely when the struct has no
// lifecycle dependency (struct accesses currently always take this interpreted route).

static void check_lifecycle(lua_State *L, GCstruct *Struct, CSTRING FieldName)
{
   if (lj_struct_stale(Struct)) {
      luaL_error(L, ERR::DoesNotExist, "Cannot access field '%s'; the object providing this struct has been destroyed.",
         FieldName);
   }
}

static int struct_get(lua_State *L)
{
   auto *value = lj_lib_checkstruct(L, 1);
   auto field_name = luaL_checkstring(L, 2);
   if (std::string_view("structSize") IS field_name) {
      lua_pushvalue(L, 1);
      lua_pushcclosure(L, &struct_struct_size, 1);
      return 1;
   }
   check_lifecycle(L, value, field_name);
   if (not value->data) {
      luaL_error(L, ERR::Failed, "Cannot reference field '%s' because struct address is NULL.", field_name);
   }

   if (auto field_opt = find_field(value, field_name)) {
      auto &field = field_opt->get();
      APTR address = (int8_t *)value->data + field.Offset;
      int array_size = (not field.ArraySize) ? -1 : field.ArraySize;

      if ((field.Type & FD_CPP) and (field.Type & FD_ARRAY) and (not field.StructRef.empty()) and
            (not (field.Type & FD_PTR))) {
         auto vector = (kt::vector<int> *)address;
         make_struct_serial_array(L, field.StructRef, vector->size(), vector->data());
      }
      else if ((field.Type & FD_STRUCT) and (field.Type & FD_PTR) and (not field.StructRef.empty())) {
         if (((APTR *)address)[0]) {
            if (field.Type & FD_ARRAY) {
               if (field.Type & FD_CPP) {
                  auto vector = (kt::vector<int> *)address;
                  lua_createarray(L, vector->size(), ff_to_element(field.Type), (APTR *)vector->data(), ARRAY_CACHED,
                     field.StructRef);
               }
               else lua_createarray(L, array_size, ff_to_element(field.Type), (APTR *)address, ARRAY_CACHED,
                  field.StructRef);
            }
            else if (not push_external_struct(L, ((APTR *)address)[0], field.StructRef, value->lifecycle)) {
               luaL_error(L, ERR::Search, "Failed to find struct '%s'", field.StructRef.c_str());
            }
         }
         else lua_pushnil(L);
      }
      else if ((field.Type & FD_STRUCT) and (field.Type & FD_ARRAY)) {
         if (field.Type & FD_CPP) {
            auto vector = (kt::vector<int> *)address;
            make_any_array(L, field.Type, field.StructRef, vector->size(), vector->data());
         }
         else make_struct_array(L, field.StructRef, array_size, address);
      }
      else if (field.Type & FD_STRUCT) {
         if (not push_external_struct(L, address, field.StructRef, value->lifecycle)) {
            luaL_error(L, ERR::Search, "Failed to find struct '%s'", field.StructRef.c_str());
         }
      }
      else if (field.Type & FD_STRING) {
         if (field.Type & FD_ARRAY) {
            if (field.Type & FD_CPP) {
               auto vector = (kt::vector<std::string> *)address;
               lua_createarray(L, vector->size(), AET::STR_CPP, (APTR *)vector->data(), ARRAY_CACHED);
            }
            else lua_createarray(L, array_size, AET::CSTR, (APTR *)address, ARRAY_CACHED);
         }
         else if (field.Type & FD_CPP) lua_pushstring(L, *((std::string *)address));
         else lua_pushstring(L, ((STRING *)address)[0]);
      }
      else if (field.Type & FD_OBJECT) {
         if (auto object = ((OBJECTPTR *)address)[0]) push_object(L, object);
         else lua_pushnil(L);
      }
      else if (field.Type & FD_POINTER) {
         if (((APTR *)address)[0]) lua_pushlightuserdata(L, ((APTR *)address)[0]);
         else lua_pushnil(L);
      }
      else if (field.Type & FD_FUNCTION) lua_pushnil(L);
      else if (read_primitive_field(L, address, field.Type, array_size));
      else luaL_error(L, ERR::InvalidType,
         std::format("Field '{}' does not use a supported type of {:x}", field_name, field.Type));
      return 1;
   }
   luaL_error(L, ERR::FieldNotFound, "Field '%s' does not exist in structure.", field_name);
   return 0;
}

static int struct_set(lua_State *L)
{
   auto *value = lj_lib_checkstruct(L, 1);
   auto field_name = luaL_checkstring(L, 2);
   check_lifecycle(L, value, field_name);
   if (not value->data) luaL_error(L, "Cannot reference field '%s' because struct address is NULL.", field_name);

   if (auto field_opt = find_field(value, field_name)) {
      auto &field = field_opt->get();
      APTR address = (int8_t *)value->data + field.Offset;
      write_field(L, address, field, 3, field_name);
      return 0;
   }
   luaL_error(L, "Invalid field reference '%s'", field_name);
   return 0;
}

#include "lj_libdef.h"

extern "C" int luaopen_struct(lua_State *L)
{
   LJ_LIB_REG(L, "struct", struct);
   GCtab *lib = tabV(L->top - 1);
   global_State *g = G(L);

   lua_pushcfunction(L, struct_get);
   lua_setfield(L, -2, "__index");
   lua_pushcfunction(L, struct_set);
   lua_setfield(L, -2, "__newindex");
   lua_pushcfunction(L, struct_len);
   lua_setfield(L, -2, "__len");
   setgcref(basemt_it(g, LJ_TSTRUCT), obj2gco(lib));

   reg_iface_prototype("struct", "new", { TiriType::Struct }, { TiriType::Str, TiriType::Table });
   reg_iface_prototype("struct", "size", { TiriType::Num }, { TiriType::Str });
   reg_iface_prototype("struct", "structSize", { TiriType::Num }, { TiriType::Struct });
   return 1;
}
