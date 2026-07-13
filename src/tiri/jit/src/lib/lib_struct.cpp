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
#include <cstring>
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
   else if (Type & FD_WORD) {
      if (Type & FD_UNSIGNED) ((uint16_t *)Address)[ElementIndex] = lua_tointeger(L, StackIndex);
      else ((int16_t *)Address)[ElementIndex] = lua_tointeger(L, StackIndex);
   }
   else if (Type & FD_BYTE)   ((uint8_t *)Address)[ElementIndex] = lua_tointeger(L, StackIndex);
   else return false;
   return true;
}

template <typename T> static void copy_array_to_vector(APTR Address, GCarray *Source)
{
   auto &dest = ((kt::vector<T> *)Address)[0];
   dest.resize(Source->len);
   if (Source->len) std::memcpy(dest.data(), Source->arraydata(), size_t(Source->len) * sizeof(T));
}

static void write_array_field(lua_State *L, APTR Address, const struct_field &Field, int StackIndex, CSTRING FieldName)
{
   if (not lua_isarray(L, StackIndex)) {
      luaL_error(L, ERR::InvalidType, "Array field '%s' requires a native array.", FieldName);
   }

   auto source = lua_toarray(L, StackIndex);
   auto expected_type = ff_to_aet(Field.Type);

   if ((Field.Type & FD_STRING) and (not (Field.Type & FD_CPP))) {
      luaL_error(L, ERR::NoSupport, "Raw C string array field '%s' does not support assignment.", FieldName);
   }

   if ((Field.Type & FD_STRING) and (Field.Type & FD_CPP)) {
      if ((source->elemtype != AET::STR_GC) and (source->elemtype != AET::CSTR) and
            (source->elemtype != AET::STR_CPP)) {
         luaL_error(L, ERR::InvalidType, "Array field '%s' requires string elements.", FieldName);
      }

      auto &dest = ((kt::vector<std::string> *)Address)[0];
      dest.resize(source->len);
      for (MSize i = 0; i < source->len; i++) {
         if (source->elemtype IS AET::STR_GC) {
            auto str = strref(source->get<GCRef>()[i]);
            dest[i].assign(strdata(str), str->len);
         }
         else if (source->elemtype IS AET::CSTR) dest[i].assign(source->get<CSTRING>()[i]);
         else dest[i] = source->get<std::string>()[i];
      }
      return;
   }

   if (source->elemtype != expected_type) {
      luaL_error(L, ERR::InvalidType, "Array field '%s' has an incompatible element type.", FieldName);
   }

   if (Field.Type & FD_CPP) {
      if (Field.Type & FD_FLOAT) copy_array_to_vector<float>(Address, source);
      else if (Field.Type & FD_DOUBLE) copy_array_to_vector<double>(Address, source);
      else if (Field.Type & FD_INT64) copy_array_to_vector<int64_t>(Address, source);
      else if (Field.Type & FD_INT) copy_array_to_vector<int>(Address, source);
      else if (Field.Type & FD_WORD) copy_array_to_vector<int16_t>(Address, source);
      else if (Field.Type & FD_BYTE) copy_array_to_vector<uint8_t>(Address, source);
      else luaL_error(L, ERR::NoSupport, "Array field '%s' uses an unsupported vector type.", FieldName);
   }
   else {
      if (source->len > MSize(Field.ArraySize)) {
         luaL_error(L, ERR::OutOfRange, "Array assignment for field '%s' exceeds its fixed size of %d.", FieldName,
            Field.ArraySize);
      }
      const auto copied_bytes = size_t(source->len) * source->elemsize;
      const auto remaining_bytes = (size_t(Field.ArraySize) - source->len) * source->elemsize;
      if (copied_bytes) std::memcpy(Address, source->arraydata(), copied_bytes);
      if (remaining_bytes) std::memset((uint8_t *)Address + copied_bytes, 0, remaining_bytes);
   }
}

static void write_field(lua_State *L, APTR Address, const struct_field &Field, int StackIndex, CSTRING FieldName)
{
   if (Field.Type & FD_ARRAY) {
      write_array_field(L, Address, Field, StackIndex, FieldName);
      return;
   }
   else if (Field.Type & FD_STRING) {
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

static bool struct_has_unsupported_cpp_arrays(const struct_record &Def)
{
   for (auto &field : Def.Fields) {
      if ((field.Type & FD_CPP) and (field.Type & FD_ARRAY) and
            (not (field.Type & (FD_STRING|FD_FLOAT|FD_DOUBLE|FD_INT64|FD_INT|FD_WORD|FD_BYTE)))) return true;
      if ((field.Type & FD_STRUCT) and (not (field.Type & FD_PTR)) and (not (field.Type & FD_CPP)) and
            (not field.StructRef.empty())) {
         if (auto child = glStructs.find(std::string_view(field.StructRef)); child != glStructs.end()) {
            if (struct_has_unsupported_cpp_arrays(child->second)) return true;
         }
      }
   }
   return false;
}

static void copy_cpp_fields(const struct_record &Def, APTR Dest, CPTR Source)
{
   for (auto &field : Def.Fields) {
      auto dest = (int8_t *)Dest + field.Offset;
      auto source = (const int8_t *)Source + field.Offset;
      if ((field.Type & FD_CPP) and (field.Type & FD_ARRAY)) {
         if (field.Type & FD_STRING) {
            ((kt::vector<std::string> *)dest)[0] = ((const kt::vector<std::string> *)source)[0];
         }
         else if (field.Type & FD_FLOAT) ((kt::vector<float> *)dest)[0] = ((const kt::vector<float> *)source)[0];
         else if (field.Type & FD_DOUBLE) ((kt::vector<double> *)dest)[0] = ((const kt::vector<double> *)source)[0];
         else if (field.Type & FD_INT64) ((kt::vector<int64_t> *)dest)[0] = ((const kt::vector<int64_t> *)source)[0];
         else if (field.Type & FD_INT) ((kt::vector<int> *)dest)[0] = ((const kt::vector<int> *)source)[0];
         else if (field.Type & FD_WORD) ((kt::vector<int16_t> *)dest)[0] = ((const kt::vector<int16_t> *)source)[0];
         else if (field.Type & FD_BYTE) ((kt::vector<uint8_t> *)dest)[0] = ((const kt::vector<uint8_t> *)source)[0];
      }
      else if ((field.Type & FD_STRING) and (field.Type & FD_CPP)) {
         ((std::string *)dest)[0] = ((const std::string *)source)[0];
      }
      else if ((field.Type & FD_STRUCT) and (not (field.Type & FD_PTR)) and (not field.StructRef.empty())) {
         if (auto child = glStructs.find(std::string_view(field.StructRef)); child != glStructs.end()) {
            int count = (field.Type & FD_ARRAY) ? field.ArraySize : 1;
            for (int i = 0; i < count; i++) {
               copy_cpp_fields(child->second, dest + (child->second.Size * i), source + (child->second.Size * i));
            }
         }
      }
   }
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
   else if (Type & FD_WORD) {
      if (Type & FD_UNSIGNED) lua_pushinteger(L, ((uint16_t *)Address)[0]);
      else lua_pushinteger(L, ((int16_t *)Address)[0]);
   }
   else if (Type & FD_BYTE)   lua_pushinteger(L, ((uint8_t *)Address)[0]);
   return true;
}

template <typename T> static void push_vector_array(lua_State *L, APTR Address, AET Type)
{
   auto &source = ((kt::vector<T> *)Address)[0];
   lua_createarray(L, source.size(), Type, source.data(), ARRAY_CACHED);
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
   Object *Lifecycle = nullptr, GCstruct *Parent = nullptr)
{
   if (auto def = glStructs.find(StructName); def != glStructs.end()) {
      return lua_pushstruct(L, def->second, Address, 0, Lifecycle, Parent);
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

void lj_struct_push_size_closure(lua_State *L, GCstruct *Struct)
{
   setstructV(L, L->top, Struct);
   incr_top(L);
   lua_pushcclosure(L, &struct_struct_size, 1);
}

static int struct_len(lua_State *L)
{
   auto *value = lj_lib_checkstruct(L, 1);
   lua_pushnumber(L, value->def->Fields.size());
   return 1;
}

static int struct_next_pair(lua_State *L)
{
   auto value = lua_tostruct(L, lua_upvalueindex(1));
   int field_index = lua_tointeger(L, lua_upvalueindex(2));
   if (field_index < 0 or field_index >= int(value->def->Fields.size())) return 0;

   auto &field = value->def->Fields[field_index];
   lj_struct_check_lifecycle(L, value, field.Name.c_str());
   lua_pushinteger(L, field_index + 1);
   lua_replace(L, lua_upvalueindex(2));
   lua_pushstring(L, field.Name);
   lua_getfield(L, lua_upvalueindex(1), field.Name.c_str());
   return 2;
}

static int struct_pairs(lua_State *L)
{
   lj_lib_checkstruct(L, 1);
   lua_pushvalue(L, 1);
   lua_pushinteger(L, 0);
   lua_pushcclosure(L, struct_next_pair, 2);
   lua_pushnil(L);
   lua_pushnil(L);
   return 3;
}

static int struct_tostring(lua_State *L)
{
   auto value = lj_lib_checkstruct(L, 1);
   lua_pushfstring(L, "%s: %p", value->def->Name.c_str(), value->data);
   return 1;
}

void lj_struct_getfield_core(lua_State *L, GCstruct *Struct, struct_field &Field, APTR Address)
{
   int array_size = (not Field.ArraySize) ? -1 : Field.ArraySize;

   if ((Field.Type & FD_CPP) and (Field.Type & FD_ARRAY) and (not Field.StructRef.empty()) and
         (not (Field.Type & FD_PTR))) {
      auto vector = (kt::vector<int> *)Address;
      make_struct_serial_array(L, Field.StructRef, vector->size(), vector->data());
   }
   else if ((Field.Type & FD_STRUCT) and (Field.Type & FD_PTR) and (not Field.StructRef.empty())) {
      if (((APTR *)Address)[0]) {
         if (Field.Type & FD_ARRAY) {
            if (Field.Type & FD_CPP) {
               auto vector = (kt::vector<int> *)Address;
               lua_createarray(L, vector->size(), ff_to_element(Field.Type), (APTR *)vector->data(), ARRAY_CACHED,
                  Field.StructRef);
            }
            else lua_createarray(L, array_size, ff_to_element(Field.Type), (APTR *)Address, ARRAY_CACHED,
               Field.StructRef);
         }
         else if (not push_external_struct(L, ((APTR *)Address)[0], Field.StructRef, Struct->lifecycle)) {
            luaL_error(L, ERR::Search, "Failed to find struct '%s'", Field.StructRef.c_str());
         }
      }
      else lua_pushnil(L);
   }
   else if ((Field.Type & FD_STRUCT) and (Field.Type & FD_ARRAY)) {
      if (Field.Type & FD_CPP) {
         auto vector = (kt::vector<int> *)Address;
         make_any_array(L, Field.Type, Field.StructRef, vector->size(), vector->data());
      }
      else make_struct_array(L, Field.StructRef, array_size, Address);
   }
   else if ((Field.Type & FD_CPP) and (Field.Type & FD_ARRAY) and (not (Field.Type & FD_STRING))) {
      if (Field.Type & FD_FLOAT) push_vector_array<float>(L, Address, AET::FLOAT);
      else if (Field.Type & FD_DOUBLE) push_vector_array<double>(L, Address, AET::DOUBLE);
      else if (Field.Type & FD_INT64) push_vector_array<int64_t>(L, Address, AET::INT64);
      else if (Field.Type & FD_INT) push_vector_array<int>(L, Address, AET::INT32);
      else if (Field.Type & FD_WORD) push_vector_array<int16_t>(L, Address, AET::INT16);
      else if (Field.Type & FD_BYTE) push_vector_array<uint8_t>(L, Address, AET::BYTE);
      else luaL_error(L, ERR::NoSupport, "Vector field '%s' uses an unsupported element type.", Field.Name.c_str());
   }
   else if (Field.Type & FD_STRUCT) {
      GCstruct *parent = nullptr;
      if (not Struct->is_lifecycle_bound()) {
         if (Struct->is_external()) {
            if (gcref(Struct->parent)) parent = structref(Struct->parent);
         }
         else parent = Struct;
      }
      if (not push_external_struct(L, Address, Field.StructRef, Struct->lifecycle, parent)) {
         luaL_error(L, ERR::Search, "Failed to find struct '%s'", Field.StructRef.c_str());
      }
   }
   else if (Field.Type & FD_STRING) {
      if (Field.Type & FD_ARRAY) {
         if (Field.Type & FD_CPP) {
            auto vector = (kt::vector<std::string> *)Address;
            lua_createarray(L, vector->size(), AET::STR_CPP, vector, ARRAY_CACHED);
         }
         else lua_createarray(L, array_size, AET::CSTR, (APTR *)Address, ARRAY_CACHED);
      }
      else if (Field.Type & FD_CPP) lua_pushstring(L, *((std::string *)Address));
      else lua_pushstring(L, ((STRING *)Address)[0]);
   }
   else if (Field.Type & FD_OBJECT) {
      if (auto object = ((OBJECTPTR *)Address)[0]) push_object(L, object);
      else lua_pushnil(L);
   }
   else if (Field.Type & FD_POINTER) {
      if (((APTR *)Address)[0]) lua_pushlightuserdata(L, ((APTR *)Address)[0]);
      else lua_pushnil(L);
   }
   else if (Field.Type & FD_FUNCTION) lua_pushnil(L);
   else if (read_primitive_field(L, Address, Field.Type, array_size));
   else luaL_error(L, ERR::InvalidType,
      std::format("Field '{}' does not use a supported type of {:x}", Field.Name, Field.Type));
}

void lj_struct_setfield_core(lua_State *L, GCstruct *, struct_field &Field, APTR Address)
{
   write_field(L, Address, Field, -1, Field.Name.c_str());
   lua_pop(L, 1);
}

static int struct_get(lua_State *L)
{
   auto *value = lj_lib_checkstruct(L, 1);
   auto field_name = luaL_checkstring(L, 2);
   if (std::string_view("structSize") IS field_name) {
      lj_struct_push_size_closure(L, value);
      return 1;
   }
   lj_struct_check_lifecycle(L, value, field_name);
   if (not value->data) {
      luaL_error(L, ERR::Failed, "Cannot reference field '%s' because struct address is NULL.", field_name);
   }

   if (auto field_opt = find_field(value, field_name)) {
      auto &field = field_opt->get();
      APTR address = (int8_t *)value->data + field.Offset;
      lj_struct_getfield_core(L, value, field, address);
      return 1;
   }
   luaL_error(L, ERR::FieldNotFound, "Field '%s' does not exist in structure.", field_name);
   return 0;
}

static int struct_set(lua_State *L)
{
   auto *value = lj_lib_checkstruct(L, 1);
   auto field_name = luaL_checkstring(L, 2);
   lj_struct_check_lifecycle(L, value, field_name);
   if (not value->data) luaL_error(L, "Cannot reference field '%s' because struct address is NULL.", field_name);

   if (auto field_opt = find_field(value, field_name)) {
      auto &field = field_opt->get();
      APTR address = (int8_t *)value->data + field.Offset;
      lua_pushvalue(L, 3);
      lj_struct_setfield_core(L, value, field, address);
      return 0;
   }
   luaL_error(L, "Invalid field reference '%s'", field_name);
   return 0;
}

static void copy_struct_payload(lua_State *L, GCstruct *Dest, GCstruct *Source)
{
   if (Dest->def != Source->def) luaL_error(L, ERR::Mismatch, "Struct definitions must match for copying.");
   lj_struct_check_lifecycle(L, Dest, "copy destination");
   lj_struct_check_lifecycle(L, Source, "copy source");
   if ((not Dest->data) or (not Source->data)) luaL_error(L, ERR::Failed, "Cannot copy a null struct payload.");
   if (Dest->data IS Source->data) return;
   if (struct_has_unsupported_cpp_arrays(*Dest->def)) {
      luaL_error(L, ERR::NoSupport, "Struct copy encountered an unsupported C++ array field type.");
   }

   auto dest_start = uintptr_t(Dest->data);
   auto source_start = uintptr_t(Source->data);
   auto size = size_t(Dest->structsize);
   if ((dest_start < source_start + size) and (source_start < dest_start + size)) {
      luaL_error(L, ERR::NoSupport, "Struct copy does not support overlapping payloads.");
   }

   destroy_struct_cpp_strings(*Dest->def, Dest->data);
   std::memcpy(Dest->data, Source->data, size);
   construct_struct_cpp_strings(*Dest->def, Dest->data);
   copy_cpp_fields(*Dest->def, Dest->data, Source->data);
}

LJLIB_CF(struct_copy)
{
   auto dest = lj_lib_checkstruct(L, 1);
   auto source = lj_lib_checkstruct(L, 2);
   copy_struct_payload(L, dest, source);
   lua_settop(L, 1);
   return 1;
}

LJLIB_CF(struct_clone)
{
   auto source = lj_lib_checkstruct(L, 1);
   lj_struct_check_lifecycle(L, source, "clone source");
   if (struct_has_unsupported_cpp_arrays(*source->def)) {
      luaL_error(L, ERR::NoSupport, "Struct clone encountered an unsupported C++ array field type.");
   }
   auto dest = lua_pushstruct(L, *source->def);
   copy_struct_payload(L, dest, source);
   return 1;
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
   lua_pushcfunction(L, struct_pairs);
   lua_setfield(L, -2, "__pairs");
   lua_pushcfunction(L, struct_tostring);
   lua_setfield(L, -2, "__tostring");
   setgcref(basemt_it(g, LJ_TSTRUCT), obj2gco(lib));

   reg_iface_prototype("struct", "new", { TiriType::Struct }, { TiriType::Str, TiriType::Table });
   reg_iface_prototype("struct", "size", { TiriType::Num }, { TiriType::Str });
   reg_iface_prototype("struct", "structSize", { TiriType::Num }, { TiriType::Struct });
   reg_iface_prototype("struct", "copy", { TiriType::Struct }, { TiriType::Struct, TiriType::Struct });
   reg_iface_prototype("struct", "clone", { TiriType::Struct }, { TiriType::Struct });
   return 1;
}
