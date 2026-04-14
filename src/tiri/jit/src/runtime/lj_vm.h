// Assembler VM interface definitions.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_obj.h"

// Entry points for ASM parts of VM.
LJ_ASMF void lj_vm_call(lua_State *L, TValue *base, int nres1);
LJ_ASMF int lj_vm_pcall(lua_State *L, TValue *base, int nres1, ptrdiff_t ef);
using lua_CPFunction = TValue*(*)(lua_State *L, lua_CFunction func, void *ud);
LJ_ASMF int lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud, lua_CPFunction cp);
LJ_ASMF int lj_vm_resume(lua_State *L, TValue *base, int nres1, ptrdiff_t ef);
LJ_ASMF_NORET void lj_vm_unwind_c(void *cframe, int errcode);
LJ_ASMF_NORET void lj_vm_unwind_ff(void *cframe);
LJ_ASMF_NORET void lj_vm_resume_try(void *cframe);
#if LJ_ABI_WIN && LJ_TARGET_X86
LJ_ASMF_NORET void lj_vm_rtlunwind(void *cframe, void *excptrec, void *unwinder, int errcode);
#endif
LJ_ASMF void lj_vm_unwind_c_eh(void);
LJ_ASMF void lj_vm_unwind_ff_eh(void);
LJ_ASMF void lj_vm_resume_try_eh(void);
#if LJ_TARGET_X86ORX64
LJ_ASMF void lj_vm_unwind_rethrow(void);
#endif

// Miscellaneous functions.
#if LJ_TARGET_X86ORX64
LJ_ASMF int lj_vm_cpuid(uint32_t f, uint32_t res[4]);
#endif
#if LJ_TARGET_PPC
void lj_vm_cachesync(void *start, void *end);
#endif
LJ_ASMF double lj_vm_foldarith(double x, double y, int op);
#if LJ_HASJIT
LJ_ASMF double lj_vm_foldfpm(double x, int op);
#endif
#if !LJ_ARCH_HASFPU
// Declared in lj_obj.h: LJ_ASMF int32_t lj_vm_tobit(double x);
#endif

// Dispatch targets for recording and hooks.
LJ_ASMF void lj_vm_record(void);
LJ_ASMF void lj_vm_inshook(void);
LJ_ASMF void lj_vm_rethook(void);
LJ_ASMF void lj_vm_callhook(void);
LJ_ASMF void lj_vm_profhook(void);
LJ_ASMF void lj_vm_IITERN(void);
LJ_ASMF void lj_vm_IITERA(void);

// Trace exit handling.
LJ_ASMF void lj_vm_exit_handler(void);
LJ_ASMF void lj_vm_exit_interp(void);

// Internal math helper functions.
#if LJ_TARGET_PPC || LJ_TARGET_ARM64
#define lj_vm_floor   floor
#define lj_vm_ceil   ceil
#else
LJ_ASMF double lj_vm_floor(double);
LJ_ASMF double lj_vm_ceil(double);
#if LJ_TARGET_ARM
LJ_ASMF double lj_vm_floor_sf(double);
LJ_ASMF double lj_vm_ceil_sf(double);
#endif
#endif
// C math library wrappers for C++ compatibility.
LJ_ASMF double cmath_log10(double);
LJ_ASMF double deg(double);
LJ_ASMF double rad(double);
LJ_ASMF double cmath_exp(double);
LJ_ASMF double cmath_sin(double);
LJ_ASMF double cmath_cos(double);
LJ_ASMF double cmath_tan(double);
LJ_ASMF double cmath_asin(double);
LJ_ASMF double cmath_acos(double);
LJ_ASMF double cmath_atan(double);
LJ_ASMF double cmath_sinh(double);
LJ_ASMF double cmath_cosh(double);
LJ_ASMF double cmath_tanh(double);
LJ_ASMF double cmath_sqrt(double);
LJ_ASMF double cmath_log(double);
LJ_ASMF double cmath_log2(double);
LJ_ASMF double cmath_atan2(double, double);
LJ_ASMF double cmath_ldexp(double, int);

#ifdef LUAJIT_NO_LOG2
LJ_ASMF double lj_vm_log2(double);
#else
#define lj_vm_log2   cmath_log2
#endif
LJ_ASMF int32_t lj_vm_modi(int32_t, int32_t);

#if LJ_HASJIT
#if LJ_TARGET_X86ORX64
LJ_ASMF void lj_vm_floor_sse(void);
LJ_ASMF void lj_vm_ceil_sse(void);
LJ_ASMF void lj_vm_trunc_sse(void);
#endif
#if LJ_TARGET_PPC || LJ_TARGET_ARM64
#define lj_vm_trunc   trunc
#else
LJ_ASMF double lj_vm_trunc(double);
#if LJ_TARGET_ARM
LJ_ASMF double lj_vm_trunc_sf(double);
#endif
#endif
LJ_ASMF TValue *lj_vm_next(GCtab *t, uint32_t idx);
#endif

LJ_ASMF double lj_vm_powi(double, int32_t);
LJ_ASMF double lj_vm_pow(double, double);

// Continuations for metamethods.
LJ_ASMF void lj_cont_cat(void);  //  Continue with concatenation.
LJ_ASMF void lj_cont_ra(void);  //  Store result in RA from instruction.
LJ_ASMF void lj_cont_nop(void);  //  Do nothing, just continue execution.
LJ_ASMF void lj_cont_condt(void);  //  Branch if result is true.
LJ_ASMF void lj_cont_condf(void);  //  Branch if result is false.
LJ_ASMF void lj_cont_hook(void);  //  Continue from hook yield.
LJ_ASMF void lj_cont_stitch(void);  //  Trace stitching.

// Start of the ASM code.
LJ_ASMF char lj_vm_asm_begin[];

// Bytecode offsets are relative to lj_vm_asm_begin.
#define makeasmfunc(ofs)   ((ASMFunction)(lj_vm_asm_begin + (ofs)))
