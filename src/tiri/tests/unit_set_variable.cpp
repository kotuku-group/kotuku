/*********************************************************************************************************************

Unit tests for the ti::SetVariable() host API.

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>

#include <array>
#include <string>
#include <string_view>

#include "../defs.h"

#ifdef UNIT_TESTS

namespace {

class SetVariableTestScript {
public:
   ~SetVariableTestScript()
   {
      if (this->script) FreeResource(this->script);
   }

   bool initialise(std::string_view Statement, kt::Log &Log)
   {
      if (NewObject(CLASSID::TIRI, &this->script) != ERR::Okay) {
         Log.error("failed to create a Tiri test object");
         return false;
      }
      if (this->script->setStatement(Statement) != ERR::Okay or
          Action(AC::Init, this->script, nullptr) != ERR::Okay) {
         Log.error("failed to initialise a Tiri test object");
         return false;
      }
      return true;
   }

   extTiri *get() const { return (extTiri *)this->script; }

private:
   objTiri *script = nullptr;
};

static bool activate_test_script(extTiri *Script, kt::Log &Log)
{
   if (Action(AC::Activate, Script, nullptr) != ERR::Okay or Script->Error != ERR::Okay) {
      Log.error("failed to activate test script: %s", Script->ErrorMessage.c_str());
      return false;
   }
   return true;
}

static bool global_string_is(lua_State *Lua, std::string_view Name, std::string_view Expected)
{
   lua_getglobal(Lua, Name);
   auto value = lua_tostringview(Lua, -1);
   bool matches = value IS Expected;
   lua_pop(Lua, 1);
   return matches;
}

static bool global_is_nil(lua_State *Lua, std::string_view Name)
{
   lua_getglobal(Lua, Name);
   bool is_nil = lua_isnil(Lua, -1);
   lua_pop(Lua, 1);
   return is_nil;
}

static bool test_set_variable_pre_activation(kt::Log &Log)
{
   SetVariableTestScript holder;
   if (not holder.initialise("global glSeedObserved = glSeedString", Log)) return false;
   auto script = holder.get();
   int pointer_target = 1;

   if (ti::SetVariable(script, "glSeedString", FD_STRING, (STRING)"seed") != ERR::Okay or
       ti::SetVariable(script, "glSeedPointer", FD_POINTER, (APTR)&pointer_target) != ERR::Okay or
       ti::SetVariable(script, "glSeedInt", FD_INT, 23) != ERR::Okay or
       ti::SetVariable(script, "glSeedInt64", FD_INT64, int64_t(9000000000)) != ERR::Okay or
       ti::SetVariable(script, "glSeedDouble", FD_DOUBLE, 2.5) != ERR::Okay) {
      Log.error("a supported pre-activation SetVariable() store failed");
      return false;
   }

   if (not activate_test_script(script, Log)) return false;
   lua_State *lua = script->Lua;
   if (not global_string_is(lua, "glSeedString", "seed") or
       not global_string_is(lua, "glSeedObserved", "seed")) {
      Log.error("the script did not observe the seeded string");
      return false;
   }

   lua_getglobal(lua, "glSeedPointer");
   bool pointer_matches = lua_touserdata(lua, -1) IS &pointer_target;
   lua_pop(lua, 1);
   lua_getglobal(lua, "glSeedInt");
   bool int_matches = lua_tointeger(lua, -1) IS 23;
   lua_pop(lua, 1);
   lua_getglobal(lua, "glSeedInt64");
   bool int64_matches = lua_tonumber(lua, -1) IS lua_Number(9000000000LL);
   lua_pop(lua, 1);
   lua_getglobal(lua, "glSeedDouble");
   bool double_matches = lua_tonumber(lua, -1) IS 2.5;
   lua_pop(lua, 1);

   if (not pointer_matches or not int_matches or not int64_matches or not double_matches) {
      Log.error("a supported seeded value changed before activation");
      return false;
   }
   return true;
}

static bool test_set_variable_policy_rejections(kt::Log &Log)
{
   SetVariableTestScript holder;
   if (not holder.initialise(
       "global glHostContract = 'original' global glHostConst <const> = 'fixed'", Log)) return false;
   auto script = holder.get();
   if (not activate_test_script(script, Log)) return false;

   if (ti::SetVariable(script, "glHostContract", FD_INT, 7) != ERR::FieldTypeMismatch or
       not global_string_is(script->Lua, "glHostContract", "original")) {
      Log.error("a sticky contract rejection returned the wrong result or changed the value");
      return false;
   }
   if (ti::SetVariable(script, "glHostContract", FD_STRING, (STRING)"replacement") != ERR::Okay or
       not global_string_is(script->Lua, "glHostContract", "replacement")) {
      Log.error("the sticky contract changed after a rejected store");
      return false;
   }

   if (ti::SetVariable(script, "glHostConst", FD_STRING, (STRING)"changed") != ERR::ReadOnly or
       not global_string_is(script->Lua, "glHostConst", "fixed") or
       ti::SetVariable(script, "glHostConst", FD_STRING, (STRING)"changed-again") != ERR::ReadOnly) {
      Log.error("a const rejection returned the wrong result or changed its persisted policy");
      return false;
   }

   if (ti::SetVariable(script, "tostring", FD_STRING, (STRING)"changed") != ERR::ReadOnly) {
      Log.error("a protected built-in store did not return ERR::ReadOnly");
      return false;
   }
   lua_getglobal(script->Lua, "tostring");
   bool function_preserved = lua_isfunction(script->Lua, -1);
   lua_pop(script->Lua, 1);
   if (not function_preserved) {
      Log.error("a rejected SetVariable() store changed a protected built-in");
      return false;
   }
   return true;
}

static bool test_set_variable_name_and_state(kt::Log &Log)
{
   SetVariableTestScript holder;
   if (not holder.initialise("global glNameTestReady = true", Log)) return false;
   auto script = holder.get();
   if (not activate_test_script(script, Log)) return false;

   std::string storage = "glExactTrailing";
   std::string_view exact_name(storage.data(), 7);
   if (ti::SetVariable(script, exact_name, FD_INT, 41) != ERR::Okay) {
      Log.error("the non-NUL-terminated variable name was rejected");
      return false;
   }
   lua_getglobal(script->Lua, "glExact");
   bool exact_value = lua_tointeger(script->Lua, -1) IS 41;
   lua_pop(script->Lua, 1);
   if (not exact_value or not global_is_nil(script->Lua, storage)) {
      Log.error("SetVariable() read beyond the supplied string_view");
      return false;
   }

   script->Recurse++;
   ERR recurse_error = ti::SetVariable(script, "glReentrant", FD_INT, 1);
   script->Recurse--;
   if (recurse_error != ERR::InvalidState or not global_is_nil(script->Lua, "glReentrant")) {
      Log.error("SetVariable() did not reject a store on an executing state");
      return false;
   }
   return true;
}

static bool test_set_variable_reactivation(kt::Log &Log)
{
   SetVariableTestScript holder;
   if (not holder.initialise("global glCachedContract = 'initial'", Log)) return false;
   auto script = holder.get();
   if (not activate_test_script(script, Log) or not activate_test_script(script, Log)) return false;

   if (ti::SetVariable(script, "glCachedContract", FD_INT, 2) != ERR::FieldTypeMismatch or
       not global_string_is(script->Lua, "glCachedContract", "initial")) {
      Log.error("a reactivated script did not retain its global contract and value");
      return false;
   }
   if (ti::SetVariable(script, "glCachedContract", FD_STRING, (STRING)"compatible") != ERR::Okay or
       not global_string_is(script->Lua, "glCachedContract", "compatible")) {
      Log.error("a compatible store failed after script reactivation");
      return false;
   }
   return true;
}

static bool test_set_variable_newindex_dispatch(kt::Log &Log)
{
   SetVariableTestScript holder;
   constexpr std::string_view statement = R"(
      setmetatable(_G, {
         __newindex = function(Name, Value)
            rawset(&&, 'glHostInterceptedName', Name)
            rawset(&&, 'glHostInterceptedValue', Value)
         end
      })
   )";
   if (not holder.initialise(statement, Log)) return false;
   auto script = holder.get();
   if (not activate_test_script(script, Log)) return false;

   if (ti::SetVariable(script, "glHostProxied", FD_INT, 73) != ERR::Okay) {
      Log.error("SetVariable() rejected a name handled by _G.__newindex");
      return false;
   }
   if (not global_is_nil(script->Lua, "glHostProxied") or
       not global_string_is(script->Lua, "glHostInterceptedName", "glHostProxied")) {
      Log.error("SetVariable() bypassed the _G.__newindex handler");
      return false;
   }
   lua_getglobal(script->Lua, "glHostInterceptedValue");
   bool value_matches = lua_tointeger(script->Lua, -1) IS 73;
   lua_pop(script->Lua, 1);
   if (not value_matches) {
      Log.error("the _G.__newindex handler received the wrong SetVariable() value");
      return false;
   }
   return true;
}

} // namespace

void set_variable_unit_tests(int &Passed, int &Total)
{
   struct TestCase {
      const char *name;
      bool (*function)(kt::Log &Log);
   };
   constexpr std::array<TestCase, 5> tests = { {
      { "set_variable_pre_activation", test_set_variable_pre_activation },
      { "set_variable_policy_rejections", test_set_variable_policy_rejections },
      { "set_variable_name_and_state", test_set_variable_name_and_state },
      { "set_variable_reactivation", test_set_variable_reactivation },
      { "set_variable_newindex_dispatch", test_set_variable_newindex_dispatch }
   } };

   for (const auto &test : tests) {
      kt::Log log("SetVariableTests");
      log.branch("Running %s", test.name);
      Total++;
      if (test.function(log)) {
         Passed++;
         log.msg("%s passed", test.name);
      }
      else log.error("%s failed", test.name);
   }
}

#endif // UNIT_TESTS
