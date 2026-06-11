#pragma once

// SSE2 availability detection, shared by modules that implement SIMD rendering paths.  SSE2 is guaranteed on
// all x86-64 targets and on 32-bit MSVC builds compiled with /arch:SSE2 or better.  Code must guard its SIMD
// paths with KOTUKU_SSE2 and fall back to scalar routines on other architectures.

#if defined(__SSE2__)
   #define KOTUKU_SSE2 1
#elif defined(_M_X64)
   #define KOTUKU_SSE2 1
#elif defined(_M_IX86_FP)
   #if _M_IX86_FP >= 2
      #define KOTUKU_SSE2 1
   #endif
#endif

#ifdef KOTUKU_SSE2
#include <emmintrin.h>
#endif
