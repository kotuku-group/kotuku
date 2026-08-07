/*********************************************************************************************************************

Unit tests for module call marshalling.

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

constexpr int SYNTHETIC_ARG_COUNT = 16;

struct synthetic_observation {
   std::array<std::string, SYNTHETIC_ARG_COUNT> Values;
   bool Corrupt = false;
};

static thread_local synthetic_observation glSyntheticObservation;

class ModuleMarshallingTestScript {
public:
   ~ModuleMarshallingTestScript()
   {
      if (this->script) FreeResource(this->script);
   }

   bool initialise(kt::Log &Log)
   {
      if (NewObject(CLASSID::TIRI, &this->script) != ERR::Okay) {
         Log.error("failed to create a Tiri test object");
         return false;
      }
      this->script->setStatement("");
      if (Action(AC::Init, this->script, nullptr) != ERR::Okay) {
         Log.error("failed to initialise a Tiri test object");
         return false;
      }
      return true;
   }

   extTiri * get() const { return (extTiri *)this->script; }

private:
   objTiri *script = nullptr;
};

static void synthetic_views_16(
   const std::string_view *A0, const std::string_view *A1, const std::string_view *A2, const std::string_view *A3,
   const std::string_view *A4, const std::string_view *A5, const std::string_view *A6, const std::string_view *A7,
   const std::string_view *A8, const std::string_view *A9, const std::string_view *A10, const std::string_view *A11,
   const std::string_view *A12, const std::string_view *A13, const std::string_view *A14, const std::string_view *A15)
{
   const std::array<const std::string_view *, SYNTHETIC_ARG_COUNT> views = {
      A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15
   };

   glSyntheticObservation = { };
   for (size_t index = 0; index < views.size(); ++index) {
      if (not views[index]) {
         glSyntheticObservation.Corrupt = true;
         continue;
      }
      glSyntheticObservation.Values[index].assign(views[index]->data(), views[index]->size());
   }
}

static bool run_max_string_view_call(extTiri *Script, const std::array<std::string, SYNTHETIC_ARG_COUNT> &Inputs,
   kt::Log &Log)
{
   glSyntheticObservation = { };
   auto failure = test_module_string_view_call(Script->Lua, (APTR)synthetic_views_16, Inputs);
   if (not failure.empty()) {
      Log.error("%s", failure.c_str());
      return false;
   }
   if (glSyntheticObservation.Corrupt) {
      Log.error("the synthetic call delivered a null string-view reference");
      return false;
   }
   for (int index = 0; index < SYNTHETIC_ARG_COUNT; ++index) {
      if (glSyntheticObservation.Values[index] != Inputs[index]) {
         Log.error("argument %d arrived as '%s' rather than '%s'", index,
            glSyntheticObservation.Values[index].c_str(), Inputs[index].c_str());
         return false;
      }
   }
   return true;
}

// Sixteen arguments cross the old reserve(8) reallocation boundary and exercise the documented module argument limit.

static bool test_max_string_view_signature(kt::Log &Log)
{
   ModuleMarshallingTestScript holder;
   if (not holder.initialise(Log)) return false;

   std::array<std::string, SYNTHETIC_ARG_COUNT> inputs;
   for (int index = 0; index < SYNTHETIC_ARG_COUNT; ++index) {
      inputs[index] = std::format("synthetic-argument-{:03}{}", index, std::string(size_t(index), 'x'));
   }

   for (int repeat = 0; repeat < 4; ++repeat) {
      if (not run_max_string_view_call(holder.get(), inputs, Log)) {
         Log.error("synthetic call repeat %d failed", repeat);
         return false;
      }
   }
   return true;
}

} // namespace

void module_marshalling_unit_tests(int &Passed, int &Total)
{
   kt::Log log("ModuleMarshallingTests");
   log.branch("Running maximum string-view signature test");
   Total++;
   if (test_max_string_view_signature(log)) {
      Passed++;
      log.msg("maximum string-view signature passed");
   }
   else log.error("maximum string-view signature failed");
}

#endif // UNIT_TESTS
