/*********************************************************************************************************************

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <inttypes.h>
#include <mutex>

#include "lib.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_object.h"
#include "hashes.h"
#include "defs.h"
#include "lj_proto_registry.h"

/*********************************************************************************************************************
** Usage: proc = processing.new({ timeout=5.0, signals={ obj1, obj2, ... } })
**
** Creates a new processing object.
*/

static int processing_new(lua_State *Lua)
{
   if (auto fp = (fprocessing *)lua_newuserdata(Lua, sizeof(fprocessing))) {
      luaL_getmetatable(Lua, "Tiri.processing");
      lua_setmetatable(Lua, -2);

      // Default configuration
      fp->Timeout = -1;
      fp->Signals = 0;

      if (not (fp->Signals = new (std::nothrow) std::list<ObjectSignal>)) {
         luaL_error(Lua, ERR::Memory);
      }

      if (lua_istable(Lua, 1)) {
         lua_pushnil(Lua);  // Access first key for lua_next()
         while (lua_next(Lua, 1) != 0) {
            if (auto field_name = luaL_checkstring(Lua, -2)) {
               auto field_hash = strihash(field_name);

               switch (field_hash) {
                  case HASH_TIMEOUT:
                     fp->Timeout = lua_tonumber(Lua, -1);
                     break;

                  case HASH_SIGNALS: {
                     if (lua_type(Lua, -1) IS LUA_TARRAY) { // { obj1, obj2, ... }
                        GCarray *arr = lua_toarray(Lua, -1);
                        if (arr->elemtype IS AET::OBJECT) {
                           auto refs = arr->get<GCRef>();
                           for (MSize i = 0; i < arr->len; i++) {
                              if (gcref(refs[i])) {
                                 auto obj = gco_to_object(gcref(refs[i]));
                                 ObjectSignal sig = { .Object = obj->ptr };
                                 fp->Signals->push_back(sig);
                              }
                              else luaL_error(Lua, ERR::InvalidType, "Nil entry at index %d in signal array.", i);
                           }
                        }
                        else luaL_error(Lua, ERR::InvalidType, "The signals option requires an array of objects.");
                     }
                     else luaL_error(Lua, "The signals option requires an array<object> reference.");
                     break;
                  }

                  default:
                     luaL_error(Lua, ERR::UnknownProperty, "Unrecognised option '%s'", field_name);
               }
            }
            else luaL_error(Lua, ERR::UnknownProperty, "Unrecognised option.");

            lua_pop(Lua, 1);  // removes 'value'; keeps 'key' for the proceeding lua_next() iteration
         }
      }

      if (fp->Signals->empty()) { // Monitor the script for a signal if the client did not specify any objects
         ObjectSignal sig = { .Object = Lua->script };
         fp->Signals->push_back(sig);
      }

      return 1;  // new userdatum is already on the stack
   }
   else luaL_error(Lua, "Failed to create new processing object.");

   return 0;
}

//********************************************************************************************************************
// Usage: err = proc.sleep([Seconds], [WakeOnSignal=true])
//
// Puts a process to sleep with message processing in the background.  Can be woken early with a signal (i.e.
// proc.signal()).
//
// Lua's internal signal flag is always reset on entry in case it has been polluted by prior activity.  This behaviour
// can be disabled by setting the third argument to false.
//
// Setting seconds to zero will process outstanding messages and return immediately.
//
// NOTE: Can be called directly as an interface function or as a member of a processing object.
//       Errors are promoted to exceptions if used in a try statement.

static int processing_sleep(lua_State *Lua)
{
   pf::Log log;
   static std::recursive_mutex recursion; // Intentionally accessible to all threads

   ERR error;
   int timeout;

   auto fp = (fprocessing *)get_meta(Lua, lua_upvalueindex(1), "Tiri.processing");
   if (fp) timeout = int(fp->Timeout * 1000.0);
   else timeout = -1;

   if (lua_type(Lua, 1) IS LUA_TNUMBER) timeout = int(lua_tonumber(Lua, 1) * 1000.0);
   if (timeout < 0) timeout = -1; // Wait indefinitely

   bool wake_on_signal;
   if (lua_type(Lua, 2) IS LUA_TBOOLEAN) wake_on_signal = lua_toboolean(Lua, 2);
   else if (!timeout) wake_on_signal = false; // We don't want to intercept signals if just processing messages
   else wake_on_signal = true;

   bool reset_state = true;
   if (lua_type(Lua, 3) IS LUA_TBOOLEAN) reset_state = lua_toboolean(Lua, 3);

   log.branch("Timeout: %d, WakeOnSignal: %c", timeout, wake_on_signal ? 'Y' : 'N');

   if (!timeout) {
      // Always collect your garbage before going to sleep.  Can be prevented with processing.stopCollector() if
      // absolutely necessary.
      if (lua_gc(Lua, LUA_GCISRUNNING, 0)) {
         pf::Log log;
         log.traceBranch("Collecting garbage.");
         lua_gc(Lua, LUA_GCCOLLECT, 0);
      }
   }

   if (wake_on_signal) {
      if ((fp) and (fp->Signals) and (not fp->Signals->empty())) {
         // Use custom signals provided by the client (or Tiri if no objects were specified).
         auto signal_list_c = std::make_unique<ObjectSignal[]>(fp->Signals->size() + 1);
         int i = 0;
         for (auto &entry : *fp->Signals) signal_list_c[i++] = entry;
         signal_list_c[i].Object = nullptr;

         std::scoped_lock lock(recursion);
         error = WaitForObjects(timeout IS -1 ? PMF::EVENT_LOOP : PMF::NIL, timeout, signal_list_c.get());
      }
      else { // Default behaviour: Sleeping can be broken with a signal to the Tiri object.
         if (Lua->script->defined(NF::SIGNALLED)) {
            log.detail("Lua script already in signalled state.");
            Lua->script->clearFlag(NF::SIGNALLED);
            error = ERR::Okay;
         }
         else {
            ObjectSignal signal_list_c[2] = { { .Object = Lua->script }, { .Object = nullptr } };
            std::scoped_lock lock(recursion);
            error = WaitForObjects(timeout IS -1 ? PMF::EVENT_LOOP : PMF::NIL, timeout, signal_list_c);
         }
      }
   }
   else { // Ignore signals, just process messages for the specified time
      std::scoped_lock lock(recursion);
      WaitTime(timeout / 1000.0); // Convert milliseconds to seconds
      error = ERR::Okay;
   }

   // Promote errors to exceptions
   if ((error != ERR::Okay) and (in_try_immediate_scope(Lua))) luaL_error(Lua, error);

   lua_pushinteger(Lua, int(error));
   return 1;
}

//********************************************************************************************************************
// Usage: proc.signal() or processing.signal()
//
// Signals the Tiri object.  Note that this is ineffective if the user provided a list of objects to monitor for signaling.

static int processing_signal(lua_State *Lua)
{
   Action(AC::Signal, Lua->script, nullptr);
   return 0;
}

//********************************************************************************************************************
// Usage: processing.flush()
//
// Flushes any pending signals from the Tiri object.

static int processing_flush(lua_State *Lua)
{
   Lua->script->clearFlag(NF::SIGNALLED);
   return 0;
}

//********************************************************************************************************************

static int processing_stop_collector(lua_State *Lua)
{
   lua_gc(Lua, LUA_GCSTOP, 0);
   return 0;
}

//********************************************************************************************************************

static int processing_start_collector(lua_State *Lua)
{
   lua_gc(Lua, LUA_GCRESTART, 0);
   return 0;
}

//********************************************************************************************************************
// Usage: processing.collect([mode], [options])
//
// Controls the garbage collector.
//
// Modes:
//   "full"    - Full collection cycle (default)
//   "step"    - Incremental collection step
//
// Options table (for "step" mode):
//   stepSize  - Size of the incremental step

static int processing_collect(lua_State *Lua)
{
   int gc_mode = LUA_GCCOLLECT;  // Default: full collection
   int step_size = 0;

   // Arg 1: Optional mode string

   if (lua_type(Lua, 1) IS LUA_TSTRING) {
      auto mode_str = lua_tostring(Lua, 1);
      if (std::string_view("full") IS mode_str) gc_mode = LUA_GCCOLLECT;
      else if (std::string_view("step") IS mode_str) gc_mode = LUA_GCSTEP;
      else luaL_error(Lua, "Invalid mode '%s'. Use 'full', 'step'.", mode_str);
   }

   // Arg 2: Optional options table

   if (lua_istable(Lua, 2)) {
      lua_getfield(Lua, 2, "stepSize");
      if (lua_type(Lua, -1) IS LUA_TNUMBER) {
         step_size = lua_tointeger(Lua, -1);
      }
      lua_pop(Lua, 1);
   }

   int result = lua_gc(Lua, gc_mode, step_size);
   lua_pushinteger(Lua, result);
   return 1;
}

//********************************************************************************************************************
// Usage: stats = processing.gcStats()
//
// Returns a table containing garbage collector statistics:
//   memoryKB    - Memory usage in kilobytes
//   memoryBytes - Remainder bytes (memoryKB * 1024 + memoryBytes = total bytes)
//   memoryMB    - Total memory usage in megabytes (convenience field)
//   isRunning   - Boolean indicating if the GC is currently running
//   pause       - Current pause multiplier (controls GC frequency)
//   stepMul     - Current step multiplier (controls GC speed)

static int processing_gcStats(lua_State *Lua)
{
   lua_createtable(Lua, 0, 6);  // Pre-allocate for 6 fields

   // Memory usage
   int kb = lua_gc(Lua, LUA_GCCOUNT, 0);
   int bytes = lua_gc(Lua, LUA_GCCOUNTB, 0);
   double mb = kb / 1024.0 + bytes / (1024.0 * 1024.0);

   lua_pushinteger(Lua, kb);
   lua_setfield(Lua, -2, "memoryKB");

   lua_pushinteger(Lua, bytes);
   lua_setfield(Lua, -2, "memoryBytes");

   lua_pushnumber(Lua, mb);
   lua_setfield(Lua, -2, "memoryMB");

   // GC state
   lua_pushboolean(Lua, lua_gc(Lua, LUA_GCISRUNNING, 0));
   lua_setfield(Lua, -2, "isRunning");

   // Current tuning parameters (query by setting to same value)
   int pause = lua_gc(Lua, LUA_GCSETPAUSE, 200);
   lua_gc(Lua, LUA_GCSETPAUSE, pause);  // Restore
   lua_pushinteger(Lua, pause);
   lua_setfield(Lua, -2, "pause");

   int step_mul = lua_gc(Lua, LUA_GCSETSTEPMUL, 200);
   lua_gc(Lua, LUA_GCSETSTEPMUL, step_mul);  // Restore
   lua_pushinteger(Lua, step_mul);
   lua_setfield(Lua, -2, "stepMul");

   return 1;
}

//********************************************************************************************************************
// Usage: task = processing.task()
//
// Returns an object that references the current task.

static int processing_task(lua_State *Lua)
{
   auto prv = (prvTiri *)Lua->script->ChildPrivate;
   GCobject *obj = push_object(prv->Lua, CurrentTask());
   obj->set_detached(true);  // External reference
   return 1;
}

//********************************************************************************************************************
// Internal: Processing index call - for objects returned from processing.new() only.

static int processing_get(lua_State *Lua)
{
   if (auto fieldname = luaL_checkstring(Lua, 2)) {
      if (std::string_view("sleep") IS fieldname) {
         lua_pushvalue(Lua, 1);
         lua_pushcclosure(Lua, &processing_sleep, 1);
         return 1;
      }
      else if (std::string_view("signal") IS fieldname) {
         lua_pushvalue(Lua, 1);
         lua_pushcclosure(Lua, &processing_signal, 1);
         return 1;
      }
      else if (std::string_view("flush") IS fieldname) {
         Lua->script->clearFlag(NF::SIGNALLED);
         if (auto fp = (fprocessing *)get_meta(Lua, lua_upvalueindex(1), "Tiri.processing")) {
            for (auto &entry : *fp->Signals) {
               entry.Object->clearFlag(NF::SIGNALLED);
            }
         }
         return 0;
      }
      else luaL_error(Lua, "Unrecognised index '%s'", fieldname);
   }

   return 0;
}

//********************************************************************************************************************
// Call a function on the next message processing cycle.
//
// Usage: processing.delayedCall(function() ... end)

static MsgHandler *delayed_call_handle;

static ERR msg_handler(APTR Meta, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   if (MsgSize != sizeof(int)) return pf::Log(__FUNCTION__).warning(ERR::Args);

   auto lua = (lua_State *)Meta;
   auto prv = (prvTiri *)lua->script->ChildPrivate;
   int ref = *(int *)Message;
   lua_rawgeti(lua, LUA_REGISTRYINDEX, ref); // Get the function from the registry
   luaL_unref(lua, LUA_REGISTRYINDEX, ref); // Remove it
   if (lua_pcall(prv->Lua, 0, 0, 0)) {
      process_error(lua->script, "delayedCall()");
   }
   return ERR::Okay;
}

static int processing_delayed_call(lua_State *Lua)
{
   static MSGID msgid = MSGID::NIL;
   if (msgid IS MSGID::NIL) {
      msgid = MSGID(AllocateID(IDTYPE::MESSAGE));
      auto func = C_FUNCTION(msg_handler, Lua);
      if (auto error = AddMsgHandler(msgid, &func, &delayed_call_handle); error != ERR::Okay) {
         luaL_error(Lua, error);
      }
   }

   if (lua_type(Lua, 1) IS LUA_TFUNCTION) {
      int ref = luaL_ref(Lua, LUA_REGISTRYINDEX);
      SendMessage(msgid, MSF::NIL, &ref, sizeof(ref));
   }
   else luaL_error(Lua, "Expected a function to register as a message hook.");
   return 0;
}

//********************************************************************************************************************
// Garbage collector.

static int processing_destruct(lua_State *Lua)
{
   auto fp = (fprocessing *)luaL_checkudata(Lua, 1, "Tiri.processing");
   if (fp->Signals) { delete fp->Signals; fp->Signals = nullptr; }
   return 0;
}

//********************************************************************************************************************
// Register the processing interface.

static const luaL_Reg processinglib_functions[] = {
   { "new",            processing_new },
   { "collect",        processing_collect },
   { "stopCollector",  processing_stop_collector },
   { "startCollector", processing_start_collector },
   { "gcStats",        processing_gcStats },
   { "sleep",          processing_sleep },
   { "signal",         processing_signal },
   { "task",           processing_task },
   { "flush",          processing_flush },
   { "delayedCall",    processing_delayed_call },
   { nullptr, nullptr }
};

static const luaL_Reg processinglib_methods[] = {
   { "__index",    processing_get },
   { "__gc",       processing_destruct },
   { nullptr, nullptr }
};

void register_processing_class(lua_State *Lua)
{
   pf::Log log(__FUNCTION__);
   log.trace("Registering processing interface.");

   luaL_newmetatable(Lua, "Tiri.processing");
   lua_pushstring(Lua, "Tiri.processing");
   lua_setfield(Lua, -2, "__name");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable created earlier
   lua_settable(Lua, -3);   // metatable.__index = metatable
   luaL_openlib(Lua, nullptr, processinglib_methods, 0);

   luaL_openlib(Lua, "processing", processinglib_functions, 0);

   // Register processing interface prototypes for compile-time type inference
   reg_iface_prototype("processing", "new", { TiriType::Any }, { TiriType::Table });
   reg_iface_prototype("processing", "collect", { TiriType::Num }, { TiriType::Str, TiriType::Table });
   reg_iface_prototype("processing", "stopCollector", {}, {});
   reg_iface_prototype("processing", "startCollector", {}, {});
   reg_iface_prototype("processing", "gcStats", { TiriType::Table }, {});
   reg_iface_prototype("processing", "sleep", { TiriType::Num }, { TiriType::Num, TiriType::Bool, TiriType::Bool });
   reg_iface_prototype("processing", "signal", {}, {});
   reg_iface_prototype("processing", "task", { TiriType::Any }, {});
   reg_iface_prototype("processing", "flush", {}, {});
   reg_iface_prototype("processing", "delayedCall", {}, { TiriType::Func });
}
