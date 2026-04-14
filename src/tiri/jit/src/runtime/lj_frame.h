/*
** Stack frames.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lj_obj.h"
#include "lj_bc.h"

// -- Lua stack frame -----------------------------------------------------

/* Frame type markers in LSB of PC (4-byte aligned) or delta (8-byte aligned:
**
**    PC  00  Lua frame
** delta 001  C frame
** delta 010  Continuation frame
** delta 011  Lua vararg frame
** delta 101  cpcall() frame
** delta 110  ff pcall() frame
** delta 111  ff pcall() frame with active hook
*/
enum class FrameType : int {
   Lua = 0,
   C = 1,
   Cont = 2,
   Varg = 3,
   LuaP = 4,
   CP = 5,
   Pcall = 6,
   PcallH = 7
};

// Legacy frame type enum values for backwards compatibility
inline constexpr int FRAME_LUA = int(FrameType::Lua);
inline constexpr int FRAME_C = int(FrameType::C);
inline constexpr int FRAME_CONT = int(FrameType::Cont);
inline constexpr int FRAME_VARG = int(FrameType::Varg);
inline constexpr int FRAME_LUAP = int(FrameType::LuaP);
inline constexpr int FRAME_CP = int(FrameType::CP);
inline constexpr int FRAME_PCALL = int(FrameType::Pcall);
inline constexpr int FRAME_PCALLH = int(FrameType::PcallH);

inline constexpr int FRAME_TYPE = 3;
inline constexpr int FRAME_P = 4;
inline constexpr int FRAME_TYPEP = (FRAME_TYPE | FRAME_P);

// Macros to access and modify Lua frames.
#if LJ_FR2
/* Two-slot frame info, required for 64 bit PC/GCRef:
**
**                   base-2  base-1      |  base  base+1 ...
**                  [func   PC/delta/ft] | [slots ...]
**                  ^-- frame            | ^-- base   ^-- top
**
** Continuation frames:
**
**   base-4  base-3  base-2  base-1      |  base  base+1 ...
**  [cont      PC ] [func   PC/delta/ft] | [slots ...]
**                  ^-- frame            | ^-- base   ^-- top
*/
#define frame_gc(f)      (gcval((f)-1))
#define frame_ftsz(f)      ((ptrdiff_t)(f)->ftsz)
#define frame_pc(f)      ((const BCIns *)frame_ftsz(f))
#define setframe_gc(f, p, tp)   (setgcVraw((f), (p), (tp)))
#define setframe_ftsz(f, sz)   ((f)->ftsz = (sz))
#define setframe_pc(f, pc)   ((f)->ftsz = (int64_t)(intptr_t)(pc))
#else
/* One-slot frame info, sufficient for 32 bit PC/GCRef:
**
**              base-1              |  base  base+1 ...
**              lo     hi           |
**             [func | PC/delta/ft] | [slots ...]
**             ^-- frame            | ^-- base   ^-- top
**
** Continuation frames:
**
**  base-2      base-1              |  base  base+1 ...
**  lo     hi   lo     hi           |
** [cont | PC] [func | PC/delta/ft] | [slots ...]
**             ^-- frame            | ^-- base   ^-- top
*/
#define frame_gc(f)      (gcref((f)->fr.func))
#define frame_ftsz(f)      ((ptrdiff_t)(f)->fr.tp.ftsz)
#define frame_pc(f)      (mref<const BCIns>((f)->fr.tp.pcr))
#define setframe_gc(f, p, tp)   (setgcref((f)->fr.func, (p)), UNUSED(tp))
#define setframe_ftsz(f, sz)   ((f)->fr.tp.ftsz = (int32_t)(sz))
#define setframe_pc(f, pc)   (setmref((f)->fr.tp.pcr, (pc)))
#endif

#define frame_type(f)      (frame_ftsz(f) & FRAME_TYPE)
#define frame_typep(f)      (frame_ftsz(f) & FRAME_TYPEP)
#define frame_islua(f)      (frame_type(f) == FRAME_LUA)
#define frame_isc(f)      (frame_type(f) == FRAME_C)
#define frame_iscont(f)      (frame_typep(f) == FRAME_CONT)
#define frame_isvarg(f)      (frame_typep(f) == FRAME_VARG)
#define frame_ispcall(f)   ((frame_ftsz(f) & 6) == FRAME_PCALL)

#define frame_func(f)      (&frame_gc(f)->fn)
#define frame_delta(f)      (frame_ftsz(f) >> 3)
#define frame_sized(f)      (frame_ftsz(f) & ~FRAME_TYPEP)

enum { LJ_CONT_TAILCALL, LJ_CONT_FFI_CALLBACK };  //  Special continuations.

#if LJ_FR2
#define frame_contpc(f)      (frame_pc((f)-2))
#define frame_contv(f)      (((f)-3)->u64)
#else
#define frame_contpc(f)      (frame_pc((f)-1))
#define frame_contv(f)      (((f)-1)->u32.lo)
#endif
#if LJ_FR2
#define frame_contf(f) ((ASMFunction)(uintptr_t)((f)-3)->u64)
#elif LJ_64
#define frame_contf(f) ((ASMFunction)(void *)((intptr_t)lj_vm_asm_begin + (intptr_t)(int32_t)((f)-1)->u32.lo))
#else
#define frame_contf(f)      ((ASMFunction)gcrefp(((f)-1)->gcr, void))
#endif
#define frame_iscont_fficb(f) (LJ_HASFFI and frame_contv(f) == LJ_CONT_FFI_CALLBACK)

#define frame_prevl(f)      ((f) - (1+LJ_FR2+bc_a(frame_pc(f)[-1])))
#define frame_prevd(f)      ((TValue *)((char *)(f) - frame_sized(f)))
#define frame_prev(f)      (frame_islua(f)?frame_prevl(f):frame_prevd(f))
// Note: this macro does not skip over FRAME_VARG.

// -- C stack frame -------------------------------------------------------

/* Macros to access and modify the C stack frame chain.
**
** IMPORTANT: C frame offsets must be kept in sync with the arch-specific
** DynASM files (vm_x64.dasc, vm_arm.dasc, etc.). The CFRAME_OFS_* constants
** define byte offsets from the C frame pointer (rsp/sp) to various saved
** values (previous frame, PC, Lua state, etc.).
**
** The CFRAME_SIZE varies depending on:
** - The target architecture (x86, x64, ARM, etc.)
** - The ABI (Windows vs. POSIX)
** - Whether internal unwinding is enabled (LJ_NO_UNWIND)
**
** When LJ_NO_UNWIND is enabled, additional callee-saved registers must be
** preserved for exception unwinding, increasing CFRAME_SIZE and affecting
** the relative offsets of fields within the frame.
**
** See: vm_x64.dasc (or equivalent) for the actual stack layout diagrams
** and CFRAME_SPACE macro that defines how much space is allocated for
** the C frame structure on each architecture.
*/

// These definitions must match with the arch-specific *.dasc files.
#if LJ_TARGET_X86
#if LJ_ABI_WIN
#define CFRAME_OFS_ERRF      (19*4)
#define CFRAME_OFS_NRES      (18*4)
#define CFRAME_OFS_PREV      (17*4)
#define CFRAME_OFS_L         (16*4)
#define CFRAME_OFS_SEH       (9*4)
#define CFRAME_OFS_PC        (6*4)
#define CFRAME_OFS_MULTRES   (5*4)
#define CFRAME_SIZE          (16*4)
#define CFRAME_SHIFT_MULTRES 0
#else
#define CFRAME_OFS_ERRF      (15*4)
#define CFRAME_OFS_NRES      (14*4)
#define CFRAME_OFS_PREV      (13*4)
#define CFRAME_OFS_L         (12*4)
#define CFRAME_OFS_PC        (6*4)
#define CFRAME_OFS_MULTRES   (5*4)
#define CFRAME_SIZE          (12*4)
#define CFRAME_SHIFT_MULTRES 0
#endif
#elif LJ_TARGET_X64
#if LJ_ABI_WIN
#define CFRAME_OFS_PREV      (13*8)
#define CFRAME_OFS_PC        (12*8)
#define CFRAME_OFS_L         (11*8)
#define CFRAME_OFS_ERRF      (21*4)
#define CFRAME_OFS_NRES      (20*4)
#define CFRAME_OFS_MULTRES   (8*4)
#define CFRAME_SIZE          (10*8)
#define CFRAME_SIZE_JIT      (CFRAME_SIZE + 9*16 + 4*8)
#define CFRAME_SHIFT_MULTRES 0
#else
/* x64/POSIX stack layout
**
** NOTE: This code uses external frame unwinding (-DLUAJIT_UNWIND_EXTERNAL)
** instead of internal unwinding (-DLUAJIT_NO_UNWIND). Internal unwinding
** had bugs related to frame offset calculations that were difficult to fix
** reliably across Debug and Release builds.
**
** External unwinding uses DWARF2-based unwinding tables which are more
** reliable and have better C++ exception integration.
**
** CFRAME_OFS_PREV offset for external unwinding:
** - Register saves: rbp, rbx, r15, r14 = 4 registers = 32 bytes
** - Then: sub rsp, CFRAME_SPACE (40 bytes)
** - CFRAME_OFS_PREV reads from [rsp+4*8] = correct location
*/

#define CFRAME_OFS_PREV      (4*8)
#define CFRAME_OFS_PC        (3*8)
#define CFRAME_OFS_L         (2*8)
#define CFRAME_OFS_ERRF      (3*4)
#define CFRAME_OFS_NRES      (2*4)
#define CFRAME_OFS_MULTRES   (0*4)

/* C frame size on x64/POSIX with external unwinding.
** CFRAME_SIZE = 10*8 = 80 bytes
**
** NOTE: If you modify the register save sequence in vm_x64.dasc, you must
** update CFRAME_SIZE and CFRAME_OFS_PREV accordingly. These values must
** match the actual stack frame layout generated by the DynASM code.
*/
#define CFRAME_SIZE          (10*8)
#define CFRAME_SIZE_JIT      (CFRAME_SIZE + 16)
#define CFRAME_SHIFT_MULTRES 0
#endif
#elif LJ_TARGET_ARM
#define CFRAME_OFS_ERRF      24
#define CFRAME_OFS_NRES      20
#define CFRAME_OFS_PREV      16
#define CFRAME_OFS_L      12
#define CFRAME_OFS_PC      8
#define CFRAME_OFS_MULTRES   4
#if LJ_ARCH_HASFPU
#define CFRAME_SIZE      128
#else
#define CFRAME_SIZE      64
#endif
#define CFRAME_SHIFT_MULTRES   3
#elif LJ_TARGET_ARM64
#define CFRAME_OFS_ERRF      36
#define CFRAME_OFS_NRES      40
#define CFRAME_OFS_PREV      0
#define CFRAME_OFS_L         16
#define CFRAME_OFS_PC        8
#define CFRAME_OFS_MULTRES   32
#define CFRAME_SIZE          208
#define CFRAME_SHIFT_MULTRES 3
#elif LJ_TARGET_PPC
#if LJ_TARGET_XBOX360
#define CFRAME_OFS_ERRF      424
#define CFRAME_OFS_NRES      420
#define CFRAME_OFS_PREV      400
#define CFRAME_OFS_L         416
#define CFRAME_OFS_PC        412
#define CFRAME_OFS_MULTRES   408
#define CFRAME_SIZE          384
#define CFRAME_SHIFT_MULTRES   3
#elif LJ_ARCH_PPC32ON64
#define CFRAME_OFS_ERRF      472
#define CFRAME_OFS_NRES      468
#define CFRAME_OFS_PREV      448
#define CFRAME_OFS_L         464
#define CFRAME_OFS_PC        460
#define CFRAME_OFS_MULTRES   456
#define CFRAME_SIZE      400
#define CFRAME_SHIFT_MULTRES   3
#else
#define CFRAME_OFS_ERRF      48
#define CFRAME_OFS_NRES      44
#define CFRAME_OFS_PREV      40
#define CFRAME_OFS_L         36
#define CFRAME_OFS_PC        32
#define CFRAME_OFS_MULTRES   28
#define CFRAME_SIZE          (LJ_ARCH_HASFPU ? 272 : 128)
#define CFRAME_SHIFT_MULTRES 3
#endif
#else
#error "Missing CFRAME_* definitions for this architecture"
#endif

#ifndef CFRAME_SIZE_JIT
#define CFRAME_SIZE_JIT      CFRAME_SIZE
#endif

#define CFRAME_RAWMASK      (~(intptr_t)(CFRAME_RESUME|CFRAME_UNWIND_FF))
inline constexpr int CFRAME_RESUME = 1;
inline constexpr int CFRAME_UNWIND_FF = 2;  //  Only used in unwinder.

#define cframe_errfunc(cf)   (*(int32_t *)(((char *)(cf))+CFRAME_OFS_ERRF))
#define cframe_nres(cf)      (*(int32_t *)(((char *)(cf))+CFRAME_OFS_NRES))
#define cframe_prev(cf)      (*(void **)(((char *)(cf))+CFRAME_OFS_PREV))
#define cframe_multres(cf)   (*(uint32_t *)(((char *)(cf))+CFRAME_OFS_MULTRES))
#define cframe_multres_n(cf) (cframe_multres((cf)) >> CFRAME_SHIFT_MULTRES)
#define cframe_L(cf)         (&gcref(*(GCRef *)(((char *)(cf))+CFRAME_OFS_L))->th)
#define cframe_pc(cf)        (mref<const BCIns>(*(MRef *)(((char *)(cf))+CFRAME_OFS_PC)))
#define setcframe_L(cf, L)   (setmref(*(MRef *)(((char *)(cf))+CFRAME_OFS_L), (L)))
#define setcframe_pc(cf, pc) (setmref(*(MRef *)(((char *)(cf))+CFRAME_OFS_PC), (pc)))
#define cframe_canyield(cf)  ((intptr_t)(cf) & CFRAME_RESUME)
#define cframe_unwind_ff(cf) ((intptr_t)(cf) & CFRAME_UNWIND_FF)
#define cframe_raw(cf)       ((void *)((intptr_t)(cf) & CFRAME_RAWMASK))
#define cframe_Lpc(L)        cframe_pc(cframe_raw(L->cframe))
