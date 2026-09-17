// Unit tests for table internals that cannot be observed from Tiri.

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tab.h"

#include <array>

#include "../../defs.h"

static extTiri *glTestScript = nullptr;

namespace {

struct LuaStateHolder {
   LuaStateHolder()
   {
      this->state = luaL_newstate(glTestScript);
   }

   ~LuaStateHolder()
   {
      if (this->state) lua_close(this->state);
   }

   lua_State * get() const { return this->state; }

private:
   lua_State *state = nullptr;
};

struct TestCase {
   const char *name;
   bool (*fn)(kt::Log &Log);
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

static bool test_table_hash_mixer_matches_expected(kt::Log &Log)
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

static bool test_table_string_hash_uses_interned_sid(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab *table = lj_tab_new(lua, 0, 5);
   GCstr *key = lj_str_newlit(lua, "hash_string_key");
   TValue value;
   setnumV(&value, 42.0);
   copyTV(lua, lj_tab_setstr(lua, table, key), &value);

   Node *expected_node = hashmask(table, key->sid);
   Node *actual_node = hashstr(table, key);
   if (not (actual_node IS expected_node)) {
      Log.error("string hash key no longer uses its interned sid");
      return false;
   }

   return true;
}

static bool test_mutable_string_key_survives_native_mutation(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab *table = lj_tab_new(lua, 0, 1);
   settabV(lua, lua->top, table);
   incr_top(lua);
   GCstr *key = lj_str_newbuf(lua, 4);
   setstrV(lua, lua->top, key);
   incr_top(lua);
   memcpy(strdatawr(key), "key1", 4);

   TValue value;
   setnumV(&value, 42.0);
   copyTV(lua, lj_tab_setstr(lua, table, key), &value);

   cTValue *found = lj_tab_getstr(table, key);
   if (not found or not tvisnum(found) or numV(found) != 42.0) {
      Log.error("mutable string key did not retrieve its table value before mutation");
      return false;
   }
   const uint32_t sid = key->sid;
   memcpy(strdatawr(key), "key2", 4);
   GCstr *mutated_content = lj_str_newlit(lua, "key2");
   if (key->sid != sid or key->hash != 0 or lj_str_cmp(key, mutated_content) != 0) {
      Log.error("mutable string metadata or content comparison changed after native mutation");
      return false;
   }
   if (lj_tab_getstr(table, key) != found or lj_tab_getstr(table, mutated_content)) {
      Log.error("native mutation invalidated mutable string identity-key lookup");
      return false;
   }

   return true;
}

static bool test_table_flags_initialise_clear(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   GCtab *colocated = lj_tab_new(lua, 4, 0);
   GCtab *separated = lj_tab_new(lua, 0, 3);
   if (colocated->flags != 0 or separated->flags != 0) {
      Log.error("a new table did not initialise its flags to zero");
      return false;
   }

   colocated->flags |= TAB_METHOD_COMPATIBLE;
   if (not lj_tab_is_sequence(colocated) or (colocated->flags & TAB_NOT_SEQUENCE) != 0) {
      Log.error("method-compatible metadata affected table classification");
      return false;
   }

   return true;
}

} // namespace

extern void indexing_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 4> Tests = { {
      { "table_hash_mixer_matches_expected", test_table_hash_mixer_matches_expected },
      { "table_string_hash_uses_interned_sid", test_table_string_hash_uses_interned_sid },
      { "mutable_string_key_survives_native_mutation", test_mutable_string_key_survives_native_mutation },
      { "table_flags_initialise_clear", test_table_flags_initialise_clear }
   } };

   if (NewObject(CLASSID::TIRI, &glTestScript) != ERR::Okay) return;
   glTestScript->setStatement("");
   if (Action(AC::Init, glTestScript, nullptr) != ERR::Okay) {
      FreeResource(glTestScript);
      glTestScript = nullptr;
      return;
   }

   for (const TestCase &test : Tests) {
      kt::Log log("IndexingTests");
      log.branch("Running %s", test.name);
      ++Total;
      if (test.fn(log)) {
         ++Passed;
         log.msg("%s passed", test.name);
      }
      else log.error("%s failed", test.name);
   }

   FreeResource(glTestScript);
   glTestScript = nullptr;
}

#endif // UNIT_TESTS
