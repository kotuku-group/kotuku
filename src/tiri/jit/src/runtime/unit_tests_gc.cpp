// Unit tests for Tiri garbage-collector lifecycle behaviour.
// Copyright (C) 2026 Paul Manias

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lj_gc.h"
#include "lj_object.h"

#include <array>
#include <cstddef>
#include <limits>

namespace {

constexpr OBJECTID test_object_id()
{
   return OBJECTID(std::numeric_limits<int32_t>::max());
}

struct TestCase {
   const char* name;
   bool (*fn)(kt::Log &Log);
};

static thread_local std::array<int, 8> glShutdownFinaliserCalls = { };
static thread_local size_t glShutdownFinaliserCallCount = 0;
static thread_local bool glShutdownFinaliserCreationSucceeded = false;

static void reset_shutdown_finaliser_state()
{
   glShutdownFinaliserCalls.fill(0);
   glShutdownFinaliserCallCount = 0;
   glShutdownFinaliserCreationSucceeded = false;
}

static void record_shutdown_finaliser_call(int Identifier)
{
   if (glShutdownFinaliserCallCount < glShutdownFinaliserCalls.size()) {
      glShutdownFinaliserCalls[glShutdownFinaliserCallCount++] = Identifier;
   }
}

static int table_identifier(lua_State *Lua)
{
   lua_getfield(Lua, 1, "identifier");
   int identifier = int(lua_tointeger(Lua, -1));
   lua_pop(Lua, 1);
   return identifier;
}

static int shutdown_recording_finaliser(lua_State *Lua)
{
   record_shutdown_finaliser_call(table_identifier(Lua));
   return 0;
}

static int shutdown_failing_finaliser(lua_State *Lua)
{
   record_shutdown_finaliser_call(table_identifier(Lua));
   luaL_error(Lua, "expected shutdown finaliser failure");
}

static bool push_finalisable_table(lua_State *Lua, lua_CFunction Finaliser, int Identifier)
{
   lua_newtable(Lua);
   int table_index = lua_gettop(Lua);
   lua_pushinteger(Lua, Identifier);
   lua_setfield(Lua, table_index, "identifier");

   lua_newtable(Lua);
   lua_pushcfunction(Lua, Finaliser);
   lua_setfield(Lua, -2, "__gc");
   return lua_setmetatable(Lua, table_index) != 0;
}

static int shutdown_creating_finaliser(lua_State *Lua)
{
   record_shutdown_finaliser_call(table_identifier(Lua));
   glShutdownFinaliserCreationSucceeded = push_finalisable_table(Lua, shutdown_recording_finaliser, 2);
   return 0;
}

template<size_t Size>
static bool calls_match(const std::array<int, Size>& Expected)
{
   if (glShutdownFinaliserCallCount != Expected.size()) return false;
   for (size_t i = 0; i < Expected.size(); i++) {
      if (glShutdownFinaliserCalls[i] != Expected[i]) return false;
   }
   return true;
}

static bool test_shutdown_drains_pre_registered_finalisers_after_error(kt::Log &Log)
{
   reset_shutdown_finaliser_state();
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   bool setup_ok = push_finalisable_table(lua, shutdown_recording_finaliser, 1);
   lua_pop(lua, 1);
   setup_ok = push_finalisable_table(lua, shutdown_failing_finaliser, 2) and setup_ok;
   lua_pop(lua, 1);
   setup_ok = push_finalisable_table(lua, shutdown_recording_finaliser, 3) and setup_ok;
   lua_pop(lua, 1);
   lua_close(lua);

   constexpr std::array<int, 3> expected = { 3, 2, 1 };
   if (not setup_ok) {
      Log.error("failed to register the shutdown finalisers");
      return false;
   }
   if (not calls_match(expected)) {
      Log.error("shutdown finaliser order/count was incorrect");
      return false;
   }
   return true;
}

static bool test_shutdown_does_not_run_new_finalisers(kt::Log &Log)
{
   reset_shutdown_finaliser_state();
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   bool setup_ok = push_finalisable_table(lua, shutdown_creating_finaliser, 1);
   lua_pop(lua, 1);
   lua_close(lua);

   constexpr std::array<int, 1> expected = { 1 };
   if (not setup_ok or not glShutdownFinaliserCreationSucceeded) {
      Log.error("failed to create a finalisable table for the shutdown lifecycle test");
      return false;
   }
   if (not calls_match(expected)) {
      Log.error("a shutdown-time finaliser ran during the same shutdown");
      return false;
   }
   return true;
}

static bool test_owned_wrapper_accounting(kt::Log &Log)
{
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   auto collector = gc(lua);
   auto invalid = lj_object_new(lua, 0, nullptr, nullptr, 0);
   auto owned = lj_object_new(lua, test_object_id(), nullptr, nullptr, 0);
   auto detached = lj_object_new(lua, test_object_id(), nullptr, nullptr, GCOBJ_DETACHED);
   bool result = owned->is_resource_tracked() and collector.ownedObjectCount() IS 1 and
      collector.resourceCyclePending() and not invalid->is_resource_tracked() and not detached->is_resource_tracked();

   collector.releaseOwnedObject(owned);
   collector.releaseOwnedObject(owned);
   result = result and collector.ownedObjectCount() IS 0 and not owned->is_resource_tracked();

   lua_close(lua);
   if (not result) Log.error("owned and detached wrappers were not accounted for correctly");
   return result;
}

static bool test_resource_cycle_transitions(kt::Log &Log)
{
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   auto collector = gc(lua);
   auto owned = lj_object_new(lua, test_object_id(), nullptr, nullptr, 0);
   collector.beginResourceCycle();
   bool result = collector.resourceCycleActive() and not collector.resourceCyclePending();

   collector.requestResourceCycle();
   collector.finishResourceCycle();
   result = result and not collector.resourceCycleActive() and collector.resourceCyclePending() and
      collector.resourceCooldown() IS GarbageCollector::RESOURCE_COOLDOWN;

   collector.beginResourceCycle(false);
   result = result and collector.resourceCycleActive() and collector.resourceCyclePending();
   collector.finishResourceCycle();
   collector.beginResourceCycle();
   result = result and collector.resourceCycleActive() and not collector.resourceCyclePending();
   collector.finishResourceCycle();
   for (uint16_t i = 0; i < GarbageCollector::RESOURCE_COOLDOWN; i++) collector.deferResourceCycle();
   result = result and collector.resourceCycleDue();

   collector.releaseOwnedObject(owned);
   lua_close(lua);
   if (not result) Log.error("resource cycle request, active state or cooldown transition was incorrect");
   return result;
}

static bool test_stopped_collector_preserves_resource_work(kt::Log &Log)
{
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   auto collector = gc(lua);
   auto owned = lj_object_new(lua, test_object_id(), nullptr, nullptr, 0);
   collector.beginResourceCycle();
   collector.requestResourceCycle();
   collector.stop();
   bool result = not collector.isRunning() and collector.resourceCycleActive() and
      collector.resourceCyclePending() and collector.ownedObjectCount() IS 1;

   collector.restart();
   result = result and collector.isRunning() and collector.resourceCycleActive() and
      collector.resourceCyclePending();
   collector.releaseOwnedObject(owned);
   lua_close(lua);
   if (not result) Log.error("stop/restart changed pending native resource collection state");
   return result;
}

static bool test_full_cycle_clears_unreachable_owned_wrapper(kt::Log &Log)
{
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   auto collector = gc(lua);
   lj_object_new(lua, test_object_id(), nullptr, nullptr, 0);
   collector.fullCycle(lua);
   bool result = collector.ownedObjectCount() IS 0 and not collector.resourceCycleActive() and
      not collector.resourceCyclePending();

   lua_close(lua);
   if (not result) Log.error("full collection left unreachable native wrapper scheduling state behind");
   return result;
}

} // namespace

extern void gc_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 6> Tests = { {
      { "shutdown_drains_pre_registered_finalisers_after_error",
         test_shutdown_drains_pre_registered_finalisers_after_error },
      { "shutdown_does_not_run_new_finalisers", test_shutdown_does_not_run_new_finalisers },
      { "owned_wrapper_accounting", test_owned_wrapper_accounting },
      { "resource_cycle_transitions", test_resource_cycle_transitions },
      { "stopped_collector_preserves_resource_work", test_stopped_collector_preserves_resource_work },
      { "full_cycle_clears_unreachable_owned_wrapper", test_full_cycle_clears_unreachable_owned_wrapper }
   } };

   for (const TestCase& Test : Tests) {
      kt::Log Log("GcTests");
      Log.branch("Running %s", Test.name);
      ++Total;
      if (Test.fn(Log)) {
         ++Passed;
         Log.msg("%s passed", Test.name);
      }
      else {
         Log.error("%s failed", Test.name);
      }
   }
}

#endif // UNIT_TESTS
