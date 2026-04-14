
#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE

#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/strings.hpp>

#include <inttypes.h>
#include <mutex>
#include <algorithm>
#include <string_view>
#include <vector>

#include "lua.h"
#include "lj_obj.h"
#include "lj_str.h"
#include "parser/parser_diagnostics.h"

#include "hashes.h"
#include "defs.h"

static int lua_load(lua_State *Lua, class objFile *File, CSTRING SourceName)
{
   std::string buffer;
   auto filesize = File->get<int>(FID_Size);
   buffer.resize(filesize);
   File->read(buffer.data(), filesize);

   return lua_load(Lua, std::string_view(buffer.data(), buffer.size()), SourceName);
}

//********************************************************************************************************************
// lua_load() failures are handled here.  At least one parser diagnostic is expected - failure to produce a
// diagnostic requires further investigation and a fix to the parser code.

[[noreturn]] static void lua_load_failed(lua_State *Lua)
{
   if (Lua->parser_diagnostics and Lua->parser_diagnostics->has_errors()) {
      std::string msg;
      for (const auto &entry : Lua->parser_diagnostics->entries()) {
         if (not msg.empty()) msg += "\n";
         msg += entry.to_string(Lua->script->LineOffset);
      }
      luaL_error(Lua, "%s", msg.c_str());
   }
   else if (auto error_msg = lua_tostring(Lua, -1)) {
      // When not in diagnose mode, errors are thrown via lj_err_lex which pushes the message to the stack
      luaL_error(Lua, "%s", error_msg);
   }
   else luaL_error(Lua, "Parsing failed but no diagnostics are available.");
}

//********************************************************************************************************************
// The event callback will be called with the following prototype:
//
// function callback(EventID, Args)
//
// Where Args is a named array containing the event parameters.  If the event is not known to Tiri, then no Args will
// be provided.

static void receive_event(pf::Event *Info, int InfoSize, APTR CallbackMeta)
{
   auto Script = (objScript *)CurrentContext();
   auto prv = (prvTiri *)Script->ChildPrivate;
   if (not prv) return;

   pf::Log log(__FUNCTION__);
   log.trace("Received event $%.8x%.8x", (int)((Info->EventID>>32) & 0xffffffff), (int)(Info->EventID & 0xffffffff));

   lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, intptr_t(CallbackMeta));

   lua_pushnumber(prv->Lua, Info->EventID);
   if (lua_pcall(prv->Lua, 1, 0, 0)) {
      process_error(Script, "Event Subscription");
   }

   if (lua_gc(prv->Lua, LUA_GCISRUNNING, 0)) {
      log.traceBranch("Collecting garbage.");
      lua_gc(prv->Lua, LUA_GCCOLLECT, 0); // Run the garbage collector
   }
}

//********************************************************************************************************************
// Usage: unsubscribeEvent(handle)

int fcmd_unsubscribe_event(lua_State *Lua)
{
   auto prv = (prvTiri *)Lua->script->ChildPrivate;
   if (not prv) return 0;

   if (auto handle = lua_touserdata(Lua, 1)) {
      pf::Log log("unsubscribe_event");
      if ((Lua->script->Flags & SCF::LOG_ALL) != SCF::NIL) log.msg("Handle: %p", handle);

      auto erased = std::erase_if(prv->EventList, [&](const auto& event) {
         if (event.EventHandle IS handle) {
            luaL_unref(prv->Lua, LUA_REGISTRYINDEX, event.Function);
            return true;
         }
         return false;
      });

      if (erased > 0) return 0;

      log.warning("Failed to link an event to handle %p.", handle);
   }
   else luaL_argerror(Lua, 1, "No handle provided.");

   return 0;
}

//********************************************************************************************************************
// Usage: error, handle = subscribeEvent("group.subgroup.name", function)

int fcmd_subscribe_event(lua_State *Lua)
{
   CSTRING event;
   if (not (event = lua_tostring(Lua, 1))) {
      luaL_argerror(Lua, 1, "Event string expected.");
      return 0;
   }

   if (not lua_isfunction(Lua, 2)) {
      if (not lua_isnil(Lua, 2)) {
         luaL_argerror(Lua, 2, "Function or nil expected.");
         return 0;
      }
   }

   // Generate the event ID

   constexpr size_t GROUP_BUFFER_SIZE = 60;
   uint32_t group_hash = 0, subgroup_hash = 0;
   std::string_view event_view(event);
   std::string group_str, subgroup_str;

   auto first_dot = event_view.find('.');
   if (first_dot != std::string_view::npos) {
      auto group_part = event_view.substr(0, first_dot);
      if (group_part.size() >= GROUP_BUFFER_SIZE) luaL_error(Lua, ERR::BufferOverflow);

      group_str = std::string(group_part);
      group_hash = strihash(group_str.c_str());
      event_view.remove_prefix(first_dot + 1);

      auto second_dot = event_view.find('.');
      if (second_dot != std::string_view::npos) {
         auto subgroup_part = event_view.substr(0, second_dot);
         if (subgroup_part.size() >= GROUP_BUFFER_SIZE) luaL_error(Lua, ERR::BufferOverflow);

         subgroup_str = std::string(subgroup_part);
         subgroup_hash = strihash(subgroup_str.c_str());
         event_view.remove_prefix(second_dot + 1);
         event = event_view.data();
      }
   }

   auto group_id = EVG::NIL;
   if ((group_hash) and (subgroup_hash)) {
      switch (group_hash) {
         case HASH_FILESYSTEM: group_id = EVG::FILESYSTEM; break;
         case HASH_NETWORK:    group_id = EVG::NETWORK; break;
         case HASH_USER:       group_id = EVG::USER; break;
         case HASH_SYSTEM:     group_id = EVG::SYSTEM; break;
         case HASH_GUI:        group_id = EVG::GUI; break;
         case HASH_DISPLAY:    group_id = EVG::DISPLAY; break;
         case HASH_IO:         group_id = EVG::IO; break;
         case HASH_HARDWARE:   group_id = EVG::HARDWARE; break;
         case HASH_AUDIO:      group_id = EVG::AUDIO; break;
         case HASH_POWER:      group_id = EVG::POWER; break;
         case HASH_CLASS:      group_id = EVG::CLASS; break;
         case HASH_APP:        group_id = EVG::APP; break;
      }
   }

   if (group_id IS EVG::NIL) {
      luaL_error(Lua, "Invalid group name '%s' in event string.", event);
      return 0;
   }

   EVENTID event_id = GetEventID(group_id, subgroup_str.c_str(), event);

   if (not event_id) {
      luaL_argerror(Lua, 1, "Failed to build event ID.");
      lua_pushinteger(Lua, int(ERR::Failed));
      return 1;
   }
   else {
      APTR handle;
      lua_settop(Lua, 2);
      auto client_function = luaL_ref(Lua, LUA_REGISTRYINDEX);
      if (auto error = SubscribeEvent(event_id, C_FUNCTION(receive_event, client_function), &handle); error IS ERR::Okay) {
         auto prv = (prvTiri *)Lua->script->ChildPrivate;
         prv->EventList.emplace_back(client_function, event_id, handle);
         lua_pushlightuserdata(Lua, handle); // 1: Handle
         lua_pushinteger(Lua, int(error)); // 2: Error code
      }
      else {
         lua_pushnil(Lua); // Handle
         lua_pushinteger(Lua, int(error)); // Error code
      }
      return 2;
   }
}

//********************************************************************************************************************
// Usage: msg("Message")
// Prints a debug message, with no support for input parameters.  This is the safest way to call LogF().

int fcmd_msg(lua_State *Lua)
{
   int n = lua_gettop(Lua);  // number of arguments
   lua_getglobal(Lua, "tostring");
   for (int i=1; i <= n; i++) {
      lua_pushvalue(Lua, -1);  // function to be called (tostring)
      lua_pushvalue(Lua, i);   // value to pass to tostring
      lua_call(Lua, 1, 1);
      CSTRING s = lua_tostring(Lua, -1);  // get result
      if (not s) luaL_error(Lua, LUA_QL("tostring") " must return a string to " LUA_QL("print"));

      {
         pf::Log log("Tiri");
         log.msg("%s", s);
      }

      lua_pop(Lua, 1);  // pop the string result
   }
   return 0;
}

//********************************************************************************************************************
// Usage: print(...)
// Prints a message to stderr.  On Android stderr is unavailable, so the message is printed in the debug output.

int fcmd_print(lua_State *Lua)
{
   int n = lua_gettop(Lua);  // number of arguments
   lua_getglobal(Lua, "tostring");
   for (int i=1; i <= n; i++) {
      lua_pushvalue(Lua, -1);  // function to be called
      lua_pushvalue(Lua, i);   // value to print
      lua_call(Lua, 1, 1);
      CSTRING s = lua_tostring(Lua, -1);  // get result
      if (not s) luaL_error(Lua, LUA_QL("tostring") " must return a string to " LUA_QL("print"));

      #ifdef __ANDROID__
         {
            pf::Log log("Tiri");
            log.msg("%s", s);
         }
      #else
         fprintf(stderr, "%s", s);
      #endif

      lua_pop(Lua, 1);  // pop result
   }
   fprintf(stderr, "\n");
   return 0;
}

//********************************************************************************************************************
// Usage: include "Module1","Module2","Module3",...
// Loads the constants for a module without the overhead of creating a module object.

int fcmd_include(lua_State *Lua)
{
   if (not lua_isstring(Lua, 1)) {
      luaL_argerror(Lua, 1, "Include name(s) required.");
      return 0;
   }

   int top = lua_gettop(Lua);
   for (int n=1; n <= top; n++) {
      CSTRING include = lua_tostring(Lua, n);

      // For security purposes, check the validity of the include name.

      int i;
      for (i=0; include[i]; i++) {
         if ((include[i] >= 'a') and (include[i] <= 'z')) continue;
         if ((include[i] >= 'A') and (include[i] <= 'Z')) continue;
         if ((include[i] >= '0') and (include[i] <= '9')) continue;
         break;
      }

      if ((include[i]) or (i >= 32)) {
         luaL_error(Lua, "Invalid module name; only alpha-numeric names are permitted with max 32 chars.");
         return 0;
      }

      if (auto error = load_include(Lua->script, include); error != ERR::Okay) {
         if (error IS ERR::FileNotFound) luaL_error(Lua, "Requested include file '%s' does not exist.", include);
         else luaL_error(Lua, error, "Failed to process include file: %s", GetErrorMsg(error));
         return 0;
      }
   }

   return 0;
}

//********************************************************************************************************************
// Usage: results = loadFile("Path")
//
// Loads a Tiri language file from any location and executes it.  Any return values from the script will be returned
// as-is.  Any error that occurs will be thrown with a descriptive string.

int fcmd_loadfile(lua_State *Lua)
{
   CSTRING error_msg = nullptr;
   int results = 0;
   ERR error = ERR::Okay;

   if (auto path = lua_tostring(Lua, 1)) {
      pf::Log log("loadfile");

      log.branch("%s", path);

      bool recompile = false;
      CSTRING src = path;

      #if 0
      int pathlen = strlen(path);
      char fbpath[pathlen+6];
      if (iequals(".tiri", path + pathlen - 6)) {
         // File is a .tiri.  Let's check if a .fb exists and is date-stamped for the same date as the .tiri version.
         // Note: The developer can also delete the .tiri file in favour of a .fb that is already present (for
         // production releases)

         StrCopy(path, fbpath, pathlen - 5);
         StrCopy(".fb", fbpath + pathlen - 6);

         log.msg("Checking for a compiled Tiri file: %s", fbpath);

         objFile::create fb_file = { fl::Path(fbpath) };
         if (fb_file.ok()) { // A compiled version exists.  Compare datestamps
            objFile::create src_file = { fl::Path(path) };
            if (src_file.ok()) {
               int64_t fb_ts, src_ts;
               fb_file->get(FID_TimeStamp, &fb_ts);
               src_file->get(FID_TimeStamp, &src_ts);

               if (fb_ts != src_ts) {
                  log.msg("Timestamp mismatch, will recompile the cached version.");
                  recompile = true;
                  error = ERR::Failed;
               }
               else src = fbpath;
            }
            else if (error IS ERR::FileNotFound) {
               src = fbpath; // Use the .fb if the developer removed the .tiri (typically done for production releases)
            }
         }
      }
      #endif

      objFile::create file = { fl::Path(src), fl::Flags(FL::READ) };
      if (file.ok()) {
         // Check for the presence of a compiled header and skip it if present

         {
            int len, i;
            char header[256];
            if (file->read(header, sizeof(header), &len) IS ERR::Okay) {
               if (pf::startswith(LUA_COMPILED, std::string_view(header, sizeof(header)))) {
                  recompile = false; // Do not recompile that which is already compiled
                  for (i=sizeof(LUA_COMPILED)-1; (i < len) and (header[i]); i++);
                  if (not header[i]) i++;
                  else i = 0;
               }
               else i = 0;
            }
            else i = 0;

            file->setPosition(i);
         }

#ifdef SHORT_TIRI_PATHS
         int i;
         for (i=strlen(path); i > 0; i--) { // Get the file name from the path
            if ((path[i-1] IS '\\') or (path[i-1] IS '/') or (path[i-1] IS ':')) break;
         }

         // Prefix chunk name with '@' (Lua convention for file-based chunks) for better debug output
         std::string chunk_name = std::string("@") + (path + i);
#else
         // Resolve the full path for the chunk name (needed for import statement path resolution)
         std::string resolved_path;
         if (ResolvePath(path, RSF::NIL, &resolved_path) IS ERR::Okay) {
            // Use resolved path for chunk name
         }
         else resolved_path = path;  // Fall back to original if resolution fails

         // Prefix chunk name with '@' (Lua convention for file-based chunks) for better debug output
         std::string chunk_name = std::string("@") + resolved_path;
#endif

         if (not lua_load(Lua, *file, chunk_name.c_str())) {
            // TODO Code compilation not currently supported
         /*
            if (recompile) {
               objFile::create cachefile = {
                  fl::Path(fbpath),
                  fl::Flags(FL::NEW|FL::WRITE),
                  fl::Permissions(PERMIT::USER_READ|PERMIT::USER_WRITE)
               };

               if (cachefile.ok()) {
                  const Proto *f;
                  struct DateTime *date;
                  f = clvalue(Lua->top + (-1))->l.p;
                  luaU_dump(Lua, f, &code_writer, cachefile, (Self->Flags & SCF_DEBUG) ? 0 : 1);
                  if (not file.obj->getPtr(FID_Date, &date)) {
                     cachefile->setDate(date);
                  }
               }
            }
            */

            int result_top = lua_gettop(Lua);

            if (not lua_pcall(Lua, 0, LUA_MULTRET, 0)) {
               results = lua_gettop(Lua) - result_top + 1;
            }
            else error_msg = lua_tostring(Lua, -1);
         }
         else { lua_load_failed(Lua); return 0; }
      }
      else error = ERR::DoesNotExist;

      if ((not error_msg) and (error != ERR::Okay)) error_msg = GetErrorMsg(error);
      if (error_msg) luaL_error(Lua, "Failed to load/parse file '%s', error: %s", path, error_msg);
   }
   else luaL_argerror(Lua, 1, "File path required.");

   return results;
}

//********************************************************************************************************************
// Usage: exec(Statement)
//
// Executes a string statement within a pcall.  Returns results if there are any.  An exception will be raised if an
// error occurs during statement execution.

int fcmd_exec(lua_State *Lua)
{
   int results = 0;

   size_t len;
   if (auto statement = lua_tolstring(Lua, 1, &len)) {
      CSTRING error_msg = nullptr;

      {
         pf::Log log("exec");
         log.branch();

         // Check for the presence of a compiled header and skip it if present

         if (pf::startswith(LUA_COMPILED, std::string_view(statement, len))) {
            size_t i;
            for (i=sizeof(LUA_COMPILED)-1; statement[i]; i++);
            statement += i + 1;
            len -= (i + 1);
         }

         if (not lua_load(Lua, std::string_view(statement, len), "exec")) {
            int result_top = lua_gettop(Lua);
            if (not lua_pcall(Lua, 0, LUA_MULTRET, 0)) {
               results = lua_gettop(Lua) - result_top + 1;
            }
            else error_msg = lua_tostring(Lua, -1);
         }
         else { lua_load_failed(Lua); return 0; }
      }

      if (error_msg) luaL_error(Lua, error_msg);
   }
   else luaL_argerror(Lua, 1, "Tiri statement required.");

   return results;
}

//********************************************************************************************************************
// Usage: arg = arg("Width", IfNullValue)
//
// NB: Arguments are set as variables and this is managed in the base Script class.

int fcmd_arg(lua_State *Lua)
{
   objScript *Self = Lua->script;

   int args = lua_gettop(Lua);

   auto key = lua_tostring(Lua, 1);
   if (auto it = Self->Vars.find(key); it != Self->Vars.end()) {
      lua_pushstring(Lua, it->second.c_str());
      return 1;
   }

   if (args IS 2) return 1; // Return value 2 (top of the stack)
   else {
      lua_pushnil(Lua);
      return 1;
   }
}
