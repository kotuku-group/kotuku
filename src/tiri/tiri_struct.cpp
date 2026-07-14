/*********************************************************************************************************************

To create a struct definition:                    MAKESTRUCT('XTag', 'Definition')
To create a struct from a registered definition:  xmltag = struct.new('XTag')
To create a struct with pre-configured values:    xmltag = struct.new('XTag', { name='Hello' })
To get the byte size of any structure definition: size = struct.size('XTag')
To get the total number of fields in a structure: #xmltag
To get the byte size of a created structure:      xmltag.structSize()

Acceptable field definitions:

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

TODO: Support for kt::vector

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

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lib.h"
#include "lj_obj.h"
#include "hashes.h"
#include "defs.h"

static constexpr int MAX_STRUCT_DEF = 2048; // Struct definitions are typically 100 - 400 bytes.

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

      if ((type & FD_STRUCT) and (not (type & FD_PTR)) and (not (type & FD_CPP)) and
            (not field->StructRef.empty())) {
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

      if ((type & FD_STRING) and (type & FD_CPP) and (not (type & FD_ARRAY))) {
         if (Construct) new (field_address) std::string();
         else ((std::string *)field_address)->~basic_string();
      }

      if ((type & FD_CPP) and (type & FD_ARRAY)) {
         if (type & FD_STRING) {
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
      std::vector<lua_ref> ref;
      auto error = struct_to_table(Lua, ref, *def, Address);
      unref_struct_references(Lua, ref);
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
      if ((rec.Ref != LUA_NOREF) and (rec.Ref != LUA_REFNIL)) luaL_unref(Lua, LUA_REGISTRYINDEX, rec.Ref);
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

//********************************************************************************************************************

// Convert a Lua table to a C structure.  The returned structure is owned by a unique pointer.
// Types that would require an allocation are not supported - our goal is to support primitive structs and anything
// more complex than that should really be managed as an object.

[[nodiscard]] ERR table_to_struct(lua_State *Lua, std::string_view StructName, std::unique_ptr<uint8_t[]> &Result)
{
   kt::Log log(__FUNCTION__);

   Result.reset();

   if (not lua_istable(Lua, -1)) return log.warning(ERR::WrongType);

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

               if (type & FD_ARRAY) {
                  if (type & FD_CPP);
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
   lua_rawgeti(Lua, LUA_REGISTRYINDEX, table_ref); // Retrieve the struct table

   for (auto &field : StructDef.Fields) {
      lua_pushstring(Lua, field.Name.c_str());

      CPTR address = (int8_t *)Address + field.Offset;
      auto type = field.Type;
      struct_record *field_def = field.StructDefinition;
      if ((not field_def) and (type & FD_STRUCT) and (not field.StructRef.empty())) {
         field_def = find_struct_reference(Lua, StructDef, field.StructRef);
      }

      if (type & FD_ARRAY) {
         if (type & FD_CPP) { // kt::vector<ANY>
            auto vector = (kt::vector<int> *)(address); // Uses int as a placeholder
            if (type & FD_STRUCT) {
               if (field_def) {
                  make_any_array(Lua, type, field.StructRef, vector->size(), vector->data(), field_def);
               }
               else lua_pushnil(Lua);
            }
            else make_array(Lua, ff_to_aet(type), vector->size(), vector->data());
         }
         else if (field.ArraySize IS -1) { // Pointer to a null-terminated array.
            if (type & FD_STRUCT) {
               if (field_def) {
                  if (((CPTR *)address)[0]) {
                     make_any_array(Lua, type, field.StructRef, -1, ((CPTR *)address)[0], field_def);
                  }
                  else lua_pushnil(Lua);
               }
               else lua_pushnil(Lua);
            }
            else make_array(Lua, ff_to_aet(type), -1, ((CPTR *)address)[0]);
         }
         else { // It's an embedded array of fixed size.
            if (type & FD_STRUCT) {
               if (field_def) {
                  make_struct_array(Lua, field.StructRef, field.ArraySize, address, 0, field_def);
               }
               else lua_pushnil(Lua);
            }
            else make_array(Lua, ff_to_aet(type), field.ArraySize, address);
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
            log.msg("Struct '%s' not found for field '%s'", field.StructRef.c_str(), field.Name.c_str());
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
      else if (type & FD_BYTE)   lua_pushinteger(Lua, ((uint8_t *)address)[0]);
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
      return push_struct_def(Self->Lua, Address, empty, false, Lifecycle);
   }
   else {
      if (Deallocate) FreeResource(Address);
      log.warning("Unrecognised struct '%s'", StructName.data());
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
   else if (make_struct(Lua->script, name, sequence) != ERR::Okay) {
      luaL_error(Lua, "Failed to register structure '%s'.", name);
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

static ERR layout_struct_field(struct_field &Field, int Type, int FieldSize, int ArraySize, int &Offset)
{
   if ((FieldSize >= 8) and (Type != FD_STRUCT)) Offset = ALIGN64(Offset);
   else if (FieldSize IS 4) Offset = ALIGN32(Offset);
   else if ((FieldSize IS 2) and (Offset & 1)) Offset++;

   // Offset is a uint16_t in struct_field, so oversized fixed arrays must be rejected rather than silently wrapped.

   uint64_t storage_size = uint64_t(FieldSize);
   if (Type & FD_ARRAY) {
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

//********************************************************************************************************************
// The TypeName is optional and usually refers to the name of a struct.  The list is sorted by name for fast lookups.

[[nodiscard]] static ERR generate_structdef(extTiri *Self, const std::string_view StructName, const std::string Sequence,
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
         field.StructRef.assign(Sequence, pos, i-pos);
         if (auto def = glStructs.find(struct_key(field.StructRef)); def != glStructs.end()) {
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
         type |= FD_ARRAY;
         if (type & FD_CPP) { // In the case of kt::vector, fixed array sizes are meaningless
            field_size = sizeof(kt::vector<int>);
         }
         else if ((Sequence[pos] >= '0') and (Sequence[pos] <= '9')) { // Sanity check
            array_size = strtol(Sequence.c_str() + pos, nullptr, 0);
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

      log.trace("Added field %s @ offset %d", field.Name.c_str(), field.Offset);

      Record.Fields.push_back(field);

      while ((pos < Sequence.size()) and ((Sequence[pos] <= 0x20) or (Sequence[pos] IS ','))) pos++;
   }

   *StructSize = offset;
   return ERR::Okay;
}

//********************************************************************************************************************
// Parse a struct definition and permanently store it in the glStructs dictionary.

[[nodiscard]] ERR make_struct(extTiri *Self, std::string_view StructName, CSTRING Sequence)
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
   if (auto error = generate_structdef(Self, StructName, Sequence, it->second, &computed_size); error != ERR::Okay) {
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
// Resolve declarative definitions in the active state before falling back to process-wide MAKESTRUCT definitions.

[[nodiscard]] struct_record * find_struct(lua_State *Lua, std::string_view Name)
{
   if (Lua) {
      if (auto found = Lua->struct_declarations.find(struct_key(Name)); found != Lua->struct_declarations.end()) {
         return &found->second;
      }
   }

   const std::lock_guard lock(glStructMutex);
   if (auto found = glStructs.find(struct_key(Name)); found != glStructs.end()) return &found->second;
   return nullptr;
}

[[nodiscard]] struct_record * find_struct_reference(lua_State *Lua, const struct_record &Owner,
   std::string_view Name)
{
   {
      const std::lock_guard lock(glStructMutex);
      auto owner = glStructs.find(struct_key(Owner.Name));
      if ((owner != glStructs.end()) and (&owner->second IS &Owner)) {
         if (auto found = glStructs.find(struct_key(Name)); found != glStructs.end()) return &found->second;
         return nullptr;
      }
   }

   return find_struct(Lua, Name);
}

// Returns the storage size of one element of a declared field, or 0 if the field's type cannot be resolved.
//
// The test order is significant.  A declared field may combine a storage flag with the type it refers to, e.g.
// ptr<int[]> is FD_POINTER|FD_INT|FD_ARRAY and ptr<Name> is FD_POINTER|FD_STRUCT.  Pointer-ness must therefore be
// tested before the scalar and struct types, otherwise a pointer field would be sized as its referent.

static int declared_field_size(lua_State *Lua, const struct_field &Field)
{
   // FD_PTR is an alias of FD_POINTER, so this only matches a struct embedded inline rather than by reference.

   if ((Field.Type & FD_STRUCT) and (not (Field.Type & FD_PTR))) {
      if (Field.StructDefinition) return Field.StructDefinition->Size;
      if (auto def = find_struct(Lua, Field.StructRef)) return def->Size;
      return 0;
   }
   if (Field.Type & FD_STRING) return (Field.Type & FD_CPP) ? int(sizeof(std::string)) : int(sizeof(STRING));
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
   if ((Left.Size != Right.Size) or (Left.Fields.size() != Right.Fields.size())) return false;
   for (size_t i = 0; i < Left.Fields.size(); i++) {
      const auto &left = Left.Fields[i];
      const auto &right = Right.Fields[i];
      if ((left.Name != right.Name) or (left.StructRef != right.StructRef) or
         (left.ObjectClassID != right.ObjectClassID) or (left.Offset != right.Offset) or
            (left.Type != right.Type)) return false;
      if ((left.Type & FD_ARRAY) and (left.ArraySize != right.ArraySize)) return false;
      if ((left.NativeType != NativeStructType::Legacy) and (right.NativeType != NativeStructType::Legacy) and
            (left.NativeType != right.NativeType)) return false;
   }
   return true;
}

// Register a parser-built declaration.  Keeping layout calculation here ensures declarative definitions use the
// same alignment and embedded-structure rules as MAKESTRUCT.

[[nodiscard]] ERR register_declared_struct(lua_State *Lua, struct_record &&Record, bool *Inserted,
   const struct_record **Existing)
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

      // Mirror the sequence parser's convention: scalars carry a count of 1, and the parser's ArraySize of -1 for
      // ptr<T[]> passes straight through as the null-terminated-pointer marker.

      int array_size = (field.Type & FD_ARRAY) ? field.ArraySize : 1;
      if (auto error = layout_struct_field(field, field.Type, field_size, array_size, offset); error != ERR::Okay) {
         return error;
      }
      field.precomputeNameHash();
   }
   Record.Size = offset;

   if (auto found = Lua->struct_declarations.find(key);
         found != Lua->struct_declarations.end()) {
      if (Existing) *Existing = &found->second;
      if (not identical_struct_layout(found->second, Record)) return ERR::Exists;
      return ERR::Okay;
   }

   Lua->struct_declarations.emplace(key, std::move(Record));
   if (Inserted) *Inserted = true;
   return ERR::Okay;
}
