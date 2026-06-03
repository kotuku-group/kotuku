// Refer: lib_object.cpp

//********************************************************************************************************************
// Lua C closure executed via calls to obj.acName()

static int object_action_call_args(lua_State *Lua)
{
   auto obj_ref = object_context(Lua);
   AC action_id = AC(lua_tointeger(Lua, lua_upvalueindex(2)));
   bool release = false;

   auto argbuffer = std::make_unique<int8_t[]>(glActions[int(action_id)].Size+8); // +8 for overflow protection in build_args()
   ERR error = build_args(Lua, glActions[int(action_id)].Name, glActions[int(action_id)].Args, glActions[int(action_id)].Size, argbuffer.get(), nullptr);
   if (error != ERR::Okay) {
      luaL_error(Lua, ERR::Args, "Argument build failure for %s.", glActions[int(action_id)].Name);
      return 0;
   }

   int results = 1;
   if (obj_ref->ptr) error = Action(action_id, obj_ref->ptr, argbuffer.get());
   else if (auto obj = access_object(obj_ref)) {
      error = Action(action_id, obj, argbuffer.get());
      release = true;
   }
   else error = ERR::AccessObject;

   // NB: Even if an error is returned, always get the results (any results parameters are nullified prior to
   // function entry and the action can return results legitimately even if an error code is returned - e.g.
   // quite common when returning ERR::Terminate).

   lua_pushinteger(Lua, int(error));
   results += get_results(Lua, glActions[int(action_id)].Args, argbuffer.get());

   if (release) release_object(obj_ref);
   report_action_error(Lua, obj_ref, glActions[int(action_id)].Name, error);
   return results;
}

// This variant is for actions that take no parameters.

static int object_action_call(lua_State *Lua)
{
   auto def = object_context(Lua);
   AC action_id = AC(lua_tointeger(Lua, lua_upvalueindex(2)));
   ERR error;
   bool release = false;

   if (def->ptr) error = Action(action_id, def->ptr, nullptr);
   else if (auto obj = access_object(def)) {
      error = Action(action_id, obj, nullptr);
      release = true;
   }
   else error = ERR::AccessObject;

   lua_pushinteger(Lua, int(error));

   if (release) release_object(def);
   report_action_error(Lua, def, glActions[int(action_id)].Name, error);
   return 1;
}

//********************************************************************************************************************
// Lua C closure executed via calls to obj.mtName()

static int object_method_call_args(lua_State *Lua)
{
   auto def = object_context(Lua);
   auto method = (MethodEntry *)lua_touserdata(Lua, lua_upvalueindex(2));

   auto argbuffer = std::make_unique<int8_t[]>(method->Size+8); // +8 for overflow protection in build_args()
   int result_count;
   ERR error = build_args(Lua, method->Name, method->Args, method->Size, argbuffer.get(), &result_count);
   if (error != ERR::Okay) luaL_error(Lua, ERR::Args, "Argument build failure for method %s.", method->Name);

   int results = 1;
   bool release = false;

   if (def->ptr) error = Action(method->MethodID, def->ptr, argbuffer.get());
   else if (auto obj = access_object(def)) {
      error = Action(method->MethodID, obj, argbuffer.get());
      release = true;
   }
   else error = ERR::AccessObject;

   lua_pushinteger(Lua, int(error));

   results += get_results(Lua, method->Args, (const int8_t *)argbuffer.get());

   if (release) release_object(def);
   report_action_error(Lua, def, method->Name, error);
   return results;
}

// This variant is for methods that take no parameters.

static int object_method_call(lua_State *Lua)
{
   auto def = object_context(Lua);
   ERR error;
   bool release = false;
   auto method = (MethodEntry *)lua_touserdata(Lua, lua_upvalueindex(2));

   if (def->ptr) error = Action(method->MethodID, def->ptr, nullptr);
   else if (auto obj = access_object(def)) {
      error = Action(method->MethodID, obj, nullptr);
      release = true;
   }
   else error = ERR::AccessObject;

   lua_pushinteger(Lua, int(error));

   if (release) release_object(def);
   report_action_error(Lua, def, method->Name, error);
   return 1;
}

//********************************************************************************************************************
// Build an argument buffer for actions and methods.  This follows the FD parsing logic of module_call() for the most
// part, as that is the base-line for argument parsing.  However, some differences apply in part due to the fact that
// action parameters are stored in structs.
//
// NOTE: FD_RESULT types are treated as real result values that are returned after the ERR code.  They don't need to
// be provided up-front by the client.  FD_MUTABLE types are mutable buffers that must be provided by the client and
// are manipulated in-place by the action.

static bool check_mutable_string_arg(lua_State *Lua, int Arg, GCstr *String)
{
   if (not lj_str_ismutable(String)) {
      luaL_argerror(Lua, Arg, "Mutable buffer required.");
      return false;
   }
   return true;
}

static bool check_buffer_size_arg(lua_State *Lua, int Arg, int64_t Size, size_t Capacity)
{
   if ((Size < 0) or (uint64_t(Size) > uint64_t(Capacity))) {
      luaL_argerror(Lua, Arg, "Buffer size exceeds supplied buffer capacity.");
      return false;
   }
   return true;
}

ERR build_args(lua_State *Lua, CSTRING Name, const FunctionField *args, int ArgsSize, int8_t *argbuffer, int *ResultCount)
{
   kt::Log log(__FUNCTION__);

   int top = lua_gettop(Lua);

   log.traceBranch("%d, %p, Top: %d", ArgsSize, argbuffer, top);

   clearmem(argbuffer, ArgsSize);

   int i, n;
   int resultcount = 0;
   int j = 0;
   size_t buffer_capacity = 0;
   bool buffer_capacity_known = false;
   for (i=0,n=1; (args[i].Name) and (j < ArgsSize) and (top > 0); i++,n++,top--) {
      int type = lua_type(Lua, n);

      if (not (args[i].Type & FD_BUFSIZE)) buffer_capacity_known = false;

      if (args[i].Type & FD_RESULT) resultcount++;

      //log.trace("Processing arg %s, type $%.8x", args[i].Name, args[i].Type);

      if ((args[i].Type & FD_BUFFER) or (args[i+1].Type & FD_BUFSIZE)) {
         j = ALIGN64(j);
         if (type IS LUA_TARRAY) {
            //log.trace("Arg: %s, Value: Buffer (Source is Memory)", args[i].Name);

            auto array = lua_toarray(Lua, n);
            ((APTR *)(argbuffer + j))[0] = array->arraydata();
            j += sizeof(APTR);

            if (args[i+1].Type & FD_BUFSIZE) {
               // Buffer size is optional (can be nil), so set the buffer size parameter by default.  The user can override it if
               // more arguments are specified in the function call.

               size_t memsize = array->len * array->elemsize;
               buffer_capacity = memsize;
               buffer_capacity_known = true;
               if (args[i+1].Type & FD_INT)  ((int *)(argbuffer + j))[0] = int(memsize);
               else if (args[i+1].Type & FD_INT64) ((int64_t *)(argbuffer + j))[0] = int64_t(memsize);
            }
         }
         else if (auto fstruct = (struct fstruct *)get_meta(Lua, n, "Tiri.struct")) {
            //log.trace("Arg: %s, Value: Buffer (Source is a struct)", args[i].Name);

            ((APTR *)(argbuffer + j))[0] = fstruct->Data;
            j += sizeof(APTR);

            if (args[i+1].Type & FD_BUFSIZE) {
               // Buffer size is optional (can be nil), so set the buffer size parameter by default.
               // The user can override it if more arguments are specified in the function call.

               buffer_capacity = fstruct->AlignedSize;
               buffer_capacity_known = true;
               if (args[i+1].Type & FD_INT) ((int *)(argbuffer + j))[0] = fstruct->AlignedSize;
               else if (args[i+1].Type & FD_INT64) ((int64_t *)(argbuffer + j))[0] = fstruct->AlignedSize;
            }
            n--; // Adjustment required due to successful get_meta()
         }
         else if (type IS LUA_TSTRING) {
            //log.trace("Arg: %s, Value: Buffer (Source is String)", args[i].Name);
            auto string = strV(Lua->base + n - 1);
            if ((args[i].Type & FD_MUTABLE) and (not check_mutable_string_arg(Lua, n, string))) return ERR::WrongType;
            size_t len = string->len;
            CSTRING str = (args[i].Type & FD_MUTABLE) ? strdatawr(string) : strdata(string);
            ((CSTRING *)(argbuffer + j))[0] = str;
            j += sizeof(APTR);

            if (args[i+1].Type & FD_BUFSIZE) {
               buffer_capacity = len;
               buffer_capacity_known = true;
               if (args[i+1].Type & FD_INT) ((int *)(argbuffer + j))[0] = len;
               else if (args[i+1].Type & FD_INT64) ((int64_t *)(argbuffer + j))[0] = len;
            }
         }
         else if (type IS LUA_TNUMBER) luaL_argerror(Lua, n, "Cannot use a number as a buffer pointer.");
         else {
            //log.trace("Arg: %s, Value: Buffer", args[i].Name);
            ((APTR *)(argbuffer + j))[0] = lua_touserdata(Lua, n);
            j += sizeof(APTR);
         }
      }
      else if (args[i].Type & FD_STR) {
         j = ALIGN64(j);
         if (args[i].Type & FD_MUTABLE) {
            if (type != LUA_TSTRING) luaL_argerror(Lua, n, "Mutable buffer required.");
            auto string = strV(Lua->base + n - 1);
            if (not check_mutable_string_arg(Lua, n, string)) return ERR::WrongType;
            ((CSTRING *)(argbuffer + j))[0] = strdatawr(string);
            j += sizeof(CSTRING);
         }
         else if ((type IS LUA_TSTRING) or (type IS LUA_TNUMBER)) {
            if (args[i].Type & FD_CPP) {
               ((std::string_view *)(argbuffer + j))[0] = lua_tostringview(Lua, n);
               j += sizeof(std::string_view);
            }
            else {
               ((CSTRING *)(argbuffer + j))[0] = lua_tostring(Lua, n);
               j += sizeof(CSTRING);
            }
         }
         else if (type <= 0) {
            if (args[i].Type & FD_CPP) {
               ((std::string_view *)(argbuffer + j))[0] = std::string_view{};
               j += sizeof(std::string_view);
            }
            else {
               ((CSTRING *)(argbuffer + j))[0] = nullptr;
               j += sizeof(CSTRING);
            }
         }
         else luaL_error(Lua, ERR::InvalidType, "Arg #%d (%s) of %s() requires a string, got %s '%s'.", i, args[i].Name, Name, lua_typename(Lua, type), lua_tostring(Lua, n));

         //log.trace("Arg: %s, Value: %s", args[i].Name, ((STRING *)(argbuffer + j))[0]);

      }
      else if (args[i].Type & FD_PTR) {
         j = ALIGN64(j);
         if (args[i].Type & FD_OBJECT) {
            if (auto obj_ref = lj_lib_optobject(Lua, n)) {
               OBJECTPTR ptr_obj;
               if (obj_ref->ptr) {
                  ((OBJECTPTR *)(argbuffer + j))[0] = obj_ref->ptr;
               }
               else if ((ptr_obj = access_object(obj_ref))) {
                  ((OBJECTPTR *)(argbuffer + j))[0] = ptr_obj;
                  release_object(obj_ref);
               }
               else {
                  log.warning("Unable to resolve object pointer for #%d.", obj_ref->uid);
                  ((OBJECTPTR *)(argbuffer + j))[0] = nullptr;
               }
            }
            else ((OBJECTPTR *)(argbuffer + j))[0] = nullptr;
         }
         else if (args[i].Type & FD_FUNCTION) {
            if ((type IS LUA_TSTRING) or (type IS LUA_TFUNCTION)) {
               FUNCTION *func;

               if (AllocMemory(sizeof(FUNCTION), MEM::DATA, &func) IS ERR::Okay) {
                  if (type IS LUA_TSTRING) {
                     lua_getglobal(Lua, lua_tostring(Lua, n));
                     *func = FUNCTION(Lua->script, luaL_ref(Lua, LUA_REGISTRYINDEX));
                  }
                  else {
                     lua_pushvalue(Lua, n);
                     *func = FUNCTION(Lua->script, luaL_ref(Lua, LUA_REGISTRYINDEX));
                  }

                  ((FUNCTION **)(argbuffer + j))[0] = func;

                  // The FUNCTION structure is freed when processing results
               }
               else luaL_error(Lua, ERR::AllocMemory);
            }
            else luaL_error(Lua, ERR::InvalidType, "Arg #%d (%s) of %s() requires a string or function, got %s '%s'.", i, args[i].Name, Name, lua_typename(Lua, type), lua_tostring(Lua, n));
         }
         else if (type IS LUA_TSTRING) {
            //log.trace("Arg: %s, Value: Pointer (Source is String)", args[i].Name);
            auto string = strV(Lua->base + n - 1);
            if ((args[i].Type & FD_MUTABLE) and (not check_mutable_string_arg(Lua, n, string))) return ERR::WrongType;
            ((CSTRING *)(argbuffer + j))[0] = (args[i].Type & FD_MUTABLE) ? strdatawr(string) : strdata(string);
         }
         else if (type IS LUA_TNUMBER) {
            luaL_argerror(Lua, n, "Unable to convert number to a pointer.");
            return ERR::WrongType;
         }
         else if (type IS LUA_TARRAY) {
            auto array = arrayV(Lua, n);
            ((APTR *)(argbuffer + j))[0] = array->arraydata();
         }
         else {
            //log.trace("Arg: %s, Value: Pointer, SrcType: %s", args[i].Name, lua_typename(Lua, type));

            if (auto fstruct = (struct fstruct *)get_meta(Lua, n, "Tiri.struct")) {
               ((APTR *)(argbuffer + j))[0] = fstruct->Data;
               //n--; // Adjustment required due to successful get_meta()
            }
            else ((APTR *)(argbuffer + j))[0] = lua_touserdata(Lua, n);
         }

         j += sizeof(APTR);
      }
      else if (args[i].Type & FD_INT) {
         if ((type IS LUA_TUSERDATA) or (type IS LUA_TLIGHTUSERDATA)) {
            if (auto obj = lj_lib_checkobject(Lua, n)) {
               ((int *)(argbuffer + j))[0] = obj->uid;
            }
            else luaL_argerror(Lua, n, "Unable to convert usertype to an integer.");
         }
         else if (type IS LUA_TBOOLEAN) {
            auto value = lua_toboolean(Lua, n);
            if ((args[i].Type & FD_BUFSIZE) and buffer_capacity_known) {
               if (not check_buffer_size_arg(Lua, n, value, buffer_capacity)) return ERR::WrongType;
               buffer_capacity_known = false;
            }
            ((int *)(argbuffer + j))[0] = value;
         }
         else if (type != LUA_TNIL) {
            auto value = lua_tointeger(Lua, n);
            if ((args[i].Type & FD_BUFSIZE) and buffer_capacity_known) {
               if (not check_buffer_size_arg(Lua, n, value, buffer_capacity)) return ERR::WrongType;
               buffer_capacity_known = false;
            }
            ((int *)(argbuffer + j))[0] = value;
         }
         else if (args[i].Type & FD_BUFSIZE) {
            buffer_capacity_known = false; // Do not alter as the FD_BUFFER support would have managed it
         }
         else ((int *)(argbuffer + j))[0] = 0;
         //log.trace("Arg: %s, Value: %d / $%.8x", args[i].Name, ((int *)(argbuffer + j))[0], ((int *)(argbuffer + j))[0]);
         j += sizeof(int);
      }
      else if (args[i].Type & FD_DOUBLE) {
         j = ALIGN64(j);
         ((double *)(argbuffer + j))[0] = lua_tonumber(Lua, n);
         //log.trace("Arg: %s, Value: %.2f", args[i].Name, ((double *)(argbuffer + j))[0]);
         j += sizeof(double);
      }
      else if (args[i].Type & FD_INT64) {
         j = ALIGN64(j);
         if ((args[i].Type & FD_BUFSIZE) and (type IS LUA_TNIL)) {
            buffer_capacity_known = false;
         }
         else {
            auto value = lua_tointeger(Lua, n);
            if ((args[i].Type & FD_BUFSIZE) and buffer_capacity_known) {
               if (not check_buffer_size_arg(Lua, n, value, buffer_capacity)) return ERR::WrongType;
               buffer_capacity_known = false;
            }
            ((int64_t *)(argbuffer + j))[0] = value;
         }
         //log.trace("Arg: %s, Value: %" PF64, args[i].Name, ((LARGE *)(argbuffer + j))[0]);
         j += sizeof(int64_t);
      }
      else {
         log.warning("Unsupported arg %s, flags $%.8x, aborting now.", args[i].Name, args[i].Type);
         return ERR::WrongType;
      }
   }

   // Finish counting the number of result types registered in the argument list

   for (; (args[i].Name); i++) {
      if (args[i].Type & FD_RESULT) resultcount++;
   }

   log.trace("Processed %d args (%d bytes), detected %d result parameters.", i, j, resultcount);
   if (ResultCount) *ResultCount = resultcount;
   return ERR::Okay;
}

// Note: Please refer to process_results() in tiri_module.c for the 'official' take on result handling.

static int get_results(lua_State *Lua, const FunctionField *args, const int8_t *ArgBuf)
{
   kt::Log log(__FUNCTION__);
   int i;

   RMSG("get_results(%p)", ArgBuf);

   int total = 0;
   int of = 0;
   for (i=0; args[i].Name; i++) {
      const int type = args[i].Type;
      if (type & FD_ARRAY) { // Pointer to an array.
         if (sizeof(APTR) IS 8) of = ALIGN64(of);
         if (type & FD_RESULT) {
            int total_elements = -1;  // If -1, make_any_array() assumes the array is null terminated.
            if (args[i+1].Type & FD_ARRAYSIZE) {
               CPTR size_var = ArgBuf + of + sizeof(APTR);
               if (args[i+1].Type & FD_INT) total_elements = ((int *)size_var)[0];
               else if (args[i+1].Type & FD_INT64) total_elements = ((int64_t *)size_var)[0];
               else log.warning("Invalid parameter definition for '%s' of $%.8x", args[i+1].Name, args[i+1].Type);
            }

            CPTR values = ((APTR *)(ArgBuf + of))[0];
            if (values) {
               make_any_array(Lua, type, args[i].Name, total_elements, values);
               if (type & FD_ALLOC) FreeResource(values);
            }
            else lua_pushnil(Lua);
            total++;
         }
         of += sizeof(APTR);
      }
      else if (type & FD_STR) {
         if (sizeof(APTR) IS 8) of = ALIGN64(of);
         if (type & FD_RESULT) {
            CPTR val = ArgBuf + of;
            RMSG("Result-Arg: %s, Value: %.20s (String)", args[i].Name, ((STRING *)val)[0]);
            lua_pushstring(Lua, ((STRING *)val)[0]);
            if (type & FD_ALLOC) {
               APTR ptr = ((STRING *)val)[0];
               if (ptr) FreeResource(ptr);
            }
            total++;
         }
         of += sizeof(STRING);
      }
      else if (type & FD_STRUCT) { // Pointer to a struct
         if (sizeof(APTR) IS 8) of = ALIGN64(of);
         if (type & FD_RESULT) {
            APTR ptr_struct = ((APTR *)(ArgBuf + of))[0];
            RMSG("Result-Arg: %s, Struct: %p", args[i].Name, ptr_struct);
            if (ptr_struct) {
               if (type & FD_RESOURCE) {
                  push_struct(Lua->script, ptr_struct, args[i].Name, (type & FD_ALLOC) ? true : false, false);
               }
               else {
                  if (named_struct_to_table(Lua, args[i].Name, ptr_struct) != ERR::Okay) {
                     luaL_error(Lua, ERR::Failed, "Failed to create struct for %s, %p", args[i].Name, ptr_struct);
                     return total;
                  }
                  if (type & FD_ALLOC) FreeResource(ptr_struct);
               }
            }
            else lua_pushnil(Lua);

            total++;
         }
         of += sizeof(APTR);
      }
      else if (type & FD_PTR) {
         if (sizeof(APTR) IS 8) of = ALIGN64(of);
         if (type & FD_FUNCTION) {
            FUNCTION *func;
            if ((func = (FUNCTION *)((APTR *)(ArgBuf+of))[0])) {
               log.trace("Removing function memory allocation %p", func);
               FreeResource(func);
            }
         }
         else if (type & FD_RESULT) {
            if (type & FD_OBJECT) {
               auto obj = (OBJECTPTR)((APTR *)(ArgBuf+of))[0];

               RMSG("Result-Arg: %s, Value: %p (Object)", args[i].Name, obj);

               if (obj) {
                  auto new_obj = push_object(Lua, obj);
                  new_obj->set_detached((type & FD_ALLOC) ? false : true);
               }
               else lua_pushnil(Lua);
            }
            else {
               RMSG("Result-Arg: %s, Value: %p (Pointer)", args[i].Name, ((APTR *)(ArgBuf+of))[0]);
               lua_pushlightuserdata(Lua, ((APTR *)(ArgBuf+of))[0]);
            }
            total++;
         }
         of += sizeof(APTR);
      }
      else if (type & FD_INT) {
         if (type & FD_RESULT) {
            RMSG("Result-Arg: %s, Value: %d (Long)", args[i].Name, ((int *)(ArgBuf+of))[0]);
            lua_pushinteger(Lua, ((int *)(ArgBuf+of))[0]);
            total++;
         }
         of += sizeof(int);
      }
      else if (type & FD_DOUBLE) {
         of = ALIGN64(of);
         if (type & FD_RESULT) {
            RMSG("Result-Arg: %s, Offset: %d, Value: %.2f (Double)", args[i].Name, of, ((double *)(ArgBuf+of))[0]);
            lua_pushnumber(Lua, ((double *)(ArgBuf+of))[0]);
            total++;
         }
         of += sizeof(double);
      }
      else if (type & FD_INT64) {
         of = ALIGN64(of);
         if (type & FD_RESULT) {
            RMSG("Result-Arg: %s, Value: %" PF64 " (Large)", args[i].Name, ((int64_t *)(ArgBuf+of))[0]);
            lua_pushnumber(Lua, ((int64_t *)(ArgBuf+of))[0]);
            total++;
         }
         of += sizeof(int64_t);
      }
      else if (type & FD_TAGS) {
         // Tags come last and have no result
         break;
      }
      else {
         log.warning("Unsupported arg %s, flags $%x, aborting now.", args[i].Name, type);
         break;
      }
   }

   RMSG("get_results: Wrote %d args.", total);
   return total;
}
