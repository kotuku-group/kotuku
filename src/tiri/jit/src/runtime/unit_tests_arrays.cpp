// Unit tests for native array type.
// Copyright (C) 2025-2026 Paul Manias

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_array.h"
#include "lj_str.h"
#include "lj_struct.h"
#include "lj_tab.h"
#include "lj_object.h"
#include "lj_vmarray.h"

#include <cstring>
#include <array>
#include <atomic>

#include "../../defs.h"

static extTiri* glArrayTestScript = nullptr;
static std::atomic_int glArrayResourceFrees = 0;

static ERR array_test_resource_free(ResourceRecord &, APTR)
{
   glArrayResourceFrees.fetch_add(1, std::memory_order_relaxed);
   return ERR::Terminate;
}

static ResourceManager glArrayResourceManager = {
   "TiriArrayTest",
   &array_test_resource_free,
   false
};

// Helper: dostring equivalent - loads and executes a string
static int dostring(lua_State *L, const char* s)
{
   int result = lua_load(L, std::string_view(s, strlen(s)), "test");
   if (result IS 0) result = lua_pcall(L, 0, LUA_MULTRET, 0);
   return result;
}

namespace {

struct TestCase {
   const char* name;
   bool (*fn)(kt::Log &Log);
};

struct LuaStateHolder {
   LuaStateHolder()
   {
      this->state = luaL_newstate(glArrayTestScript);
   }

   ~LuaStateHolder()
   {
      if (this->state) {
         lua_close(this->state);
      }
   }

   lua_State* get() const { return this->state; }

private:
   lua_State* state = nullptr;
};

//********************************************************************************************************************
// Core Data Structures
static bool test_array_recursive_identity(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   std::array<AET, 4> implicit_types { AET::INT32, AET::STR_GC, AET::ANY, AET::ARRAY };
   for (AET type : implicit_types) {
      GCarray *ordinary = lj_array_new(lua, 0, type);
      if (ordinary->nested_identity()) {
         Log.error("ordinary array type %d stored a redundant identity", int(type));
         return false;
      }
   }

   struct_record structure("ArrayIdentityStructure");
   structure.Size = sizeof(int32_t);
   GCarray *structures = lj_array_new(lua, 0, AET::STRUCT, nullptr, 0, structure.Name, &structure);
   if (structures->struct_definition() != &structure or structures->nested_identity()) {
      Log.error("a structure array did not use the tagged structure-definition slot");
      return false;
   }

   GCstr *matrix_identity = lj_str_new(lua, "array<array<double>>", sizeof("array<array<double>>") - 1);

   ArrayAllocationDescriptor descriptor {
      .storage = AET::ARRAY,
      .nested_identity = matrix_identity
   };
   GCarray *matrix = lj_array_new(lua, 3, descriptor);
   setarrayV(lua, lua->top++, matrix);
   lua_gc(lua, LUA_GCCOLLECT);
   GCstr *identity = matrix->nested_identity();
   if (identity != matrix_identity or std::string_view(strdata(identity), identity->len) != "array<array<double>>") {
      Log.error("a descriptor-aware array allocation lost its identity during GC traversal");
      return false;
   }

   GCarray *derived = lj_array_new_like(lua, matrix, 0);
   if (derived->nested_identity() != matrix_identity) {
      Log.error("a derived recursive array did not share its source identity");
      return false;
   }

   GCarray *matching = lj_array_new(lua, 0, AET::DOUBLE);
   GCarray *mismatch = lj_array_new(lua, 0, AET::STR_GC);
   TValue value;
   setarrayV(lua, &value, matching);
   if (lj_array_validate_element(matrix, &value) != ArrayElementResult::OK) {
      Log.error("recursive array storage rejected a matching inner identity");
      return false;
   }
   setarrayV(lua, &value, mismatch);
   if (lj_array_validate_element(matrix, &value) != ArrayElementResult::INVALID_TYPE) {
      Log.error("recursive array storage accepted a mismatched inner identity");
      return false;
   }

   ArrayAllocationDescriptor wildcard_descriptor {
      .storage = AET::ARRAY,
      .nested_identity = lj_str_new(lua, "array<array<any>>", sizeof("array<array<any>>") - 1)
   };
   GCarray *wildcard = lj_array_new(lua, 0, wildcard_descriptor);
   if (lj_array_validate_element(wildcard, &value) != ArrayElementResult::OK) {
      Log.error("a recursive wildcard rejected a valid inner array");
      return false;
   }

   ArrayAllocationDescriptor deep_wildcard_descriptor {
      .storage = AET::ARRAY,
      .nested_identity = lj_str_new(lua, "array<array<array>>", sizeof("array<array<array>>") - 1)
   };
   ArrayAllocationDescriptor specialised_matrix_descriptor {
      .storage = AET::ARRAY,
      .nested_identity = lj_str_new(lua, "array<array<int>>", sizeof("array<array<int>>") - 1)
   };
   GCarray *deep_wildcard = lj_array_new(lua, 0, deep_wildcard_descriptor);
   GCarray *specialised_matrix = lj_array_new(lua, 0, specialised_matrix_descriptor);
   setarrayV(lua, &value, specialised_matrix);
   if (lj_array_validate_element(deep_wildcard, &value) != ArrayElementResult::OK) {
      Log.error("a deeply recursive bare array wildcard rejected a valid specialisation");
      return false;
   }

   GCarray *bare_matrix = lj_array_new(lua, 0, AET::ARRAY);
   if (not lj_array_member_identity_matches(bare_matrix, "array<any>") or
       not lj_array_identity_matches(bare_matrix, "array<array<any>>")) {
      Log.error("an array<any> wildcard rejected a bare array identity");
      return false;
   }

   GCarray *source = lj_array_new(lua, 5, AET::INT32);
   GCarray *copy = lj_array_new_like(lua, source, 5);
   for (MSize i = 0; i < source->len; i++) source->get<int32_t>()[i] = int32_t(i + 1);
   lj_array_copy(lua, copy, 0, source, 0, source->len);
   if (copy->get<int32_t>()[4] != 5) {
      Log.error("a contiguous internal array copy lost values");
      return false;
   }
   lj_array_copy_unchecked(lua, copy, 0, source, 4, 1);
   lj_array_copy_unchecked(lua, copy, 1, source, 2, 1);
   lj_array_copy_unchecked(lua, copy, 2, source, 0, 1);
   if (copy->get<int32_t>()[0] != 5 or copy->get<int32_t>()[1] != 3 or copy->get<int32_t>()[2] != 1) {
      Log.error("stepped or descending internal copies lost values");
      return false;
   }

   GCarray *empty = lj_array_new_like(lua, matrix, 0);
   if (empty->len != 0 or empty->nested_identity() != matrix_identity) {
      Log.error("an empty derived array lost its recursive identity");
      return false;
   }
   return true;
}

static bool test_array_strided_copy(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   GCarray *source = lj_array_new(lua, 9, AET::INT32);
   setarrayV(lua, lua->top++, source);
   GCarray *destination = lj_array_new_like(lua, source, 6);
   setarrayV(lua, lua->top++, destination);
   for (MSize i = 0; i < source->len; i++) source->get<int32_t>()[i] = int32_t(i);

   lj_array_copy_strided_unchecked(lua, destination, 1, source, 8, -2, 4);
   const int32_t expected[] = { 8, 6, 4, 2 };
   for (MSize i = 0; i < std::size(expected); i++) {
      if (destination->get<int32_t>()[i + 1] != expected[i]) {
         Log.error("a descending strided copy lost a primitive value at slot %d", int(i));
         return false;
      }
   }

   GCarray *strings = lj_array_new(lua, 5, AET::STR_GC);
   setarrayV(lua, lua->top++, strings);
   GCarray *string_copy = lj_array_new_like(lua, strings, 3);
   setarrayV(lua, lua->top++, string_copy);
   for (MSize i = 0; i < strings->len; i++) {
      std::string value = std::to_string(i);
      GCstr *string = lj_str_new(lua, value.data(), value.size());
      setgcref(strings->get<GCRef>()[i], obj2gco(string));
      lj_gc_objbarrier(lua, strings, string);
   }
   lj_array_copy_strided_unchecked(lua, string_copy, 0, strings, 0, 2, 3);
   for (MSize i = 0; i < string_copy->len; i++) {
      GCstr *string = strref(string_copy->get<GCRef>()[i]);
      if (string->len != 1 or strdata(string)[0] != char('0' + (i * 2))) {
         Log.error("a forward strided copy lost a string reference at slot %d", int(i));
         return false;
      }
   }

   struct_record structure("StridedCopyStructure");
   structure.Size = sizeof(uint64_t);
   GCarray *structures = lj_array_new(lua, 5, AET::STRUCT, nullptr, 0, structure.Name, &structure);
   setarrayV(lua, lua->top++, structures);
   GCarray *structure_copy = lj_array_new_like(lua, structures, 3);
   setarrayV(lua, lua->top++, structure_copy);
   for (MSize i = 0; i < structures->len; i++) structures->get<uint64_t>()[i] = uint64_t(i + 100);
   lj_array_copy_strided_unchecked(lua, structure_copy, 0, structures, 4, -2, 3);
   if (structure_copy->get<uint64_t>()[0] != 104 or structure_copy->get<uint64_t>()[1] != 102 or
       structure_copy->get<uint64_t>()[2] != 100) {
      Log.error("a descending strided copy lost structure storage");
      return false;
   }

   GCarray *any = lj_array_new(lua, 5, AET::ANY);
   setarrayV(lua, lua->top++, any);
   GCarray *any_copy = lj_array_new_like(lua, any, 3);
   setarrayV(lua, lua->top++, any_copy);
   for (MSize i = 0; i < any->len; i++) setintV(&any->get<TValue>()[i], int32_t(i * 10));
   lj_array_copy_strided_unchecked(lua, any_copy, 0, any, 4, -2, 3);
   for (MSize i = 0; i < any_copy->len; i++) {
      cTValue *value = &any_copy->get<TValue>()[i];
      int32_t expected_value = int32_t(40 - int32_t(i * 20));
      bool matches = (tvisint(value) and intV(value) IS expected_value) or
         (tvisnum(value) and numberVint(value) IS expected_value);
      if (not matches) {
         Log.error("a descending strided copy lost an any value at slot %d", int(i));
         return false;
      }
   }

   lj_array_copy_strided_unchecked(lua, destination, destination->len, source, source->len, -1, 0);
   return true;
}

//********************************************************************************************************************

static bool test_array_fresh_copy(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   GCarray *strings = lj_array_new(lua, 6, AET::STR_GC);
   setarrayV(lua, lua->top++, strings);
   for (MSize i = 0; i < strings->len; i++) {
      std::string value = std::to_string(i);
      GCstr *string = lj_str_new(lua, value.data(), value.size());
      setgcref(strings->get<GCRef>()[i], obj2gco(string));
      lj_gc_objbarrier(lua, strings, string);
   }

   GCarray *contiguous = lj_array_new_like(lua, strings, 3);
   lj_array_copy_to_fresh(lua, contiguous, 0, strings, 1, 1, 3);
   setarrayV(lua, lua->top++, contiguous);
   GCarray *descending = lj_array_new_like(lua, strings, 3);
   lj_array_copy_to_fresh(lua, descending, 0, strings, 5, -2, 3);
   setarrayV(lua, lua->top++, descending);
   GCarray *empty = lj_array_new_like(lua, strings, 0);
   lj_array_copy_to_fresh(lua, empty, 0, strings, strings->len, 1, 0);
   setarrayV(lua, lua->top++, empty);

   lj_gc_fullgc(lua);
   const char contiguous_expected[] = { '1', '2', '3' };
   const char descending_expected[] = { '5', '3', '1' };
   for (MSize i = 0; i < 3; i++) {
      GCstr *contiguous_string = strref(contiguous->get<GCRef>()[i]);
      GCstr *descending_string = strref(descending->get<GCRef>()[i]);
      if (contiguous_string->len != 1 or strdata(contiguous_string)[0] != contiguous_expected[i] or
          descending_string->len != 1 or strdata(descending_string)[0] != descending_expected[i]) {
         Log.error("a barrier-free fresh array copy lost a string reference at slot %d", int(i));
         return false;
      }
   }
   if (empty->len != 0) {
      Log.error("an empty fresh array copy changed the destination length");
      return false;
   }
   return true;
}

//********************************************************************************************************************

template<size_t Size>
static bool array_test_all_black(const std::array<GCarray *, Size> &Arrays)
{
   for (GCarray *array : Arrays) {
      if (not isblack(obj2gco(array))) return false;
   }
   return true;
}

template<size_t Size>
static bool array_test_move_to_incremental_black(lua_State *Lua, const std::array<GCarray *, Size> &Arrays)
{
   global_State *global = G(Lua);
   MSize original_step_multiplier = global->gc.stepmul;
   global->gc.stepmul = 1;
   for (unsigned step = 0; step < 1024; step++) {
      if (global->gc.state IS GCPhase::Propagate and array_test_all_black(Arrays)) {
         global->gc.stepmul = original_step_multiplier;
         return true;
      }
      if (step > 0 and global->gc.state IS GCPhase::Pause) break;
      lj_gc_step(Lua);
   }
   global->gc.stepmul = original_step_multiplier;
   return global->gc.state IS GCPhase::Propagate and array_test_all_black(Arrays);
}

static bool array_test_advance_to_phase(lua_State *Lua, GCPhase Phase)
{
   global_State *global = G(Lua);
   MSize original_step_multiplier = global->gc.stepmul;
   global->gc.stepmul = 1;
   for (unsigned step = 0; step < 4096; step++) {
      if (global->gc.state IS Phase) {
         global->gc.stepmul = original_step_multiplier;
         return true;
      }
      if (global->gc.state IS GCPhase::Pause) break;
      lj_gc_step(Lua);
   }
   global->gc.stepmul = original_step_multiplier;
   return global->gc.state IS Phase;
}

static bool array_test_finish_incremental_cycle(lua_State *Lua)
{
   global_State *global = G(Lua);
   for (unsigned step = 0; step < 4096 and global->gc.state != GCPhase::Pause; step++) lj_gc_step(Lua);
   return global->gc.state IS GCPhase::Pause;
}

static bool test_gc_ref_array_bulk_copy_incremental(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   GCarray *strings = lj_array_new(lua, 1, AET::STR_GC);
   setarrayV(lua, lua->top++, strings);
   GCarray *tables = lj_array_new(lua, 1, AET::TABLE);
   setarrayV(lua, lua->top++, tables);
   GCarray *arrays = lj_array_new(lua, 1, AET::ARRAY);
   setarrayV(lua, lua->top++, arrays);
   GCarray *objects = lj_array_new(lua, 1, AET::OBJECT);
   setarrayV(lua, lua->top++, objects);
   const std::array<GCarray *, 4> destinations = { strings, tables, arrays, objects };
   if (not array_test_move_to_incremental_black(lua, destinations)) {
      Log.error("GC-reference destinations did not become black during incremental propagation");
      return false;
   }

   GCarray *string_source = lj_array_new(lua, 1, AET::STR_GC);
   GCstr *string = lj_str_newz(lua, "bulk-copy-string");
   setgcref(string_source->get<GCRef>()[0], obj2gco(string));
   lj_array_copy_unchecked(lua, strings, 0, string_source, 0, 1);
   GCobj *first_grayagain = gcref(G(lua)->gc.grayagain);
   GCobj *first_array_link = gcref(strings->gclist);
   lj_array_copy_unchecked(lua, strings, 0, string_source, 0, 1);
   if (gcref(G(lua)->gc.grayagain) != first_grayagain or gcref(strings->gclist) != first_array_link) {
      Log.error("repeated GC-reference bulk copies linked an already-grey destination twice");
      return false;
   }

   GCarray *table_source = lj_array_new(lua, 1, AET::TABLE);
   GCtab *table = lj_tab_new(lua, 0, 1);
   setintV(lj_tab_setint(lua, table, 1), 71);
   setgcref(table_source->get<GCRef>()[0], obj2gco(table));
   lj_array_copy_unchecked(lua, tables, 0, table_source, 0, 1);

   GCarray *array_source = lj_array_new(lua, 1, AET::ARRAY);
   GCarray *nested = lj_array_new(lua, 1, AET::INT32);
   nested->get<int32_t>()[0] = 72;
   setgcref(array_source->get<GCRef>()[0], obj2gco(nested));
   lj_array_copy_unchecked(lua, arrays, 0, array_source, 0, 1);

   GCarray *object_source = lj_array_new(lua, 1, AET::OBJECT);
   GCobject *object = lj_object_new(lua, OBJECTID(73), nullptr, nullptr, GCOBJ_DETACHED);
   setgcref(object_source->get<GCRef>()[0], obj2gco(object));
   lj_array_copy_unchecked(lua, objects, 0, object_source, 0, 1);

   for (GCarray *destination : destinations) {
      if (not isgray(obj2gco(destination))) {
         Log.error("a black GC-reference destination was not moved to grayagain");
         return false;
      }
   }
   if (not array_test_finish_incremental_cycle(lua)) {
      Log.error("incremental collection did not complete after GC-reference bulk copies");
      return false;
   }

   GCstr *retained_string = strref(strings->get<GCRef>()[0]);
   GCtab *retained_table = tabref(tables->get<GCRef>()[0]);
   GCarray *retained_array = arrayref(arrays->get<GCRef>()[0]);
   GCobject *retained_object = objectref(objects->get<GCRef>()[0]);
   cTValue *table_value = lj_tab_getint(retained_table, 1);
   bool passed = retained_string IS string and retained_string->len IS sizeof("bulk-copy-string") - 1 and
      retained_table IS table and tvisnumber(table_value) and numberVnum(table_value) IS 71 and
      retained_array IS nested and retained_array->get<int32_t>()[0] IS 72 and retained_object IS object and
      retained_object->uid IS OBJECTID(73);
   if (not passed) Log.error("a GC-reference bulk copy lost a child during incremental collection");
   return passed;
}

static bool test_array_bulk_copy_small_span_forward_barrier(kt::Log &Log)
{
   constexpr MSize destination_size = 4096;
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   GCarray *string_destination = lj_array_new(lua, destination_size, AET::STR_GC);
   setarrayV(lua, lua->top++, string_destination);
   GCarray *any_destination = lj_array_new(lua, destination_size, AET::ANY);
   setarrayV(lua, lua->top++, any_destination);
   const std::array<GCarray *, 2> destinations = { string_destination, any_destination };
   if (not array_test_move_to_incremental_black(lua, destinations)) {
      Log.error("small-span destinations did not become black during incremental propagation");
      return false;
   }

   GCarray *string_source = lj_array_new(lua, 1, AET::STR_GC);
   setarrayV(lua, lua->top++, string_source);
   GCstr *string = lj_str_newz(lua, "small-span-string");
   setgcref(string_source->get<GCRef>()[0], obj2gco(string));
   GCarray *any_source = lj_array_new(lua, 1, AET::ANY);
   setarrayV(lua, lua->top++, any_source);
   GCstr *any_string = lj_str_newz(lua, "small-span-any");
   setstrV(lua, &any_source->get<TValue>()[0], any_string);

   lj_array_copy_unchecked(lua, string_destination, 2048, string_source, 0, 1);
   lj_array_copy_unchecked(lua, any_destination, 2048, any_source, 0, 1);
   if (not isblack(obj2gco(string_destination)) or not isblack(obj2gco(any_destination))) {
      Log.error("a small-span copy queued its large destination for traversal");
      return false;
   }
   if (iswhite(obj2gco(string)) or iswhite(obj2gco(any_string))) {
      Log.error("a small-span copy did not apply forward barriers to its children");
      return false;
   }
   if (not array_test_finish_incremental_cycle(lua)) {
      Log.error("incremental collection did not complete after small-span copies");
      return false;
   }
   lj_gc_fullgc(lua);

   GCstr *retained_string = strref(string_destination->get<GCRef>()[2048]);
   TValue *retained_any = &any_destination->get<TValue>()[2048];
   bool passed = retained_string IS string and retained_string->len IS sizeof("small-span-string") - 1 and
      tvisstr(retained_any) and strV(retained_any) IS any_string and
      any_string->len IS sizeof("small-span-any") - 1;
   if (not passed) Log.error("a small-span copy lost a child during collection");
   return passed;
}

static bool test_gc_ref_array_bulk_copy_overlap(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   GCarray *strings = lj_array_new(lua, 6, AET::STR_GC);
   setarrayV(lua, lua->top++, strings);
   std::array<GCstr *, 6> original;
   for (MSize i = 0; i < original.size(); i++) {
      std::string text = std::to_string(i);
      original[i] = lj_str_new(lua, text.data(), text.size());
      setgcref(strings->get<GCRef>()[i], obj2gco(original[i]));
   }

   lj_array_copy(lua, strings, 1, strings, 0, 5);
   lj_gc_fullgc(lua);
   for (MSize i = 0; i < 5; i++) {
      if (strref(strings->get<GCRef>()[i + 1]) != original[i]) {
         Log.error("an overlapping GC-reference bulk copy failed at slot %d", int(i + 1));
         return false;
      }
   }
   return true;
}

static bool test_gc_ref_array_bulk_copy_phases(kt::Log &Log)
{
   constexpr std::array<GCPhase, 6> phases = { GCPhase::Propagate, GCPhase::Atomic, GCPhase::SweepString,
      GCPhase::Sweep, GCPhase::Finalize, GCPhase::Pause };

   for (GCPhase phase : phases) {
      LuaStateHolder holder;
      lua_State *lua = holder.get();
      if (not lua) return false;
      GCarray *destination = lj_array_new(lua, 1, AET::STR_GC);
      setarrayV(lua, lua->top++, destination);
      const std::array<GCarray *, 1> destinations = { destination };
      global_State *global = G(lua);

      if (phase IS GCPhase::Finalize) global->gc.state = GCPhase::Finalize;
      else if (phase != GCPhase::Pause) {
         if (not array_test_move_to_incremental_black(lua, destinations)) return false;
         if (phase IS GCPhase::Atomic) global->gc.state = GCPhase::Atomic;
         else if (phase != GCPhase::Propagate and not array_test_advance_to_phase(lua, phase)) return false;
         if (not isblack(obj2gco(destination))) {
            Log.error("GC-reference destination was not black in collector phase %d", int(phase));
            return false;
         }
      }

      GCarray *source = lj_array_new(lua, 1, AET::STR_GC);
      std::string text = std::string("phase-") + std::to_string(int(phase));
      GCstr *string = lj_str_new(lua, text.data(), text.size());
      setgcref(source->get<GCRef>()[0], obj2gco(string));
      lj_array_copy_unchecked(lua, destination, 0, source, 0, 1);

      if (phase != GCPhase::Pause and not array_test_finish_incremental_cycle(lua)) return false;
      lj_gc_fullgc(lua);
      GCstr *retained = strref(destination->get<GCRef>()[0]);
      if (retained != string or retained->len != text.size() or
          std::string_view(strdata(retained), retained->len) != text) {
         Log.error("a GC-reference copy lost its child in collector phase %d", int(phase));
         return false;
      }
   }
   return true;
}

//********************************************************************************************************************
// Null-terminated pointer fields use a negative array size as a sentinel.  The struct reader must scan the pointed-to
// values before creating the cached array rather than passing that sentinel to the unsigned array allocator.

static bool test_struct_pointer_array_sentinel(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   int values[] = { 17, -4, 0 };
   APTR pointer = values;
   struct_field field;
   field.Type = FD_POINTER|FD_ARRAY|FD_INT;
   field.ArraySize = -1;

   lj_struct_getfield_core(lua, nullptr, field, &pointer);
   if (not lua_isarray(lua, -1)) {
      Log.error("struct pointer array field did not produce an array");
      return false;
   }

   auto array = lua_toarray(lua, -1);
   if ((array->len != 2) or (array->elemtype != AET::INT32)) {
      Log.error("struct pointer array field produced length %d and type %d", array->len, int(array->elemtype));
      return false;
   }

   auto result = array->get<int>();
   if ((result[0] != 17) or (result[1] != -4)) {
      Log.error("struct pointer array field did not copy values up to its sentinel");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// struct_to_table() receives the address of a native structure rather than using the struct field getter.  String
// vectors require the vector object itself because the array cache reads std::string elements through its vector header.

static bool test_struct_to_table_string_vector(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   kt::vector<std::string> names;
   names.push_back("alpha");
   names.push_back("beta");

   struct_record def("StringArrayResult");
   def.Size = int(sizeof(names));
   struct_field field;
   field.Name = "Names";
   field.Type = FD_VECTOR|FD_CPP|FD_STRING;
   field.ArraySize = 1;
   def.Fields.push_back(field);

   std::vector<lua_ref> references;
   if (struct_to_table(lua, references, def, &names) != ERR::Okay) {
      Log.error("failed to convert native structure containing a string vector");
      unref_struct_references(lua, references);
      return false;
   }
   unref_struct_references(lua, references);

   lua_getfield(lua, -1, "Names");
   if (not lua_isarray(lua, -1)) {
      Log.error("string vector field did not produce an array");
      return false;
   }

   auto array = lua_toarray(lua, -1);
   if ((array->len != 2) or (array->elemtype != AET::CSTR)) {
      Log.error("string vector field produced length %d and type %d", array->len, int(array->elemtype));
      return false;
   }

   auto result = array->get<CSTRING>();
   if ((std::string_view(result[0]) != "alpha") or (std::string_view(result[1]) != "beta")) {
      Log.error("C++ string array field did not preserve its values");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_array_elemsize(kt::Log &Log)
{
   if (lj_array_elemsize(AET::BYTE) != 1 or lj_array_elemsize(AET::INT8) != 1 or
       lj_array_elemsize(AET::UINT8) != 1) {
      Log.error("eight-bit array element size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::INT16) != 2 or lj_array_elemsize(AET::UINT16) != 2) {
      Log.error("sixteen-bit array element size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::INT32) != 4 or lj_array_elemsize(AET::UINT32) != 4) {
      Log.error("thirty-two-bit array element size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::INT64) != 8 or lj_array_elemsize(AET::UINT64) != 8) {
      Log.error("sixty-four-bit array element size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::FLOAT) != 4) {
      Log.error("AET::FLOAT size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::DOUBLE) != 8) {
      Log.error("AET::DOUBLE size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::PTR) != sizeof(void*)) {
      Log.error("AET::PTR size incorrect");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_array_external_unmanaged(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   int32_t external_data[2] = { 10, 20 };
   GCarray *array = lj_array_new(lua, 2, AET::INT32, external_data, ARRAY_EXTERNAL|ARRAY_READONLY);
   if (array->resource_id) {
      Log.error("unmanaged external array retained a resource pin");
      return false;
   }

   external_data[1] = 30;
   if (array->get<int32_t>()[1] != 30) {
      Log.error("unmanaged external array did not observe native mutation");
      return false;
   }

   setarrayV(lua, lua->top++, array);
   lua_settop(lua, 0);
   lua_gc(lua, LUA_GCCOLLECT);
   return true;
}

static bool test_array_external_resource(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(lua);

   APTR memory = nullptr;
   if (AllocResource(sizeof(int32_t) * 2, MEM::NIL, &memory, &glArrayResourceManager) != ERR::Okay) return false;
   auto resource_id = GetMemoryID(memory);
   auto values = (int32_t *)memory;
   values[0] = 10;
   values[1] = 20;
   glArrayResourceFrees.store(0, std::memory_order_relaxed);

   GCarray *array = lj_array_new(lua, 2, AET::INT32, memory, ARRAY_EXTERNAL|ARRAY_READONLY);
   setarrayV(lua, lua->top++, array);
   if (PinResource(resource_id) != ERR::Okay) return false;
   array->resource_id = resource_id;
   lua_setglobal(lua, "array_resource_view");

   if (dostring(lua, "local total = 0; for index in {0 to 200} do "
         "total += array_resource_view[index % #array_resource_view] end; assert(total is 3000)") != 0) {
      Log.error("resource-backed external array did not use the normal hot-loop load path");
      return false;
   }

   if ((array->resource_id != resource_id) or (FreeResource(resource_id) != ERR::InUse) or
       (array->get<int32_t>()[1] != 20)) {
      Log.error("resource-backed external array did not retain readable storage");
      return false;
   }

   lua_pushnil(lua);
   lua_setglobal(lua, "array_resource_view");
   lua_gc(lua, LUA_GCCOLLECT);
   if (glArrayResourceFrees.load(std::memory_order_relaxed) != 1) {
      Log.error("resource-backed external array did not release its pin exactly once");
      return false;
   }

   memory = nullptr;
   if (AllocResource(sizeof(int32_t), MEM::NIL, &memory, &glArrayResourceManager) != ERR::Okay) return false;
   resource_id = GetMemoryID(memory);
   values = (int32_t *)memory;
   values[0] = 42;
   glArrayResourceFrees.store(0, std::memory_order_relaxed);

   GCarray *first = lj_array_new(lua, 1, AET::INT32, memory, ARRAY_EXTERNAL|ARRAY_READONLY);
   setarrayV(lua, lua->top++, first);
   if (PinResource(resource_id) != ERR::Okay) return false;
   first->resource_id = resource_id;

   GCarray *second = lj_array_new(lua, 1, AET::INT32, memory, ARRAY_EXTERNAL|ARRAY_READONLY);
   setarrayV(lua, lua->top++, second);
   if (PinResource(resource_id) != ERR::Okay) return false;
   second->resource_id = resource_id;

   if (FreeResource(resource_id) != ERR::InUse) return false;
   lua_settop(lua, 1);
   lua_gc(lua, LUA_GCCOLLECT);
   if ((glArrayResourceFrees.load(std::memory_order_relaxed) != 0) or (first->get<int32_t>()[0] != 42)) {
      Log.error("the first collected view released a multiply pinned resource");
      return false;
   }

   lua_settop(lua, 0);
   lua_gc(lua, LUA_GCCOLLECT);
   if (glArrayResourceFrees.load(std::memory_order_relaxed) != 1) {
      Log.error("the final collected view did not release the deferred resource");
      return false;
   }

   int32_t local_value = 7;
   GCarray *failed = lj_array_new(lua, 1, AET::INT32, &local_value, ARRAY_EXTERNAL|ARRAY_READONLY);
   setarrayV(lua, lua->top++, failed);
   auto missing_id = AllocateID(IDTYPE::RESOURCE);
   if ((PinResource(missing_id) IS ERR::Okay) or failed->resource_id) {
      Log.error("a failed resource pin left an ownership token on the array");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_array_unsupported_storage_contract(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   TValue value;
   GCarray *pointers = lj_array_new(lua, 1, AET::PTR);
   setrawlightudV(&value, lua);
   if (lj_array_validate_element(pointers, &value) != ArrayElementResult::OK) {
      Log.error("pointer storage rejected a raw pointer value");
      return false;
   }
   GCarray *cached_strings = lj_array_new(lua, 0, AET::CSTR);
   if (lj_array_validate_element(cached_strings, &value) != ArrayElementResult::UNSUPPORTED_STORAGE) {
      Log.error("cached-string storage accepted a direct write");
      return false;
   }
   setnilV(&value);
   if (lj_array_validate_element(pointers, &value) != ArrayElementResult::INVALID_TYPE) {
      Log.error("pointer storage accepted nil as an element");
      return false;
   }
   if (lj_array_validate_element(cached_strings, &value) != ArrayElementResult::UNSUPPORTED_STORAGE) {
      Log.error("cached-string storage did not retain its unsupported-storage result for nil");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_array_type_tag(kt::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 10, AET::BYTE);

   if (arr->gct != uint8_t(~LJ_TARRAY)) {
      Log.error("array has incorrect GC type tag: %d, expected %d", arr->gct, uint8_t(~LJ_TARRAY));
      return false;
   }

   return true;
}

//********************************************************************************************************************
// VM Type System Integration

static bool test_tvalue_array(kt::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 10, AET::INT32);

   // Create a TValue holding the array
   TValue tv;
   setgcVraw(&tv, obj2gco(arr), LJ_TARRAY);

   if (itype(&tv) != LJ_TARRAY) {
      Log.error("TValue itype does not match LJ_TARRAY: %u vs %u", itype(&tv), LJ_TARRAY);
      return false;
   }
   if (!tvisarray(&tv)) {
      Log.error("tvisarray check failed");
      return false;
   }
   if (arrayV(&tv) != arr) {
      Log.error("arrayV does not extract correct pointer");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_setarrayV(kt::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 5, AET::DOUBLE);

   TValue tv;
   setarrayV(L, &tv, arr);

   if (!tvisarray(&tv)) {
      Log.error("setarrayV did not set array type");
      return false;
   }
   if (arrayV(&tv) != arr) {
      Log.error("setarrayV did not store correct pointer");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Bytecode C Helpers

// Helper to check if TValue contains an integer value (handles LJ_DUALNUM=0 case)
static bool tv_is_integer(cTValue* o, int32_t expected)
{
   if (tvisint(o)) return intV(o) IS expected;
   if (tvisnum(o)) return numberVint(o) IS expected;
   return false;
}

static bool test_any_array_bulk_copy_collection(kt::Log &Log)
{
   constexpr MSize count = 64;
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   GCarray *numeric_destination = lj_array_new(lua, count, AET::ANY);
   setarrayV(lua, lua->top++, numeric_destination);
   GCarray *sparse_destination = lj_array_new(lua, count, AET::ANY);
   setarrayV(lua, lua->top++, sparse_destination);
   GCarray *dense_destination = lj_array_new(lua, count, AET::ANY);
   setarrayV(lua, lua->top++, dense_destination);
   const std::array<GCarray *, 3> destinations = {
      numeric_destination, sparse_destination, dense_destination
   };
   if (not array_test_move_to_incremental_black(lua, destinations)) {
      Log.error("ANY destinations did not become black during incremental propagation");
      return false;
   }

   GCarray *numeric_source = lj_array_new(lua, count, AET::ANY);
   GCarray *sparse_source = lj_array_new(lua, count, AET::ANY);
   GCarray *dense_source = lj_array_new(lua, count, AET::ANY);
   std::array<GCstr *, count> sparse_strings = {};
   std::array<GCstr *, count> dense_strings = {};
   for (MSize i = 0; i < count; i++) {
      setintV(&numeric_source->get<TValue>()[i], int32_t(i * 3));
      setintV(&sparse_source->get<TValue>()[i], int32_t(i * 5));
      std::string dense_text = std::string("dense-") + std::to_string(i);
      dense_strings[i] = lj_str_new(lua, dense_text.data(), dense_text.size());
      setstrV(lua, &dense_source->get<TValue>()[i], dense_strings[i]);
      if ((i % 16) IS 0) {
         std::string sparse_text = std::string("sparse-") + std::to_string(i);
         sparse_strings[i] = lj_str_new(lua, sparse_text.data(), sparse_text.size());
         setstrV(lua, &sparse_source->get<TValue>()[i], sparse_strings[i]);
      }
   }

   lj_array_copy_unchecked(lua, numeric_destination, 0, numeric_source, 0, count);
   lj_array_copy_unchecked(lua, sparse_destination, 0, sparse_source, 0, count);
   lj_array_copy_unchecked(lua, dense_destination, 0, dense_source, 0, count);
   for (GCarray *destination : destinations) {
      if (not isgray(obj2gco(destination))) {
         Log.error("a black ANY destination was not moved to grayagain");
         return false;
      }
   }
   if (not array_test_finish_incremental_cycle(lua)) {
      Log.error("incremental collection did not complete after ANY bulk copies");
      return false;
   }
   lj_gc_fullgc(lua);

   for (MSize i = 0; i < count; i++) {
      cTValue *numeric = &numeric_destination->get<TValue>()[i];
      cTValue *sparse = &sparse_destination->get<TValue>()[i];
      cTValue *dense = &dense_destination->get<TValue>()[i];
      if (not tv_is_integer(numeric, int32_t(i * 3))) {
         Log.error("numeric ANY bulk copy failed at slot %d", int(i));
         return false;
      }
      if ((i % 16) IS 0) {
         std::string expected = std::string("sparse-") + std::to_string(i);
         GCstr *retained = tvisstr(sparse) ? strV(sparse) : nullptr;
         if (not retained or retained != sparse_strings[i] or retained->len != expected.size() or
             std::string_view(strdata(retained), retained->len) != expected) {
            Log.error("sparse ANY bulk copy lost a reference at slot %d", int(i));
            return false;
         }
      }
      else if (not tv_is_integer(sparse, int32_t(i * 5))) {
         Log.error("sparse ANY bulk copy lost a numeric value at slot %d", int(i));
         return false;
      }
      std::string expected = std::string("dense-") + std::to_string(i);
      GCstr *retained = tvisstr(dense) ? strV(dense) : nullptr;
      if (not retained or retained != dense_strings[i] or retained->len != expected.size() or
          std::string_view(strdata(retained), retained->len) != expected) {
         Log.error("dense ANY bulk copy lost a reference at slot %d", int(i));
         return false;
      }
   }
   return true;
}

//********************************************************************************************************************

static bool test_array_load_allocation_classification(kt::Log &Log)
{
   for (int type_index = 0; type_index < int(AET::MAX); type_index++) {
      AET element_type = AET(type_index);
      bool expected = element_type IS AET::CSTR or element_type IS AET::STR_CPP or element_type IS AET::STRUCT;
      if (array_element_load_allocates(element_type) != expected) {
         Log.error("array load allocation classification is incorrect for type %d", type_index);
         return false;
      }
   }
   return true;
}

static bool test_arr_getidx_noalloc(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(lua);

   TValue result;
   GCarray *integers = lj_array_new(lua, 1, AET::INT32);
   integers->get<int32_t>()[0] = 73;
   lj_arr_getidx_noalloc(lua, integers, 0, &result);
   if (not tv_is_integer(&result, 73)) {
      Log.error("no-allocation integer array load failed");
      return false;
   }

   GCarray *strings = lj_array_new(lua, 1, AET::STR_GC);
   GCstr *text = lj_str_newz(lua, "native");
   TValue value;
   setstrV(lua, &value, text);
   lj_array_store_checked(lua, strings, 0, &value);
   lj_arr_getidx_noalloc(lua, strings, 0, &result);
   if (not tvisstr(&result) or strV(&result) != text) {
      Log.error("no-allocation GC-reference array load failed");
      return false;
   }

   GCarray *values = lj_array_new(lua, 3, AET::ANY);
   TValue *slots = values->get<TValue>();
   setnilV(&slots[0]);
   setnumV(&slots[1], 4.5);
   setstrV(lua, &slots[2], text);
   lj_gc_objbarrier(lua, values, text);

   lj_arr_getidx_noalloc(lua, values, 0, &result);
   if (not tvisnil(&result)) {
      Log.error("no-allocation ANY nil load failed");
      return false;
   }
   lj_arr_getidx_noalloc(lua, values, 1, &result);
   if (not tvisnum(&result) or numV(&result) != 4.5) {
      Log.error("no-allocation ANY number load failed");
      return false;
   }
   lj_arr_getidx_noalloc(lua, values, 2, &result);
   if (not tvisstr(&result) or strV(&result) != text) {
      Log.error("no-allocation ANY string load failed");
      return false;
   }

   return true;
}

// Test runner

} // namespace

void array_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 19> Tests = { {
      // Core Data Structures
      { "array_recursive_identity", test_array_recursive_identity },
      { "array_strided_copy", test_array_strided_copy },
      { "array_fresh_copy", test_array_fresh_copy },
      { "gc_ref_array_bulk_copy_incremental", test_gc_ref_array_bulk_copy_incremental },
      { "array_bulk_copy_small_span_forward_barrier", test_array_bulk_copy_small_span_forward_barrier },
      { "gc_ref_array_bulk_copy_overlap", test_gc_ref_array_bulk_copy_overlap },
      { "gc_ref_array_bulk_copy_phases", test_gc_ref_array_bulk_copy_phases },
      { "struct_pointer_array_sentinel", test_struct_pointer_array_sentinel },
      { "struct_to_table_string_vector", test_struct_to_table_string_vector },
      { "array_elemsize", test_array_elemsize },
      { "array_external_unmanaged", test_array_external_unmanaged },
      { "array_external_resource", test_array_external_resource },
      { "array_unsupported_storage_contract", test_array_unsupported_storage_contract },
      { "array_type_tag", test_array_type_tag },
      // VM Type System
      { "tvalue_array", test_tvalue_array },
      { "setarrayV", test_setarrayV },
      // Bytecode C Helpers
      { "array_load_allocation_classification", test_array_load_allocation_classification },
      { "arr_getidx_noalloc", test_arr_getidx_noalloc },
      { "any_array_bulk_copy_collection", test_any_array_bulk_copy_collection }
   } };

   if (NewObject(CLASSID::TIRI, &glArrayTestScript) != ERR::Okay) return;
   glArrayTestScript->setStatement("");
   if (Action(AC::Init, glArrayTestScript, nullptr) != ERR::Okay) return;

   for (const TestCase& Test : Tests) {
      kt::Log Log("ArrayTests");
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

   FreeResource(glArrayTestScript);
   glArrayTestScript = nullptr;
}

#else

#endif // UNIT_TESTS
