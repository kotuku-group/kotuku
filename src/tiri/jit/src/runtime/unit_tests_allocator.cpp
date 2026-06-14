// Unit tests for the bundled LuaJIT allocator.

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lj_alloc.h"
#include "lj_prng.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

struct TestCase {
   const char* name;
   bool (*fn)(kt::Log &Log);
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

} // namespace

extern void allocator_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 2> Tests = { {
      { "allocator_returns_16_byte_aligned_blocks", test_allocator_returns_16_byte_aligned_blocks },
      { "allocator_realloc_preserves_16_byte_alignment", test_allocator_realloc_preserves_16_byte_alignment }
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
