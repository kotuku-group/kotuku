// Unit tests for Tiri garbage-collector lifecycle behaviour.
// Copyright (C) 2026 Paul Manias

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lj_array.h"
#include "lj_gc.h"
#include "lj_object.h"
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
   luaL_error(Lua, ERR::Failed, "expected shutdown finaliser failure");
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

static bool all_arrays_black(const std::array<GCarray *, 5> &Arrays)
{
   for (GCarray *array : Arrays) {
      if (not isblack(obj2gco(array))) return false;
   }
   return true;
}

static bool move_arrays_to_incremental_black(lua_State *Lua, const std::array<GCarray *, 5> &Arrays)
{
   global_State *global = G(Lua);
   MSize original_step_multiplier = global->gc.stepmul;
   global->gc.stepmul = 1;
   for (unsigned step = 0; step < 1024; step++) {
      if (global->gc.state IS GCPhase::Propagate and all_arrays_black(Arrays)) {
         global->gc.stepmul = original_step_multiplier;
         return true;
      }
      if (step > 0 and global->gc.state IS GCPhase::Pause) break;
      lj_gc_step(Lua);
   }
   global->gc.stepmul = original_step_multiplier;
   return global->gc.state IS GCPhase::Propagate and all_arrays_black(Arrays);
}

static bool array_backward_barrier_once(global_State *Global, GCarray *Array)
{
   GCobj *previous = gcref(Global->gc.grayagain);
   lj_gc_barrierbackarray(Global, Array);
   if (not isgray(obj2gco(Array)) or gcref(Global->gc.grayagain) != obj2gco(Array) or
       gcref(Array->gclist) != previous) return false;

   lj_gc_barrierbackarray(Global, Array);
   return gcref(Global->gc.grayagain) IS obj2gco(Array) and gcref(Array->gclist) IS previous;
}

static bool test_array_backward_barrier_retains_incremental_children(kt::Log &Log)
{
   lua_State *lua = luaL_newstate(nullptr);
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCarray *strings = lj_array_new(lua, 1, AET::STR_GC);
   GCarray *tables = lj_array_new(lua, 1, AET::TABLE);
   GCarray *arrays = lj_array_new(lua, 1, AET::ARRAY);
   GCarray *objects = lj_array_new(lua, 1, AET::OBJECT);
   GCarray *values = lj_array_new(lua, 1, AET::ANY);
   const std::array<GCarray *, 5> parents = { strings, tables, arrays, objects, values };
   for (GCarray *parent : parents) setarrayV(lua, lua->top++, parent);

   global_State *global = G(lua);
   if (not move_arrays_to_incremental_black(lua, parents)) {
      Log.error("array parents did not become black during incremental propagation");
      lua_close(lua);
      return false;
   }

   GCstr *string = lj_str_newz(lua, "array_barrier_string");
   setgcref(strings->get<GCRef>()[0], obj2gco(string));
   bool passed = iswhite(obj2gco(string));
   passed = array_backward_barrier_once(global, strings) and passed;

   GCtab *table = lj_tab_new(lua, 0, 1);
   setintV(lj_tab_setint(lua, table, 1), 41);
   setgcref(tables->get<GCRef>()[0], obj2gco(table));
   passed = iswhite(obj2gco(table)) and passed;
   passed = array_backward_barrier_once(global, tables) and passed;

   GCarray *nested = lj_array_new(lua, 1, AET::INT32);
   nested->get<int32_t>()[0] = 42;
   setgcref(arrays->get<GCRef>()[0], obj2gco(nested));
   passed = iswhite(obj2gco(nested)) and passed;
   passed = array_backward_barrier_once(global, arrays) and passed;

   GCobject *object = lj_object_new(lua, OBJECTID(43), nullptr, nullptr, GCOBJ_DETACHED);
   setgcref(objects->get<GCRef>()[0], obj2gco(object));
   passed = iswhite(obj2gco(object)) and passed;
   passed = array_backward_barrier_once(global, objects) and passed;

   GCtab *any_table = lj_tab_new(lua, 0, 1);
   setintV(lj_tab_setint(lua, any_table, 1), 44);
   settabV(lua, &values->get<TValue>()[0], any_table);
   passed = iswhite(obj2gco(any_table)) and passed;
   passed = array_backward_barrier_once(global, values) and passed;
   if (not passed) {
      Log.error("array backward barrier setup or duplicate-link check failed");
      lua_close(lua);
      return false;
   }

   for (unsigned step = 0; step < 4096 and global->gc.state != GCPhase::Pause; step++) lj_gc_step(lua);
   if (global->gc.state != GCPhase::Pause) {
      Log.error("incremental collection did not return to the pause state");
      lua_close(lua);
      return false;
   }

   GCstr *retained_string = strref(strings->get<GCRef>()[0]);
   GCtab *retained_table = tabref(tables->get<GCRef>()[0]);
   GCarray *retained_array = arrayref(arrays->get<GCRef>()[0]);
   GCobject *retained_object = objectref(objects->get<GCRef>()[0]);
   cTValue *retained_any = &values->get<TValue>()[0];
   cTValue *table_value = lj_tab_getint(retained_table, 1);
   cTValue *any_value = tvistab(retained_any) ? lj_tab_getint(tabV(retained_any), 1) : nullptr;
   passed = retained_string IS string and retained_string->len IS sizeof("array_barrier_string") - 1;
   if (not passed) Log.error("array backward barrier did not preserve string storage");
   if (retained_table != table or not tvisnumber(table_value) or numberVnum(table_value) != 41) {
      Log.error("array backward barrier did not preserve table storage");
      passed = false;
   }
   if (retained_array != nested or retained_array->get<int32_t>()[0] != 42) {
      Log.error("array backward barrier did not preserve nested-array storage");
      passed = false;
   }
   if (retained_object != object or retained_object->uid != OBJECTID(43)) {
      Log.error("array backward barrier did not preserve object storage");
      passed = false;
   }
   if (not tvistab(retained_any) or tabV(retained_any) != any_table or not any_value or not tvisnumber(any_value) or
       numberVnum(any_value) != 44) {
      Log.error("array backward barrier did not preserve TValue storage");
      passed = false;
   }
   lua_close(lua);
   return passed;
}

} // namespace

extern void gc_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 7> Tests = { {
      { "shutdown_drains_pre_registered_finalisers_after_error",
         test_shutdown_drains_pre_registered_finalisers_after_error },
      { "shutdown_does_not_run_new_finalisers", test_shutdown_does_not_run_new_finalisers },
      { "context_root_tracks_environment_replacement", test_context_root_tracks_environment_replacement },
      { "nested_context_ownership_and_stack_relocation", test_nested_context_ownership_and_stack_relocation },
      { "active_context_survives_full_collection", test_active_context_survives_full_collection },
      { "pending_close_error_is_thread_local_gc_root", test_pending_close_error_is_thread_local_gc_root },
      { "array_backward_barrier_retains_incremental_children",
         test_array_backward_barrier_retains_incremental_children }
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
