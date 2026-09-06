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
#include "lua.h"

#ifdef UNIT_TESTS

extern int test_struct_live_references();

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

// Complex input contracts remain on CIF.  Separate native targets verify the actual reference ABI and distinguish
// NUL-terminated strings from length-bearing views, with collection during resolution of a later argument.
static thread_local int glComplexCalls = 0;

static int synthetic_c_string(CSTRING Value, int Tail)
{
   ++glComplexCalls;
   return (Value ? int(std::strlen(Value)) : -1) + Tail;
}

static int synthetic_string_view(const std::string_view &Value, int Tail)
{
   ++glComplexCalls;
   return (Value.data() ? int(Value.size()) : -1) + Tail;
}

static bool test_complex_inputs(kt::Log &Log)
{
   ModuleMarshallingTestScript holder;
   if (not holder.initialise(Log)) return false;
   holder.get()->setStatement("local ready = true");
   if (Action(AC::Query, holder.get(), nullptr) != ERR::Okay) return false;
   for (bool cpp : { false, true }) {
      const uint32_t types[] = { uint32_t(cpp ? FD_STR|FD_CPP : FD_STR), FD_INT };
      for (bool bridge : { false, true }) {
         for (bool jit : { false, true }) {
            auto source = std::format(R"(
               jit.{}()
               local call = syntheticZero
               local value, extra = call('abc', 4)
               assert(value is 7 and extra is nil, 'String result contract')
               assert(call(nil, 0) is -1, 'Nil must pass null storage')
               assert(call() is -1, 'Missing must pass null storage')
               assert(call('', 0) is 0, 'Empty string differs from nil')
               assert(call('a\0b', 0) is {}, 'Embedded NUL contract')
               local resolutions = 0
               thunk later():num
                  resolutions++
                  local garbage = {{}}
                  for i in {{0 to 500}} do garbage[i] = tostring(i) .. 'allocation' end
                  processing.collect()
                  return 2
               end
               thunk text():str
                  return 'rooted-' .. tostring(12345)
               end
               assert(call(text(), later()) is 14, 'Earlier string must remain rooted')
               try
                  call(false, later())
               except e when ERR_InvalidType
                  assert(resolutions is 1, 'Early failure must not resolve later input')
               success
                  assert(false, 'Boolean string input must fail')
               end
               try
                  call(text(), false)
               except e when ERR_InvalidType
                  assert(e.message.find('arg #2 (Arg2)'), 'Later conversion diagnostic')
               success
                  assert(false, 'Later conversion must fail before native invocation')
               end
            )", jit ? "on" : "off", cpp ? 3 : 1);
            glComplexCalls = 0;
            auto failure = test_module_simple_call(holder.get()->Lua,
               cpp ? (APTR)synthetic_string_view : (APTR)synthetic_c_string, FD_INT, types,
               bridge, false, source.c_str());
            if ((not failure.empty()) or (glComplexCalls != 7)) {
               Log.error("Complex input cpp %d, bridge %d, JIT %d: %s (calls %d)", int(cpp), int(bridge),
                  int(jit), failure.c_str(), glComplexCalls);
               return false;
            }
         }
      }
   }
   return true;
}

// Independent expected values also preserve the legacy signed conversion of unsigned output slots.
static thread_local int glOutputCalls = 0;
static thread_local bool glOutputAligned = true;

static ERR synthetic_outputs(int Input, int *First, int64_t *Wide, double *Real, APTR *Pointer, int Tail)
{
   ++glOutputCalls;
   glOutputAligned = glOutputAligned and (uintptr_t(First) % alignof(int) IS 0) and
      (uintptr_t(Wide) % alignof(int64_t) IS 0) and (uintptr_t(Real) % alignof(double) IS 0) and
      (uintptr_t(Pointer) % alignof(APTR) IS 0);
   if ((*First != 0) or (*Wide != 0) or (*Real != 0) or (*Pointer != nullptr)) glOutputAligned = false;
   *First = Input + Tail - 2147483647;
   *Wide = (Input IS 99) ? 9007199254740993LL : -9007199254740991LL;
   *Real = -1234.125;
   return ERR::Okay;
}

static bool test_output_slots(kt::Log &Log)
{
   ModuleMarshallingTestScript holder;
   if (not holder.initialise(Log)) return false;
   holder.get()->setStatement("local ready = true");
   if (Action(AC::Query, holder.get(), nullptr) != ERR::Okay) return false;
   const std::array<uint32_t, 6> types = {
      FD_INT, FD_INT|FD_RESULT|FD_UNSIGNED, FD_INT64|FD_RESULT, FD_DOUBLE|FD_RESULT, FD_PTR|FD_RESULT, FD_INT
   };
   for (bool bridge : { false, true }) {
      for (bool jit : { false, true }) {
         glOutputCalls = 0;
         glOutputAligned = true;
         auto source = std::format(R"(
            jit.{}()
            local call = syntheticZero
            processing.collect()
            for _ in {{0 to 100}} do
               local err, first, wide, real, pointer, extra = call(4, nil, nil, nil, nil, 6, 'ignored')
               assert(err is ERR_Okay and first is -2147483637, 'Interleaved inputs preserve positions')
               assert(wide is -9007199254740991 and real is -1234.125, 'Exact numeric outputs')
               assert(pointer != nil and type(pointer) is 'userdata', 'Null output is light userdata')
               assert(extra is nil, 'Output arity')
            end
            local _, _, rounded = call(99)
            assert(rounded is 9007199254740992, 'Int64 outputs use double precision above 2^53')
            local err, first = call()
            assert(err is ERR_Okay and first is -2147483647, 'Missing defaults')
            try
               call(1, nil, nil, nil, nil, false)
            except e when ERR_InvalidType
               assert(e.message.find('arg #6'), 'Output slots must not compact later input indices')
            success
               assert(false, 'Bad later input must fail')
            end
         )", jit ? "on" : "off");
         auto failure = test_module_simple_call(holder.get()->Lua, (APTR)synthetic_outputs, FD_ERROR,
            types, bridge, false, source.c_str());
         if ((not failure.empty()) or (not glOutputAligned) or (glOutputCalls != 103)) {
            Log.error("Output slots: %s, aligned %d, calls %d", failure.c_str(), int(glOutputAligned), glOutputCalls);
            return false;
         }
      }
   }
   return true;
}

struct ownership_observation {
   int Calls = 0;
   int Allocations = 0;
   int Releases = 0;
   int FailAfter = -1;
   int PendingFailure = -1;
   int Failures = 0;
   lua_Alloc Allocator = nullptr;
   void *AllocatorData = nullptr;
};

static thread_local ownership_observation glOwnership;
static thread_local uint64_t glOutputSerial = 0;

static ERR synthetic_release(ResourceRecord &, APTR)
{
   ++glOwnership.Releases;
   return ERR::Terminate; // Let Core free its AllocResource block after recording destruction.
}

static ResourceManager glOutputResourceManager{ "Module output fixture", synthetic_release, false };

static STRING synthetic_allocate()
{
   APTR result = nullptr;
   if (AllocResource(256, MEM::NIL, &result, &glOutputResourceManager) != ERR::Okay) return nullptr;
   ++glOwnership.Allocations;
   std::memset(result, 'x', 255);
   auto serial = ++glOutputSerial;
   for (int index = 0; index < 16; ++index) ((STRING)result)[index] = char('a' + ((serial >> (index * 4)) & 15));
   return (STRING)result;
}

static void * synthetic_allocator(void *Data, void *Pointer, size_t OldSize, size_t NewSize)
{
   auto &state = *(ownership_observation *)Data;
   if ((NewSize > OldSize) and (state.PendingFailure >= 0)) {
      if (state.PendingFailure-- IS 0) {
         ++state.Failures;
         return nullptr;
      }
   }
   return state.Allocator(state.AllocatorData, Pointer, OldSize, NewSize);
}

static ERR synthetic_owned_outputs(int Mode, STRING *First, STRING *Last, std::string *Text)
{
   ++glOwnership.Calls;
   *First = synthetic_allocate();
   *Last = synthetic_allocate();
   Text->assign("full\0length", 11);
   glOwnership.PendingFailure = glOwnership.FailAfter;
   return Mode ? ERR::Args : ERR::Okay;
}

static STRING synthetic_owned_return(STRING *Output)
{
   ++glOwnership.Calls;
   *Output = synthetic_allocate();
   auto result = synthetic_allocate();
   glOwnership.PendingFailure = glOwnership.FailAfter;
   return result;
}

static ERR synthetic_mutable_boundaries(std::string *Text, int Mode, STRING *Output)
{
   ++glOwnership.Calls;
   *Output = synthetic_allocate();
   if (Mode IS 0) Text->clear();
   else if (Mode IS 1) Text->assign(Text->size(), 'q');
   else if (Mode IS 2) *Text = "abc";
   else {
      if (*Text != "abc") return ERR::InvalidData;
      Text->clear();
   }
   return ERR::Args;
}

static ERR synthetic_deferred_input(std::string *, int)
{
   ++glOwnership.Calls;
   return ERR::Okay;
}

static void synthetic_cpp_outputs(std::string *Text, std::string_view *View)
{
   ++glOwnership.Calls;
   Text->assign("full\0length", 11);
   *View = std::string_view("a\0b", 3);
}

static ERR synthetic_mutable_output(std::string *Text, STRING *Output)
{
   ++glOwnership.Calls;
   *Output = synthetic_allocate();
   Text->append("suffix");
   return ERR::Args;
}

static ERR synthetic_vector_output(kt::vector<int> *Values, int Tail)
{
   ++glOwnership.Calls;
   Values->push_back(-2147483647);
   Values->push_back(Tail);
   glOwnership.PendingFailure = glOwnership.FailAfter;
   return ERR::Okay;
}

static ERR synthetic_struct_vector(kt::vector<APTR> *Values, int)
{
   static thread_local int value = -123;
   ++glOwnership.Calls;
   Values->push_back(&value);
   glOwnership.PendingFailure = glOwnership.FailAfter;
   return ERR::Okay;
}

static bool test_owned_outputs(kt::Log &Log)
{
   ModuleMarshallingTestScript holder;
   if (not holder.initialise(Log)) return false;
   auto lua = holder.get()->Lua;
   holder.get()->setStatement("local ready = true");
   if (Action(AC::Query, holder.get(), nullptr) != ERR::Okay) return false;
   const std::array<uint32_t, 4> types = {
      FD_INT, FD_STR|FD_RESULT|FD_ALLOC, FD_STR|FD_RESULT|FD_ALLOC, FDF_CPPSTRING|FD_RESULT|FD_MUTABLE
   };
   for (bool bridge : { false, true }) {
      for (bool jit : { false, true }) {
         glOwnership = { };
         auto source = std::format(R"(
            jit.{}()
            local call = syntheticZero
            local err:num, first:str, last:str, text:str, extra = call(0)
            assert(err is ERR_Okay and #first is 255 and #last is 255 and #text is 11 and extra is nil)
            err, first, last, text = call(1)
            assert(err is ERR_Args and #first is 255 and #last is 255 and #text is 11)
            try
               checkall call(1) end
            except e when ERR_Args
               assert(e.message.find('SyntheticZero'), 'Native error diagnostic')
            success
               assert(false, 'checkall must promote native error')
            end
            try
               check call(1)
            except e when ERR_Args
            success
               assert(false, 'check must promote native error')
            end
            try
               call(false)
            except e when ERR_InvalidType
            success
               assert(false, 'Input failure must precede native execution')
            end
            function unchecked():num
               local code = call(1)
               return code
            end
            checkall assert(unchecked() is ERR_Args, 'checkall is immediate-scope only') end
         )", jit ? "on" : "off");
         auto failure = test_module_simple_call(lua, (APTR)synthetic_owned_outputs, FD_ERROR,
            types, bridge, false, source.c_str(), false);
         if ((not failure.empty()) or (glOwnership.Calls != 5) or (glOwnership.Allocations != 10) or
             (glOwnership.Releases != 10) or test_module_live_temporaries()) {
            Log.error("Owned outputs: %s, calls %d, allocations %d, releases %d, temporaries %d", failure.c_str(),
               glOwnership.Calls, glOwnership.Allocations, glOwnership.Releases, test_module_live_temporaries());
            return false;
         }
      }
   }

   // Real Lua allocator failure after native execution, including failure while pushing a later result.
   for (int allocation = 0; allocation < 2; ++allocation) {
      glOwnership = { };
      glOwnership.FailAfter = allocation;
      glOwnership.Allocator = lua_getallocf(lua, &glOwnership.AllocatorData);
      lua_setallocf(lua, synthetic_allocator, &glOwnership);
      auto failure = test_module_simple_call(lua, (APTR)synthetic_owned_outputs, FD_ERROR, types, true, false, R"(
         local caught = false
         try
            syntheticZero(0)
         except e when ERR_NoMemory
            caught = true
         end
         assert(caught, 'Injected Lua allocation failure must raise')
         processing.collect()
      )", false);
      lua_setallocf(lua, glOwnership.Allocator, glOwnership.AllocatorData);
      if ((not failure.empty()) or (glOwnership.Calls != 1) or (glOwnership.Failures != 1) or
          (glOwnership.Allocations != glOwnership.Releases) or
          test_module_live_temporaries()) {
         Log.error("Allocation failure %d: %s, allocations %d, releases %d, temporaries %d", allocation,
            failure.c_str(), glOwnership.Allocations, glOwnership.Releases, test_module_live_temporaries());
         return false;
      }
   }

   const std::array<uint32_t, 1> return_types = { FD_STR|FD_RESULT|FD_ALLOC };
   for (int allocation : { -1, 0, 1 }) {
      glOwnership = { };
      glOwnership.FailAfter = allocation;
      glOwnership.Allocator = lua_getallocf(lua, &glOwnership.AllocatorData);
      lua_setallocf(lua, synthetic_allocator, &glOwnership);
      auto failure = test_module_simple_call(lua, (APTR)synthetic_owned_return, FD_STR|FD_ALLOC,
         return_types, true, false, R"(
         try
            local first, last, extra = syntheticZero()
            assert(#first is 255 and #last is 255 and extra is nil, 'Native return precedes output parameter')
         except e when ERR_NoMemory
         end
      )", false);
      glOwnership.PendingFailure = -1;
      lua_setallocf(lua, glOwnership.Allocator, glOwnership.AllocatorData);
      if ((not failure.empty()) or (glOwnership.Calls != 1) or (glOwnership.Allocations != 2) or
          (glOwnership.Releases != 2)) {
         Log.error("Native allocated return failure %d: %s", allocation, failure.c_str());
         return false;
      }
   }

   // Resource transfer commits only after the wrapper is pushed.  Later failures must keep the transferred
   // wrapper alive while releasing the remaining native allocation, then let GC release the transferred value.
   for (int allocation : { -1, 0, 1, 2 }) {
      glOwnership = { };
      glOwnership.FailAfter = allocation;
      glOwnership.Allocator = lua_getallocf(lua, &glOwnership.AllocatorData);
      lua_setallocf(lua, synthetic_allocator, &glOwnership);
      auto resource_types = types;
      resource_types[1] = FD_PTR|FD_STRUCT|FD_RESOURCE|FD_RESULT|FD_ALLOC;
      auto failure = test_module_simple_call(lua, (APTR)synthetic_owned_outputs, FD_ERROR,
         resource_types, true, false, R"(
         try
            local err, resource, text = syntheticZero(0)
            assert(err is ERR_Okay and resource != nil and #text is 255)
         except e when ERR_NoMemory
         end
         processing.collect()
      )", false);
      glOwnership.PendingFailure = -1;
      lua_setallocf(lua, glOwnership.Allocator, glOwnership.AllocatorData);
      lua_gc(lua, LUA_GCCOLLECT, 0);
      if ((not failure.empty()) or (glOwnership.Allocations != glOwnership.Releases) or
          test_module_live_temporaries()) {
         Log.error("Resource transfer failure %d: %s, allocations %d, releases %d", allocation, failure.c_str(),
            glOwnership.Allocations, glOwnership.Releases);
         return false;
      }
   }

   const std::array<uint32_t, 2> vector_types = { FDF_VECTOR|FD_MUTABLE|FD_RESULT|FD_INT, FD_INT };
   for (int allocation : { -1, 0 }) {
      glOwnership = { };
      glOwnership.FailAfter = allocation;
      glOwnership.Allocator = lua_getallocf(lua, &glOwnership.AllocatorData);
      lua_setallocf(lua, synthetic_allocator, &glOwnership);
      auto failure = test_module_simple_call(lua, (APTR)synthetic_vector_output, FD_ERROR,
         vector_types, true, false, R"(
         try
            local err, values = syntheticZero(nil, 7)
            assert(err is ERR_Okay and #values is 2 and values[0] is -2147483647 and values[1] is 7)
         except e when ERR_NoMemory
         end
      )", false);
      glOwnership.PendingFailure = -1;
      lua_setallocf(lua, glOwnership.Allocator, glOwnership.AllocatorData);
      if ((not failure.empty()) or (glOwnership.Calls != 1) or test_module_live_temporaries()) {
         Log.error("Container failure %d: %s", allocation, failure.c_str());
         return false;
      }
   }

   if (make_struct(lua, "Arg1", "lValue") != ERR::Okay) return false;
   const std::array<uint32_t, 2> structure_vector_types = {
      FDF_VECTOR|FD_MUTABLE|FD_RESULT|FD_PTR|FD_STRUCT|FD_ALLOC, FD_INT
   };
   for (int allocation : { -1, 0, 1, 2, 3 }) {
      glOwnership = { };
      glOwnership.FailAfter = allocation;
      glOwnership.Allocator = lua_getallocf(lua, &glOwnership.AllocatorData);
      lua_setallocf(lua, synthetic_allocator, &glOwnership);
      auto failure = test_module_simple_call(lua, (APTR)synthetic_struct_vector, FD_ERROR,
         structure_vector_types, true, false, R"(
         try
            local err, values = syntheticZero(nil, 7)
            assert(err is ERR_Okay and #values is 1 and values[0].value is -123)
         except e when ERR_NoMemory
         end
      )", false);
      glOwnership.PendingFailure = -1;
      lua_setallocf(lua, glOwnership.Allocator, glOwnership.AllocatorData);
      if ((not failure.empty()) or (glOwnership.Calls != 1) or test_module_live_temporaries() or
          test_struct_live_references()) {
         Log.error("Structure container failure %d: %s", allocation, failure.c_str());
         return false;
      }
   }

   // A registered output structure creates registry references while it is copied.  Fail after reference capture,
   // while another owned result is still pending, and verify both the reference graph and native allocations.
   if (make_struct(lua, "Arg2", "lValue") != ERR::Okay) return false;
   for (int allocation : { -1, 0, 1, 2, 3 }) {
      glOwnership = { };
      glOwnership.FailAfter = allocation;
      glOwnership.Allocator = lua_getallocf(lua, &glOwnership.AllocatorData);
      lua_setallocf(lua, synthetic_allocator, &glOwnership);
      auto copied_types = types;
      copied_types[1] = FD_PTR|FD_STRUCT|FD_RESULT|FD_ALLOC;
      auto failure = test_module_simple_call(lua, (APTR)synthetic_owned_outputs, FD_ERROR,
         copied_types, true, false, R"(
         try
            local err, value, text = syntheticZero(0)
            assert(err is ERR_Okay and type(value) is 'table' and #text is 255)
         except e when ERR_NoMemory
         end
      )", false);
      glOwnership.PendingFailure = -1;
      lua_setallocf(lua, glOwnership.Allocator, glOwnership.AllocatorData);
      if ((not failure.empty()) or (glOwnership.Allocations != glOwnership.Releases) or
          test_module_live_temporaries() or test_struct_live_references()) {
         Log.error("Copied structure failure %d: %s, references %d", allocation, failure.c_str(),
            test_struct_live_references());
         return false;
      }
   }

   glOwnership = { };
   const std::array<uint32_t, 2> mutable_types = { FDF_CPPSTRING|FD_MUTABLE, FD_STR|FD_RESULT|FD_ALLOC };
   auto failure = test_module_simple_call(lua, (APTR)synthetic_mutable_output, FD_ERROR, mutable_types, true, false, R"(
      local text = string.alloc(3)
      try
         checkall syntheticZero(text) end
      except e when ERR_BufferOverflow
         assert(e.message is 'Mutable buffer too small.', 'Copy-back failure takes precedence')
      success
         assert(false, 'Oversized copy-back must fail')
      end
   )", false);
   if ((not failure.empty()) or (glOwnership.Calls != 1) or (glOwnership.Releases != 1) or
       test_module_live_temporaries()) {
      Log.error("Copy-back cleanup: %s, releases %d", failure.c_str(), glOwnership.Releases);
      return false;
   }
   glOwnership = { };
   const std::array<uint32_t, 3> boundary_types = { FDF_CPPSTRING|FD_MUTABLE, FD_INT, FD_STR|FD_RESULT|FD_ALLOC };
   failure = test_module_simple_call(lua, (APTR)synthetic_mutable_boundaries, FD_ERROR,
      boundary_types, true, false, R"(
      local empty = string.alloc(0)
      assert(syntheticZero(empty, 0) is ERR_Args and #empty is 0, 'Zero-capacity copy-back')
      local exact = string.alloc(3)
      assert(syntheticZero(exact, 2) is ERR_Args and exact.byte(0) is 97 and exact.byte(2) is 99,
         'Exact capacity on native error')
      assert(syntheticZero(exact, 3) is ERR_Args, 'Initial mutable contents are supplied to native code')
      assert(exact.byte(0) is 0 and exact.byte(2) is 0, 'Shortened output zero-fills capacity')
      local larger = string.alloc(8)
      assert(syntheticZero(larger, 2) is ERR_Args and larger.byte(0) is 97 and
         larger.byte(3) is 0 and larger.byte(7) is 0,
         'Short copy-back')
      try
         syntheticZero(empty, 2)
      except e when ERR_BufferOverflow
      success
         assert(false, 'Nonempty output cannot fit zero capacity')
      end
      try
         syntheticZero('abc', 0)
      except e when ERR_InvalidType
      success
         assert(false, 'Read-only string must be rejected before native execution')
      end
   )", false);
   if ((not failure.empty()) or (glOwnership.Calls != 5) or (glOwnership.Releases != 5) or
       test_module_live_temporaries()) {
      Log.error("Mutable capacity contracts: %s, calls %d, releases %d", failure.c_str(), glOwnership.Calls,
         glOwnership.Releases);
      return false;
   }

   glOwnership = { };
   const std::array<uint32_t, 2> cpp_types = { FDF_CPPSTRING|FD_RESULT|FD_MUTABLE, FDF_CPPSTRING|FD_RESULT };
   failure = test_module_simple_call(lua, (APTR)synthetic_cpp_outputs, FD_VOID, cpp_types, true, false, R"(
      local text, view, extra = syntheticZero()
      assert(#text is 11 and text is 'full\0length', 'C++ string output copies embedded NUL')
      assert(#view is 3 and view is 'a\0b' and extra is nil, 'C++ view output uses reference ABI and full length')
   )");
   if ((not failure.empty()) or (glOwnership.Calls != 2) or test_module_live_temporaries()) {
      Log.error("C++ output copies: %s", failure.c_str());
      return false;
   }

   glOwnership = { };
   const std::array<uint32_t, 2> deferred_types = { FDF_CPPSTRING|FD_MUTABLE, FD_INT };
   failure = test_module_simple_call(lua, (APTR)synthetic_deferred_input, FD_ERROR, deferred_types, true, false, R"(
      local text = string.alloc(8)
      thunk later():num
         processing.collect()
         raise ERR_Args, 'Deferred conversion failed'
      end
      try
         syntheticZero(text, later())
      except e when ERR_Args
         assert(e.message is 'Deferred conversion failed', 'Deferred error is preserved')
      success
         assert(false, 'Deferred input must fail')
      end
   )", false);
   if ((not failure.empty()) or glOwnership.Calls or test_module_live_temporaries()) {
      Log.error("Deferred input cleanup: %s, calls %d, temporaries %d", failure.c_str(), glOwnership.Calls,
         test_module_live_temporaries());
      return false;
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
   Total++;
   if (test_output_slots(log)) {
      Passed++;
      log.msg("aligned output contracts passed");
   }
   else log.error("aligned output contracts failed");

   Total++;
   if (test_owned_outputs(log)) {
      Passed++;
      log.msg("owned output cleanup passed");
   }
   else log.error("owned output cleanup failed");

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
   if (test_complex_inputs(log)) {
      Passed++;
      log.msg("complex input bridge contracts passed");
   }
   else log.error("complex input bridge contracts failed");

   Total++;
   if (test_simple_calls(log)) {
      Passed++;
      log.msg("simple-input differential calls passed");
   }
   else log.error("simple-input differential calls failed");
}

#endif // UNIT_TESTS
