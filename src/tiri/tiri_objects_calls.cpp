// Refer: lib_object.cpp

#include <new>
#include <array>
#include <limits>
#include <vector>

struct pending_function_arg {
   int Offset;
   int StackIndex;
   int Type;
};

//********************************************************************************************************************

[[nodiscard]] inline ERR dispatch_action(GCobject *Def, ACTIONID ActionID, APTR Args, bool &Release)
{
   Release = false;

   if (auto direct = direct_object_ptr(Def)) return Action(ActionID, direct, Args);

   OBJECTPTR obj;
   auto error = access_object(Def, obj);
   if (error IS ERR::Okay) {
      Release = true;
      return Action(ActionID, obj, Args);
   }
   else return error;
}

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
   error = dispatch_action(obj_ref, action_id, argbuffer.get(), release);

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

   error = dispatch_action(def, action_id, nullptr, release);

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
   auto error = build_args(Lua, method->Name, method->Args, method->Size, argbuffer.get(), &result_count, arg_index,
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

   error = dispatch_action(def, method->MethodID, argbuffer.get(), release);

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
   bool release = false;
   auto method = (MethodEntry *)lua_touserdata(Lua, lua_upvalueindex(2));

   auto error = dispatch_action(def, method->MethodID, nullptr, release);

   lua_pushinteger(Lua, int(error));

   if (release) release_object(def);
   report_action_error(Lua, def, method->Name, error);
   return 1;
}

//********************************************************************************************************************
// Helpers for argument building

inline void release_func_id(lua_State *Lua, FUNCTION *Func)
{
   if (Func->ProcedureID > 0) luaL_unref(Lua, LUA_REGISTRYINDEX, Func->ProcedureID);
}

inline void release_consumed_func(lua_State *Lua, FUNCTION *Func)
{
   if (Func->consumed() and (Func->ProcedureID > 0)) luaL_unref(Lua, LUA_REGISTRYINDEX, Func->ProcedureID);
}

static void materialise_function_arg(lua_State *Lua, const pending_function_arg &Pending, int8_t *ArgBuffer)
{
   int ref;

   if (Pending.Type IS LUA_TSTRING) {
      lua_getglobal(Lua, lua_tostring(Lua, Pending.StackIndex));
      ref = luaL_ref(Lua, LUA_REGISTRYINDEX);
   }
   else {
      lua_pushvalue(Lua, Pending.StackIndex);
      ref = luaL_ref(Lua, LUA_REGISTRYINDEX);
   }

   *(FUNCTION *)(ArgBuffer + Pending.Offset) = FUNCTION(Lua->script, ref);
}

template <class T> static void delete_cpp_array_arg(APTR Array)
{
   delete (kt::vector<T> *)Array;
}

static void delete_cpp_array_arg(int Type, APTR Array)
{
   if (not Array) return;

   if (Type & FD_STR) delete_cpp_array_arg<std::string>(Array);
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) delete_cpp_array_arg<OBJECTPTR>(Array);
   else if (Type & FD_PTR) delete_cpp_array_arg<APTR>(Array);
   else if (Type & FD_DOUBLE) delete_cpp_array_arg<double>(Array);
   else if (Type & FD_FLOAT) delete_cpp_array_arg<float>(Array);
   else if (Type & FD_INT64) delete_cpp_array_arg<int64_t>(Array);
   else if (Type & FD_INT) delete_cpp_array_arg<int>(Array);
   else if (Type & FD_WORD) delete_cpp_array_arg<int16_t>(Array);
   else if (Type & FD_BYTE) delete_cpp_array_arg<uint8_t>(Array);
}

template <class T> static ERR make_cpp_array_arg(APTR *Result)
{
   auto vector = new (std::nothrow) kt::vector<T>;
   if (not vector) return ERR::AllocMemory;

   *Result = vector;
   return ERR::Okay;
}

static ERR make_cpp_array_arg(int Type, APTR *Result)
{
   if (Type & FD_STR) return make_cpp_array_arg<std::string>(Result);
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) return make_cpp_array_arg<OBJECTPTR>(Result);
   else if (Type & FD_PTR) return make_cpp_array_arg<APTR>(Result);
   else if (Type & FD_DOUBLE) return make_cpp_array_arg<double>(Result);
   else if (Type & FD_FLOAT) return make_cpp_array_arg<float>(Result);
   else if (Type & FD_INT64) return make_cpp_array_arg<int64_t>(Result);
   else if (Type & FD_INT) return make_cpp_array_arg<int>(Result);
   else if (Type & FD_WORD) return make_cpp_array_arg<int16_t>(Result);
   else if (Type & FD_BYTE) return make_cpp_array_arg<uint8_t>(Result);
   else return ERR::NoSupport;
}

template <class T> static ERR copy_primitive_cpp_array_arg(GCarray *Array, AET Type, kt::vector<T> *Vector)
{
   if (Array->elemtype != Type) return ERR::InvalidType;

   Vector->reserve(Array->len);
   auto values = Array->get<T>();
   for (uint32_t i=0; i < Array->len; i++) {
      Vector->push_back(values[i]);
   }

   return ERR::Okay;
}

static ERR copy_string_cpp_array_arg(GCarray *Array, kt::vector<std::string> *Vector)
{
   Vector->reserve(Array->len);

   if (Array->elemtype IS AET::STR_GC) {
      auto refs = Array->get<GCRef>();
      for (uint32_t i=0; i < Array->len; i++) {
         if (gcref(refs[i])) {
            auto string = gco_to_string(gcref(refs[i]));
            Vector->emplace_back(strdata(string), string->len);
         }
         else Vector->emplace_back();
      }
   }
   else if (Array->elemtype IS AET::STR_CPP) {
      auto strings = Array->get<std::string>();
      for (uint32_t i=0; i < Array->len; i++) {
         Vector->push_back(strings[i]);
      }
   }
   else if (Array->elemtype IS AET::CSTR) {
      auto strings = Array->get<CSTRING>();
      for (uint32_t i=0; i < Array->len; i++) {
         if (strings[i]) Vector->emplace_back(strings[i]);
         else Vector->emplace_back();
      }
   }
   else return ERR::InvalidType;

   return ERR::Okay;
}

static ERR copy_object_cpp_array_arg(GCarray *Array, kt::vector<OBJECTPTR> *Vector)
{
   if (Array->elemtype != AET::OBJECT) return ERR::InvalidType;

   Vector->reserve(Array->len);
   auto refs = Array->get<GCRef>();
   for (uint32_t i=0; i < Array->len; i++) {
      if (not gcref(refs[i])) {
         Vector->push_back(nullptr);
         continue;
      }

      auto obj_ref = gco_to_object(gcref(refs[i]));
      if (auto direct = direct_object_ptr(obj_ref)) Vector->push_back(direct);
      else {
         OBJECTPTR ptr_obj;
         if (!access_object(obj_ref, ptr_obj)) {
            Vector->push_back(ptr_obj);
            release_object(obj_ref);
         }
         else Vector->push_back(nullptr);
      }
   }

   return ERR::Okay;
}

static ERR copy_cpp_array_arg(int Type, GCarray *Array, APTR *Result)
{
   APTR output = nullptr;
   auto error = make_cpp_array_arg(Type, &output);
   if (error != ERR::Okay) return error;

   if (Type & FD_STR) error = copy_string_cpp_array_arg(Array, (kt::vector<std::string> *)output);
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) {
      error = copy_object_cpp_array_arg(Array, (kt::vector<OBJECTPTR> *)output);
   }
   else if (Type & FD_PTR) {
      error = copy_primitive_cpp_array_arg<APTR>(Array, AET::PTR, (kt::vector<APTR> *)output);
   }
   else if (Type & FD_DOUBLE) {
      error = copy_primitive_cpp_array_arg<double>(Array, AET::DOUBLE, (kt::vector<double> *)output);
   }
   else if (Type & FD_FLOAT) {
      error = copy_primitive_cpp_array_arg<float>(Array, AET::FLOAT, (kt::vector<float> *)output);
   }
   else if (Type & FD_INT64) {
      error = copy_primitive_cpp_array_arg<int64_t>(Array, AET::INT64, (kt::vector<int64_t> *)output);
   }
   else if (Type & FD_INT) {
      error = copy_primitive_cpp_array_arg<int>(Array, AET::INT32, (kt::vector<int> *)output);
   }
   else if (Type & FD_WORD) {
      error = copy_primitive_cpp_array_arg<int16_t>(Array, AET::INT16, (kt::vector<int16_t> *)output);
   }
   else if (Type & FD_BYTE) {
      error = copy_primitive_cpp_array_arg<uint8_t>(Array, AET::BYTE, (kt::vector<uint8_t> *)output);
   }
   else error = ERR::NoSupport;

   if (error != ERR::Okay) {
      delete_cpp_array_arg(Type, output);
      return error;
   }

   *Result = output;
   return ERR::Okay;
}

template <class T> static void push_cpp_array_arg(lua_State *Lua, int Type, std::string_view Name, APTR Source)
{
   auto vector = (kt::vector<T> *)Source;
   make_any_array(Lua, Type, Name, int(vector->size()), vector->data());
}

static bool push_cpp_array_arg(lua_State *Lua, int Type, std::string_view Name, APTR Source)
{
   if (Type & FD_STR) {
      auto vector = (kt::vector<std::string> *)Source;
      make_array(Lua, AET::STR_CPP, int(vector->size()), vector);
   }
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) {
      auto vector = (kt::vector<OBJECTPTR> *)Source;
      make_array(Lua, AET::OBJECT, int(vector->size()), vector->data());
   }
   else if (Type & FD_PTR) push_cpp_array_arg<APTR>(Lua, Type, Name, Source);
   else if (Type & FD_DOUBLE) push_cpp_array_arg<double>(Lua, Type, Name, Source);
   else if (Type & FD_FLOAT) push_cpp_array_arg<float>(Lua, Type, Name, Source);
   else if (Type & FD_INT64) push_cpp_array_arg<int64_t>(Lua, Type, Name, Source);
   else if (Type & FD_INT) push_cpp_array_arg<int>(Lua, Type, Name, Source);
   else if (Type & FD_WORD) push_cpp_array_arg<int16_t>(Lua, Type, Name, Source);
   else if (Type & FD_BYTE) push_cpp_array_arg<uint8_t>(Lua, Type, Name, Source);
   else return false;

   return true;
}

//********************************************************************************************************************
// All dynamic std::span specialisations share the same pointer and element-count layout.  The field descriptor supplies
// the element type required to validate Tiri's backing storage.

using GenericSpan = std::span<int8_t>;

static ERR span_storage_bounds(size_t Offset, size_t Capacity, size_t *AlignedOffset, size_t *EndOffset)
{
   constexpr auto alignment = alignof(GenericSpan);
   if (Offset > std::numeric_limits<size_t>::max() - (alignment - 1)) return ERR::OutOfRange;

   const auto aligned_offset = (Offset + alignment - 1) & ~(alignment - 1);
   if (aligned_offset > std::numeric_limits<size_t>::max() - sizeof(GenericSpan)) return ERR::OutOfRange;
   const auto end_offset = aligned_offset + sizeof(GenericSpan);
   if (end_offset > Capacity) return ERR::OutOfRange;

   *AlignedOffset = aligned_offset;
   *EndOffset = end_offset;
   return ERR::Okay;
}

static ERR span_element_metadata(lua_State *Lua, const FunctionField &Field, AET *ElementType,
   size_t *ElementSize, struct_record **StructDef, bool *RawBytes)
{
   *StructDef = nullptr;
   *RawBytes = false;

   if (Field.Type & FD_STRUCT) {
      if ((not Field.Name) or (not valid_struct_name(Field.Name))) return ERR::InvalidData;
      auto definition = find_struct(Lua, struct_name_prefix(Field.Name));
      if (not definition) return ERR::Search;
      if (definition->Size <= 0) return ERR::InvalidData;
      *ElementType = AET::STRUCT;
      *ElementSize = size_t(definition->Size);
      *StructDef = definition;
      return ERR::Okay;
   }

   const int type_count = bool(Field.Type & FD_DOUBLE) + bool(Field.Type & FD_INT64) +
      bool(Field.Type & FD_FLOAT) + bool(Field.Type & FD_INT) + bool(Field.Type & FD_WORD) +
      bool(Field.Type & FD_BYTE) + bool(Field.Type & FD_PTR) + bool(Field.Type & FD_STR);
   if (type_count != 1) return ERR::InvalidData;

   if (Field.Type & FD_DOUBLE) { *ElementType = AET::DOUBLE; *ElementSize = sizeof(double); }
   else if (Field.Type & FD_INT64) { *ElementType = AET::INT64; *ElementSize = sizeof(int64_t); }
   else if (Field.Type & FD_FLOAT) { *ElementType = AET::FLOAT; *ElementSize = sizeof(float); }
   else if (Field.Type & FD_INT) { *ElementType = AET::INT32; *ElementSize = sizeof(int); }
   else if (Field.Type & FD_WORD) { *ElementType = AET::INT16; *ElementSize = sizeof(int16_t); }
   else if (Field.Type & (FD_BYTE|FD_PTR)) {
      *ElementType = AET::BYTE;
      *ElementSize = sizeof(int8_t);
      *RawBytes = bool(Field.Type & FD_PTR);
   }
   else return ERR::InvalidData;

   return ERR::Okay;
}

static ERR build_span_arg(lua_State *Lua, int StackIndex, const FunctionField &Field, int8_t *ArgBuffer,
   size_t Offset, size_t Capacity, size_t *EndOffset, CSTRING *ErrorMsg)
{
   if (Field.Type & (FD_RESULT|FD_ALLOC)) {
      *ErrorMsg = "Result and allocated spans are not supported.";
      return ERR::NoSupport;
   }

   size_t aligned_offset;
   if (auto error = span_storage_bounds(Offset, Capacity, &aligned_offset, EndOffset); error != ERR::Okay) {
      *ErrorMsg = "Span storage exceeds the argument record.";
      return error;
   }

   AET element_type;
   size_t element_size;
   struct_record *struct_def;
   bool raw_bytes;
   if (auto error = span_element_metadata(Lua, Field, &element_type, &element_size, &struct_def, &raw_bytes);
       error != ERR::Okay) {
      *ErrorMsg = (error IS ERR::Search) ? "Unknown span structure metadata." : "Invalid span element metadata.";
      return error;
   }

   CPTR data = nullptr;
   size_t extent = 0;
   const bool mutable_span = Field.Type & FD_MUTABLE;
   const auto value_type = lua_type(Lua, StackIndex);

   if ((value_type IS LUA_TNIL) or (value_type IS LUA_TNONE)) {
      // The canonical empty span is constructed below.
   }
   else if (value_type IS LUA_TARRAY) {
      auto array = lua_toarray(Lua, StackIndex);
      if (mutable_span and array->is_readonly()) {
         *ErrorMsg = "Mutable span requires writable array storage.";
         return ERR::Immutable;
      }

      if (raw_bytes) {
         if (array->len and (array->elemsize > std::numeric_limits<size_t>::max() / array->len)) {
            *ErrorMsg = "Array backing storage is too large for the span extent.";
            return ERR::OutOfRange;
         }
         extent = size_t(array->len) * array->elemsize;
      }
      else {
         if ((array->elemtype != element_type) or (size_t(array->elemsize) != element_size)) {
            *ErrorMsg = "Array element type does not match the span element type.";
            return ERR::InvalidType;
         }
         if ((element_type IS AET::STRUCT) and (array->structdef != struct_def)) {
            *ErrorMsg = "Structure array type does not match the span structure type.";
            return ERR::InvalidType;
         }
         extent = array->len;
      }
      data = array->arraydata();
   }
   else if (value_type IS LUA_TSTRING) {
      if (not (element_type IS AET::BYTE)) {
         *ErrorMsg = "String storage is compatible only with byte or character spans.";
         return ERR::InvalidType;
      }

      auto string = strV(Lua->base + StackIndex - 1);
      if (mutable_span and not lj_str_ismutable(string)) {
         *ErrorMsg = "Mutable span requires string.alloc() storage.";
         return ERR::Immutable;
      }
      data = mutable_span ? CPTR(strdatawr(string)) : CPTR(strdata(string));
      extent = string->len;
   }
   else if (auto native_struct = lua_isstruct(Lua, StackIndex) ? lua_tostruct(Lua, StackIndex) : nullptr) {
      if (not (element_type IS AET::STRUCT)) {
         *ErrorMsg = "Structure storage requires a matching structure span.";
         return ERR::InvalidType;
      }
      if (lj_struct_stale(native_struct)) {
         *ErrorMsg = "Struct's providing object has been destroyed.";
         return ERR::DoesNotExist;
      }
      if ((native_struct->def != struct_def) or (size_t(native_struct->structsize) != element_size)) {
         *ErrorMsg = "Structure type does not match the span structure type.";
         return ERR::InvalidType;
      }
      if (not native_struct->data) {
         *ErrorMsg = "Structure span storage is unavailable.";
         return ERR::InvalidData;
      }
      data = native_struct->data;
      extent = 1;
   }
   else {
      *ErrorMsg = "Span requires a compatible array, string, structure or nil.";
      return ERR::InvalidType;
   }

   if (extent and not data) {
      *ErrorMsg = "Non-empty span storage is unavailable.";
      return ERR::InvalidData;
   }
   new (ArgBuffer + aligned_offset) GenericSpan((int8_t *)data, extent);
   return ERR::Okay;
}

// Cleans up allocations held by an argument buffer.
// If ReleaseFunctions is true, also releases any FD_FUNCTION Lua registry references.
void cleanup_argbuffer(lua_State *Lua, const FunctionField *Args, int ArgsSize, int8_t *ArgBuffer,
   bool ReleaseFunctions)
{
   if ((not Args) or (not ArgBuffer)) return;

   for (int i=0, j=0; (Args[i].Name) and (j < ArgsSize); i++) {
      const int type = Args[i].Type;

      if ((type & FDF_SPAN) IS FDF_SPAN) {
         size_t aligned_offset, end_offset;
         if (span_storage_bounds(size_t(j), size_t(ArgsSize), &aligned_offset, &end_offset) != ERR::Okay) {
            return;
         }
         j = int(end_offset);
      }
      else if (type & FD_RESULT) {
         if (type & FD_ARRAY) {
            j = ALIGN64(j);
            if ((type & FD_CPP) and (j + int(sizeof(APTR)) <= ArgsSize)) {
               delete_cpp_array_arg(type, ((APTR *)(ArgBuffer + j))[0]);
               ((APTR *)(ArgBuffer + j))[0] = nullptr;
            }
            j += sizeof(APTR);
         }
         else if (type & FD_FUNCTION) {
            j = ALIGN64(j);
            if (ReleaseFunctions) release_func_id(Lua, ((FUNCTION *)(ArgBuffer + j)));
            j += sizeof(FUNCTION);
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
         else if ((type & FD_PTR) or (type & FD_STRUCT)) j = ALIGN64(j) + sizeof(APTR);
         else if (type & FD_INT) j += sizeof(int);
         else if (type & FD_DOUBLE) j = ALIGN64(j) + sizeof(double);
         else if (type & FD_INT64) j = ALIGN64(j) + sizeof(int64_t);
         continue;
      }

      if (type & FD_ARRAY) {
         j = ALIGN64(j);
         if ((type & FD_CPP) and (j + int(sizeof(APTR)) <= ArgsSize)) {
            delete_cpp_array_arg(type, ((APTR *)(ArgBuffer + j))[0]);
            ((APTR *)(ArgBuffer + j))[0] = nullptr;
         }
         j += sizeof(APTR);
      }
      else if (type & FD_STR) {
         j = ALIGN64(j);
         if (type & FD_MUTABLE) j += sizeof(CSTRING);
         else if (type & FD_CPP) j += sizeof(std::string_view);
         else j += sizeof(CSTRING);
      }
      else if (type & FD_FUNCTION) {
         j = ALIGN64(j);
         if (ReleaseFunctions) release_func_id(Lua, ((FUNCTION *)(ArgBuffer + j)));
         j += sizeof(FUNCTION);
      }
      else if (type & FD_PTR) j = ALIGN64(j) + sizeof(APTR);
      else if (type & FD_INT) j += sizeof(int);
      else if (type & FD_DOUBLE) j = ALIGN64(j) + sizeof(double);
      else if (type & FD_INT64) j = ALIGN64(j) + sizeof(int64_t);
      else if (type & FD_TAGS) break;
   }
}

inline bool check_buffer_size_arg(int64_t Size, size_t Capacity)
{
   return (Size >= 0) and (uint64_t(Size) <= uint64_t(Capacity));
}

//********************************************************************************************************************
// Build an argument buffer for actions and methods.  This follows the FD parsing logic of module_call() for the most
// part, as that is the base-line for argument parsing.  However, some differences apply in part due to the fact that
// action parameters are stored in structs.
//
// NOTE: FD_RESULT types are treated as real result values that are returned after the ERR code.  They don't need to
// be provided up-front by the client.  FD_MUTABLE types are mutable buffers that must be provided by the client and
// are manipulated in-place by the action.
//
// Long jumps are not permitted (interferes with RAII cleanup).  Thunk arguments are resolved up-front under
// error protection so that later stack reads can never raise a Lua error mid-function.

ERR build_args(lua_State *Lua, CSTRING Name, const FunctionField *Args, int ArgsSize, int8_t *ArgBuffer,
   int *ResultCount, int &ErrorArg, CSTRING &ErrorMsg)
{
   kt::Log log(__FUNCTION__);

   int top = lua_gettop(Lua);

   log.traceBranch("%d, %p, Top: %d", ArgsSize, ArgBuffer, top);

   if (top > 0) {
      if (int failed = lua_resolve_thunks(Lua, 1, top)) {
         ErrorArg = failed;
         ErrorMsg = lua_tostring(Lua, failed);  // Error value is anchored in the failed slot
         if (not ErrorMsg) ErrorMsg = "Exception in thunk argument.";
         return ERR::Exception;
      }
   }

   clearmem(ArgBuffer, ArgsSize);

   auto fail = [&](ERR Error) -> ERR {
      cleanup_argbuffer(Lua, Args, ArgsSize, ArgBuffer, true);
      return Error;
   };

   auto fail_arg = [&](int ArgIndex, CSTRING Message) -> ERR {
      cleanup_argbuffer(Lua, Args, ArgsSize, ArgBuffer, true);
      ErrorArg = ArgIndex;
      ErrorMsg = Message;
      return ERR::WrongType;
   };

   int i, n;
   int resultcount = 0;
   int j = 0;
   size_t buffer_capacity = 0;
   bool buffer_capacity_known = false;
   std::vector<pending_function_arg> pending_functions;
   for (i=0,n=1; (Args[i].Name) and (j < ArgsSize); i++) {
      if (not (Args[i].Type & FD_BUFSIZE)) buffer_capacity_known = false;

      if ((Args[i].Type & FDF_SPAN) IS FDF_SPAN) {
         size_t end_offset;
         CSTRING span_error = nullptr;
         auto error = build_span_arg(Lua, n, Args[i], ArgBuffer, size_t(j), size_t(ArgsSize), &end_offset,
            &span_error);
         if (error != ERR::Okay) {
            cleanup_argbuffer(Lua, Args, ArgsSize, ArgBuffer, true);
            ErrorArg = n;
            ErrorMsg = span_error;
            return error;
         }
         j = int(end_offset);
         n++;
         if (top > 0) top--;
         continue;
      }
      else if (Args[i].Type & FD_RESULT) {
         resultcount++;

         if (Args[i].Type & FD_ARRAY) {
            j = ALIGN64(j);

            if (Args[i].Type & FD_CPP) { // kt::vector<> *
               auto error = make_cpp_array_arg(Args[i].Type, (APTR *)(ArgBuffer + j));
               if (error != ERR::Okay) return fail(error);
            }

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

      int type = (top > 0) ? lua_resolved_type(Lua, n) : LUA_TNIL;

      //log.trace("Processing arg %s, type $%.8x", Args[i].Name, Args[i].Type);

      if (Args[i].Type & FD_ARRAY) {
         j = ALIGN64(j);
         if (type != LUA_TARRAY) return fail_arg(n, "Array required.");

         auto array = lua_toarray(Lua, n); // Safe (confirmed array type)
         if (Args[i].Type & FD_CPP) {
            APTR vector = nullptr;
            auto error = copy_cpp_array_arg(Args[i].Type, array, &vector);
            if (error IS ERR::InvalidType) return fail_arg(n, "Array element type mismatch.");
            else if (error != ERR::Okay) return fail(error);
            ((APTR *)(ArgBuffer + j))[0] = vector;
            j += sizeof(APTR);
         }
         else {
            ((APTR *)(ArgBuffer + j))[0] = array->arraydata();
            j += sizeof(APTR);

            if (Args[i+1].Type & (FD_BUFSIZE|FD_ARRAYSIZE)) {
               size_t total = (Args[i+1].Type & FD_BUFSIZE) ? array->len * array->elemsize : array->len;
               if (Args[i+1].Type & FD_BUFSIZE) {
                  buffer_capacity = total;
                  buffer_capacity_known = true;
               }
               if (Args[i+1].Type & FD_INT) ((int *)(ArgBuffer + j))[0] = int(total);
               else if (Args[i+1].Type & FD_INT64) ((int64_t *)(ArgBuffer + j))[0] = int64_t(total);
            }
         }
      }
      else if ((Args[i].Type & FD_PTR) and (Args[i+1].Type & FD_PTRSIZE)) {
         j = ALIGN64(j);
         if (type IS LUA_TARRAY) {
            auto array = lua_toarray(Lua, n);
            if (array->len and (array->elemsize > std::numeric_limits<size_t>::max() / array->len)) {
               return fail_arg(n, "Array backing storage is too large for the pointer-size contract.");
            }

            ((APTR *)(ArgBuffer + j))[0] = array->arraydata();
            j += sizeof(APTR);

            const size_t memsize = size_t(array->len) * array->elemsize;
            buffer_capacity = memsize;
            buffer_capacity_known = true;
            if ((Args[i+1].Type & FD_INT) and (memsize <= size_t(INT_MAX))) {
               ((int *)(ArgBuffer + j))[0] = int(memsize);
            }
            else if (Args[i+1].Type & FD_INT64) ((int64_t *)(ArgBuffer + j))[0] = int64_t(memsize);
            else return fail_arg(n, "Array backing storage exceeds the pointer-size field.");
         }
         else if (auto native_struct = lua_isstruct(Lua, n) ? lua_tostruct(Lua, n) : nullptr) {
            if (lj_struct_stale(native_struct)) return fail_arg(n, "Struct's providing object has been destroyed.");
            ((APTR *)(ArgBuffer + j))[0] = native_struct->data;
            j += sizeof(APTR);

            buffer_capacity = native_struct->structsize;
            buffer_capacity_known = true;
            if (Args[i+1].Type & FD_INT) ((int *)(ArgBuffer + j))[0] = int(native_struct->structsize);
            else if (Args[i+1].Type & FD_INT64) ((int64_t *)(ArgBuffer + j))[0] = int64_t(native_struct->structsize);
         }
         else if (type IS LUA_TSTRING) {
            auto string = strV(Lua->base + n - 1);
            if ((Args[i].Type & FD_MUTABLE) and (not lj_str_ismutable(string))) {
               return fail_arg(n, "Mutable buffer required.");
            }

            ((CSTRING *)(ArgBuffer + j))[0] = (Args[i].Type & FD_MUTABLE) ? strdatawr(string) : strdata(string);
            j += sizeof(APTR);

            buffer_capacity = string->len;
            buffer_capacity_known = true;
            if ((Args[i+1].Type & FD_INT) and (string->len <= size_t(INT_MAX))) {
               ((int *)(ArgBuffer + j))[0] = int(string->len);
            }
            else if (Args[i+1].Type & FD_INT64) ((int64_t *)(ArgBuffer + j))[0] = int64_t(string->len);
            else return fail_arg(n, "String storage exceeds the pointer-size field.");
         }
         else {
            ((APTR *)(ArgBuffer + j))[0] = lua_touserdata(Lua, n);
            j += sizeof(APTR);
         }
      }
      else if (Args[i].Type & FD_STR) {
         j = ALIGN64(j);
         if (Args[i].Type & FD_MUTABLE) {
            if (type != LUA_TSTRING) return fail_arg(n, "Mutable buffer required.");
            auto string = strV(Lua->base + n - 1);
            if (not lj_str_ismutable(string)) return fail_arg(n, "Mutable buffer required.");
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
         else return fail_arg(n, "String required.");

         //log.trace("Arg: %s, Value: %s", Args[i].Name, ((STRING *)(ArgBuffer + j))[0]);
      }
      else if (Args[i].Type & FD_FUNCTION) {
         j = ALIGN64(j);
         new (ArgBuffer + j) FUNCTION;

         if ((type IS LUA_TSTRING) or (type IS LUA_TFUNCTION)) {
            pending_functions.push_back({ j, n, type });
         }
         else if ((type != LUA_TNIL) and (type != LUA_TNONE)) {
            return fail_arg(n, "String or function required.");
         }

         j += sizeof(FUNCTION);
      }
      else if (Args[i].Type & FD_PTR) {
         j = ALIGN64(j);
         if (Args[i].Type & FD_OBJECT) {
            if (auto obj_ref = lj_lib_optobject(Lua, n, false)) { // Performs thunk resolution
               OBJECTPTR ptr_obj;
               if (auto direct = direct_object_ptr(obj_ref)) {
                  ((OBJECTPTR *)(ArgBuffer + j))[0] = direct;
               }
               else if (!access_object(obj_ref, ptr_obj)) {
                  ((OBJECTPTR *)(ArgBuffer + j))[0] = ptr_obj;
                  release_object(obj_ref);
               }
               else {
                  log.warning("Unable to resolve object pointer for #%d.", obj_ref->uid);
                  ((OBJECTPTR *)(ArgBuffer + j))[0] = nullptr;
               }
            }
            else if (type IS LUA_TNIL) ((OBJECTPTR *)(ArgBuffer + j))[0] = nullptr;
            else return fail_arg(n, "Object required.");
         }
         else if (Args[i].Type & FD_FUNCTION) {
            return fail_arg(n, "Function pointers are not supported (require embedding)");
         }
         else if (type IS LUA_TSTRING) {
            //log.trace("Arg: %s, Value: Pointer (Source is String)", Args[i].Name);
            auto string = strV(Lua->base + n - 1);
            if ((Args[i].Type & FD_MUTABLE) and (not lj_str_ismutable(string))) {
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

            if (auto native_struct = lua_isstruct(Lua, n) ? lua_tostruct(Lua, n) : nullptr) {
               // Guard specific to lifecycle-bound struct views; structs without an object dependency skip it.
               if (lj_struct_stale(native_struct)) return fail_arg(n, "Struct's providing object has been destroyed.");
               ((APTR *)(ArgBuffer + j))[0] = native_struct->data;
               //n--; // Adjustment required due to successful get_meta()
            }
            else ((APTR *)(ArgBuffer + j))[0] = lua_touserdata(Lua, n);
         }

         j += sizeof(APTR);
      }
      else if (Args[i].Type & FD_INT) {
         if (type IS LUA_TOBJECT) {
            if (auto obj = lj_lib_optobject(Lua, n, false)) {
               ((int *)(ArgBuffer + j))[0] = obj->uid;
            }
            else return fail_arg(n, "Failed to read object type.");
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
         else if ((type IS LUA_TUSERDATA) or (type IS LUA_TLIGHTUSERDATA)) {
            return fail_arg(n, "Unable to convert usertype to an integer.");
         }
         else if (type != LUA_TNIL) { // Attempt numeric conversion
            auto value = lua_tointeger(Lua, n);
            if ((Args[i].Type & FD_BUFSIZE) and buffer_capacity_known) {
               if (not check_buffer_size_arg(value, buffer_capacity)) {
                  return fail_arg(n, "Buffer size exceeds supplied buffer capacity.");
               }
               buffer_capacity_known = false;
            }
            ((int *)(ArgBuffer + j))[0] = value;
         }
         else if (Args[i].Type & (FD_BUFSIZE|FD_ARRAYSIZE)) {
            buffer_capacity_known = false; // An inferred array extent is no longer being validated.
         }
         else ((int *)(ArgBuffer + j))[0] = 0; // Value is nil
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
         if ((Args[i].Type & (FD_BUFSIZE|FD_ARRAYSIZE)) and (type IS LUA_TNIL)) {
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

   for (auto &pending : pending_functions) materialise_function_arg(Lua, pending, ArgBuffer);

   return ERR::Okay;
}

//********************************************************************************************************************
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
      if ((type & FDF_SPAN) IS FDF_SPAN) {
         size_t aligned_offset, end_offset;
         if (span_storage_bounds(size_t(of), size_t(std::numeric_limits<int>::max()), &aligned_offset,
             &end_offset) != ERR::Okay) {
            log.warning("Invalid embedded span layout for arg %s.", Args[i].Name);
            break;
         }
         of = int(end_offset);
      }
      else if (type & FD_ARRAY) { // Pointer to an array.
         of = ALIGN64(of);
         if (type & FD_CPP) {
            auto slot = (APTR *)(ArgBuf + of);
            APTR vector = slot[0];
            if (type & FD_RESULT) {
               if (vector) {
                  if (not push_cpp_array_arg(Lua, type, Args[i].Name, vector)) {
                     log.warning("Unsupported C++ array result arg %s, flags $%.8x", Args[i].Name, type);
                     lua_pushnil(Lua);
                  }
               }
               else lua_pushnil(Lua);
               total++;
            }

            delete_cpp_array_arg(type, vector);
            slot[0] = nullptr;
         }
         else if (type & FD_RESULT) {
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
      else if (type & FD_FUNCTION) {
         of = ALIGN64(of);
         if (type & FD_RESULT); // We don't process functions as results
         else release_consumed_func(Lua, (FUNCTION *)(ArgBuf + of));
         of += sizeof(FUNCTION);
      }
      else if (type & FD_PTR) {
         of = ALIGN64(of);
         if (type & FD_RESULT) {
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
