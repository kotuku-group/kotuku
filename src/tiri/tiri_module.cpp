
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
#include "lj_struct.h"
#include "lj_tab.h"

#include "defs.h"
#include "lj_proto_registry.h"

#include <ffi.h>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <set>
#include <mutex>
#include <new>
#include <limits>

struct static_module_function_signature {
   std::string Name;
   std::vector<std::string> FieldNames;
   std::vector<FunctionField> Fields;
};

struct static_module_signature {
   std::string Name;
   std::vector<static_module_function_signature> Functions;
};

constexpr int MAX_MODULE_ARGS = 16;
constexpr size_t BUFFER_ELEMENT_SIZE = 16;
constexpr size_t BUFFER_SIZE = MAX_MODULE_ARGS * BUFFER_ELEMENT_SIZE;
constexpr size_t MAX_STRING_PREFIX_LENGTH = 200;

struct module;

// One process-wide callable record per exported module function.  Records are allocated individually so that their
// addresses remain stable from publication until expunge_modules(), which is required because Tiri closures retain a
// raw pointer to their record and the cached ffi_cif refers to ArgTypes in place.
//
// Every field is written once during preparation and is immutable afterwards, so concurrent calls from independent
// Tiri states can share one record without synchronisation.

struct module_callable {
   CSTRING Name = nullptr;               // Canonical name, owned by the module's Function list
   APTR Address = nullptr;               // Native function address; leave as null if function wasn't processed
   const FunctionField *Fields = nullptr; // Argument metadata, owned by the module's Function list

   ffi_cif Cif = { };
   ffi_type *ArgTypes[MAX_MODULE_ARGS] = { };

   inline bool trivial() { return Fields IS nullptr; }

   inline bool valid() { // Returns false if the function couldn't be processed
      return Address != nullptr;
   }

   inline void invalidate() { Address = nullptr; }
};

struct module {
   std::string Name;
   objModule *Module = nullptr;
   std::unique_ptr<const static_module_signature> Signature;
   ankerl::unordered_dense::map<uint32_t, std::vector<module_callable *>> FunctionMap;
   std::vector<std::unique_ptr<module_callable>> Callables;

   ~module() {
      if (Module) FreeResource(Module);
   }
};

static std::mutex glModuleMutex;
static std::vector<std::unique_ptr<module>> glModules;

template<class... Args> void RMSG(Args...) {
   //log.msg(Args)
}

struct module_span_arg {
   std::span<int8_t> Span;

   APTR set(CPTR Data, size_t Extent) noexcept {
      Span = std::span<int8_t>((int8_t *)Data, Extent);
      return &Span;
   }
};

// Resolve and validate the element metadata for a callable array.

static ERR module_span_element_metadata(lua_State *Lua, const FunctionField &Field, AET *ElementType,
   size_t *ElementSize, struct_record **StructDef)
{
   *StructDef = nullptr;

   if (Field.Type & FD_STRUCT) {
      if ((Field.Type & FD_PTR) or (not Field.Name) or (not valid_struct_name(Field.Name))) return ERR::InvalidData;
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
      bool(Field.Type & FD_BYTE) + bool(Field.Type & FD_PTR);
   if (type_count != 1) return ERR::InvalidData;

   if (Field.Type & FD_DOUBLE) { *ElementType = AET::DOUBLE; *ElementSize = sizeof(double); }
   else if (Field.Type & FD_INT64) { *ElementType = AET::INT64; *ElementSize = sizeof(int64_t); }
   else if (Field.Type & FD_FLOAT) { *ElementType = AET::FLOAT; *ElementSize = sizeof(float); }
   else if (Field.Type & FD_INT) { *ElementType = AET::INT32; *ElementSize = sizeof(int); }
   else if (Field.Type & FD_WORD) { *ElementType = AET::INT16; *ElementSize = sizeof(int16_t); }
   else if (Field.Type & FD_BYTE) { *ElementType = AET::BYTE; *ElementSize = sizeof(int8_t); }
   else { *ElementType = AET::PTR; *ElementSize = sizeof(APTR); }

   return ERR::Okay;
}

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

static std::unique_ptr<const static_module_signature> make_module_signature(
   std::string_view Name, const Function *Functions)
{
   if (not Functions) return {};

   auto signature = std::make_unique<static_module_signature>();
   signature->Name = Name;

   size_t function_count = 0;
   while (Functions[function_count].Name) function_count++;
   signature->Functions.reserve(function_count);

   for (size_t i = 0; i < function_count; ++i) {
      auto &entry = signature->Functions.emplace_back();
      entry.Name = Functions[i].Name;

      if (const FunctionField *fields = Functions[i].Args) {
         size_t field_count = 0;
         while (fields[field_count].Name) field_count++;
         entry.FieldNames.reserve(field_count);
         entry.Fields.reserve(field_count + 1);
         for (size_t j = 0; j < field_count; ++j) entry.FieldNames.emplace_back(fields[j].Name);
         for (size_t j = 0; j < field_count; ++j) {
            entry.Fields.push_back({ entry.FieldNames[j].c_str(), fields[j].Type });
         }
         entry.Fields.push_back({ nullptr, 0 });
      }
      else entry.Fields.push_back({ nullptr, 0 });
   }

   return signature;
}

StaticModuleHandle static_module_by_name(std::string_view Name) noexcept
{
   std::lock_guard lock(glModuleMutex);
   for (const auto &entry : glModules) {
      if (kt::iequals(entry->Name, Name)) return entry->Signature.get();
   }
   return nullptr;
}

const FunctionField * static_module_function(
   StaticModuleHandle Module, std::string_view Name) noexcept
{
   if (not Module) return nullptr;
   for (const auto &function : Module->Functions) {
      if (kt::iequals(function.Name, Name)) return function.Fields.data();
   }
   return nullptr;
}

std::string_view static_module_name(StaticModuleHandle Module) noexcept
{
   return Module ? std::string_view(Module->Name) : std::string_view();
}

std::string_view static_module_function_name(StaticModuleHandle Module, std::string_view Name) noexcept
{
   if (not Module) return {};
   for (const auto &function : Module->Functions) {
      if (kt::iequals(function.Name, Name)) return function.Name;
   }
   return {};
}

//********************************************************************************************************************
// Determine the libffi type for one argument.  The mapping mirrors the argument marshalling in module_call_inner();
// both must agree on the number and type of arguments passed to ffi_call().
//
// Signature-dependent validation that does not require a lua_State is performed here so that it happens once per
// process rather than on every call.  State-dependent checks, such as resolving a named structure for a span, remain
// in the call bridge.

static ERR callable_arg_type(const FunctionField &Field, ffi_type *&Type, std::string &ErrorMsg)
{
   const int argtype = Field.Type;

   if ((argtype & FDF_SPAN) IS FDF_SPAN) {
      if (argtype & (FD_RESULT|FD_ALLOC)) {
         ErrorMsg = std::format("Unsupported span result for arg '{}'.", Field.Name);
         return ERR::NoSupport;
      }
      Type = &ffi_type_pointer;
      return ERR::Okay;
   }

   if (argtype & FD_ARRAY) { // Non-span arrays are no longer marshalled
      ErrorMsg = std::format("Unsupported pointer array arg '{}'.", Field.Name);
      return ERR::NoSupport;
   }

   if (argtype & FD_RESULT) {
      // Every result argument is passed as a pointer to storage that the bridge provides.
      if ((((argtype & FDF_VECTOR) IS FDF_VECTOR) and (argtype & FD_MUTABLE)) or
          (argtype & (FD_STR|FD_PTR|FD_INT|FD_DOUBLE|FD_INT64))) {
         Type = &ffi_type_pointer;
         return ERR::Okay;
      }
      ErrorMsg = std::format("Unrecognised result arg '{}', flags ${:08x}.", Field.Name, argtype);
      return ERR::InvalidType;
   }

   if (argtype & FD_FUNCTION) { Type = &ffi_type_pointer; return ERR::Okay; }
   if (argtype & FD_STR) { Type = &ffi_type_pointer; return ERR::Okay; }

   if ((argtype & FDF_VECTOR) IS FDF_VECTOR) {
      ErrorMsg = "C++ array inputs are not supported.";
      return ERR::NoSupport;
   }

   if (argtype & FD_PTR) { Type = &ffi_type_pointer; return ERR::Okay; }
   if (argtype & FD_INT) { Type = &ffi_type_sint32; return ERR::Okay; }
   if (argtype & FD_DOUBLE) { Type = &ffi_type_double; return ERR::Okay; }
   if (argtype & FD_INT64) { Type = &ffi_type_sint64; return ERR::Okay; }

   if (argtype & (FD_TAGS|FD_VARTAGS)) {
      ErrorMsg = "Functions using tags are not supported.";
      return ERR::NoSupport;
   }

   ErrorMsg = std::format("Unsupported arg '{}', flags ${:08x}.", Field.Name, argtype);
   return ERR::NoSupport;
}

//********************************************************************************************************************
// Determine the libffi return type from the function's result descriptor, which is stored in element zero of the
// argument list.

static ffi_type * callable_return_type(int ResultType)
{
   if (ResultType & FD_STR) return &ffi_type_pointer;
   if (ResultType & FD_OBJECT) return &ffi_type_pointer;
   if (ResultType & FD_PTR) return &ffi_type_pointer;
   if (ResultType & (FD_INT|FD_ERROR)) {
      return (ResultType & FD_UNSIGNED) ? &ffi_type_uint32 : &ffi_type_sint32;
   }
   if (ResultType & FD_DOUBLE) return &ffi_type_double;
   if (ResultType & FD_INT64) return &ffi_type_sint64;
   return &ffi_type_void;
}

//********************************************************************************************************************
// Build the process-wide callable records for a module.  Called once, while the module is being resolved and before
// it is published to glModules, so no other thread can observe a partially prepared record.
//
// A preparation failure is recorded on the individual callable rather than propagated, because one unsupported
// signature must not prevent the rest of the module from being used.

static void prepare_module_callables(module *Module, const Function *Functions)
{
   kt::Log log(__FUNCTION__);

   if (not Functions) return;

   size_t function_count = 0;
   while (Functions[function_count].Name) function_count++;
   Module->Callables.reserve(function_count);

   for (size_t index = 0; index < function_count; ++index) {
      const auto &function = Functions[index];
      auto callable = std::make_unique<module_callable>();

      callable->Name    = function.Name;
      callable->Address = function.Address;
      callable->Fields  = function.Args;

      if (not function.Args) {
         // A function with no argument list takes no parameters and returns nothing, so libffi is unnecessary.
         Module->Callables.push_back(std::move(callable));
         continue;
      }

      const FunctionField *args = function.Args;
      int total = 0;
      for (int arg = 1; args[arg].Name; ++arg) {
         if (total >= MAX_MODULE_ARGS) {
            callable->invalidate();
            log.msg("Function '%s' exceeds the %d argument limit.", function.Name, MAX_MODULE_ARGS);
            break;
         }

         ffi_type *type = nullptr;
         std::string message;
         if (auto error = callable_arg_type(args[arg], type, message); error != ERR::Okay) {
            callable->invalidate();
            log.msg("Function '%s': %s", function.Name, message.c_str());
            break;
         }
         callable->ArgTypes[total++] = type;
      }

      if (callable->valid()) {
         ffi_type *return_type = callable_return_type(args->Type);

         if (ffi_prep_cif(&callable->Cif, FFI_DEFAULT_ABI, total, return_type, callable->ArgTypes) != FFI_OK) {
            callable->invalidate();
            log.msg("Failed to prepare the call interface for '%s'.", function.Name);
         }
      }

      Module->FunctionMap[strihash(function.Name)].push_back(callable.get());
      Module->Callables.push_back(std::move(callable));
   }
}

//********************************************************************************************************************
// Resolve one process-wide module instance.  Entries remain stable until the Tiri module is expunged.

static ERR resolve_module(std::string_view Name, module *&Result)
{
   std::lock_guard lock(glModuleMutex);
   for (const auto &entry : glModules) {
      if (kt::iequals(entry->Name, Name)) {
         Result = entry.get();
         return ERR::Okay;
      }
   }

   auto loaded_module = objModule::create::global(fl::Name(Name));
   if (not loaded_module) return ERR::CreateObject;

   auto entry = std::make_unique<module>();
   entry->Module = loaded_module;
   const Function *functions = nullptr;
   loaded_module->getFunctionList(functions);

   std::string_view canonical_name;
   if (loaded_module->getName(canonical_name) != ERR::Okay or canonical_name.empty()) canonical_name = Name;
   entry->Name = canonical_name;
   entry->Signature = make_module_signature(entry->Name, functions);
   prepare_module_callables(entry.get(), functions);

   Result = entry.get();
   glModules.push_back(std::move(entry));
   return ERR::Okay;
}

void expunge_modules()
{
   {
      std::lock_guard lock(glModuleMutex);
      glModules.clear();
   }
   {
      std::unique_lock lock(glConstantMutex);
      glLoadedConstants.clear();
   }
}

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
// Process a process-wide module's definitions once.

static ERR ensure_module_defs(extTiri *Script, module *Module)
{
   std::unique_lock lock(glConstantMutex);
   if (glLoadedConstants.contains(Module->Name)) return ERR::Okay;

   ERR error = process_module_defs(Script, Module->Module, Module->Name);
   if (error IS ERR::Okay) glLoadedConstants.emplace(Module->Name);
   return error;
}

//********************************************************************************************************************
// For the 'include' statement.  Resolves the process-wide module and makes its definitions available to the parser.

[[nodiscard]] ERR load_include(extTiri *Script, std::string_view Module)
{
   module *resolved_module = nullptr;
   if (auto error = resolve_module(Module, resolved_module); error != ERR::Okay) return error;

   AdjustLogLevel(1);
   ERR error = ensure_module_defs(Script, resolved_module);
   AdjustLogLevel(-1);
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
// Usage: passed, total = mod.test(ModuleName, Options)
// Resolves the named module and runs its unit tests, if any.

static int module_test(lua_State *Lua)
{
   CSTRING module_name = luaL_checkstring(Lua, 1);

   module *mod = nullptr;
   if (auto error = resolve_module(module_name, mod); error != ERR::Okay) {
      luaL_error(Lua, error, "Failed to load the %s module.", module_name);
   }

   auto options = lua_tostringview(Lua, 2);
   int passed = 0, total = 0;
   if (mod->Module->test(options, &passed, &total) IS ERR::Okay) {
      lua_pushinteger(Lua, passed);
      lua_pushinteger(Lua, total);
      return 2;
   }
   luaL_error(Lua, ERR::Failed, "Module test failed.");
   return 0;
}

//********************************************************************************************************************
// Locate one exported function by canonical name.  Case-insensitive, matching the compiler's source-level resolution.

static module_callable * find_module_callable(module *Module, std::string_view Name)
{
   auto hash = strihash(Name);
   if (auto it = Module->FunctionMap.find(hash); it != Module->FunctionMap.end()) {
      for (auto callable : it->second) {
         if (kt::iequals(callable->Name, Name)) return callable;
      }
   }
   return nullptr;
}

//********************************************************************************************************************
// Activate a compiler-declared module dependency.
//
// Usage: binding1, binding2, ... = mod.<dependency>(ModuleName, FunctionName1, FunctionName2, ...)
//
// The compiler passes the canonical module name followed by the canonical names of the functions its compilation unit
// actually references, and receives one closure per name in matching order.  Only names are persisted in bytecode, so
// a chunk loaded in a fresh process resolves against the module's current function list.
//
// No module table, metatable, environment cache or per-function name lookup table is created.  A declaration that
// references no function still resolves its module and returns one sentinel value, so that the generated local
// declaration remains well-formed and the dependency is retained.

static int module_dependency(lua_State *Lua)
{
   auto modname = luaL_checkstring(Lua, 1);
   if (not modname) luaL_argerror(Lua, 1, "String expected for module name.");

   module *mod = nullptr;
   if (auto error = resolve_module(modname, mod); error != ERR::Okay) {
      luaL_error(Lua, error, "Failed to load the %s module.", modname);
   }
   if (auto error = ensure_module_defs(Lua->script, mod); error != ERR::Okay) {
      luaL_error(Lua, error, "Failed to process definitions for the %s module.", modname);
   }

   int requested = lua_gettop(Lua) - 1;
   if (requested <= 0) { // Dependency-only activation; the module is now resolved and retained.
      lua_pushboolean(Lua, 1);
      return 1;
   }

   for (int arg = 2; arg <= requested + 1; ++arg) {
      auto function_name = lua_tostringview(Lua, arg);
      if (function_name.empty()) luaL_argerror(Lua, arg, "String expected for module function name.");

      auto callable = find_module_callable(mod, function_name);
      if (not callable) {
         luaL_error(Lua, ERR::Search, "Function %.*s() is not exported by the %s module.",
            int(function_name.size()), function_name.data(), modname);
      }

      lua_pushlightuserdata(Lua, callable);
      lua_pushcclosure(Lua, module_call, 1);
   }

   return requested;
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
// ARRAY        = Unsupported.  Raw pointer-array marshalling has been removed.
// ARRAY|CPP    = Unsupported as input.  kt::vector<> input marshalling is not implemented for module calls.
// SPAN         = const std::span<T> &.  Accepts matching Tiri array or structure storage, byte strings or an empty value.
// SPAN|MUTABLE = const std::span<T> &.  Requires writable array, structure or mutable byte-string storage.
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
// RESULT|PTR|ALLOC  = APTR *.  Function writes an allocated block; returned as a byte array when BUFSIZE is
//                     available, then freed.
// RESULT|PTR|STRUCT = Struct **.  Function writes a struct pointer; returned as a Tiri table, or as a managed
//                     struct when RESOURCE is set.
// RESULT|ARRAY      = Unsupported.  Raw pointer-array marshalling has been removed.
// RESULT|ARRAY|CPP  = kt::vector<> *.  Tiri provides an empty kt::vector<>; returned to Tiri as an array.
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

   uint8_t buffer[BUFFER_SIZE]; // 16 bytes per argument accommodates output metadata such as sizes.
   int i;

   // Track dynamically allocated objects for cleanup
   struct allocated_struct_ref {
      std::unique_ptr<uint8_t[]> Data;
      struct_record *Def = nullptr;
      lua_State *Lua = nullptr;

      allocated_struct_ref() = default;
      allocated_struct_ref(std::unique_ptr<uint8_t[]> InitData, struct_record *InitDef, lua_State *InitLua):
         Data(std::move(InitData)), Def(InitDef), Lua(InitLua) { }

      allocated_struct_ref(const allocated_struct_ref &) = delete;
      allocated_struct_ref & operator=(const allocated_struct_ref &) = delete;

      allocated_struct_ref(allocated_struct_ref &&Other) noexcept:
         Data(std::move(Other.Data)), Def(Other.Def), Lua(Other.Lua) {
         Other.Def = nullptr;
         Other.Lua = nullptr;
      }

      allocated_struct_ref & operator=(allocated_struct_ref &&Other) noexcept {
         if (this != &Other) {
            release();
            Data = std::move(Other.Data);
            Def = Other.Def;
            Lua = Other.Lua;
            Other.Def = nullptr;
            Other.Lua = nullptr;
         }
         return *this;
      }

      ~allocated_struct_ref() {
         release();
      }

      void release() {
         if (Data) {
            if (Def) destroy_struct_cpp_strings(Lua, *Def, Data.get());
            Data.reset();
            Def = nullptr;
            Lua = nullptr;
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
   std::array<module_span_arg, MAX_MODULE_ARGS> span_args;
   size_t span_count = 0;
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

   // One light-userdata upvalue holds the process-wide callable record, which carries the native address, the argument
   // metadata and the libffi call interface that was prepared when the module was resolved.

   auto callable = (module_callable *)lua_touserdata(Lua, lua_upvalueindex(1));
   if (not callable) {
      ErrorMsg = "module_call() expected a callable record in its upvalue.";
      return ERR::Args;
   }

   // Signature problems are detected during preparation and reported here, so that an unsupported function fails
   // consistently whether it is called directly or through an extracted callable.
   if (not callable->valid()) {
      ErrorMsg = "Function not compatible with Tiri";
      return ERR::NoSupport;
   }

   int nargs = lua_gettop(Lua);
   if (nargs > MAX_MODULE_ARGS-1) {
      log.warning("Limit of %d args exceeded.", MAX_MODULE_ARGS - 1);
      nargs = MAX_MODULE_ARGS-1;
   }

   uint8_t *end = buffer + sizeof(buffer);

   log.trace("%s() Args: %d", callable->Name, nargs);

   if (callable->trivial()) {
      auto function = (void (*)(void))callable->Address;
      function();
      return ERR::Okay;
   }

   const FunctionField *args = callable->Fields;
   APTR function = callable->Address;
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

   union {
      ffi_arg Arg;
      double  Double;
      int64_t Int64;
   } rc = { };
   void * arg_values[MAX_MODULE_ARGS];
   int in = 0;

   int j = 0;

   for (i=1; args[i].Name; i++) {
      int argtype = args[i].Type;

      if ((argtype & FDF_SPAN) IS FDF_SPAN) {
         if (span_count >= span_args.size()) {
            ErrorMsg = "Too many span arguments.";
            return ERR::Args;
         }

         if (argtype & (FD_RESULT|FD_ALLOC)) {
            ErrorMsg = std::format("Function '{}' uses an unsupported span result.", callable->Name);
            return ERR::NoSupport;
         }

         bool mutable_span = argtype & FD_MUTABLE;

         AET element_type;
         size_t element_size;
         struct_record *struct_def;
         if (auto error = module_span_element_metadata(Lua, args[i], &element_type, &element_size, &struct_def);
             error != ERR::Okay) {
            ErrorMsg = (error IS ERR::Search) ?
               std::format("Function '{}' references an unknown span structure.", callable->Name) :
               std::format("Function '{}' uses invalid span element metadata.", callable->Name);
            return error;
         }

         CPTR data = nullptr;
         size_t extent = 0;
         auto &span_arg = span_args[span_count++];
         auto value_type = lua_type(Lua, i);

         if (value_type IS LUA_TARRAY) {
            auto array = arrayV(Lua, i);
            if (mutable_span and array->is_readonly()) {
               ErrorMsg = std::format("Arg #{} ({}) requires a writable array.", i, args[i].Name);
               return ERR::InvalidType;
            }

            if ((array->elemtype != element_type) or (size_t(array->elemsize) != element_size)) {
               ErrorMsg = std::format("Arg #{} ({}) array element type does not match the span.", i, args[i].Name);
               return ERR::InvalidType;
            }
            if ((element_type IS AET::STRUCT) and (array->structdef != struct_def)) {
               ErrorMsg = std::format("Arg #{} ({}) structure array type does not match the span.", i, args[i].Name);
               return ERR::InvalidType;
            }
            data = array->arraydata();
            extent = array->len;
         }
         else if (value_type IS LUA_TSTRING) {
            if (element_type != AET::BYTE) {
               ErrorMsg = std::format("Arg #{} ({}) only accepts string storage for a byte span.", i, args[i].Name);
               return ERR::InvalidType;
            }

            auto string = string_arg(Lua, i);
            if (mutable_span and not lj_str_ismutable(string)) {
               ErrorMsg = std::format("Arg #{} ({}) requires a mutable string buffer.", i, args[i].Name);
               return ERR::InvalidType;
            }
            data = mutable_span ? CPTR(strdatawr(string)) : CPTR(strdata(string));
            extent = string->len;
         }
         else if ((value_type IS LUA_TNIL) or (value_type IS LUA_TNONE)) {
            // The canonical empty span is constructed below.
         }
         else if (auto native_struct = lua_isstruct(Lua, i) ? lua_tostruct(Lua, i) : nullptr) {
            if (element_type != AET::STRUCT) {
               ErrorMsg = std::format("Arg #{} ({}) requires matching array storage.", i, args[i].Name);
               return ERR::InvalidType;
            }
            if (lj_struct_stale(native_struct)) {
               ErrorMsg = std::format("Arg #{} ({}) structure storage is stale.", i, args[i].Name);
               return ERR::DoesNotExist;
            }
            if ((native_struct->def != struct_def) or (size_t(native_struct->structsize) != element_size)) {
               ErrorMsg = std::format("Arg #{} ({}) structure type does not match the span.", i, args[i].Name);
               return ERR::InvalidType;
            }
            if (not native_struct->data) {
               ErrorMsg = std::format("Arg #{} ({}) structure storage is unavailable.", i, args[i].Name);
               return ERR::InvalidData;
            }
            data = native_struct->data;
            extent = 1;
         }
         else {
            ErrorMsg = std::format("Arg #{} ({}) expected an array, string, structure or nil for a span, got {}.", i,
               args[i].Name, lua_typename(Lua, value_type));
            return ERR::InvalidType;
         }

         if (extent and not data) {
            ErrorMsg = std::format("Arg #{} ({}) span storage is unavailable.", i, args[i].Name);
            return ERR::InvalidData;
         }

         auto span_address = span_arg.set(data, extent);
         ((APTR *)(buffer + j))[0] = span_address;
         arg_values[in] = buffer + j;
         in++;
         j += sizeof(APTR);
      }
      else if (argtype & FD_RESULT) {
         // Result arguments are stored in the buffer with a pointer to an empty variable space (stored at the end of
         // the buffer)

         RMSG("Result for arg %d stored at %p", i, end);

         if (((argtype & FDF_VECTOR) IS FDF_VECTOR) and (argtype & FD_MUTABLE)) {
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
               in++;
               j += sizeof(APTR);
            }
            else {
               ErrorMsg = std::format("Function '{}' uses an unsupported C++ array result type.", callable->Name);
               return error;
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
                  in++;
                  j += sizeof(APTR);
               }
               else {
                  // Non-mutable string reference, supported as a std::string_view
                  string_views.emplace_back();
                  ((std::string_view **)(buffer + j))[0] = &string_views.back();
                  arg_values[in]  = buffer + j;
                  in++;
                  j += sizeof(APTR);
               }
            }
            else { // C-style string reference, this parameter style is being deprecated.
               end -= sizeof(APTR);
               ((APTR *)(buffer + j))[0] = end;
               ((APTR *)end)[0] = nullptr;
               arg_values[in]  = buffer + j;
               in++;
               j += sizeof(APTR);
            }
         }
         else if (argtype & FD_PTR) { // FD_RESULT
            end -= sizeof(APTR);
            ((APTR *)(buffer + j))[0] = end;
            ((APTR *)end)[0] = nullptr;
            arg_values[in]  = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else if (argtype & FD_INT) { // FD_RESULT
            end -= sizeof(int);
            ((APTR *)(buffer + j))[0] = end;
            ((int *)end)[0] = 0;
            arg_values[in]  = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else if (argtype & (FD_DOUBLE|FD_INT64)) { // FD_RESULT
            end -= sizeof(int64_t);
            ((APTR *)(buffer + j))[0] = end;
            ((int64_t *)end)[0] = 0;
            arg_values[in]  = buffer + j;
            in++;
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
         in++;
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
         in++;
         j += type_size;
      }
      else if ((argtype & FDF_VECTOR) IS FDF_VECTOR) {
         ErrorMsg = "C++ array inputs are not supported.";
         return ERR::NoSupport;
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

            ((CSTRING *)(buffer + j))[0] = (argtype & FD_MUTABLE) ? strdatawr(string) : strdata(string);
            arg_values[in] = buffer + j;
            in++;
            j += sizeof(CSTRING);
         }
         else if (auto native_struct = lua_isstruct(Lua, i) ? lua_tostruct(Lua, i) : nullptr) {
            // Guard specific to lifecycle-bound struct views; structs without an object dependency skip it.
            if (lj_struct_stale(native_struct)) {
               ErrorMsg = "A struct argument's providing object has been destroyed.";
               return ERR::DoesNotExist;
            }
            ((APTR *)(buffer + j))[0] = native_struct->data;
            arg_values[in] = buffer + j;
            in++;
            j += sizeof(APTR);

            log.trace("Struct address %p inserted to arg offset %d", native_struct->data, j);
         }
         else if (arg_type IS LUA_TOBJECT) {
            auto obj = lua_toobject(Lua, i);
            if (auto direct = direct_object_ptr(obj)) {
               ((OBJECTPTR *)(buffer + j))[0] = direct;
            }
            else {
               OBJECTPTR ptr_obj;
               if (auto error = access_object(obj, ptr_obj); error IS ERR::Okay) {
                  ((OBJECTPTR *)(buffer + j))[0] = ptr_obj;
                  release_object(obj);
               }
               else {
                  // Referenced object probably does not exist
                  ErrorMsg = std::format("Unable to resolve object #{}: {}", obj->uid, GetErrorMsg(error));
                  return error;
               }
            }

            arg_values[in] = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else if (arg_type IS LUA_TARRAY) {
            GCarray *array = lua_toarray(Lua, i);

            ((APTR *)(buffer + j))[0] = array->arraydata();
            arg_values[in] = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else if (arg_type IS LUA_TTABLE) {
            if (args[i].Type & FD_STRUCT) {
               // Convert Lua table to C struct
               lua_pushvalue(Lua, i); // Duplicate table for table_to_struct (consumes stack)
               std::unique_ptr<uint8_t[]> struct_data;
               if (!table_to_struct(Lua, args[i].Name, struct_data)) {
                  auto struct_address = struct_data.get();
                  auto def = find_struct(Lua, args[i].Name);
                  if (def) allocated_structs.emplace_back(std::move(struct_data), def, Lua);
                  else allocated_structs.emplace_back(std::move(struct_data), nullptr, Lua);
                  ((APTR *)(buffer + j))[0] = struct_address;
                  arg_values[in] = buffer + j;
                  in++;
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
            in++;
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
         in++;
         j += sizeof(int);
      }
      else if (argtype & FD_DOUBLE) {
         ((double *)(buffer + j))[0] = lua_tonumber(Lua, i);
         arg_values[in] = buffer + j;
         in++;
         j += sizeof(double);
      }
      else if (argtype & FD_INT64) {
         ((int64_t *)(buffer + j))[0] = lua_tointeger(Lua, i);
         arg_values[in] = buffer + j;
         in++;
         j += sizeof(int64_t);
      }
      else if (argtype & (FD_TAGS|FD_VARTAGS)) {
         ErrorMsg = "Functions using tags are not supported.";
         return ERR::NoSupport;
      }
      else {
         ErrorMsg = std::format("{}() unsupported arg '{}', flags ${:08x}.", callable->Name, args[i].Name, argtype);
         return ERR::NoSupport;
      }
   }

   // Call the function.  The call interface and return type were prepared when the module was resolved, so the hot
   // path only has to verify that the marshaller produced the argument count the interface was built for.

   int restype = args->Type;
   int result = (callable->Cif.rtype IS &ffi_type_void) ? 0 : 1;

   if (in != int(callable->Cif.nargs)) {
      ErrorMsg = std::format("Function '{}' marshalled {} args but its call interface expects {}.",
         callable->Name, in, callable->Cif.nargs);
      return ERR::Mismatch;
   }

   {
      func_guard.OwnsReference = false;
      ffi_call(&callable->Cif, (void (*)())function, &rc, arg_values);
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
               ErrorMsg = std::format("{}() failed: {}", callable->Name, GetErrorMsg(error));
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

      if ((argtype & FDF_SPAN) IS FDF_SPAN) {
         scan += sizeof(APTR);
      }
      else if ((argtype & FDF_VECTOR) IS FDF_VECTOR) {
         if (argtype & FD_RESULT) {
            auto var = ((APTR *)scan)[0];
            scan += sizeof(APTR);
            if (var) {
               std::string_view argname(args[i].Name);
               if (not push_cpp_array_result(lua, argtype, argname, var)) {
                  log.warning("Unsupported C++ array result arg %s, flags $%.8x", args[i].Name, argtype);
                  lua_pushnil(lua);
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
      else if (argtype & (FD_PTR|FD_STRUCT)) {
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
               else if (argtype & FD_ALLOC) {
                  // Misconfigured or unsupported parameter
                  lua_pushnil(lua);
                  if (((APTR *)var)[0]) FreeResource(((APTR *)var)[0]);
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
      { "\x1f" "dependency", module_dependency },
      { "test", module_test },
      { nullptr, nullptr}
   };

   luaL_openlib(Lua, "mod", modlib_functions, 0);
   lua_pop(Lua, 1); // Drop the mod library table

   // Register mod interface prototypes for compile-time type inference
   reg_iface_prototype("mod", "\x1f" "dependency", { TiriType::Table }, { TiriType::Str });
   reg_iface_prototype("mod", "test", { TiriType::Num, TiriType::Num }, { TiriType::Str, TiriType::Str });
}
