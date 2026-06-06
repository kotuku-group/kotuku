// Refer: lib_object.cpp

#include <new>

//********************************************************************************************************************
// Lua C closure executed via calls to obj.acName()

static int object_action_call_args(lua_State *Lua)
{
   auto obj_ref = object_context(Lua);
   AC action_id = AC(lua_tointeger(Lua, lua_upvalueindex(2)));
   bool release = false;

   // Extra 8 bytes are for overflow protection in build_args().

   int arg_index = 0;
   CSTRING error_msg = nullptr;
   auto argbuffer = std::make_unique<int8_t[]>(glActions[int(action_id)].Size+8);
   ERR error = build_args(Lua, glActions[int(action_id)].Name, glActions[int(action_id)].Args,
      glActions[int(action_id)].Size, argbuffer.get(), nullptr, arg_index, error_msg);

   if (error != ERR::Okay) {
      argbuffer.reset();
      if (error_msg) {
         if (arg_index) luaL_argerror(Lua, arg_index, error_msg);
         else luaL_error(Lua, error, "%s", error_msg);
      }
      else luaL_error(Lua, error, "Argument build failure for %s.", glActions[int(action_id)].Name);
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
   argbuffer.reset();
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
   int arg_index = 0;
   CSTRING error_msg = nullptr;
   ERR error = build_args(Lua, method->Name, method->Args, method->Size, argbuffer.get(), &result_count, arg_index,
      error_msg);
   if (error != ERR::Okay) {
      argbuffer.reset();
      if (error_msg) {
         if (arg_index) luaL_argerror(Lua, arg_index, error_msg);
         else luaL_error(Lua, error, "%s", error_msg);
      }
      else luaL_error(Lua, ERR::Args, "Argument build failure for method %s.", method->Name);
   }

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
   argbuffer.reset();
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

static void cleanup_function_arg(lua_State *Lua, FUNCTION *Func)
{
   if (not Func) return;

   if ((Func->isScript()) and (Func->Context IS Lua->script) and (Func->ProcedureID > 0)) {
      luaL_unref(Lua, LUA_REGISTRYINDEX, Func->ProcedureID);
      Func->ProcedureID = 0;
   }

   FreeResource(Func);
}

static void cleanup_argbuffer(lua_State *Lua, const FunctionField *Args, int ArgsSize, int8_t *ArgBuffer)
{
   if ((not Args) or (not ArgBuffer)) return;

   for (int i=0, j=0; (Args[i].Name) and (j < ArgsSize); i++) {
      const int type = Args[i].Type;

      if (type & FD_RESULT) {
         if (type & FD_ARRAY) {
            j = ALIGN64(j);
            j += sizeof(APTR);
         }
         else if ((type & FD_PTR) or (type & FD_STRUCT)) {
            j = ALIGN64(j);
            if ((type & FD_PTR) and (type & FD_FUNCTION) and (j + int(sizeof(APTR)) <= ArgsSize)) {
               cleanup_function_arg(Lua, ((FUNCTION **)(ArgBuffer + j))[0]);
               ((FUNCTION **)(ArgBuffer + j))[0] = nullptr;
            }
            j += sizeof(APTR);
         }
         else if (type & FD_STR) {
            j = ALIGN64(j);
            if (type & FD_CPP) {
               if (type & FD_MUTABLE) {
                  if (j + int(sizeof(std::string *)) <= ArgsSize) {
                     delete ((std::string **)(ArgBuffer + j))[0];
                     ((std::string **)(ArgBuffer + j))[0] = nullptr;
                  }
                  j += sizeof(std::string *);
               }
               else j += sizeof(std::string_view);
            }
            else j += sizeof(STRING);
         }
         else if (type & FD_INT) j += sizeof(int);
         else if (type & FD_DOUBLE) {
            j = ALIGN64(j);
            j += sizeof(double);
         }
         else if (type & FD_INT64) {
            j = ALIGN64(j);
            j += sizeof(int64_t);
         }
         continue;
      }

      if ((type & FD_BUFFER) or (Args[i+1].Type & FD_BUFSIZE)) {
         j = ALIGN64(j);
         j += sizeof(APTR);
      }
      else if (type & FD_STR) {
         j = ALIGN64(j);
         if (type & FD_MUTABLE) j += sizeof(CSTRING);
         else if (type & FD_CPP) j += sizeof(std::string_view);
         else j += sizeof(CSTRING);
      }
      else if (type & FD_PTR) {
         j = ALIGN64(j);
         if ((type & FD_FUNCTION) and (j + int(sizeof(APTR)) <= ArgsSize)) {
            cleanup_function_arg(Lua, ((FUNCTION **)(ArgBuffer + j))[0]);
            ((FUNCTION **)(ArgBuffer + j))[0] = nullptr;
         }
         j += sizeof(APTR);
      }
      else if (type & FD_INT) j += sizeof(int);
      else if (type & FD_DOUBLE) {
         j = ALIGN64(j);
         j += sizeof(double);
      }
      else if (type & FD_INT64) {
         j = ALIGN64(j);
         j += sizeof(int64_t);
      }
      else if (type & FD_TAGS) break;
   }
}

inline bool check_mutable_string_arg(GCstr *String)
{
   return lj_str_ismutable(String);
}

inline bool check_buffer_size_arg(int64_t Size, size_t Capacity)
{
   return (Size >= 0) and (uint64_t(Size) <= uint64_t(Capacity));
}

ERR build_args(lua_State *Lua, CSTRING Name, const FunctionField *Args, int ArgsSize, int8_t *ArgBuffer,
   int *ResultCount, int &ErrorArg, CSTRING &ErrorMsg)
{
   kt::Log log(__FUNCTION__);

   int top = lua_gettop(Lua);

   log.traceBranch("%d, %p, Top: %d", ArgsSize, ArgBuffer, top);

   clearmem(ArgBuffer, ArgsSize);

   auto fail = [&](ERR Error) -> ERR {
      cleanup_argbuffer(Lua, Args, ArgsSize, ArgBuffer);
      return Error;
   };

   auto fail_arg = [&](int ArgIndex, CSTRING Message) -> ERR {
      cleanup_argbuffer(Lua, Args, ArgsSize, ArgBuffer);
      ErrorArg = ArgIndex;
      ErrorMsg = Message;
      return ERR::WrongType;
   };

   int i, n;
   int resultcount = 0;
   int j = 0;
   size_t buffer_capacity = 0;
   bool buffer_capacity_known = false;
   for (i=0,n=1; (Args[i].Name) and (j < ArgsSize); i++) {
      if (not (Args[i].Type & FD_BUFSIZE)) buffer_capacity_known = false;

      if (Args[i].Type & FD_RESULT) {
         resultcount++;

         if (Args[i].Type & FD_ARRAY) {
            j = ALIGN64(j);
            j += sizeof(APTR);
         }
         else if ((Args[i].Type & FD_PTR) or (Args[i].Type & FD_STRUCT)) {
            j = ALIGN64(j);
            j += sizeof(APTR);
         }
         else if (Args[i].Type & FD_STR) {
            // std::string is treated as a special result type
            j = ALIGN64(j);
            if (Args[i].Type & FD_CPP) {
               if (Args[i].Type & FD_MUTABLE) { // std::string *
                  auto str_result = new (std::nothrow) std::string;
                  if (not str_result) return fail(ERR::AllocMemory);
                  ((std::string **)(ArgBuffer + j))[0] = str_result;
                  j += sizeof(std::string *);
               }
               else { // Embedded std::string_view
                  new (ArgBuffer + j) std::string_view;
                  j += sizeof(std::string_view);
               }
            }
            else j += sizeof(STRING);
         }
         else if (Args[i].Type & FD_INT) {
            j += sizeof(int);
         }
         else if (Args[i].Type & FD_DOUBLE) {
            j = ALIGN64(j);
            j += sizeof(double);
         }
         else if (Args[i].Type & FD_INT64) {
            j = ALIGN64(j);
            j += sizeof(int64_t);
         }
         else {
            log.warning("Unsupported result arg %s, flags $%.8x, aborting now.", Args[i].Name, Args[i].Type);
            return fail(ERR::WrongType);
         }

         continue;
      }

      int type = (top > 0) ? lua_type(Lua, n) : LUA_TNIL;

      //log.trace("Processing arg %s, type $%.8x", Args[i].Name, Args[i].Type);

      if ((Args[i].Type & FD_BUFFER) or (Args[i+1].Type & FD_BUFSIZE)) {
         j = ALIGN64(j);
         if (type IS LUA_TARRAY) {
            //log.trace("Arg: %s, Value: Buffer (Source is Memory)", Args[i].Name);

            auto array = lua_toarray(Lua, n);
            ((APTR *)(ArgBuffer + j))[0] = array->arraydata();
            j += sizeof(APTR);

            if (Args[i+1].Type & FD_BUFSIZE) {
               // Buffer size is optional (can be nil), so set the buffer size parameter by default.  The user can override it if
               // more arguments are specified in the function call.

               size_t memsize = array->len * array->elemsize;
               buffer_capacity = memsize;
               buffer_capacity_known = true;
               if (Args[i+1].Type & FD_INT)  ((int *)(ArgBuffer + j))[0] = int(memsize);
               else if (Args[i+1].Type & FD_INT64) ((int64_t *)(ArgBuffer + j))[0] = int64_t(memsize);
            }
         }
         else if (auto fstruct = (struct fstruct *)get_meta(Lua, n, "Tiri.struct")) {
            //log.trace("Arg: %s, Value: Buffer (Source is a struct)", Args[i].Name);

            ((APTR *)(ArgBuffer + j))[0] = fstruct->Data;
            j += sizeof(APTR);

            if (Args[i+1].Type & FD_BUFSIZE) {
               // Buffer size is optional (can be nil), so set the buffer size parameter by default.
               // The user can override it if more arguments are specified in the function call.

               buffer_capacity = fstruct->AlignedSize;
               buffer_capacity_known = true;
               if (Args[i+1].Type & FD_INT) ((int *)(ArgBuffer + j))[0] = fstruct->AlignedSize;
               else if (Args[i+1].Type & FD_INT64) ((int64_t *)(ArgBuffer + j))[0] = fstruct->AlignedSize;
            }
            n--; // Adjustment required due to successful get_meta()
         }
         else if (type IS LUA_TSTRING) {
            //log.trace("Arg: %s, Value: Buffer (Source is String)", Args[i].Name);
            auto string = strV(Lua->base + n - 1);
            if ((Args[i].Type & FD_MUTABLE) and (not check_mutable_string_arg(string))) {
               return fail_arg(n, "Mutable buffer required.");
            }
            size_t len = string->len;
            CSTRING str = (Args[i].Type & FD_MUTABLE) ? strdatawr(string) : strdata(string);
            ((CSTRING *)(ArgBuffer + j))[0] = str;
            j += sizeof(APTR);

            if (Args[i+1].Type & FD_BUFSIZE) {
               buffer_capacity = len;
               buffer_capacity_known = true;
               if (Args[i+1].Type & FD_INT) ((int *)(ArgBuffer + j))[0] = len;
               else if (Args[i+1].Type & FD_INT64) ((int64_t *)(ArgBuffer + j))[0] = len;
            }
         }
         else if (type IS LUA_TNUMBER) return fail_arg(n, "Cannot use a number as a buffer pointer.");
         else {
            //log.trace("Arg: %s, Value: Buffer", Args[i].Name);
            ((APTR *)(ArgBuffer + j))[0] = lua_touserdata(Lua, n);
            j += sizeof(APTR);
         }
      }
      else if (Args[i].Type & FD_STR) {
         j = ALIGN64(j);
         if (Args[i].Type & FD_MUTABLE) {
            if (type != LUA_TSTRING) return fail_arg(n, "Mutable buffer required.");
            auto string = strV(Lua->base + n - 1);
            if (not check_mutable_string_arg(string)) return fail_arg(n, "Mutable buffer required.");
            ((CSTRING *)(ArgBuffer + j))[0] = strdatawr(string);
            j += sizeof(CSTRING);
         }
         else if ((type IS LUA_TSTRING) or (type IS LUA_TNUMBER)) {
            if (Args[i].Type & FD_CPP) {
               ((std::string_view *)(ArgBuffer + j))[0] = lua_tostringview(Lua, n);
               j += sizeof(std::string_view);
            }
            else {
               ((CSTRING *)(ArgBuffer + j))[0] = lua_tostring(Lua, n);
               j += sizeof(CSTRING);
            }
         }
         else if (type <= 0) {
            if (Args[i].Type & FD_CPP) {
               ((std::string_view *)(ArgBuffer + j))[0] = std::string_view{};
               j += sizeof(std::string_view);
            }
            else {
               ((CSTRING *)(ArgBuffer + j))[0] = nullptr;
               j += sizeof(CSTRING);
            }
         }
         else {
            return fail_arg(n, "String required.");
         }

         //log.trace("Arg: %s, Value: %s", Args[i].Name, ((STRING *)(ArgBuffer + j))[0]);

      }
      else if (Args[i].Type & FD_PTR) {
         j = ALIGN64(j);
         if (Args[i].Type & FD_OBJECT) {
            if (auto obj_ref = lj_lib_optobject(Lua, n)) {
               OBJECTPTR ptr_obj;
               if (obj_ref->ptr) {
                  ((OBJECTPTR *)(ArgBuffer + j))[0] = obj_ref->ptr;
               }
               else if ((ptr_obj = access_object(obj_ref))) {
                  ((OBJECTPTR *)(ArgBuffer + j))[0] = ptr_obj;
                  release_object(obj_ref);
               }
               else {
                  log.warning("Unable to resolve object pointer for #%d.", obj_ref->uid);
                  ((OBJECTPTR *)(ArgBuffer + j))[0] = nullptr;
               }
            }
            else ((OBJECTPTR *)(ArgBuffer + j))[0] = nullptr;
         }
         else if (Args[i].Type & FD_FUNCTION) {
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

                  ((FUNCTION **)(ArgBuffer + j))[0] = func;

                  // The FUNCTION structure is freed when processing results
               }
               else return fail(ERR::AllocMemory);
            }
            else if ((type IS LUA_TNIL) or (type IS LUA_TNONE)) {
               ((FUNCTION **)(ArgBuffer + j))[0] = nullptr;
            }
            else {
               return fail_arg(n, "String or function required.");
            }
         }
         else if (type IS LUA_TSTRING) {
            //log.trace("Arg: %s, Value: Pointer (Source is String)", Args[i].Name);
            auto string = strV(Lua->base + n - 1);
            if ((Args[i].Type & FD_MUTABLE) and (not check_mutable_string_arg(string))) {
               return fail_arg(n, "Mutable buffer required.");
            }
            ((CSTRING *)(ArgBuffer + j))[0] = (Args[i].Type & FD_MUTABLE) ? strdatawr(string) : strdata(string);
         }
         else if (type IS LUA_TNUMBER) {
            return fail_arg(n, "Unable to convert number to a pointer.");
         }
         else if (type IS LUA_TARRAY) {
            auto array = arrayV(Lua, n);
            ((APTR *)(ArgBuffer + j))[0] = array->arraydata();
         }
         else {
            //log.trace("Arg: %s, Value: Pointer, SrcType: %s", Args[i].Name, lua_typename(Lua, type));

            if (auto fstruct = (struct fstruct *)get_meta(Lua, n, "Tiri.struct")) {
               ((APTR *)(ArgBuffer + j))[0] = fstruct->Data;
               //n--; // Adjustment required due to successful get_meta()
            }
            else ((APTR *)(ArgBuffer + j))[0] = lua_touserdata(Lua, n);
         }

         j += sizeof(APTR);
      }
      else if (Args[i].Type & FD_INT) {
         if ((type IS LUA_TUSERDATA) or (type IS LUA_TLIGHTUSERDATA)) {
            if (auto obj = lj_lib_checkobject(Lua, n)) {
               ((int *)(ArgBuffer + j))[0] = obj->uid;
            }
            else return fail_arg(n, "Unable to convert usertype to an integer.");
         }
         else if (type IS LUA_TBOOLEAN) {
            auto value = lua_toboolean(Lua, n);
            if ((Args[i].Type & FD_BUFSIZE) and buffer_capacity_known) {
               if (not check_buffer_size_arg(value, buffer_capacity)) {
                  return fail_arg(n, "Buffer size exceeds supplied buffer capacity.");
               }
               buffer_capacity_known = false;
            }
            ((int *)(ArgBuffer + j))[0] = value;
         }
         else if (type != LUA_TNIL) {
            auto value = lua_tointeger(Lua, n);
            if ((Args[i].Type & FD_BUFSIZE) and buffer_capacity_known) {
               if (not check_buffer_size_arg(value, buffer_capacity)) {
                  return fail_arg(n, "Buffer size exceeds supplied buffer capacity.");
               }
               buffer_capacity_known = false;
            }
            ((int *)(ArgBuffer + j))[0] = value;
         }
         else if (Args[i].Type & FD_BUFSIZE) {
            buffer_capacity_known = false; // Do not alter as the FD_BUFFER support would have managed it
         }
         else ((int *)(ArgBuffer + j))[0] = 0;
         //log.trace("Arg: %s, Value: %d / $%.8x", Args[i].Name, ((int *)(ArgBuffer + j))[0], ((int *)(ArgBuffer + j))[0]);
         j += sizeof(int);
      }
      else if (Args[i].Type & FD_DOUBLE) {
         j = ALIGN64(j);
         ((double *)(ArgBuffer + j))[0] = lua_tonumber(Lua, n);
         //log.trace("Arg: %s, Value: %.2f", Args[i].Name, ((double *)(ArgBuffer + j))[0]);
         j += sizeof(double);
      }
      else if (Args[i].Type & FD_INT64) {
         j = ALIGN64(j);
         if ((Args[i].Type & FD_BUFSIZE) and (type IS LUA_TNIL)) {
            buffer_capacity_known = false;
         }
         else {
            auto value = lua_tointeger(Lua, n);
            if ((Args[i].Type & FD_BUFSIZE) and buffer_capacity_known) {
               if (not check_buffer_size_arg(value, buffer_capacity)) {
                  return fail_arg(n, "Buffer size exceeds supplied buffer capacity.");
               }
               buffer_capacity_known = false;
            }
            ((int64_t *)(ArgBuffer + j))[0] = value;
         }
         //log.trace("Arg: %s, Value: %" PF64, Args[i].Name, ((LARGE *)(ArgBuffer + j))[0]);
         j += sizeof(int64_t);
      }
      else {
         log.warning("Unsupported arg %s, flags $%.8x, aborting now.", Args[i].Name, Args[i].Type);
         return fail(ERR::WrongType);
      }

      n++;
      if (top > 0) top--;
   }

   log.trace("Processed %d Args (%d bytes), detected %d result parameters.", i, j, resultcount);
   if (ResultCount) *ResultCount = resultcount;
   return ERR::Okay;
}

// Note: Please refer to process_results() in tiri_module.c for the 'official' take on result handling.

static int get_results(lua_State *Lua, const FunctionField *Args, const int8_t *ArgBuf)
{
   kt::Log log(__FUNCTION__);
   int i;

   RMSG("get_results(%p)", ArgBuf);

   int total = 0;
   int of = 0;
   for (i=0; Args[i].Name; i++) {
      const int type = Args[i].Type;
      if (type & FD_ARRAY) { // Pointer to an array.
         of = ALIGN64(of);
         if (type & FD_RESULT) {
            int total_elements = -1;  // If -1, make_any_array() assumes the array is null terminated.
            if (Args[i+1].Type & FD_ARRAYSIZE) {
               CPTR size_var = ArgBuf + of + sizeof(APTR);
               if (Args[i+1].Type & FD_INT) total_elements = ((int *)size_var)[0];
               else if (Args[i+1].Type & FD_INT64) total_elements = ((int64_t *)size_var)[0];
               else log.warning("Invalid parameter definition for '%s' of $%.8x", Args[i+1].Name, Args[i+1].Type);
            }

            if (CPTR values = ((APTR *)(ArgBuf + of))[0]) {
               make_any_array(Lua, type, Args[i].Name, total_elements, values);
               if (type & FD_ALLOC) FreeResource(values);
            }
            else lua_pushnil(Lua);
            total++;
         }
         of += sizeof(APTR);
      }
      else if (type & FD_STR) {
         of = ALIGN64(of);
         if (type & FD_RESULT) {
            // RESULT|CPP|STR = std::string_view = Function manipulates an embedded std::string_view that references an internal buffer
            //                  Can be combined with ALLOC, in which case the client must free the referenced data pointer.
            // RESULT|MUTABLE|CPP|STR = std::string * = Function manipulates the std::string reference directly
            // RESULT|STR = CSTRING * = Function returns a char pointer, either internal or allocated
            if (type & FD_CPP) {
               if (type & FD_MUTABLE) { // std::string *
                  if (auto str_result = ((std::string **)(ArgBuf + of))[0]; str_result) {
                     RMSG("Result-Arg: %s, Value: %.*s (std::string)", Args[i].Name,
                        int(str_result->size()), str_result->data());
                     lua_pushstring(Lua, str_result[0]);
                     delete str_result;
                  }
                  else lua_pushnil(Lua);
               }
               else { // Embedded std::string_view
                  auto &str_result = ((std::string_view *)(ArgBuf + of))[0];
                  RMSG("Result-Arg: %s, Value: %.*s (std::string_view)", Args[i].Name,
                     int(str_result.size()), str_result.data());
                  lua_pushstring(Lua, str_result);
                  if ((type & FD_ALLOC) and str_result.data()) FreeResource(GetMemoryID(str_result.data()));
               }
            }
            else { // CSTRING
               CPTR val = ArgBuf + of;
               RMSG("Result-Arg: %s, Value: %.20s (String)", Args[i].Name, ((STRING *)val)[0]);
               lua_pushstring(Lua, ((STRING *)val)[0]);
               if (type & FD_ALLOC) {
                  APTR ptr = ((STRING *)val)[0];
                  if (ptr) FreeResource(ptr);
               }
            }
            total++;
         }
         if ((type & FD_CPP) and (not (type & FD_MUTABLE))) of += sizeof(std::string_view);
         else of += sizeof(STRING);
      }
      else if (type & FD_STRUCT) { // Pointer to a struct
         of = ALIGN64(of);
         if (type & FD_RESULT) {
            APTR ptr_struct = ((APTR *)(ArgBuf + of))[0];
            RMSG("Result-Arg: %s, Struct: %p", Args[i].Name, ptr_struct);
            if (ptr_struct) {
               if (type & FD_RESOURCE) {
                  push_struct(Lua->script, ptr_struct, Args[i].Name, (type & FD_ALLOC) ? true : false, false);
               }
               else {
                  if (named_struct_to_table(Lua, Args[i].Name, ptr_struct) != ERR::Okay) {
                     luaL_error(Lua, ERR::Failed, "Failed to create struct for %s, %p", Args[i].Name, ptr_struct);
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
         of = ALIGN64(of);
         if (type & FD_FUNCTION) {
            if (auto func = (FUNCTION *)((APTR *)(ArgBuf+of))[0]) {
               log.trace("Removing function memory allocation %p", func);
               FreeResource(func);
            }
         }
         else if (type & FD_RESULT) {
            if (type & FD_OBJECT) {
               auto obj = (OBJECTPTR)((APTR *)(ArgBuf+of))[0];

               RMSG("Result-Arg: %s, Value: %p (Object)", Args[i].Name, obj);

               if (obj) {
                  auto new_obj = push_object(Lua, obj);
                  new_obj->set_detached((type & FD_ALLOC) ? false : true);
               }
               else lua_pushnil(Lua);
            }
            else {
               RMSG("Result-Arg: %s, Value: %p (Pointer)", Args[i].Name, ((APTR *)(ArgBuf+of))[0]);
               lua_pushlightuserdata(Lua, ((APTR *)(ArgBuf+of))[0]);
            }
            total++;
         }
         of += sizeof(APTR);
      }
      else if (type & FD_INT) {
         if (type & FD_RESULT) {
            RMSG("Result-Arg: %s, Value: %d (Long)", Args[i].Name, ((int *)(ArgBuf+of))[0]);
            lua_pushinteger(Lua, ((int *)(ArgBuf+of))[0]);
            total++;
         }
         of += sizeof(int);
      }
      else if (type & FD_DOUBLE) {
         of = ALIGN64(of);
         if (type & FD_RESULT) {
            RMSG("Result-Arg: %s, Offset: %d, Value: %.2f (Double)", Args[i].Name, of, ((double *)(ArgBuf+of))[0]);
            lua_pushnumber(Lua, ((double *)(ArgBuf+of))[0]);
            total++;
         }
         of += sizeof(double);
      }
      else if (type & FD_INT64) {
         of = ALIGN64(of);
         if (type & FD_RESULT) {
            RMSG("Result-Arg: %s, Value: %" PF64 " (Large)", Args[i].Name, ((int64_t *)(ArgBuf+of))[0]);
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
         log.warning("Unsupported arg %s, flags $%x, aborting now.", Args[i].Name, type);
         break;
      }
   }

   RMSG("get_results: Wrote %d Args.", total);
   return total;
}
