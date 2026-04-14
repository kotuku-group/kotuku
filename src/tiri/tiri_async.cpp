/*********************************************************************************************************************

The async interface provides support for the parallel execution of actions and methods against objects:

  async.action(Object, Action, Callback, Key, Args...)

  async.method(Object, Action, Callback, Key, Args...)

The script() method is a simplified variant of async.action() for scripts, but there's some potential to add
additional functionality in the future.

  async.script(Script, Callback)

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/strings.hpp>
#include <thread>
#include <cassert>
#include <mutex>

#include "lib.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_object.h"
#include "hashes.h"
#include "defs.h"
#include "lj_proto_registry.h"

// Message payload for thread completion callbacks (used by script, action, and method)

struct ThreadMsg {
   int       Callback;  // Client callback reference
   int       ObjRef;    // Registry reference that pins the GCobject from GC collection
   objScript *Owner;    // The parent script that owns the registry references
   double    Key;       // Client-provided key value forwarded to the callback
};

//********************************************************************************************************************
// Callback following execution (executed by the main thread, not the child)
// Must follow the signature declared in AsyncAction() documentation.

static void msg_thread_complete(ACTIONID ActionID, OBJECTPTR Object, ERR Error, ThreadMsg *Msg)
{
   pf::Log log("thread_callback");

   auto prv = (prvTiri *)Msg->Owner->ChildPrivate;

   if (Msg->Callback != LUA_NOREF) {
      if ((Object) and (Object->baseClassID() IS CLASSID::SCRIPT)) {
         auto args = std::to_array<ScriptArg>({
            { "Object", Object, FD_OBJECTPTR }
         });
         Msg->Owner->callback(Msg->Callback, args.data(), int(args.size()), nullptr);
      }
      else {
         auto args = std::to_array<ScriptArg>({
            { "ActionID", int(ActionID) },
            { "Object",   Object, FD_OBJECTPTR },
            { "Error",    int(Error) },
            { "Key",      Msg->Key }
         });
         Msg->Owner->callback(Msg->Callback, args.data(), int(args.size()), nullptr);
      }
      luaL_unref(prv->Lua, LUA_REGISTRYINDEX, Msg->Callback); // Drop the procedure reference
   }

   // Unpin the GCobject from the registry and release the pin on the underlying object.

   lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, Msg->ObjRef);
   auto gc_script = lua_toobject(prv->Lua, -1);
   lua_pop(prv->Lua, 1);
   luaL_unref(prv->Lua, LUA_REGISTRYINDEX, Msg->ObjRef);

   if (gc_script and gc_script->ptr) gc_script->ptr->unpin(true);
   delete Msg;
}

// async_wait() state has moved to the Core (AsyncWait API).

//********************************************************************************************************************
// Usage: async.script(Script, Callback)
//
// Pins the Script object to prevent premature destruction, then executes it in its own thread.  The pin is
// released when the thread completes and the callback message is processed on the main thread.  No object lock
// is held across the thread boundary — acActivate() acquires its own lock internally via ScopedObjectAccess.

static int async_script(lua_State *Lua)
{
   pf::Log log("async.script");

   GCobject *gc_script = lua_toobject(Lua, 1);
   if (gc_script->classptr->ClassID != CLASSID::SCRIPT) luaL_error(Lua, ERR::WrongClass);
   if (not gc_script->ptr) luaL_error(Lua, ERR::ObjectCorrupt);

   log.branch("Script: %d", gc_script->uid);

   gc_script->ptr->pin(); // Prevent the object from being freed while the thread is running.

   // Share the parent's pool with the child script so that async.pool accesses the same data.
   {
      auto parent_prv = (prvTiri *)Lua->script->ChildPrivate;
      auto child_prv  = (prvTiri *)gc_script->ptr->ChildPrivate;
      if (parent_prv and child_prv) {
         if (not parent_prv->Pool) parent_prv->Pool = std::make_shared<SharedPool>();
         child_prv->Pool = parent_prv->Pool;
      }
   }

   int client_callback = LUA_NOREF;
   if (lua_isfunction(Lua, 2)) {
      lua_pushvalue(Lua, 2);
      client_callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
   }

   // Pin the script in the registry so the GC cannot collect it while the thread is running.
   lua_pushvalue(Lua, 1);
   int obj_ref = luaL_ref(Lua, LUA_REGISTRYINDEX);

   auto msg = new ThreadMsg { client_callback, obj_ref, Lua->script, 0 };
   auto callback = C_FUNCTION(msg_thread_complete, msg);

   if (AsyncAction(AC::Activate, gc_script->ptr, nullptr, &callback) != ERR::Okay) {
      gc_script->ptr->unpin(true);
      luaL_unref(Lua, LUA_REGISTRYINDEX, obj_ref);
      luaL_unref(Lua, LUA_REGISTRYINDEX, client_callback);
      delete msg;
      luaL_error(Lua, "Failed to run script in new thread.");
   }

   return 0;
}

//********************************************************************************************************************
// Usage: async.action(Object, Action, Callback, Key, Args...)

static int async_action(lua_State *Lua)
{
   pf::Log log("async.action");

   // Args: Object (1), Action (2), Callback (3), Key (4), Parameters...

   GCobject *gc_obj = lj_lib_checkobject(Lua, 1);
   if (not gc_obj->ptr) luaL_error(Lua, ERR::ObjectCorrupt);

   auto type = lua_type(Lua, 2);
   AC action_id = AC::NIL;
   CSTRING action = nullptr;
   if (type IS LUA_TSTRING) {
      action = lua_tostring(Lua, 2);
      if (auto it = glActionLookup.find(action); it != glActionLookup.end()) {
         action_id = it->second;
      }
      else luaL_argerror(Lua, 2, "Action name is not recognised (is it a method?)");
   }
   else if (type IS LUA_TNUMBER) {
      action_id = AC(lua_tointeger(Lua, 2));
      if ((action_id <= AC::NIL) or (action_id >= AC::END)) luaL_argerror(Lua, 2, "Action ID out of range");
   }
   else luaL_argerror(Lua, 2, "Action name required.");

   int client_callback = LUA_NOREF;
   type = lua_type(Lua, 3); // Optional callback.
   if (type IS LUA_TSTRING) {
      lua_getglobal(Lua, lua_tostring(Lua, 3));
      client_callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
   }
   else if (type IS LUA_TFUNCTION) {
      lua_pushvalue(Lua, 3);
      client_callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
   }

   int arg_size = 0;
   const FunctionField *args = nullptr;

   if ((glActions[int(action_id)].Args) and (glActions[int(action_id)].Size)) {
      arg_size = glActions[int(action_id)].Size;
      args = glActions[int(action_id)].Args;
   }

   log.trace("#%d/%p, Action: %s/%d, Args: %d", gc_obj->uid, gc_obj->ptr, action, int(action_id), arg_size);

   // Pin the object and GCobject to prevent destruction while the thread is running.

   gc_obj->ptr->pin();

   lua_pushvalue(Lua, 1);
   int obj_ref = luaL_ref(Lua, LUA_REGISTRYINDEX);

   auto msg = new ThreadMsg { client_callback, obj_ref, Lua->script, lua_tonumber(Lua, 4) };
   auto callback = C_FUNCTION(msg_thread_complete, msg);

   auto abort = [&]() {
      gc_obj->ptr->unpin(true);
      luaL_unref(Lua, LUA_REGISTRYINDEX, obj_ref);
      luaL_unref(Lua, LUA_REGISTRYINDEX, client_callback);
      delete msg;
   };

   ERR error = ERR::Okay;
   if (arg_size > 0) {
      auto arg_buffer = std::make_unique<int8_t[]>(arg_size+8); // +8 for overflow protection in build_args()
      int result_count;

      if ((error = build_args(Lua, args, arg_size, arg_buffer.get(), &result_count)) IS ERR::Okay) {
         if (!result_count) {
            error = AsyncAction(action_id, gc_obj->ptr, arg_buffer.get(), &callback);
         }
         else {
            abort();
            luaL_error(Lua, "Actions that return results are not yet supported.");
         }
      }
      else {
         abort();
         luaL_error(Lua, "Argument build failure for %s.", glActions[int(action_id)].Name);
      }
   }
   else { // No parameters.
      error = AsyncAction(action_id, gc_obj->ptr, nullptr, &callback);
   }

   if (error != ERR::Okay) {
      abort();
      luaL_error(Lua, error);
   }

   return 0;
}

//********************************************************************************************************************
// Usage: error = async.method(Object, Method, Callback, Key, Args...)

static int async_method(lua_State *Lua)
{
   pf::Log log("async.method");

   auto gc_obj = lj_lib_checkobject(Lua, 1);
   if (not gc_obj->ptr) luaL_error(Lua, ERR::ObjectCorrupt);

   MethodEntry *table;
   int total_methods, i;

   auto type = lua_type(Lua, 2);
   CSTRING method = nullptr;
   AC method_id = AC::NIL;

   if ((gc_obj->classptr->get(FID_Methods, table, total_methods) IS ERR::Okay) and (table)) {
      bool found = false;

      if (type IS LUA_TSTRING) {
         method = lua_tostring(Lua, 2);
         for (i=1; i < total_methods; i++) {
            if ((table[i].Name) and (iequals(table[i].Name, method))) { found = true; break; }
         }
      }
      else if (type IS LUA_TNUMBER) {
         method_id = AC(lua_tointeger(Lua, 2));
         for (i=1; i < total_methods; i++) {
            if (table[i].MethodID IS method_id) { found = true; break; }
         }
      }
      else luaL_argerror(Lua, 2, "Method name or ID required.");

      if (found) {
         auto args      = table[i].Args;
         auto argsize   = table[i].Size;
         auto action_id = table[i].MethodID;

         int client_callback = LUA_NOREF;
         int type = lua_type(Lua, 3); // Optional callback.
         if (type IS LUA_TSTRING) {
            lua_getglobal(Lua, (STRING)lua_tostring(Lua, 3));
            client_callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
         }
         else if (type IS LUA_TFUNCTION) {
            lua_pushvalue(Lua, 3);
            client_callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
         }

         // Pin the object and GCobject to prevent destruction while the thread is running.

         gc_obj->ptr->pin();

         lua_pushvalue(Lua, 1);
         int obj_ref = luaL_ref(Lua, LUA_REGISTRYINDEX);

         auto msg = new ThreadMsg { client_callback, obj_ref, Lua->script, lua_tonumber(Lua, 4) };
         auto callback = C_FUNCTION(msg_thread_complete, msg);

         auto abort = [&]() {
            gc_obj->ptr->unpin(true);
            luaL_unref(Lua, LUA_REGISTRYINDEX, obj_ref);
            luaL_unref(Lua, LUA_REGISTRYINDEX, client_callback);
            delete msg;
         };

         ERR error = ERR::Okay;
         if (argsize > 0) {
            auto argbuffer = std::make_unique<int8_t[]>(argsize+8); // +8 for overflow protection in build_args()
            int resultcount;

            // Remove the first 4 required arguments so that the user's custom parameters are left on the stack.
            lua_rotate(Lua, 1, -4);
            lua_pop(Lua, 4);
            if ((error = build_args(Lua, args, argsize, argbuffer.get(), &resultcount)) IS ERR::Okay) {
               if (!resultcount) {
                  error = AsyncAction(action_id, gc_obj->ptr, argbuffer.get(), &callback);
               }
               else {
                  abort();
                  luaL_error(Lua, "Methods that return results are not yet supported.");
               }
            }
            else {
               abort();
               luaL_error(Lua, "Argument build failure for %s.", glActions[int(action_id)].Name);
            }
         }
         else { // No parameters.
            error = AsyncAction(action_id, gc_obj->ptr, nullptr, &callback);
         }

         if (error != ERR::Okay) {
            abort();
            luaL_error(Lua, error);
         }

         return 0;
      }
   }

   if (method) luaL_error(Lua, "No '%s' method for class %s.", method, gc_obj->classptr->ClassName);
   else luaL_error(Lua, "No method %d for class %s.", int(method_id), gc_obj->classptr->ClassName);
   return 0;
}

//********************************************************************************************************************
// Usage: error = async.wait(Object|array<object>, [Timeout])
//
// Thin wrapper around the Core AsyncWait() API.  Collects object IDs from the Lua arguments
// and delegates to AsyncWait().

static int async_wait(lua_State *Lua)
{
   pf::Log log("async.wait");

   // Collect object IDs from argument 1 into a zero-terminated array.

   std::vector<OBJECTID> ids;

   auto type = lua_type(Lua, 1);
   if (type IS LUA_TOBJECT) {
      auto gc_obj = lua_toobject(Lua, 1);
      if (not gc_obj or not gc_obj->ptr) luaL_error(Lua, ERR::ObjectCorrupt);
      ids.push_back(gc_obj->uid);
   }
   else if (type IS LUA_TARRAY) {
      GCarray *arr = lua_toarray(Lua, 1);
      if (arr->elemtype IS AET::OBJECT) {
         auto refs = arr->get<GCRef>();
         for (MSize i = 0; i < arr->len; i++) {
            if (gcref(refs[i])) {
               auto gc_obj = gco_to_object(gcref(refs[i]));
               if (gc_obj and gc_obj->uid) ids.push_back(gc_obj->uid);
            }
         }
      }
      else luaL_argerror(Lua, 1, "Expected an array<object>.");
   }
   else luaL_argerror(Lua, 1, "Expected an object or array<object>.");

   ERR error = ERR::Okay;
   if (not ids.empty()) {
      // Timeout (seconds → milliseconds).  Default: indefinite (-1).

      int timeout_ms = -1;
      if (lua_type(Lua, 2) IS LUA_TNUMBER) {
         timeout_ms = int(lua_tonumber(Lua, 2) * 1000.0);
         if (timeout_ms < 0) timeout_ms = -1;
      }

      error = AsyncWait(ids.data(), ids.size(), timeout_ms);

      if ((error != ERR::Okay) and (in_try_immediate_scope(Lua))) luaL_error(Lua, error);
   }

   lua_pushinteger(Lua, int(error));
   return 1;
}

//********************************************************************************************************************
// Usage: count = async.pending(Object)
//
// Returns the number of queued and in-flight async actions for an object.

static int async_pending(lua_State *Lua)
{
   GCobject *gc_obj = lj_lib_checkobject(Lua, 1);
   if (not gc_obj->ptr) luaL_error(Lua, ERR::ObjectCorrupt);
   lua_pushinteger(Lua, AsyncPending(gc_obj->uid));
   return 1;
}

//********************************************************************************************************************
// Usage: error = async.cancel(Object|array<object>)
//
// Cancel all pending asynchronous actions for the listed objects.  The in-flight thread (if any) is interrupted
// and the remaining action queue is drained without execution.

static int async_cancel(lua_State *Lua)
{
   std::vector<OBJECTID> ids;

   auto type = lua_type(Lua, 1);
   if (type IS LUA_TOBJECT) {
      auto gc_obj = lua_toobject(Lua, 1);
      if (not gc_obj or not gc_obj->ptr) luaL_error(Lua, ERR::ObjectCorrupt);
      ids.push_back(gc_obj->uid);
   }
   else if (type IS LUA_TARRAY) {
      GCarray *arr = lua_toarray(Lua, 1);
      if (arr->elemtype IS AET::OBJECT) {
         auto refs = arr->get<GCRef>();
         for (MSize i = 0; i < arr->len; i++) {
            if (gcref(refs[i])) {
               auto gc_obj = gco_to_object(gcref(refs[i]));
               if (gc_obj and gc_obj->uid) ids.push_back(gc_obj->uid);
            }
         }
      }
      else luaL_argerror(Lua, 1, "Expected an array<object>.");
   }
   else luaL_argerror(Lua, 1, "Expected an object or array<object>.");

   ERR error = ERR::Okay;
   if (not ids.empty()) {
      error = AsyncCancel(ids.data(), int(ids.size()));
      if ((error != ERR::Okay) and (in_try_immediate_scope(Lua))) luaL_error(Lua, error);
   }

   lua_pushinteger(Lua, int(error));
   return 1;
}

//********************************************************************************************************************
// Register the async interface.

static const luaL_Reg asynclib_functions[] = {
   { "action",  async_action },
   { "cancel",  async_cancel },
   { "method",  async_method },
   { "pending", async_pending },
   { "script",  async_script },
   { "wait",    async_wait },
   { nullptr, nullptr }
};

static const luaL_Reg asynclib_methods[] = {
   //{ "__index",    async_get },
   //{ "__newindex", async_set },
   { nullptr, nullptr }
};

//********************************************************************************************************************
// Retrieve the shared pool from the script's private data.  Creates it on first use (lazy initialisation).

static SharedPool * get_pool(lua_State *Lua)
{
   auto prv = (prvTiri *)Lua->script->ChildPrivate;
   if (not prv->Pool) prv->Pool = std::make_shared<SharedPool>();
   return prv->Pool.get();
}

//********************************************************************************************************************
// async.pool.__index — Thread-safe read from the shared pool.
// Stack: [1] = pool table, [2] = key string

static int pool_get(lua_State *Lua)
{
   size_t key_len;
   auto key = luaL_checklstring(Lua, 2, &key_len);
   auto pool = get_pool(Lua);

   // Copy the value under lock, then release before touching the Lua stack.
   // Lua push APIs can longjmp on allocation failure, which would skip C++ destructors
   // and leave the shared mutex permanently locked.

   PoolValue local_val;
   bool found = false;
   {
      std::shared_lock lock(pool->mutex);
      if (auto it = pool->data.find(std::string(key, key_len)); it != pool->data.end()) {
         local_val = it->second;
         found = true;
      }
   }

   if (found) {
      switch (local_val.tag) {
         case PoolTag::Number:   lua_pushnumber(Lua, local_val.num); break;
         case PoolTag::String:   lua_pushlstring(Lua, local_val.str.data(), local_val.str.size()); break;
         case PoolTag::Boolean:  lua_pushboolean(Lua, local_val.flag); break;
         case PoolTag::ObjectID: push_object_id(Lua, local_val.oid); break;
      }
   }
   else lua_pushnil(Lua);

   return 1;
}

//********************************************************************************************************************
// async.pool.__newindex — Thread-safe write to the shared pool.
// Stack: [1] = pool table, [2] = key string, [3] = value

static int pool_set(lua_State *Lua)
{
   size_t key_len;
   auto key = luaL_checklstring(Lua, 2, &key_len);

   // Validate the value type and extract it before acquiring the mutex lock.  This prevents
   // luaL_argerror's longjmp from leaving the mutex permanently locked.

   auto value_type = lua_type(Lua, 3);

   PoolValue pv;
   switch (value_type) {
      case LUA_TNIL:
         break; // Deletion — no value needed
      case LUA_TNUMBER:
         pv = PoolValue(lua_tonumber(Lua, 3));
         break;
      case LUA_TSTRING: {
         size_t len;
         auto s = lua_tolstring(Lua, 3, &len);
         pv = PoolValue(std::string(s, len));
         break;
      }
      case LUA_TBOOLEAN:
         pv = PoolValue(bool(lua_toboolean(Lua, 3)));
         break;
      case LUA_TOBJECT: {
         auto gc_obj = lua_toobject(Lua, 3);
         if (not gc_obj) luaL_argerror(Lua, 3, "Invalid object.");
         pv = PoolValue::object(gc_obj->uid);
         break;
      }
      default:
         luaL_argerror(Lua, 3, "async.pool supports number, string, boolean, and object values.");
   }

   auto pool = get_pool(Lua);
   std::unique_lock lock(pool->mutex);

   auto key_str = std::string(key, key_len);
   if (value_type IS LUA_TNIL) pool->data.erase(key_str);
   else pool->data.insert_or_assign(std::move(key_str), std::move(pv));

   return 0;
}

//********************************************************************************************************************
// async.pool.clear() — Remove all entries from the shared pool.

static int pool_clear(lua_State *Lua)
{
   auto pool = get_pool(Lua);
   std::unique_lock lock(pool->mutex);
   pool->data.clear();
   return 0;
}

//********************************************************************************************************************

void register_async_class(lua_State *Lua)
{
   pf::Log log;

   log.trace("Registering async interface.");

   luaL_newmetatable(Lua, "Tiri.async");
   lua_pushstring(Lua, "Tiri.async");
   lua_setfield(Lua, -2, "__name");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable created earlier
   lua_settable(Lua, -3);   // metatable.__index = metatable

   luaL_openlib(Lua, nullptr, asynclib_methods, 0);
   luaL_openlib(Lua, "async", asynclib_functions, 0);

   // Create the async.pool sub-table with __index/__newindex metamethods for thread-safe access.

   luaL_newmetatable(Lua, "Tiri.async.pool");
   lua_pushcfunction(Lua, pool_get);
   lua_setfield(Lua, -2, "__index");
   lua_pushcfunction(Lua, pool_set);
   lua_setfield(Lua, -2, "__newindex");
   lua_pop(Lua, 1);

   lua_newtable(Lua);                          // Create the pool table
   luaL_getmetatable(Lua, "Tiri.async.pool");
   lua_setmetatable(Lua, -2);                  // Assign the metatable

   // Store pool.clear() as a raw key so it takes precedence over __index.
   lua_pushstring(Lua, "clear");
   lua_pushcfunction(Lua, pool_clear);
   lua_rawset(Lua, -3);

   lua_getglobal(Lua, "async");                // Push the async table
   lua_pushvalue(Lua, -2);                     // Push the pool table
   lua_setfield(Lua, -2, "pool");              // async.pool = pool_table
   lua_pop(Lua, 2);                            // Pop async table and pool table

   // Register async interface prototypes for compile-time type inference
   reg_iface_prototype("async", "action",  {}, { TiriType::Any, TiriType::Any, TiriType::Func, TiriType::Num });
   reg_iface_prototype("async", "cancel",  { TiriType::Num }, { TiriType::Any });
   reg_iface_prototype("async", "method",  {}, { TiriType::Any, TiriType::Any, TiriType::Func, TiriType::Num });
   reg_iface_prototype("async", "pending", { TiriType::Num }, { TiriType::Object });
   reg_iface_prototype("async", "script",  {}, { TiriType::Object, TiriType::Func });
   reg_iface_prototype("async", "wait",    { TiriType::Num }, { TiriType::Any, TiriType::Num });
}
