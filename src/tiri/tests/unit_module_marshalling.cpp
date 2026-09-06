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
#include <type_traits>
#include <cstdlib>

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

static thread_local int glZeroCalls = 0;

static void synthetic_void() { ++glZeroCalls; }
static int synthetic_signed() { ++glZeroCalls; return INT32_MIN; }
static uint32_t synthetic_unsigned() { ++glZeroCalls; return UINT32_MAX; }
static int64_t synthetic_int64() { ++glZeroCalls; return -9007199254740991LL; }
static double synthetic_double() { ++glZeroCalls; return -1234.125; }
static ERR synthetic_okay() { ++glZeroCalls; return ERR::Okay; }
static ERR synthetic_error() { ++glZeroCalls; return ERR::Args; }

static bool test_zero_arg_calls(kt::Log &Log)
{
   if (auto failure = test_module_zero_eligibility(); not failure.empty()) {
      Log.error("%s", failure.c_str());
      return false;
   }
   struct scalar_case {
      APTR Address;
      uint32_t Type;
      CSTRING Expected;
   };
   const scalar_case cases[] = {
      { (APTR)synthetic_void, FD_VOID, "nil" },
      { (APTR)synthetic_signed, FD_INT, "-2147483648" },
      { (APTR)synthetic_unsigned, FD_INT|FD_UNSIGNED, "4294967295" },
      { (APTR)synthetic_int64, FD_INT64, "-9007199254740991" },
      { (APTR)synthetic_double, FD_DOUBLE, "-1234.125" },
      { (APTR)synthetic_okay, FD_ERROR, "ERR_Okay" },
      // A modifier outside the allowlist must still work through the bridge.
      { (APTR)synthetic_signed, FD_INT|FD_ALLOC, "-2147483648" }
   };
   for (int state = 0; state < 2; ++state) {
      ModuleMarshallingTestScript holder;
      if (not holder.initialise(Log)) return false;
      holder.get()->setStatement("local ready = true");
      if (Action(AC::Query, holder.get(), nullptr) != ERR::Okay) {
         Log.error("failed to prepare the synthetic script libraries");
         return false;
      }
      for (bool force_bridge : { false, true }) {
         for (bool jit_enabled : { false, true }) {
            for (const auto &entry : cases) {
               auto source = std::format(R"(
                  jit.{}()
                  local first, second = syntheticZero()
                  assert(first is {}, 'Direct result differs')
                  assert(second is nil, 'Unexpected second result')
                  local extracted = syntheticZero
                  processing.collect()
                  for _ in {{0 to 200}} do
                     local value, extra = extracted(nil, 'ignored')
                     assert(value is {}, 'Extracted result differs')
                     assert(extra is nil, 'Unexpected extracted result')
                  end
                  try
                     syntheticZero(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17)
                  except e when ERR_Args
                  success
                     assert(false, 'Surplus arguments must fail before invocation')
                  end
               )", jit_enabled ? "on" : "off", entry.Expected, entry.Expected);
               glZeroCalls = 0;
               auto failure = test_module_zero_call(holder.get()->Lua, entry.Address, entry.Type,
                  force_bridge, source.c_str());
               if ((not failure.empty()) or (glZeroCalls != 202)) {
                  Log.error("Scalar type %x, bridge %d, JIT %d: %s (calls %d)", entry.Type,
                     int(force_bridge), int(jit_enabled), failure.c_str(), glZeroCalls);
                  return false;
               }
            }
            glZeroCalls = 0;
            auto failure = test_module_zero_call(holder.get()->Lua, (APTR)synthetic_error, FD_ERROR,
               force_bridge, R"(
                  assert(syntheticZero() is ERR_Args, 'Plain call returns error')
                  try
                     checkall
                        syntheticZero()
                     end
                  except e when ERR_Args
                     assert(e.message.find('SyntheticZero()'), 'Error retains native name')
                  success
                     assert(false, 'Direct error must be promoted')
                  end
                  function unchecked():num
                     return syntheticZero()
                  end
                  checkall
                     assert(unchecked() is ERR_Args, 'Checking must not enter a called Tiri function')
                  end
                  local extracted = syntheticZero
                  try
                     checkall
                        extracted()
                     end
                  except e when ERR_Args
                  success
                     assert(false, 'Extracted error must be promoted')
                  end
               )");
            if ((not failure.empty()) or (glZeroCalls != 5)) {
               Log.error("Error result, bridge %d: %s (calls %d)", int(force_bridge),
                  failure.c_str(), glZeroCalls);
               return false;
            }
         }
      }
   }
   return true;
}

static thread_local int glSimpleCalls = 0;

template<class... Args> static double synthetic_numbers(Args... Values)
{
   ++glSimpleCalls;
   const double values[] = { double(Values)... };
   double result = 0;
   int weight = 0;
   for (double value : values) result += value * ++weight;
   return result;
}

template<class T> static constexpr uint32_t numeric_descriptor()
{
   if constexpr (std::is_same_v<T, int>) return FD_INT;
   else if constexpr (std::is_same_v<T, int64_t>) return FD_INT64;
   else return FD_DOUBLE;
}

template<class... Args> static bool test_numeric_signature(extTiri *Script, kt::Log &Log)
{
   const std::array<uint32_t, sizeof...(Args)> types = { numeric_descriptor<Args>()... };
   std::string arguments, nils, tail;
   double expected = 0;
   double first = 0;
   for (size_t index = 0; index < types.size(); ++index) {
      double value = types[index] IS FD_INT ? -100 - double(index) :
         types[index] IS FD_INT64 ? -5000000000.0 - double(index) : 2.75 + double(index);
      expected += value * double(index + 1);
      if (not index) first = value;
      else { arguments += ", "; nils += ", "; tail += ", " + std::format("{}", value); }
      arguments += std::format("{}", value);
      nils += "nil";
   }
   for (bool bridge : { false, true }) {
      for (bool jit : { false, true }) {
         auto source = std::format(R"(
            jit.{}()
            assert(syntheticZero({}) is {}, 'Direct numeric result differs')
            local call = syntheticZero
            processing.collect()
            for _ in {{0 to 64}} do assert(call({}) is {}, 'Extracted numeric result differs') end
            assert(call({}) is 0, 'Nil defaults differ')
            assert(call() is 0, 'Missing defaults differ')
            assert(call({}) is {}, 'Partial defaults differ')
            local resolutions = 0
            thunk deferred():num
               resolutions++
               processing.collect()
               return {}
            end
            assert(call(deferred(){}) is {}, 'Deferred input differs')
            assert(resolutions is 1, 'Deferred input must resolve once')
            try
               call('bad'{})
            except e when ERR_InvalidType
               assert(e.message.find("arg #1 (Arg1) expected number"), 'Numeric diagnostic differs')
            success
               assert(false, 'A string must not be accepted as a number')
            end
            try
               call(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17)
            except e when ERR_Args
            success
               assert(false, 'Too many arguments must fail')
            end
         )", jit ? "on" : "off", arguments, expected, arguments, expected, nils, first, first,
            first, tail, expected, tail);
         glSimpleCalls = 0;
         auto failure = test_module_simple_call(Script->Lua, (APTR)synthetic_numbers<Args...>, FD_DOUBLE, types,
            bridge, types.size() <= 4, source.c_str());
         if ((not failure.empty()) or (glSimpleCalls != 70)) {
            Log.error("Numeric inputs [%s], bridge %d, JIT %d: %s (calls %d)", arguments.c_str(),
               int(bridge), int(jit), failure.c_str(), glSimpleCalls);
            return false;
         }
      }
   }
   return true;
}

template<class... Args> static bool test_numeric_permutations(extTiri *Script, kt::Log &Log)
{
   if constexpr (sizeof...(Args) > 0) {
      if (not test_numeric_signature<Args...>(Script, Log)) return false;
   }
   if constexpr (sizeof...(Args) < 4) {
      return test_numeric_permutations<Args..., int>(Script, Log) and
         test_numeric_permutations<Args..., int64_t>(Script, Log) and
         test_numeric_permutations<Args..., double>(Script, Log);
   }
   return true;
}

template<class R> static R synthetic_simple_result(int, double, int64_t, double)
{
   ++glSimpleCalls;
   if constexpr (std::is_same_v<R, ERR>) return ERR::Args;
   else if constexpr (std::is_same_v<R, uint32_t>) return UINT32_MAX;
   else if constexpr (not std::is_void_v<R>) return R(-123);
}

template<class R> static bool test_simple_result(extTiri *Script, kt::Log &Log, uint32_t Type, CSTRING Expected)
{
   const uint32_t types[] = { FD_INT, FD_DOUBLE, FD_INT64, FD_DOUBLE };
   for (bool bridge : { false, true }) {
      auto source = std::format(R"(
         local value, extra = syntheticZero(1, 2.5, 5000000000, 4.5)
         assert(value is {}, 'Mixed signature return differs')
         assert(extra is nil, 'Unexpected extra return')
      )", Expected);
      if constexpr (std::is_same_v<R, ERR>) source += R"(
         function unchecked():num
            return syntheticZero(1, 2.5, 5000000000, 4.5)
         end
         checkall
            assert(unchecked() is ERR_Args, 'Checking must not propagate into a called Tiri function')
         end
         try
            checkall
               syntheticZero(1, 2.5, 5000000000, 4.5)
            end
         except e when ERR_Args
            assert(e.message.find('SyntheticZero() failed:'), 'Error message differs')
         success
            assert(false, 'Native error must be promoted')
         end
      )";
      glSimpleCalls = 0;
      auto failure = test_module_simple_call(Script->Lua, (APTR)synthetic_simple_result<R>, Type, types,
         bridge, true, source.c_str());
      if ((not failure.empty()) or (glSimpleCalls != (std::is_same_v<R, ERR> ? 4 : 2))) {
         Log.error("Simple return type %x: %s (calls %d)", Type, failure.c_str(), glSimpleCalls);
         return false;
      }
   }
   return true;
}

static bool test_simple_conversion_errors(extTiri *Script, kt::Log &Log)
{
   for (uint32_t type : { uint32_t(FD_INT), uint32_t(FD_INT64), uint32_t(FD_DOUBLE) }) {
      const uint32_t types[] = { type, type };
      APTR address = type IS FD_INT ? (APTR)synthetic_numbers<int, int> :
         type IS FD_INT64 ? (APTR)synthetic_numbers<int64_t, int64_t> : (APTR)synthetic_numbers<double, double>;
      for (bool bridge : { false, true }) {
         glSimpleCalls = 0;
         auto failure = test_module_simple_call(Script->Lua, address, FD_DOUBLE, types, bridge, true, R"(
            local resolutions = 0
            thunk deferred():num
               resolutions++
               return 7
            end
            try
               syntheticZero(false, deferred())
            except e when ERR_InvalidType
               assert(resolutions is 0, 'Later argument must remain unresolved after failure')
            success
               assert(false, 'Boolean must be rejected')
            end
            try
               syntheticZero(deferred(), {})
            except e when ERR_InvalidType
               assert(resolutions is 1, 'Earlier deferred argument must resolve once')
               assert(e.message.find('arg #2 (Arg2)'), 'Error argument position differs')
            success
               assert(false, 'Table must be rejected')
            end
         )");
         // Only the helper's successful arity probe is allowed to execute native code.
         if ((not failure.empty()) or (glSimpleCalls != 1)) {
            Log.error("Conversion order: %s (calls %d)", failure.c_str(), glSimpleCalls);
            return false;
         }
      }
   }
   return true;
}

template<class T> static T synthetic_identity(T Value)
{
   ++glSimpleCalls;
   return Value;
}

static bool test_simple_boundaries(extTiri *Script, kt::Log &Log)
{
   struct boundary_case { uint32_t Type; APTR Address; CSTRING Source; };
   const boundary_case cases[] = {
      { FD_INT, (APTR)synthetic_identity<int>, R"(
         assert(syntheticZero(-2147483648) is -2147483648)
         assert(syntheticZero(2147483647) is 2147483647)
         assert(syntheticZero(2147483648) is -2147483648)
         assert(syntheticZero(4294967295) is -1)
         assert(syntheticZero(-1.75) is -1)
      )" },
      { FD_INT64, (APTR)synthetic_identity<int64_t>, R"(
         assert(syntheticZero(9007199254740991) is 9007199254740991)
         assert(syntheticZero(-9007199254740991) is -9007199254740991)
         assert(syntheticZero(-9223372036854775808) is -9223372036854775808)
         assert(syntheticZero(-1.75) is -1)
      )" },
      { FD_DOUBLE, (APTR)synthetic_identity<double>, R"(
         assert(syntheticZero(math.huge) is math.huge)
         assert(syntheticZero(-math.huge) is -math.huge)
         assert(1 / syntheticZero(-0.0) is -math.huge)
         local nan = syntheticZero(0 / 0)
         assert(nan != nan)
         assert(syntheticZero(-1.75) is -1.75)
      )" },
      { FD_INT|FD_UNSIGNED, (APTR)synthetic_identity<uint32_t>, R"(
         assert(syntheticZero(-1) is 4294967295)
         assert(syntheticZero(4294967295) is 4294967295)
      )" }
   };
   for (const auto &entry : cases) {
      for (bool bridge : { false, true }) {
         for (bool jit : { false, true }) {
            auto source = std::format("jit.{}()\n{}", jit ? "on" : "off", entry.Source);
            auto failure = test_module_simple_call(Script->Lua, entry.Address, entry.Type, { &entry.Type, 1 },
               bridge, entry.Type != (FD_INT|FD_UNSIGNED), source.c_str());
            if (not failure.empty()) {
               Log.error("Boundary type %x, bridge %d: %s", entry.Type, int(bridge), failure.c_str());
               return false;
            }
         }
      }
   }
   return true;
}

// Explicitly requested microbenchmarks use separate native targets with no observation counters.  Samples are
// printed after each timed loop and never affect test acceptance.

template<class... Args> static double benchmark_numbers(Args... Values)
{
   return (double(Values) + ...);
}

template<class... Args> static bool benchmark_simple_signature(extTiri *Script, kt::Log &Log, CSTRING Arguments)
{
   const std::array<uint32_t, sizeof...(Args)> types = { numeric_descriptor<Args>()... };
   std::string name;
   for (uint32_t type : types) name += type IS FD_INT ? "i" : type IS FD_INT64 ? "l" : "d";
   for (bool jit : { false, true }) {
      for (bool bridge : { false, true }) {
         auto source = std::format(R"(
            jit.{}()
            local call = syntheticZero
            local samples = array<double>
            local sink = 0
            for _ in {{0 to 9}} do
               local start = mSys.PreciseTime()
               for _ in {{0 to 100000}} do sink += call({}) end
               samples.push((mSys.PreciseTime() - start) / 100000)
            end
            samples.sort()
            print('Simple {} JIT {} bridge {}: ' .. samples[4] .. ' [' .. samples[0] .. ', ' .. samples[8] .. ']')
            assert(sink != 0)
         )", jit ? "on" : "off", Arguments, name, jit ? "on" : "off", bridge ? "on" : "off");
         auto failure = test_module_simple_call(Script->Lua, (APTR)benchmark_numbers<Args...>, FD_DOUBLE, types,
            bridge, true, source.c_str());
         if (not failure.empty()) { Log.error("%s", failure.c_str()); return false; }
      }
   }
   return true;
}

static bool test_simple_calls(kt::Log &Log)
{
   ModuleMarshallingTestScript holder;
   if (not holder.initialise(Log)) return false;
   holder.get()->setStatement("local ready = true");
   if (Action(AC::Query, holder.get(), nullptr) != ERR::Okay) return false;
   auto script = holder.get();
   if (not test_numeric_permutations<>(script, Log)) return false;
   if (not test_numeric_signature<int, double, int64_t, double, int>(script, Log)) return false;
   if (not test_simple_conversion_errors(script, Log)) return false;
   if (not test_simple_boundaries(script, Log)) return false;
   if (std::getenv("TIRI_MODULE_BENCH_SIMPLE")) {
      if (not (benchmark_simple_signature<int>(script, Log, "-123") and
          benchmark_simple_signature<int64_t>(script, Log, "-5000000000") and
          benchmark_simple_signature<double>(script, Log, "2.75") and
          benchmark_simple_signature<int, double>(script, Log, "-123, 2.75") and
          benchmark_simple_signature<double, int64_t, int>(script, Log, "2.75, -5000000000, -123") and
          benchmark_simple_signature<int, double, int64_t, double>(script, Log, "-123, 2.75, -5000000000, 3.25"))) {
         return false;
      }
   }
   return test_simple_result<void>(script, Log, FD_VOID, "nil") and
      test_simple_result<int>(script, Log, FD_INT, "-123") and
      test_simple_result<uint32_t>(script, Log, FD_INT|FD_UNSIGNED, "4294967295") and
      test_simple_result<int64_t>(script, Log, FD_INT64, "-123") and
      test_simple_result<double>(script, Log, FD_DOUBLE, "-123") and
      test_simple_result<ERR>(script, Log, FD_ERROR, "ERR_Args");
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

   Total++;
   if (test_zero_arg_calls(log)) {
      Passed++;
      log.msg("zero-argument differential calls passed");
   }
   else log.error("zero-argument differential calls failed");

   Total++;
   if (test_simple_calls(log)) {
      Passed++;
      log.msg("simple-input differential calls passed");
   }
   else log.error("simple-input differential calls failed");
}

#endif // UNIT_TESTS
