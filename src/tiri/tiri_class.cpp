/*********************************************************************************************************************

-CLASS-
Tiri: Extends the Script class with support for the Tiri language.

The Tiri class provides functionality for running scripts written in the Tiri programming language.

<header>Automatic Caching</>

Setting `SCF::AUTO_CACHE` automatically caches eligible file-backed scripts under `temp:tiri/cache/`.  A non-empty
@Script.CacheFile remains authoritative and selects an explicit destination even when automatic caching is enabled.
Automatic caching is disabled by default.  Without `SCF::AUTO_CACHE`, a script uses a cache only when
@Script.CacheFile is set explicitly.  Direct byte-code input, statement input and overrides, string paths,
documentation processing and parser diagnostic modes do not use automatically selected caches.

Cache output records a schema-versioned manifest and gzip-compressed byte-code payload in one envelope.  The manifest
identifies the producing build, root source, compilation options, imports, path resolutions and compile-time conditions.
A cache is reused only when those observations can be reproduced from current source content.  Content digests detect
edits even when modification dates and file sizes are unchanged.  Any mismatch recompiles from the retained source
snapshot and republishes the cache.

Schema-versioned caches require readable root and imported sources.  Obsolete schemas and legacy raw cache files are
cache misses.  A current cache envelope opened directly as a `.tbc` file remains authoritative.  Cache lookup and
publication are best-effort optimisations: a missing, invalid or unwritable cache does not prevent valid source from
compiling.  Warm validation still reads and digests the root and imported source files.

Caches are disabled for a script that sets `SCF::PROCESS_DOC`, because the parser metadata collected for
documentation tools is not stored in byte code.  Runtime-only Script flags and JIT options do not change cache
identity.

-END-

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/modules/compression.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/processes.h>
#include <kotuku/strings.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "lua.hpp"

#include "lj_obj.h"
#include "lj_state.h"
#include "parser/parser_diagnostics.h"
#include "jit/src/debug/dump_bytecode.h"
#include "lj_proto_registry.h"
#include "tiri_build_identity.h"
#include "packaging/cache_manifest.h"
#include "bytecode_storage.h"
#include "packaging/import_module_format.h"

#include "defs.h"

enum class CompilationInputOrigin : uint8_t {
   SOURCE,
   DIRECT_BYTECODE,
   SELECTED_CACHE
};

struct CompilationInput {
   std::string OwnedPayload;
   bool OwnsPayload = false;
   bool Binary = false;
};

static ERR run_script(extTiri *);
static ERR stack_args(lua_State *, OBJECTID, const FunctionField *, int8_t *);
static ERR save_binary(lua_State *, OBJECTPTR, std::string_view);
static ERR register_interfaces(lua_State *);
ERR initialise_tiri_compilation_state(lua_State *);
static ERR load_statement(extTiri *);

static ERR TIRI_Activate(extTiri *);
static ERR TIRI_DataFeed(extTiri *, struct acDataFeed *);
static ERR TIRI_Init(extTiri *);
static ERR TIRI_NewChild(extTiri *, struct acNewChild &);
static ERR TIRI_Query(extTiri *);
static ERR TIRI_SaveToObject(extTiri *, struct acSaveToObject *);

//********************************************************************************************************************

constexpr std::string_view check_bom(std::string_view Value)
{
   if ((Value.size() >= 3) and (Value[0] IS '\xef') and (Value[1] IS '\xbb') and (Value[2] IS '\xbf'))
      return Value.substr(3); // UTF-8 BOM
   if ((Value.size() >= 2) and (Value[0] IS '\xfe') and (Value[1] IS '\xff'))
      return Value.substr(2); // UTF-16 BOM big endian
   if ((Value.size() >= 2) and (Value[0] IS '\xff') and (Value[1] IS '\xfe'))
      return Value.substr(2); // UTF-16 BOM little endian
   return Value;
}

//********************************************************************************************************************

static bool has_script_extension(std::string_view Path, std::string_view Extension)
{
   return (Path.size() >= Extension.size()) and iequals(Path.substr(Path.size() - Extension.size()), Extension);
}

//********************************************************************************************************************

static std::string make_chunk_name(const extTiri *Self)
{
   const std::string &path = (Self->Path.empty() or Self->CompilationSourcePath.empty()) ?
      Self->Path : Self->CompilationSourcePath;
   if (path.empty()) return "=script";

   std::string chunk_name;
   chunk_name.reserve(path.size() + 1);
   chunk_name.push_back('@');
   chunk_name.append(path);
   return chunk_name;
}

//********************************************************************************************************************

static ERR read_open_file_to_string(objFile *File, int64_t Size, std::string &Buffer)
{
   if (not File) return ERR::NullArgs;
   if ((Size < 0) or (Size > int64_t(std::numeric_limits<int>::max()))) return ERR::OutOfRange;

   Buffer.resize(size_t(Size));
   int total = 0;
   while (total < Size) {
      int result = 0;
      auto output = std::span((int8_t *)Buffer.data() + total, size_t(Size - total));
      if (auto error = File->read(output, &result); error != ERR::Okay) {
         Buffer.clear();
         return error;
      }
      if (not result) {
         Buffer.clear();
         return ERR::Read;
      }
      total += result;
   }

   int64_t final_size = 0;
   if ((File->getSize(final_size) != ERR::Okay) or (final_size != Size)) {
      Buffer.clear();
      return ERR::Read;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// The build identity field always leads an identity token so that a consumer can verify the build without knowing
// whether the source content was recorded.

static std::string identity_build_field()
{
   return std::format("b:g:{}", TIRI_BUILD_COMMIT);
}

//********************************************************************************************************************
// Compose the identity token embedded in compiled output.  Source is omitted when the compilation unit did not
// originate from readable source text, in which case only the build identity can be verified on reload.

static std::string make_identity_token(const std::string *Source)
{
   if (Source) return std::format("{} s:{:x},{:08x}", identity_build_field(), Source->size(), kt::strhash(*Source));
   else return identity_build_field();
}

//********************************************************************************************************************
// Classify source, direct bytecode and selected cache input without changing Script provenance.

static ERR classify_compilation_input(std::string_view Source, CompilationInputOrigin Origin, CompilationInput &Input,
   std::string &Diagnostic)
{
   if (Origin IS CompilationInputOrigin::DIRECT_BYTECODE) {
      if (tiri::import_cache::is_envelope(Source)) {
         Diagnostic = "Import-module artefacts cannot be loaded directly; "
            "embed them in a complete root bytecode file.";
         return ERR::InvalidData;
      }
      tiri::cache::EnvelopeView envelope;
      if (tiri::cache::decode_envelope(Source, envelope) IS tiri::cache::FormatError::OKAY) {
         Input.OwnedPayload = std::move(envelope.Payload);
         Input.OwnsPayload = true;
         Input.Binary = true;
         Diagnostic.clear();
         return ERR::Okay;
      }
   }

   if (Origin IS CompilationInputOrigin::SELECTED_CACHE) {
      if (not Source.starts_with("\x1bLJ")) {
         Diagnostic = "Selected cache does not contain internal VM bytecode.";
         return ERR::InvalidData;
      }
      Input.OwnsPayload = false;
      Input.Binary = true;
      Diagnostic.clear();
      return ERR::Okay;
   }

   if (Source.starts_with(LUA_COMPILED)) {
      auto error = tiri::bytecode_storage::decode_wrapper(Source, Input.OwnedPayload);
      if (error != tiri::bytecode_storage::Error::OKAY) {
         Diagnostic = std::format("Invalid compiled Tiri wrapper ({}).",
            tiri::bytecode_storage::error_name(error));
         return ERR::InvalidData;
      }
      Input.OwnsPayload = true;
      Input.Binary = true;
      Diagnostic.clear();
      return ERR::Okay;
   }

   if (Origin IS CompilationInputOrigin::DIRECT_BYTECODE) {
      Diagnostic = "Persisted Tiri bytecode requires a compiled wrapper and gzip payload.";
      return ERR::InvalidData;
   }

   if (Source.starts_with("\x1bLJ")) {
      Input.OwnsPayload = false;
      Input.Binary = true;
      Diagnostic.clear();
      return ERR::Okay;
   }
   if (Source.starts_with("\x1f\x8b")) {
      Diagnostic = "A gzip member without a compiled Tiri wrapper is not executable.";
      return ERR::InvalidData;
   }

   Input.OwnsPayload = false;
   Input.Binary = false;
   Diagnostic.clear();
   return ERR::Okay;
}

//********************************************************************************************************************
// Load one compilation unit.  Failure restores the stack; success leaves exactly one executable function.

static ERR load_compilation_input(lua_State *Lua, extTiri *Self, std::string_view Source,
   CompilationInputOrigin Origin, std::string &Diagnostic)
{
   const int stack_top = lua_gettop(Lua);
   CompilationInput input;
   if (auto error = classify_compilation_input(Source, Origin, input, Diagnostic); error != ERR::Okay) return error;

   auto chunk_name = make_chunk_name(Self);
   const std::string_view payload = input.OwnsPayload ? std::string_view(input.OwnedPayload) : Source;
   const int result = lua_load(Lua, payload, chunk_name.c_str());
   if (result) {
      if (not input.Binary and Lua->parser_diagnostics and Lua->parser_diagnostics->has_errors()) {
         Diagnostic.clear();
         for (const auto &entry : Lua->parser_diagnostics->entries()) {
            if (not Diagnostic.empty()) Diagnostic += "\n";
            Diagnostic += entry.to_string(Self->LineOffset, Lua);
         }
      }
      else if (auto errorstr = lua_tostringview(Lua, -1); not errorstr.empty()) {
         Diagnostic.assign(errorstr.data(), errorstr.size());
      }
      else Diagnostic = input.Binary ? "Invalid compiled Tiri bytecode." : "Failed to compile Tiri source.";

      lua_settop(Lua, stack_top);
      return input.Binary ? ERR::InvalidData : ERR::Syntax;
   }

   Diagnostic.clear();
   return ERR::Okay;
}

static CompilationInputOrigin compilation_input_origin(const extTiri *Self)
{
   if (Self->LoadedFromCache) return CompilationInputOrigin::SELECTED_CACHE;
   if (Self->LoadedFromBytecodeFile and has_script_extension(Self->Path, ".tbc")) {
      return CompilationInputOrigin::DIRECT_BYTECODE;
   }
   return CompilationInputOrigin::SOURCE;
}

//********************************************************************************************************************
// Dump the variables of any global table

[[maybe_unused]] static void dump_global_table(extTiri *Self, STRING Global)
{
   kt::Log log("print_env");
   lua_State *lua = Self->Lua;
   lua_getglobal(lua, Global);
   if (lua_istable(lua, -1) ) {
      lua_pushnil(lua);
      while (lua_next(lua, -2) != 0) {
         int type = lua_type(lua, -2);
         log.msg("%s = %s", lua_tostring(lua, -2), lua_typename(lua, type));
         lua_pop(lua, 1);
      }
   }
}

//********************************************************************************************************************
// Only to be used immediately after a failed lua_pcall().  Lua stores a description of the error that occurred on the
// stack, this will be popped and copied to the ErrorMessage field.

void process_error(extTiri *Self, CSTRING Procedure)
{
   auto flags = VLF::WARNING;
   if (Self->Lua->CaughtError != ERR::Okay) {
      Self->Error = Self->Lua->CaughtError;
      if (Self->Error <= ERR::Terminate) flags = VLF::DETAIL; // Non-critical errors are muted to prevent log noise.
   }
   else Self->Error = ERR::Exception; // Unspecified exception, e.g. an error() or assert().  The result string will indicate detail.

   kt::Log log;
   auto str = lua_tostringview(Self->Lua, -1);
   Self->setErrorMessage(str);

   auto error_msg = str.empty() ? "" : str.data();
   if (Self->Lua->pending_exception_valid and Self->Lua->pending_exception_source) {
      log.msg(flags, "%.*s", int(str.size()), error_msg);
   }
   else if (not Self->Path.empty()) {
      auto file = std::string_view(Self->Path);
      auto i = file.find_last_of("/\\");
      if (i != std::string_view::npos) file.remove_prefix(i + 1);
      log.msg(flags, "%.*s: %.*s", int(file.size()), file.data(), int(str.size()), error_msg);
   }
   else log.msg(flags, "%s: Error: %.*s", Procedure, int(str.size()), error_msg);

   lua_pop(Self->Lua, 1);  // pop returned value

   // NB: CurrentLine is set by hook_debug(), so if debugging isn't active, you don't know what line we're on.

   if (Self->CurrentLine >= 0) {
      char line[60];
      get_line(Self, Self->CurrentLine, line, sizeof(line));
      log.msg(flags, "Line %d: %s...", Self->CurrentLine+1+Self->LineOffset, line);
   }
}

//********************************************************************************************************************
// This routine is intended for handling action notifications only.  It takes the FunctionField list provided by the
// action and copies them into a table.  Each value is represented by the relevant parameter name for ease of use.

static ERR stack_args(lua_State *Lua, OBJECTID ObjectID, const FunctionField *args, int8_t *Buffer)
{
   kt::Log log(__FUNCTION__);

   if (not args) return ERR::Okay;

   log.traceBranch("Args: %p, Buffer: %p", args, Buffer);

   int of = 0;
   for (int i=0; args[i].Name; i++) {
      std::string name(args[i].Name);
      std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });

      lua_pushlstring(Lua, name.c_str(), name.size());

      // Note: If the object is public and the call was messaged from a foreign process, all strings/pointers are
      // invalid because the message handlers cannot do deep pointer resolution of the structure we receive from
      // action notifications.

      if (args[i].Type & FD_STR) {
         if (sizeof(APTR) IS 8) of = ALIGN64(of);
         if (ObjectID > 0) lua_pushstring(Lua, ((STRING *)(Buffer + of))[0]);
         else lua_pushnil(Lua);
         of += sizeof(STRING);
      }
      else if (args[i].Type & FD_PTR) {
         if (sizeof(APTR) IS 8) of = ALIGN64(of);
         if (ObjectID > 0) lua_pushlightuserdata(Lua, ((APTR *)(Buffer + of))[0]);
         else lua_pushnil(Lua);
         of += sizeof(APTR);
      }
      else if (args[i].Type & FD_INT) {
         lua_pushinteger(Lua, ((int *)(Buffer + of))[0]);
         of += sizeof(int);
      }
      else if (args[i].Type & FD_DOUBLE) {
         if (sizeof(APTR) IS 8) of = ALIGN64(of);
         lua_pushnumber(Lua, ((double *)(Buffer + of))[0]);
         of += sizeof(double);
      }
      else if (args[i].Type & FD_INT64) {
         if (sizeof(APTR) IS 8) of = ALIGN64(of);
         lua_pushnumber(Lua, ((int64_t *)(Buffer + of))[0]);
         of += sizeof(int64_t);
      }
      else {
         log.warning("Unsupported arg %s, flags $%.8x, aborting now.", args[i].Name, args[i].Type);
         return ERR::UnrecognisedFieldType;
      }
      lua_settable(Lua, -3);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Action notifications arrive when the user has used object.subscribe() in the Tiri script.
//
// function(ObjectID, Args, Reference)

void notify_action(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extTiri *)CurrentContext();

   if (Result != ERR::Okay) return;

   for (auto &scan : Self->ActionList) {
      if ((Object->UID IS scan.ObjectID) and (ActionID IS scan.ActionID)) {
         LuaContextRootGuard context_guard(Self->Lua);
         int depth = GetResource(RES::LOG_DEPTH); // Required because thrown errors cause the debugger to lose its branch

         {
            kt::Log log;

            log.msg(VLF::BRANCH|VLF::DETAIL, "Action notification for object #%d, action %d.  Top: %d", Object->UID, int(ActionID), lua_gettop(Self->Lua));

            LuaCallbackContextGuard callback_context(Self->Lua);
            if (push_tiri_function(Self->Lua, scan.Function, callback_context) != ERR::Okay) {
               log.warning("Action subscription callback is no longer valid.");
               return;
            }
            push_object_id(Self->Lua, Object->UID);  // +1: Pass the object ID
            lua_newtable(Self->Lua);  // +1: Table to store the parameters

            if ((scan.Args) and (Args)) {
               stack_args(Self->Lua, Object->UID, scan.Args, (int8_t *)Args);
            }

            int total_args = 2;

            if (scan.Reference) { // +1: Custom reference (optional)
               lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, scan.Reference);
               total_args++; // ObjectID, ArgTable, Reference
            }

            if (lua_pcall(Self->Lua, total_args, 0, 0)) { // Make the call, function & args are removed from stack.
               process_error(Self, "Action Subscription");
            }

         }

         SetResource(RES::LOG_DEPTH, depth);

         if (ActionID IS AC::Free) {
            std::erase_if(Self->ActionList, [&](auto &item) {
               if (item.ObjectID IS Object->UID) {
                  if (item.Function.defined()) {
                     release_tiri_function(Self->Lua, &item.Function);
                  }
                  if (item.Reference) {
                     luaL_unref(Self->Lua, LUA_REGISTRYINDEX, item.Reference);
                     item.Reference = 0;
                  }

                  // The object is already being destroyed, so suppress destructor-time unsubscribe attempts.
                  item.ObjectID = 0;
                  return true;
               }
               else return false;
            });
         }

         collect_garbage(Self->Lua);

         return;
      }
   }
}

//********************************************************************************************************************

static ERR TIRI_Activate(extTiri *Self)
{
   kt::Log log;

   if (Self->Statement.empty() and not Self->LoadedFromCache and not has_script_extension(Self->Path, ".tbc")) {
      return log.warning(ERR::FieldNotSet);
   }

   log.trace("Target: %d, Procedure: %s / ID #%u", Self->TargetID,
      Self->Procedure.empty() ? "." : Self->Procedure.c_str(), FUNCTION::unpackProcedureID(Self->ProcedureID));

   if ((Self->Recurse) and (Self->Procedure.empty()) and (not Self->ProcedureID)) {
      return ERR::Okay; // Do nothing, script is running.
   }

   Self->CurrentLine = -1;
   Self->Error       = ERR::Okay;
   if (auto error = acQuery(Self); error <= ERR::ExceptionThreshold) {
      Self->Recurse++;

      if ((Self->JitOptions & JOF::DISABLE_JIT) != JOF::NIL) {
         luaJIT_setmode(Self->Lua, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
      }

      if ((not Self->Procedure.empty()) or (Self->ProcedureID)) {
         // The Lua script needs to have been executed at least once in order for the procedures to be initialised and recognised.

         if (Self->ActivationCount IS 0) {
            kt::Log log;
            log.traceBranch("Collecting functions prior to procedure call...");

            if (lua_pcall(Self->Lua, 0, 0, 0)) {
               process_error(Self, "Activation");
               if (!(error = Self->Error)) error = ERR::Exception;
            }
         }
      }

      Self->ActivationCount++;

      if (!Self->Error) run_script(Self); // Will set Self->Error if there's an issue

      Self->Recurse--;

      // Automated garbage collection runs for initial activations only, in order to ensure that any temporary
      // objects don't persist in memory.  After that, the script is expected to manage its own memory usage.

      collect_garbage(Self->Lua, Self->ActivationCount <= 2);

      return ERR::Okay; // The error reflects on the initial processing of the script only - the developer must check the Error field for information on script execution
   }
   else { // Failure during parsing
      Self->Error = error;
      return error;
   }
}

//********************************************************************************************************************

static ERR TIRI_DataFeed(extTiri *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if (not Args) return ERR::NullArgs;

   if ((Args->Datatype IS DATA::TEXT) or (Args->Datatype IS DATA::XML)) {
      Self->setStatement(std::string_view((const char *)Args->Buffer.data(), Args->Buffer.size()));
   }
   else if (Args->Datatype IS DATA::RECEIPT) {
      log.branch("Incoming data receipt from #%d", Args->Object ? Args->Object->UID : 0);

      for (auto it = Self->Requests.begin(); it != Self->Requests.end(); ) {
         if ((Args->Object) and (it->SourceID IS Args->Object->UID)) {
            LuaContextRootGuard context_guard(Self->Lua);
            // Execute the callback associated with this input subscription: function({Items...})

            int step = GetResource(RES::LOG_DEPTH); // Required as thrown errors cause the debugger to lose its step position

            {
               LuaCallbackContextGuard callback_context(Self->Lua);
               if (push_tiri_function(Self->Lua, it->Callback, callback_context) != ERR::Okay) {
                  SetResource(RES::LOG_DEPTH, step);
                  release_tiri_function(Self->Lua, &it->Callback);
                  it = Self->Requests.erase(it);
                  continue;
               }
               lua_newtable(Self->Lua); // +1 Item table

               if (auto xml = objXML::create::local(fl::Statement(
                     std::string_view((const char *)Args->Buffer.data(), Args->Buffer.size())))) {
                  // <file path="blah.exe"/> becomes { item='file', path='blah.exe' }

                  if (not xml->Tags.empty()) {
                     auto &tag = xml->Tags[0];
                     int i = 0;
                     if (iequals("receipt", tag.name())) {
                        for (auto &scan : tag.Children) {
                           lua_pushinteger(Self->Lua, i++);
                           lua_newtable(Self->Lua);

                           lua_pushstring(Self->Lua, "item");
                           lua_pushstring(Self->Lua, scan.name());
                           lua_settable(Self->Lua, -3);

                           for (unsigned a=1; a < scan.Attribs.size(); a++) {
                              lua_pushstring(Self->Lua, scan.Attribs[a].Name.c_str());
                              lua_pushstring(Self->Lua, scan.Attribs[a].Value.c_str());
                              lua_settable(Self->Lua, -3);
                           }

                           lua_settable(Self->Lua, -3);
                        }
                     }
                  }

                  FreeResource(xml);

                  if (lua_pcall(Self->Lua, 1, 0, 0)) { // function(Items)
                     process_error(Self, "Data Receipt Callback");
                  }
               }
               else lua_pop(Self->Lua, 2);
            }

            SetResource(RES::LOG_DEPTH, step);

            release_tiri_function(Self->Lua, &it->Callback);
            it = Self->Requests.erase(it);
            continue;
         }
         it++;
      }

      collect_garbage(Self->Lua);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

extTiri::~extTiri()
{
   if (FocusEventHandle) { UnsubscribeEvent(FocusEventHandle); FocusEventHandle = nullptr; }

   auto lua = Lua;
   Lua = nullptr; // Release the Lua state now because the Free action manager can reference it on return
   if (lua) lua_close(lua);
}

//********************************************************************************************************************

#include "packaging/cache.cpp"

//********************************************************************************************************************

static ERR TIRI_Init(extTiri *Self)
{
   kt::Log log;

   // Cache selection and source compilation must see the same process-wide and Script-local effective options.
   Self->LocalJitOptions = Self->JitOptions;
   Self->GlobalJitOptions = glJitOptions;
   Self->JitOptions |= Self->GlobalJitOptions;

   if (not Self->Path.empty()) {
      if (Self->Path.starts_with("string:") or Self->Path.starts_with("STRING:")); // Assume Tiri for string paths
      else if (not (has_script_extension(Self->Path, ".tiri") or has_script_extension(Self->Path, ".tbc"))) {
         log.warning("Path extension not recognised for '%s'", Self->Path.c_str());
         return ERR::NoSupport;
      }
   }

   if ((Self->defined(NF::RECLASSED)) and (Self->Statement.empty())) {
      log.trace("No support for reclassed Script with no String field value.");
      return ERR::NoSupport;
   }

   if ((Self->Statement.empty()) and (not Self->Path.empty())) {
      if (auto error = prepare_cached_input(Self); error != ERR::Okay) return log.warning(error);
   }

   if (not (Self->Lua = luaL_newstate(Self))) {
      log.warning("Failed to open a Lua instance.");
      return ERR::CreateResource;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// If the script is being executed, retarget the new resource to refer to the current task (because we don't want
// client resources allocated by the script to be automatically destroyed when the script is terminated by the client).

static ERR TIRI_NewChild(extTiri *Self, struct acNewChild &Args)
{
   if (Self->Recurse) {
      SetOwner(Args.Object, CurrentTask());
      return ERR::OwnerPassThrough;
   }
   else return ERR::Okay;
}

//********************************************************************************************************************
// Initialise the standard compilation environment in a Lua state without copying any values from another state.

ERR initialise_tiri_compilation_state(lua_State *Lua)
{
   lua_gc(Lua, LUA_GCSTOP, 0);  // Stop collector during initialisation
      luaL_openlibs(Lua);  // Open Lua libraries
   lua_gc(Lua, LUA_GCRESTART, 0);

   // Register private variables in the registry, which is tamper proof from the user's Lua code.

   if (auto error = register_interfaces(Lua); error != ERR::Okay) return error;

   // 'mSys' is a compiler-managed namespace for Core rather than a global value, so no module object is created
   // here.  The compiler materialises Core's callables as hidden locals in any compilation unit that uses them.

   lua_protect_globals(Lua);
   return ERR::Okay;
}

//********************************************************************************************************************
// Prepare the script's execution state once, including when compilation is retried.

static ERR prepare_compilation(extTiri *Self)
{
   if (not Self->Lua) return ERR::NotInitialised;
   if (Self->CompilationPrepared) return ERR::Okay;

   if (auto error = initialise_tiri_compilation_state(Self->Lua); error != ERR::Okay) return error;

   // Line hook, executes on the execution of a new line (doesn't execute during Query() compilation)

   if ((Self->Flags & SCF::LOG_ALL) != SCF::NIL) {
      // LUA_MASKLINE:  Interpreter is executing a line.
      // LUA_MASKCALL:  Interpreter is calling a function.
      // LUA_MASKRET:   Interpreter returns from a function.
      // LUA_MASKCOUNT: The hook runs every X instructions (set to 1 for exactness).

      lua_sethook(Self->Lua, hook_debug, LUA_MASKCALL|LUA_MASKRET|LUA_MASKLINE, 0);
   }

   Self->CompilationPrepared = true;
   return ERR::Okay;
}

//********************************************************************************************************************
// A failed load leaves the stack unchanged; success leaves one executable function.

static ERR load_statement(extTiri *Self)
{
   Self->CompilationManifest.reset();

   std::unique_ptr<tiri::cache::Manifest> capture;
   const auto origin = compilation_input_origin(Self);
   if ((origin IS CompilationInputOrigin::SOURCE) and not Self->CompilationSourcePath.empty()) {
      capture = std::make_unique<tiri::cache::Manifest>();
      capture->BuildIdentity = TIRI_BUILD_COMMIT;
      capture->MainSource.ResolvedPath = Self->CompilationSourcePath;
      capture->MainSource.Size = Self->Statement.size();
      capture->MainSource.ModifiedHint = Self->SourceModifiedHint;
      capture->MainSource.ContentDigest = tiri::cache::content_digest(Self->Statement);
      capture->Options = cache_compilation_options(Self);
   }

   struct CaptureGuard {
      lua_State *Lua;
      ~CaptureGuard() { Lua->cache_manifest_capture = nullptr; }
   } capture_guard { Self->Lua };
   Self->Lua->cache_manifest_capture = capture.get();

   std::string diagnostic;
   if (origin IS CompilationInputOrigin::SOURCE) Self->SourceCompilationCount++;
   auto error = load_compilation_input(Self->Lua, Self, Self->Statement, origin, diagnostic);
   if (error IS ERR::Okay) Self->CompilationManifest = std::move(capture);
   Self->setErrorMessage(diagnostic);
   return error;
}

/*********************************************************************************************************************

-ACTION-
Query: Compiles the script and prepares it for execution.

Query() performs the process of compiling the Tiri script.  A Tiri script can be queried only once, at which point
the object enters a configured state.   Further queries will do nothing, and return `ERR::NothingDone`.

Note that due to the code not being run, declared functions and variables won't be formally registered.
Introspection of available procedures will be limited until the script is activated.

*********************************************************************************************************************/

static ERR TIRI_Query(extTiri *Self)
{
   kt::Log log;
   if (auto error = refresh_cache_lifecycle(Self); error != ERR::Okay) return log.warning(error);

   if (Self->Statement.empty() and not Self->LoadedFromCache and not has_script_extension(Self->Path, ".tbc")) {
      return log.warning(ERR::FieldNotSet);
   }
   if (Self->Recurse) return ERR::NothingDone;

   if (not Self->MainChunkRef) {
      if (auto error = prepare_compilation(Self); error != ERR::Okay) return error;

      std::optional<std::string> cache_fallback_source;
      if (auto error = prepare_query_cache(Self, cache_fallback_source); error != ERR::Okay) return error;

      auto error = load_statement_with_cache_fallback(Self, cache_fallback_source);
      if (error != ERR::Okay) {
         log.warning("%s", Self->ErrorMessage.c_str());
         return error;
      }

      lua_pushvalue(Self->Lua, -1);
      Self->MainChunkRef = luaL_ref(Self->Lua, LUA_REGISTRYINDEX);

      publish_pending_cache(Self);

      return ERR::Okay;
   }
   else return ERR::NothingDone; // Script already compiled
}

/*********************************************************************************************************************

-ACTION-
SaveToObject: Compiles the current script statement and saves it as byte code.

Use the SaveToObject action to compile @Script.Statement and save the resulting byte code to a target object without
executing the program.  Each save compiles the statement afresh, including after #Query() or #Activate(), and
preserves any pending executable chunk.  Saving during active execution returns `ERR::InvalidState`.

Source statements, wrapped byte code and caller-supplied raw VM byte code can be saved.  Persisted `.tbc` input must
use the wrapped gzip format; bare VM byte code and unwrapped gzip data are rejected.  A rejected selected cache
receives one local source fallback during each save, without consuming the later Query fallback or changing cache
provenance.  A direct `.tbc` file is authoritative and never falls back to another file.

The output contains the Tiri compiled marker, an identity token, a NUL separator and gzip-compressed VM byte code with
debug information.  The token records the build that produced the output, and is informational here because a
caller-owned destination is authoritative when it is loaded.  Automatic caches record a source identity alongside it
and are validated on reload.  Byte code is platform-agnostic across supported 64-bit Kōtuku targets, but requires a
compatible Tiri bytecode ABI and the referenced runtime interfaces.  Runtime state is not saved.  Required locally
named struct definitions, imported declarations, source identities and diagnostic line mappings are embedded in the
output; unused struct declarations and source text are not embedded.

Loading publishes embedded layouts in the consumer state for its lifetime.  An identical existing declaration is
reused, while a conflicting declaration rejects the complete load without changing the prior registry.  Save
compilation is isolated from the execution state, so rejected and repeated saves do not change imports, declarations,
diagnostics, captures or a pending executable chunk.  To-be-closed locals are preserved in saved byte code.

A failed save can leave partial output.

*********************************************************************************************************************/

static ERR TIRI_SaveToObject(extTiri *Self, struct acSaveToObject *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Dest)) return log.warning(ERR::NullArgs);

   if (Self->Statement.empty() and not Self->LoadedFromCache and not has_script_extension(Self->Path, ".tbc")) {
      return log.warning(ERR::FieldNotSet);
   }

   if (not Self->Lua) return ERR::NotInitialised;
   if (Self->Recurse) return ERR::InvalidState;

   const int stack_top = lua_gettop(Self->Lua);
   const int main_chunk_ref = Self->MainChunkRef;

   // The parser intentionally keeps imports, source maps and declarations in a state for runtime loadFile() calls.
   // Saving is speculative, so compile in a fresh state rather than trying to roll back every Lua-owned registration.

   std::unique_ptr<lua_State, decltype(&lua_close)> compilation(luaL_newstate(Self), lua_close);
   if (not compilation) return ERR::CreateResource;
   if (auto error = initialise_tiri_compilation_state(compilation.get()); error != ERR::Okay) return error;

   struct CaptureRestore {
      extTiri *Script;
      std::vector<VariableInfo> Saved;
      ~CaptureRestore() { Script->CapturedVariables = std::move(Saved); }
   } capture_restore { Self, std::move(Self->CapturedVariables) };

   log.branch("Loading the statement for saving...");

   const auto origin = compilation_input_origin(Self);
   std::string diagnostic;
   ERR error = load_compilation_input(compilation.get(), Self, Self->Statement, origin, diagnostic);

   // A selected cache is replaceable.  Keep this fallback local so saving cannot consume or alter Query's state.

   std::string source;
   if ((error != ERR::Okay) and (origin IS CompilationInputOrigin::SELECTED_CACHE)) {
      objFile::create source_file = { fl::Path(Self->Path), fl::Flags(FL::READ) };
      if (source_file.ok() and (read_source_file(*source_file, Self->Path, source) IS ERR::Okay)) {
         error = load_compilation_input(compilation.get(), Self, source, CompilationInputOrigin::SOURCE, diagnostic);
      }
   }

   // Saved output records the build identity only.  A caller-owned destination is authoritative on load, so a source
   // identity would imply a content comparison that is never performed for it.

   if (error IS ERR::Okay) error = save_binary(compilation.get(), Args->Dest, make_identity_token(nullptr));
   else {
      Self->setErrorMessage(diagnostic);
      log.warning("Save load failure: %s", diagnostic.c_str());
      if (error IS ERR::Syntax) error = ERR::InvalidData;
   }

   if (error IS ERR::Okay) Self->setErrorMessage("");
   if ((lua_gettop(Self->Lua) != stack_top) or (Self->MainChunkRef != main_chunk_ref)) return ERR::InvalidState;
   return error;
}

/*********************************************************************************************************************

-FIELD-
JitOptions: Defines JIT debugging options.

This field allows the client to configure debugging options related to the Just-In-Time (JIT) compilation process.

-END-

*********************************************************************************************************************/

static ERR GET_JitOptions(extTiri *Self, JOF *Value)
{
   *Value = Self->JitOptions;
   return ERR::Okay;
}

static ERR SET_JitOptions(extTiri *Self, JOF Value)
{
   if (Self->Recurse) {
      kt::Log().warning("Changing JIT options after parsing is ineffective.");
      return ERR::InvalidState;
   }
   if (Self->Lua) {
      Self->LocalJitOptions = Value;
      Self->JitOptions = Value|Self->GlobalJitOptions;
   }
   else Self->JitOptions = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Procedures: Returns a string array of all named procedures defined by a script.

This field will return a string array of all procedures loaded into the script, conditional on it being activated.
It will otherwise return an empty array.
-END-

*********************************************************************************************************************/

static ERR GET_Procedures(extTiri *Self, std::span<std::string> &Value)
{
   if (Self->Lua) {
      Self->Procedures.clear();
      lua_pushnil(Self->Lua);
      while (lua_next(Self->Lua, LUA_GLOBALSINDEX)) {
         if (lua_type(Self->Lua, -1) IS LUA_TFUNCTION) {
            if (auto name = lua_tostringview(Self->Lua, -2); not name.empty()) {
               Self->Procedures.emplace_back(name);
            }
         }
         lua_pop(Self->Lua, 1);
      }

      Value = std::span<std::string>(Self->Procedures.data(), Self->Procedures.size());
      return ERR::Okay;
   }
   else return ERR::NotInitialised;
}

//********************************************************************************************************************
// The top stack entry must be a compiled Tiri function.  Preserve the stack and report success only after every
// wrapper and VM byte has been written.  The initial compatibility contract is the same build and platform.

struct BytecodeWriter {
   OBJECTPTR Destination;
   ERR Error = ERR::Okay;
};

static int write_bytecode(lua_State *, const void *Data, size_t Size, void *Context)
{
   auto &writer = *((BytecodeWriter *)Context);
   auto data = (const int8_t *)Data;
   while (Size and (writer.Error IS ERR::Okay)) {
      const auto count = std::min(Size, size_t(std::numeric_limits<int>::max()));
      int written = 0;
      writer.Error = acWrite(writer.Destination, std::span<const int8_t>(data, count), &written);
      if ((writer.Error <= ERR::ExceptionThreshold) and
          ((writer.Error != ERR::Okay) or (written != int(count)))) writer.Error = ERR::Write;
      if (writer.Error != ERR::Okay) break;
      data += count;
      Size -= count;
   }
   return writer.Error IS ERR::Okay ? 0 : 1;
}

//********************************************************************************************************************

static ERR save_binary(lua_State *Lua, OBJECTPTR Target, std::string_view Token)
{
   if ((not Lua) or (not Target)) return ERR::NullArgs;

   if ((not lua_gettop(Lua)) or (not lua_isfunction(Lua, -1)) or lua_iscfunction(Lua, -1)) return ERR::InvalidData;
   if ((not Token.empty()) and (Token.size() + 1 > tiri::bytecode_storage::MAX_IDENTITY_TOKEN)) {
      return ERR::BufferOverflow;
   }

   std::string header(LUA_COMPILED);
   if (not Token.empty()) {
      header.push_back(' ');
      header.append(Token);
   }
   header.push_back('\0');

   const int stack_top = lua_gettop(Lua);
   BytecodeWriter writer { Target };
   if (write_bytecode(Lua, header.data(), header.size(), &writer)) return writer.Error;

   objCompressedStream::create compressed(NF::LOCAL);
   if (not compressed.ok() or (compressed->setOutput(Target) != ERR::Okay) or
       (compressed->setFormat(CF::GZIP) != ERR::Okay) or (compressed->init() != ERR::Okay)) {
      return ERR::Compression;
   }

   writer.Destination = *compressed;
   const int result = lua_dump(Lua, write_bytecode, &writer);
   lua_settop(Lua, stack_top);
   if (writer.Error != ERR::Okay) return writer.Error;
   if (result) return ERR::InvalidData;
   if (auto error = compressed->write(std::span<const int8_t>()); error != ERR::Okay) return error;
   if (not compressed->Finished) return ERR::Compression;
   if ((compressed->TotalInput < 0) or (compressed->TotalOutput < 0)) return ERR::Compression;
   if ((uint64_t(compressed->TotalInput) > tiri::bytecode_storage::MAX_DECODED_SIZE) or
       (uint64_t(compressed->TotalOutput) > tiri::bytecode_storage::MAX_ENCODED_SIZE)) return ERR::BufferOverflow;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR run_script(extTiri *Self)
{
   kt::Log log(__FUNCTION__);
   auto procedure_id = FUNCTION::unpackProcedureID(Self->ProcedureID);
   auto context_id = FUNCTION::unpackContextID(Self->ProcedureID);
   bool native_callback = Self->Procedure.empty() and bool(Self->ProcedureID);
   std::optional<LuaCallbackContextGuard> callback_context;

   if (native_callback) {
      callback_context.emplace(Self->Lua);
      if (context_id != 0) {
         lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, context_id);
         if (lua_type(Self->Lua, -1) != LUA_TTABLE) {
            lua_pop(Self->Lua, 1);
            auto message = std::format("Callback context #{} does not exist or is not a table.", context_id);
            Self->setErrorMessage(message.c_str());
            Self->Error = ERR::InvalidData;
            return Self->Error;
         }

         callback_context->activate(tabV(Self->Lua->top - 1));
         lua_pop(Self->Lua, 1);
      }
   }

   log.traceBranch("Procedure: %s, Top: %d", Self->Procedure.c_str(), lua_gettop(Self->Lua));

   Self->Lua->CaughtError = ERR::Okay;
   std::array<GCobject*, 8> release_list;
   size_t r = 0;
   int top;
   bool pcall_failed = false;
   if ((not Self->Procedure.empty()) or (Self->ProcedureID)) {
      if (not Self->Procedure.empty()) lua_getglobal(Self->Lua, Self->Procedure);
      else lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, int(procedure_id));

      if (lua_isfunction(Self->Lua, -1)) {
         if ((Self->Flags & SCF::LOG_ALL) != SCF::NIL) {
            log.branch("Executing procedure: %s, Args: %d", Self->Procedure.c_str(), Self->TotalArgs);
         }

         top = lua_gettop(Self->Lua);

         int count = 0;
         const ScriptArg *args;
         if ((args = Self->ProcArgs)) {
            for (int i=0; i < Self->TotalArgs; i++, args++) {
               int type = args->Type;

               if ((type & FDF_SPAN) IS FDF_SPAN) {
                  auto span = (std::span<std::byte> *)args->Address;
                  if (span) lua_createarray(Self->Lua, span->size(), AET::BYTE, span->data(), ARRAY_EXTERNAL);
                  else lua_pushnil(Self->Lua);
               }
               else if (type & (FD_ARRAY|FD_VECTOR)) {
                  log.trace("Setting arg '%s', Array: %p", args->Name, args->Address);

                  APTR values = args->Address;
                  std::string_view arg_name(args->Name);

                  if (values) {
                     make_any_array(Self->Lua, type, arg_name, -1, values);

                     if (type & FD_ALLOC) FreeResource(values);
                  }
                  else lua_pushnil(Self->Lua);
               }
               else if (type & FD_STR) {
                  log.trace("Setting arg '%s', Value: %.20s", args->Name, (CSTRING)args->Address);
                  lua_pushstring(Self->Lua, (CSTRING)args->Address);
               }
               else if (type & FD_STRUCT) {
                  // Pointer to a struct, which can be referenced with a name of "StructName" or "StructName:ArgName"
                  if (args->Address) {
                     if (named_struct_to_table(Self->Lua, args->Name, args->Address) != ERR::Okay) lua_pushnil(Self->Lua);
                     if (type & FD_ALLOC) FreeResource(args->Address);
                  }
                  else lua_pushnil(Self->Lua);
               }
               else if (type & FD_PTR) {
                  // Try and make the pointer safer/more usable by translating it into a buffer, object ID or whatever.
                  // (In a secure environment, pointers may be passed around but may be useless if their use is
                  // disallowed within Lua).

                  log.trace("Setting arg '%s', Value: %p", args->Name, args->Address);
                  if (type & FD_OBJECT) {
                     // Pushing direct object pointers is considered safe because they are treated as detached, then
                     // a lock is gained for the duration of the call that is then released on return.  This is a
                     // solid optimisation that also protects the object from unwarranted termination during the call.

                     if (args->Address) {
                        GCobject *obj = push_object(Self->Lua, (OBJECTPTR)args->Address);
                        OBJECTPTR ptr_obj;
                        if ((r < release_list.size()) and (access_object(obj, ptr_obj) IS ERR::Okay)) {
                           release_list[r++] = obj;
                        }
                     }
                     else lua_pushnil(Self->Lua);
                  }
                  else lua_pushlightuserdata(Self->Lua, args->Address);
               }
               else if (type & FD_INT)   {
                  log.trace("Setting arg '%s', Value: %d", args->Name, args->Int);
                  if (type & FD_OBJECT) {
                     if (args->Int) push_object_id(Self->Lua, args->Int);
                     else lua_pushnil(Self->Lua);
                  }
                  else lua_pushinteger(Self->Lua, args->Int);
               }
               else if (type & FD_INT64)  { log.trace("Setting arg '%s', Value: %" PRId64, args->Name, (long long)args->Int64); lua_pushnumber(Self->Lua, args->Int64); }
               else if (type & FD_DOUBLE) { log.trace("Setting arg '%s', Value: %.2f", args->Name, args->Double); lua_pushnumber(Self->Lua, args->Double); }
               else { lua_pushnil(Self->Lua); log.warning("Arg '%s' uses unrecognised type $%.8x", args->Name, type); }
               count++;
            }
         }

         int step = GetResource(RES::LOG_DEPTH);

         if (lua_pcall(Self->Lua, count, LUA_MULTRET, 0)) {
            pcall_failed = true;
         }

         SetResource(RES::LOG_DEPTH, step);

         while (r > 0) release_object(release_list[--r]);
      }
      else {
         auto str = std::format("Procedure '{}' / #{} does not exist in the script.",
            Self->Procedure.empty() ? "NULL" : Self->Procedure.c_str(), procedure_id);
         Self->setErrorMessage(str.c_str());
         log.warning("%s", str.c_str());

         #ifndef NDEBUG
            std::span<std::string> list;
            if (!GET_Procedures(Self, list)) {
               for (unsigned i=0; i < list.size(); i++) log.trace("%s", list[i].c_str());
            }
         #endif

         Self->Error = ERR::NotFound;
         return ERR::NotFound;
      }
   }
   else {
      if (Self->ActivationCount > 1) {
         // Re-execution: restore the compiled main chunk from the registry reference since the previous
         // lua_pcall() consumed the function from the stack.
         lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, Self->MainChunkRef);
      }

      int depth = GetResource(RES::LOG_DEPTH);

         top = lua_gettop(Self->Lua);
         if (lua_pcall(Self->Lua, 0, LUA_MULTRET, 0)) pcall_failed = true;

      SetResource(RES::LOG_DEPTH, depth);
   }

   if (not pcall_failed) { // If the procedure returned results, copy them to the Results field of the Script.
      int results = lua_gettop(Self->Lua) - top + 1;

      ERR error = ERR::Okay;
      if (results > 0) {
         kt::vector<std::string> array;
         array.resize(results);
         for (int i=0; i < results; i++) {
            size_t size;
            auto str = lua_tolstring(Self->Lua, -results+i, &size);
            if (str) array[i] = std::string_view(str, size);
            else Self->Error = error = ERR::LimitedSuccess;
         }
         Self->setResults(array);
         lua_pop(Self->Lua, results);  // pop returned values
      }

      // Flush pending messages before returning (critical - some message handlers may assume that
      // pointers to the script object are valid, so flushing ensures safety).

      if ((Self->Recurse IS 1) and GetResource(RES::MAIN_THREAD)) {
         ProcessMessages(PMF::NIL, 0);
      }

      return error;
   }
   else {
      // LuaJIT catches C++ exceptions, but we would prefer that crashes occur normally so that they can be traced in
      // the debugger.  As we don't have a solution to this design issue yet, the following context check will suffice
      // to prevent unwanted behaviour.

      if (CurrentContext() != Self) abort(); // A C++ exception was caught by Lua - the software stack is unstable so we must abort.

      if ((Self->Recurse IS 1) and GetResource(RES::MAIN_THREAD)) {
         ProcessMessages(PMF::NIL, 0);
      }

      process_error(Self, Self->Procedure.empty() ? "run_script" : Self->Procedure.c_str());
      return Self->Error;
   }
}

//********************************************************************************************************************

static ERR register_interfaces(lua_State *Lua)
{
   kt::Log log;

   log.traceBranch("Registering Kotuku and Tiri interfaces with Lua.");

#ifndef NDEBUG
   int stack_top = lua_gettop(Lua);
#endif

   register_io_class(Lua);
   register_module_class(Lua);
   register_regex_class(Lua);
   register_async_class(Lua);
#ifndef DISABLE_DISPLAY
   register_input_class(Lua);
#endif
   register_processing_class(Lua);

   lua_register(Lua, "arg", fcmd_arg);
   lua_register(Lua, "loadFile", fcmd_loadfile);
   lua_register(Lua, "exec", fcmd_exec);
   lua_register(Lua, "print", fcmd_print);
   lua_register(Lua, "msg", fcmd_msg);
   lua_register(Lua, "subscribeEvent", fcmd_subscribe_event);
   lua_register(Lua, "unsubscribeEvent", fcmd_unsubscribe_event);
   lua_register(Lua, "MAKESTRUCT", MAKESTRUCT);

   // Register global function prototypes for compile-time type inference
   reg_func_prototype("arg", { TiriType::Str }, { TiriType::Str, TiriType::Str }, FProtoFlags::None,
      FProtoArity::required(1));
   reg_func_prototype("loadFile", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("exec", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("print", {}, {}, FProtoFlags::Variadic);
   reg_func_prototype("msg", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("subscribeEvent", { TiriType::Num, TiriType::Userdata }, { TiriType::Str, TiriType::Func });
   reg_func_prototype("unsubscribeEvent", {}, { TiriType::Userdata });
   reg_func_prototype("MAKESTRUCT", { TiriType::Any }, { TiriType::Str }, FProtoFlags::Variadic);

   if (auto error = load_module_defs("core"); error != ERR::Okay) {
      log.error("Failed to process the core includes.");
      return error;
   }

   // Every built-in prototype has now been published, so the registry contents are final and it can be sealed for
   // lock-free lookups.  This is the last registration point: luaL_openlibs() publishes the core libraries and the
   // calls above add the remaining interfaces.  Later states repeat the same registrations, which validate against
   // the new state and return ERR::Exists without mutating the maps.
   seal_proto_registry();

#ifndef NDEBUG
   int stack_delta = lua_gettop(Lua) - stack_top;
   if (stack_delta) log.warning("Lua initialisation left %d value(s) on the Lua stack.", stack_delta);
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

#include "tiri_class_methods.cpp"

#include "tiri_class_def.cpp"

static ERR GET_JitOptions(extTiri *, JOF *);
static ERR SET_JitOptions(extTiri *, JOF);
static ERR GET_Procedures(extTiri *, std::span<std::string> &);

static const FieldArray clFields[] = {
   { "JitOptions", FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW|FDF_PURE, GET_JitOptions, SET_JitOptions, &clTiriJOF },
   { "Procedures", FDF_VIRTUAL|FDF_VECTOR|FDF_CPPSTRING|FDF_R, GET_Procedures },
   END_FIELD
};

//********************************************************************************************************************

ERR create_tiri(void)
{
   clTiri = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::SCRIPT),
      fl::ClassID(CLASSID::TIRI),
      fl::ClassVersion(1.0),
      fl::Name("Tiri"),
      fl::Category(CCF::DATA),
      fl::FileExtension("tiri|tbc"),
      fl::FileDescription("Tiri"),
      fl::Actions(clTiriActions),
      fl::Methods(clMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extTiri)),
      fl::Path(MOD_PATH));

   return clTiri ? ERR::Okay : ERR::AddClass;
}
