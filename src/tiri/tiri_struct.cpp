/*********************************************************************************************************************

To create a struct from a registered definition:  xmltag = struct.new('XTag')
To create a struct with pre-configured values:    xmltag = struct.new('XTag', { name='Hello' })
To get the byte size of any structure definition: size = struct.size('XTag')
To get the byte size of a created structure:      size = struct.size(xmltag)
To get the total number of fields in a structure: #xmltag

Acceptable short-hand field definitions, for MAKESTRUCT() and IDL usage only:

  l = Long
  d = Double
  x = Large
  f = Float
  w = Word
  b = Byte
  c = Char (If used in an array, array will be interpreted as a string)
  p = Pointer (For a pointer to refer to another structure, use the suffix ':StructName')
  s = String
  o = Object (Pointer)
  r = Function (Embedded)
  e = Embedded structure (e.g. 'eColour:RGB' would embed an RGB structure)

Prefixes for variants, in order of acceptable usage:

  z = Use the C++ variant of the type, e.g. 'cs' for std::string
  u = Unsigned (Use in conjunction with a type)

Embedded arrays are permitted if you follow the field name with [n] where 'n' is the array size.  For pointers to
null terminated arrays, use [0].

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <inttypes.h>
#include <format>
#include <new>
#include <limits>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <climits>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lib.h"
#include "lj_obj.h"
#include "defs.h"
#include "protected_call.h"

#ifdef UNIT_TESTS
static thread_local int glStructLiveReferences = 0;
int test_struct_live_references() { return glStructLiveReferences; }
#endif

static constexpr int MAX_STRUCT_DEF = 2048; // Struct definitions are typically 100 - 400 bytes.

struct trivial_struct_vector {
   size_t Capacity;
   size_t Length;
   APTR Elements;
};

static_assert(sizeof(trivial_struct_vector) IS sizeof(kt::vector<int>));

void construct_trivial_struct_vector(APTR Address)
{
   new (Address) trivial_struct_vector { 0, 0, nullptr };
}

void destroy_trivial_struct_vector(APTR Address)
{
   auto vector = (trivial_struct_vector *)Address;
   ::operator delete(vector->Elements);
   vector->Elements = nullptr;
   vector->Length = 0;
   vector->Capacity = 0;
}

void assign_trivial_struct_vector(APTR Address, CPTR Source, size_t Elements, size_t Stride)
{
   auto vector = (trivial_struct_vector *)Address;
   if (Elements > vector->Capacity) {
      APTR replacement = Elements ? ::operator new(Elements * Stride) : nullptr;
      ::operator delete(vector->Elements);
      vector->Elements = replacement;
      vector->Capacity = Elements;
   }
   if (Elements) std::memcpy(vector->Elements, Source, Elements * Stride);
   vector->Length = Elements;
}

void copy_trivial_struct_vector(APTR Dest, CPTR Source, size_t Stride)
{
   auto source = (const trivial_struct_vector *)Source;
   assign_trivial_struct_vector(Dest, source->Elements, source->Length, Stride);
}

size_t trivial_struct_vector_size(CPTR Address)
{
   return ((const trivial_struct_vector *)Address)->Length;
}

APTR trivial_struct_vector_data(APTR Address)
{
   return ((trivial_struct_vector *)Address)->Elements;
}

CPTR trivial_struct_vector_data(CPTR Address)
{
   return ((const trivial_struct_vector *)Address)->Elements;
}

inline GCstruct * push_struct_def(lua_State *Lua, APTR Address, struct_record &StructDef, bool Deallocate,
   OBJECTPTR Lifecycle = nullptr)
{
   return lua_pushstruct(Lua, StructDef, Address, Deallocate ? STRUCT_DEALLOCATE : 0, Lifecycle);
}

// Handles both construction and destruction of std::string usage in a structure.

static void process_struct_cpp_strings(lua_State *Lua, const struct_record &StructDef, APTR Address, bool Construct)
{
   if (not Address) return;

   for (auto field = Construct ? StructDef.Fields.begin() : StructDef.Fields.end();
      Construct ? (field != StructDef.Fields.end()) : (field != StructDef.Fields.begin());) {
      if (not Construct) --field;

      APTR field_address = (int8_t *)Address + field->Offset;
      auto type = field->Type;

      if ((type & FD_STRUCT) and (not (type & FD_PTR)) and (not (type & FD_VECTOR)) and
            (field->StructRef != 0)) {
         auto def = field->StructDefinition ? field->StructDefinition :
            find_struct_reference(Lua, StructDef, field->StructRef);
         if (def) {
            if ((type & FD_ARRAY) and (field->ArraySize > 0)) {
               for (int i = Construct ? 0 : field->ArraySize; Construct ? (i < field->ArraySize) : (i > 0);) {
                  if (not Construct) --i;
                  process_struct_cpp_strings(Lua, *def, (int8_t *)field_address + (def->Size * i), Construct);
                  if (Construct) i++;
               }
            }
            else process_struct_cpp_strings(Lua, *def, field_address, Construct);
         }
      }

      if ((type & FD_STRING) and (type & FD_CPP) and (not (type & FD_VECTOR))) {
         if (Construct) new (field_address) std::string();
         else ((std::string *)field_address)->~basic_string();
      }

      if (type & FD_VECTOR) {
         if ((type & FD_STRUCT) and (not (type & FD_PTR))) {
            if (Construct) construct_trivial_struct_vector(field_address);
            else destroy_trivial_struct_vector(field_address);
         }
         else if (type & FD_STRING) {
            if (Construct) new (field_address) kt::vector<std::string>();
            else ((kt::vector<std::string> *)field_address)->~vector();
         }
         else if (type & FD_FLOAT) {
            if (Construct) new (field_address) kt::vector<float>();
            else ((kt::vector<float> *)field_address)->~vector();
         }
         else if (type & FD_DOUBLE) {
            if (Construct) new (field_address) kt::vector<double>();
            else ((kt::vector<double> *)field_address)->~vector();
         }
         else if (type & FD_INT64) {
            if (Construct) new (field_address) kt::vector<int64_t>();
            else ((kt::vector<int64_t> *)field_address)->~vector();
         }
         else if (type & FD_INT) {
            if (Construct) new (field_address) kt::vector<int>();
            else ((kt::vector<int> *)field_address)->~vector();
         }
         else if (type & FD_WORD) {
            if (Construct) new (field_address) kt::vector<int16_t>();
            else ((kt::vector<int16_t> *)field_address)->~vector();
         }
         else if (type & FD_BYTE) {
            if (Construct) new (field_address) kt::vector<uint8_t>();
            else ((kt::vector<uint8_t> *)field_address)->~vector();
         }
         else {
            if (Construct) new (field_address) kt::vector<int>();
            else ((kt::vector<int> *)field_address)->~vector();
         }
      }

      if (Construct) ++field;
   }
}

void construct_struct_cpp_strings(lua_State *Lua, const struct_record &StructDef, APTR Address)
{
   process_struct_cpp_strings(Lua, StructDef, Address, true);
}

void destroy_struct_cpp_strings(lua_State *Lua, const struct_record &StructDef, APTR Address)
{
   process_struct_cpp_strings(Lua, StructDef, Address, false);
}

//********************************************************************************************************************
// Create a standard Lua table and copy the struct values to that table.  Pushes nil if there was a conversion issue.
// Note the use of the References lookup, which prevents circular referencing and duplication of existing structs.
//
// NOTE: In the event of an error code being returned, no value is pushed to the stack.

[[nodiscard]] ERR named_struct_to_table(lua_State *Lua, std::string_view StructName, CPTR Address)
{
   // NB: Custom comparator will stop if a colon is encountered in StructName
   if (auto def = find_struct(Lua, StructName)) {
      ERR error = ERR::Okay;
      int status;
      {
         std::vector<lua_ref> ref;
         auto convert = [&]() { error = struct_to_table(Lua, ref, *def, Address); };
         status = protected_tiri_call(Lua, convert);
         unref_struct_references(Lua, ref);
      }
      if (status) lj_err_throw(Lua, status);
      return error;
   }
   else if (StructName.starts_with("KeyValue")) {
      // A struct name of 'KeyValue' allows the KEYVALUE type to be used for building structures dynamically.
      // ankerl::unordered_dense::map<std::string, std::string>

      keyvalue_to_table(Lua, (const KEYVALUE *)Address);
      return ERR::Okay;
   }
   else {
      kt::Log().warning("Unknown struct name '%.*s' - use 'include' to load module definitions.", int(StructName.size()), StructName.data());
      return ERR::Search;
   }
}

//********************************************************************************************************************

void unref_struct_references(lua_State *Lua, std::vector<lua_ref> &References)
{
   for (auto &rec : References) {
      if ((rec.Ref != LUA_NOREF) and (rec.Ref != LUA_REFNIL)) {
         luaL_unref(Lua, LUA_REGISTRYINDEX, rec.Ref);
#ifdef UNIT_TESTS
         --glStructLiveReferences;
#endif
      }
   }

   References.clear();
}

//********************************************************************************************************************

void keyvalue_to_table(lua_State *Lua, const KEYVALUE *Map)
{
   if (not Map) { lua_pushnil(Lua); return; }

   lua_createtable(Lua, 0, Map->size()); // Create a new table on the stack.

   for (auto & [ key, val ] : *Map) {
      lua_pushlstring(Lua, key.c_str(), key.size());
      lua_pushlstring(Lua, val.c_str(), val.size());
      lua_settable(Lua, -3);
   }
}

//********************************************************************************************************************

static bool is_primitive_field(int Type)
{
   return Type & (FD_FLOAT|FD_DOUBLE|FD_INT64|FD_INT|FD_WORD|FD_BYTE);
}

//********************************************************************************************************************

static bool write_primitive_field(lua_State *Lua, APTR Address, int Type, int StackIndex, int ElementIndex = 0)
{
   if (Type & FD_FLOAT)       ((float *)Address)[ElementIndex]   = lua_tonumber(Lua, StackIndex);
   else if (Type & FD_DOUBLE) ((double *)Address)[ElementIndex]  = lua_tonumber(Lua, StackIndex);
   else if (Type & FD_INT64)  ((int64_t *)Address)[ElementIndex] = lua_tonumber(Lua, StackIndex);
   else if (Type & FD_INT)    ((int *)Address)[ElementIndex]     = lua_tointeger(Lua, StackIndex);
   else if (Type & FD_WORD) {
      if (Type & FD_UNSIGNED) ((uint16_t *)Address)[ElementIndex] = lua_tointeger(Lua, StackIndex);
      else ((int16_t *)Address)[ElementIndex] = lua_tointeger(Lua, StackIndex);
   }
   else if (Type & FD_BYTE)   ((uint8_t *)Address)[ElementIndex] = lua_tointeger(Lua, StackIndex);
   else return false;

   return true;
}

template <typename T> static ERR table_values_to_vector(lua_State *Lua, int StackIndex, int Type, APTR Address)
{
   auto &dest = ((kt::vector<T> *)Address)[0];
   size_t elements = lua_objlen(Lua, StackIndex);
   dest.resize(elements);
   for (size_t i = 0; i < elements; i++) {
      lua_rawgeti(Lua, StackIndex, i);
      if (not lua_isnumber(Lua, -1)) {
         lua_pop(Lua, 1);
         return ERR::InvalidType;
      }
      (void)write_primitive_field(Lua, dest.data(), Type, -1, int(i));
      lua_pop(Lua, 1);
   }
   return ERR::Okay;
}

template <typename T> static ERR array_values_to_vector(GCarray *Source, APTR Address)
{
   auto &dest = ((kt::vector<T> *)Address)[0];
   dest.resize(Source->len);
   if (Source->len) std::memcpy(dest.data(), Source->arraydata(), size_t(Source->len) * sizeof(T));
   return ERR::Okay;
}

static ERR value_to_cpp_vector(lua_State *Lua, int StackIndex, const struct_field &Field, APTR Address)
{
   const int type = Field.Type;
   if (lua_isarray(Lua, StackIndex)) {
      auto source = lua_toarray(Lua, StackIndex);
      if (type & FD_STRING) {
         if ((source->elemtype != AET::STR_GC) and (source->elemtype != AET::CSTR) and
               (source->elemtype != AET::STR_CPP)) return ERR::InvalidType;
         auto &dest = ((kt::vector<std::string> *)Address)[0];
         dest.resize(source->len);
         for (MSize i = 0; i < source->len; i++) {
            if (source->elemtype IS AET::STR_GC) {
               auto value = strref(source->get<GCRef>()[i]);
               dest[i].assign(strdata(value), value->len);
            }
            else if (source->elemtype IS AET::CSTR) dest[i] = source->get<CSTRING>()[i];
            else dest[i] = source->get<std::string>()[i];
         }
         return ERR::Okay;
      }
      if (source->elemtype != ff_to_aet(type, Field.NativeType)) return ERR::InvalidType;
      if (type & FD_FLOAT) return array_values_to_vector<float>(source, Address);
      if (type & FD_DOUBLE) return array_values_to_vector<double>(source, Address);
      if (type & FD_INT64) return array_values_to_vector<int64_t>(source, Address);
      if (type & FD_INT) return array_values_to_vector<int>(source, Address);
      if (type & FD_WORD) return array_values_to_vector<int16_t>(source, Address);
      if ((type & FD_BYTE) and Field.NativeType IS NativeStructType::Int8) {
         return array_values_to_vector<int8_t>(source, Address);
      }
      if (type & FD_BYTE) return array_values_to_vector<uint8_t>(source, Address);
      return ERR::NoSupport;
   }

   if (not lua_istable(Lua, StackIndex)) return ERR::InvalidType;
   if (type & FD_STRING) {
      auto &dest = ((kt::vector<std::string> *)Address)[0];
      size_t elements = lua_objlen(Lua, StackIndex);
      dest.resize(elements);
      for (size_t i = 0; i < elements; i++) {
         lua_rawgeti(Lua, StackIndex, i);
         size_t length = 0;
         auto value = lua_tolstring(Lua, -1, &length);
         if (not value) {
            lua_pop(Lua, 1);
            return ERR::InvalidType;
         }
         dest[i].assign(value, length);
         lua_pop(Lua, 1);
      }
      return ERR::Okay;
   }
   if (type & FD_FLOAT) return table_values_to_vector<float>(Lua, StackIndex, type, Address);
   if (type & FD_DOUBLE) return table_values_to_vector<double>(Lua, StackIndex, type, Address);
   if (type & FD_INT64) return table_values_to_vector<int64_t>(Lua, StackIndex, type, Address);
   if (type & FD_INT) return table_values_to_vector<int>(Lua, StackIndex, type, Address);
   if (type & FD_WORD) return table_values_to_vector<int16_t>(Lua, StackIndex, type, Address);
   if ((type & FD_BYTE) and Field.NativeType IS NativeStructType::Int8) {
      return table_values_to_vector<int8_t>(Lua, StackIndex, type, Address);
   }
   if (type & FD_BYTE) return table_values_to_vector<uint8_t>(Lua, StackIndex, type, Address);
   return ERR::NoSupport;
}

//********************************************************************************************************************

// Convert a Lua table to a C structure.  The returned structure is owned by a unique pointer.
// Types that would require an allocation are not supported - our goal is to support primitive structs and anything
// more complex than that should really be managed as an object.

[[nodiscard]] ERR table_to_struct(lua_State *Lua, std::string_view StructName, std::unique_ptr<uint8_t[]> &Result)
{
   kt::Log log(__FUNCTION__);

   Result.reset();

   if (not lua_istable(Lua, -1)) return log.warning(ERR::TypeMismatch);

   auto def = find_struct(Lua, StructName);
   if (not def) return ERR::Search;

   auto &struct_def = *def;

   auto memory = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[struct_def.Size]());
   if (not memory) return ERR::AllocMemory;

   construct_struct_cpp_strings(Lua, struct_def, memory.get());

   lua_pushnil(Lua); // Access first key for lua_next()
   while (lua_next(Lua, -2) != 0) { // Pops the current key and pushes the k,v pair.
      if (auto field_name = lua_tostring(Lua, -2)) {
         // Find matching field in struct definition
         auto field_hash = strihash(field_name);
         for (auto &field : struct_def.Fields) {
            if (field.nameHash() IS field_hash) {
               APTR address = memory.get() + field.Offset;
               auto type = field.Type;

               if (type & (FD_ARRAY|FD_VECTOR)) {
                  if ((type & FD_VECTOR) and (type & FD_STRUCT)) {
                     if (not field.TrivialElements) {
                        destroy_struct_cpp_strings(Lua, struct_def, memory.get());
                        return ERR::NoSupport;
                     }
                     auto field_def = field.StructDefinition ? field.StructDefinition :
                        find_struct_reference(Lua, struct_def, field.StructRef);
                     if ((not field_def) or (not lua_isarray(Lua, -1))) {
                        destroy_struct_cpp_strings(Lua, struct_def, memory.get());
                        return ERR::InvalidType;
                     }
                     auto source = lua_toarray(Lua, -1);
                     if (source->elemtype != AET::TABLE) {
                        destroy_struct_cpp_strings(Lua, struct_def, memory.get());
                        return ERR::InvalidType;
                     }
                     std::vector<uint8_t> serial(size_t(source->len) * field.ElementStride);
                     for (MSize i = 0; i < source->len; i++) {
                        auto table = tabref(source->get<GCRef>()[i]);
                        settabV(Lua, Lua->top++, table);
                        std::unique_ptr<uint8_t[]> element;
                        auto error = table_to_struct(Lua, field_def->Name, element);
                        lua_pop(Lua, 1);
                        if (error != ERR::Okay) {
                           destroy_struct_cpp_strings(Lua, struct_def, memory.get());
                           return error;
                        }
                        std::memcpy(serial.data() + (size_t(i) * field.ElementStride), element.get(),
                           field.ElementStride);
                     }
                     assign_trivial_struct_vector(address, serial.data(), source->len, field.ElementStride);
                  }
                  else if (type & FD_VECTOR) {
                     if (auto error = value_to_cpp_vector(Lua, -1, field, address); error != ERR::Okay) {
                        destroy_struct_cpp_strings(Lua, struct_def, memory.get());
                        return error;
                     }
                  }
                  else if (field.ArraySize IS - 1); // Pointer to a null-terminated array
                  else if (lua_istable(Lua, -1) and is_primitive_field(type)) { // Embedded, fixed size array
                     for (int i = 0; i < field.ArraySize; i++) {
                        lua_pushinteger(Lua, i);
                        lua_gettable(Lua, -2); // Get value at index
                        (void)write_primitive_field(Lua, address, type, -1, i);
                        lua_pop(Lua, 1); // Remove value
                     }
                  }
               }
               else if ((type & FD_STRING) and (type & FD_CPP)) {
                  size_t len;
                  auto str = lua_tolstring(Lua, -1, &len);
                  ((std::string *)address)[0].assign(str, len);
               }
               else if (type & (FD_STRING|FD_STRUCT|FD_POINTER));
               else (void)write_primitive_field(Lua, address, type, -1);
               break;
            }
         }
      }
      lua_pop(Lua, 1); // Remove value, keep key for next iteration
   }

   Result = std::move(memory);
   return ERR::Okay;
}

//********************************************************************************************************************

[[nodiscard]] ERR struct_to_table(lua_State *Lua, std::vector<lua_ref> &References, struct_record &StructDef, CPTR Address)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Struct: %s, Data: %p", StructDef.Name.c_str(), Address);

   if (not Address) { lua_pushnil(Lua); return ERR::NullArgs; }

   // Check if there is an existing struct table already associated with this address.  If so, return it
   // rather than creating another table.

   for (auto &rec : References) {
      if ((Address IS rec.Address) and (&StructDef IS rec.Def)) {
         lua_rawgeti(Lua, LUA_REGISTRYINDEX, rec.Ref);
         return ERR::Okay;
      }
   }

   lua_createtable(Lua, 0, StructDef.Fields.size()); // Create a new table on the stack.

   // Record the address associated with the newly created table.  This is necessary because there may be circular
   // references to it.

   int table_ref = luaL_ref(Lua, LUA_REGISTRYINDEX);
   References.push_back({ Address, &StructDef, table_ref });
#ifdef UNIT_TESTS
   ++glStructLiveReferences;
#endif
   lua_rawgeti(Lua, LUA_REGISTRYINDEX, table_ref); // Retrieve the struct table

   for (auto &field : StructDef.Fields) {
      lua_pushstring(Lua, field.Name.c_str());

      CPTR address = (int8_t *)Address + field.Offset;
      auto type = field.Type;
      struct_record *field_def = field.StructDefinition;
      if ((not field_def) and (type & FD_STRUCT) and (field.StructRef != 0)) {
         field_def = find_struct_reference(Lua, StructDef, field.StructRef);
      }

      if (type & (FD_ARRAY|FD_VECTOR)) {
         if (type & FD_VECTOR) { // kt::vector<ANY>
            if (type & FD_STRUCT) {
               if (field_def) {
                  make_struct_array(Lua, field_def->Name, int(trivial_struct_vector_size(address)),
                     trivial_struct_vector_data(address), field.ElementStride, field_def);
               }
               else lua_pushnil(Lua);
            }
            else if (type & FD_STRING) {
               auto vector = (kt::vector<std::string> *)(address);
               make_array(Lua, AET::STR_CPP, int(vector->size()), vector);
            }
            else {
               auto vector = (kt::vector<int> *)(address); // Uses int as a type-stable layout placeholder
               make_array(Lua, ff_to_aet(type, field.NativeType), vector->size(), vector->data());
            }
         }
         else if (field.ArraySize IS -1) { // Pointer to a null-terminated array.
            if (type & FD_STRUCT) {
               if (field_def) {
                  if (((CPTR *)address)[0]) {
                     make_any_array(Lua, type, field_def->Name, -1, ((CPTR *)address)[0], field_def);
                  }
                  else lua_pushnil(Lua);
               }
               else lua_pushnil(Lua);
            }
            else make_array(Lua, ff_to_aet(type, field.NativeType), -1, ((CPTR *)address)[0]);
         }
         else { // It's an embedded array of fixed size.
            if (type & FD_STRUCT) {
               if (field_def) {
                  make_struct_array(Lua, field_def->Name, field.ArraySize, address, 0, field_def);
               }
               else lua_pushnil(Lua);
            }
            else make_array(Lua, ff_to_aet(type, field.NativeType), field.ArraySize, address);
         }
      }
      else if (type & FD_STRUCT) {
         if (field_def) {
            if (type & FD_PTR) {
               if (((APTR *)address)[0]) {
                  (void)struct_to_table(Lua, References, *field_def, ((APTR *)address)[0]);
               }
               else lua_pushnil(Lua);
            }
            else (void)struct_to_table(Lua, References, *field_def, address);
         }
         else {
            log.msg("Struct reference $%.8x not found for field '%s'", field.StructRef, field.Name.c_str());
            lua_pushnil(Lua);
         }
      }
      else if (type & FD_STRING) {
         if (type & FD_CPP) lua_pushstring(Lua, ((std::string *)address)[0]);
         else lua_pushstring(Lua, ((STRING *)address)[0]);
      }
      else if (type & FD_OBJECT) push_object(Lua, ((OBJECTPTR *)address)[0]);
      else if (type & FD_POINTER) {
         if (((APTR *)address)[0]) lua_pushlightuserdata(Lua, ((APTR *)address)[0]);
         else lua_pushnil(Lua);
      }
      else if (type & FD_FLOAT)  lua_pushnumber(Lua, ((float *)address)[0]);
      else if (type & FD_DOUBLE) lua_pushnumber(Lua, ((double *)address)[0]);
      else if (type & FD_INT64)  lua_pushnumber(Lua, ((int64_t *)address)[0]);
      else if (type & FD_INT)    lua_pushinteger(Lua, ((int *)address)[0]);
      else if (type & FD_WORD) {
         if (type & FD_UNSIGNED) lua_pushinteger(Lua, ((uint16_t *)address)[0]);
         else lua_pushinteger(Lua, ((int16_t *)address)[0]);
      }
      else if ((type & FD_BYTE) and field.NativeType IS NativeStructType::Int8) {
         lua_pushinteger(Lua, ((int8_t *)address)[0]);
      }
      else if (type & FD_BYTE) lua_pushinteger(Lua, ((uint8_t *)address)[0]);
      else lua_pushnil(Lua);

      lua_settable(Lua, -3);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Use this for creating a struct on the Lua stack.

GCstruct * push_struct(extTiri *Self, APTR Address, std::string_view StructName, bool Deallocate, bool AllowEmpty,
   OBJECTPTR Lifecycle)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Struct: %s, Address: %p, Deallocate: %d", StructName.data(), Address, Deallocate);

   if (auto def = find_struct(Self->Lua, StructName)) {
      return push_struct_def(Self->Lua, Address, *def, Deallocate, Lifecycle);
   }
   else if (AllowEmpty) {
      // The AllowEmpty option is useful in situations where a successful API call returns a structure that is strictly
      // unavailable to Tiri.  Rather than return NULL because the structure isn't in the dictionary, we return
      // an empty structure declaration.

      static struct_record empty("");
      return push_struct_def(Self->Lua, Address, empty, Deallocate, Lifecycle);
   }
   else {
      if (Deallocate) FreeResource(Address);
      log.warning("Unrecognised struct '%s'", StructName.data());
      return nullptr;
   }
}

//********************************************************************************************************************
// Use this for creating a struct on the Lua stack from a pre-hashed structure name.

GCstruct * push_struct(extTiri *Self, APTR Address, uint32_t StructKey, bool Deallocate, bool AllowEmpty,
   OBJECTPTR Lifecycle)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Struct: $%.8x, Address: %p, Deallocate: %d", StructKey, Address, Deallocate);

   if (auto def = find_struct(Self->Lua, StructKey)) {
      return push_struct_def(Self->Lua, Address, *def, Deallocate, Lifecycle);
   }
   else if (AllowEmpty) {
      // Preserve the name-based overload's fallback for resource structs that Tiri does not know about.

      static struct_record empty("");
      return push_struct_def(Self->Lua, Address, empty, Deallocate, Lifecycle);
   }
   else {
      if (Deallocate) FreeResource(Address);
      log.warning("Unrecognised struct hash $%.8x", StructKey);
      return nullptr;
   }
}

//********************************************************************************************************************
// structdef = MAKESTRUCT(Name, Sequence)
//
// This function makes a structure definition which can be passed to struct.new()
//
// Tiri clients should use the `struct` parser instead of this function.  It is exposed only for the purpose of
// testing the parsing of IDL struct definitions.

int MAKESTRUCT(lua_State *Lua)
{
   CSTRING sequence, name;
   if (not (name = lua_tostring(Lua, 1))) luaL_argerror(Lua, 1, "Structure name required.");
   else if (not (sequence = lua_tostring(Lua, 2))) luaL_argerror(Lua, 2, "Structure definition required.");
   else if (ERR error = make_struct(Lua, name, sequence); error != ERR::Okay) {
      luaL_error(Lua, error, "Failed to register structure '%s'.", name);
   }
   return 0;
}

//********************************************************************************************************************
// Camel-case adjustment for field names.  Has to handle cases like IPAddress -> ipAddress; ID -> id

static void make_camel_case(std::string &String)
{
   if (String.empty()) return;

   if ((String[0] >= 'A') and (String[0] <= 'Z')) String[0] = String[0] - 'A' + 'a';

   if (String.size() < 2) return;

   if ((String[1] >= 'A') and (String[1] <= 'Z')) {
      size_t f;
      for (f=2; f < String.size(); f++) {
         if ((String[f] >= 'a') and (String[f] <= 'z')) break;
      }

      if (f >= String.size()) { // Field is all upper-case
         for (size_t f=0; f < String.size(); f++) {
            if ((String[f] >= 'A') and (String[f] <= 'Z')) String[f] = String[f] - 'A' + 'a';
         }
      }
      else {
         bool lcase = false;
         for (f=1; f < String.size(); f++) {
            if ((String[f] >= 'A') and (String[f] <= 'Z')) {
               if (lcase) String[f-1] = String[f-1] - 'A' + 'a';
               lcase = true;
            }
            else break;
         }
      }
   }
}

//********************************************************************************************************************
// Computes the alignment, offset and storage size for a single field.  Shared by the MAKESTRUCT sequence parser and
// the declarative `struct` statement so that both paths produce byte-identical layouts.
//
// ArraySize is the caller's element count and is interpreted alongside FD_ARRAY as follows:
//
//   No FD_ARRAY          Scalar field.  Callers pass 1; the recorded ArraySize is always 1.
//   FD_ARRAY, count > 0  Fixed inline array occupying FieldSize * count bytes.
//   FD_ARRAY, count <= 0 Pointer to a null-terminated array (the sequence format's [0] suffix, or ptr<T[]> in a
//                        declaration).  Occupies one pointer and is recorded as -1, which downstream readers such
//                        as struct_to_table() test for to distinguish it from an inline array.
//
// NB: A scalar field is recorded as 1, never -1.  Only an explicit [0]/ptr<T[]> yields -1.

static int struct_field_alignment(const struct_field &Field, int Type, int FieldSize)
{
   if (Type & FD_VECTOR) return alignof(kt::vector<int>);
   if ((Type & FD_OBJECT) and (Type & FD_INT)) return alignof(OBJECTID);
   if (Type & (FD_POINTER|FD_OBJECT|FD_FUNCTION)) return alignof(APTR);
   if ((Type & FD_STRUCT) and (not (Type & FD_PTR)) and Field.StructDefinition) {
      return Field.StructDefinition->Alignment;
   }
   if (FieldSize >= 8) return 8;
   if (FieldSize >= 4) return 4;
   if (FieldSize >= 2) return 2;
   return 1;
}

static ERR layout_struct_field(struct_field &Field, int Type, int FieldSize, int ArraySize, int &Offset)
{
   int alignment = struct_field_alignment(Field, Type, FieldSize);
   Offset = (Offset + alignment - 1) & ~(alignment - 1);

   // Offset is a uint16_t in struct_field, so oversized fixed arrays must be rejected rather than silently wrapped.

   uint64_t storage_size = uint64_t(FieldSize);
   if (Type & FD_VECTOR) {
      storage_size = sizeof(kt::vector<int>);
   }
   else if (Type & FD_ARRAY) {
      storage_size = (ArraySize > 0) ? uint64_t(FieldSize) * uint64_t(ArraySize) : sizeof(APTR);
   }
   if ((uint64_t(Offset) + storage_size) > std::numeric_limits<uint16_t>::max()) {
      return ERR::OutOfRange;
   }

   Field.Offset = Offset;
   Field.Type = Type;
   Field.ArraySize = (Type & FD_ARRAY) ? (ArraySize ? ArraySize : -1) : 1;
   Offset += int(storage_size);
   return ERR::Okay;
}

static bool struct_is_trivial(lua_State *Lua, const struct_record &Record, std::string &Offending);

//********************************************************************************************************************
// The TypeName is optional and usually refers to the name of a struct.  The list is sorted by name for fast lookups.

[[nodiscard]] static ERR generate_structdef(lua_State *Lua, const std::string_view StructName, const std::string Sequence,
   struct_record &Record, int *StructSize)
{
   kt::Log log(__FUNCTION__);

   size_t pos = 0;
   int offset = 0;

   while (pos < Sequence.size()) {
      struct_field field;
      int type = 0, field_size;

      if (Sequence[pos] IS 'z') {
         type |= FD_CPP;
         pos++;
      }

      if (Sequence[pos] IS 'u') {
         type |= FD_UNSIGNED;
         pos++;
      }

      switch (Sequence[pos]) {
         case 'l': type |= FD_INT;      field_size = sizeof(int); break;
         case 'd': type |= FD_DOUBLE;   field_size = sizeof(double); break;
         case 'x': type |= FD_INT64;    field_size = sizeof(int64_t); break;
         case 'f': type |= FD_FLOAT;    field_size = sizeof(float); break;
         case 'r': type |= FD_FUNCTION; field_size = sizeof(FUNCTION); break;
         case 'w': type |= FD_WORD;     field_size = sizeof(int16_t); break;
         case 'b': type |= FD_BYTE;     field_size = sizeof(uint8_t); break;
         case 'c': type |= FD_BYTE|FD_CUSTOM; field_size = sizeof(uint8_t); break;
         case 'p': type |= FD_POINTER;  field_size = sizeof(APTR); break;

         case 'o': type |= FD_OBJECT;   field_size = sizeof(OBJECTPTR); break;

         case 's':
            type |= FD_STRING;
            if (type & FD_CPP) field_size = sizeof(std::string);
            else field_size = sizeof(STRING);
            break;

         case 'e': { // Embedded structure in the format "eName:Struct[Size]" where [Size] is optional.
            type |= FD_STRUCT;
            auto sep = Sequence.find_first_of(":,[", pos+1);
            if ((sep != std::string::npos) and (Sequence[sep] IS ':')) {
               sep++;
               auto end = Sequence.find_first_of(",[", sep);
               if (end IS std::string::npos) end = Sequence.size();
               auto name = Sequence.substr(sep, end-sep);

               if (auto def = glStructs.find(struct_key(name)); def != glStructs.end()) {
                  field_size = def->second.Size;
                  field.StructDefinition = &def->second;
                  break;
               }
               else {
                  log.warning("Failed to find referenced struct '%s'", name.c_str());
                  return ERR::NotFound;
               }
            }
            else return ERR::Syntax;
         }

         default:
            return ERR::Syntax;
      }

      pos++;

      auto i = Sequence.find_first_of(",[:", pos);
      if (i IS std::string::npos) i = Sequence.size();
      field.Name.assign(Sequence, pos, i-pos);
      pos = i;

      // If a struct reference follows the field name, output it and add FD_STRUCT to the type.

      if (Sequence[pos] IS ':') {
         pos++;
         auto i = Sequence.find_first_of(",[", pos);
         if (i IS std::string::npos) i = Sequence.size();
         std::string_view reference_name(Sequence.data() + pos, i - pos);
         field.StructRef = struct_key(reference_name);
         if (auto def = glStructs.find(field.StructRef); def != glStructs.end()) {
            field.StructDefinition = &def->second;
         }
         type |= FD_STRUCT;
         pos = i;
      }

      make_camel_case(field.Name);
      field.precomputeNameHash();

      // Manage fields that are based on fixed array sizes.  NOTE: An array size of zero, i.e. [0] is an indicator
      // that the field is a pointer to a null terminated array.
      //
      // The default of 1 applies to every field that has no '[' suffix, so a scalar reaches layout_struct_field()
      // with a count of 1 and FD_ARRAY unset.  Zero is only ever reached via an explicit [0].

      int array_size = 1;
      if (Sequence[pos] IS '[') {
         pos++;
         if (type & FD_CPP) { // The 'z' prefix combined with an array suffix identifies a kt::vector.
            type |= FD_VECTOR;
            if (not (type & FD_STRING)) type &= ~FD_CPP;
            field_size = sizeof(kt::vector<int>);
         }
         else {
            type |= FD_ARRAY;
            if ((Sequence[pos] >= '0') and (Sequence[pos] <= '9')) { // Sanity check
               array_size = strtol(Sequence.c_str() + pos, nullptr, 0);
            }
         }
         pos = Sequence.find_first_of("],", pos);
         if (pos IS std::string::npos) pos = Sequence.size();
         else pos++;
      }

      // Alignment and offset management

      if ((field_size >= 8) and (type != FD_STRUCT)) {
         if (offset & 7) {
            log.msg("%s", std::format("Warning: {}.{} ({} bytes) is mis-aligned.", StructName, field.Name, field_size).c_str());
         }
      }

      pos = Sequence.find(',', pos);

      if (auto error = layout_struct_field(field, type, field_size, array_size, offset); error != ERR::Okay) {
         return error;
      }

      Record.Alignment = std::max(Record.Alignment, struct_field_alignment(field, type, field_size));

      log.trace("Added field %s @ offset %d", field.Name.c_str(), field.Offset);

      Record.Fields.push_back(field);

      while ((pos < Sequence.size()) and ((Sequence[pos] <= 0x20) or (Sequence[pos] IS ','))) pos++;
   }

   for (auto &field : Record.Fields) {
      if ((field.Type & FD_VECTOR) and (field.Type & FD_STRUCT) and
            (not (field.Type & FD_PTR)) and field.StructDefinition) {
         field.ElementStride = field.StructDefinition->Size;
         std::string offending;
         field.TrivialElements = struct_is_trivial(Lua, *field.StructDefinition, offending);
      }
   }

   *StructSize = (offset + Record.Alignment - 1) & ~(Record.Alignment - 1);
   return ERR::Okay;
}

//********************************************************************************************************************
// Parse a struct definition and permanently store it in the glStructs dictionary.
//
// Lua may be null, and is null for module-definition parsing.  A null state confines embedded-structure resolution to
// the global dictionary, which is required because a process-wide definition must not be shaped by whichever state
// happened to request it first: a state-local `struct ... end` declaration shadowing the same name would otherwise
// change the published global layout for every other state.

[[nodiscard]] ERR make_struct(lua_State *Lua, std::string_view StructName, CSTRING Sequence)
{
   kt::Log log(__FUNCTION__);
   const std::lock_guard lock(glStructMutex);

   if (not Sequence) {
      log.warning("Missing struct name and/or definition.");
      return ERR::NullArgs;
   }

   if (not valid_struct_name(StructName)) {
      log.warning("Invalid structure name '%s'.", StructName.data());
      return ERR::Syntax;
   }

   const auto key = struct_key(StructName);
   auto [it, inserted] = glStructs.try_emplace(key, StructName);
   if (not inserted) {
      log.warning("Structure '%s' is already registered.", StructName.data());
      return ERR::Exists;
   }

   log.traceBranch("%s, %.50s", StructName.data(), Sequence);

   int computed_size = 0;
   if (auto error = generate_structdef(Lua, StructName, Sequence, it->second, &computed_size); error != ERR::Okay) {
      if (error IS ERR::BufferOverflow) log.warning("String too long - buffer overflow");
      else if (error IS ERR::Syntax) log.warning("Unsupported struct character in definition: %s", Sequence);
      else log.warning("Failed to make struct for %s, error: %s", StructName.data(), GetErrorMsg(error));
      glStructs.erase(it);
      return error;
   }

   if (auto size = glStructSizes->find(key); size != glStructSizes->end()) it->second.Size = size->second.Size;
   else it->second.Size = computed_size;

   return ERR::Okay;
}

//********************************************************************************************************************
// Withdraw a structure that was registered by make_struct().
//
// This exists to roll back a partially published module-definition batch.  It must only be used for structures whose
// publication is being undone within the same operation that registered them, because a structure that any script has
// already resolved cannot be withdrawn safely.

void remove_struct(std::string_view StructName)
{
   const std::lock_guard lock(glStructMutex);
   glStructs.erase(struct_key(StructName));
}

//********************************************************************************************************************
// Resolve declarative definitions in the active state before falling back to process-wide MAKESTRUCT definitions.

[[nodiscard]] struct_record * find_struct(lua_State *Lua, uint32_t Key)
{
   if (Lua) {
      if (auto found = Lua->struct_declarations.find(Key); found != Lua->struct_declarations.end()) {
         return &found->second;
      }
   }

   const std::lock_guard lock(glStructMutex);
   if (auto found = glStructs.find(Key); found != glStructs.end()) return &found->second;
   return nullptr;
}

[[nodiscard]] struct_record * find_struct(lua_State *Lua, std::string_view Name)
{
   return find_struct(Lua, struct_key(Name));
}

[[nodiscard]] struct_record * find_struct_reference(lua_State *Lua, const struct_record &Owner, uint32_t Key)
{
   {
      const std::lock_guard lock(glStructMutex);
      auto owner = glStructs.find(struct_key(Owner.Name));
      if ((owner != glStructs.end()) and (&owner->second IS &Owner)) {
         if (auto found = glStructs.find(Key); found != glStructs.end()) return &found->second;
         return nullptr;
      }
   }

   return find_struct(Lua, Key);
}

[[nodiscard]] struct_record * find_struct_reference(lua_State *Lua, const struct_record &Owner,
   std::string_view Name)
{
   return find_struct_reference(Lua, Owner, struct_key(Name));
}

// Returns the storage size of one element of a declared field, or 0 if the field's type cannot be resolved.
//
// The test order is significant.  A declared field may combine a storage flag with the type it refers to, e.g.
// ptr<int[]> is FD_POINTER|FD_INT|FD_ARRAY and ptr<Name> is FD_POINTER|FD_STRUCT.  Pointer-ness must therefore be
// tested before the scalar and struct types, otherwise a pointer field would be sized as its referent.

static int declared_field_size(lua_State *Lua, const struct_field &Field)
{
   if (Field.Type & FD_VECTOR) return sizeof(kt::vector<int>);
   // FD_PTR is an alias of FD_POINTER, so this only matches a struct embedded inline rather than by reference.

   if ((Field.Type & FD_STRUCT) and (not (Field.Type & FD_PTR))) {
      if (Field.StructDefinition) return Field.StructDefinition->Size;
      if (auto def = find_struct(Lua, Field.StructRef)) return def->Size;
      return 0;
   }
   if (Field.Type & FD_STRING) return (Field.Type & FD_CPP) ? int(sizeof(std::string)) : int(sizeof(STRING));
   if ((Field.Type & FD_OBJECT) and (Field.Type & FD_INT)) return sizeof(OBJECTID);
   if (Field.Type & FD_OBJECT) return sizeof(OBJECTPTR);
   if (Field.Type & FD_POINTER) return sizeof(APTR);
   if (Field.Type & FD_FUNCTION) return sizeof(FUNCTION);
   if (Field.Type & FD_DOUBLE) return sizeof(double);
   if (Field.Type & FD_INT64) return sizeof(int64_t);
   if (Field.Type & FD_FLOAT) return sizeof(float);
   if (Field.Type & FD_INT) return sizeof(int32_t);
   if (Field.Type & FD_WORD) return sizeof(int16_t);
   if (Field.Type & FD_BYTE) return sizeof(uint8_t);
   return 0;
}

static bool identical_struct_layout(const struct_record &Left, const struct_record &Right)
{
   if ((Left.Size != Right.Size) or (Left.Alignment != Right.Alignment) or
         (Left.Fields.size() != Right.Fields.size())) return false;
   for (size_t i = 0; i < Left.Fields.size(); i++) {
      const auto &left = Left.Fields[i];
      const auto &right = Right.Fields[i];
      if ((left.Name != right.Name) or (left.StructRef != right.StructRef) or
         (left.ObjectClassID != right.ObjectClassID) or (left.Offset != right.Offset) or
            (left.Type != right.Type)) return false;
      if (not left.ObjectClassName.empty() and not right.ObjectClassName.empty() and
          left.ObjectClassName != right.ObjectClassName) return false;
      if ((left.Type & FD_ARRAY) and (left.ArraySize != right.ArraySize)) return false;
      if (left.ElementStride != right.ElementStride) return false;
      if (left.TrivialElements != right.TrivialElements) return false;
      if ((left.NativeType != NativeStructType::Legacy) and (right.NativeType != NativeStructType::Legacy) and
            (left.NativeType != right.NativeType)) return false;
   }
   return true;
}

namespace {

constexpr uint32_t portable_struct_flags = FD_OBJECT|FD_STRUCT|FD_ARRAY|FD_CPP|FD_CUSTOM|FD_UNSIGNED|FD_VECTOR|
   FD_WORD|FD_STRING|FD_BYTE|FD_FUNCTION|FD_INT64|FD_POINTER|FD_FLOAT|FD_INT|FD_DOUBLE;

bool valid_portable_semantics(const struct_field &Field)
{
   uint32_t flags = uint32_t(Field.Type);
   if ((flags & ~portable_struct_flags) or ((flags & FD_ARRAY) and (flags & FD_VECTOR))) return false;
   const uint32_t modifiers = flags & (FD_ARRAY|FD_VECTOR);
   const uint32_t base = flags & ~(FD_ARRAY|FD_VECTOR);
   switch (Field.NativeType) {
      case NativeStructType::Bool: return base IS FD_BYTE;
      case NativeStructType::Char: return base IS (FD_BYTE|FD_CUSTOM);
      case NativeStructType::Int8:
      case NativeStructType::UInt8: return base IS FD_BYTE;
      case NativeStructType::Int16: return base IS FD_WORD;
      case NativeStructType::UInt16: return base IS (FD_WORD|FD_UNSIGNED);
      case NativeStructType::Int32: return base IS FD_INT;
      case NativeStructType::UInt32: return base IS (FD_INT|FD_UNSIGNED);
      case NativeStructType::Int64: return base IS FD_INT64;
      case NativeStructType::UInt64: return base IS (FD_INT64|FD_UNSIGNED);
      case NativeStructType::Float: return base IS FD_FLOAT;
      case NativeStructType::Double: return base IS FD_DOUBLE;
      case NativeStructType::String: return base IS (FD_STRING|FD_CPP);
      case NativeStructType::CStr: return base IS FD_STRING and not modifiers;
      case NativeStructType::Struct: return base IS FD_STRUCT;
      case NativeStructType::Object: return base IS FD_OBJECT and not modifiers;
      case NativeStructType::Function: return base IS FD_FUNCTION and not modifiers;
      case NativeStructType::Pointer: {
         if (not (base & FD_POINTER) or (base & (FD_CPP|FD_CUSTOM|FD_OBJECT|FD_FUNCTION|FD_STRING))) return false;
         uint32_t referent = base & ~uint32_t(FD_POINTER|FD_UNSIGNED);
         return referent IS 0 or referent IS FD_STRUCT or referent IS FD_BYTE or referent IS FD_WORD or
            referent IS FD_INT or referent IS FD_INT64 or referent IS FD_FLOAT or referent IS FD_DOUBLE;
      }
      case NativeStructType::Legacy: return false;
   }
   return false;
}

void manifest_uleb(std::vector<uint8_t> &Output, uint32_t Value)
{
   do {
      uint8_t byte = uint8_t(Value & 0x7f);
      Value >>= 7;
      if (Value) byte |= 0x80;
      Output.push_back(byte);
   } while (Value);
}

void manifest_string(std::vector<uint8_t> &Output, std::string_view Value)
{
   manifest_uleb(Output, uint32_t(Value.size()));
   Output.insert(Output.end(), Value.begin(), Value.end());
}

bool portable_field(const struct_field &Field)
{
   if (not valid_portable_semantics(Field)) return false;
   if ((Field.Type & FD_ARRAY) and Field.ArraySize <= 0 and
       not ((Field.Type & FD_POINTER) and Field.ArraySize IS -1)) return false;
   if ((Field.Type & FD_VECTOR) and Field.ArraySize != 1) return false;
   if ((Field.Type & FD_STRUCT) and Field.StructRef IS 0) return false;
   return true;
}

bool valid_field_name(std::string_view Name)
{
   if (Name.empty() or Name.find('\0') != std::string_view::npos) return false;
   uint8_t first = uint8_t(Name.front());
   if (not ((first >= 'A' and first <= 'Z') or (first >= 'a' and first <= 'z') or first IS '_')) return false;
   for (uint8_t value : Name.substr(1)) {
      if (not ((value >= 'A' and value <= 'Z') or (value >= 'a' and value <= 'z') or
          (value >= '0' and value <= '9') or value IS '_')) return false;
   }
   return true;
}

struct ManifestReader {
   const uint8_t *cursor;
   const uint8_t *end;

   bool byte(uint8_t &Value) {
      if (cursor >= end) return false;
      Value = *cursor++;
      return true;
   }

   bool uleb(uint32_t &Value) {
      Value = 0;
      for (unsigned shift = 0; shift <= 28; shift += 7) {
         uint8_t current;
         if (not byte(current) or (shift IS 28 and current > 0x0f)) return false;
         Value |= uint32_t(current & 0x7f) << shift;
         if (not (current & 0x80)) return true;
      }
      return false;
   }

   bool string(std::string &Value) {
      uint32_t size;
      if (not uleb(size) or size > uint32_t(end - cursor)) return false;
      Value.assign((const char *)cursor, size);
      cursor += size;
      return Value.find('\0') IS std::string::npos;
   }
};

}

//********************************************************************************************************************
// Build a dependency-first portable manifest for the state-local declarations used by one compilation unit.

ERR build_declared_struct_manifest(lua_State *Lua, const std::vector<std::string> &Roots,
   const std::vector<std::string> &Owned, bool DynamicReference, std::vector<uint8_t> &Manifest, std::string *Detail)
{
   Manifest.clear();
   if (not Lua) return ERR::NullArgs;

   std::vector<const struct_record *> ordered;
   std::unordered_map<std::string, uint8_t> visits;
   auto visit = [&](auto &Self, const struct_record &record) -> ERR {
      uint8_t &state = visits[record.Name];
      if (state IS 2) return ERR::Okay;
      if (state IS 1) {
         if (Detail) *Detail = std::format("Cyclic struct dependency involving '{}'", record.Name);
         return ERR::InvalidData;
      }
      state = 1;
      for (const auto &field : record.Fields) {
         if (not portable_field(field)) return ERR::NoSupport;
         if ((field.Type & FD_STRUCT) and field.StructRef) {
            auto dependency = field.StructDefinition ? field.StructDefinition :
               find_struct_reference(Lua, record, field.StructRef);
            if (not dependency) return ERR::NotFound;
            auto local = Lua->struct_declarations.find(struct_key(dependency->Name));
            if (local != Lua->struct_declarations.end() and &local->second IS dependency) {
               if (auto error = Self(Self, *dependency); error != ERR::Okay) return error;
            }
         }
      }
      state = 2;
      ordered.push_back(&record);
      return ERR::Okay;
   };

   std::vector<std::string> roots = Roots;
   if (DynamicReference) roots.insert(roots.end(), Owned.begin(), Owned.end());
   for (const auto &name : roots) {
      auto found = Lua->struct_declarations.find(struct_key(name));
      if (found IS Lua->struct_declarations.end() or found->second.Name != name) return ERR::NotFound;
      if (auto error = visit(visit, found->second); error != ERR::Okay) return error;
   }
   if (ordered.size() > STRUCT_MANIFEST_MAX_DEFINITIONS) return ERR::BufferOverflow;

   Manifest.push_back(STRUCT_MANIFEST_VERSION);
   manifest_uleb(Manifest, uint32_t(ordered.size()));
   uint32_t fields = 0;
   for (const auto *record : ordered) {
      if (record->Name.find('\0') != std::string::npos or not valid_struct_name(record->Name)) return ERR::InvalidData;
      fields += uint32_t(record->Fields.size());
      if (fields > STRUCT_MANIFEST_MAX_FIELDS) return ERR::BufferOverflow;
      manifest_string(Manifest, record->Name);
      manifest_uleb(Manifest, uint32_t(record->Fields.size()));
      for (const auto &field : record->Fields) {
         if (not valid_field_name(field.Name)) return ERR::InvalidData;
         manifest_string(Manifest, field.Name);
         Manifest.push_back(uint8_t(field.NativeType));
         manifest_uleb(Manifest, uint32_t(field.Type));
         manifest_uleb(Manifest, (field.Type & FD_ARRAY) ? uint32_t(field.ArraySize) : 0);
         std::string_view reference;
         if ((field.Type & FD_STRUCT) and field.StructRef) {
            auto definition = field.StructDefinition ? field.StructDefinition :
               find_struct_reference(Lua, *record, field.StructRef);
            if (not definition) return ERR::NotFound;
            reference = definition->Name;
         }
         manifest_string(Manifest, reference);
         std::string_view class_name = field.ObjectClassName;
         if (class_name.empty() and field.ObjectClassID != CLASSID::NIL) {
            CSTRING resolved = ResolveClassID(field.ObjectClassID);
            if (not resolved) return ERR::NoSupport;
            class_name = resolved;
         }
         manifest_string(Manifest, class_name);
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Validate and publish a portable manifest.  Insertions are reported to the caller for whole-load rollback.

ERR load_declared_struct_manifest(lua_State *Lua, std::string_view Manifest, std::vector<uint32_t> &Inserted,
   std::string *Detail)
{
   if (not Lua or Manifest.empty()) return ERR::NullArgs;
   ManifestReader reader { (const uint8_t *)Manifest.data(), (const uint8_t *)Manifest.data() + Manifest.size() };
   uint8_t version;
   uint32_t count;
   if (not reader.byte(version) or version != STRUCT_MANIFEST_VERSION or not reader.uleb(count) or
       count > STRUCT_MANIFEST_MAX_DEFINITIONS) return ERR::InvalidData;

   struct PendingRecord {
      struct_record record;
      std::vector<std::string> references;
   };
   std::vector<PendingRecord> records;
   records.reserve(count);
   uint32_t total_fields = 0;
   std::unordered_set<std::string> names;
   std::unordered_map<uint32_t, std::string> keys;
   for (uint32_t i = 0; i < count; ++i) {
      std::string name;
      uint32_t field_count;
      if (not reader.string(name) or not valid_struct_name(name) or not names.insert(name).second or
          not reader.uleb(field_count) or field_count IS 0 or
          field_count > STRUCT_MANIFEST_MAX_FIELDS - total_fields) {
         if (Detail) *Detail = std::format("Invalid definition {} ('{}')", i, name);
         return ERR::InvalidData;
      }
      auto [key, unique_key] = keys.emplace(struct_key(name), name);
      if (not unique_key and key->second != name) {
         if (Detail) *Detail = std::format("Struct names '{}' and '{}' have an ambiguous key", key->second, name);
         return ERR::InvalidData;
      }
      total_fields += field_count;
      PendingRecord pending;
      pending.record.Name = name;
      pending.references.reserve(field_count);
      std::unordered_set<uint32_t> field_names;
      for (uint32_t f = 0; f < field_count; ++f) {
         struct_field field;
         uint8_t native;
         uint32_t flags;
         uint32_t dimension;
         std::string reference;
         if (not reader.string(field.Name) or not valid_field_name(field.Name) or
             not field_names.insert(kt::strihash(field.Name)).second or not reader.byte(native) or
             native <= uint8_t(NativeStructType::Legacy) or native > uint8_t(NativeStructType::Function) or
             not reader.uleb(flags) or (flags & ~portable_struct_flags) or not reader.uleb(dimension) or
             not reader.string(reference) or not reader.string(field.ObjectClassName)) {
            if (Detail) *Detail = std::format("Invalid field {} in definition {}", f, i);
            return ERR::InvalidData;
         }
         field.NativeType = NativeStructType(native);
         field.Type = int(flags);
         bool pointer_array = (flags & FD_POINTER) and dimension IS 0xffffffffu;
         field.ArraySize = (flags & FD_ARRAY) ? (pointer_array ? -1 : int(dimension)) :
            (flags & FD_VECTOR) ? 1 : 0;
         if (((flags & FD_ARRAY) and (dimension IS 0 or
              (dimension > uint32_t(INT_MAX) and not pointer_array))) or
             (not (flags & FD_ARRAY) and dimension != 0)) {
            if (Detail) *Detail = std::format("Invalid flags or dimension for '{}.{}'", name, field.Name);
            return ERR::InvalidData;
         }
         if (flags & FD_STRUCT) {
            if (not valid_struct_name(reference)) {
               if (Detail) *Detail = std::format("Invalid reference for '{}.{}'", name, field.Name);
               return ERR::InvalidData;
            }
            field.StructRef = struct_key(reference);
         }
         else if (not reference.empty()) return ERR::InvalidData;
         if (not portable_field(field)) {
            if (Detail) *Detail = std::format("Unsupported semantics for '{}.{}'", name, field.Name);
            return ERR::InvalidData;
         }
         if (flags & FD_OBJECT) {
            if (not field.ObjectClassName.empty()) {
               if (not valid_struct_name(field.ObjectClassName)) return ERR::InvalidData;
               field.ObjectClassID = CLASSID(kt::strihash(field.ObjectClassName));
            }
         }
         else if (not field.ObjectClassName.empty()) return ERR::InvalidData;
         field.precomputeNameHash();
         pending.record.Fields.push_back(std::move(field));
         pending.references.push_back(std::move(reference));
      }
      records.push_back(std::move(pending));
   }
   if (reader.cursor != reader.end) {
      if (Detail) *Detail = "Trailing struct manifest bytes";
      return ERR::InvalidData;
   }

   std::unordered_set<std::string> available;
   for (auto &pending : records) {
      for (size_t i = 0; i < pending.record.Fields.size(); ++i) {
         auto &reference = pending.references[i];
         if (reference.empty()) continue;
         if (not available.contains(reference)) {
            if (names.contains(reference)) {
               if (Detail) *Detail = std::format("Out-of-order dependency '{}'", reference);
               return ERR::InvalidData;
            }
            auto external = find_struct(Lua, reference);
            if (not external or external->Name != reference) {
               if (Detail) *Detail = std::format("Missing or out-of-order dependency '{}'", reference);
               return ERR::NotFound;
            }
         }
      }
      available.insert(pending.record.Name);
   }

   for (auto &pending : records) {
      std::string name = pending.record.Name;
      for (size_t i = 0; i < pending.record.Fields.size(); ++i) {
         auto &field = pending.record.Fields[i];
         if (pending.references[i].empty()) continue;
         auto dependency = find_struct(Lua, pending.references[i]);
         if (not dependency or dependency->Name != pending.references[i]) return ERR::NotFound;
         field.StructDefinition = dependency;
      }
      bool inserted = false;
      const struct_record *existing = nullptr;
      ERR error = register_declared_struct(Lua, std::move(pending.record), &inserted, &existing, Detail);
      if (error != ERR::Okay) return error;
      if (inserted) Inserted.push_back(struct_key(name));
   }
   return ERR::Okay;
}

// Register a parser-built declaration.  Keeping layout calculation here ensures declarative definitions use the
// same alignment and embedded-structure rules as MAKESTRUCT.

static bool struct_is_trivial(lua_State *Lua, const struct_record &Record, std::string &Offending)
{
   for (auto &field : Record.Fields) {
      if (((field.Type & FD_CPP) and (field.Type & FD_STRING)) or (field.Type & FD_VECTOR)) {
         Offending = field.Name;
         return false;
      }
      if ((field.Type & FD_STRUCT) and (not (field.Type & FD_PTR))) {
         auto child = field.StructDefinition ? field.StructDefinition :
            find_struct_reference(Lua, Record, field.StructRef);
         if (child and not struct_is_trivial(Lua, *child, Offending)) {
            Offending = field.Name + "." + Offending;
            return false;
         }
      }
   }
   return true;
}

[[nodiscard]] ERR register_declared_struct(lua_State *Lua, struct_record &&Record, bool *Inserted,
   const struct_record **Existing, std::string *Detail)
{
   if (Inserted) *Inserted = false;
   if (Existing) *Existing = nullptr;
   if ((not Lua) or Record.Name.empty() or Record.Fields.empty()) return ERR::NullArgs;
   if (not valid_struct_name(Record.Name)) return ERR::Syntax;

   const auto key = struct_key(Record.Name);

   int offset = 0;
   for (auto &field : Record.Fields) {
      int field_size = declared_field_size(Lua, field);
      if (field_size <= 0) return ERR::NotFound;

      if ((field.Type & FD_VECTOR) and (field.Type & FD_STRUCT) and
            (not (field.Type & FD_PTR))) {
         auto child = field.StructDefinition ? field.StructDefinition : find_struct(Lua, field.StructRef);
         std::string offending;
         if ((not child) or (not struct_is_trivial(Lua, *child, offending))) {
            if (Detail) {
               auto field_path = field.Name + (offending.empty() ? std::string() : "." + offending);
               *Detail = std::format("Struct array field '{}.{}' requires a trivially owned element layout",
                  Record.Name, field_path);
            }
            return ERR::NoSupport;
         }
         field.ElementStride = child->Size;
         field.TrivialElements = true;
      }

      // Mirror the sequence parser's convention: scalars carry a count of 1, and the parser's ArraySize of -1 for
      // ptr<T[]> passes straight through as the null-terminated-pointer marker.

      int array_size = (field.Type & FD_ARRAY) ? field.ArraySize : 1;
      if (auto error = layout_struct_field(field, field.Type, field_size, array_size, offset); error != ERR::Okay) {
         return error;
      }
      Record.Alignment = std::max(Record.Alignment, struct_field_alignment(field, field.Type, field_size));
      field.precomputeNameHash();
   }
   Record.Size = (offset + Record.Alignment - 1) & ~(Record.Alignment - 1);

   if (auto found = Lua->struct_declarations.find(key);
         found != Lua->struct_declarations.end()) {
      if (Existing) *Existing = &found->second;
      if (found->second.Name != Record.Name) return ERR::Exists;
      if (not identical_struct_layout(found->second, Record)) return ERR::Exists;
      return ERR::Okay;
   }

   Lua->struct_declarations.emplace(key, std::move(Record));
   if (Inserted) *Inserted = true;
   return ERR::Okay;
}
