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

static ERR object_set_string(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
{
   std::string_view value;
   if (auto error = lua_string_view(Lua, ValueIndex, value); error != ERR::Okay) return error;
   return Object->set(Field->FieldID, value);
}

//********************************************************************************************************************

static ERR set_array(lua_State *Lua, OBJECTPTR Object, Field *Field, int Values, int total)
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
      kt::vector<CSTRING> values((size_t)total);
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

         return Object->set(Field->FieldID, structbuf.get(), total);
      }
      else return ERR::SetValueNotArray;
   }
   else return ERR::SetValueNotArray;
}

//********************************************************************************************************************

static ERR object_set_array(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
{
   auto type = lua_type(Lua, ValueIndex);

   if (type IS LUA_TSTRING) { // Treat the source as a CSV field
      return object_set_string(Lua, Object, Field, ValueIndex);
   }
   else if (type IS LUA_TTABLE) {
      lua_settop(Lua, ValueIndex);
      int t = lua_gettop(Lua);
      int total = lua_objlen(Lua, t);

      if (total < 1024) {
         return set_array(Lua, Object, Field, t, total);
      }
      else return ERR::BufferOverflow;
   }
   else if (type IS LUA_TARRAY) {
      GCarray *arr = arrayV(Lua, ValueIndex);
      return Object->set(Field->FieldID, arr->arraydata(), arr->len, arr->type_flags());
   }
   else return ERR::SetValueNotArray;
}

static ERR object_set_function(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
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

static ERR object_set_object(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
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

static ERR object_set_ptr(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
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

static ERR object_set_cppstring(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
{
   auto type = lua_type(Lua, ValueIndex);

   if (type IS LUA_TSTRING) {
      return object_set_string(Lua, Object, Field, ValueIndex);
   }
   else if (type IS LUA_TNUMBER) {
      return Object->set(Field->FieldID, lua_tonumber(Lua, ValueIndex));
   }
   else if (type IS LUA_TNIL) {
      return Object->set(Field->FieldID, std::string_view());
   }
   else return ERR::SetValueNotString;
}

static ERR object_set_double(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      case LUA_TNUMBER:
         return Object->set(Field->FieldID, lua_tonumber(Lua, ValueIndex));

      case LUA_TSTRING: // Allow internal string parsing to do its thing - important if the field is variable
         return object_set_string(Lua, Object, Field, ValueIndex);

      case LUA_TNIL: // Setting a numeric with nil does nothing.  Use zero to be explicit.
         return ERR::Okay;

      default:
         return ERR::SetValueNotNumeric;
   }
}

static ERR object_set_lookup(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      case LUA_TNUMBER: return Object->set(Field->FieldID, (int)lua_tointeger(Lua, ValueIndex));
      case LUA_TSTRING: return object_set_string(Lua, Object, Field, ValueIndex);
      default: return ERR::SetValueNotLookup;
   }
}

static ERR object_set_oid(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
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
         if (FindObject(name, CLASSID::NIL, &id) IS ERR::Okay) {
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

static ERR object_set_number(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
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
      if (fieldname.starts_with('$')) { // Get field as a string, good for CSV arrays, flags and lookups
         fieldname.remove_prefix(1);
         std::string buffer;
         if (obj->get(fieldhash(fieldname), buffer) IS ERR::Okay) lua_pushlstring(Lua, buffer.c_str(), buffer.size());
         else lua_pushvalue(Lua, 2); // Push the client's default value
         release_object(def);
         return 1;
      }
      else if (auto field = FindField(obj, fieldhash(fieldname), &target)) {
         int result = 0;
         if (field->Flags & FD_ARRAY) {
            result = object_get_array(Lua, obj_read(0, nullptr, field), def);
         }
         else if (field->Flags & FD_STRUCT) {
            result = object_get_struct(Lua, obj_read(0, nullptr, field), def);
         }
         else if (field->Flags & FD_STRING) {
            result = object_get_string(Lua, obj_read(0, nullptr, field), def);
         }
         else if (field->Flags & FD_POINTER) {
            if (field->Flags & (FD_OBJECT|FD_LOCAL)) {
               result = object_get_object(Lua, obj_read(0, nullptr, field), def);
            }
            else result = object_get_ptr(Lua, obj_read(0, nullptr, field), def);
         }
         else if (field->Flags & FD_DOUBLE) {
            result = object_get_double(Lua, obj_read(0, nullptr, field), def);
         }
         else if (field->Flags & FD_INT64) {
            result = object_get_large(Lua, obj_read(0, nullptr, field), def);
         }
         else if (field->Flags & FD_INT) {
            if (field->Flags & FD_UNSIGNED) {
               result = object_get_ulong(Lua, obj_read(0, nullptr, field), def);
            }
            else result = object_get_long(Lua, obj_read(0, nullptr, field), def);
         }

         release_object(def);
         if (not result) lua_pushvalue(Lua, 2); // An error occurred if no result.  Push the client's default value
         return 1;
      }
      else { // Revert to getKey() if the class supports it failed
         std::string buffer;

         if ((acGetKey(obj, fieldname, buffer) IS ERR::Okay) and (not buffer.empty())) {
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
         if ((error = acGetKey(obj, fieldname, buffer)) IS ERR::Okay) {
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
         return object_set_cppstring(Lua, target, field, ValueIndex);
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
      if (field->Flags & FD_CPP) {
         if (field->Flags & FD_STRING) {
            kt::vector<std::string> *values; // std::string doesn't work like standard primitives - at least not in MSVC - so it gets a special handler.
            if ((error = obj->get(field->FieldID, values, total, false)) IS ERR::Okay) {
               if (total <= 0) lua_pushnil(Lua);
               else make_array(Lua, AET::STR_CPP, total, values);
            }
         }
         else {
            // For kt::vector primitives we can just convert to a raw data array.
            kt::vector<APTR> *values; // The type doesn't matter.
            if ((error = obj->get(field->FieldID, values, total, false)) IS ERR::Okay) {
               if (total <= 0) lua_pushnil(Lua);
               else {
                  std::string_view struct_name = field->Flags & FD_STRUCT ? std::string_view((CSTRING)field->Arg) : std::string_view {};
                  make_any_array(Lua, field->Flags, struct_name, total, values->data());
               }
            }
         }
      }
      else if ((error = obj->get(field->FieldID, list, total, false)) IS ERR::Okay) {
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
         if ((error = obj->get(field->FieldID, result)) IS ERR::Okay) {
            if (result) { // Structs are copied into standard Lua tables.
               if (field->Flags & FD_RESOURCE) {
                   push_struct(Lua->script, result, (CSTRING)field->Arg, (field->Flags & FD_ALLOC) ? TRUE : FALSE, TRUE);
               }
               else named_struct_to_table(Lua, (CSTRING)field->Arg, result);
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
      if ((error = obj->get(field->FieldID, result)) IS ERR::Okay) {
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
      if ((error = obj->get(field->FieldID, result)) IS ERR::Okay) lua_pushlightuserdata(Lua, result);
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
      if ((error = obj->get(field->FieldID, objval)) IS ERR::Okay) {
         if (objval) push_object(Lua, objval);
         else lua_pushnil(Lua);
      }
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
      if ((error = obj->get(field->FieldID, result)) IS ERR::Okay) lua_pushnumber(Lua, result);
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
      if ((error = obj->get(field->FieldID, result)) IS ERR::Okay) lua_pushnumber(Lua, result);
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
      if ((error = obj->get(field->FieldID, result)) IS ERR::Okay) {
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
      if ((error = obj->get(field->FieldID, (int &)result)) IS ERR::Okay) {
         lua_pushnumber(Lua, result);
      }
      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}
