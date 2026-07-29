// Unit tests for Tiri garbage-collector lifecycle behaviour.
// Copyright (C) 2026 Paul Manias

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"

#include <array>
#include <cstddef>

namespace {

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

} // namespace

extern void gc_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 2> Tests = { {
      { "shutdown_drains_pre_registered_finalisers_after_error",
         test_shutdown_drains_pre_registered_finalisers_after_error },
      { "shutdown_does_not_run_new_finalisers", test_shutdown_does_not_run_new_finalisers }
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
