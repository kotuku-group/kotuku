/*********************************************************************************************************************

-CLASS-
Tiri: Extends the Script class with support for the Tiri language.

The Tiri class provides functionality for running scripts written in the Tiri programming language.

-END-

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "lua.hpp"

#include "lj_obj.h"
#include "parser/parser_diagnostics.h"
#include "jit/src/debug/dump_bytecode.h"
#include "lj_proto_registry.h"

#include "defs.h"

static ERR run_script(extTiri *);
static ERR stack_args(lua_State *, OBJECTID, const FunctionField *, int8_t *);
static ERR save_binary(extTiri *, OBJECTPTR);

[[maybe_unused]] constexpr std::string_view check_bom(std::string_view Value)
{
   if ((Value.size() >= 3) and (Value[0] IS '\xef') and (Value[1] IS '\xbb') and (Value[2] IS '\xbf'))
      return Value.substr(3); // UTF-8 BOM
   if ((Value.size() >= 2) and (Value[0] IS '\xfe') and (Value[1] IS '\xff'))
      return Value.substr(2); // UTF-16 BOM big endian
   if ((Value.size() >= 2) and (Value[0] IS '\xff') and (Value[1] IS '\xfe'))
      return Value.substr(2); // UTF-16 BOM little endian
   return Value;
}

static std::string make_chunk_name(const extTiri *Self)
{
   if (Self->Path.empty()) return "=script";

   std::string chunk_name;
   chunk_name.reserve(Self->Path.size() + 1);
   chunk_name.push_back('@');
   chunk_name.append(Self->Path);
   return chunk_name;
}

static ERR read_file_to_string(const std::string_view &Path, int64_t Size, std::string &Buffer, int *BytesRead)
{
   if ((Size < 0) or (Size > int64_t(std::numeric_limits<int>::max()))) return ERR::OutOfRange;

   if (Size IS 0) {
      Buffer.clear();
      if (BytesRead) *BytesRead = 0;
      return ERR::Okay;
   }

   int read_size = int(Size);
   int bytes_read = 0;
   Buffer.resize(read_size);

   auto error = ReadFileToBuffer(Path, Buffer.data(), read_size, &bytes_read);
   if (!error) {
      Buffer.resize(bytes_read);
      if (BytesRead) *BytesRead = bytes_read;
   }
   else Buffer.clear();

   return error;
}

static ERR compiled_payload(std::string_view Source, std::string_view &Payload)
{
   auto header_len = std::string_view(LUA_COMPILED).size();
   auto payload_offset = Source.find('\0', header_len);
   if (payload_offset IS std::string_view::npos) return ERR::InvalidData;

   Payload = Source.substr(payload_offset + 1);
   return ERR::Okay;
}

[[maybe_unused]] static ERR register_interfaces(extTiri *);

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

static ERR TIRI_Activate(extTiri *);
static ERR TIRI_DataFeed(extTiri *, struct acDataFeed *);
static ERR TIRI_Init(extTiri *);
static ERR TIRI_NewChild(extTiri *, struct acNewChild &);
static ERR TIRI_Query(extTiri *);
static ERR TIRI_SaveToObject(extTiri *, struct acSaveToObject *);

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
   if (not Self->Path.empty()) {
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
         return ERR::Failed;
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
         int depth = GetResource(RES::LOG_DEPTH); // Required because thrown errors cause the debugger to lose its branch

         {
            kt::Log log;

            log.msg(VLF::BRANCH|VLF::DETAIL, "Action notification for object #%d, action %d.  Top: %d", Object->UID, int(ActionID), lua_gettop(Self->Lua));

            lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, scan.Function); // +1 stack: Get the function reference
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

            if (lua_gc(Self->Lua, LUA_GCISRUNNING, 0)) {
               log.traceBranch("Collecting garbage.");
               lua_gc(Self->Lua, LUA_GCCOLLECT, 0);
            }
         }

         SetResource(RES::LOG_DEPTH, depth);

         if (ActionID IS AC::Free) {
            std::erase_if(Self->ActionList, [&](auto &item) {
               if (item.ObjectID IS Object->UID) {
                  if (item.Function) {
                     luaL_unref(Self->Lua, LUA_REGISTRYINDEX, item.Function);
                     item.Function = 0;
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

         return;
      }
   }
}

//********************************************************************************************************************

static ERR TIRI_Activate(extTiri *Self)
{
   kt::Log log;

   if (Self->Statement.empty()) return log.warning(ERR::FieldNotSet);

   log.trace("Target: %d, Procedure: %s / ID #%" PRId64, Self->TargetID,
      Self->Procedure.empty() ? "." : Self->Procedure.c_str(), (long long)Self->ProcedureID);

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
               if (!(error = Self->Error)) error = ERR::Failed;
            }
         }
      }

      Self->ActivationCount++;

      if (!Self->Error) run_script(Self); // Will set Self->Error if there's an issue

      Self->Recurse--;

      if (Self->Lua) {
         if (lua_gc(Self->Lua, LUA_GCISRUNNING, 0)) {
            kt::Log log;
            log.traceBranch("Collecting garbage.");
            lua_gc(Self->Lua, LUA_GCCOLLECT, 0); // Run the garbage collector
         }
      }

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

   if (Args->Datatype IS DATA::TEXT) {
      Self->setStatement((CSTRING)Args->Buffer);
   }
   else if (Args->Datatype IS DATA::XML) {
      Self->setStatement((CSTRING)Args->Buffer);
   }
   else if (Args->Datatype IS DATA::RECEIPT) {
      log.branch("Incoming data receipt from #%d", Args->Object ? Args->Object->UID : 0);

      for (auto it = Self->Requests.begin(); it != Self->Requests.end(); ) {
         if ((Args->Object) and (it->SourceID IS Args->Object->UID)) {
            // Execute the callback associated with this input subscription: function({Items...})

            int step = GetResource(RES::LOG_DEPTH); // Required as thrown errors cause the debugger to lose its step position

               lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, it->Callback); // +1 Reference to callback
               lua_newtable(Self->Lua); // +1 Item table

               if (auto xml = objXML::create::local(fl::Statement((CSTRING)Args->Buffer))) {
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

            SetResource(RES::LOG_DEPTH, step);

            it = Self->Requests.erase(it);
            continue;
         }
         it++;
      }

      if (lua_gc(Self->Lua, LUA_GCISRUNNING, 0)) {
         kt::Log log;
         log.traceBranch("Collecting garbage.");
         lua_gc(Self->Lua, LUA_GCCOLLECT, 0); // Run the garbage collector
      }
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

static ERR TIRI_Init(extTiri *Self)
{
   kt::Log log;

   if (not Self->Path.empty()) {
      if (Self->Path.starts_with("string:") or Self->Path.starts_with("STRING:")); // Assume Tiri for string paths
      else if (not wildcmp("*.tiri|*.tbc", Self->Path)) {
         log.warning("Path extension not recognised for '%s'", Self->Path.c_str());
         return ERR::NoSupport;
      }
   }

   if ((Self->defined(NF::RECLASSED)) and (Self->Statement.empty())) {
      log.trace("No support for reclassed Script with no String field value.");
      return ERR::NoSupport;
   }

   ERR error;
   bool compile = false;
   bool loaded = false;
   objFile *src_file = nullptr;
   if ((Self->Statement.empty()) and (not Self->Path.empty())) {
      int64_t src_ts = 0, src_size = 0;

      if ((src_file = objFile::create::local(fl::Path(Self->Path)))) {
         error = src_file->getTimestamp(src_ts);
         if ((!error) or (error IS ERR::NoSupport)) error = src_file->getSize(src_size);
      }
      else error = ERR::File;

      if (not Self->CacheFile.empty()) {
         // Compare the cache file date to the original source.  If they match, or if there was a problem
         // analysing the original location (i.e. the original location does not exist) then the cache file is loaded
         // instead of the original source code.

         int64_t cache_ts = -1, cache_size = 0;

         {
            objFile::create cache_file = { fl::Path(Self->CacheFile) };
            if (cache_file.ok()) {
               auto cache_error = cache_file->getTimestamp(cache_ts);
               if (!cache_error) cache_error = cache_file->getSize(cache_size);
               if (cache_error != ERR::Okay) cache_ts = -1;
            }
         }

         if (cache_ts != -1) {
            if ((cache_ts IS src_ts) or (error != ERR::Okay)) {
               log.msg("Using cache '%s'", Self->CacheFile.c_str());
               int len = 0;
               error = read_file_to_string(Self->CacheFile, cache_size, Self->Statement, &len);
               if (!error) loaded = len > 0;
            }
         }
      }

      if ((!error) and (not loaded)) {
         int len = 0;
         error = read_file_to_string(Self->Path, src_size, Self->Statement, &len);
         if (!error) {
            // Unicode BOM handler - in case the file starts with a BOM header.
            auto content = check_bom(Self->Statement);
            if (content.data() != Self->Statement.data()) Self->Statement.assign(content);

            if (not Self->CacheFile.empty()) compile = true; // Saving a compilation of the source is desired
         }
         else {
            log.trace("Failed to read %" PRId64 " bytes from '%s'", (int64_t)src_size, Self->Path.c_str());
            Self->Statement.clear();
            if (error != ERR::OutOfRange) error = ERR::ReadFileToBuffer;
         }
      }
   }
   else error = ERR::Okay;

   if ((!error) and (Self->SaveCompiled = compile)) {
      DateTime *dt;
      if (!src_file->getDate(dt)) Self->CacheDate = *dt;
      src_file->getPermissions(Self->CachePermissions);
   }

   if (error != ERR::Okay) {
      if (src_file) FreeResource(src_file);
      return log.warning(error);
   }

   Self->JitOptions |= glJitOptions;

   if (not (Self->Lua = luaL_newstate(Self))) {
      log.warning("Failed to open a Lua instance.");
      if (src_file) FreeResource(src_file);
      return ERR::Failed;
   }

   if (src_file) FreeResource(src_file);
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

   if (Self->Statement.empty()) return log.warning(ERR::FieldNotSet);

   if (Self->Recurse) return ERR::NothingDone; // Do nothing, script is running.

   if (not Self->MainChunkRef) {
      log.branch("Target: %d, Procedure: %s / ID #%" PRId64, Self->TargetID,
         Self->Procedure.empty() ? "." : Self->Procedure.c_str(), Self->ProcedureID);

      auto cleanup = kt::Defer([&]() {
         if (Self->Lua) {
            kt::Log().traceBranch("Collecting garbage.");
            lua_gc(Self->Lua, LUA_GCCOLLECT, 0); // Run the garbage collector
         }
      });

      lua_gc(Self->Lua, LUA_GCSTOP, 0);  // Stop collector during initialization
         luaL_openlibs(Self->Lua);  // Open Lua libraries
      lua_gc(Self->Lua, LUA_GCRESTART, 0);

      // Register private variables in the registry, which is tamper proof from the user's Lua code

      if (register_interfaces(Self) != ERR::Okay) return ERR::Failed;

      // Line hook, executes on the execution of a new line (doesn't execute during Query() compilation)

      if ((Self->Flags & SCF::LOG_ALL) != SCF::NIL) {
         // LUA_MASKLINE:  Interpreter is executing a line.
         // LUA_MASKCALL:  Interpreter is calling a function.
         // LUA_MASKRET:   Interpreter returns from a function.
         // LUA_MASKCOUNT: The hook will be called every X number of instructions executed (could be set to 1 for exactness).

         lua_sethook(Self->Lua, hook_debug, LUA_MASKCALL|LUA_MASKRET|LUA_MASKLINE, 0);
      }

      // Pre-load the Core module: mSys = mod.load('core')

      if (auto core = objModule::create::global(fl::Name("core"))) {
         SetName(core, "mSys");
         new_module(Self->Lua, core);
         lua_setglobal(Self->Lua, "mSys");
      }
      else {
         log.warning("Failed to create module object.");
         return ERR::LoadModule;
      }

      // Determine chunk name for better debug output.
      // Prefix with '@' to indicate file-based chunk (Lua convention), otherwise use '=' for special sources.
      // This ensures debug output shows the actual filename instead of "[string]".

      auto chunk_name = make_chunk_name(Self);

      int result;
      std::string_view source(Self->Statement);
      if (source.starts_with(LUA_COMPILED)) { // The source is compiled
         log.trace("Loading pre-compiled Lua script.");
         if (auto payload_error = compiled_payload(source, source); payload_error != ERR::Okay) {
            return log.warning(payload_error);
         }
         result = lua_load(Self->Lua, source, chunk_name.c_str());
      }
      else {
         log.trace("Compiling Lua script.");
         result = lua_load(Self->Lua, source, chunk_name.c_str());
      }

      if (result) { // Error reported from parser
         if (auto errorstr = lua_tostring(Self->Lua, -1)) {
            if (Self->Lua->parser_diagnostics and Self->Lua->parser_diagnostics->has_errors()) {
               std::string error_msg;
               for (const auto &entry : Self->Lua->parser_diagnostics->entries()) {
                  if (not error_msg.empty()) error_msg += "\n";
                  error_msg += entry.to_string(Self->LineOffset);
               }
               Self->setErrorMessage(error_msg);
            }
            else Self->setErrorMessage(errorstr);

            log.warning("%s", Self->ErrorMessage.c_str());
         }

         lua_pop(Self->Lua, 1);  // Pop error string
         return ERR::Syntax;
      }
      else {
         log.trace("Script successfully compiled.");

         // Store a reference to the compiled main chunk for post-execution analysis (e.g., bytecode disassembly)
         if (Self->MainChunkRef) luaL_unref(Self->Lua, LUA_REGISTRYINDEX, Self->MainChunkRef);
         lua_pushvalue(Self->Lua, -1); // Duplicate the function on top of the stack
         Self->MainChunkRef = luaL_ref(Self->Lua, LUA_REGISTRYINDEX); // Store reference, pops the duplicate
      }

      if (Self->SaveCompiled) { // Compile the script and save the result to the cache file
         log.msg("Compiling the source into the cache file.");

         Self->SaveCompiled = false;

         objFile::create cachefile = {
            fl::Path(Self->CacheFile), fl::Flags(FL::NEW|FL::WRITE), fl::Permissions(Self->CachePermissions)
         };

         if (cachefile.ok()) {
            save_binary(Self, *cachefile);
            cachefile->setDate(Self->CacheDate);
         }
      }

      return ERR::Okay;
   }
   else return ERR::NothingDone; // Script already compiled
}

/*********************************************************************************************************************

-ACTION-
SaveToObject: Compiles the current script statement and saves it as byte code.

Use the SaveToObject action to compile the statement in the Script's String field and save the resulting byte code to a
target object.  The byte code can be loaded into any script object for execution or referenced in the Tiri code for
usage.

*********************************************************************************************************************/

static ERR TIRI_SaveToObject(extTiri *Self, struct acSaveToObject *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Dest)) return log.warning(ERR::NullArgs);

   if (Self->Statement.empty()) return log.warning(ERR::FieldNotSet);

   log.branch("Compiling the statement...");

   auto chunk_name = make_chunk_name(Self);

   if (not lua_load(Self->Lua, std::string_view(Self->Statement), chunk_name.c_str())) {
      ERR error = save_binary(Self, Args->Dest);
      return error;
   }
   else {
      auto str = lua_tostringview(Self->Lua,-1);
      auto error_msg = str.empty() ? "" : str.data();
      log.warning("Compile Failure: %.*s", int(str.size()), error_msg);
      lua_pop(Self->Lua, 1);
      return ERR::InvalidData;
   }
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
   Self->JitOptions = Value;
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
// LuaJIT does support saving multi-platform compiled bytecode and we just need to implement it here.

static ERR save_binary(extTiri *Self, OBJECTPTR Target)
{
   // TODO No support for save_binary() yet.

   return ERR::NoSupport;
}

//********************************************************************************************************************

static ERR run_script(extTiri *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Procedure: %s, Top: %d", Self->Procedure.c_str(), lua_gettop(Self->Lua));

   Self->Lua->CaughtError = ERR::Okay;
   std::array<GCobject*, 8> release_list;
   size_t r = 0;
   int top;
   bool pcall_failed = false;
   if ((not Self->Procedure.empty()) or (Self->ProcedureID)) {
      if (not Self->Procedure.empty()) lua_getglobal(Self->Lua, Self->Procedure);
      else lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, Self->ProcedureID);

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

               if (type & FD_ARRAY) {
                  log.trace("Setting arg '%s', Array: %p", args->Name, args->Address);

                  APTR values = args->Address;
                  int total_elements = -1;
                  std::string_view arg_name(args->Name);
                  if (args[1].Type & FD_ARRAYSIZE) {
                     if (args[1].Type & FD_INT) total_elements = args[1].Int;
                     else if (args[1].Type & FD_INT64) total_elements = args[1].Int64;
                     else values = nullptr;
                     i++; args++; // Because we took the array-size parameter into account
                  }
                  else log.trace("The size of the array is not defined.");

                  if (values) {
                     make_any_array(Self->Lua, type, arg_name, total_elements, values);

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
               else if (type & (FD_PTR|FD_BUFFER)) {
                  // Try and make the pointer safer/more usable by translating it into a buffer, object ID or whatever.
                  // (In a secure environment, pointers may be passed around but may be useless if their use is
                  // disallowed within Lua).

                  log.trace("Setting arg '%s', Value: %p", args->Name, args->Address);
                  if ((type & FD_BUFFER) and (i+1 < Self->TotalArgs) and (args[1].Type & FD_BUFSIZE)) {
                     // Buffers are considered to be directly writable regions of memory, so the array interface is
                     // used to represent them.
                     if (args[1].Type & FD_INT) lua_createarray(Self->Lua, args[1].Int, AET::BYTE, (APTR *)args->Address, ARRAY_EXTERNAL);
                     else if (args[1].Type & FD_INT64) lua_createarray(Self->Lua, args[1].Int64, AET::BYTE, (APTR *)args->Address, ARRAY_EXTERNAL);
                     else lua_pushnil(Self->Lua);
                     i++; args++; // Because we took the buffer-size parameter into account
                  }
                  else if (type & FD_OBJECT) {
                     // Pushing direct object pointers is considered safe because they are treated as detached, then
                     // a lock is gained for the duration of the call that is then released on return.  This is a
                     // solid optimisation that also protects the object from unwarranted termination during the call.

                     if (args->Address) {
                        GCobject *obj = push_object(Self->Lua, (OBJECTPTR)args->Address);
                        if ((r < release_list.size()) and (access_object(obj))) {
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
            Self->Procedure.empty() ? "NULL" : Self->Procedure.c_str(), Self->ProcedureID);
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

static ERR register_interfaces(extTiri *Self)
{
   kt::Log log;

   log.traceBranch("Registering Kotuku and Tiri interfaces with Lua.");

#ifndef NDEBUG
   int stack_top = lua_gettop(Self->Lua);
#endif

   register_io_class(Self->Lua);
   register_module_class(Self->Lua);
   register_regex_class(Self->Lua);
   register_struct_class(Self->Lua);
   register_async_class(Self->Lua);
#ifndef DISABLE_DISPLAY
   register_input_class(Self->Lua);
#endif
   register_processing_class(Self->Lua);

   lua_register(Self->Lua, "arg", fcmd_arg);
   lua_register(Self->Lua, "loadFile", fcmd_loadfile);
   lua_register(Self->Lua, "exec", fcmd_exec);
   lua_register(Self->Lua, "print", fcmd_print);
   lua_register(Self->Lua, "include", fcmd_include);
   lua_register(Self->Lua, "msg", fcmd_msg);
   lua_register(Self->Lua, "subscribeEvent", fcmd_subscribe_event);
   lua_register(Self->Lua, "unsubscribeEvent", fcmd_unsubscribe_event);
   lua_register(Self->Lua, "MAKESTRUCT", MAKESTRUCT);

   // Register global function prototypes for compile-time type inference
   reg_func_prototype("arg", { TiriType::Any }, { TiriType::Str, TiriType::Any });
   reg_func_prototype("loadFile", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("exec", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("getExecutionState", { TiriType::Table }, {});
   reg_func_prototype("print", {}, {}, FProtoFlags::Variadic);
   reg_func_prototype("include", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("require", { TiriType::Table }, { TiriType::Str });
   reg_func_prototype("msg", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("subscribeEvent", { TiriType::Num, TiriType::Any }, { TiriType::Str, TiriType::Func });
   reg_func_prototype("unsubscribeEvent", {}, { TiriType::Any });
   reg_func_prototype("MAKESTRUCT", { TiriType::Any }, { TiriType::Str });

   load_include(Self, "core");

#ifndef NDEBUG
   int stack_delta = lua_gettop(Self->Lua) - stack_top;
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
   { "Procedures", FDF_VIRTUAL|FDF_ARRAY|FDF_CPPSTRING|FDF_R, GET_Procedures },
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
