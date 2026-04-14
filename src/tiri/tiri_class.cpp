
#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/strings.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lua.hpp"

#include "lj_obj.h"
#include "lj_bc.h"
#include "parser/parser_diagnostics.h"
#include "jit/src/debug/dump_bytecode.h"
#include "lj_proto_registry.h"

#include "hashes.h"
#include "defs.h"

static ERR run_script(objScript *);
static ERR stack_args(lua_State *, OBJECTID, const FunctionField *, int8_t *);
static ERR save_binary(objScript *, OBJECTPTR);

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

[[maybe_unused]] static ERR register_interfaces(objScript *);

//********************************************************************************************************************
// Dump the variables of any global table

[[maybe_unused]] static void dump_global_table(objScript *Self, STRING Global)
{
   pf::Log log("print_env");
   lua_State *lua = ((prvTiri *)Self->ChildPrivate)->Lua;
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

static const FieldDef clJitOptions[] = {
   { "Diagnose",           JOF::DIAGNOSE },
   { "DisableJit",         JOF::DISABLE_JIT },
   { "DumpBytecode",       JOF::DUMP_BYTECODE },
   { "Profile",            JOF::PROFILE },
   { "TopTips",            JOF::TOP_TIPS },
   { "Tips",               JOF::TIPS },
   { "AllTips",            JOF::ALL_TIPS },
   { "Trace",              JOF::TRACE },
   { "TraceTypes",         JOF::TRACE_TYPES },
   { "TraceTokens",        JOF::TRACE_TOKENS },
   { "TraceBoundary",      JOF::TRACE_BOUNDARY },
   { "TraceExpect",        JOF::TRACE_EXPECT },
   { "TraceCfg",           JOF::TRACE_CFG },
   { "TraceOperators",     JOF::TRACE_OPERATORS },
   { "TraceRegisters",     JOF::TRACE_REGISTERS },
   { "TraceAssignments",   JOF::TRACE_ASSIGNMENTS },
   { "TraceValueCategory", JOF::TRACE_VALUE_CATEGORY },
   { nullptr, 0 }
};

static ERR GET_JitOptions(objScript *, JOF *);
static ERR SET_JitOptions(objScript *, JOF);
static ERR GET_Procedures(objScript *, pf::vector<std::string> **, int *);

static const FieldArray clFields[] = {
   { "JitOptions", FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, GET_JitOptions, SET_JitOptions, &clJitOptions },
   { "Procedures", FDF_VIRTUAL|FDF_CPP|FDF_ARRAY|FDF_STRING|FDF_R, GET_Procedures },
   END_FIELD
};

//********************************************************************************************************************

static ERR TIRI_Activate(objScript *);
static ERR TIRI_DataFeed(objScript *, struct acDataFeed *);
static ERR TIRI_Free(objScript *);
static ERR TIRI_Init(objScript *);
static ERR TIRI_NewChild(objScript *, struct acNewChild &);
static ERR TIRI_NewObject(objScript *);
static ERR TIRI_Query(objScript *);
static ERR TIRI_SaveToObject(objScript *, struct acSaveToObject *);

static const ActionArray clActions[] = {
   { AC::Activate,     TIRI_Activate },
   { AC::DataFeed,     TIRI_DataFeed },
   { AC::Free,         TIRI_Free },
   { AC::Init,         TIRI_Init },
   { AC::NewChild,     TIRI_NewChild },
   { AC::NewObject,    TIRI_NewObject },
   { AC::Query,        TIRI_Query },
   { AC::SaveToObject, TIRI_SaveToObject },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************
// Only to be used immediately after a failed lua_pcall().  Lua stores a description of the error that occurred on the
// stack, this will be popped and copied to the ErrorMessage field.

void process_error(objScript *Self, CSTRING Procedure)
{
   auto prv = (prvTiri *)Self->ChildPrivate;

   auto flags = VLF::WARNING;
   if (prv->Lua->CaughtError != ERR::Okay) {
      Self->Error = prv->Lua->CaughtError;
      if (Self->Error <= ERR::Terminate) flags = VLF::DETAIL; // Non-critical errors are muted to prevent log noise.
   }
   else Self->Error = ERR::Exception; // Unspecified exception, e.g. an error() or assert().  The result string will indicate detail.

   pf::Log log;
   auto str = lua_tostring(prv->Lua, -1);
   lua_pop(prv->Lua, 1);  // pop returned value
   Self->setErrorMessage(str);

   if (auto file = Self->Path) {
      int i;
      for (i=strlen(file); (i > 0) and (file[i-1] != '/') and (file[i-1] != '\\'); i--);
      log.msg(flags, "%s: %s", file+i, str);
   }
   else log.msg(flags, "%s: Error: %s", Procedure, str);

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
   pf::Log log(__FUNCTION__);

   if (not args) return ERR::Okay;

   log.traceBranch("Args: %p, Buffer: %p", args, Buffer);

   for (int i=0; args[i].Name; i++) {
      std::string name(args[i].Name);
      std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });

      lua_pushlstring(Lua, name.c_str(), name.size());

      // Note: If the object is public and the call was messaged from a foreign process, all strings/pointers are
      // invalid because the message handlers cannot do deep pointer resolution of the structure we receive from
      // action notifications.

      if (args[i].Type & FD_STR) {
         if (ObjectID > 0) lua_pushstring(Lua, ((STRING *)Buffer)[0]);
         else lua_pushnil(Lua);
         Buffer += sizeof(STRING);
      }
      else if (args[i].Type & FD_PTR) {
         if (ObjectID > 0) lua_pushlightuserdata(Lua, ((APTR *)Buffer)[0]);
         else lua_pushnil(Lua);
         Buffer += sizeof(APTR);
      }
      else if (args[i].Type & FD_INT) {
         lua_pushinteger(Lua, ((int *)Buffer)[0]);
         Buffer += sizeof(int);
      }
      else if (args[i].Type & FD_DOUBLE) {
         lua_pushnumber(Lua, ((double *)Buffer)[0]);
         Buffer += sizeof(double);
      }
      else if (args[i].Type & FD_INT64) {
         lua_pushnumber(Lua, ((int64_t *)Buffer)[0]);
         Buffer += sizeof(int64_t);
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
   auto Self = (objScript *)CurrentContext();

   if (Result != ERR::Okay) return;

   auto prv = (prvTiri *)Self->ChildPrivate;
   if (not prv) return;

   for (auto &scan : prv->ActionList) {
      if ((Object->UID IS scan.ObjectID) and (ActionID IS scan.ActionID)) {
         int depth = GetResource(RES::LOG_DEPTH); // Required because thrown errors cause the debugger to lose its branch

         {
            pf::Log log;

            log.msg(VLF::BRANCH|VLF::DETAIL, "Action notification for object #%d, action %d.  Top: %d", Object->UID, int(ActionID), lua_gettop(prv->Lua));

            lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, scan.Function); // +1 stack: Get the function reference
            push_object_id(prv->Lua, Object->UID);  // +1: Pass the object ID
            lua_newtable(prv->Lua);  // +1: Table to store the parameters

            if ((scan.Args) and (Args)) {
               stack_args(prv->Lua, Object->UID, scan.Args, (int8_t *)Args);
            }

            int total_args = 2;

            if (scan.Reference) { // +1: Custom reference (optional)
               lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, scan.Reference);
               total_args++; // ObjectID, ArgTable, Reference
            }

            if (lua_pcall(prv->Lua, total_args, 0, 0)) { // Make the call, function & args are removed from stack.
               process_error(Self, "Action Subscription");
            }

            if (lua_gc(prv->Lua, LUA_GCISRUNNING, 0)) {
               log.traceBranch("Collecting garbage.");
               lua_gc(prv->Lua, LUA_GCCOLLECT, 0);
            }
         }

         SetResource(RES::LOG_DEPTH, depth);
         return;
      }
   }
}

//********************************************************************************************************************

static ERR TIRI_Activate(objScript *Self)
{
   pf::Log log;

   if ((not Self->String) or (not Self->String[0])) return log.warning(ERR::FieldNotSet);

   log.trace("Target: %d, Procedure: %s / ID #%" PRId64, Self->TargetID, Self->Procedure ? Self->Procedure : (STRING)".", (long long)Self->ProcedureID);

   auto prv = (prvTiri *)Self->ChildPrivate;
   if (not prv) return log.warning(ERR::ObjectCorrupt);

   if ((prv->Recurse) and (not Self->Procedure) and (not Self->ProcedureID)) {
      return ERR::Okay; // Do nothing, script is running.
   }

   Self->CurrentLine = -1;
   Self->Error       = ERR::Okay;
   if (auto error = acQuery(Self); error <= ERR::ExceptionThreshold) {
      prv->Recurse++;

      if ((prv->JitOptions & JOF::DISABLE_JIT) != JOF::NIL) {
         luaJIT_setmode(prv->Lua, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
      }

      if ((Self->Procedure) or (Self->ProcedureID)) {
         // The Lua script needs to have been executed at least once in order for the procedures to be initialised and recognised.

         if (Self->ActivationCount IS 0) {
            pf::Log log;
            log.traceBranch("Collecting functions prior to procedure call...");

            if (lua_pcall(prv->Lua, 0, 0, 0)) {
               process_error(Self, "Activation");
               if ((error = Self->Error) IS ERR::Okay) error = ERR::Failed;
            }
         }
      }

      Self->ActivationCount++;

      if (Self->Error IS ERR::Okay) run_script(Self); // Will set Self->Error if there's an issue

      prv->Recurse--;

      if (prv->Lua) {
         if (lua_gc(prv->Lua, LUA_GCISRUNNING, 0)) {
            pf::Log log;
            log.traceBranch("Collecting garbage.");
            lua_gc(prv->Lua, LUA_GCCOLLECT, 0); // Run the garbage collector
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

static ERR TIRI_DataFeed(objScript *Self, struct acDataFeed *Args)
{
   pf::Log log;

   if (not Args) return ERR::NullArgs;

   if (Args->Datatype IS DATA::TEXT) {
      Self->setStatement((CSTRING)Args->Buffer);
   }
   else if (Args->Datatype IS DATA::XML) {
      Self->setStatement((CSTRING)Args->Buffer);
   }
   else if (Args->Datatype IS DATA::RECEIPT) {
      auto prv = (prvTiri *)Self->ChildPrivate;

      log.branch("Incoming data receipt from #%d", Args->Object ? Args->Object->UID : 0);

      for (auto it = prv->Requests.begin(); it != prv->Requests.end(); ) {
         if ((Args->Object) and (it->SourceID IS Args->Object->UID)) {
            // Execute the callback associated with this input subscription: function({Items...})

            int step = GetResource(RES::LOG_DEPTH); // Required as thrown errors cause the debugger to lose its step position

               lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, it->Callback); // +1 Reference to callback
               lua_newtable(prv->Lua); // +1 Item table

               if (auto xml = objXML::create::local(fl::Statement((CSTRING)Args->Buffer))) {
                  // <file path="blah.exe"/> becomes { item='file', path='blah.exe' }

                  if (not xml->Tags.empty()) {
                     auto &tag = xml->Tags[0];
                     int i = 0;
                     if (iequals("receipt", tag.name())) {
                        for (auto &scan : tag.Children) {
                           lua_pushinteger(prv->Lua, i++);
                           lua_newtable(prv->Lua);

                           lua_pushstring(prv->Lua, "item");
                           lua_pushstring(prv->Lua, scan.name());
                           lua_settable(prv->Lua, -3);

                           for (unsigned a=1; a < scan.Attribs.size(); a++) {
                              lua_pushstring(prv->Lua, scan.Attribs[a].Name.c_str());
                              lua_pushstring(prv->Lua, scan.Attribs[a].Value.c_str());
                              lua_settable(prv->Lua, -3);
                           }

                           lua_settable(prv->Lua, -3);
                        }
                     }
                  }

                  FreeResource(xml);

                  if (lua_pcall(prv->Lua, 1, 0, 0)) { // function(Items)
                     process_error(Self, "Data Receipt Callback");
                  }
               }

            SetResource(RES::LOG_DEPTH, step);

            it = prv->Requests.erase(it);
            continue;
         }
         it++;
      }

      if (lua_gc(prv->Lua, LUA_GCISRUNNING, 0)) {
         pf::Log log;
         log.traceBranch("Collecting garbage.");
         lua_gc(prv->Lua, LUA_GCCOLLECT, 0); // Run the garbage collector
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TIRI_Free(objScript *Self)
{
   if (auto prv = (prvTiri *)Self->ChildPrivate) {
      if (prv->FocusEventHandle) { UnsubscribeEvent(prv->FocusEventHandle); prv->FocusEventHandle = nullptr; }

      auto lua = prv->Lua;
      prv->~prvTiri();

      if (lua) lua_close(lua);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TIRI_Init(objScript *Self)
{
   pf::Log log;

   if (Self->Path) {
      if (not wildcmp("*.tiri|*.tbc", Self->Path)) {
         log.warning("No support for path '%s'", Self->Path);
         return ERR::NoSupport;
      }
   }

   if ((Self->defined(NF::RECLASSED)) and (not Self->String)) {
      log.trace("No support for reclassed Script with no String field value.");
      return ERR::NoSupport;
   }

   ERR error;
   bool compile = false;
   int loaded_size = 0;
   objFile *src_file = nullptr;
   if ((not Self->String) and (Self->Path)) {
      int64_t src_ts = 0, src_size = 0;

      if ((src_file = objFile::create::local(fl::Path(Self->Path)))) {
         error = src_file->get(FID_TimeStamp, src_ts);
         if (error IS ERR::Okay) error = src_file->get(FID_Size, src_size);
      }
      else error = ERR::File;

      if (Self->CacheFile) {
         // Compare the cache file date to the original source.  If they match, or if there was a problem
         // analysing the original location (i.e. the original location does not exist) then the cache file is loaded
         // instead of the original source code.

         int64_t cache_ts = -1, cache_size;

         {
            objFile::create cache_file = { fl::Path(Self->CacheFile) };
            if (cache_file.ok()) {
               cache_file->get(FID_TimeStamp, cache_ts);
               cache_file->get(FID_Size, cache_size);
            }
         }

         if (cache_ts != -1) {
            if ((cache_ts IS src_ts) or (error != ERR::Okay)) {
               log.msg("Using cache '%s'", Self->CacheFile);
               if (AllocMemory(cache_size, MEM::STRING|MEM::NO_CLEAR, &Self->String) IS ERR::Okay) {
                  int len;
                  error = ReadFileToBuffer(Self->CacheFile, Self->String, cache_size, &len);
                  loaded_size = cache_size;
               }
               else error = ERR::AllocMemory;
            }
         }
      }

      if ((error IS ERR::Okay) and (not loaded_size)) {
         if (AllocMemory(src_size+1, MEM::STRING|MEM::NO_CLEAR, &Self->String) IS ERR::Okay) {
            int len;
            if (ReadFileToBuffer(Self->Path, Self->String, src_size, &len) IS ERR::Okay) {
               Self->String[len] = 0;

               // Unicode BOM handler - in case the file starts with a BOM header.
               auto content = check_bom(std::string_view(Self->String, len));
               if (content.data() != Self->String) {
                  // Use memmove for overlapping memory regions (content.data() points into Self->String)
                  std::memmove(Self->String, content.data(), content.size());
                  Self->String[content.size()] = '\0';
                  len = content.size();
               }

               loaded_size = len;

               if (Self->CacheFile) compile = true; // Saving a compilation of the source is desired
            }
            else {
               log.trace("Failed to read %" PRId64 " bytes from '%s'", (long long)src_size, Self->Path);
               FreeResource(Self->String);
               Self->String = nullptr;
               error = ERR::ReadFileToBuffer;
            }
         }
         else error = ERR::AllocMemory;
      }
   }
   else error = ERR::Okay;

   // Allocate private structure if not done by NewObject().

   auto prv = (prvTiri *)Self->ChildPrivate;
   if ((error IS ERR::Okay) and (not prv)) {
      if (AllocMemory(sizeof(prvTiri), MEM::DATA, &Self->ChildPrivate) IS ERR::Okay) {
         prv = (prvTiri *)Self->ChildPrivate;
         new (prv) prvTiri;
      }
      else error = ERR::AllocMemory;
   }

   if ((error IS ERR::Okay) and (prv->SaveCompiled = compile)) {
      DateTime *dt;
      if (src_file->get(FID_Date, dt) IS ERR::Okay) prv->CacheDate = *dt;
      src_file->get(FID_Permissions, (int &)prv->CachePermissions);
      prv->LoadedSize = loaded_size;
   }

   if (error != ERR::Okay) {
      if (src_file) FreeResource(src_file);
      return log.warning(error);
   }

   prv->JitOptions |= glJitOptions;

   if (not (prv->Lua = luaL_newstate(Self))) {
      log.warning("Failed to open a Lua instance.");
      FreeResource(Self->ChildPrivate);
      Self->ChildPrivate = nullptr;
      if (src_file) FreeResource(src_file);
      return ERR::Failed;
   }

   if (src_file) FreeResource(src_file);
   return ERR::Okay;
}

//********************************************************************************************************************
// If the script is being executed, retarget the new resource to refer to the current task (because we don't want
// client resources allocated by the script to be automatically destroyed when the script is terminated by the client).

static ERR TIRI_NewChild(objScript *Self, struct acNewChild &Args)
{
   auto prv = (prvTiri*)Self->ChildPrivate;
   if (not prv) return ERR::Okay;

   if (prv->Recurse) {
      SetOwner(Args.Object, CurrentTask());
      return ERR::OwnerPassThrough;
   }
   else return ERR::Okay;
}

//********************************************************************************************************************
// The client has specifically asked for a Tiri script to be created - this allows us to configure ChildPrivate
// early.  Otherwise, it is created during Init().

static ERR TIRI_NewObject(objScript *Self)
{
   if (AllocMemory(sizeof(prvTiri), MEM::DATA, &Self->ChildPrivate) IS ERR::Okay) {
      auto prv = (prvTiri *)Self->ChildPrivate;
      new (prv) prvTiri;
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

/*********************************************************************************************************************

-ACTION-
Query: Compiles the script and prepares it for execution.

Query() performs the process of compiling the Tiri script.  A Tiri script can be queried only once, at which point
the object enters a configured state.   Further queries will do nothing, and return `ERR::NothingDone`.

Note that due to the code not being run, declared functions and variables won't be formally registered.
Introspection of available procedures will be limited until the script is activated.

*********************************************************************************************************************/

static ERR TIRI_Query(objScript *Self)
{
   pf::Log log;

   if ((not Self->String) or (not Self->String[0])) return log.warning(ERR::FieldNotSet);

   auto prv = (prvTiri *)Self->ChildPrivate;
   if (not prv) return log.warning(ERR::ObjectCorrupt);
   if (prv->Recurse) return ERR::NothingDone; // Do nothing, script is running.

   if (not prv->MainChunkRef) {
      log.branch("Target: %d, Procedure: %s / ID #%" PRId64, Self->TargetID, Self->Procedure ? Self->Procedure : (STRING)".", Self->ProcedureID);

      auto cleanup = pf::Defer([&]() {
         if (prv->Lua) {
            pf::Log().traceBranch("Collecting garbage.");
            lua_gc(prv->Lua, LUA_GCCOLLECT, 0); // Run the garbage collector
         }
      });

      lua_gc(prv->Lua, LUA_GCSTOP, 0);  // Stop collector during initialization
         luaL_openlibs(prv->Lua);  // Open Lua libraries
      lua_gc(prv->Lua, LUA_GCRESTART, 0);

      // Register private variables in the registry, which is tamper proof from the user's Lua code

      if (register_interfaces(Self) != ERR::Okay) return ERR::Failed;

      // Line hook, executes on the execution of a new line (doesn't execute during Query() compilation)

      if ((Self->Flags & SCF::LOG_ALL) != SCF::NIL) {
         // LUA_MASKLINE:  Interpreter is executing a line.
         // LUA_MASKCALL:  Interpreter is calling a function.
         // LUA_MASKRET:   Interpreter returns from a function.
         // LUA_MASKCOUNT: The hook will be called every X number of instructions executed (could be set to 1 for exactness).

         lua_sethook(prv->Lua, hook_debug, LUA_MASKCALL|LUA_MASKRET|LUA_MASKLINE, 0);
      }

      // Pre-load the Core module: mSys = mod.load('core')

      if (auto core = objModule::create::global(fl::Name("core"))) {
         SetName(core, "mSys");
         new_module(prv->Lua, core);
         lua_setglobal(prv->Lua, "mSys");
      }
      else {
         log.warning("Failed to create module object.");
         return ERR::LoadModule;
      }

      // Determine chunk name for better debug output.
      // Prefix with '@' to indicate file-based chunk (Lua convention), otherwise use '=' for special sources.
      // This ensures debug output shows the actual filename instead of "[string]".

      std::string chunk_name;
      if (Self->Path) chunk_name = std::string("@") + Self->Path;
      else chunk_name = "=script";

      int result;
      if (startswith(LUA_COMPILED, Self->String)) { // The source is compiled
         log.trace("Loading pre-compiled Lua script.");
         int headerlen = strlen(Self->String) + 1;
         result = lua_load(prv->Lua, std::string_view(Self->String + headerlen, prv->LoadedSize - headerlen), chunk_name.c_str());
      }
      else {
         log.trace("Compiling Lua script.");
         result = lua_load(prv->Lua, std::string_view(Self->String), chunk_name.c_str());
      }

      if (result) { // Error reported from parser
         if (auto errorstr = lua_tostring(prv->Lua, -1)) {
            if (prv->Lua->parser_diagnostics and prv->Lua->parser_diagnostics->has_errors()) {
               std::string error_msg;
               for (const auto &entry : prv->Lua->parser_diagnostics->entries()) {
                  if (not error_msg.empty()) error_msg += "\n";
                  error_msg += entry.to_string(Self->LineOffset);
               }
               Self->setErrorMessage(error_msg);
            }
            else Self->setErrorMessage(errorstr);

            log.warning("%s", Self->ErrorMessage);
         }

         lua_pop(prv->Lua, 1);  // Pop error string
         return ERR::Syntax;
      }
      else {
         log.trace("Script successfully compiled.");

         // Store a reference to the compiled main chunk for post-execution analysis (e.g., bytecode disassembly)
         if (prv->MainChunkRef) luaL_unref(prv->Lua, LUA_REGISTRYINDEX, prv->MainChunkRef);
         lua_pushvalue(prv->Lua, -1); // Duplicate the function on top of the stack
         prv->MainChunkRef = luaL_ref(prv->Lua, LUA_REGISTRYINDEX); // Store reference, pops the duplicate
      }

      if (prv->SaveCompiled) { // Compile the script and save the result to the cache file
         log.msg("Compiling the source into the cache file.");

         prv->SaveCompiled = false;

         objFile::create cachefile = {
            fl::Path(Self->CacheFile), fl::Flags(FL::NEW|FL::WRITE), fl::Permissions(prv->CachePermissions)
         };

         if (cachefile.ok()) {
            save_binary(Self, *cachefile);
            cachefile->setDate(&prv->CacheDate);
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

static ERR TIRI_SaveToObject(objScript *Self, struct acSaveToObject *Args)
{
   pf::Log log;

   if ((not Args) or (not Args->Dest)) return log.warning(ERR::NullArgs);

   if (not Self->String) return log.warning(ERR::FieldNotSet);

   log.branch("Compiling the statement...");

   auto prv = (prvTiri *)Self->ChildPrivate;
   if (not prv) return log.warning(ERR::ObjectCorrupt);

   std::string chunk_name;
   if (Self->Path) chunk_name = std::string("@") + Self->Path;
   else chunk_name = "=script";

   if (not lua_load(prv->Lua, std::string_view(Self->String, strlen(Self->String)), chunk_name.c_str())) {
      ERR error = save_binary(Self, Args->Dest);
      return error;
   }
   else {
      auto str = lua_tostring(prv->Lua,-1);
      lua_pop(prv->Lua, 1);
      log.warning("Compile Failure: %s", str);
      return ERR::InvalidData;
   }
}

/*********************************************************************************************************************

-FIELD-
JitOptions: Defines JIT debugging options.

This field allows the client to configure debugging options related to the Just-In-Time (JIT) compilation process.

-END-

*********************************************************************************************************************/

static ERR GET_JitOptions(objScript *Self, JOF *Value)
{
   if (auto prv = (prvTiri *)Self->ChildPrivate) {
      *Value = prv->JitOptions;
      return ERR::Okay;
   }
   else return ERR::InvalidState; // Either requires initialisation or to be created as a Tiri script from NewObject()
}

static ERR SET_JitOptions(objScript *Self, JOF Value)
{
   if (auto prv = (prvTiri *)Self->ChildPrivate) {
      if (prv->Recurse) {
         pf::Log().warning("Changing JIT options after parsing is ineffective.");
         return ERR::InvalidState;
      }
      prv->JitOptions = Value;
      return ERR::Okay;
   }
   else return ERR::InvalidState; // Either requires initialisation or to be created as a Tiri script from NewObject()
}

/*********************************************************************************************************************

-FIELD-
Procedures: Returns a string array of all named procedures defined by a script.

This field will return a string array of all procedures loaded into the script, conditional on it being activated.
It will otherwise return an empty array.
-END-

*********************************************************************************************************************/

static ERR GET_Procedures(objScript *Self, pf::vector<std::string> **Value, int *Elements)
{
   if (auto prv = (prvTiri *)Self->ChildPrivate) {
      prv->Procedures.clear();
      lua_pushnil(prv->Lua);
      while (lua_next(prv->Lua, LUA_GLOBALSINDEX)) {
         if (lua_type(prv->Lua, -1) IS LUA_TFUNCTION) {
            prv->Procedures.push_back(lua_tostring(prv->Lua, -2));
         }
         lua_pop(prv->Lua, 1);
      }

      *Value = &prv->Procedures;
      *Elements = prv->Procedures.size();
      return ERR::Okay;
   }
   else return ERR::NotInitialised;
}

//********************************************************************************************************************
// LuaJIT does support saving multi-platform compiled bytecode and we just need to implement it here.

static ERR save_binary(objScript *Self, OBJECTPTR Target)
{
   // TODO No support for save_binary() yet.

   return ERR::NoSupport;
}

//********************************************************************************************************************

static ERR run_script(objScript *Self)
{
   pf::Log log(__FUNCTION__);

   auto prv = (prvTiri *)Self->ChildPrivate;

   log.traceBranch("Procedure: %s, Top: %d", Self->Procedure, lua_gettop(prv->Lua));

   prv->Lua->CaughtError = ERR::Okay;
   std::array<GCobject*, 8> release_list;
   size_t r = 0;
   int top;
   bool pcall_failed = false;
   if ((Self->Procedure) or (Self->ProcedureID)) {
      if (Self->Procedure) lua_getglobal(prv->Lua, Self->Procedure);
      else lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, Self->ProcedureID);

      if (lua_isfunction(prv->Lua, -1)) {
         if ((Self->Flags & SCF::LOG_ALL) != SCF::NIL) log.branch("Executing procedure: %s, Args: %d", Self->Procedure, Self->TotalArgs);

         top = lua_gettop(prv->Lua);

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
                     make_any_array(prv->Lua, type, arg_name, total_elements, values);

                     if (type & FD_ALLOC) FreeResource(values);
                  }
                  else lua_pushnil(prv->Lua);
               }
               else if (type & FD_STR) {
                  log.trace("Setting arg '%s', Value: %.20s", args->Name, (CSTRING)args->Address);
                  lua_pushstring(prv->Lua, (CSTRING)args->Address);
               }
               else if (type & FD_STRUCT) {
                  // Pointer to a struct, which can be referenced with a name of "StructName" or "StructName:ArgName"
                  if (args->Address) {
                     if (named_struct_to_table(prv->Lua, args->Name, args->Address) != ERR::Okay) lua_pushnil(prv->Lua);
                     if (type & FD_ALLOC) FreeResource(args->Address);
                  }
                  else lua_pushnil(prv->Lua);
               }
               else if (type & (FD_PTR|FD_BUFFER)) {
                  // Try and make the pointer safer/more usable by translating it into a buffer, object ID or whatever.
                  // (In a secure environment, pointers may be passed around but may be useless if their use is
                  // disallowed within Lua).

                  log.trace("Setting arg '%s', Value: %p", args->Name, args->Address);
                  if ((type & FD_BUFFER) and (i+1 < Self->TotalArgs) and (args[1].Type & FD_BUFSIZE)) {
                     // Buffers are considered to be directly writable regions of memory, so the array interface is
                     // used to represent them.
                     if (args[1].Type & FD_INT) lua_createarray(prv->Lua, args[1].Int, AET::BYTE, (APTR *)args->Address, ARRAY_EXTERNAL);
                     else if (args[1].Type & FD_INT64) lua_createarray(prv->Lua, args[1].Int64, AET::BYTE, (APTR *)args->Address, ARRAY_EXTERNAL);
                     else lua_pushnil(prv->Lua);
                     i++; args++; // Because we took the buffer-size parameter into account
                  }
                  else if (type & FD_OBJECT) {
                     // Pushing direct object pointers is considered safe because they are treated as detached, then
                     // a lock is gained for the duration of the call that is then released on return.  This is a
                     // solid optimisation that also protects the object from unwarranted termination during the call.

                     if (args->Address) {
                        GCobject *obj = push_object(prv->Lua, (OBJECTPTR)args->Address);
                        if ((r < release_list.size()) and (access_object(obj))) {
                           release_list[r++] = obj;
                        }
                     }
                     else lua_pushnil(prv->Lua);
                  }
                  else lua_pushlightuserdata(prv->Lua, args->Address);
               }
               else if (type & FD_INT)   {
                  log.trace("Setting arg '%s', Value: %d", args->Name, args->Int);
                  if (type & FD_OBJECT) {
                     if (args->Int) push_object_id(prv->Lua, args->Int);
                     else lua_pushnil(prv->Lua);
                  }
                  else lua_pushinteger(prv->Lua, args->Int);
               }
               else if (type & FD_INT64)  { log.trace("Setting arg '%s', Value: %" PRId64, args->Name, (long long)args->Int64); lua_pushnumber(prv->Lua, args->Int64); }
               else if (type & FD_DOUBLE) { log.trace("Setting arg '%s', Value: %.2f", args->Name, args->Double); lua_pushnumber(prv->Lua, args->Double); }
               else { lua_pushnil(prv->Lua); log.warning("Arg '%s' uses unrecognised type $%.8x", args->Name, type); }
               count++;
            }
         }

         int step = GetResource(RES::LOG_DEPTH);

         if (lua_pcall(prv->Lua, count, LUA_MULTRET, 0)) {
            pcall_failed = true;
         }

         SetResource(RES::LOG_DEPTH, step);

         while (r > 0) release_object(release_list[--r]);
      }
      else {
         auto str = std::format("Procedure '{}' / #{} does not exist in the script.",
            Self->Procedure ? Self->Procedure : "NULL", Self->ProcedureID);
         Self->setErrorMessage(str.c_str());
         log.warning("%s", str.c_str());

         #ifndef NDEBUG
            pf::vector<std::string> *list;
            int total_procedures;
            if (GET_Procedures(Self, &list, &total_procedures) IS ERR::Okay) {
               for (int i=0; i < total_procedures; i++) log.trace("%s", list[0][i]);
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
         lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, prv->MainChunkRef);
      }

      int depth = GetResource(RES::LOG_DEPTH);

         top = lua_gettop(prv->Lua);
         if (lua_pcall(prv->Lua, 0, LUA_MULTRET, 0)) pcall_failed = true;

      SetResource(RES::LOG_DEPTH, depth);
   }

   if (not pcall_failed) { // If the procedure returned results, copy them to the Results field of the Script.
      int results = lua_gettop(prv->Lua) - top + 1;

      if (results > 0) {
         std::vector<CSTRING> array;
         array.resize(results+1);

         // NB: The Results field will take a clone of the Lua strings, so this sub-routine is safe to pass
         // on Lua's temporary string results.

         int i;
         for (i=0; i < results; i++) {
            array[i] = lua_tostring(prv->Lua, -results+i);
            log.trace("Result: %d/%d: %s", i, results, array[i]);
         }
         array[i] = nullptr;
         Self->set(FID_Results, array.data(), i);
         lua_pop(prv->Lua, results);  // pop returned values
      }

      // Flush pending messages before returning (critical - some message handlers may assume that
      // pointers to the script object are valid, so flushing ensures safety).

      if ((prv->Recurse IS 1) and GetResource(RES::MAIN_THREAD)) {
         ProcessMessages(PMF::NIL, 0);
      }

      return ERR::Okay;
   }
   else {
      // LuaJIT catches C++ exceptions, but we would prefer that crashes occur normally so that they can be traced in
      // the debugger.  As we don't have a solution to this design issue yet, the following context check will suffice
      // to prevent unwanted behaviour.

      if (CurrentContext() != Self) abort(); // A C++ exception was caught by Lua - the software stack is unstable so we must abort.

      if ((prv->Recurse IS 1) and GetResource(RES::MAIN_THREAD)) {
         ProcessMessages(PMF::NIL, 0);
      }

      process_error(Self, Self->Procedure ? Self->Procedure : "run_script");
      return Self->Error;
   }
}

//********************************************************************************************************************

static ERR register_interfaces(objScript *Self)
{
   pf::Log log;

   log.traceBranch("Registering Kotuku and Tiri interfaces with Lua.");

   auto prv = (prvTiri *)Self->ChildPrivate;

   register_io_class(prv->Lua);
   register_module_class(prv->Lua);
   register_regex_class(prv->Lua);
   register_struct_class(prv->Lua);
   register_async_class(prv->Lua);
   #ifndef DISABLE_DISPLAY
      register_input_class(prv->Lua);
   #endif
   register_number_class(prv->Lua);
   register_processing_class(prv->Lua);

   lua_register(prv->Lua, "arg", fcmd_arg);
   lua_register(prv->Lua, "loadFile", fcmd_loadfile);
   lua_register(prv->Lua, "exec", fcmd_exec);
   lua_register(prv->Lua, "print", fcmd_print);
   lua_register(prv->Lua, "include", fcmd_include);
   lua_register(prv->Lua, "msg", fcmd_msg);
   lua_register(prv->Lua, "subscribeEvent", fcmd_subscribe_event);
   lua_register(prv->Lua, "unsubscribeEvent", fcmd_unsubscribe_event);
   lua_register(prv->Lua, "MAKESTRUCT", MAKESTRUCT);

   // Register global function prototypes for compile-time type inference
   reg_func_prototype("arg", { TiriType::Any }, { TiriType::Str, TiriType::Any });
   reg_func_prototype("loadFile", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("exec", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("getExecutionState", { TiriType::Table }, {});
   reg_func_prototype("print", {}, {}, FProtoFlags::Variadic);
   reg_func_prototype("include", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("require", { TiriType::Table }, { TiriType::Str });
   reg_func_prototype("msg", {}, { TiriType::Str }, FProtoFlags::Variadic);
   reg_func_prototype("subscribeEvent", { TiriType::Any, TiriType::Num }, { TiriType::Str, TiriType::Func });
   reg_func_prototype("unsubscribeEvent", {}, { TiriType::Any });
   reg_func_prototype("MAKESTRUCT", { TiriType::Any }, { TiriType::Str });

   load_include(Self, "core");

   return ERR::Okay;
}

//********************************************************************************************************************

#include "tiri_class_methods.cpp"

ERR create_tiri(void)
{
   clTiri = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::SCRIPT),
      fl::ClassID(CLASSID::TIRI),
      fl::ClassVersion(1.0),
      fl::Name("Tiri"),
      fl::Category(CCF::DATA),
      fl::FileExtension("*.tiri|*.tbc"),
      fl::FileDescription("Tiri"),
      fl::Actions(clActions),
      fl::Methods(clMethods),
      fl::Fields(clFields),
      fl::Path(MOD_PATH));

   return clTiri ? ERR::Okay : ERR::AddClass;
}
