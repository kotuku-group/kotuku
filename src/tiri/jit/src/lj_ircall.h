// IR CALL* instruction definitions.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_obj.h"
#include "lj_ir.h"
#include "lj_jit.h"

// C call info for CALL* instructions.

typedef struct CCallInfo {
  ASMFunction func;      //  Function pointer.
  uint32_t    flags;      //  Number of arguments and flags.
} CCallInfo;

#define CCI_NARGS(ci)    ((ci)->flags & 0xff)   //  # of args.
#define CCI_NARGS_MAX    32         //  Max. # of args.

#define CCI_OTSHIFT      16
#define CCI_OPTYPE(ci)   ((ci)->flags >> CCI_OTSHIFT)  //  Get op/type.
#define CCI_TYPE(ci)     (((ci)->flags>>CCI_OTSHIFT) & IRT_TYPE)
#define CCI_OPSHIFT      24
#define CCI_OP(ci)       ((ci)->flags >> CCI_OPSHIFT)  //  Get op.

#define CCI_CALL_N      (IR_CALLN << CCI_OPSHIFT)
#define CCI_CALL_A      (IR_CALLA << CCI_OPSHIFT)
#define CCI_CALL_L      (IR_CALLL << CCI_OPSHIFT)
#define CCI_CALL_S      (IR_CALLS << CCI_OPSHIFT)
#define CCI_CALL_FN     (CCI_CALL_N|CCI_CC_FASTCALL)
#define CCI_CALL_FA     (CCI_CALL_A|CCI_CC_FASTCALL)
#define CCI_CALL_FL     (CCI_CALL_L|CCI_CC_FASTCALL)
#define CCI_CALL_FS     (CCI_CALL_S|CCI_CC_FASTCALL)

// C call info flags.
#define CCI_T            (IRT_GUARD << CCI_OTSHIFT)  //  May throw.
#define CCI_L            0x0100   //  Implicit L arg.
#define CCI_CASTU64      0x0200   //  Cast u64 result to number.
#define CCI_NOFPRCLOBBER 0x0400   //  Does not clobber any FPRs.
#define CCI_VARARG       0x0800   //  Vararg function.

#define CCI_CC_MASK      0x3000   //  Calling convention mask.
#define CCI_CC_SHIFT     12
// ORDER CC
#define CCI_CC_CDECL      0x0000   //  Default cdecl calling convention.
#define CCI_CC_THISCALL   0x1000   //  Thiscall calling convention.
#define CCI_CC_FASTCALL   0x2000   //  Fastcall calling convention.
#define CCI_CC_STDCALL    0x3000   //  Stdcall calling convention.

// Extra args for SOFTFP, SPLIT 64 bit.
#define CCI_XARGS_SHIFT   14
#define CCI_XARGS(ci)     (((ci)->flags >> CCI_XARGS_SHIFT) & 3)
#define CCI_XA            (1u << CCI_XARGS_SHIFT)

#define CCI_XNARGS(ci)      CCI_NARGS((ci))

// Helpers for conditional function definitions.
#define IRCALLCOND_ANY(x)      x

#if LJ_TARGET_X86ORX64
#define IRCALLCOND_FPMATH(x)      NULL
#else
#define IRCALLCOND_FPMATH(x)      x
#endif

#define IRCALLCOND_SOFTFP(x)      NULL
#define IRCALLCOND_SOFTFP_FFI(x)   NULL

#define LJ_NEED_FP64   (LJ_TARGET_ARM || LJ_TARGET_PPC)

#if LJ_HASFFI && LJ_NEED_FP64
#define IRCALLCOND_FP64_FFI(x)      x
#else
#define IRCALLCOND_FP64_FFI(x)      NULL
#endif

#if LJ_HASFFI
#define IRCALLCOND_FFI(x)      x
#else
#define IRCALLCOND_FFI(x)      NULL
#endif

#define IRCALLCOND_FFI32(x)      NULL
#define IRCALLCOND_BUFFER(x)      NULL
#define IRCALLCOND_BUFFFI(x)      NULL

#define XA_FP      0
#define XA2_FP     0
#define XA_FP32    0
#define XA2_FP32   0
#define XA_64      0
#define XA2_64     0

// Function definitions for CALL* instructions.
// cond, name, nargs, kind, type, flags
//
// Cond values (conditional compilation):
//   ANY        - Always included
//   FPMATH     - Included when FP math calls are needed (non-x86/x64)
//   FP64_FFI   - Included for 64-bit FP conversion with FFI
//   FFI        - Included when FFI is enabled
//   BUFFER     - Included when buffer library is enabled
//   BUFFFI     - Included when both buffer and FFI are enabled
//
// Kind values (base call types):
//   N  = IR_CALLN - Normal call, no special semantics
//   A  = IR_CALLA - Call that may allocate (triggers GC barriers)
//   L  = IR_CALLL - Call with implicit lua_State* prefix argument
//   S  = IR_CALLS - Call with store semantics (may modify memory)
//
// Kind values (fastcall variants):
//   FN = N + fastcall calling convention
//   FA = A + fastcall calling convention
//   FL = L + fastcall calling convention
//   FS = S + fastcall calling convention
//
// Type values (return type, maps to IRT_* constants):
//   NIL, INT, NUM, FLOAT, STR, TAB, PTR, PGC, I64, U64, INTP

#define IRCALLDEF(_) \
  _(ANY,    lj_str_cmp,            2,  FN, INT, CCI_NOFPRCLOBBER) \
  _(ANY,    lj_str_find,           4,   N, PGC, 0) \
  _(ANY,    lj_str_new,            3,   S, STR, CCI_L|CCI_T) \
  _(ANY,    lj_strscan_num,        2,  FN, INT, 0) \
  _(ANY,    lj_strfmt_int,         2,  FN, STR, CCI_L|CCI_T) \
  _(ANY,    lj_strfmt_num,         2,  FN, STR, CCI_L|CCI_T) \
  _(ANY,    lj_strfmt_char,        2,  FN, STR, CCI_L|CCI_T) \
  _(ANY,    lj_strfmt_obj,         2,  FN, STR, CCI_L|CCI_T) \
  _(ANY,    lj_strfmt_putint,      2,  FL, PGC, CCI_T) \
  _(ANY,    lj_strfmt_putnum,      2,  FL, PGC, CCI_T) \
  _(ANY,    lj_strfmt_putquoted,   2,  FL, PGC, CCI_T) \
  _(ANY,    lj_strfmt_putfxint,    3,   L, PGC, XA_64|CCI_T) \
  _(ANY,    lj_strfmt_putfnum_int, 3,   L, PGC, XA_FP|CCI_T) \
  _(ANY,    lj_strfmt_putfnum_uint,3,   L, PGC, XA_FP|CCI_T) \
  _(ANY,    lj_strfmt_putfnum,     3,   L, PGC, XA_FP|CCI_T) \
  _(ANY,    lj_strfmt_putfstr,     3,   L, PGC, CCI_T) \
  _(ANY,    lj_strfmt_putfchar,    3,   L, PGC, CCI_T) \
  _(ANY,    lj_buf_putmem,         3,   S, PGC, CCI_T) \
  _(ANY,    lj_buf_putstr,         2,  FL, PGC, CCI_T) \
  _(ANY,    lj_buf_putchar,        2,  FL, PGC, CCI_T) \
  _(ANY,    lj_buf_putstr_reverse, 2,  FL, PGC, CCI_T) \
  _(ANY,    lj_buf_putstr_lower,   2,  FL, PGC, CCI_T) \
  _(ANY,    lj_buf_putstr_upper,   2,  FL, PGC, CCI_T) \
  _(ANY,    lj_buf_putstr_rep,     3,   L, PGC, CCI_T) \
  _(ANY,    lj_buf_puttab,         5,   L, PGC, CCI_T) \
  _(BUFFER, lj_serialize_put,      2,  FS, PGC, CCI_T) \
  _(BUFFER, lj_serialize_get,      2,  FS, PTR, CCI_T) \
  _(BUFFER, lj_serialize_encode,   2,  FA, STR, CCI_L|CCI_T) \
  _(BUFFER, lj_serialize_decode,   3,   A, INT, CCI_L|CCI_T) \
  _(ANY,    lj_buf_tostr,          1,  FL, STR, CCI_T) \
  _(ANY,    lj_tab_new_ah,         3,   A, TAB, CCI_L|CCI_T) \
  _(ANY,    lj_tab_new1,           2,  FA, TAB, CCI_L|CCI_T) \
  _(ANY,    lj_tab_dup,            2,  FA, TAB, CCI_L|CCI_T) \
  _(ANY,    lj_tab_clear,          1,  FS, NIL, 0) \
  _(ANY,    lj_tab_newkey,         3,   S, PGC, CCI_L|CCI_T) \
  _(ANY,    lj_tab_keyindex,       2,  FL, INT, 0) \
  _(ANY,    lj_vm_next,            2,  FL, PTR, 0) \
  _(ANY,    lj_tab_len,            1,  FL, INT, 0) \
  _(ANY,    lj_tab_len_hint,       2,  FL, INT, 0) \
  _(ANY,    lj_gc_step_jit,        2,  FS, NIL, CCI_L) \
  _(ANY,    lj_gc_barrieruv,       2,  FS, NIL, 0) \
  _(ANY,    lj_mem_newgco,         2,  FA, PGC, CCI_L|CCI_T) \
  _(ANY,    lj_prng_u64d,          1,  FS, NUM, CCI_CASTU64) \
  _(ANY,    lj_vm_modi,            2,  FN, INT, 0) \
  _(ANY,    cmath_log10,           1,   N, NUM, XA_FP) \
  _(ANY,    deg,                   1,   N, NUM, XA_FP) \
  _(ANY,    rad,                   1,   N, NUM, XA_FP) \
  _(ANY,    cmath_exp,             1,   N, NUM, XA_FP) \
  _(ANY,    cmath_sin,             1,   N, NUM, XA_FP) \
  _(ANY,    cmath_cos,             1,   N, NUM, XA_FP) \
  _(ANY,    cmath_tan,             1,   N, NUM, XA_FP) \
  _(ANY,    cmath_asin,            1,   N, NUM, XA_FP) \
  _(ANY,    cmath_acos,            1,   N, NUM, XA_FP) \
  _(ANY,    cmath_atan,            1,   N, NUM, XA_FP) \
  _(ANY,    cmath_sinh,            1,   N, NUM, XA_FP) \
  _(ANY,    cmath_cosh,            1,   N, NUM, XA_FP) \
  _(ANY,    cmath_tanh,            1,   N, NUM, XA_FP) \
  _(ANY,    fputc,                 2,   S, INT, 0) \
  _(ANY,    fwrite,                4,   S, INT, 0) \
  _(ANY,    fflush,                1,   S, INT, 0) \
  /* ORDER FPM */ \
  _(FPMATH,     lj_vm_floor,       1,   N, NUM, XA_FP) \
  _(FPMATH,     lj_vm_ceil,        1,   N, NUM, XA_FP) \
  _(FPMATH,     lj_vm_trunc,       1,   N, NUM, XA_FP) \
  _(FPMATH,     cmath_sqrt,        1,   N, NUM, XA_FP) \
  _(ANY,        cmath_log,         1,   N, NUM, XA_FP) \
  _(ANY,        lj_vm_log2,        1,   N, NUM, XA_FP) \
  _(ANY,        lj_vm_powi,        2,   N, NUM, XA_FP) \
  _(ANY,        lj_vm_pow,         2,   N, NUM, XA2_FP) \
  _(ANY,        cmath_atan2,       2,   N, NUM, XA2_FP) \
  _(ANY,        cmath_ldexp,       2,   N, NUM, XA_FP) \
  _(FP64_FFI,   fp64_l2d,          1,   N, NUM, XA_64) \
  _(FP64_FFI,   fp64_ul2d,         1,   N, NUM, XA_64) \
  _(FP64_FFI,   fp64_l2f,          1,   N, FLOAT, XA_64) \
  _(FP64_FFI,   fp64_ul2f,         1,   N, FLOAT, XA_64) \
  _(FP64_FFI,   fp64_d2l,          1,   N, I64, XA_FP) \
  _(FP64_FFI,   fp64_d2ul,         1,   N, U64, XA_FP) \
  _(FP64_FFI,   fp64_f2l,          1,   N, I64, 0) \
  _(FP64_FFI,   fp64_f2ul,         1,   N, U64, 0) \
  _(FFI,        lj_carith_divi64,  2,   N, I64, XA2_64|CCI_NOFPRCLOBBER) \
  _(FFI,        lj_carith_divu64,  2,   N, U64, XA2_64|CCI_NOFPRCLOBBER) \
  _(FFI,        lj_carith_modi64,  2,   N, I64, XA2_64|CCI_NOFPRCLOBBER) \
  _(FFI,        lj_carith_modu64,  2,   N, U64, XA2_64|CCI_NOFPRCLOBBER) \
  _(FFI,        lj_carith_powi64,  2,   N, I64, XA2_64|CCI_NOFPRCLOBBER) \
  _(FFI,        lj_carith_powu64,  2,   N, U64, XA2_64|CCI_NOFPRCLOBBER) \
  _(FFI,        strlen,            1,   L, INTP, 0) \
  _(FFI,        memcpy,            3,   S, PTR, 0) \
  _(FFI,        memset,            3,   S, PTR, 0) \
  _(FFI,        lj_vm_errno,       0,   S, INT, CCI_NOFPRCLOBBER) \
  /* Native array helpers */ \
  _(ANY,        lj_arr_getidx,     4,   S, NIL, CCI_L|CCI_T) \
  _(ANY,        lj_arr_setidx,     4,   S, NIL, CCI_L|CCI_T) \
  /* Try-except exception handling */ \
  _(ANY,        lj_try_enter,      4,  FS, NIL, CCI_L|CCI_T) \
  _(ANY,        lj_try_leave,      1,  FS, NIL, CCI_L) \
  /* Native object field access */ \
  _(ANY,        bc_object_getfield, 5, S, NIL, CCI_L|CCI_T) \
  _(ANY,        bc_object_setfield, 5, S, NIL, CCI_L|CCI_T) \
  /* JIT direct field access lock/unlock */ \
  _(ANY,        jit_object_lock,   1, S, PTR, 0) \
  _(ANY,        jit_object_unlock, 1, S, NIL, 0) \
  _(ANY,        jit_object_getstr, 4, S, NIL, CCI_L|CCI_T) \
  _(ANY,        jit_object_getobj, 4, S, NIL, CCI_L|CCI_T) \
  \
  // End of list.

typedef enum {
#define IRCALLENUM(cond, name, nargs, kind, type, flags)   IRCALL_##name,
IRCALLDEF(IRCALLENUM)
#undef IRCALLENUM
  IRCALL__MAX
} IRCallID;

LJ_FUNC TRef lj_ir_call(jit_State *J, IRCallID id, ...);

LJ_DATA const CCallInfo lj_ir_callinfo[IRCALL__MAX+1];

#if LJ_HASFFI && LJ_NEED_FP64
#if defined(__GNUC__) || defined(__clang__)
#define fp64_l2d __floatdidf
#define fp64_ul2d __floatundidf
#define fp64_l2f __floatdisf
#define fp64_ul2f __floatundisf
#define fp64_d2l __fixdfdi
#define fp64_d2ul __fixunsdfdi
#define fp64_f2l __fixsfdi
#define fp64_f2ul __fixunssfdi
#else
#error "Missing fp64 helper definitions for this compiler"
#endif
#endif

// Try-except exception handling runtime functions
extern "C" void lj_try_enter(lua_State *L, GCfunc *Func, TValue *Base, uint16_t TryBlockIndex);
extern "C" void lj_try_leave(lua_State *L);
