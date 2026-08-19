// Unit tests for Tiri garbage-collector lifecycle behaviour.
// Copyright (C) 2026 Paul Manias

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lj_gc.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tab.h"

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
   settabV(Lua, Lua->top++, lj_context_current(Lua));
   lua_getfield(Lua, -1, "identifier");
   int identifier = int(lua_tointeger(Lua, -1));
   lua_pop(Lua, 2);
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

static bool test_context_root_tracks_environment_replacement(kt::Log &Log)
{
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab *initial = tabref(lua->env);
   GCtab *replacement = lj_tab_new(lua, 0, 1);
   setgcref(lua->env, obj2gco(replacement));
   bool passed = lj_context_depth(lua) IS 1 and lj_context_current(lua) IS replacement and replacement != initial;
   if (not passed) Log.error("root context did not resolve through the replacement environment");
   lua_close(lua);
   return passed;
}

static bool test_nested_context_ownership_and_stack_relocation(kt::Log &Log)
{
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab *outer = lj_tab_new(lua, 0, 1);
   GCtab *inner = lj_tab_new(lua, 0, 1);
   lj_context_push(lua, outer, lua->base);
   lj_context_push(lua, inner, lua->base);
   lj_state_growstack(lua, 256);

   bool passed = lj_context_depth(lua) IS 3 and lj_context_current(lua) IS inner;
   lj_context_pop(lua, lua->base);
   passed = passed and lj_context_current(lua) IS outer;
   lj_context_pop(lua, lua->base);
   passed = passed and lj_context_depth(lua) IS 1 and lj_context_current(lua) IS tabref(lua->env);
   if (not passed) Log.error("nested context ownership did not survive stack relocation or restore in order");
   lua_close(lua);
   return passed;
}

static bool test_active_context_survives_full_collection(kt::Log &Log)
{
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab *context = lj_tab_new(lua, 0, 1);
   GCstr *key = lj_str_newlit(lua, "context_gc_marker");
   TValue *slot = lj_tab_setstr(lua, context, key);
   setintV(slot, 73);
   lj_context_push(lua, context, lua->base);
   lj_gc_fullgc(lua);

   cTValue *retained = lj_tab_getstr(lj_context_current(lua), key);
   bool passed = retained and tvisnumber(retained) and numberVnum(retained) IS 73;
   if (not passed) {
      Log.error("active context table was not retained across a full collection (context=%p current=%p type=%d)",
         context, lj_context_current(lua), retained ? int(itype(retained)) : 0);
   }
   lj_context_pop(lua, lua->base);
   lua_close(lua);
   return passed;
}

static bool test_pending_close_error_is_thread_local_gc_root(kt::Log &Log)
{
   lua_State *first = luaL_newstate(nullptr);
   lua_State *second = luaL_newstate(nullptr);
   if (not first or not second) {
      if (first) lua_close(first);
      if (second) lua_close(second);
      Log.error("failed to create Lua states");
      return false;
   }

   GCtab *pending = lj_tab_new(first, 0, 1);
   GCstr *key = lj_str_newlit(first, "pending_close_marker");
   TValue *slot = lj_tab_setstr(first, pending, key);
   setintV(slot, 91);
   settabV(first, &first->pending_close_error, pending);
   lj_gc_fullgc(first);

   cTValue *retained = lj_tab_getstr(tabV(&first->pending_close_error), key);
   bool passed = retained and tvisnumber(retained) and numberVnum(retained) IS 91 and
      tvisnil(&second->pending_close_error);
   if (not passed) Log.error("pending close-error state was not rooted and isolated per Lua state");

   setnilV(&first->pending_close_error);
   lua_close(second);
   lua_close(first);
   return passed;
}

} // namespace

extern void gc_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 6> Tests = { {
      { "shutdown_drains_pre_registered_finalisers_after_error",
         test_shutdown_drains_pre_registered_finalisers_after_error },
      { "shutdown_does_not_run_new_finalisers", test_shutdown_does_not_run_new_finalisers },
      { "context_root_tracks_environment_replacement", test_context_root_tracks_environment_replacement },
      { "nested_context_ownership_and_stack_relocation", test_nested_context_ownership_and_stack_relocation },
      { "active_context_survives_full_collection", test_active_context_survives_full_collection },
      { "pending_close_error_is_thread_local_gc_root", test_pending_close_error_is_thread_local_gc_root }
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
