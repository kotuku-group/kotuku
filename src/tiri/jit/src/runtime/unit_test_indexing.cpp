// Unit tests for array indexing configuration.

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

#include <array>
#include <string>
#include <string_view>

#include "../../defs.h"

static objTiri *glTestScript = nullptr;

namespace {

#undef STRINGIFY
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

struct LuaStateHolder {
   LuaStateHolder()
   {
      this->state = luaL_newstate(glTestScript);
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

struct TestCase {
   const char* name;
   bool (*fn)(kt::Log& Log);
};

static uint32_t expected_table_hash(uint32_t Lo, uint32_t Hi)
{
#if LJ_TARGET_X64
   uint64_t hash = (uint64_t(Hi) << 32) | uint64_t(Lo);
   hash = (hash ^ (hash >> 30)) * HASH_MIX64_MUL1;
   hash = (hash ^ (hash >> 27)) * HASH_MIX64_MUL2;
   hash ^= hash >> 31;
   return uint32_t(hash);
#else
   return hashrot(Lo, Hi);
#endif
}

// Execute Lua code and check result
static bool run_lua_test(lua_State* L, std::string_view Code, std::string& Error)
{
   if (lua_load(L, Code, "indexing-test")) {
      Error = lua_tostring(L, -1);
      lua_pop(L, 1);
      return false;
   }
   if (lua_pcall(L, 0, LUA_MULTRET, 0)) {
      Error = lua_tostring(L, -1);
      lua_pop(L, 1);
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Core indexing tests - validate 0-based indexing

static bool test_array_first_element_access(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   const char* Code = R"(
      local t = {10, 20, 30}
      return t[0]
   )";  // 0-based: first element at index 0

   if (not run_lua_test(L, Code, Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   lua_Number Value = lua_tonumber(L, -1);
   if (Value IS 10) {
      return true;
   }

   Log.error("expected first element to be 10, got %g", Value);
   return false;
}

static bool test_table_length_operator(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   if (not run_lua_test(L, "local t = {1, 2, 3, 4, 5} return #t", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   lua_Number Length = lua_tonumber(L, -1);
   if (Length IS 5) return true;

   Log.error("expected length 5, got %g", Length);
   return false;
}

static bool test_ipairs_starting_index(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // First, check what ipairs() returns as the initial control variable
   std::string Error;
   const char* CheckInit = R"(
      local iter, t, init = ipairs({10, 20, 30})
      return init
   )";

   if (not run_lua_test(L, CheckInit, Error)) {
      Log.error("ipairs init check failed: %s", Error.c_str());
      return false;
   }
   lua_Number InitVal = lua_tonumber(L, -1);
   Log.msg("ipairs() returned init value: %g", InitVal);
   lua_pop(L, 1);

   // Now check the first call to ipairs_aux
   const char* CheckFirstCall = R"(
      local iter, t, init = ipairs({10, 20, 30})
      local idx, val = iter(t, init)
      return idx, val
   )";

   if (not run_lua_test(L, CheckFirstCall, Error)) {
      Log.error("ipairs_aux first call failed: %s", Error.c_str());
      return false;
   }
   lua_Number FirstIdx = lua_tonumber(L, -2);
   lua_Number FirstVal = lua_tonumber(L, -1);
   Log.msg("First ipairs_aux call: idx=%g, val=%g", FirstIdx, FirstVal);
   lua_pop(L, 2);

   // Now run the full iteration test
   const char* Code = R"(
      local first = nil
      local count = 0
      for i, v in ipairs({10, 20, 30}) do
         if not first then first = i end
         count = count + 1
      end
      return first, count
   )";

   if (not run_lua_test(L, Code, Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   lua_Number First = lua_tonumber(L, -2);
   lua_Number Count = lua_tonumber(L, -1);
   if (First IS 0 and Count IS 3) return true;  // 0-based: first index is 0

   Log.error("expected first index 0 and count 3, got %g and %g", First, Count);
   return false;
}

static bool test_table_insert_position(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   const char* Code = R"(
      local t = {}
      table.insert(t, 'a')
      table.insert(t, 'b')
      return t[0], t[1], #t
   )";  // 0-based: elements at indices 0 and 1

   if (not run_lua_test(L, Code, Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   const char* First = lua_tostring(L, -3);
   const char* Second = lua_tostring(L, -2);
   lua_Number Length = lua_tonumber(L, -1);

   if (First and Second and std::string_view(First) IS std::string_view("a") and
      std::string_view(Second) IS std::string_view("b") and Length IS 2) {
      return true;
   }

   Log.error("unexpected table.insert results, got '%s', '%s', len=%g", First ? First : "(nil)",
      Second ? Second : "(nil)", Length);
   return false;
}

static bool test_string_find_returns_correct_index(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   if (not run_lua_test(L, "return string.find('hello', 'l')", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   lua_Number Index = lua_tonumber(L, -2);
   lua_Number Expected = 2;  // 0-based: 'l' is at index 2 in "hello"
   if (Index IS Expected) return true;

   Log.error("expected index %g, got %g", Expected, Index);
   return false;
}

static bool test_string_byte_default_start(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   if (not run_lua_test(L, "return string.byte('ABC')", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   lua_Number Byte = lua_tonumber(L, -1);
   if (Byte IS 65) return true;

   Log.error("expected first byte 65, got %g", Byte);
   return false;
}

static bool test_table_concat_default_range(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   if (not run_lua_test(L, "return table.concat({'a', 'b', 'c'}, ',')", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("a,b,c")) return true;

   Log.error("expected 'a,b,c', got '%s'", Result ? Result : "(nil)");
   return false;
}

static bool test_table_sort_operates_on_sequence(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   const char* Code = R"(
      local t = {3, 1, 2}
      table.sort(t)
      return t[0], t[1], t[2]
   )";  // 0-based: elements at indices 0, 1, 2

   if (not run_lua_test(L, Code, Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   lua_Number A = lua_tonumber(L, -3);
   lua_Number B = lua_tonumber(L, -2);
   lua_Number C = lua_tonumber(L, -1);
   if (A IS 1 and B IS 2 and C IS 3) return true;

   Log.error("expected sorted values 1, 2, 3, got %g, %g, %g", A, B, C);
   return false;
}

static bool test_empty_table_length(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   if (not run_lua_test(L, "return #{}", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   lua_Number Length = lua_tonumber(L, -1);
   if (Length IS 0) return true;

   Log.error("expected empty table length 0, got %g", Length);
   return false;
}

static bool test_negative_string_indices_unchanged(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   std::string Error;
   if (not run_lua_test(L, "return string.substr('hello', -1)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("o")) return true;

   Log.error("expected 'o', got '%s'", Result ? Result : "(nil)");
   return false;
}

//********************************************************************************************************************
// Low-level table API tests

static bool test_lj_tab_getint_semantic_index(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab* Table = lj_tab_new(L, 4, 0);
   TValue Value;
   setintV(&Value, 10);
   copyTV(L, lj_tab_setint(L, Table, 0), &Value);  // 0-based
   setintV(&Value, 20);
   copyTV(L, lj_tab_setint(L, Table, 1), &Value);
   setintV(&Value, 30);
   copyTV(L, lj_tab_setint(L, Table, 2), &Value);

   cTValue* First = lj_tab_getint(Table, 0);  // 0-based: first element at index 0
   // Note: tvisint() returns false when LJ_DUALNUM is not enabled (the default).
   // In that case, setintV stores the value as a number, so check tvisnumber instead.
   if (First and tvisnumber(First) and numberVnum(First) == 10.0) return true;

   Log.error("lj_tab_getint failed for first element");
   return false;
}

static bool test_lj_tab_len_returns_element_count(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab* Table = lj_tab_new(L, 4, 0);
   TValue Value;
   for (int Index = 0; Index < 3; Index++) {
      setintV(&Value, (Index + 1) * 10);
      copyTV(L, lj_tab_setint(L, Table, Index), &Value);  // 0-based
   }

   MSize Length = lj_tab_len(Table);
   if (Length IS 3) return true;

   Log.error("expected lj_tab_len to return 3, got %u", (unsigned)Length);
   return false;
}

static bool test_table_hash_mixer_matches_expected(kt::Log& Log)
{
   constexpr std::array<uint32_t, 4> LoValues = { 0x00000000u, 0x89abcdefu, 0xffffffffu, 0x13579bdfu };
   constexpr std::array<uint32_t, 4> HiValues = { 0x00000000u, 0x01234567u, 0x80000000u, 0xfedcba98u };

   for (size_t index = 0; index < LoValues.size(); index++) {
      const uint32_t hash = hashlohi_bits(LoValues[index], HiValues[index]);
      const uint32_t expected = expected_table_hash(LoValues[index], HiValues[index]);
      if (not (hash IS expected)) {
         Log.error("hashlohi_bits mismatch at %u: got 0x%08x expected 0x%08x",
            (unsigned)index, hash, expected);
         return false;
      }
   }

   return true;
}

static bool test_table_numeric_hash_keys_roundtrip(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab* table = lj_tab_new(L, 0, 5);
   constexpr std::array<lua_Number, 4> Keys = { 0.5, 1.5, 65536.5, 131072.5 };

   for (size_t index = 0; index < Keys.size(); index++) {
      TValue key;
      TValue value;
      setnumV(&key, Keys[index]);
      setnumV(&value, lua_Number(index) + 10.0);
      copyTV(L, lj_tab_set(L, table, &key), &value);
   }

   for (size_t index = 0; index < Keys.size(); index++) {
      TValue key;
      setnumV(&key, Keys[index]);
      cTValue* found = lj_tab_get(L, table, &key);
      const lua_Number expected = lua_Number(index) + 10.0;
      if ((not found) or (not tvisnum(found)) or not (numV(found) IS expected)) {
         Log.error("numeric hash key %u did not roundtrip", (unsigned)index);
         return false;
      }
   }

   return true;
}

static bool test_table_gc_hash_keys_roundtrip(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab* table = lj_tab_new(L, 0, 5);
   GCtab* key_one = lj_tab_new(L, 0, 0);
   GCtab* key_two = lj_tab_new(L, 0, 0);
   std::array<GCtab*, 2> Keys = { key_one, key_two };

   for (size_t index = 0; index < Keys.size(); index++) {
      TValue key;
      TValue value;
      settabV(L, &key, Keys[index]);
      setnumV(&value, lua_Number(index) + 20.0);
      copyTV(L, lj_tab_set(L, table, &key), &value);
   }

   for (size_t index = 0; index < Keys.size(); index++) {
      TValue key;
      settabV(L, &key, Keys[index]);
      cTValue* found = lj_tab_get(L, table, &key);
      const lua_Number expected = lua_Number(index) + 20.0;
      if ((not found) or (not tvisnum(found)) or not (numV(found) IS expected)) {
         Log.error("GC hash key %u did not roundtrip", (unsigned)index);
         return false;
      }
   }

   return true;
}

static bool test_table_string_hash_keys_unchanged(kt::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab* table = lj_tab_new(L, 0, 5);
   GCstr* key = lj_str_newlit(L, "hash_string_key");
   TValue value;
   setnumV(&value, 42.0);
   copyTV(L, lj_tab_setstr(L, table, key), &value);

   cTValue* found = lj_tab_getstr(table, key);
   if ((not found) or (not tvisnum(found)) or not (numV(found) IS 42.0)) {
      Log.error("string hash key did not roundtrip");
      return false;
   }

   Node* expected_node = hashmask(table, key->sid);
   Node* actual_node = hashstr(table, key);
   if (not (actual_node IS expected_node)) {
      Log.error("string hash key no longer uses interned sid");
      return false;
   }

   return true;
}

}  // namespace

extern void indexing_unit_tests(int& Passed, int& Total)
{
   constexpr std::array<TestCase, 16> Tests = { {
      { "array_first_element_access", test_array_first_element_access },
      { "table_length_operator", test_table_length_operator },
      { "ipairs_starting_index", test_ipairs_starting_index },
      { "table_insert_position", test_table_insert_position },
      { "string_find_returns_correct_index", test_string_find_returns_correct_index },
      { "string_byte_default_start", test_string_byte_default_start },
      { "table_concat_default_range", test_table_concat_default_range },
      { "table_sort_operates_on_sequence", test_table_sort_operates_on_sequence },
      { "empty_table_length", test_empty_table_length },
      { "negative_string_indices_unchanged", test_negative_string_indices_unchanged },
      { "lj_tab_getint_semantic_index", test_lj_tab_getint_semantic_index },
      { "lj_tab_len_returns_element_count", test_lj_tab_len_returns_element_count },
      { "table_hash_mixer_matches_expected", test_table_hash_mixer_matches_expected },
      { "table_numeric_hash_keys_roundtrip", test_table_numeric_hash_keys_roundtrip },
      { "table_gc_hash_keys_roundtrip", test_table_gc_hash_keys_roundtrip },
      { "table_string_hash_keys_unchanged", test_table_string_hash_keys_unchanged }
   } };

   if (NewObject(CLASSID::TIRI, &glTestScript) != ERR::Okay) return;
   glTestScript->setStatement("");
   if (Action(AC::Init, glTestScript, nullptr) != ERR::Okay) return;

   for (const TestCase& Test : Tests) {
      kt::Log Log("IndexingTests");
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
