// Unit tests for the bundled LuaJIT allocator.

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"

#include "lj_alloc.h"
#include "lj_str.h"
#include "lj_prng.h"
#include "lj_func.h"
#include "lj_dispatch.h"
#include "lj_ircall.h"
#include "lj_gc.h"
#include "lj_target.h"
#include "lualib.h"
#include "../../../defs.h"
#include <memory>
#include <string>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

struct TestCase {
   const char* name;
   bool (*fn)(kt::Log &Log);
};

struct LuaStateHolder {
   LuaStateHolder()
   {
      this->state = luaL_newstate(nullptr);
   }

   ~LuaStateHolder()
   {
      if (this->state) lua_close(this->state);
   }

   lua_State* get() const { return this->state; }

private:
   lua_State* state = nullptr;
};

#ifndef LUAJIT_USE_SYSMALLOC

static void free_blocks(void *AllocState, std::array<void *, 12> &Blocks)
{
   for (void *block : Blocks) {
      if (block) lj_alloc_f(AllocState, block, 0, 0);
   }
}

static bool is_16_byte_aligned(const void *Block)
{
   return ((((uintptr_t)Block) & (uintptr_t)15) IS 0);
}

#endif

static bool test_allocator_returns_16_byte_aligned_blocks(kt::Log &Log)
{
#ifdef LUAJIT_USE_SYSMALLOC
   (void)Log;
   return true;
#else
   PRNGState prng;
   lj_prng_seed_fixed(&prng);

   void *alloc_state = lj_alloc_create(&prng);
   if (not alloc_state) {
      Log.error("allocator state creation failed");
      return false;
   }

   constexpr std::array<size_t, 12> sizes = { {
      1, 8, 15, 16, 24, 31, 32, 64, 255, 4096, 128 * 1024, 256 * 1024
   } };
   std::array<void *, 12> blocks = { };
   bool ok = true;

   for (size_t i = 0; i < sizes.size(); i++) {
      blocks[i] = lj_alloc_f(alloc_state, nullptr, 0, sizes[i]);
      if (not blocks[i]) {
         Log.error("allocation of %zu bytes failed", sizes[i]);
         ok = false;
         break;
      }
      if (not is_16_byte_aligned(blocks[i])) {
         Log.error("allocation of %zu bytes returned unaligned block %p", sizes[i], blocks[i]);
         ok = false;
         break;
      }
   }

   free_blocks(alloc_state, blocks);
   lj_alloc_destroy(alloc_state);
   return ok;
#endif
}

static bool test_allocator_realloc_preserves_16_byte_alignment(kt::Log &Log)
{
#ifdef LUAJIT_USE_SYSMALLOC
   (void)Log;
   return true;
#else
   PRNGState prng;
   lj_prng_seed_fixed(&prng);

   void *alloc_state = lj_alloc_create(&prng);
   if (not alloc_state) {
      Log.error("allocator state creation failed");
      return false;
   }

   void *block = lj_alloc_f(alloc_state, nullptr, 0, 7);
   if (not block) {
      Log.error("initial allocation failed");
      lj_alloc_destroy(alloc_state);
      return false;
   }

   bool ok = is_16_byte_aligned(block);
   if (not ok) Log.error("initial allocation returned unaligned block %p", block);

   constexpr std::array<size_t, 5> sizes = { { 33, 257, 4097, 128 * 1024, 17 } };
   size_t old_size = 7;

   for (size_t new_size : sizes) {
      void *new_block = lj_alloc_f(alloc_state, block, old_size, new_size);
      if (not new_block) {
         Log.error("reallocation to %zu bytes failed", new_size);
         ok = false;
         break;
      }

      block = new_block;
      old_size = new_size;

      if (not is_16_byte_aligned(block)) {
         Log.error("reallocation to %zu bytes returned unaligned block %p", new_size, block);
         ok = false;
         break;
      }
   }

   lj_alloc_f(alloc_state, block, old_size, 0);
   lj_alloc_destroy(alloc_state);
   return ok;
#endif
}

static bool test_mutable_string_buffers_are_zero_initialised(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   constexpr std::array<MSize, 10> Lengths = { { 0, 1, 2, 3, 4, 5, 7, 8, 9, 4096 } };
   for (MSize length : Lengths) {
      GCstr *buffer = lj_str_newbuf(lua, length);
      if (buffer->len != length or not lj_str_ismutable(buffer)) {
         Log.error("mutable string allocation of %u bytes has incorrect metadata", (unsigned)length);
         return false;
      }

      const MSize storage_size = (length + 4) & ~MSize(3);
      const uint8_t *data = (const uint8_t *)strdata(buffer);
      for (MSize index = 0; index < storage_size; index++) {
         if (data[index] != 0) {
            Log.error("mutable string allocation of %u bytes contains 0x%02x at storage offset %u",
               (unsigned)length, (unsigned)data[index], (unsigned)index);
            return false;
         }
      }
   }

   return true;
}

// Injection is owned by the protected caller, never by an object in a VM-unwound frame.
struct ClosureAllocator {
   lua_Alloc original;
   void *original_data;
   lua_State *state;
   GCproto *prototype;
   int helper;
   int fail_at;
   int requests = 0;
   int failed_site = -2;
   int trace = 0;
   uint8_t flags = 0;
   bool armed = true;
   int64_t balance = 0;

   static void *allocate(void *Data, void *Block, size_t OldSize, size_t NewSize)
   {
      auto &self = *(ClosureAllocator *)Data;
      auto &probe = lj_func_allocation_probe();
      if (self.armed and NewSize > OldSize and probe.active and probe.state IS self.state and
          probe.prototype IS self.prototype and probe.helper IS self.helper) {
         if (self.requests IS 0) self.flags = self.prototype->flags;
         ++self.requests;
         if (self.requests IS self.fail_at) {
            self.failed_site = probe.site;
            self.trace = G(self.state)->vmstate;
            self.armed = false;
            return nullptr;
         }
      }
      void *result = self.original(self.original_data, Block, OldSize, NewSize);
      if (result or not NewSize) self.balance += int64_t(NewSize) - int64_t(OldSize);
      return result;
   }
};

static bool closure_integrity(lua_State *L, kt::Log &Log)
{
   auto *g = G(L);
   TValue *previous = L->top;
   unsigned count = 0;
   for (GCobj *object = gcref(L->openupval); object; object = gcref(object->gch.nextgc)) {
      auto *cell = gco_to_upval(object);
      if (++count > 10000 or cell->closed or uvval(cell) >= previous or uvval(cell) < tvref(L->stack)) {
         Log.error("invalid ordered open-cell list");
         return false;
      }
      previous = uvval(cell);
   }
   count = 0;
   for (auto *cell = uvnext(&g->uvhead); cell != &g->uvhead; cell = uvnext(cell)) {
      if (++count > 10000 or cell->closed or uvnext(uvprev(cell)) != cell or uvprev(uvnext(cell)) != cell) {
         Log.error("invalid reciprocal global cell list");
         return false;
      }
   }
   count = 0;
   for (GCobj *object = gcref(g->gc.root); object; object = gcref(object->gch.nextgc)) {
      if (++count > 1000000) return false;
      if (object->gch.gct != uint8_t(~LJ_TFUNC)) continue;
      auto *function = gco_to_function(object);
      if (not isluafunc(function)) continue;
      if (function->l.nupvalues != funcproto(function)->sizeuv) {
         Log.error("partially sized function after allocation failure");
         return false;
      }
      for (unsigned index = 0; index < function->l.nupvalues; ++index) {
         if (not gcref(function->l.uvptr[index])) return false;
      }
   }
   return true;
}

static bool test_closure_allocation_failures(kt::Log &Log)
{
   struct ScriptOwner {
      objTiri *object = nullptr;
      ~ScriptOwner() { if (object) FreeResource(object); }
   } script;
   if (NewObject(CLASSID::TIRI, &script.object) != ERR::Okay) return false;
   if (script.object->setStatement("") != ERR::Okay or Action(AC::Init, script.object, nullptr) != ERR::Okay)
      return false;

   struct Shape { const char *body; int helper; int sites; int expected; bool sibling; };
   constexpr Shape shapes[] = {
      { "return () => num: 6", 1, 1, 6, false },
      { "return () => num: inherited", 2, 1, 10, false },
      { "local a=1; local b=2; local c=3; return () => num: a+b+c", 3, 4, 6, false },
      { "local a=1; local b=2; local c=3; return () => num: inherited+a+b+c", 3, 4, 16, false },
      { "local a=1; local b=2; local c=3; glSibling=() => num: a; "
        "return () => num: inherited+a+b+c", 3, 3, 16, true }
   };
   for (bool compiled : { false, true }) {
      for (const auto &shape : shapes) {
         unsigned failed_sites = 0;
         for (int failure = 1; failure <= shape.sites; ++failure) {
            std::unique_ptr<lua_State, decltype(&lua_close)> state(
               luaL_newstate((extTiri *)script.object), lua_close);
            if (not state) return false;
            auto *lua = state.get();
            luaL_openlibs(lua);
            std::string source = R"tiri(
global glSibling = () => num: 0
local inherited = 10
global function factory():func
)tiri";
            source += shape.body;
            source += R"tiri(
end
global glSample = factory()
global function driver(Count:num):num
   local total = 0
   local index = 0
   while index < Count do
      local callback = factory()
      total += callback()
      index++
   end
   return total
end
jit.opt.start('hotloop=1', 'hotexit=1000', '-sink')
)tiri";
            if (not compiled) source += "jit.off()\n";
            source += "assert(driver(1000) is " + std::to_string(shape.expected * 1000) + ")";
            if (lua_load(lua, source.c_str(), "=closure-oom") or lua_pcall(lua, 0, 0, 0)) {
               Log.error("closure OOM setup: %s", lua_tostring(lua, -1));
               return false;
            }
            lua_getglobal(lua, "glSample");
            auto *prototype = funcproto(funcV(lua->top - 1));
            lua_pop(lua, 1);
            prototype->flags &= PROTO_CLCOUNT - 1;
            const IRCallID calls[] = { IRCALL_lj_func_newL_zero, IRCALL_lj_func_newL_inherited,
               IRCALL_lj_func_newL_local };
            bool loop_call = false;
            auto *jit = L2J(lua);
            for (TraceNo number = 1; number < jit->sizetrace; ++number) {
               auto *trace = traceref(jit, number);
               if (not trace) continue;
               bool in_loop = false;
               for (IRRef ref = REF_BASE; ref < trace->nins; ++ref) {
                  auto &ins = trace->ir[ref];
                  if (ins.o IS IR_LOOP) in_loop = true;
                  if (in_loop and ins.o IS IR_CALLA and ins.op2 IS calls[shape.helper - 1]) loop_call = true;
               }
            }
            if (compiled and not loop_call) {
               Log.error("helper %d has no retained loop allocation", shape.helper);
               return false;
            }
            lua_gc(lua, LUA_GCCOLLECT, 0);
            lua_gc(lua, LUA_GCSTOP, 0);
            lua_getglobal(lua, "driver");
            lua_pushinteger(lua, 1000);
            auto *g = G(lua);
            ClosureAllocator allocator { g->allocf, g->allocd, lua, prototype,
               compiled ? shape.helper : 4, failure };
            auto original_total = g->gc.total;
            g->allocf = ClosureAllocator::allocate;
            g->allocd = &allocator;
            int status = lua_pcall(lua, 1, 1, 0);
            allocator.armed = false;
            lj_func_allocation_probe() = {};
            bool ok = status IS LUA_ERRMEM and allocator.failed_site >= -1 and
               prototype->flags IS allocator.flags and closure_integrity(lua, Log);
            if (compiled) {
               auto *trace = allocator.trace > 0 and TraceNo(allocator.trace) < jit->sizetrace ?
                  traceref(jit, allocator.trace) : nullptr;
               bool entered_helper = false;
               if (trace) for (IRRef ref = REF_BASE; ref < trace->nins; ++ref) {
                  auto &ins = trace->ir[ref];
                  if (ins.o IS IR_CALLA and ins.op2 IS calls[shape.helper - 1]) entered_helper = true;
               }
               ok = ok and entered_helper;
            }
            if (allocator.failed_site >= -1) failed_sites |= 1u << (allocator.failed_site + 1);
            lua_settop(lua, 0);
            lua_gc(lua, LUA_GCCOLLECT, 0);
            ok = ok and closure_integrity(lua, Log);
            if (shape.sibling) {
               lua_getglobal(lua, "glSibling");
               int sibling_status = lua_pcall(lua, 0, 1, 0);
               ok = ok and sibling_status IS 0 and lua_tointeger(lua, -1) IS 1;
               lua_settop(lua, 0);
            }
            lua_getglobal(lua, "driver");
            lua_pushinteger(lua, 1000);
            int recovery = lua_pcall(lua, 1, 1, 0);
            ok = ok and recovery IS 0 and lua_tointeger(lua, -1) IS shape.expected * 1000;
            lua_settop(lua, 0);
            if (shape.sibling) {
               lua_getglobal(lua, "glSibling");
               ok = ok and lua_pcall(lua, 0, 1, 0) IS 0 and lua_tointeger(lua, -1) IS 1;
               lua_settop(lua, 0);
            }
            lua_gc(lua, LUA_GCCOLLECT, 0);
            ok = ok and int64_t(g->gc.total) IS int64_t(original_total) + allocator.balance;
            // Restore both fields before lua_close: the bundled arena destructor recognises the original allocator.
            g->allocf = allocator.original;
            g->allocd = allocator.original_data;
            Log.msg("closure OOM compiled=%d helper=%d request=%d site=%d trace=%d status=%d",
               compiled, shape.helper, failure, allocator.failed_site, allocator.trace, status);
            if (not ok) {
               Log.error("closure allocation failure/recovery invariant failed");
               return false;
            }
         }
         unsigned distinct = 0;
         for (; failed_sites; failed_sites >>= 1) distinct += failed_sites & 1;
         if (distinct != unsigned(shape.sites)) {
            Log.error("closure sweep did not fail every distinct allocation site");
            return false;
         }
      }
   }
   return true;
}

static bool test_zero_closure_sinking(kt::Log &Log)
{
   struct ScriptOwner {
      objTiri *object = nullptr;
      ~ScriptOwner() { if (object) FreeResource(object); }
   } script;
   if (NewObject(CLASSID::TIRI, &script.object) != ERR::Okay) return false;
   if (script.object->setStatement("") != ERR::Okay or Action(AC::Init, script.object, nullptr) != ERR::Okay)
      return false;
   std::unique_ptr<lua_State, decltype(&lua_close)> state(
      luaL_newstate((extTiri *)script.object), lua_close);
   if (not state) return false;
   auto *lua = state.get();
   luaL_openlibs(lua);
   constexpr auto source = R"tiri(
global glInside = -1
global function factory():func
   return function(Value:num):num
      if Value is glInside then return Value + 2 end
      return Value + 1
   end
end
global glSample = factory()
global function driver(Count:num, Stop:num):<any, any>
   local total = 0
   local index = 0
   while index < Count do
      local callback = factory()
      local alias = callback
      total += callback(index)
      if index is Stop then return callback, alias end
      index++
   end
   return total, nil
end
jit.opt.start('hotloop=1', 'hotexit=1000')
assert(driver(1000, -1) is 500500)
)tiri";
   if (lua_load(lua, source, "=zero-closure-sink") or lua_pcall(lua, 0, 0, 0)) {
      Log.error("sinking setup: %s", lua_tostring(lua, -1));
      return false;
   }
   auto *jit = L2J(lua);
   bool sunk = false;
   for (TraceNo number = 1; number < jit->sizetrace; ++number) {
      auto *trace = traceref(jit, number);
      if (not trace) continue;
      bool in_loop = false;
      for (IRRef ref = REF_BASE; ref < trace->nins; ++ref) {
         auto &ins = trace->ir[ref];
         if (ins.o IS IR_LOOP) in_loop = true;
         if (in_loop and ins.o IS IR_CALLA and ins.op2 IS IRCALL_lj_func_newL_zero and
             (ins.r IS RID_SINK or ins.r IS RID_SUNK)) sunk = true;
      }
   }
   if (not sunk) {
      Log.error("no sunk zero-upvalue allocation in the loop");
      return false;
   }
   lua_getglobal(lua, "glSample");
   auto *prototype = funcproto(funcV(lua->top - 1));
   lua_pop(lua, 1);
   auto *g = G(lua);
   ClosureAllocator allocator { g->allocf, g->allocd, lua, prototype, 1, INT_MAX };
   g->allocf = ClosureAllocator::allocate;
   g->allocd = &allocator;
   lua_getglobal(lua, "driver");
   lua_pushinteger(lua, 10000);
   lua_pushinteger(lua, -1);
   int status = lua_pcall(lua, 2, 1, 0);
   allocator.armed = false;
   lj_func_allocation_probe() = {};
   g->allocf = allocator.original;
   g->allocd = allocator.original_data;
   bool ok = status IS 0 and lua_tointeger(lua, -1) IS 50005000 and allocator.requests < 5;
   Log.msg("zero closure loop: helper allocations=%d for 10000 iterations", allocator.requests);
   lua_settop(lua, 0);
   if (not ok) return false;

   // The cold return needs the same virtual closure in two slots. Repeated exits must create distinct identities.
   constexpr auto exits = R"tiri(
local previous:any = nil
for index in {20 to 30} do
   local first, second = driver(1000, index)
   assert(first is second and first(41) is 42)
   assert(first != previous)
   previous = first
end
jit.opt.start('hotexit=1')
for index in {30 to 60} do
   local first, second = driver(1000, index)
   assert(first is second and first(41) is 42 and first != previous)
   previous = first
end
)tiri";
   if (lua_load(lua, exits, "=zero-closure-exits") or lua_pcall(lua, 0, 0, 0)) {
      Log.error("sinking exits: %s", lua_tostring(lua, -1));
      return false;
   }
   bool replayed = false;
   for (TraceNo number = 1; number < jit->sizetrace; ++number) {
      auto *trace = traceref(jit, number);
      if (not trace or not trace->root) continue;
      for (IRRef ref = REF_BASE; ref < trace->nins; ++ref) {
         auto &ins = trace->ir[ref];
         if (ins.o IS IR_CALLA and ins.op2 IS IRCALL_lj_func_newL_zero) replayed = true;
      }
   }
   if (not replayed) {
      Log.error("no compiled side trace replayed the virtual closure");
      return false;
   }
   lua_gc(lua, LUA_GCCOLLECT, 0);
   // Disable new side traces so the next guard exit must reconstruct in the interpreter.
   if (lua_load(lua, "jit.opt.start('hotexit=1000'); jit.flush(); assert(driver(1000,-1) is 500500)",
       "=zero-closure-oom-warm") or lua_pcall(lua, 0, 0, 0)) return false;
   // Repeat failure after the call and while its inlined frame is live in the snapshot.
   for (int inside : { -1, 50 }) {
      lua_pushinteger(lua, inside);
      lua_setglobal(lua, "glInside");
      lua_getglobal(lua, "driver");
      lua_pushinteger(lua, 1000);
      lua_pushinteger(lua, 50);
      allocator.requests = 0;
      allocator.fail_at = 1;
      allocator.armed = true;
      g->allocf = ClosureAllocator::allocate;
      g->allocd = &allocator;
      status = lua_pcall(lua, 2, 2, 0);
      allocator.armed = false;
      lj_func_allocation_probe() = {};
      g->allocf = allocator.original;
      g->allocd = allocator.original_data;
      if (status != LUA_ERRMEM or allocator.requests != 1) {
         Log.error("materialisation OOM not reached: status=%d allocations=%d", status, allocator.requests);
         return false;
      }
      lua_settop(lua, 0);
      lua_gc(lua, LUA_GCCOLLECT, 0);
      if (not closure_integrity(lua, Log)) return false;
      if (lua_load(lua, "local a,b=driver(1000,50); assert(a is b and a(41) is 42)", "=zero-closure-recovery") or
          lua_pcall(lua, 0, 0, 0)) {
         Log.error("materialisation recovery: %s", lua_tostring(lua, -1));
         return false;
      }
   }
   return true;
}

} // namespace

extern void allocator_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 5> Tests = { {
      { "allocator_returns_16_byte_aligned_blocks", test_allocator_returns_16_byte_aligned_blocks },
      { "allocator_realloc_preserves_16_byte_alignment", test_allocator_realloc_preserves_16_byte_alignment },
      { "mutable_string_buffers_are_zero_initialised", test_mutable_string_buffers_are_zero_initialised },
      { "closure_allocation_failures", test_closure_allocation_failures },
      { "zero_closure_sinking", test_zero_closure_sinking }
   } };

   for (const TestCase& Test : Tests) {
      kt::Log Log("AllocatorTests");
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
