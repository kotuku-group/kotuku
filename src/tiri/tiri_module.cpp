
#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/strings.hpp>
#include <inttypes.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_object.h"
#include "lib.h"

#include "hashes.h"
#include "defs.h"
#include "lj_proto_registry.h"

#include <ffi.h>
#include <unordered_map>
#include <algorithm>
#include <set>
#include <mutex>
#include <memory>
#include <span>

template<class... Args> void RMSG(Args...) {
   //log.msg(Args)
}

constexpr int MAX_MODULE_ARGS = 16;
constexpr size_t BUFFER_ELEMENT_SIZE = 16;
constexpr size_t BUFFER_SIZE = MAX_MODULE_ARGS * BUFFER_ELEMENT_SIZE;
constexpr size_t MAX_STRING_PREFIX_LENGTH = 200;
struct CaseInsensitiveCompare {
   bool operator()(const std::string &A, const std::string &B) const {
      return std::lexicographical_compare(
         A.begin(), A.end(), B.begin(), B.end(),
         [](char a, char b) { return std::tolower((unsigned char)a) < std::tolower((unsigned char)b); }
      );
   }
};

static std::set<std::string, CaseInsensitiveCompare> glLoadedConstants; // Stores the names of modules that have loaded constants (system wide)

[[nodiscard]] static CSTRING load_include_struct(objScript *, CSTRING, std::string_view);
[[nodiscard]] static CSTRING load_include_constant(CSTRING, std::string_view);

static int module_call(lua_State *);
static int process_results(prvTiri *, APTR, const FunctionField *);

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
   pf::Log log("load_include");

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

         glConstantRegistry.emplace(pf::strhash(name), constant);
      }

      if (*Line IS ',') Line++;
   }

   return next_line(Line);
}

//********************************************************************************************************************

static ERR process_module_defs(objScript *Script, objModule *module, CSTRING Name)
{
   OBJECTPTR root;
   if (auto error = module->get(FID_Root, root); error IS ERR::Okay) {
      struct ModHeader *header;
      if ((error = root->get(FID_Header, header)) != ERR::Okay) return error;
      if (not header) return ERR::NoData;

      if (auto structs = header->StructDefs) {
         for (auto &s : structs[0]) glStructSizes[s.first] = s.second;
      }

      if (auto idl = header->Definitions) {
         while ((idl) and (*idl)) {
            if ((idl[0] IS 's') and (idl[1] IS '.')) idl = load_include_struct(Script, idl+2, Name);
            else if ((idl[0] IS 'c') and (idl[1] IS '.')) idl = load_include_constant(idl+2, Name);
            else idl = next_line(idl);
         }
      }
      return ERR::Okay;
   }
   else return error;
}

//********************************************************************************************************************
// For the 'include' keyword.  Creates a temporary module object to process the definitions without formally opening
// an interface.

[[nodiscard]] ERR load_include(objScript *Script, CSTRING Module)
{
   ERR error = ERR::Okay;

   std::unique_lock lock(glConstantMutex); // Required to update the constant registry

   // Constants are system-wide, so process only once per session.

   bool process_constants = false;
   if (not glLoadedConstants.contains(Module)) {
      process_constants = true;
      glLoadedConstants.insert(Module);
   }

   if (process_constants) {
      pf::Log log(__FUNCTION__);
      log.branch("Definition: %s", Module);

      AdjustLogLevel(1);

         objModule::create module = { fl::Name(Module) };
         if (module.ok()) error = process_module_defs(Script, *module, Module);
         else error = ERR::CreateObject;

      AdjustLogLevel(-1);
   }

   return error;
}

//********************************************************************************************************************
// Format: s.Name:typeField,...
// TODO: This parses the struct definitions in advance - ideally we'd record the definition string and parse on first-use.

[[nodiscard]] static CSTRING load_include_struct(objScript *Script, CSTRING Line, std::string_view Source)
{
   int i;
   for (i=0; (Line[i] >= 0x20) and (Line[i] != ':'); i++);

   if (Line[i] IS ':') {
      std::string name(Line, i);
      Line += i + 1;

      int j;
      for (j=0; (Line[j] != '\n') and (Line[j] != '\r') and (Line[j]); j++);

      if ((Line[j] IS '\n') or (Line[j] IS '\r')) {
         std::string linebuf(Line, j);
         make_struct(Script, name, linebuf.c_str());
         while ((Line[j] IS '\n') or (Line[j] IS '\r')) j++;
         return Line + j;
      }
      else {
         make_struct(Script, name, Line);
         return Line + j;
      }
   }
   else {
      pf::Log(__FUNCTION__).warning("Malformed struct name in %.*s.", int(Source.size()), Source.data());
      return next_line(Line);
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
   Module->get(FID_FunctionList, mod->Functions);

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
      auto options = lua_tostring(Lua, 2);
      int passed = 0, total = 0;
      ((objModule *)mod->Module)->test(options, &passed, &total);
      lua_pushinteger(Lua, passed);
      lua_pushinteger(Lua, total);
      return 2;
   }
   else {
      luaL_argerror(Lua, 1, "Expected module.");
      return 0;
   }
}

//********************************************************************************************************************
// Usage: module = mod.load('core')

static int module_load(lua_State *Lua)
{
   auto modname = luaL_checkstring(Lua, 1);
   if (not modname) {
      luaL_argerror(Lua, 1, "String expected for module name.");
      return 0;
   }

   pf::Log log(__FUNCTION__);
   log.branch("Module: %s", modname);

   int i;
   for (i=0; modname[i]; i++) {
      if ((modname[i] >= 'a') and (modname[i] <= 'z')) continue;
      if ((modname[i] >= 'A') and (modname[i] <= 'Z')) continue;
      if ((modname[i] >= '0') and (modname[i] <= '9')) continue;
      break;
   }

   if ((modname[i]) or (i >= 32)) {
      luaL_error(Lua, ERR::Syntax, "Invalid module name; only alpha-numeric names are permitted with max 32 chars.");
      return 0;
   }

   if (auto loaded_mod = objModule::create::global(fl::Name(modname))) {
      {
         std::unique_lock lock(glConstantMutex); // Required to update the constant registry

         bool process_constants = false;
         if (not glLoadedConstants.contains(modname)) {
            process_constants = true;
            glLoadedConstants.insert(modname);
         }

         if (process_constants) {
            process_module_defs(Lua->script, loaded_mod, modname);
         }
      }

      new_module(Lua, loaded_mod);
      return 1;  // new userdatum is already on the stack
   }
   else {
      log.debranch();
      luaL_error(Lua, ERR::LoadModule, "Failed to load the %s module.", modname);
      return 0;
   }
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
      CSTRING name;
      if (mod->Module->get(FID_Name, name) IS ERR::Okay) {
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

static int module_call(lua_State *Lua)
{
   pf::Log log(__FUNCTION__);
   objScript *Self = Lua->script;
   uint8_t buffer[MAX_MODULE_ARGS * 16]; // 16 bytes seems overkill but some parameters output meta information (e.g. size).
   int i;

   // Track dynamically allocated objects for cleanup
   std::vector<std::string*> allocated_strings;
   std::vector<std::string_view*> allocated_string_views;
   std::vector<APTR> allocated_structs;

   // Cleanup lambda for early exits.  Note that we can't rely on RAII because luaL_error() breaks out of the function.
   auto cleanup = [&]() {
      for (auto ptr : allocated_strings) delete ptr;
      for (auto ptr : allocated_string_views) delete ptr;
      for (auto ptr : allocated_structs) FreeResource(ptr);
   };

   auto prv = (prvTiri *)Self->ChildPrivate;
   if (not prv) {
      log.warning(ERR::ObjectCorrupt);
      return 0;
   }

   auto mod = (module *)get_meta(Lua, lua_upvalueindex(1), "Tiri.mod");
   if (not mod) luaL_error(Lua, ERR::Args, "module_call() expected module in upvalue.");

   if (not mod->Functions) return 0;

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
      return 0;
   }

   APTR function = mod->Functions[index].Address;
   FUNCTION func;
   ffi_cif cif;
   ffi_arg rc;
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

         if (argtype & FD_BUFFER) {
            // The client must supply an argument that will store a buffer result.  This is a different case to the
            // storage of type values.  Buffers can be combined with FD_ARRAY to store more than one element.

            if (argtype & FD_CPP) {
               cleanup();
               luaL_error(Lua, ERR::NoSupport, "No support for calls utilising C++ arrays.");
               return 0;
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
                  cleanup();
                  luaL_error(Lua, ERR::InvalidType, "Function '%s' is not compatible with Tiri.", mod->Functions[index].Name);
                  return 0;
               }
            }
            else {
               cleanup();
               luaL_error(Lua, ERR::Args, "A memory buffer is required in arg #%d.", i);
               return 0;
            }
         }
         else if (argtype & FD_STR) { // FD_RESULT
            if (argtype & FD_CPP) {
               // Special case; we provide a std::string that will be used as a buffer for storing the result and destroy
               // it after processing results.
               auto str_ptr = new std::string;
               allocated_strings.push_back(str_ptr);
               ((std::string **)(buffer + j))[0] = str_ptr;
               arg_values[in]  = buffer + j;
               arg_types[in++] = &ffi_type_pointer;
               j += sizeof(APTR);
            }
            else {
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
            cleanup();
            luaL_error(Lua, ERR::InvalidType, "Unrecognised arg %d type %d", i, argtype);
            return 0;
         }
      }
      else if (argtype & FD_FUNCTION) {
         if (func.defined()) { // Is the function reserve already used?
            luaL_error(Lua, ERR::Args, "Multiple function arguments are not supported.");
            return 0;
         }

         switch(lua_type(Lua, i)) {
            case LUA_TSTRING: { // Name of function to call
               lua_getglobal(Lua, lua_tostring(Lua, i));
               func = FUNCTION(Self, luaL_ref(Lua, LUA_REGISTRYINDEX));
               ((FUNCTION **)(buffer + j))[0] = &func;
               break;
            }

            case LUA_TFUNCTION: { // Direct function reference
               lua_pushvalue(Lua, i);
               func = FUNCTION(Self, luaL_ref(Lua, LUA_REGISTRYINDEX));
               ((FUNCTION **)(buffer + j))[0] = &func;
               break;
            }

            case LUA_TNIL:
            case LUA_TNONE:
               ((FUNCTION **)(buffer + j))[0] = nullptr;
               break;

            default:
               cleanup();
               luaL_error(Lua, ERR::InvalidType, "Type mismatch, arg #%d (%s) expected function, got %s '%s'.", i, args[i].Name, lua_typename(Lua, lua_type(Lua, i)), lua_tostring(Lua, i));
               return 0;
         }

         arg_values[in]  = buffer + j;
         arg_types[in++] = &ffi_type_pointer;
         j += sizeof(FUNCTION *);
      }
      else if (argtype & FD_STR) {
         auto type = lua_type(Lua, i);

         if (argtype & FD_CPP) { // std::string_view (enforced, cannot be nullptr)
            size_t len;
            auto str = lua_tolstring(Lua, i, &len);
            auto view_ptr = new std::string_view(str, len);
            allocated_string_views.push_back(view_ptr);
            ((std::string_view **)(buffer + j))[0] = view_ptr;
         }
         else if ((type IS LUA_TSTRING) or (type IS LUA_TNUMBER) or (type IS LUA_TBOOLEAN)) {
            ((CSTRING *)(buffer + j))[0] = lua_tostring(Lua, i);
         }
         else if (type <= 0) {
            ((CSTRING *)(buffer + j))[0] = nullptr;
         }
         else if ((type IS LUA_TUSERDATA) or (type IS LUA_TLIGHTUSERDATA)) {
            cleanup();
            luaL_error(Lua, ERR::InvalidType, "Arg #%d (%s) requires a string and not untyped pointer.", i, args[i].Name, lua_typename(Lua, lua_type(Lua, i)), lua_tostring(Lua, i));
            return 0;
         }
         else {
            cleanup();
            luaL_error(Lua, ERR::InvalidType, "Type mismatch, arg #%d (%s) expected string, got %s '%s'.", i, args[i].Name, lua_typename(Lua, lua_type(Lua, i)), lua_tostring(Lua, i));
            return 0;
         }

         arg_values[in] = buffer + j;
         arg_types[in++] = &ffi_type_pointer;
         j += sizeof(APTR);
      }
      else if (argtype & FD_ARRAY) { // Pass array data pointer
         if (argtype & FD_CPP) {
            luaL_error(Lua, ERR::NoSupport, "No support for calls utilising C++ arrays.");
            return 0;
         }

         if (lua_type(Lua, i) IS LUA_TARRAY) {
            GCarray *arr = arrayV(Lua, i);
            arg_values[in] = arr->arraydata();
            arg_types[in++] = &ffi_type_pointer;
            j += sizeof(APTR); // Dummy increment

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
                     luaL_error(Lua, ERR::NoSupport, "Function '%s' is not compatible with Tiri.", mod->Functions[index].Name);
                     return 0;
                  }
               }
               else {
                  if (args[i+1].Type & FD_INT) {
                     ((int *)(buffer + j))[0] = arr->len;
                     arg_values[in] = buffer + j;
                     arg_types[in++] = &ffi_type_sint32;
                     j += sizeof(int);
                     i++;
                  }
                  else if (args[i+1].Type & FD_INT64) {
                     ((int64_t *)(buffer + j))[0] = arr->len;
                     arg_values[in] = buffer + j;
                     arg_types[in++] = &ffi_type_sint64;
                     j += sizeof(int64_t);
                     i++;
                  }
                  else {
                     cleanup();
                     luaL_error(Lua, ERR::NoSupport, "Function '%s' is not compatible with Tiri.", mod->Functions[index].Name);
                     return 0;
                  }
               }
            }
            else {
               cleanup();
               luaL_error(Lua, ERR::NoSupport, "Function '%s' is not compatible with Tiri.", mod->Functions[index].Name);
               return 0;
            }
         }
         else {
            cleanup();
            luaL_error(Lua, ERR::InvalidType, "Type mismatch, arg #%d (%s) expected array, got '%s'.", i, args[i].Name, lua_typename(Lua, lua_type(Lua, i)));
            return 0;
         }
      }
      else if (argtype & FD_PTR) {
         auto arg_type = lua_type(Lua, i);
         if (arg_type IS LUA_TSTRING) {
            // Lua strings need to be converted to C strings
            size_t strlen;
            ((CSTRING *)(buffer + j))[0] = lua_tolstring(Lua, i, &strlen);
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
               APTR struct_data;
               if (table_to_struct(Lua, args[i].Name, &struct_data) IS ERR::Okay) {
                  allocated_structs.push_back(struct_data); // Track for cleanup
                  ((APTR *)(buffer + j))[0] = struct_data;
                  arg_values[in] = buffer + j;
                  arg_types[in++] = &ffi_type_pointer;
                  j += sizeof(APTR);
               }
               else {
                  cleanup();
                  luaL_error(Lua, ERR::Failed, "Failed to convert table to struct for arg #%d (%s).", i, args[i].Name);
                  return 0;
               }
            }
            else {
               cleanup();
               luaL_error(Lua, ERR::InvalidType, "Type mismatch, arg #%d (%s) expected pointer, got table.", i, args[i].Name);
               return 0;
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
         cleanup();
         luaL_error(Lua, ERR::NoSupport, "Functions using tags are not supported.");
         return 0;
      }
      else {
         cleanup();
         log.warning("%s() unsupported arg '%s', flags $%.8x, aborting now.", mod->Functions[index].Name, args[i].Name, argtype);
         return 0;
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
      ffi_call(&cif, (void (*)())function, &rc, arg_values);

      // Process the result based on the return type
      if (restype & FD_STR) {
         lua_pushstring(Lua, (CSTRING)rc);
      }
      else if (restype & FD_OBJECT) {
         if ((OBJECTPTR)rc) {
            push_object(Lua, (OBJECTPTR)rc,  (restype & FD_ALLOC) ? false : true);
         }
         else lua_pushnil(Lua);
      }
      else if (restype & FD_PTR) {
         if (restype & FD_STRUCT) {
            if (auto structptr = (APTR)rc) {
               ERR error;
               // A structure marked as a resource will be returned as an accessible struct pointer.  This is typically
               // needed when a struct's use is beyond informational and can be passed to other functions.
               //
               // Otherwise, the default behaviour is to convert the struct's content to a regular Lua table.
               if (restype & FD_RESOURCE) push_struct(Self, structptr, args->Name, (restype & FD_ALLOC) ? TRUE : FALSE, TRUE);
               else if ((error = named_struct_to_table(Lua, args->Name, structptr)) != ERR::Okay) {
                  if (error IS ERR::Search) {
                     // Unknown structs are returned as pointers - this is mainly to indicate that there is a value
                     // and not a nil.
                     lua_pushlightuserdata(Lua, (APTR)structptr);
                  }
                  else {
                     cleanup();
                     luaL_error(Lua, ERR::Search, "Failed to resolve struct %s, error: %s", args->Name, GetErrorMsg(error));
                     return 0;
                  }
               }
            }
            else lua_pushnil(Lua);
         }
         else {
            if ((APTR)rc) lua_pushlightuserdata(Lua, (APTR)rc);
            else lua_pushnil(Lua);
         }
      }
      else if (restype & (FD_INT|FD_ERROR)) {
         if (restype & FD_UNSIGNED) {
            lua_pushnumber(Lua, (uint32_t)rc);
         }
         else {
            lua_pushinteger(Lua, (int)rc);
            if ((restype & FD_ERROR) and (rc >= int(ERR::ExceptionThreshold)) and in_try_immediate_scope(Lua)) {
               // Scope isolation: Only throw exceptions for direct calls within the try block.
               luaL_error(Lua, ERR(rc));
            }
         }
      }
      else if (restype & FD_DOUBLE) {
         lua_pushnumber(Lua, (double)rc);
      }
      else if (restype & FD_INT64) {
         lua_pushnumber(Lua, (int64_t)rc);
      }
      // Void functions don't push anything to the stack
   }
   else {
      lua_pushnil(Lua);
   }

   auto return_code = process_results(prv, buffer, args) + result;

   cleanup();

   return return_code;
}

//********************************************************************************************************************
// Convert FD_RESULT parameters to the equivalent Tiri result value.
// Also takes care of any cleanup code for dynamically allocated values.

static int process_results(prvTiri *prv, APTR resultsidx, const FunctionField *args)
{
   pf::Log log(__FUNCTION__);

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
               APTR values = ((APTR *)var)[0];
               int total_elements = -1; // If -1, make_any_table() assumes the array is null terminated.

               if (args[i+1].Type & FD_ARRAYSIZE) {
                  CPTR size_var = ((APTR *)scan)[0];
                  if (args[i+1].Type & FD_INT) total_elements = ((int *)size_var)[0];
                  else if (args[i+1].Type & FD_INT64) total_elements = ((int64_t *)size_var)[0];
                  else log.warning("Invalid arg %s, flags $%.8x", args[i+1].Name, args[i+1].Type);
               }

               if (values) {
                  make_any_array(prv->Lua, argtype, argname, total_elements, values);
                  if (argtype & FD_ALLOC) FreeResource(values);
               }
               else lua_pushnil(prv->Lua);
            }
            else lua_pushnil(prv->Lua);
            results++;
         }
         else scan += sizeof(APTR);
      }
      else if (argtype & FD_STR) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) {
               if (argtype & FD_CPP) { // std::string variant
                  auto str_result = (std::string *)var;
                  lua_pushlstring(prv->Lua, str_result->data(), str_result->size());
               }
               else {
                  lua_pushstring(prv->Lua, ((STRING *)var)[0]);
                  if ((argtype & FD_ALLOC) and (((STRING *)var)[0])) FreeResource(((STRING *)var)[0]);
               }
            }
            else lua_pushnil(prv->Lua);
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
                     push_object(prv->Lua, ((OBJECTPTR *)var)[0], (argtype & FD_ALLOC) ? false : true);
                  }
                  else lua_pushnil(prv->Lua);
               }
               else if (argtype & FD_STRUCT) {
                  if (((APTR *)var)[0]) {
                     if (argtype & FD_RESOURCE) {
                        // Resource structures are managed with direct data addresses.
                        push_struct(prv->Lua->script, ((APTR *)var)[0], args[i].Name, (argtype & FD_ALLOC) ? TRUE : FALSE, TRUE);
                     }
                     else {
                        if (named_struct_to_table(prv->Lua, args[i].Name, ((APTR *)var)[0]) != ERR::Okay) lua_pushnil(prv->Lua);
                        if (argtype & FD_ALLOC) FreeResource(((APTR *)var)[0]);
                     }
                  }
                  else lua_pushnil(prv->Lua);
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
                     if (MemoryIDInfo(GetMemoryID(((APTR *)var)[0]), &meminfo) IS ERR::Okay) size = meminfo.Size;
                  }

                  if (size > 0) lua_pushlstring(prv->Lua, ((CSTRING *)var)[0], size);
                  else lua_pushnil(prv->Lua);

                  if (((APTR *)var)[0]) FreeResource(((APTR *)var)[0]);
               }
               else if (args[i+1].Type & FD_BUFSIZE) { // We can convert the data to a binary string rather than work with unsafe pointers.
                  int64_t size = 0;
                  CPTR size_var = ((APTR *)scan)[0];
                  if (args[i+1].Type & FD_INT) size = ((int *)size_var)[0];
                  else if (args[i+1].Type & FD_INT64) size = ((int64_t *)size_var)[0];
                  else log.warning("Invalid arg %s, flags $%.8x", args[i+1].Name, args[i+1].Type);

                  if (size > 0) lua_pushlstring(prv->Lua, ((CSTRING *)var)[0], size);
                  else lua_pushnil(prv->Lua);
               }
               else lua_pushlightuserdata(prv->Lua, ((APTR *)var)[0]);
            }
            else lua_pushnil(prv->Lua);
            results++;
         }
         else scan += sizeof(APTR);
      }
      else if (argtype & FD_INT) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushinteger(prv->Lua, ((int *)var)[0]);
            else lua_pushnil(prv->Lua);
            scan += sizeof(APTR);
            results++;
         }
         else scan += sizeof(int);
      }
      else if (argtype & FD_DOUBLE) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushnumber(prv->Lua, ((double *)var)[0]);
            else lua_pushnil(prv->Lua);
            scan += sizeof(APTR);
            results++;
         }
         else scan += sizeof(double);
      }
      else if (argtype & FD_INT64) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushnumber(prv->Lua, ((int64_t *)var)[0]);
            else lua_pushnil(prv->Lua);
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
   pf::Log log;

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

   luaL_newmetatable(Lua, "Tiri.mod");
   lua_pushstring(Lua, "Tiri.mod");
   lua_setfield(Lua, -2, "__name");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable
   lua_settable(Lua, -3);   // metatable.__index = metatable

   luaL_openlib(Lua, nullptr, modlib_methods, 0);
   luaL_openlib(Lua, "mod", modlib_functions, 0);

   // Register mod interface prototypes for compile-time type inference
   reg_iface_prototype("mod", "load", { TiriType::Any }, { TiriType::Str });
   reg_iface_prototype("mod", "test", { TiriType::Num, TiriType::Num }, { TiriType::Any, TiriType::Str });
}
