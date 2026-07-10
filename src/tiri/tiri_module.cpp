
#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_str.h"

#include "defs.h"
#include "lj_proto_registry.h"

#include <ffi.h>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <set>
#include <mutex>
#include <new>

struct module {
   const struct Function *Functions = nullptr;
   objModule *Module = nullptr;
   ankerl::unordered_dense::map<uint32_t, int> FunctionMap; // Hash map for O(1) function lookup

   ~module() {
      if (Module) FreeResource(Module);
   }
};

template<class... Args> void RMSG(Args...) {
   //log.msg(Args)
}

constexpr int MAX_MODULE_ARGS = 16;
constexpr size_t BUFFER_ELEMENT_SIZE = 16;
constexpr size_t BUFFER_SIZE = MAX_MODULE_ARGS * BUFFER_ELEMENT_SIZE;
constexpr size_t MAX_STRING_PREFIX_LENGTH = 200;
struct CaseInsensitiveCompare {
   using is_transparent = void;

   bool operator()(std::string_view A, std::string_view B) const {
      return std::lexicographical_compare(
         A.begin(), A.end(), B.begin(), B.end(),
         [](char CharA, char CharB) { return std::tolower((unsigned char)CharA) < std::tolower((unsigned char)CharB); }
      );
   }
};

static std::set<std::string, CaseInsensitiveCompare> glLoadedConstants; // Stores the names of modules that have loaded constants (system wide)

[[nodiscard]] static ERR load_include_struct(extTiri *, CSTRING, std::string_view, CSTRING *);
[[nodiscard]] static CSTRING load_include_constant(CSTRING, std::string_view);

static int module_call(lua_State *);
static ERR module_call_inner(lua_State *, std::string &, int &);
static int process_results(extTiri *, APTR, const FunctionField *);

//********************************************************************************************************************
// Support for mutable array results

struct cpp_array_result {
   APTR Data = nullptr;
   void (*Delete)(APTR) = nullptr;

   cpp_array_result() = default;
   cpp_array_result(const cpp_array_result &) = delete;
   cpp_array_result & operator=(const cpp_array_result &) = delete;

   cpp_array_result(cpp_array_result &&Other) noexcept:
      Data(Other.Data),
      Delete(Other.Delete)
   {
      Other.Data = nullptr;
      Other.Delete = nullptr;
   }

   cpp_array_result & operator=(cpp_array_result &&Other) noexcept
   {
      if (this != &Other) {
         if ((Data) and (Delete)) Delete(Data);
         Data = Other.Data;
         Delete = Other.Delete;
         Other.Data = nullptr;
         Other.Delete = nullptr;
      }
      return *this;
   }

   ~cpp_array_result()
   {
      if ((Data) and (Delete)) Delete(Data);
   }
};

template <class T> static void delete_cpp_array_result(APTR Result)
{
   delete (kt::vector<T> *)Result;
}

template <class T> static ERR make_cpp_array_result(cpp_array_result *Result)
{
   auto vector = new (std::nothrow) kt::vector<T>;
   if (not vector) return ERR::AllocMemory;

   Result->Data = vector;
   Result->Delete = delete_cpp_array_result<T>;
   return ERR::Okay;
}

// Create an empty kt::vector<T> suitable for passing to a module API function.

static ERR make_cpp_array_result(int Type, cpp_array_result *Result)
{
   if (Type & FD_STR) return make_cpp_array_result<std::string>(Result);
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) return make_cpp_array_result<OBJECTPTR>(Result);
   else if (Type & FD_PTR) return make_cpp_array_result<APTR>(Result);
   else if (Type & FD_DOUBLE) return make_cpp_array_result<double>(Result);
   else if (Type & FD_FLOAT) return make_cpp_array_result<float>(Result);
   else if (Type & FD_INT64) return make_cpp_array_result<int64_t>(Result);
   else if (Type & FD_INT) return make_cpp_array_result<int>(Result);
   else if (Type & FD_WORD) return make_cpp_array_result<int16_t>(Result);
   else if (Type & FD_BYTE) return make_cpp_array_result<uint8_t>(Result);
   else return ERR::NoSupport;
}

template <class T> static void push_cpp_array_result(lua_State *Lua, int Type, std::string_view Name, APTR Source)
{
   auto vector = (kt::vector<T> *)Source;
   make_any_array(Lua, Type, Name, int(vector->size()), vector->data());
}

static bool push_cpp_array_result(lua_State *Lua, int Type, std::string_view Name, APTR Source)
{
   if (Type & FD_STR) {
      auto vector = (kt::vector<std::string> *)Source;
      make_array(Lua, AET::STR_CPP, int(vector->size()), vector);
   }
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) push_cpp_array_result<OBJECTPTR>(Lua, Type, Name, Source);
   else if (Type & FD_PTR) push_cpp_array_result<APTR>(Lua, Type, Name, Source);
   else if (Type & FD_DOUBLE) push_cpp_array_result<double>(Lua, Type, Name, Source);
   else if (Type & FD_FLOAT) push_cpp_array_result<float>(Lua, Type, Name, Source);
   else if (Type & FD_INT64) push_cpp_array_result<int64_t>(Lua, Type, Name, Source);
   else if (Type & FD_INT) push_cpp_array_result<int>(Lua, Type, Name, Source);
   else if (Type & FD_WORD) push_cpp_array_result<int16_t>(Lua, Type, Name, Source);
   else if (Type & FD_BYTE) push_cpp_array_result<uint8_t>(Lua, Type, Name, Source);
   else return false;

   return true;
}

//********************************************************************************************************************

[[nodiscard]] static GCstr * string_arg(lua_State *Lua, int Arg) noexcept
{
   return strV(Lua->base + Arg - 1);
}

//********************************************************************************************************************

[[nodiscard]] static constexpr int8_t datatype(std::string_view String) noexcept
{
   auto it = std::ranges::find_if(String, [](char c) { return c > 0x20; });
   if (it IS String.end()) return 's';

   auto remaining = String.substr(it - String.begin());

   if (remaining.starts_with("0x")) {
      auto hex_digits = remaining.substr(2);
      if (hex_digits.empty()) return 's';
      bool all_hex = std::ranges::all_of(hex_digits, [](char c) { return std::isxdigit(c); });
      return all_hex ? 'h' : 's';
   }

   // Check for numeric content
   bool is_number = true;
   bool is_float  = false;

   for (char c : remaining) {
      if ((not std::isdigit(c)) and (c != '.') and (c != '-')) is_number = false;
      if (c IS '.') is_float = true;
   }

   if (is_float and is_number) return 'f';
   else if (is_number) return 'i';
   else return 's';
}

//********************************************************************************************************************
// Update the constant registry.
// A lock on glConstantMutex must be held before calling this function.

static CSTRING load_include_constant(CSTRING Line, std::string_view Source)
{
   kt::Log log("load_include");

   int i;
   for (i=0; (unsigned(Line[i]) > 0x20) and (Line[i] != ':'); i++);

   if (Line[i] != ':') {
      log.warning("Malformed const name in %.*s.", int(Source.size()), Source.data());
      return next_line(Line);
   }

   std::string name(Line, i);
   name.reserve(MAX_STRING_PREFIX_LENGTH);

   Line += i + 1;

   if (not name.empty()) name += '_';
   auto append_from = name.size();

   while (*Line > 0x20) {
      int n;
      for (n=0; (Line[n] > 0x20) and (Line[n] != '='); n++);

      if (Line[n] != '=') {
         log.warning("Malformed const definition, expected '=' after name '%s'", name.c_str());
         break;
      }

      name.erase(append_from);
      name.append(Line, n);
      Line += n + 1;

      for (n=0; (Line[n] > 0x20) and (Line[n] != ','); n++);
      std::string value(Line, n);
      Line += n;

      //log.warning("%s = %s", name.c_str(), value.c_str());

      if (n > 0) {
         auto dt = datatype(value);
         TiriConstant constant(int64_t(0));

         if (dt IS 'i') constant = TiriConstant(int64_t(strtoll(value.c_str(), nullptr, 0)));
         else if (dt IS 'f') constant = TiriConstant(strtod(value.c_str(), nullptr));
         else if (dt IS 'h') constant = TiriConstant(int64_t(strtoull(value.c_str(), nullptr, 0)));
         else log.warning("Unsupported constant value: %s", value.c_str());

         glConstantRegistry.emplace(kt::strhash(name), constant);
      }

      if (*Line IS ',') Line++;
   }

   return next_line(Line);
}

//********************************************************************************************************************

static ERR process_module_defs(extTiri *Script, objModule *module, std::string_view Name)
{
   if (auto root = (OBJECTPTR)module->Root) {
      struct ModHeader *header;
      if (auto error = root->get(strhash("header"), header); error != ERR::Okay) return error;
      if (not header) return ERR::NoData;

      if (auto idl = header->Definitions) {
         while ((idl) and (*idl)) {
            if ((idl[0] IS 's') and (idl[1] IS '.')) {
               if (auto error = load_include_struct(Script, idl+2, Name, &idl); error != ERR::Okay) return error;
            }
            else if ((idl[0] IS 'c') and (idl[1] IS '.')) idl = load_include_constant(idl+2, Name);
            else idl = next_line(idl);
         }
      }
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

//********************************************************************************************************************
// For the 'include' statement.  Creates a temporary module object to process the definitions without formally opening
// an interface.

[[nodiscard]] ERR load_include(extTiri *Script, std::string_view Module)
{
   ERR error = ERR::Okay;

   std::unique_lock lock(glConstantMutex); // Required to update the constant registry

   // Constants are system-wide, so process only once per session.

   bool process_constants = false;
   if (not glLoadedConstants.contains(Module)) {
      process_constants = true;
   }

   if (process_constants) {
      kt::Log log(__FUNCTION__);
      log.branch("Definition: %.*s", int(Module.size()), Module.data());

      AdjustLogLevel(1);

         objModule::create module = { fl::Name(Module) };
         if (module.ok()) error = process_module_defs(Script, *module, Module);
         else error = ERR::CreateObject;

      AdjustLogLevel(-1);

      if (!error) glLoadedConstants.emplace(Module);
   }

   return error;
}

//********************************************************************************************************************
// Format: s.Name:typeField,...
// TODO: This parses the struct definitions in advance - ideally we'd record the definition string and parse on first-use.

[[nodiscard]] static ERR load_include_struct(extTiri *Script, CSTRING Line, std::string_view Source, CSTRING *NextLine)
{
   if (not NextLine) return ERR::NullArgs;
   *NextLine = next_line(Line);

   int i;
   for (i=0; (Line[i] >= 0x20) and (Line[i] != ':'); i++);

   if (Line[i] IS ':') {
      std::string name(Line, i);
      Line += i + 1;

      int j;
      for (j=0; (Line[j] != '\n') and (Line[j] != '\r') and (Line[j]); j++);

      if ((Line[j] IS '\n') or (Line[j] IS '\r')) {
         std::string linebuf(Line, j);
         while ((Line[j] IS '\n') or (Line[j] IS '\r')) j++;
         *NextLine = Line + j;
         return make_struct(Script, name, linebuf.c_str());
      }
      else {
         *NextLine = Line + j;
         return make_struct(Script, name, Line);
      }
   }
   else {
      kt::Log(__FUNCTION__).warning("Malformed struct name in %.*s.", int(Source.size()), Source.data());
      return ERR::Syntax;
   }
}

//********************************************************************************************************************
// Configure a Tiri module object (post-loading).

void new_module(lua_State *Lua, objModule *Module)
{
   auto mod = (module *)lua_newuserdata(Lua, sizeof(module));
   new (mod) module;

   luaL_getmetatable(Lua, "Tiri.mod");
   lua_setmetatable(Lua, -2);

   mod->Module = Module;
   Module->getFunctionList(mod->Functions);

   // Build hash map for O(1) function lookups
   if (mod->Functions) {
      for (int i = 0; mod->Functions[i].Name; i++) {
         auto hash = strihash(mod->Functions[i].Name);
         mod->FunctionMap[hash] = i;
      }
   }
}

//********************************************************************************************************************
// Usage: passed, total, = mod.test(module, Options)
// Runs the module's unit tests, if any

static int module_test(lua_State *Lua)
{
   if (auto mod = (module *)luaL_checkudata(Lua, 1, "Tiri.mod")) {
      auto options = lua_tostringview(Lua, 2);
      int passed = 0, total = 0;
      if (((objModule *)mod->Module)->test(options, &passed, &total) IS ERR::Okay) {
         lua_pushinteger(Lua, passed);
         lua_pushinteger(Lua, total);
         return 2;
      }
      else luaL_error(Lua, ERR::Failed, "Module test failed.");
   }
   else luaL_argerror(Lua, 1, "Expected module.");
   return 0;
}

//********************************************************************************************************************
// Usage: module = mod.load('core')

static int module_load(lua_State *Lua)
{
   auto modname = luaL_checkstring(Lua, 1);
   if (not modname) luaL_argerror(Lua, 1, "String expected for module name.");

   kt::Log log(__FUNCTION__);
   log.branch("Module: %s", modname);

   int i;
   for (i=0; modname[i]; i++) {
      if ((modname[i] >= 'a') and (modname[i] <= 'z')) continue;
      if ((modname[i] >= 'A') and (modname[i] <= 'Z')) continue;
      if ((modname[i] >= '0') and (modname[i] <= '9')) continue;
      break;
   }

   if ((modname[i]) or (i >= 32)) {
      luaL_error(Lua, ERR::Syntax,
         "Invalid module name; only alpha-numeric names shorter than 32 characters are permitted.");
   }

   if (auto loaded_mod = objModule::create::global(fl::Name(modname))) {
      ERR defs_error = ERR::Okay;
      {
         std::unique_lock lock(glConstantMutex); // Required to update the constant registry

         bool process_constants = false;
         if (not glLoadedConstants.contains(modname)) {
            process_constants = true;
         }

         if (process_constants) {
            if (!(defs_error = process_module_defs(Lua->script, loaded_mod, modname))) {
               glLoadedConstants.insert(modname);
            }
         }
      }

      if (defs_error != ERR::Okay) {
         FreeResource(loaded_mod);
         luaL_error(Lua, defs_error, "Failed to process definitions for the %s module.", modname);
      }

      new_module(Lua, loaded_mod);
      return 1;  // new userdatum is already on the stack
   }
   else {
      log.debranch();
      luaL_error(Lua, ERR::LoadModule, "Failed to load the %s module.", modname);
   }
   return 0;
}

//********************************************************************************************************************
// Object garbage collector.

static int module_destruct(lua_State *Lua)
{
   if (auto mod = (module *)luaL_checkudata(Lua, 1, "Tiri.mod")) {
      mod->~module();
   }

   return 0;
}

//********************************************************************************************************************
// Prints the module name

static int module_tostring(lua_State *Lua)
{
   if (auto mod = (struct module *)luaL_checkudata(Lua, 1, "Tiri.mod")) {
      std::string_view name;
      if (!mod->Module->getName(name)) {
         lua_pushstring(Lua, name);
      }
      else lua_pushnil(Lua);
   }
   else lua_pushnil(Lua);

   return 1;
}

//********************************************************************************************************************
// Any Read accesses to the module object will pass through here.

static int module_index(lua_State *Lua)
{
   if (auto mod = (module *)luaL_checkudata(Lua, 1, "Tiri.mod")) {
      if (auto function = luaL_checkstring(Lua, 2)) {
         if (mod->Functions) {
            auto hash = strihash(function); // Case insensitive function calls
            if (auto it = mod->FunctionMap.find(hash); it != mod->FunctionMap.end()) {
               lua_pushvalue(Lua, 1); // Arg1: Duplicate the module reference
               lua_pushinteger(Lua, it->second); // Arg2: Index of the function that is being called
               lua_pushcclosure(Lua, module_call, 2);
               return 1;
            }

            luaL_error(Lua, ERR::UnknownProperty, "Call to function %s() not recognised.", function);
         }
         else luaL_error(Lua, ERR::UnknownProperty, "No exported function list for this module.", function);
      }
      else luaL_argerror(Lua, 2, "Expected function string.");
   }
   else luaL_argerror(Lua, 1, "Expected module.");

   return 0;
}

//********************************************************************************************************************
// Makes a call to a Kotuku module function.  module_call() sets the benchmark for processing FD parameters.
//
// A breakdown of possible FD configurations follows.  Input values:
//
// STR          = CSTRING.  Read-only string pointer.  Tiri string/number/boolean are accepted; NULL permitted
// STR|CPP      = const std::string_view &.  Read-only string view.  A real string value is required.
// PTR          = APTR.  Raw pointer input.  Tiri strings, arrays, structs, objects and userdata can provide
//                backing storage depending on the call.
// PTR|STRUCT   = Struct *.  Accepts an existing Tiri struct or converts a Tiri table to a temporary struct.
// OBJECT|PTR   = OBJECTPTR.  Accepts a Tiri object and resolves it to an object pointer.
// ARRAY        = Type *.  Accepts a Tiri array and passes its data pointer.  ARRAYSIZE can follow.
// ARRAY|BUFFER = Type *.  Accepts a Tiri array and treats ARRAYSIZE/BUFSIZE as byte capacity.
// ARRAY|CPP    = Unsupported as input.  kt::vector<> input marshalling is not implemented for module calls.
// FUNCTION     = FUNCTION *.  Accepts a Tiri function or global function name.
//
// Mutable combinations.  User is expected to supply a reference to a suitable storage area, as defined by the type
// The storage area may contain existing data for consumption prior to mutation.  Failure to provide storage would be
// considered an error.  Often used for reading data into buffers, like the Read() action.
//
// PTR|MUTABLE     = APTR.  Mutable raw pointer/buffer input.  Tiri strings must be mutable.
// STR|MUTABLE     = STRING.  A mutable Tiri string buffer is required, normally from string.alloc().
//                   The function mutates the buffer in-place.  A BUFSIZE is expected for the next parameter.
// STR|MUTABLE|CPP = std::string *.  A mutable Tiri string buffer is required.  Tiri content is copied into
//                   a temporary std::string before the call, then copied back after the call.  If the final
//                   string is larger than the Tiri buffer, the call fails.
//
// Result combintations.  The function will return a reference to its own storage area, or allocate a new one.  Result
// values will be returned as true Tiri results, unlike mutable types which are manipulated in-place.
//
// RESULT|STR        = CSTRING *.  Function writes a non-allocated C string pointer; returned to Tiri as a string.
// RESULT|STR|ALLOC  = CSTRING *.  Function writes an allocated C string pointer; returned to Tiri then freed.
// RESULT|STR|CPP    = std::string *.  Tiri provides a temporary std::string; returned to Tiri as a string.
//
// RESULT|PTR        = APTR *.  Function writes a pointer; returned to Tiri as light userdata unless specialised.
// RESULT|PTR|ALLOC  = APTR *.  Function writes an allocated block; returned as a binary string when BUFSIZE is
//                     available, then freed.
// RESULT|PTR|STRUCT = Struct **.  Function writes a struct pointer; returned as a Tiri table, or as a managed
//                     struct when RESOURCE is set.
// RESULT|ARRAY      = Type **.  Function writes an array pointer; returned to Tiri as an array.  ARRAYSIZE may
//                     follow to define element count.
// RESULT|ARRAY|CPP  = kt::vector<> *.  Tiri provides an empty kt::vector<>; returned to Tiri as an array.
// RESULT|BUFFER     = APTR.  Caller supplies a mutable Tiri buffer or array for the function to fill; BUFSIZE
//                     must follow.
// RESULT|BUFFER|CPP = Unsupported.  kt::vector<> output marshalling is not implemented for module calls.
//
// Mixed direction:
//
// MUTABLE|RESULT|CPP = An empty C++ buffer is supplied as input (e.g. std::string or kt::vector<>) and it will be
//                      used to store the result.

static int module_call(lua_State *Lua)
{
   std::string error_msg;
   int results = 0;

   auto err = module_call_inner(Lua, error_msg, results);
   if (err != ERR::Okay) {
      if (error_msg.empty()) error_msg = GetErrorMsg(err);
      luaL_error(Lua, err, std::move(error_msg));
   }
   return results;
}

static ERR module_call_inner(lua_State *Lua, std::string &ErrorMsg, int &Results)
{
   kt::Log log("module_call");

   uint8_t buffer[MAX_MODULE_ARGS * 16]; // 16 bytes seems overkill but some parameters output meta information (e.g. size).
   int i;

   // Track dynamically allocated objects for cleanup
   struct allocated_struct_ref {
      std::unique_ptr<uint8_t[]> Data;
      struct_record *Def = nullptr;

      allocated_struct_ref() = default;
      allocated_struct_ref(std::unique_ptr<uint8_t[]> InitData, struct_record *InitDef):
         Data(std::move(InitData)), Def(InitDef) { }

      allocated_struct_ref(const allocated_struct_ref &) = delete;
      allocated_struct_ref & operator=(const allocated_struct_ref &) = delete;

      allocated_struct_ref(allocated_struct_ref &&Other) noexcept: Data(std::move(Other.Data)), Def(Other.Def) {
         Other.Def = nullptr;
      }

      allocated_struct_ref & operator=(allocated_struct_ref &&Other) noexcept {
         if (this != &Other) {
            release();
            Data = std::move(Other.Data);
            Def = Other.Def;
            Other.Def = nullptr;
         }
         return *this;
      }

      ~allocated_struct_ref() {
         release();
      }

      void release() {
         if (Data) {
            if (Def) destroy_struct_cpp_strings(*Def, Data.get());
            Data.reset();
            Def = nullptr;
         }
      }
   };

   struct mutable_cpp_string_ref {
      std::string *Source;
      GCstr *Target;
      int Arg;
   };

   std::vector<std::string> strings;
   std::vector<std::string_view> string_views;
   std::vector<allocated_struct_ref> allocated_structs;
   std::vector<mutable_cpp_string_ref> mutable_cpp_strings;
   std::vector<cpp_array_result> cpp_arrays;
   strings.reserve(8); // Keep the collection stable
   string_views.reserve(8);

   auto copy_mutable_cpp_strings = [&]() {
      for (auto &entry : mutable_cpp_strings) {
         auto capacity = size_t(entry.Target->len);
         auto length = entry.Source->size();
         if (length > capacity) ErrorMsg = "Mutable buffer too small.";
         else {
            auto target = strdatawr(entry.Target);
            if (length) copymem(entry.Source->data(), target, length);
            if (length < capacity) clearmem(target + length, capacity - length);
         }
      }
   };

   auto tiri = Lua->script;
   if (not tiri) return ERR::ObjectCorrupt;

   auto value_string = [](lua_State *Lua, int Index) -> CSTRING {
      if (auto string = lua_tostring(Lua, Index)) return string;
      else return "";
   };

   auto mod = (module *)get_meta(Lua, lua_upvalueindex(1), "Tiri.mod");
   if (not mod) {
      ErrorMsg = "module_call() expected module in upvalue.";
      return ERR::Args;
   }

   if (not mod->Functions) return ERR::UnknownProperty;

   auto index = lua_tointeger(Lua, lua_upvalueindex(2));
   int nargs = lua_gettop(Lua);
   if (nargs > MAX_MODULE_ARGS-1) {
      log.warning("Limit of %d args exceeded.", MAX_MODULE_ARGS - 1);
      nargs = MAX_MODULE_ARGS-1;
   }

   uint8_t *end = buffer + sizeof(buffer);

   log.trace("%s() Index: %d, Args: %d", mod->Functions[index].Name, index, nargs);

   const FunctionField *args;
   if (not (args = mod->Functions[index].Args)) {
      auto function = (void (*)(void))mod->Functions[index].Address;
      function();
      return ERR::Okay;
   }

   APTR function = mod->Functions[index].Address;
   FUNCTION func;

   // This guard owns the Lua registry reference for an FD_FUNCTION argument until the call is handed to the module.
   // After that point, the module function is responsible for calling DerefProcedure() when it no longer needs it,
   // or alternatively marking the function as consumed.

   struct func_ref_guard {
      lua_State *Lua;
      FUNCTION  &Func;
      bool OwnsReference = true;
      ~func_ref_guard() {
         if (Func.isScript() and (Func.ProcedureID > 0)) {
            if (OwnsReference or Func.consumed()) luaL_unref(Lua, LUA_REGISTRYINDEX, Func.ProcedureID);
         }
      }
   } func_guard{ Lua, func };

   ffi_cif cif;
   union {
      ffi_arg Arg;
      double  Double;
      int64_t Int64;
   } rc = { };
   ffi_type *arg_types[MAX_MODULE_ARGS];
   void * arg_values[MAX_MODULE_ARGS];
   int in = 0;

   int j = 0;

   for (i=1; args[i].Name; i++) {
      int argtype = args[i].Type;

      //log.trace("%s() Arg: %s, Offset: %d, Type: $%.8x (received %s)", mod->Functions[index].Name, args[i].Name, j, argtype, lua_typename(Lua, lua_type(Lua, i)));

      if (argtype & FD_RESULT) {
         // Result arguments are stored in the buffer with a pointer to an empty variable space (stored at the end of
         // the buffer)

         RMSG("Result for arg %d stored at %p", i, end);

         if ((argtype & FD_CPP) and (argtype & FD_ARRAY) and (argtype & FD_MUTABLE)) {
            // Applicable to RESULT|MUTABLE only, which requires the client to provide an empty kt::vector<> to the function.
            // We set this up here, then convert the resulting values to a Tiri array when the function returns.
            cpp_array_result result_ref = { };
            if ((argtype & FD_STRUCT) and (!(argtype & FD_PTR))) {
               // TODO: args[i].Name will include the name of the struct that we need to use to build the array, e.g. "FontList:Result"
               // where FontList is the struct name.  The kt::vector<T> data() will contain a serialised list of structs and the total
               // count.  For this to work, the STRUCT type would need to host a kt::vector<> and the call site would need to
               // make a TrackResource() call that associates its destruction process with the kt::vector<> address.
               ErrorMsg = "C++ struct arrays are not supported.";
               return ERR::NoSupport;
            }
            else if (auto error = make_cpp_array_result(argtype, &result_ref); !error) {
               ((APTR *)(buffer + j))[0] = result_ref.Data; // kt::vector<>
               cpp_arrays.push_back(std::move(result_ref));
               arg_values[in]  = buffer + j;
               arg_types[in++] = &ffi_type_pointer;
               j += sizeof(APTR);
            }
            else {
               ErrorMsg = std::format("Function '{}' uses an unsupported C++ array result type.", mod->Functions[index].Name);
               return error;
            }
         }
         else if (argtype & FD_BUFFER) { // For fixed-size buffers; the next parameter must indicate the size.
            // The client must supply an argument that will store a buffer result.  This is a different case to the
            // storage of type values.  Buffers can be combined with FD_ARRAY to store more than one element.

            if (argtype & FD_CPP) {
               ErrorMsg = "No support for calls utilising C++ arrays.";
               return ERR::NoSupport;
            }

            if (lua_type(Lua, i) IS LUA_TARRAY) {
               GCarray *arr = arrayV(Lua, i);
               ((APTR *)(buffer + j))[0] = arr->arraydata();
               arg_values[in] = buffer + j;
               arg_types[in++] = &ffi_type_pointer;
               j += sizeof(APTR);

               if (args[i+1].Type & (FD_BUFSIZE|FD_ARRAYSIZE)) {
                  if (args[i+1].Type & FD_INT) {
                     ((int *)(buffer + j))[0] = arr->len * arr->elemsize;
                     arg_values[in]  = buffer + j;
                     arg_types[in++] = &ffi_type_sint32;
                     i++;
                     j += sizeof(int);
                  }
                  else if (args[i+1].Type & FD_INT64) {
                     ((int64_t *)(buffer + j))[0] = arr->len * arr->elemsize;
                     arg_values[in]  = buffer + j;
                     arg_types[in++] = &ffi_type_sint64;
                     i++;
                     j += sizeof(int64_t);
                  }
                  else log.warning("Integer type unspecified for BUFSIZE argument in %s()", mod->Functions[index].Name);
               }
               else {
                  ErrorMsg = std::format("Function '{}' is not compatible with Tiri.", mod->Functions[index].Name);
                  return ERR::InvalidType;
               }
            }
            else if (lua_type(Lua, i) IS LUA_TSTRING) {
               GCstr *string = string_arg(Lua, i);
               if (not lj_str_ismutable(string)) {
                  ErrorMsg = std::format("Arg #{} requires a mutable buffer.", i);
                  return ERR::InvalidType;
               }

               ((APTR *)(buffer + j))[0] = strdatawr(string);
               arg_values[in] = buffer + j;
               arg_types[in++] = &ffi_type_pointer;
               j += sizeof(APTR);

               if (args[i+1].Type & (FD_BUFSIZE|FD_ARRAYSIZE)) {
                  if (args[i+1].Type & FD_INT) {
                     ((int *)(buffer + j))[0] = int(string->len);
                     arg_values[in]  = buffer + j;
                     arg_types[in++] = &ffi_type_sint32;
                     i++;
                     j += sizeof(int);
                  }
                  else if (args[i+1].Type & FD_INT64) {
                     ((int64_t *)(buffer + j))[0] = string->len;
                     arg_values[in]  = buffer + j;
                     arg_types[in++] = &ffi_type_sint64;
                     i++;
                     j += sizeof(int64_t);
                  }
                  else log.warning("Integer type unspecified for BUFSIZE argument in %s()", mod->Functions[index].Name);
               }
               else {
                  ErrorMsg = std::format("Function '{}' is not compatible with Tiri.", mod->Functions[index].Name);
                  return ERR::InvalidType;
               }
            }
            else {
               ErrorMsg = std::format("A memory buffer is required in arg #{}.", i);
               return ERR::Args;
            }
         }
         else if (argtype & FD_STR) { // FD_RESULT
            if (argtype & FD_CPP) {
               if (argtype & FD_MUTABLE) {
                  // Use of MUTABLE enables support for std::string buffering.  We provide our own std::string that will be used as
                  // the buffer, it gets converted to a Tiri string when the function returns.

                  strings.emplace_back();
                  ((std::string **)(buffer + j))[0] = &strings.back();
                  arg_values[in]  = buffer + j;
                  arg_types[in++] = &ffi_type_pointer;
                  j += sizeof(APTR);
               }
               else {
                  // Non-mutable string reference, supported as a std::string_view
                  string_views.emplace_back();
                  ((std::string_view **)(buffer + j))[0] = &string_views.back();
                  arg_values[in]  = buffer + j;
                  arg_types[in++] = &ffi_type_pointer;
                  j += sizeof(APTR);
               }
            }
            else { // C-style string reference, this parameter style is being deprecated.
               end -= sizeof(APTR);
               ((APTR *)(buffer + j))[0] = end;
               ((APTR *)end)[0] = nullptr;
               arg_values[in]  = buffer + j;
               arg_types[in++] = &ffi_type_pointer;
               j += sizeof(APTR);
            }
         }
         else if (argtype & (FD_PTR|FD_ARRAY)) { // FD_RESULT
            end -= sizeof(APTR);
            ((APTR *)(buffer + j))[0] = end;
            ((APTR *)end)[0] = nullptr;
            arg_values[in]  = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR);
         }
         else if (argtype & FD_INT) { // FD_RESULT
            end -= sizeof(int);
            ((APTR *)(buffer + j))[0] = end;
            ((int *)end)[0] = 0;
            arg_values[in]  = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR);
         }
         else if (argtype & (FD_DOUBLE|FD_INT64)) { // FD_RESULT
            end -= sizeof(int64_t);
            ((APTR *)(buffer + j))[0] = end;
            ((int64_t *)end)[0] = 0;
            arg_values[in]  = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR);
         }
         else {
            ErrorMsg = std::format("Unrecognised arg {} type {}", i, argtype);
            return ERR::InvalidType;
         }
      }
      else if (argtype & FD_FUNCTION) {
         if (func.defined()) { // Is the function reserve already used?
            ErrorMsg = "Multiple function arguments are not supported.";
            return ERR::Args;
         }

         // NOTE: The client is responsible for calling DerefProcedure() on the function reference when it is no longer needed,
         // or it can mark the function as consumed.

         switch(lua_type(Lua, i)) {
            case LUA_TSTRING: { // Name of function to call
               lua_getglobal(Lua, lua_tostringview(Lua, i));
               func = FUNCTION(Lua->script, luaL_ref(Lua, LUA_REGISTRYINDEX));
               ((FUNCTION **)(buffer + j))[0] = &func;
               break;
            }

            case LUA_TFUNCTION: { // Direct function reference
               lua_pushvalue(Lua, i);
               func = FUNCTION(Lua->script, luaL_ref(Lua, LUA_REGISTRYINDEX));
               ((FUNCTION **)(buffer + j))[0] = &func;
               break;
            }

            case LUA_TNIL:
            case LUA_TNONE:
               ((FUNCTION **)(buffer + j))[0] = nullptr;
               break;

            default:
               ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected function, got {} '{}'.", i, args[i].Name,
                  lua_typename(Lua, lua_type(Lua, i)), value_string(Lua, i));
               return ERR::InvalidType;
         }

         arg_values[in]  = buffer + j;
         arg_types[in++] = &ffi_type_pointer;
         j += sizeof(FUNCTION *);
      }
      else if (argtype & FD_STR) {
         auto type = lua_type(Lua, i);
         int type_size = sizeof(APTR);

         if (argtype & FD_MUTABLE) {
            // The client is expected to provide a mutable string with preallocated size, which can be acquired from string.alloc()
            if (type != LUA_TSTRING) {
               ErrorMsg = std::format("Arg #{} requires a mutable buffer.", i);
               return ERR::InvalidType;
            }

            GCstr *string = string_arg(Lua, i);
            if (not lj_str_ismutable(string)) {
               ErrorMsg = std::format("Arg #{} requires a mutable buffer.", i);
               return ERR::InvalidType;
            }

            if (argtype & FD_CPP) { // Function expects a pointer to std::string that it can mutate
               size_t len;
               if (auto str = lua_tolstring(Lua, i, &len)) strings.emplace_back(str, len);
               else strings.emplace_back();

               auto cppstr = &strings.back();
               mutable_cpp_strings.push_back({ cppstr, string, i });
               ((std::string **)(buffer + j))[0] = cppstr;
            }
            else ((CSTRING *)(buffer + j))[0] = strdatawr(string);
         }
         else if (argtype & FD_CPP) { // Read-only std::string_view (enforced, cannot be nullptr)
            size_t len;
            if (auto str = lua_tolstring(Lua, i, &len)) string_views.emplace_back(str, len);
            else string_views.emplace_back();
            ((std::string_view **)(buffer + j))[0] = &string_views.back();
         }
         else if ((type IS LUA_TSTRING) or (type IS LUA_TNUMBER) or (type IS LUA_TBOOLEAN)) {
            ((CSTRING *)(buffer + j))[0] = lua_tostring(Lua, i);
         }
         else if (type <= 0) {
            ((CSTRING *)(buffer + j))[0] = nullptr;
         }
         else if ((type IS LUA_TUSERDATA) or (type IS LUA_TLIGHTUSERDATA)) {
            ErrorMsg = std::format("Arg #{} ({}) requires a string and not untyped pointer.", i, args[i].Name);
            return ERR::InvalidType;
         }
         else {
            ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected string, got {} '{}'.", i, args[i].Name,
               lua_typename(Lua, lua_type(Lua, i)), value_string(Lua, i));
            return ERR::InvalidType;
         }

         arg_values[in] = buffer + j;
         arg_types[in++] = &ffi_type_pointer;
         j += type_size;
      }
      else if (argtype & FD_ARRAY) { // Pass array data pointer
         if (argtype & FD_CPP) {
            ErrorMsg = "No support for calls utilising C++ arrays.";
            return ERR::NoSupport;
         }

         if (lua_type(Lua, i) IS LUA_TARRAY) {
            GCarray *arr = arrayV(Lua, i);
            ((APTR *)(buffer + j))[0] = arr->arraydata();
            arg_values[in] = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR);

            if (args[i+1].Type & (FD_BUFSIZE|FD_ARRAYSIZE)) {
               if (args[i+1].Type & FD_RESULT) {
                  if (args[i+1].Type & FD_INT) {
                     end -= sizeof(int);
                     ((int *)end)[0] = arr->len;
                     ((APTR *)(buffer + j))[0] = end;
                     arg_values[in] = buffer + j;
                     arg_types[in++] = &ffi_type_pointer;
                     j += sizeof(APTR);
                     i++;
                  }
                  else if (args[i+1].Type & FD_INT64) {
                     end -= sizeof(int64_t);
                     ((int64_t *)end)[0] = arr->len;
                     ((APTR *)(buffer + j))[0] = end;
                     arg_values[in] = buffer + j;
                     arg_types[in++] = &ffi_type_pointer;
                     j += sizeof(APTR);
                     i++;
                  }
                  else {
                     ErrorMsg = std::format("Function '{}' is not compatible with Tiri.", mod->Functions[index].Name);
                     return ERR::NoSupport;
                  }
               }
               else {
                  if (args[i+1].Type & FD_INT) {
                     ((int *)(buffer + j))[0] = (argtype & FD_BUFFER) ? arr->len * arr->elemsize : arr->len;
                     arg_values[in] = buffer + j;
                     arg_types[in++] = &ffi_type_sint32;
                     j += sizeof(int);
                     i++;
                  }
                  else if (args[i+1].Type & FD_INT64) {
                     ((int64_t *)(buffer + j))[0] = (argtype & FD_BUFFER) ? arr->len * arr->elemsize : arr->len;
                     arg_values[in] = buffer + j;
                     arg_types[in++] = &ffi_type_sint64;
                     j += sizeof(int64_t);
                     i++;
                  }
                  else {
                     ErrorMsg = std::format("Function '{}' is not compatible with Tiri.", mod->Functions[index].Name);
                     return ERR::NoSupport;
                  }
               }
            }
            else {
               ErrorMsg = std::format("Function '{}' is not compatible with Tiri.", mod->Functions[index].Name);
               return ERR::NoSupport;
            }
         }
         else {
            ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected array, got '{}'.", i, args[i].Name,
               lua_typename(Lua, lua_type(Lua, i)));
            return ERR::InvalidType;
         }
      }
      else if (argtype & FD_PTR) {
         auto arg_type = lua_type(Lua, i);
         if (arg_type IS LUA_TSTRING) {
            // Lua strings need to be converted to C strings
            auto string = string_arg(Lua, i);
            if ((argtype & FD_MUTABLE) and (not lj_str_ismutable(string))) {
               ErrorMsg = std::format("Arg #{} requires a mutable buffer.", i);
               return ERR::InvalidType;
            }

            size_t strlen = string->len;
            ((CSTRING *)(buffer + j))[0] = (argtype & FD_MUTABLE) ? strdatawr(string) : strdata(string);
            arg_values[in] = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(CSTRING);

            if (args[i+1].Type & FD_BUFSIZE) {
               if (args[i+1].Type & FD_INT) {
                  ((int *)(buffer + j))[0] = strlen;
                  i++;
                  arg_values[in] = buffer + j;
                  arg_types[in++] = &ffi_type_sint32;
                  j += sizeof(int);
               }
               else if (args[i+1].Type & FD_INT64) {
                  ((int64_t *)(buffer + j))[0] = strlen;
                  i++;
                  arg_values[in] = buffer + j;
                  arg_types[in++] = &ffi_type_sint64;
                  j += sizeof(int64_t);
               }
            }
         }
         else if (auto fstruct = (struct fstruct *)get_meta(Lua, i, "Tiri.struct")) {
            ((APTR *)(buffer + j))[0] = fstruct->Data;
            arg_values[in] = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR);

            log.trace("Struct address %p inserted to arg offset %d", fstruct->Data, j);
            if (args[i+1].Type & FD_BUFSIZE) {
               if (args[i+1].Type & FD_INT) {
                  ((int *)(buffer + j))[0] = fstruct->AlignedSize;
                  i++;
                  arg_values[in] = buffer + j;
                  arg_types[in++] = &ffi_type_sint32;
                  j += sizeof(int);
               }
               else if (args[i+1].Type & FD_INT64) {
                  ((int64_t *)(buffer + j))[0] = fstruct->AlignedSize;
                  i++;
                  arg_values[in] = buffer + j;
                  arg_types[in++] = &ffi_type_sint64;
                  j += sizeof(int64_t);
               }
            }
         }
         else if (arg_type IS LUA_TOBJECT) {
            auto obj = lua_toobject(Lua, i);
            if (obj->ptr) {
               ((OBJECTPTR *)(buffer + j))[0] = obj->ptr;
            }
            else if (auto ptr_obj = (OBJECTPTR)access_object(obj)) {
               ((OBJECTPTR *)(buffer + j))[0] = ptr_obj;
               release_object(obj);
            }
            else {
               log.warning("Unable to resolve object pointer for #%d.", obj->uid);
               ((OBJECTPTR *)(buffer + j))[0] = nullptr;
            }

            arg_values[in] = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR);
         }
         else if (arg_type IS LUA_TARRAY) {
            GCarray *array = lua_toarray(Lua, i);

            ((APTR *)(buffer + j))[0] = array->arraydata();
            arg_values[in] = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR);

            if (args[i+1].Type & FD_BUFSIZE) {
               if (args[i+1].Type & FD_INT) {
                  ((int *)(buffer + j))[0] = array->len * array->elemsize;
                  i++;
                  arg_values[in] = buffer + j;
                  arg_types[in++] = &ffi_type_sint32;
                  j += sizeof(int);
               }
               else if (args[i+1].Type & FD_INT64) {
                  ((int64_t *)(buffer + j))[0] = array->len * array->elemsize;
                  i++;
                  arg_values[in] = buffer + j;
                  arg_types[in++] = &ffi_type_sint64;
                  j += sizeof(int64_t);
               }
            }
         }
         else if (arg_type IS LUA_TTABLE) {
            if (args[i].Type & FD_STRUCT) {
               // Convert Lua table to C struct
               lua_pushvalue(Lua, i); // Duplicate table for table_to_struct (consumes stack)
               std::unique_ptr<uint8_t[]> struct_data;
               if (!table_to_struct(Lua, args[i].Name, struct_data)) {
                  auto struct_address = struct_data.get();
                  auto def = glStructs.find(std::string_view(args[i].Name));
                  if (def != glStructs.end()) allocated_structs.emplace_back(std::move(struct_data), &def->second);
                  else allocated_structs.emplace_back(std::move(struct_data), nullptr);
                  ((APTR *)(buffer + j))[0] = struct_address;
                  arg_values[in] = buffer + j;
                  arg_types[in++] = &ffi_type_pointer;
                  j += sizeof(APTR);
               }
               else {
                  ErrorMsg = std::format("Failed to convert table to struct for arg #{} ({}).", i, args[i].Name);
                  return ERR::Failed;
               }
            }
            else {
               ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected pointer, got table.", i, args[i].Name);
               return ERR::InvalidType;
            }
         }
         else {
            ((APTR *)(buffer + j))[0] = lua_touserdata(Lua, i); //lua_topointer?
            arg_values[in] = buffer + j;
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR);
         }
      }
      else if (argtype & FD_INT) {
         auto tv = resolve_index(Lua, i);

         if (argtype & FD_OBJECT) {
            if (tvisobject(tv)) ((int *)(buffer + j))[0] = objectV(tv)->uid;
            else ((int *)(buffer + j))[0] = lua_tointeger(Lua, i);
         }
         else if (argtype & FD_UNSIGNED) ((uint32_t *)(buffer + j))[0] = lua_tointeger(Lua, i);
         else ((int *)(buffer + j))[0] = lua_tointeger(Lua, i);
         arg_values[in] = buffer + j;
         arg_types[in++] = &ffi_type_sint32;
         j += sizeof(int);
      }
      else if (argtype & FD_DOUBLE) {
         ((double *)(buffer + j))[0] = lua_tonumber(Lua, i);
         arg_values[in] = buffer + j;
         arg_types[in++] = &ffi_type_double;
         j += sizeof(double);
      }
      else if (argtype & FD_INT64) {
         ((int64_t *)(buffer + j))[0] = lua_tointeger(Lua, i);
         arg_values[in] = buffer + j;
         arg_types[in++] = &ffi_type_sint64;
         j += sizeof(int64_t);
      }
      else if (argtype & FD_PTRSIZE) {
         ((int *)(buffer + j))[0] = lua_tointeger(Lua, i);
         arg_values[in] = buffer + j;
         arg_types[in++] = &ffi_type_sint32;
         j += sizeof(int);
      }
      else if (argtype & (FD_TAGS|FD_VARTAGS)) {
         ErrorMsg = "Functions using tags are not supported.";
         return ERR::NoSupport;
      }
      else {
         ErrorMsg = std::format("{}() unsupported arg '{}', flags ${:08x}.", mod->Functions[index].Name, args[i].Name, argtype);
         return ERR::NoSupport;
      }
   }

   // Call the function.  Determine return type and prepare FFI call interface once.

   int restype = args->Type;
   int result = 1;
   int total_args = i - 1;

   // Determine the correct FFI return type

   ffi_type *return_type;
   if (restype & FD_STR) return_type = &ffi_type_pointer;
   else if (restype & FD_OBJECT) return_type = &ffi_type_pointer;
   else if (restype & FD_PTR) return_type = &ffi_type_pointer;
   else if (restype & (FD_INT|FD_ERROR)) {
      if (restype & FD_UNSIGNED) return_type = &ffi_type_uint32;
      else return_type = &ffi_type_sint32;
   }
   else if (restype & FD_DOUBLE) return_type = &ffi_type_double;
   else if (restype & FD_INT64) return_type = &ffi_type_sint64;
   else { // Void
      return_type = &ffi_type_void;
      result = 0;
   }

   if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, total_args, return_type, arg_types) IS FFI_OK) {
      func_guard.OwnsReference = false;
      ffi_call(&cif, (void (*)())function, &rc, arg_values);
      copy_mutable_cpp_strings();
      if (not ErrorMsg.empty()) return ERR::BufferOverflow;

      // Process the result based on the return type
      if (restype & FD_STR) {
         lua_pushstring(Lua, (CSTRING)rc.Arg);
         if ((restype & FD_ALLOC) and ((CSTRING)rc.Arg)) FreeResource((APTR)rc.Arg);
      }
      else if (restype & FD_OBJECT) {
         if ((OBJECTPTR)rc.Arg) {
            push_object(Lua, (OBJECTPTR)rc.Arg,  (restype & FD_ALLOC) ? false : true);
         }
         else lua_pushnil(Lua);
      }
      else if (restype & FD_PTR) {
         if (restype & FD_STRUCT) {
            if (auto structptr = (APTR)rc.Arg) {
               ERR error;
               // A structure marked as a resource will be returned as an accessible struct pointer.  This is typically
               // needed when a struct's use is beyond informational and can be passed to other functions.
               //
               // Otherwise, the default behaviour is to convert the struct's content to a regular Lua table.
               if (restype & FD_RESOURCE) push_struct(Lua->script, structptr, args->Name, (restype & FD_ALLOC) ? TRUE : FALSE, TRUE);
               else if ((error = named_struct_to_table(Lua, args->Name, structptr)) != ERR::Okay) {
                  if (error IS ERR::Search) {
                     // Unknown structs are returned as pointers - this is mainly to indicate that there is a value
                     // and not a nil.
                     lua_pushlightuserdata(Lua, (APTR)structptr);
                  }
                  else {
                     ErrorMsg = std::format("Failed to resolve struct {}, error: {}", args->Name, GetErrorMsg(error));
                     return ERR::Search;
                  }
               }
            }
            else lua_pushnil(Lua);
         }
         else {
            if ((APTR)rc.Arg) lua_pushlightuserdata(Lua, (APTR)rc.Arg);
            else lua_pushnil(Lua);
         }
      }
      else if (restype & (FD_INT|FD_ERROR)) {
         if (restype & FD_UNSIGNED) {
            lua_pushnumber(Lua, (uint32_t)rc.Arg);
         }
         else {
            lua_pushinteger(Lua, (int)rc.Arg);
            if ((restype & FD_ERROR) and (rc.Arg >= int(ERR::ExceptionThreshold)) and in_try_immediate_scope(Lua)) {
               // Scope isolation: Only throw exceptions for direct calls within the try block.
               auto error = ERR(rc.Arg);
               ErrorMsg = std::format("{}() failed: {}", mod->Functions[index].Name ? mod->Functions[index].Name : "Function",
                  GetErrorMsg(error));
               return error;
            }
         }
      }
      else if (restype & FD_DOUBLE) {
         lua_pushnumber(Lua, rc.Double);
      }
      else if (restype & FD_INT64) {
         lua_pushnumber(Lua, rc.Int64);
      }
      // Void functions don't push anything to the stack
   }
   else lua_pushnil(Lua);

   Results = process_results(tiri, buffer, args) + result;
   return ERR::Okay;
}

//********************************************************************************************************************
// Convert FD_RESULT parameters to the equivalent Tiri result value.
// Also takes care of any cleanup code for dynamically allocated values.

static int process_results(extTiri *Tiri, APTR resultsidx, const FunctionField *args)
{
   kt::Log log(__FUNCTION__);

   auto lua = Tiri->Lua;
   auto scan = (uint8_t *)resultsidx;
   int results = 0;
   for (int i=1; args[i].Name; i++) {
      const auto argtype = args[i].Type;

      if ((argtype & FD_ARRAY) and (not (argtype & FD_BUFFER))) {
         if (argtype & FD_RESULT) {
            auto var = ((APTR *)scan)[0];
            scan += sizeof(APTR);
            if (var) {
               std::string_view argname(args[i].Name);
               if (argtype & FD_CPP) {
                  if (not push_cpp_array_result(lua, argtype, argname, var)) {
                     log.warning("Unsupported C++ array result arg %s, flags $%.8x", args[i].Name, argtype);
                     lua_pushnil(lua);
                  }
               }
               else {
                  APTR values = ((APTR *)var)[0];
                  int total_elements = -1; // If -1, make_any_table() assumes the array is null terminated.

                  if (args[i+1].Type & FD_ARRAYSIZE) {
                     CPTR size_var = ((APTR *)scan)[0];
                     if (args[i+1].Type & FD_INT) total_elements = ((int *)size_var)[0];
                     else if (args[i+1].Type & FD_INT64) total_elements = ((int64_t *)size_var)[0];
                     else log.warning("Invalid arg %s, flags $%.8x", args[i+1].Name, args[i+1].Type);
                  }

                  if (values) {
                     make_any_array(lua, argtype, argname, total_elements, values);
                     if (argtype & FD_ALLOC) FreeResource(values);
                  }
                  else lua_pushnil(lua);
               }
            }
            else lua_pushnil(lua);
            results++;
         }
         else scan += sizeof(APTR);
      }
      else if (argtype & FD_STR) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) {
               if (argtype & FD_CPP) {
                  if (argtype & FD_MUTABLE) { // std::string variant
                     lua_pushstring(lua, *((std::string *)var));
                  }
                  else { // std::string_view
                     lua_pushstring(lua, *((std::string_view *)var));
                  }
               }
               else {
                  lua_pushstring(lua, ((STRING *)var)[0]);
                  if ((argtype & FD_ALLOC) and (((STRING *)var)[0])) FreeResource(((STRING *)var)[0]);
               }
            }
            else lua_pushnil(lua);
            results++;
         }
         scan += sizeof(APTR);
      }
      else if (argtype & (FD_PTR|FD_BUFFER|FD_STRUCT)) {
         if (argtype & FD_RESULT) {
            auto var = ((APTR *)scan)[0];
            scan += sizeof(APTR);
            if (var) {
               if (argtype & FD_OBJECT) {
                  if (((APTR *)var)[0]) {
                     push_object(lua, ((OBJECTPTR *)var)[0], (argtype & FD_ALLOC) ? false : true);
                  }
                  else lua_pushnil(lua);
               }
               else if (argtype & FD_STRUCT) {
                  if (((APTR *)var)[0]) {
                     if (argtype & FD_RESOURCE) {
                        // Resource structures are managed with direct data addresses.
                        push_struct(Tiri, ((APTR *)var)[0], args[i].Name, (argtype & FD_ALLOC) ? TRUE : FALSE, TRUE);
                     }
                     else {
                        if (named_struct_to_table(lua, args[i].Name, ((APTR *)var)[0]) != ERR::Okay) lua_pushnil(lua);
                        if (argtype & FD_ALLOC) FreeResource(((APTR *)var)[0]);
                     }
                  }
                  else lua_pushnil(lua);
               }
               else if (argtype & FD_ALLOC) { // The result is a memory allocation.  Convert it to a binary 'string' of fixed length
                  int64_t size = 0;
                  if (args[i+1].Type & FD_BUFSIZE) {
                     const APTR size_var = ((APTR *)scan)[0];
                     if (args[i+1].Type & FD_INT) size = ((int *)size_var)[0];
                     else if (args[i+1].Type & FD_INT64) size = ((int64_t *)size_var)[0];
                     else log.warning("Invalid arg %s, flags $%.8x", args[i+1].Name, args[i+1].Type);
                  }
                  else {
                     MemInfo meminfo;
                     if (!MemoryInfo(GetMemoryID(((APTR *)var)[0]), &meminfo)) size = meminfo.Size;
                  }

                  if (size > 0) lua_pushlstring(lua, ((CSTRING *)var)[0], size);
                  else lua_pushnil(lua);

                  if (((APTR *)var)[0]) FreeResource(((APTR *)var)[0]);
               }
               else if (args[i+1].Type & FD_BUFSIZE) { // We can convert the data to a binary string rather than work with unsafe pointers.
                  int64_t size = 0;
                  CPTR size_var = ((APTR *)scan)[0];
                  if (args[i+1].Type & FD_INT) size = ((int *)size_var)[0];
                  else if (args[i+1].Type & FD_INT64) size = ((int64_t *)size_var)[0];
                  else log.warning("Invalid arg %s, flags $%.8x", args[i+1].Name, args[i+1].Type);

                  if (size > 0) lua_pushlstring(lua, ((CSTRING *)var)[0], size);
                  else lua_pushnil(lua);
               }
               else lua_pushlightuserdata(lua, ((APTR *)var)[0]);
            }
            else lua_pushnil(lua);
            results++;
         }
         else scan += sizeof(APTR);
      }
      else if (argtype & FD_INT) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushinteger(lua, ((int *)var)[0]);
            else lua_pushnil(lua);
            scan += sizeof(APTR);
            results++;
         }
         else scan += sizeof(int);
      }
      else if (argtype & FD_DOUBLE) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushnumber(lua, ((double *)var)[0]);
            else lua_pushnil(lua);
            scan += sizeof(APTR);
            results++;
         }
         else scan += sizeof(double);
      }
      else if (argtype & FD_INT64) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushnumber(lua, ((int64_t *)var)[0]);
            else lua_pushnil(lua);
            scan += sizeof(APTR);
            results++;
         }
         else scan += sizeof(int64_t);
      }
      else {
         log.warning("Unsupported arg '%s', flags $%x, aborting now.", args[i].Name, argtype);
         return results;
      }
   }

   return results;
}

//********************************************************************************************************************
// Register the module interface.

void register_module_class(lua_State *Lua)
{
   kt::Log log;

   static const struct luaL_Reg modlib_functions[] = {
      { "load", module_load },
      { "test", module_test },
      { nullptr, nullptr}
   };

   static const struct luaL_Reg modlib_methods[] = {
      { "__index",    module_index },
      { "__tostring", module_tostring },
      { "__gc",       module_destruct },
      { nullptr, nullptr }
   };

   log.trace("Registering module interface.");

   int stack_top = lua_gettop(Lua);

   luaL_newmetatable(Lua, "Tiri.mod");
   lua_pushstring(Lua, "Tiri.mod");
   lua_setfield(Lua, -2, "__name");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable
   lua_settable(Lua, -3);   // metatable.__index = metatable

   luaL_openlib(Lua, nullptr, modlib_methods, 0);
   luaL_openlib(Lua, "mod", modlib_functions, 0);

   lua_pop(Lua, 2); // Drop the Tiri.mod metatable and the mod library table

   int stack_delta = lua_gettop(Lua) - stack_top;
   if (stack_delta) log.warning("Module registration left %d value(s) on the Lua stack.", stack_delta);

   // Register mod interface prototypes for compile-time type inference
   reg_iface_prototype("mod", "load", { TiriType::Any }, { TiriType::Str });
   reg_iface_prototype("mod", "test", { TiriType::Num, TiriType::Num }, { TiriType::Any, TiriType::Str });
}
