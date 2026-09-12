// Host-side bytecode save contracts that require stack inspection and controlled Write results.
#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/strings.hpp>
#include "../defs.h"

#ifdef UNIT_TESTS
namespace {

struct WriteObservation {
   int Calls = 0;
   int FailAt = 0;
   ERR Result = ERR::Okay;
   bool Short = false;
   std::string Bytes;
};

// Compiled output is the marker, an optional identity token and a NUL separator.  Locate the separator rather than
// assuming a fixed wrapper length.  Returns zero when the input is not a well-formed wrapper.

static size_t wrapper_size(std::string_view Bytes)
{
   constexpr size_t marker_len = sizeof(LUA_COMPILED) - 1;
   if (not Bytes.starts_with(LUA_COMPILED)) return 0;
   auto separator = Bytes.find('\0', marker_len);
   if (separator IS std::string_view::npos) return 0;
   return separator + 1;
}

static ERR sink_new(OBJECTPTR Self, APTR)
{
   new (Self) Object(Self->Class, Self->UID);
   return ERR::Okay;
}

static ERR sink_init(OBJECTPTR, APTR) { return ERR::Okay; }

static ERR sink_write(OBJECTPTR Self, struct acWrite *Args)
{
   auto &state = *((WriteObservation *)Self->CreatorMeta);
   state.Calls++;
   if (state.Calls IS state.FailAt) {
      Args->Result = state.Short ? int(Args->Buffer.size()) - 1 : 0;
      return state.Result;
   }
   state.Bytes.append((const char *)Args->Buffer.data(), Args->Buffer.size());
   Args->Result = int(Args->Buffer.size());
   return ERR::Okay;
}

static bool bytecode_save_contract(kt::Log &Log)
{
   ActionArray actions[] = {
      { AC::New, sink_new }, { AC::Init, sink_init }, { AC::Write, sink_write }, { AC::NIL, nullptr }
   };
   FieldArray fields[] = { END_FIELD };
   objMetaClass::create sink_class = {
      fl::Name("BytecodeTestSink"), fl::BaseClassID(CLASSID(strihash("BytecodeTestSink"))),
      fl::Size(sizeof(Object)), fl::Actions(actions), fl::Fields(fields)
   };
   if (not sink_class.ok()) return false;
   OBJECTPTR sink = nullptr;
   if (NewObject(sink_class->ClassID, &sink) != ERR::Okay) return false;
   struct SinkGuard {
      OBJECTPTR Value;
      ~SinkGuard() { FreeResource(Value); }
   } sink_guard { sink };
   if (InitObject(sink) != ERR::Okay) return false;

   objTiri::create holder = { fl::Statement("global bytecode_runs = (bytecode_runs or 0) + 1") };
   if (not holder.ok()) return false;
   auto script = (extTiri *)*holder;
   auto lua = script->Lua;
   WriteObservation state;
   sink->CreatorMeta = &state;

   auto save_and_check = [&](extTiri *Target, ERR Expected) {
      lua_State *target_lua = Target->Lua;
      const int depth = lua_gettop(target_lua);
      const int reference = Target->MainChunkRef;
      const void *pending = depth ? lua_topointer(target_lua, -1) : nullptr;
      ERR error = acSaveToObject(Target, sink);
      if (error != Expected or lua_gettop(target_lua) != depth or Target->MainChunkRef != reference or
          (depth and lua_topointer(target_lua, -1) != pending)) {
         Log.error("Save changed stack/reference or returned %s instead of %s",
            GetErrorMsg(error), GetErrorMsg(Expected));
         return false;
      }
      return true;
   };

   for (int i = 0; i < 3; ++i) {
      state = {};
      if (not save_and_check(script, ERR::Okay)) return false;
      if (not wrapper_size(state.Bytes)) return false;
   }
   const std::string wrapped = state.Bytes;
   const std::string raw = wrapped.substr(wrapper_size(wrapped));
   lua_getglobal(lua, "bytecode_runs");
   bool dormant = lua_isnil(lua, -1);
   lua_pop(lua, 1);
   if (not dormant or acQuery(script) != ERR::Okay) return false;

   // Fail the wrapper and then the VM payload: no callback may write again after either failure.
   for (int fail_at : { 1, 2 }) {
      for (ERR result : { ERR::Okay, ERR::LimitedSuccess, ERR::ReadOnly }) {
         state = {};
         state.FailAt = fail_at;
         state.Short = true;
         state.Result = result;
         ERR expected = result IS ERR::ReadOnly ? result : ERR::Write;
         if (not save_and_check(script, expected) or state.Calls != fail_at) return false;
      }
   }
   state = {};
   if (not save_and_check(script, ERR::Okay) or acActivate(script) != ERR::Okay or script->Error != ERR::Okay) {
      return false;
   }
   if (not save_and_check(script, ERR::Okay) or acActivate(script) != ERR::Okay or script->Error != ERR::Okay) {
      return false;
   }

   lua_getglobal(lua, "bytecode_runs");
   bool ran_twice = lua_tointeger(lua, -1) IS 2;
   lua_pop(lua, 1);
   if (not ran_twice) return false;

   auto check_loaded_lifecycle = [&](const std::string &Input) {
      objTiri::create loaded_holder = { fl::Statement("assert(true)") };
      if (not loaded_holder.ok()) return false;
      auto loaded = (extTiri *)*loaded_holder;
      if (loaded->setStatement(Input) != ERR::Okay) return false;

      lua_pushinteger(loaded->Lua, 123); // A caller-owned stack value must survive every save point.
      state = {};
      if (not save_and_check(loaded, ERR::Okay) or not wrapper_size(state.Bytes)) return false;

      lua_getglobal(loaded->Lua, "bytecode_runs");
      bool not_executed = lua_isnil(loaded->Lua, -1);
      lua_pop(loaded->Lua, 1);
      if (not not_executed or acQuery(loaded) != ERR::Okay) return false;

      state = {};
      if (not save_and_check(loaded, ERR::Okay)) return false;
      lua_getglobal(loaded->Lua, "bytecode_runs");
      not_executed = lua_isnil(loaded->Lua, -1);
      lua_pop(loaded->Lua, 1);
      if (not not_executed or acActivate(loaded) != ERR::Okay or loaded->Error != ERR::Okay) return false;

      lua_getglobal(loaded->Lua, "bytecode_runs");
      bool ran_once = lua_tointeger(loaded->Lua, -1) IS 1;
      lua_pop(loaded->Lua, 1);
      state = {};
      if (not ran_once or not save_and_check(loaded, ERR::Okay)) return false;
      lua_getglobal(loaded->Lua, "bytecode_runs");
      bool still_once = lua_tointeger(loaded->Lua, -1) IS 1;
      lua_pop(loaded->Lua, 1);
      if (not still_once or (lua_tointeger(loaded->Lua, 1) != 123) or acActivate(loaded) != ERR::Okay or
          loaded->Error != ERR::Okay) return false;
      lua_getglobal(loaded->Lua, "bytecode_runs");
      bool ran_twice_after_save = lua_tointeger(loaded->Lua, -1) IS 2;
      lua_pop(loaded->Lua, 1);
      return ran_twice_after_save and (lua_tointeger(loaded->Lua, 1) IS 123);
   };

   if (not check_loaded_lifecycle(wrapped) or not check_loaded_lifecycle(raw)) return false;

   objTiri::create invalid = { fl::Statement("local =") };
   if (not invalid.ok()) return false;
   auto invalid_script = (extTiri *)*invalid;
   const int depth = lua_gettop(invalid_script->Lua);
   for (int i = 0; i < 2; ++i) {
      state = {};
      if (not save_and_check(invalid_script, ERR::InvalidData) or state.Calls or
          lua_gettop(invalid_script->Lua) != depth or invalid_script->MainChunkRef) return false;
   }
   std::vector<std::string> malformed_inputs;
   malformed_inputs.emplace_back(LUA_COMPILED, sizeof(LUA_COMPILED));
   malformed_inputs.emplace_back(LUA_COMPILED, sizeof(LUA_COMPILED) - 1);
   malformed_inputs.back() += "\x1bLJ";
   malformed_inputs.push_back(std::string("\x1bLJ\x01", 4));
   auto wrong_version = wrapped;
   wrong_version[wrapper_size(wrapped) + 3] ^= 0x7f;
   malformed_inputs.push_back(std::move(wrong_version));

   for (const auto &malformed : malformed_inputs) {
      if (invalid_script->setStatement(malformed) != ERR::Okay) return false;
      state = {};
      if (not save_and_check(invalid_script, ERR::InvalidData) or state.Calls or
          invalid_script->ErrorMessage.empty()) return false;
   }

   invalid_script->setStatement(std::string("\x1bLJ\x01", 4));
   lua_pushinteger(invalid_script->Lua, 123); // Preserve caller-owned values on failed loads too.
   for (int i = 0; i < 2; ++i) {
      if (acQuery(invalid_script) != ERR::InvalidData or lua_gettop(invalid_script->Lua) != depth + 1 or
          lua_tointeger(invalid_script->Lua, -1) != 123 or invalid_script->MainChunkRef) return false;
   }
   lua_pop(invalid_script->Lua, 1);
   if (invalid_script->setStatement("assert(6 * 7 is 42)") != ERR::Okay or
       acQuery(invalid_script) != ERR::Okay or acActivate(invalid_script) != ERR::Okay or
       invalid_script->Error != ERR::Okay) return false;
   return true;
}

} // namespace

void bytecode_file_unit_tests(int &Passed, int &Total)
{
   kt::Log log("BytecodeFileTests");
   Total++;
   if (bytecode_save_contract(log)) {
      Passed++;
      log.msg("bytecode_save_contract passed");
   }
   else log.error("bytecode_save_contract failed");
}
#endif
