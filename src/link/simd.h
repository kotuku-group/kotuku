#pragma once

// SIMD availability detection, shared by modules that implement SIMD rendering paths.  SSE4.2 is the mandatory
// baseline for all x86/x86-64 Kotuku builds (see the SIMD baseline block in the root CMakeLists.txt), which in
// turn guarantees SSE2.  Both are therefore always defined on x86 targets, but code should still guard its SIMD
// paths with KOTUKU_SSE2 / KOTUKU_SSE42 and fall back to scalar routines on other architectures (e.g. ARM).

#if defined(__SSE2__)
   #define KOTUKU_SSE2 1
#elif defined(_M_X64)
   #define KOTUKU_SSE2 1
#elif defined(_M_IX86_FP)
   #if _M_IX86_FP >= 2
      #define KOTUKU_SSE2 1
   #endif
#endif

// SSE4.2 is guaranteed by the build baseline.  GCC/Clang expose __SSE4_2__ when compiled with -msse4.2; MSVC has
// no equivalent predefined macro but always provides the SSE4.2 intrinsics, so on the MSVC toolchain the baseline
// is implied wherever SSE2 is present.

#if defined(__SSE4_2__)
   #define KOTUKU_SSE42 1
#elif defined(KOTUKU_SSE2) and defined(_MSC_VER) and !defined(__clang__)
   #define KOTUKU_SSE42 1
#endif

#ifdef KOTUKU_SSE2
#include <emmintrin.h>
#endif

#ifdef KOTUKU_SSE42
#include <nmmintrin.h>
#endif

// AVX2 kernels are compiled per-function wherever the toolchain allows it: MSVC permits AVX2
// intrinsics unconditionally, while GCC/Clang need a target attribute when the translation unit is
// not built with -mavx2.  KOTUKU_AVX2 marks compile-time capability only; callers must gate the
// kernels behind the run-time simd_has_avx2() check before invoking them.  Restricted to 64-bit
// targets so the kernels can rely on 64-bit scalar<->vector moves.

#if defined(KOTUKU_SSE2) and (defined(_M_X64) or defined(__x86_64__))
   #if defined(__AVX2__)
      #define KOTUKU_AVX2 1
      #define KOTUKU_TARGET_AVX2
   #elif defined(__GNUC__) or defined(__clang__)
      #define KOTUKU_AVX2 1
      #define KOTUKU_TARGET_AVX2 __attribute__((target("avx2")))
   #elif defined(_MSC_VER)
      #define KOTUKU_AVX2 1
      #define KOTUKU_TARGET_AVX2
   #endif
#endif

#ifdef KOTUKU_AVX2

#include <immintrin.h>

#if defined(_MSC_VER) and !defined(__clang__)
#include <intrin.h>

// Run-time AVX2 availability: the CPU must report AVX2 and the OS must have enabled YMM state
// saving (OSXSAVE plus XCR0 bits 1-2).  The result is computed once; thread-safe via C++ magic
// statics.

inline bool simd_has_avx2() noexcept
{
   static const bool result = [] {
      int info[4];
      __cpuid(info, 0);
      if (info[0] < 7) return false;
      __cpuid(info, 1);
      if (!(info[2] & (1 << 27))) return false; // OSXSAVE
      if (!(info[2] & (1 << 28))) return false; // AVX
      if ((_xgetbv(0) & 0x6) != 0x6) return false; // XMM and YMM state enabled by the OS
      __cpuidex(info, 7, 0);
      return (info[1] & (1 << 5)) != 0; // AVX2
   }();
   return result;
}

#else

inline bool simd_has_avx2() noexcept
{
   static const bool result = __builtin_cpu_supports("avx2");
   return result;
}

#endif

#endif // KOTUKU_AVX2
