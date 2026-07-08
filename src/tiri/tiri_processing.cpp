
#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <format>
#include <mutex>
#include <utility>

#include "lib.h"
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "hashes.h"
#include "defs.h"
#include "lj_proto_registry.h"

//********************************************************************************************************************
// Usage: proc = processing.new({ timeout=5.0, signals={ obj1, obj2, ... } })
//
// Creates a new processing object.

static int processing_new(lua_State *Lua)
{
   if (auto fp = (fprocessing *)lua_newuserdata(Lua, sizeof(fprocessing))) {
      luaL_getmetatable(Lua, "Tiri.processing");
      lua_setmetatable(Lua, -2);

      // Default configuration
      fp->Timeout = -1;
      fp->Signals = 0;
      fp->SignalRefs = 0;

      auto fail = [&](ERR Error, std::string Message) -> int {
         if (fp->SignalRefs) {
            for (auto ref : *fp->SignalRefs) luaL_unref(Lua, LUA_REGISTRYINDEX, ref);
            delete fp->SignalRefs;
            fp->SignalRefs = nullptr;
         }
         if (fp->Signals) {
            delete fp->Signals;
            fp->Signals = nullptr;
         }

         luaL_error(Lua, Error, std::move(Message));
      };

      if (not (fp->Signals = new (std::nothrow) std::list<ObjectSignal>)) {
         return fail(ERR::Memory, std::format("Failed to initialise processing signal list."));
      }

      if (not (fp->SignalRefs = new (std::nothrow) std::list<int>)) {
         return fail(ERR::Memory, std::format("Failed to initialise processing signal references."));
      }

      if (lua_istable(Lua, 1)) {
         lua_pushnil(Lua);  // Access first key for lua_next()
         while (lua_next(Lua, 1) != 0) {
            if (auto field_name = lua_tostring(Lua, -2)) {
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
                                 ObjectSignal sig = { .Object = obj->ptr ? obj->ptr : GetObjectPtr(obj->uid) };
                                 if (not sig.Object) {
                                    return fail(ERR::AccessObject,
                                       std::format("Signal object at index {} is not available.", i));
                                 }
                                 fp->Signals->push_back(sig);
                                 setobjectV(Lua, Lua->top++, obj);
                                 fp->SignalRefs->push_back(luaL_ref(Lua, LUA_REGISTRYINDEX));
                              }
                              else {
                                 return fail(ERR::InvalidType,
                                    std::format("Nil entry at index {} in signal array.", i));
                              }
                           }
                        }
                        else {
                           return fail(ERR::InvalidType,
                              std::format("The signals option requires an array of objects."));
                        }
                     }
                     else {
                        return fail(ERR::InvalidType,
                           std::format("The signals option requires an array<object> reference."));
                     }
                     break;
                  }

                  default:
                     return fail(ERR::UnknownProperty, std::format("Unrecognised option '{}'", field_name));
               }
            }
            else return fail(ERR::UnknownProperty, std::format("Unrecognised option."));

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
// Usage: proc.halt(Seconds)
//
// Puts a process to sleep with message processing in the background.  Cannot be woken early.
//
// Setting seconds to zero will process outstanding messages and return immediately.

static int processing_halt(lua_State *Lua)
{
   double seconds;
   if (lua_type(Lua, 1) IS LUA_TNUMBER) seconds = lua_tonumber(Lua, 1);
   else luaL_argerror(Lua, 1, "Seconds must be a number.");

   if (seconds < 0) luaL_error(Lua, ERR::Args, "Seconds must be a positive number.");

   kt::Log log("processing.halt");
   log.branch("Timeout: %.2f", seconds);

   // Always collect your garbage before going to sleep.  Can be prevented with processing.stopCollector() if
   // absolutely necessary.

   if ((seconds != 0) and (lua_gc(Lua, LUA_GCISRUNNING, 0))) {
      kt::Log log;
      log.traceBranch("Collecting garbage.");
      lua_gc(Lua, LUA_GCCOLLECT, 0);
   }

   WaitTime(seconds);
   return 0;
}

//********************************************************************************************************************
// Usage: err = proc.sleep([Seconds])
//
// Puts a process to sleep with message processing in the background.  Can be woken early with a signal to a monitored
// object (or all objects if multiple are listed).  If no objects are monitored, proc.signal() can be used to wake
// the process.
//
// Setting seconds to zero will process outstanding messages and return immediately.
// A negative or nil Seconds value will wait indefinitely for a signal.
// Returns ERR_Timeout if the timeout expires.
//
// NOTE: Can be called directly as an interface function or as a member of a processing object.
//       Errors are promoted to exceptions if used in a try statement.

static int processing_sleep(lua_State *Lua)
{
   kt::Log log("processing.sleep");
   static std::recursive_mutex recursion; // Intentionally accessible to all threads

   auto fp = (fprocessing *)get_meta(Lua, lua_upvalueindex(1), "Tiri.processing");
   double seconds = fp ? fp->Timeout : -1;

   if (lua_type(Lua, 1) IS LUA_TNUMBER) seconds = lua_tonumber(Lua, 1);
   if (seconds < 0) seconds = -1; // Wait indefinitely

   log.branch("Timeout: %g", seconds);

   ERR error;
   if (seconds != 0) {
      // Always collect your garbage before going to sleep.  Can be prevented with processing.stopCollector() if
      // absolutely necessary.
      if (lua_gc(Lua, LUA_GCISRUNNING, 0)) {
         kt::Log log;
         log.traceBranch("Collecting garbage.");
         lua_gc(Lua, LUA_GCCOLLECT, 0);
      }

      if ((fp) and (fp->Signals) and (not fp->Signals->empty())) {
         // Use custom signals provided by the client
         auto signal_list_c = std::make_unique<ObjectSignal[]>(fp->Signals->size() + 1);
         int i = 0;
         for (auto &entry : *fp->Signals) signal_list_c[i++] = entry;
         signal_list_c[i].Object = nullptr;

         std::scoped_lock lock(recursion);
         auto timeout = int(seconds * 1000.0);
         error = WaitForObjects(timeout IS -1 ? PMF::EVENT_LOOP : PMF::NIL, timeout, signal_list_c.get());
      }
      else { // Default behaviour: Sleeping can be broken with a signal to the Tiri object.
         if (Lua->script->defined(NF::SIGNALLED)) {
            log.detail("Tiri script already in signalled state.");
            Lua->script->clearFlag(NF::SIGNALLED);
            error = ERR::Okay;
         }
         else {
            ObjectSignal signal_list_c[2] = { { .Object = Lua->script }, { .Object = nullptr } };
            std::scoped_lock lock(recursion);
            auto timeout = int(seconds * 1000.0);
            error = WaitForObjects(timeout IS -1 ? PMF::EVENT_LOOP : PMF::NIL, timeout, signal_list_c);
         }
      }
   }
   else error = ProcessMessages(PMF::NIL, 0);

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
   kt::Log log("processing.signal");
   log.msg("Signaling Tiri object #%d.", Lua->script->UID);
   if (auto error = Action(AC::Signal, Lua->script, nullptr); error != ERR::Okay) luaL_error(Lua, error);
   return 0;
}

//********************************************************************************************************************
// Usage: processing.flush()
//
// Flushes any pending signals from the Tiri object.

static int processing_flush(lua_State *Lua)
{
   Lua->script->clearFlag(NF::SIGNALLED);
   if (auto fp = (fprocessing *)get_meta(Lua, lua_upvalueindex(1), "Tiri.processing")) {
      if ((fp->SignalRefs) and (not fp->SignalRefs->empty())) {
         for (auto ref : *fp->SignalRefs) {
            lua_rawgeti(Lua, LUA_REGISTRYINDEX, ref);
            if (lua_type(Lua, -1) IS LUA_TOBJECT) {
               auto object = lua_toobject(Lua, -1);
               if (auto obj = access_object(object)) {
                  obj->clearFlag(NF::SIGNALLED);
                  release_object(object);
               }
            }
            lua_pop(Lua, 1);
         }
      }
      else if ((fp->Signals) and (not fp->Signals->empty())) {
         Lua->script->clearFlag(NF::SIGNALLED);
      }
   }
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
// Use "step" when a script needs to spread collection work across regular update or idle points instead of pausing for
// a full collection.  It is most useful in interactive loops or long-running tasks where temporary allocations are
// expected and latency matters; call it repeatedly with a small stepSize rather than as a one-off replacement for
// "full".
//
// stepSize is a work budget in kibibytes, not the amount of memory that will be reclaimed.  Larger values let the
// collector do more work per call and can complete cycles sooner, but they also increase the pause caused by that call.
// For frequent calls from a frame or event loop, start with a small value such as 16-64 and increase it only if memory
// keeps growing between steps.  Values around 100 or more are better suited to idle-time or background catch-up work.
//
// Options table (for "step" mode):
//   stepSize  - Incremental step work budget in kibibytes

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

   auto result = lua_gc(Lua, gc_mode, step_size);
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
   GCobject *obj = push_object(Lua, CurrentTask());
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
         lua_pushvalue(Lua, 1);
         lua_pushcclosure(Lua, &processing_flush, 1);
         return 1;
      }
      else luaL_error(Lua, "Unrecognised index '%s'", fieldname);
   }

   return 0;
}

//********************************************************************************************************************
// Call a function on the next message processing cycle.
//
// Usage: processing.delayedCall(function() ... end)

struct delay_msg {
   lua_State *lua;
   int ref;
};

ERR delayed_msg_handler(APTR Meta, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   if (MsgSize != sizeof(delay_msg)) return kt::Log(__FUNCTION__).warning(ERR::Args);

   auto msg = (delay_msg *)Message;
   auto lua = msg->lua;
   lua_rawgeti(lua, LUA_REGISTRYINDEX, msg->ref); // Get the function from the registry
   luaL_unref(lua, LUA_REGISTRYINDEX, msg->ref); // Remove it

   kt::SwitchContext ctx(lua->script);
   if (lua_pcall(lua, 0, 0, 0)) {
      process_error(lua->script, "delayedCall()");
   }
   return ERR::Okay;
}

static int processing_delayed_call(lua_State *Lua)
{
   if (lua_type(Lua, 1) IS LUA_TFUNCTION) {
      delay_msg msg = { Lua, luaL_ref(Lua, LUA_REGISTRYINDEX) };
      if (SendMessage(glDelayedCallMsgID, MSF::NIL, &msg, sizeof(msg)) != ERR::Okay) {
         luaL_unref(Lua, LUA_REGISTRYINDEX, msg.ref);
         luaL_error(Lua, ERR::MessageOperation);
      }
   }
   else luaL_error(Lua, "Expected a function to register as a message hook.");
   return 0;
}

//********************************************************************************************************************
// Garbage collector.

static int processing_destruct(lua_State *Lua)
{
   auto fp = (fprocessing *)luaL_checkudata(Lua, 1, "Tiri.processing");
   if (fp->SignalRefs) {
      for (auto ref : *fp->SignalRefs) luaL_unref(Lua, LUA_REGISTRYINDEX, ref);
      delete fp->SignalRefs;
      fp->SignalRefs = nullptr;
   }
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
   { "halt",           processing_halt },
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
   kt::Log log(__FUNCTION__);
   log.trace("Registering processing interface.");

   luaL_newmetatable(Lua, "Tiri.processing");
   lua_pushstring(Lua, "Tiri.processing");
   lua_setfield(Lua, -2, "__name");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable created earlier
   lua_settable(Lua, -3);   // metatable.__index = metatable
   luaL_openlib(Lua, nullptr, processinglib_methods, 0);

   luaL_openlib(Lua, "processing", processinglib_functions, 0);

   lua_pop(Lua, 2); // Drop the Tiri.processing metatable and the processing library table

   // Register processing interface prototypes for compile-time type inference
   reg_iface_prototype("processing", "new", { TiriType::Any }, { TiriType::Table });
   reg_iface_prototype("processing", "collect", { TiriType::Num }, { TiriType::Str, TiriType::Table });
   reg_iface_prototype("processing", "stopCollector", {}, {});
   reg_iface_prototype("processing", "startCollector", {}, {});
   reg_iface_prototype("processing", "gcStats", { TiriType::Table }, {});
   reg_iface_prototype("processing", "halt", { TiriType::Num }, { TiriType::Num });
   reg_iface_prototype("processing", "sleep", { TiriType::Num }, { TiriType::Num });
   reg_iface_prototype("processing", "signal", {}, {});
   reg_iface_prototype("processing", "task", { TiriType::Any }, {});
   reg_iface_prototype("processing", "flush", {}, {});
   reg_iface_prototype("processing", "delayedCall", {}, { TiriType::Func });
}
