// Target architecture selection.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lua.h"

// Target endianess.

#define LUAJIT_LE   0
#define LUAJIT_BE   1

// Target architectures.

#define LUAJIT_ARCH_X86      1
#define LUAJIT_ARCH_x86      1
#define LUAJIT_ARCH_X64      2
#define LUAJIT_ARCH_x64      2
#define LUAJIT_ARCH_ARM      3
#define LUAJIT_ARCH_arm      3
#define LUAJIT_ARCH_ARM64    4
#define LUAJIT_ARCH_arm64    4
#define LUAJIT_ARCH_PPC      5
#define LUAJIT_ARCH_ppc      5

// Target OS.

#define LUAJIT_OS_OTHER      0
#define LUAJIT_OS_WINDOWS    1
#define LUAJIT_OS_LINUX      2
#define LUAJIT_OS_OSX        3
#define LUAJIT_OS_BSD        4
#define LUAJIT_OS_POSIX      5

// Number mode.

#define LJ_NUMMODE_SINGLE        0   //  Single-number mode only.
#define LJ_NUMMODE_SINGLE_DUAL   1   //  Default to single-number mode.
#define LJ_NUMMODE_DUAL          2   //  Dual-number mode only.
#define LJ_NUMMODE_DUAL_SINGLE   3   //  Default to dual-number mode.

//********************************************************************************************************************
// Select native target if no target defined.

#ifndef LUAJIT_TARGET
 #if defined(__x86_64__) || defined(__x86_64) || defined(_M_X64) || defined(_M_AMD64)
  #define LUAJIT_TARGET   LUAJIT_ARCH_X64
 #elif defined(__aarch64__)
  #define LUAJIT_TARGET   LUAJIT_ARCH_ARM64
 #elif defined(__ppc__) || defined(__ppc) || defined(__PPC__) || defined(__PPC) || defined(__powerpc__) || defined(__powerpc) || defined(__POWERPC__) || defined(__POWERPC) || defined(_M_PPC)
  #if _LP64
   #define LUAJIT_TARGET   LUAJIT_ARCH_PPC
  #else
   #error "32-bit architectures are not supported."
  #endif
 #else
  #error "Only 64-bit architectures are supported (x64, ARM64, PPC64)"
 #endif
#endif

//********************************************************************************************************************
// Select native OS if no target OS defined.
#ifndef LUAJIT_OS

#if defined(_WIN32) && !defined(_XBOX_VER)
#define LUAJIT_OS   LUAJIT_OS_WINDOWS
#elif defined(__linux__)
#define LUAJIT_OS   LUAJIT_OS_LINUX
#elif defined(__MACH__) && defined(__APPLE__)
#include "TargetConditionals.h"
#define LUAJIT_OS   LUAJIT_OS_OSX
#elif (defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || \
       defined(__NetBSD__) || defined(__OpenBSD__) || \
       defined(__DragonFly__)) && !defined(__ORBIS__)
#define LUAJIT_OS   LUAJIT_OS_BSD
#elif (defined(__sun__) && defined(__svr4__))
#define LJ_TARGET_SOLARIS   1
#define LUAJIT_OS   LUAJIT_OS_POSIX
#elif defined(__HAIKU__)
#define LUAJIT_OS   LUAJIT_OS_POSIX
#elif defined(__CYGWIN__)
#define LJ_TARGET_CYGWIN   1
#define LUAJIT_OS   LUAJIT_OS_POSIX
#else
#define LUAJIT_OS   LUAJIT_OS_OTHER
#endif

#endif

// Set target OS properties.
#if LUAJIT_OS == LUAJIT_OS_WINDOWS
#define LJ_OS_NAME   "Windows"
#elif LUAJIT_OS == LUAJIT_OS_LINUX
#define LJ_OS_NAME   "Linux"
#elif LUAJIT_OS == LUAJIT_OS_OSX
#define LJ_OS_NAME   "OSX"
#elif LUAJIT_OS == LUAJIT_OS_BSD
#define LJ_OS_NAME   "BSD"
#elif LUAJIT_OS == LUAJIT_OS_POSIX
#define LJ_OS_NAME   "POSIX"
#else
#define LJ_OS_NAME   "Other"
#endif

#define LJ_TARGET_WINDOWS  (LUAJIT_OS == LUAJIT_OS_WINDOWS)
#define LJ_TARGET_LINUX    (LUAJIT_OS == LUAJIT_OS_LINUX)
#define LJ_TARGET_OSX      (LUAJIT_OS == LUAJIT_OS_OSX)
#define LJ_TARGET_BSD      (LUAJIT_OS == LUAJIT_OS_BSD)
#define LJ_TARGET_POSIX    (LUAJIT_OS > LUAJIT_OS_WINDOWS)
#define LJ_TARGET_DLOPEN   LJ_TARGET_POSIX

#if TARGET_OS_IPHONE
#define LJ_TARGET_IOS      1
#else
#define LJ_TARGET_IOS      0
#endif

#ifdef _UWP
#define LJ_TARGET_UWP      1
#if LUAJIT_TARGET == LUAJIT_ARCH_X64
#define LJ_TARGET_GC64      1
#endif
#endif

//********************************************************************************************************************
// Arch-specific settings

#if LUAJIT_TARGET == LUAJIT_ARCH_X64

#define LJ_ARCH_NAME         "x64"
#define LJ_ARCH_BITS         64
#define LJ_ARCH_ENDIAN       LUAJIT_LE
#define LJ_TARGET_X64        1
#define LJ_TARGET_X86ORX64   1
#define LJ_TARGET_EHRETREG   0
#define LJ_TARGET_EHRAREG    16
#define LJ_TARGET_JUMPRANGE  31   //  +-2^31 = +-2GB
#define LJ_TARGET_MASKSHIFT  1
#define LJ_TARGET_MASKROT    1
#define LJ_TARGET_UNALIGNED  1
#define LJ_ARCH_NUMMODE      LJ_NUMMODE_SINGLE_DUAL // x64 SSE/AVX conversion is fast enough for this default
#define LJ_TARGET_GC64       1

#elif LUAJIT_TARGET == LUAJIT_ARCH_ARM

#error "32-bit ARM is not supported.."

#elif LUAJIT_TARGET == LUAJIT_ARCH_ARM64

#define LJ_ARCH_BITS      64
#if defined(__AARCH64EB__)
#define LJ_ARCH_NAME      "arm64be"
#define LJ_ARCH_ENDIAN      LUAJIT_BE
#else
#define LJ_ARCH_NAME      "arm64"
#define LJ_ARCH_ENDIAN      LUAJIT_LE
#endif
#define LJ_TARGET_ARM64      1
#define LJ_TARGET_EHRETREG   0
#define LJ_TARGET_EHRAREG   30
#define LJ_TARGET_JUMPRANGE   27   //  +-2^27 = +-128MB
#define LJ_TARGET_MASKSHIFT   1
#define LJ_TARGET_MASKROT   1
#define LJ_TARGET_UNIFYROT   2   //  Want only IR_BROR.
#define LJ_TARGET_GC64      1
#define LJ_ARCH_NUMMODE      LJ_NUMMODE_DUAL

#define LJ_ARCH_VERSION      80

#elif LUAJIT_TARGET == LUAJIT_ARCH_PPC

#ifndef LJ_ARCH_ENDIAN
#if __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#define LJ_ARCH_ENDIAN      LUAJIT_LE
#else
#define LJ_ARCH_ENDIAN      LUAJIT_BE
#endif
#endif

#define LJ_ARCH_BITS      64
#if LJ_ARCH_ENDIAN == LUAJIT_LE
#define LJ_ARCH_NAME      "ppc64le"
#else
#define LJ_ARCH_NAME      "ppc64"
#endif

#define LJ_ARCH_NUMMODE      LJ_NUMMODE_DUAL_SINGLE

#define LJ_TARGET_PPC      1
#define LJ_TARGET_EHRETREG   3
#define LJ_TARGET_EHRAREG   65
#define LJ_TARGET_JUMPRANGE   25   //  +-2^25 = +-32MB
#define LJ_TARGET_MASKSHIFT   0
#define LJ_TARGET_MASKROT   1
#define LJ_TARGET_UNIFYROT   1   //  Want only IR_BROL.

#if _ARCH_PWR7
#define LJ_ARCH_VERSION      70
#elif _ARCH_PWR6
#define LJ_ARCH_VERSION      60
#elif _ARCH_PWR5X
#define LJ_ARCH_VERSION      51
#elif _ARCH_PWR5
#define LJ_ARCH_VERSION      50
#elif _ARCH_PWR4
#define LJ_ARCH_VERSION      40
#else
#define LJ_ARCH_VERSION      0
#endif

#if _ARCH_PPCSQ
#define LJ_ARCH_SQRT      1
#endif

#if _ARCH_PWR5X
#define LJ_ARCH_ROUND      1
#endif

#if __PPU__
#define LJ_ARCH_CELL      1
#endif

#else
#error "No target architecture defined"
#endif

//********************************************************************************************************************
// Check target-specific constraints.

#ifndef _BUILDVM_H
#if LJ_TARGET_X64
#if __USING_SJLJ_EXCEPTIONS__
#error "Need a C compiler with native exception handling on x64"
#endif
#elif LJ_TARGET_ARM64
#if defined(_ILP32)
#error "No support for ILP32 model on ARM64"
#endif
#elif LJ_TARGET_PPC
// 64-bit PPC only - both big and little endian supported
#endif
#endif

//********************************************************************************************************************
// Enable or disable the dual-number mode for the VM.

#if (LJ_ARCH_NUMMODE == LJ_NUMMODE_SINGLE && LUAJIT_NUMMODE == 2) || \
    (LJ_ARCH_NUMMODE == LJ_NUMMODE_DUAL && LUAJIT_NUMMODE == 1)
#error "No support for this number mode on this architecture"
#endif

#if LJ_ARCH_NUMMODE == LJ_NUMMODE_DUAL || \
    (LJ_ARCH_NUMMODE == LJ_NUMMODE_DUAL_SINGLE && LUAJIT_NUMMODE != 1) || \
    (LJ_ARCH_NUMMODE == LJ_NUMMODE_SINGLE_DUAL && LUAJIT_NUMMODE == 2)
#define LJ_DUALNUM      1
#else
#define LJ_DUALNUM      0
#endif

#if LJ_TARGET_IOS
 // Runtime code generation is restricted on iOS. Complain to Apple, not me.
 #ifndef LUAJIT_ENABLE_JIT
  #define LJ_OS_NOJIT      1
 #endif
#endif

#define LJ_GC64 1   // 64 bit GC references - always enabled.
#define LJ_FR2 1    // 2-slot frame info - always enabled.
#define LJ_HASJIT 1 // JIT is always compiled in (user can switch it off at run time)

#define LJ_HASFFI      0 // Always 0
#define LJ_HASBUFFER   0 // Always 0
#define LJ_HASPROFILE  0

#ifndef LJ_ARCH_HASFPU
#define LJ_ARCH_HASFPU      1 // Always 1
#endif

#ifndef LJ_ABI_SOFTFP
#define LJ_ABI_SOFTFP      0 // Legacy, never enabled
#endif

#define LJ_SOFTFP      0 // DEPRECATED
#define LJ_SOFTFP32    0 // DEPRECATED

#if LJ_ARCH_ENDIAN == LUAJIT_BE
#define LJ_LE         0
#define LJ_BE         1
#define LJ_ENDIAN_SELECT(le, be)   be
#define LJ_ENDIAN_LOHI(lo, hi)      hi lo
#else
#define LJ_LE         1
#define LJ_BE         0
#define LJ_ENDIAN_SELECT(le, be)   le
#define LJ_ENDIAN_LOHI(lo, hi)      lo hi
#endif

#define LJ_64         1 // Always 1, builds are 64-bit only

#ifndef LJ_TARGET_UNALIGNED
#define LJ_TARGET_UNALIGNED   0
#endif

#ifndef LJ_PAGESIZE
#define LJ_PAGESIZE      4096
#endif

//********************************************************************************************************************
// Various workarounds for embedded operating systems or weak C runtimes.

#if defined(__ANDROID__) || defined(__symbian__) || LJ_TARGET_WINDOWS
#define LUAJIT_NO_LOG2
#endif

#if (LJ_TARGET_IOS && __IPHONE_OS_VERSION_MIN_REQUIRED >= __IPHONE_8_0)
#define LJ_NO_SYSTEM      1
#endif

#if LJ_TARGET_WINDOWS || LJ_TARGET_CYGWIN
#define LJ_ABI_WIN      1
#else
#define LJ_ABI_WIN      0
#endif

#if LJ_TARGET_WINDOWS
#if LJ_TARGET_UWP
#define LJ_WIN_VALLOC   VirtualAllocFromApp
#define LJ_WIN_VPROTECT   VirtualProtectFromApp
extern void* LJ_WIN_LOADLIBA(const char* path);
#else
#define LJ_WIN_VALLOC   VirtualAlloc
#define LJ_WIN_VPROTECT   VirtualProtect
#define LJ_WIN_LOADLIBA(path)   LoadLibraryExA((path), NULL, 0)
#endif
#endif

#if defined(LUAJIT_NO_UNWIND) || __GNU_COMPACT_EH__ || defined(__symbian__) || LJ_TARGET_IOS || LJ_TARGET_PS3 || LJ_TARGET_PS4
#define LJ_NO_UNWIND      1
#endif

//********************************************************************************************************************
// LJ_UNWIND_EXT controls whether external frame unwinding is used.
//
// When set to 1, LuaJIT uses the system-provided unwind handler (e.g., libgcc_s on Linux, system exception handling
// on Windows). This provides full C++ exception interoperability and allows Lua errors to propagate through C++
// frames with proper destructor calls. However, it requires all C code on the stack to have unwind tables.
//
// When set to 0, LuaJIT uses internal frame unwinding which is faster and doesn't require unwind tables, but has
// limited C++ exception support.
//
// See detailed discussion in lj_err.cpp lines 21-85 for pros/cons of each approach.

#if !LJ_NO_UNWIND && !defined(LUAJIT_UNWIND_INTERNAL) && (LJ_ABI_WIN || (defined(LUAJIT_UNWIND_EXTERNAL) && (defined(__GNUC__) || defined(__clang__))))
#define LJ_UNWIND_EXT      1
#else
#define LJ_UNWIND_EXT      0
#endif

#if LJ_UNWIND_EXT && LJ_HASJIT && !(LJ_ABI_WIN && LJ_TARGET_X86)
#define LJ_UNWIND_JIT      1
#else
#define LJ_UNWIND_JIT      0
#endif

#define LJ_52         1 // Always 1

#ifndef LUAJIT_SECURITY_PRNG
// PRNG init: 0 = fixed/insecure, 1 = secure from OS.
#define LUAJIT_SECURITY_PRNG   1
#endif

#ifndef LUAJIT_SECURITY_MCODE // Machine code page protection: 0 = insecure RWX, 1 = secure RW^X.
#define LUAJIT_SECURITY_MCODE   1
#endif
