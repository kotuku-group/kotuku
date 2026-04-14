// Refer: lib_object.cpp

//********************************************************************************************************************

static ERR set_array(lua_State *Lua, OBJECTPTR Object, Field *Field, int Values, int total)
{
   if (Field->Flags & FD_INT) {
      pf::vector<int> values((size_t)total);
      for (lua_pushnil(Lua); lua_next(Lua, Values); lua_pop(Lua, 1)) {
         int index = lua_tointeger(Lua, -2) - 1;
         if ((index >= 0) and (index < total)) {
            values[index] = lua_tointeger(Lua, -1);
         }
      }
      return Object->set(Field->FieldID, values);
   }
   else if (Field->Flags & FD_STRING) {
      pf::vector<CSTRING> values((size_t)total);
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
      return Object->set(Field->FieldID, lua_tostring(Lua, ValueIndex));
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
      return Object->set(Field->FieldID, lua_tostring(Lua, ValueIndex));
   }
   else if (type IS LUA_TNUMBER) {
      if (Field->Flags & FD_STRING) {
         return Object->set(Field->FieldID, lua_tostring(Lua, ValueIndex));
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

static ERR object_set_double(lua_State *Lua, OBJECTPTR Object, Field *Field, int ValueIndex)
{
   switch(lua_type(Lua, ValueIndex)) {
      case LUA_TNUMBER:
         return Object->set(Field->FieldID, lua_tonumber(Lua, ValueIndex));

      case LUA_TSTRING: // Allow internal string parsing to do its thing - important if the field is variable
         return Object->set(Field->FieldID, lua_tostring(Lua, ValueIndex));

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
      case LUA_TSTRING: return Object->set(Field->FieldID, lua_tostring(Lua, ValueIndex));
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
         if (FindObject(lua_tostring(Lua, ValueIndex), CLASSID::NIL, FOF::NIL, &id) IS ERR::Okay) {
            Object->set(Field->FieldID, id);
         }
         else {
            pf::Log log;
            log.warning("Object \"%s\" could not be found.", lua_tostring(Lua, ValueIndex));
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
         return Object->set(Field->FieldID, lua_tostring(Lua, ValueIndex));

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
   pf::Log log("obj.get");

   if (auto fieldname = luaL_checkstring(Lua, 1)) {
      auto def = object_context(Lua);

      auto obj = access_object(def);
      if (not obj) {
         lua_pushvalue(Lua, 2); // Push the client's default value
         return 1;
      }

      OBJECTPTR target;
      if (fieldname[0] IS '$') { // Get field as a string, good for CSV arrays, flags and lookups
         std::string buffer;
         if (obj->get(fieldhash(fieldname+1), buffer) IS ERR::Okay) lua_pushlstring(Lua, buffer.c_str(), buffer.size());
         else lua_pushvalue(Lua, 2); // Push the client's default value
         release_object(def);
         return 1;
      }
      else if (auto field = FindField(obj, fieldhash(fieldname), &target)) {
         int result = 0;
         if (field->Flags & FD_ARRAY) {
            if (field->Flags & FD_RGB) result = object_get_rgb(Lua, obj_read(0, nullptr, field), def);
            else result = object_get_array(Lua, obj_read(0, nullptr, field), def);
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
         if (!result) lua_pushvalue(Lua, 2); // An error occurred if no result.  Push the client's default value
         return 1;
      }
      else { // Revert to getKey() if the class supports it failed
         char buffer[8192];

         if ((acGetKey(obj, fieldname, buffer, sizeof(buffer)) IS ERR::Okay) and (buffer[0])) {
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
   if (auto fieldname = luaL_checkstring(Lua, 1)) {
      auto def = object_context(Lua);
      ERR error;
      if (auto obj = access_object(def)) {
         char buffer[8192];
         if ((error = acGetKey(obj, fieldname, buffer, sizeof(buffer))) IS ERR::Okay) {
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

   CSTRING fieldname;
   if (!(fieldname = luaL_checkstring(Lua, 1))) return 0;

   if (auto obj = access_object(def)) {
      int type = lua_type(Lua, 2);
      auto fh = fieldhash(fieldname); // NB: Using fieldhash() because camel-case is a valid input

      ERR error;
      if (type IS LUA_TNUMBER) error = obj->set(fh, luaL_checknumber(Lua, 2));
      else error = obj->set(fh, luaL_optstring(Lua, 2, nullptr));

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
   if (auto fieldname = luaL_checkstring(Lua, 1)) {
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

static ERR set_object_field(lua_State *Lua, OBJECTPTR obj, uint32_t FieldHash, int ValueIndex)
{
   pf::Log log("obj.setfield");

   int type = lua_type(Lua, ValueIndex);

   OBJECTPTR target;
   if (auto field = FindField(obj, FieldHash, &target)) {
      if (field->Flags & FD_ARRAY) {
         if (type IS LUA_TSTRING) { // Treat the source as a CSV field
            return target->set(field->FieldID, lua_tostring(Lua, ValueIndex));
         }
         else if (type IS LUA_TTABLE) {
            lua_settop(Lua, ValueIndex);
            int t = lua_gettop(Lua);
            int total = lua_objlen(Lua, t);

            if (total < 1024) return set_array(Lua, target, field, t, total);
            else return ERR::BufferOverflow;
         }
         else if (type IS LUA_TARRAY) {
            GCarray *arr = arrayV(Lua, ValueIndex);
            return target->set(field->FieldID, arr->arraydata(), arr->len, arr->type_flags());
         }
         else return ERR::SetValueNotArray;
      }
      else if (field->Flags & FD_FUNCTION) {
         if (type IS LUA_TSTRING) {
            lua_getglobal(Lua, lua_tostring(Lua, ValueIndex));
            auto func = FUNCTION(Lua->script, luaL_ref(Lua, LUA_REGISTRYINDEX));
            return target->set(field->FieldID, &func);
         }
         else if (type IS LUA_TFUNCTION) {
            lua_pushvalue(Lua, ValueIndex);
            auto func = FUNCTION(Lua->script, luaL_ref(Lua, LUA_REGISTRYINDEX));
            return target->set(field->FieldID, &func);
         }
         else return ERR::SetValueNotFunction;
      }
      else if (field->Flags & FD_POINTER) {
         if (field->Flags & (FD_OBJECT|FD_LOCAL)) { // Writing to an integral is permitted if marked as writeable.
            if (auto obj_ref = lua_toobject(Lua, ValueIndex)) {
               OBJECTPTR ptr_obj;
               if (obj_ref->ptr) return target->set(field->FieldID, obj_ref->ptr);
               else if ((ptr_obj = (OBJECTPTR)access_object(obj_ref))) {
                  ERR error = target->set(field->FieldID, obj_ref->ptr);
                  release_object(obj_ref);
                  return error;
               }
               else return ERR::Failed;
            }
            else return target->set(field->FieldID, (APTR)nullptr);
         }
         else if (type IS LUA_TSTRING) {
            return target->set(field->FieldID, lua_tostring(Lua, ValueIndex));
         }
         else if (type IS LUA_TNUMBER) {
            if (field->Flags & FD_STRING) return obj->set(field->FieldID, lua_tostring(Lua, ValueIndex));
            else if (lua_tointeger(Lua, ValueIndex) IS 0) {
               // Setting pointer fields with numbers is only allowed if that number evaluates to zero (NULL)
               return obj->set(field->FieldID, (APTR)nullptr);
            }
            else return ERR::SetValueNotPointer;
         }
         else if (type IS LUA_TARRAY) {
            GCarray *arr = arrayV(Lua, ValueIndex);
            return obj->set(field->FieldID, arr->arraydata());
         }
         else if (auto fs = (fstruct *)get_meta(Lua, ValueIndex, "Tiri.struct")) {
            return obj->set(field->FieldID, fs->Data);
         }
         else if (type IS LUA_TNIL) return obj->set(field->FieldID, (APTR)nullptr);
         else return ERR::SetValueNotPointer;
      }
      else if (field->Flags & (FD_DOUBLE|FD_FLOAT)) {
         switch(type) {
            case LUA_TNUMBER:
               return target->set(field->FieldID, lua_tonumber(Lua, ValueIndex));

            case LUA_TSTRING: // Allow internal string parsing to do its thing - important if the field is variable
               return target->set(field->FieldID, lua_tostring(Lua, ValueIndex));

            case LUA_TNIL: // Setting a numeric with nil does nothing.  Use zero to be explicit.
               return ERR::Okay;

            default:
               return ERR::SetValueNotNumeric;
         }
      }
      else if (field->Flags & (FD_FLAGS|FD_LOOKUP)) {
         switch(type) {
            case LUA_TNUMBER:
               return target->set(field->FieldID, (int)lua_tointeger(Lua, ValueIndex));

            case LUA_TSTRING:
               return target->set(field->FieldID, lua_tostring(Lua, ValueIndex));

            default:
               return ERR::SetValueNotLookup;
         }
      }
      else if (field->Flags & FD_OBJECT) { // Object ID
         switch(type) {
            case LUA_TNUMBER:
               return target->set(field->FieldID, (OBJECTID)lua_tointeger(Lua, ValueIndex));

            case LUA_TOBJECT: {
               auto obj_ref = lua_toobject(Lua, ValueIndex);
               return target->set(field->FieldID, obj_ref->uid);
            }

            case LUA_TSTRING: {
               OBJECTID id;
               if (FindObject(lua_tostring(Lua, ValueIndex), CLASSID::NIL, FOF::NIL, &id) IS ERR::Okay) {
                  target->set(field->FieldID, id);
               }
               else {
                  log.warning("Object \"%s\" could not be found.", lua_tostring(Lua, ValueIndex));
                  return ERR::Search;
               }
            }

            case LUA_TNIL:
               return obj->set(field->FieldID, 0);

            default:
               return ERR::SetValueNotObject;
         }
      }
      else if (field->Flags & (FD_INT|FD_INT64)) {
         switch(type) {
            case LUA_TBOOLEAN:
               return target->set(field->FieldID, lua_toboolean(Lua, ValueIndex));

            case LUA_TNUMBER:
               return target->set(field->FieldID, (int64_t)lua_tointeger(Lua, ValueIndex));

            case LUA_TSTRING: // Allow internal string parsing to do its thing - important if the field is variable
               return target->set(field->FieldID, lua_tostring(Lua, ValueIndex));

            case LUA_TNIL: // Setting a numeric with nil does nothing.  Use zero to be explicit.
               return ERR::Okay;

            default:
               return ERR::SetValueNotNumeric;
         }
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
            pf::vector<std::string> *values; // std::string doesn't work like standard primitives - at least not in MSVC - so it gets a special handler.
            if ((error = obj->get(field->FieldID, values, total, false)) IS ERR::Okay) {
               if (total <= 0) lua_pushnil(Lua);
               else make_array(Lua, AET::STR_CPP, total, values);
            }
         }
         else {
            // For pf::vector primitives we can just convert to a raw data array.
            pf::vector<APTR> *values; // The type doesn't matter.
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
         else if (field->Flags & (FD_INT|FD_INT64|FD_FLOAT|FD_DOUBLE|FD_POINTER|FD_BYTE|FD_WORD|FD_STRUCT)) {
            std::string_view struct_name = field->Flags & FD_STRUCT ? std::string_view((CSTRING)field->Arg) : std::string_view {};
            make_any_array(Lua, field->Flags, struct_name, total, list);
         }
         else {
            pf::Log(__FUNCTION__).warning("Invalid array type for '%s', flags: $%.8x", field->Name, field->Flags);
            error = ERR::FieldTypeMismatch;
         }
      }

      release_object(Def);
   }
   else error = ERR::AccessObject;

   Lua->CaughtError = error;
   return error != ERR::Okay ? 0 : 1;
}

static int object_get_rgb(lua_State *Lua, const obj_read &Handle, GCobject *Def)
{
   ERR error;
   if (auto obj = access_object(Def)) {
      auto field = (Field *)(Handle.Data);
      CSTRING rgb;
      if (((error = obj->get(field->FieldID, rgb)) IS ERR::Okay) and (rgb)) lua_pushstring(Lua, rgb);
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
         pf::Log(__FUNCTION__).warning("No struct name reference for field %s in class %s.", field->Name, obj->Class->ClassName);
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
      CSTRING result;
      if ((error = obj->get(field->FieldID, result)) IS ERR::Okay) {
         lua_pushstring(Lua, result);
         if (field->Flags & FD_ALLOC) FreeResource(result);
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
