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

   auto save_and_check = [&](ERR Expected) {
      const int depth = lua_gettop(lua);
      const int reference = script->MainChunkRef;
      const void *pending = depth ? lua_topointer(lua, -1) : nullptr;
      ERR error = acSaveToObject(script, sink);
      if (error != Expected or lua_gettop(lua) != depth or script->MainChunkRef != reference or
          (depth and lua_topointer(lua, -1) != pending)) {
         Log.error("Save changed stack/reference or returned %s instead of %s",
            GetErrorMsg(error), GetErrorMsg(Expected));
         return false;
      }
      return true;
   };

   for (int i = 0; i < 3; ++i) {
      state = {};
      if (not save_and_check(ERR::Okay)) return false;
      if (not state.Bytes.starts_with(std::string_view(LUA_COMPILED, sizeof(LUA_COMPILED)))) return false;
   }
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
         if (not save_and_check(expected) or state.Calls != fail_at) return false;
      }
   }
   state = {};
   if (not save_and_check(ERR::Okay) or acActivate(script) != ERR::Okay or script->Error != ERR::Okay) return false;
   if (not save_and_check(ERR::Okay) or acActivate(script) != ERR::Okay or script->Error != ERR::Okay) return false;

   lua_getglobal(lua, "bytecode_runs");
   bool ran_twice = lua_tointeger(lua, -1) IS 2;
   lua_pop(lua, 1);
   if (not ran_twice) return false;

   objTiri::create invalid = { fl::Statement("local =") };
   if (not invalid.ok()) return false;
   auto invalid_script = (extTiri *)*invalid;
   const int depth = lua_gettop(invalid_script->Lua);
   for (int i = 0; i < 2; ++i) {
      if (acSaveToObject(invalid_script, sink) != ERR::InvalidData or
          lua_gettop(invalid_script->Lua) != depth or invalid_script->MainChunkRef) return false;
   }
   std::string malformed(LUA_COMPILED, sizeof(LUA_COMPILED));
   malformed += "\x1bLJ\x01";
   invalid_script->setStatement(malformed);
   lua_pushinteger(invalid_script->Lua, 123); // Preserve caller-owned values on failed loads too.
   for (int i = 0; i < 2; ++i) {
      if (acQuery(invalid_script) != ERR::InvalidData or lua_gettop(invalid_script->Lua) != depth + 1 or
          lua_tointeger(invalid_script->Lua, -1) != 123 or invalid_script->MainChunkRef) return false;
   }
   lua_pop(invalid_script->Lua, 1);
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
