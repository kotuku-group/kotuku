// Bulk TValue operations.
// Copyright (C) 2026 Paul Manias

#define lj_bulk_c
#define LUA_CORE

#include "lj_bulk.h"

#include <cstring>

#if LJ_TARGET_X64
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

using BulkNilFunc = void (*)(TValue *, MSize);
using BulkCopyFunc = void (*)(TValue *, const TValue *, MSize);

extern "C" void lj_bulk_avx2_nil_tvalue(TValue *, MSize);
extern "C" void lj_bulk_avx2_copy_tvalue(TValue *, const TValue *, MSize);
extern "C" void lj_bulk_avx2_move_tvalue(TValue *, const TValue *, MSize);

static constexpr MSize LJ_BULK_AVX2_THRESHOLD = 64;

static void bulk_scalar_nil_tvalue(TValue *Dst, MSize Count)
{
   for (MSize i = 0; i < Count; i++) setnilV(&Dst[i]);
}

static void bulk_scalar_copy_tvalue(TValue *Dst, const TValue *Src, MSize Count)
{
   if (Count > 0) std::memcpy(Dst, Src, Count * sizeof(TValue));
}

static void bulk_scalar_move_tvalue(TValue *Dst, const TValue *Src, MSize Count)
{
   if (Count > 0) std::memmove(Dst, Src, Count * sizeof(TValue));
}

#if LJ_TARGET_X64

static void bulk_cpuid(uint32_t Leaf, uint32_t Subleaf, uint32_t Regs[4])
{
#if defined(_MSC_VER)
   int cpu_info[4];
   __cpuidex(cpu_info, int(Leaf), int(Subleaf));
   Regs[0] = uint32_t(cpu_info[0]);
   Regs[1] = uint32_t(cpu_info[1]);
   Regs[2] = uint32_t(cpu_info[2]);
   Regs[3] = uint32_t(cpu_info[3]);
#elif defined(__GNUC__)
   uint32_t eax = Leaf;
   uint32_t ebx = 0;
   uint32_t ecx = Subleaf;
   uint32_t edx = 0;
   __asm__ volatile("cpuid"
      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
      :
      :);
   Regs[0] = eax;
   Regs[1] = ebx;
   Regs[2] = ecx;
   Regs[3] = edx;
#else
   Regs[0] = Regs[1] = Regs[2] = Regs[3] = 0;
#endif
}

static uint64_t bulk_xgetbv(uint32_t Index)
{
#if defined(_MSC_VER)
   return _xgetbv(Index);
#elif defined(__GNUC__)
   uint32_t eax = 0;
   uint32_t edx = 0;
   __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(Index));
   return (uint64_t(edx) << 32) | eax;
#else
   return 0;
#endif
}

static bool bulk_cpu_has_avx2()
{
   uint32_t regs[4];
   bulk_cpuid(0, 0, regs);
   if (regs[0] < 7) return false;

   bulk_cpuid(1, 0, regs);
   bool has_osxsave = not ((regs[2] & (1u << 27)) IS 0);
   bool has_avx = not ((regs[2] & (1u << 28)) IS 0);
   if (not has_osxsave or not has_avx) return false;

   uint64_t xcr0 = bulk_xgetbv(0);
   if (not ((xcr0 & 0x6) IS 0x6)) return false;

   bulk_cpuid(7, 0, regs);
   return not ((regs[1] & (1u << 5)) IS 0);
}

#else

static bool bulk_cpu_has_avx2()
{
   return false;
}

#endif

static BulkNilFunc bulk_nil_impl()
{
   static BulkNilFunc impl = bulk_cpu_has_avx2() ? lj_bulk_avx2_nil_tvalue : bulk_scalar_nil_tvalue;
   return impl;
}

static BulkCopyFunc bulk_copy_impl()
{
   static BulkCopyFunc impl = bulk_cpu_has_avx2() ? lj_bulk_avx2_copy_tvalue : bulk_scalar_copy_tvalue;
   return impl;
}

static BulkCopyFunc bulk_move_impl()
{
   static BulkCopyFunc impl = bulk_cpu_has_avx2() ? lj_bulk_avx2_move_tvalue : bulk_scalar_move_tvalue;
   return impl;
}

void lj_bulk_nil_tvalue(TValue *Dst, MSize Count)
{
   if (Count >= LJ_BULK_AVX2_THRESHOLD) bulk_nil_impl()(Dst, Count);
   else bulk_scalar_nil_tvalue(Dst, Count);
}

void lj_bulk_copy_tvalue(TValue *Dst, const TValue *Src, MSize Count)
{
   if (Count >= LJ_BULK_AVX2_THRESHOLD) bulk_copy_impl()(Dst, Src, Count);
   else bulk_scalar_copy_tvalue(Dst, Src, Count);
}

void lj_bulk_move_tvalue(TValue *Dst, const TValue *Src, MSize Count)
{
   if (Count >= LJ_BULK_AVX2_THRESHOLD) bulk_move_impl()(Dst, Src, Count);
   else bulk_scalar_move_tvalue(Dst, Src, Count);
}
