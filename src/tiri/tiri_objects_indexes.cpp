// Refer: lib_object.cpp

//********************************************************************************************************************

static ERR lua_string_view(lua_State *Lua, int ValueIndex, std::string_view &Value)
{
   size_t size = 0;
   if (auto cstr = lua_tolstring(Lua, ValueIndex, &size)) {
      Value = std::string_view{cstr, size};
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

static ERR object_set_string(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   std::string_view value;
   if (auto error = lua_string_view(Lua, ValueIndex, value); error != ERR::Okay) return error;
   return Object->set(Field->FieldID, value);
}

//********************************************************************************************************************

static ERR set_array_from_table(lua_State *Lua, OBJECTPTR Object, const Field *Field, int Values, int total)
{
   if (Field->Flags & FD_INT) {
      kt::vector<int> values((size_t)total);
      for (lua_pushnil(Lua); lua_next(Lua, Values); lua_pop(Lua, 1)) {
         int index = lua_tointeger(Lua, -2) - 1;
         if ((index >= 0) and (index < total)) {
            values[index] = lua_tointeger(Lua, -1);
         }
      }
      return Object->set(Field->FieldID, values);
   }
   else if (Field->Flags & FD_STRING) {
      kt::vector<std::string> values((size_t)total);
      for (lua_pushnil(Lua); lua_next(Lua, Values); lua_pop(Lua, 1)) {
         int index = lua_tointeger(Lua, -2) - 1;
         if ((index >= 0) and (index < total)) {
            values[index] = lua_tostring(Lua, -1);
         }
      }
      return Object->set(Field->FieldID, values);
   }
   else if (Field->Flags & FD_STRUCT) {
      // Array structs can be set if the Lua table consists of Tiri.struct types.

      if (auto def = glStructs.find(std::string_view((CSTRING)Field->Arg)); def != glStructs.end()) {
         int aligned_size = ALIGN64(def->second.Size);
         auto structbuf = std::make_unique<uint8_t[]>(total * aligned_size);

         for (lua_pushnil(Lua); lua_next(Lua, Values); lua_pop(Lua, 1)) {
            int index = lua_tointeger(Lua, -2);
            if ((index >= 0) and (index < total)) {
               APTR sti = structbuf.get() + (aligned_size * index);
               int type = lua_type(Lua, -1);
               if (type IS LUA_TTABLE) {
                  lua_pop(Lua, 2);
                  return ERR::SetValueNotArray;
               }
               else if (type IS LUA_TUSERDATA) {
                  if (auto fs = (fstruct *)get_meta(Lua, -1, "Tiri.struct")) {
                     copymem(fs->Data, sti, fs->StructSize);
                  }
               }
               else {
                  lua_pop(Lua, 2);
                  return ERR::SetValueNotArray;
               }
            }
         }

         return Object->set(Field->FieldID, structbuf.get(), total, FD_STRUCT);
      }
      else return ERR::SetValueNotArray;
   }
   else return ERR::SetValueNotArray;
}

//********************************************************************************************************************
// Converts a CSV string into a raw array.  Returns element count written

static int parse_csv_array(std::string_view String, int Flags, APTR Dest)
{
   int i;
   for (i=0; not String.empty(); i++) {
      while ((not String.empty()) and (not std::isdigit((unsigned char)String.front())) and (String.front() != '-')) {
         String.remove_prefix(1);
      }

      if (String.empty()) break;

      std::string buffer(String);
      char *end = nullptr;
      if (Flags & FD_INT)         ((int *)Dest)[i]     = strtol(buffer.c_str(), &end, 0);
      else if (Flags & FD_INT64)  ((int64_t *)Dest)[i] = strtol(buffer.c_str(), &end, 0);
      else if (Flags & FD_DOUBLE) ((double *)Dest)[i]  = strtod(buffer.c_str(), &end);
      else if (Flags & FD_BYTE)   ((uint8_t *)Dest)[i] = strtol(buffer.c_str(), &end, 0);
      else if (Flags & FD_FLOAT)  ((float *)Dest)[i]   = strtod(buffer.c_str(), &end);
      else if (Flags & FD_WORD)   ((int16_t *)Dest)[i] = strtol(buffer.c_str(), &end, 0);
      else if (Flags & FD_STRING) { // Not feasible to convert a string into an array of strings
         kt::Log().warning(ERR::InvalidType);
         return 0;
      }
      else {
         kt::Log().warning(ERR::InvalidType);
         return 0;
      }

      const auto consumed = size_t(end - buffer.c_str());
      if (not consumed) break;
      if (consumed >= String.size()) String = {};
      else String.remove_prefix(consumed);
   }
   return i;
}

//********************************************************************************************************************

static ERR object_set_array(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   auto type = lua_type(Lua, ValueIndex);

   if (type IS LUA_TSTRING) { // Treat the source as a CSV field.  Works for primitives only
      if (Field->Flags & (FD_BYTE|FD_WORD|FD_FLOAT|FD_INT|FD_INT64|FD_DOUBLE)) {
         std::string_view source = lua_tostring(Lua, ValueIndex);

         auto buffer_size = source.empty() ? 1 : source.size() * 8;
         if (APTR arraybuffer = malloc(buffer_size)) {
            auto total = parse_csv_array(source, Field->Flags, arraybuffer);

            ERR error;
            if (Field->SetValue) error = ((ERR (*)(APTR, APTR, int))(Field->SetValue))(Object, arraybuffer, total);
            else if (Field->Arg > 0) { // An arg value indicates an embedded fixed-size array
               if (total > Field->Arg) total = Field->Arg;
               error = Object->set(Field->FieldID, arraybuffer, total, Field->Flags);
            }
            else error = ERR::FieldTypeMismatch;

            free(arraybuffer);
            return error;
         }
         else return ERR::AllocMemory;
      }
      else return ERR::FieldTypeMismatch;
   }
   else if (type IS LUA_TTABLE) {
      lua_settop(Lua, ValueIndex);
      int t = lua_gettop(Lua);
      int total = lua_objlen(Lua, t);

      if (total < 1024) {
         return set_array_from_table(Lua, Object, Field, t, total);
      }
      else return ERR::BufferOverflow;
   }
   else if (type IS LUA_TARRAY) {
      GCarray *arr = arrayV(Lua, ValueIndex);
      return Object->set(Field->FieldID, arr->arraydata(), arr->len, arr->type_flags());
   }
   else return ERR::SetValueNotArray;
}

static ERR object_set_function(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   int type = lua_type(Lua, ValueIndex);
   if (type IS LUA_TSTRING) {
      lua_getglobal(Lua, lua_tostring(Lua, ValueIndex));
      auto func = FUNCTION(Lua->script, luaL_ref(Lua, LUA_REGISTRYINDEX));
      return Object->set(Field->FieldID, &func);
   }
   else if (type IS LUA_TFUNCTION) {
      lua_pushvalue(Lua, ValueIndex);
      auto func = FUNCTION(Lua->script, luaL_ref(Lua, LUA_REGISTRYINDEX));
      return Object->set(Field->FieldID, &func);
   }
   else return ERR::SetValueNotFunction;
}

static ERR object_set_object(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   if (auto def = lua_toobject(Lua, ValueIndex)) {
      if (auto ptr_obj = access_object(def)) {
         ERR error = Object->set(Field->FieldID, ptr_obj);
         release_object(def);
         return error;
      }
      else return ERR::AccessObject;
   }
   else return Object->set(Field->FieldID, (APTR)nullptr);
}

static ERR object_set_ptr(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   auto type = lua_type(Lua, ValueIndex);

   if (type IS LUA_TSTRING) {
      return object_set_string(Lua, Object, Field, ValueIndex);
   }
   else if (type IS LUA_TNUMBER) {
      if (Field->Flags & FD_STRING) {
         return object_set_string(Lua, Object, Field, ValueIndex);
      }
      else if (lua_tointeger(Lua, ValueIndex) IS 0) {
         // Setting pointer fields with numbers is only allowed if that number evaluates to zero (NULL)
         return Object->set(Field->FieldID, (APTR)nullptr);
      }
      else return ERR::SetValueNotPointer;
   }
   else if (type IS LUA_TARRAY) {
      GCarray *arr = arrayV(Lua, ValueIndex);
      return Object->set(Field->FieldID, arr->arraydata());
   }
   else if (auto fstruct = (struct fstruct *)get_meta(Lua, ValueIndex, "Tiri.struct")) {
      return Object->set(Field->FieldID, fstruct->Data);
   }
   else if (type IS LUA_TNIL) {
      return Object->set(Field->FieldID, (APTR)nullptr);
   }
   else return ERR::SetValueNotPointer;
}

static ERR object_set_cppstring(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   auto type = lua_type(Lua, ValueIndex);
   if (type IS LUA_TNIL) return Object->set(Field->FieldID, std::string_view());
   else return object_set_string(Lua, Object, Field, ValueIndex);
}

static ERR object_set_double(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      case LUA_TNUMBER:
         return Object->set(Field->FieldID, lua_tonumber(Lua, ValueIndex));

      case LUA_TSTRING: // Allow string conversion to a number
         return object_set_string(Lua, Object, Field, ValueIndex);

      case LUA_TNIL: // Setting a numeric with nil does nothing.  Use zero to be explicit.
         return ERR::Okay;

      default:
         return ERR::SetValueNotNumeric;
   }
}

static ERR object_set_lookup(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      case LUA_TNUMBER: return Object->set(Field->FieldID, (int)lua_tointeger(Lua, ValueIndex));
      case LUA_TSTRING: return object_set_string(Lua, Object, Field, ValueIndex);
      default: return ERR::SetValueNotLookup;
   }
}

static ERR object_set_oid(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      default:          return ERR::SetValueNotObject;
      case LUA_TNUMBER: return Object->set(Field->FieldID, (OBJECTID)lua_tointeger(Lua, ValueIndex));
      case LUA_TNIL:    return Object->set(Field->FieldID, 0);

      case LUA_TOBJECT: {
         auto def = lua_toobject(Lua, ValueIndex);
         return Object->set(Field->FieldID, def->uid);
      }

      case LUA_TSTRING: {
         OBJECTID id;
         std::string_view name;
         if (auto error = lua_string_view(Lua, ValueIndex, name); error != ERR::Okay) return error;
         if (!FindObject(name, CLASSID::NIL, &id)) {
            return Object->set(Field->FieldID, id);
         }
         else {
            kt::Log().warning("Object \"%.*s\" could not be found.", int(name.size()), name.data());
            return ERR::Search;
         }
      }
   }

   return ERR::SetValueNotObject;
}

static ERR object_set_number(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      case LUA_TBOOLEAN:
         return Object->set(Field->FieldID, lua_toboolean(Lua, ValueIndex));

      case LUA_TNUMBER:
         return Object->set(Field->FieldID, (int64_t)lua_tointeger(Lua, ValueIndex));

      case LUA_TSTRING: // Allow internal string parsing to do its thing - important if the field is variable
         return object_set_string(Lua, Object, Field, ValueIndex);

      case LUA_TNIL: // Setting a numeric with nil does nothing.  Use zero to be explicit.
         return ERR::Okay;

      default:
         return ERR::SetValueNotNumeric;
   }
}

//********************************************************************************************************************
// Populates a struct buffer from a CSV string by walking the struct definition positionally.  The Nth CSV value is
// written to the Nth field.  Only structs composed entirely of primitive numeric fields are supported - the presence
// of a string, pointer, nested struct, array, function or object field aborts the operation.

static ERR parse_csv_struct(std::string_view String, struct_record &Def, APTR Dest)
{
   constexpr int UNSUPPORTED = FD_STRING|FD_POINTER|FD_STRUCT|FD_ARRAY|FD_FUNCTION|FD_OBJECT;

   for (auto &field : Def.Fields) {
      if (field.Type & UNSUPPORTED) return ERR::NoSupport;
   }

   for (auto &field : Def.Fields) {
      while ((not String.empty()) and (not std::isdigit((unsigned char)String.front())) and (String.front() != '-')) {
         String.remove_prefix(1);
      }
      if (String.empty()) break; // Remaining fields retain their zero-initialised value.

      std::string buffer(String);
      char *end = nullptr;
      APTR address = (int8_t *)Dest + field.Offset;
      auto type = field.Type;
      if (type & FD_DOUBLE)     ((double *)address)[0]  = strtod(buffer.c_str(), &end);
      else if (type & FD_FLOAT) ((float *)address)[0]   = strtod(buffer.c_str(), &end);
      else if (type & FD_INT64) ((int64_t *)address)[0] = strtoll(buffer.c_str(), &end, 0);
      else if (type & FD_INT)   ((int *)address)[0]      = strtol(buffer.c_str(), &end, 0);
      else if (type & FD_WORD)  ((int16_t *)address)[0] = strtol(buffer.c_str(), &end, 0);
      else if (type & FD_BYTE)  ((uint8_t *)address)[0] = strtol(buffer.c_str(), &end, 0);
      else return ERR::NoSupport;

      const auto consumed = size_t(end - buffer.c_str());
      if (not consumed) break;
      if (consumed >= String.size()) String = {};
      else String.remove_prefix(consumed);
   }

   return ERR::Okay;
}

static struct_record * lookup_struct_field_def(const Field *Field)
{
   if (not Field->Arg) return nullptr;

   auto def = glStructs.find(std::string_view((CSTRING)Field->Arg));
   if (def IS glStructs.end()) return nullptr;
   else return &def->second;
}

static ERR object_set_struct(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      case LUA_TSTRING: {
         // The user can provide a CSV list of values for the struct.  This is only valid for structs that consist of
         // primitive numeric values.  Each CSV value is mapped positionally to the struct's fields.

         auto struct_def = lookup_struct_field_def(Field);
         if (not struct_def) return ERR::SetValueNotStruct;

         std::string_view source = lua_tostring(Lua, ValueIndex);

         auto structbuf = std::make_unique<uint8_t[]>(ALIGN64(struct_def->Size));
         if (auto error = parse_csv_struct(source, *struct_def, structbuf.get()); error != ERR::Okay) return error;

         if (Field->SetValue) return Object->set(Field->FieldID, structbuf.get());
         else { // The struct is embedded, we can write to it directly because we know the struct size.
            copymem(structbuf.get(), ((int8_t *)Object) + Field->Offset, struct_def->Size);
            return ERR::Okay;
         }
      }

      case LUA_TUSERDATA:
         if (auto fs = (fstruct *)get_meta(Lua, ValueIndex, "Tiri.struct")) {
            auto struct_def = lookup_struct_field_def(Field);
            if ((not struct_def) or (fs->Def != struct_def) or (fs->StructSize < struct_def->Size)) {
               return ERR::SetValueNotStruct;
            }

            if (Field->SetValue) {
               // We only need to pass a reference to the struct as a pointer
               return Object->set(Field->FieldID, fs->Data);
            }
            else { // The struct is embedded, we can write to it directly because we know the struct size.
               copymem(fs->Data, ((int8_t *)Object) + Field->Offset, struct_def->Size);
            }
            return ERR::Okay;
         }
         else return ERR::SetValueNotStruct;

      default:
         return ERR::SetValueNotStruct;
   }
}

static ERR object_set_unit(lua_State *Lua, OBJECTPTR Object, const Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      case LUA_TNUMBER:
         return Object->set(Field->FieldID, Unit(lua_tonumber(Lua, ValueIndex)));

      case LUA_TSTRING: // Allow internal string parsing to do its thing - important if the field is variable
         return object_set_string(Lua, Object, Field, ValueIndex);

      case LUA_TNIL: // Setting a unit with nil does nothing.  Use zero to be explicit.
         return ERR::Okay;

      default:
         return ERR::SetValueNotNumeric;
   }
}

//********************************************************************************************************************
// Usage: value = obj.get("Width", [Default])
//
// The default value is optional - it is used if the get request fails.  This function never throws exceptions.

static int object_get(lua_State *Lua)
{
   kt::Log log("obj.get");

   std::string_view fieldname;
   if (luaL_checkstring(Lua, 1, fieldname)) {
      auto def = object_context(Lua);

      auto obj = access_object(def);
      if (not obj) {
         lua_pushvalue(Lua, 2); // Push the client's default value
         return 1;
      }

      OBJECTPTR target;
      if (fieldname.starts_with('$')) { // Deprecated feature
         release_object(def);
         luaL_error(Lua, "Invalid field name: %.*s", int(fieldname.size()), fieldname.data());
      }
      else if (auto field = FindField(obj, fieldhash(fieldname), &target)) {
         int result = 0;
         if (field->Flags & FD_ARRAY) {
            result = object_get_array(Lua, obj_read(0, nullptr, (APTR)field), def);
         }
         else if (field->Flags & FD_STRUCT) {
            result = object_get_struct(Lua, obj_read(0, nullptr, (APTR)field), def);
         }
         else if (field->Flags & FD_STRING) {
            result = object_get_string(Lua, obj_read(0, nullptr, (APTR)field), def);
         }
         else if (field->Flags & FD_POINTER) {
            if (field->Flags & (FD_OBJECT|FD_LOCAL)) {
               result = object_get_object(Lua, obj_read(0, nullptr, (APTR)field), def);
            }
            else result = object_get_ptr(Lua, obj_read(0, nullptr, (APTR)field), def);
         }
         else if (field->Flags & FD_DOUBLE) {
            result = object_get_double(Lua, obj_read(0, nullptr, (APTR)field), def);
         }
         else if (field->Flags & FD_INT64) {
            result = object_get_large(Lua, obj_read(0, nullptr, (APTR)field), def);
         }
         else if (field->Flags & FD_INT) {
            if (field->Flags & FD_UNSIGNED) {
               result = object_get_ulong(Lua, obj_read(0, nullptr, (APTR)field), def);
            }
            else result = object_get_long(Lua, obj_read(0, nullptr, (APTR)field), def);
         }
         else if (field->Flags & FD_UNIT) {
            result = object_get_unit(Lua, obj_read(0, nullptr, (APTR)field), def);
         }

         release_object(def);
         if (not result) lua_pushvalue(Lua, 2); // An error occurred if no result.  Push the client's default value
         return 1;
      }
      else { // Revert to getKey() if the class supports it failed
         std::string buffer;

         if ((!acGetKey(obj, fieldname, buffer)) and (not buffer.empty())) {
            lua_pushstring(Lua, buffer);
         }
         else lua_pushvalue(Lua, 2); // Push the client's default value

         release_object(def);
         return 1;
      }
   }
   else return 0;
}

//********************************************************************************************************************
// Usage: value = obj.getKey("Width", [Default])
//
// As for obj.get(), but explicitly references a custom variable name.

static int object_getkey(lua_State *Lua)
{
   std::string_view fieldname;
   if (luaL_checkstring(Lua, 1, fieldname)) {
      auto def = object_context(Lua);
      ERR error;
      if (auto obj = access_object(def)) {
         std::string buffer;
         if (!(error = acGetKey(obj, fieldname, buffer))) {
            lua_pushstring(Lua, buffer);
         }
         release_object(def);
      }
      else error = ERR::AccessObject;

      if (error != ERR::Okay) {
         if (lua_gettop(Lua) >= 2) lua_pushvalue(Lua, 2);
         else lua_pushnil(Lua);
      }

      return 1;
   }
   else return 0;
}

//********************************************************************************************************************
// Usage: error = obj.set("Width", Value)

static int object_set(lua_State *Lua)
{
   auto def = object_context(Lua);

   std::string_view fieldname;
   if (not luaL_checkstring(Lua, 1, fieldname)) return 0;

   if (auto obj = access_object(def)) {
      int type = lua_type(Lua, 2);
      auto fh = fieldhash(fieldname); // NB: Using fieldhash() because camel-case is a valid input

      ERR error;
      if (type IS LUA_TNUMBER) error = obj->set(fh, luaL_checknumber(Lua, 2));
      else {
         size_t len;
         auto str = luaL_optlstring(Lua, 2, nullptr, &len);
         std::string_view sv(str ? str : "", len);
         error = obj->set(fh, sv);
      }

      release_object(def);
      lua_pushinteger(Lua, int(error));
      report_action_error(Lua, def, "set", error);
      return 1;
   }
   else return 0;
}

//********************************************************************************************************************
// Usage: obj.setKey("Width", "Value")

static int object_setkey(lua_State *Lua)
{
   auto def = object_context(Lua);
   std::string_view fieldname;
   if (luaL_checkstring(Lua, 1, fieldname)) {
      auto value = luaL_optstring(Lua, 2, nullptr);
      if (auto obj = access_object(def)) {
         ERR error = acSetKey(obj, fieldname, value);
         release_object(def);
         lua_pushinteger(Lua, int(error));
         report_action_error(Lua, def, "setKey", error);
         return 1;
      }
   }

   return 0;
}

//********************************************************************************************************************
// Used by obj.new() exclusively.

static ERR set_object_field(lua_State *Lua, OBJECTPTR Object, uint32_t FieldHash, int ValueIndex)
{
   OBJECTPTR target;
   if (auto field = FindField(Object, FieldHash, &target)) {
      if (field->Flags & FD_ARRAY) {
         return object_set_array(Lua, target, field, ValueIndex);
      }
      else if (field->Flags & FD_FUNCTION) {
         return object_set_function(Lua, target, field, ValueIndex);
      }
      else if (field->Flags & FD_POINTER) {
         if (field->Flags & (FD_OBJECT|FD_LOCAL)) { // Writing to an integral is permitted if marked as writeable.
            return object_set_object(Lua, target, field, ValueIndex);
         }
         else return object_set_ptr(Lua, target, field, ValueIndex);
      }
      else if ((field->Flags & FD_STRING) and (field->Flags & FD_CPP)) { // std::string target
         auto type = lua_type(Lua, ValueIndex);
         if (type IS LUA_TNIL) return Object->set(field->FieldID, std::string_view{});
         else return object_set_string(Lua, target, field, ValueIndex);
      }
      else if (field->Flags & (FD_DOUBLE|FD_FLOAT)) {
         return object_set_double(Lua, target, field, ValueIndex);
      }
      else if (field->Flags & (FD_FLAGS|FD_LOOKUP)) {
         return object_set_lookup(Lua, target, field, ValueIndex);
      }
      else if (field->Flags & FD_OBJECT) { // Object ID
         return object_set_oid(Lua, target, field, ValueIndex);
      }
      else if (field->Flags & (FD_INT|FD_INT64)) {
         return object_set_number(Lua, target, field, ValueIndex);
      }
      else if (field->Flags & FD_UNIT) {
         return object_set_unit(Lua, target, field, ValueIndex);
      }
      else if (field->Flags & FD_STRUCT) {
         return object_set_struct(Lua, target, field, ValueIndex);
      }
      else return ERR::UnsupportedField;
   }
   else return ERR::UnsupportedField;
}

//********************************************************************************************************************
// Support for direct field indexing.  These functions are utilised if a field reference is easily resolved to a hash.

static int object_get_array(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      int total;
      APTR *list;
      if (field->Flags & FD_CPP) { // kt::vector<>
         if (field->Flags & FD_STRING) { // kt::vector<std::string>
            std::string *values;
            if (!(error = obj->get(field->FieldID, values, total, false))) {
               kt::vector<std::string> strings(values, values + total);
               GCarray *array = lj_array_new(Lua, total, AET::STR_CPP, (void *)&strings, ARRAY_CACHED, "");
               lj_gc_check(Lua);
               setarrayV(Lua, Lua->top++, array);
            }
         }
         else {
            // For kt::vector primitives we can just convert to a raw data array.
            APTR *values; // The type doesn't matter.
            if (!(error = obj->get(field->FieldID, values, total, false))) {
               if (total <= 0) lua_pushnil(Lua);
               else {
                  std::string_view struct_name = field->Flags & FD_STRUCT ? std::string_view((CSTRING)field->Arg) : std::string_view {};
                  make_any_array(Lua, field->Flags, struct_name, total, values);
               }
            }
         }
      }
      else if (!(error = obj->get(field->FieldID, list, total, false))) {
         if (total <= 0) lua_pushnil(Lua);
         else if (field->Flags & FD_STRING) {
            make_array(Lua, AET::CSTR, total, list);
         }
         else if (field->Flags & FD_OBJECT) {
            make_array(Lua, AET::OBJECT, total, list);
         }
         else if (field->Flags & (FD_INT|FD_INT64|FD_FLOAT|FD_DOUBLE|FD_POINTER|FD_BYTE|FD_WORD|FD_STRUCT)) {
            std::string_view struct_name = field->Flags & FD_STRUCT ? std::string_view((CSTRING)field->Arg) : std::string_view {};
            make_any_array(Lua, field->Flags, struct_name, total, list);
         }
         else {
            kt::Log(__FUNCTION__).warning("Invalid array type for '%s', flags: $%.8x", field->Name, field->Flags);
            error = ERR::FieldTypeMismatch;
         }
      }

      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_struct(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      if (field->Arg) {
         APTR result;
         if (!(error = obj->get(field->FieldID, result))) {
            if (result) { // Structs are copied into standard Lua tables.
               if (field->Flags & FD_RESOURCE) {
                   push_struct(Lua->script, result, (CSTRING)field->Arg, (field->Flags & FD_ALLOC) ? TRUE : FALSE, TRUE);
               }
               else error = named_struct_to_table(Lua, (CSTRING)field->Arg, result);
            }
            else lua_pushnil(Lua);
         }
      }
      else {
         kt::Log(__FUNCTION__).warning("No struct name reference for field %s in class %s.", field->Name, obj->Class->ClassName.c_str());
         error = ERR::Failed;
      }
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_string(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      std::string_view result;
      if (!(error = obj->get(field->FieldID, result))) {
         if (result.empty()) lua_pushnil(Lua);
         else lua_pushlstring(Lua, result.data(), result.size());
         if (field->Flags & FD_ALLOC) FreeResource(result.data());
      }
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_ptr(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      APTR result;
      if (!(error = obj->get(field->FieldID, result))) lua_pushlightuserdata(Lua, result);
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_object(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      OBJECTPTR objval;
      if (!(error = obj->get(field->FieldID, objval))) {
         if (objval) push_object(Lua, objval);
         else lua_pushnil(Lua);
      }
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_unit(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      Unit result;
      if (!(error = obj->get(field->FieldID, result))) lua_pushnumber(Lua, result.Value);
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_double(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      double result;
      if (!(error = obj->get(field->FieldID, result))) lua_pushnumber(Lua, result);
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_large(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      int64_t result;
      if (!(error = obj->get(field->FieldID, result))) lua_pushnumber(Lua, result);
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_long(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      int result;
      if (!(error = obj->get(field->FieldID, result))) {
         if (field->Flags & FD_OBJECT) push_object_id(Lua, result);
         else lua_pushinteger(Lua, result);
      }
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_ulong(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      uint32_t result;
      if (!(error = obj->get(field->FieldID, (int &)result))) {
         lua_pushnumber(Lua, result);
      }
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}
