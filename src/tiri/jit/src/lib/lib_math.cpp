/*
** Math library.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#include <math.h>

#include <cmath>
#include <limits>

#define lib_math_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ff.h"
#include "lib.h"
#include "lj_vm.h"
#include "lj_prng.h"

#define LJLIB_MODULE_math

LJLIB_ASM(math_abs)      LJLIB_REC(.)
{
   lj_lib_checknumber(L, 1);
   return FFH_RETRY;
}
LJLIB_ASM_(math_floor)      LJLIB_REC(math_round IRFPM_FLOOR)
LJLIB_ASM_(math_ceil)      LJLIB_REC(math_round IRFPM_CEIL)

LJLIB_ASM(math_sqrt)      LJLIB_REC(math_unary IRFPM_SQRT)
{
   lj_lib_checknum(L, 1);
   return FFH_RETRY;
}
LJLIB_ASM_(math_log10)     LJLIB_REC(math_call IRCALL_cmath_log10)
LJLIB_ASM_(math_deg)       LJLIB_REC(math_call IRCALL_deg)
LJLIB_ASM_(math_rad)       LJLIB_REC(math_call IRCALL_rad)
LJLIB_ASM_(math_exp)       LJLIB_REC(math_call IRCALL_cmath_exp)
LJLIB_ASM_(math_sin)       LJLIB_REC(math_call IRCALL_cmath_sin)
LJLIB_ASM_(math_cos)       LJLIB_REC(math_call IRCALL_cmath_cos)
LJLIB_ASM_(math_tan)       LJLIB_REC(math_call IRCALL_cmath_tan)
LJLIB_ASM_(math_asin)      LJLIB_REC(math_call IRCALL_cmath_asin)
LJLIB_ASM_(math_acos)      LJLIB_REC(math_call IRCALL_cmath_acos)
LJLIB_ASM_(math_atan)      LJLIB_REC(math_call IRCALL_cmath_atan)
LJLIB_ASM_(math_sinh)      LJLIB_REC(math_call IRCALL_cmath_sinh)
LJLIB_ASM_(math_cosh)      LJLIB_REC(math_call IRCALL_cmath_cosh)
LJLIB_ASM_(math_tanh)      LJLIB_REC(math_call IRCALL_cmath_tanh)
LJLIB_ASM_(math_frexp)
LJLIB_ASM_(math_modf)

LJLIB_ASM(math_log)      LJLIB_REC(math_log)
{
   double x = lj_lib_checknum(L, 1);
   if (L->base + 1 < L->top) {
      double y = lj_lib_checknum(L, 2);
#ifdef LUAJIT_NO_LOG2
      x = log(x); y = 1.0 / log(y);
#else
      x = lj_vm_log2(x); y = 1.0 / lj_vm_log2(y);
#endif
      setnumV(L->base - 1 - LJ_FR2, x * y);  //  Do NOT join the expression to x / y.
      return FFH_RES(1);
   }
   return FFH_RETRY;
}

LJLIB_ASM(math_atan2)      LJLIB_REC(.)
{
   lj_lib_checknum(L, 1);
   lj_lib_checknum(L, 2);
   return FFH_RETRY;
}

LJLIB_ASM(math_ldexp)      LJLIB_REC(.)
{
   lua_Number value = lj_lib_checknum(L, 1);
   lua_Number exponent = lj_lib_checknum(L, 2);
   if (not std::isfinite(exponent) or std::trunc(exponent) != exponent) {
      luaL_argerror(L, 2, "exponent must be a finite integer");
   }

   int native_exponent;
   if (exponent > lua_Number(std::numeric_limits<int>::max())) {
      native_exponent = std::numeric_limits<int>::max();
   }
   else if (exponent < lua_Number(std::numeric_limits<int>::min())) {
      native_exponent = std::numeric_limits<int>::min();
   }
   else native_exponent = int(exponent);

   setnumV(L->base - 1 - LJ_FR2, std::ldexp(value, native_exponent));
   return FFH_RES(1);
}

LJLIB_ASM(math_min)      LJLIB_REC(math_minmax IR_MIN)
{
   int argument = 1;
   bool find_maximum = curr_func(L)->c.ffid IS FF_math_max;
   lj_lib_checknumber(L, argument);
#if LJ_DUALNUM
   bool is_integer = tvisint(L->base);
   int32_t integer_result = is_integer ? intV(L->base) : 0;
   lua_Number number_result = is_integer ? lua_Number(integer_result) : numV(L->base);
#else
   lua_Number number_result = numV(L->base);
#endif

   while (L->base + argument < L->top) {
      lj_lib_checknumber(L, ++argument);
#if LJ_DUALNUM
      TValue* value = L->base + argument - 1;
      if (is_integer and tvisint(value)) {
         if ((find_maximum and intV(value) > integer_result) or
             (not find_maximum and intV(value) < integer_result)) integer_result = intV(value);
      }
      else {
         if (is_integer) {
            number_result = lua_Number(integer_result);
            is_integer = false;
         }
         lua_Number number = tvisint(value) ? lua_Number(intV(value)) : numV(value);
         number_result = find_maximum ? lj_vm_max(number_result, number) : lj_vm_min(number_result, number);
      }
#else
      lua_Number number = numV(L->base + argument - 1);
      number_result = find_maximum ? lj_vm_max(number_result, number) : lj_vm_min(number_result, number);
#endif
   }

#if LJ_DUALNUM
   if (is_integer) setintV(L->top - 1, integer_result);
   else setnumV(L->top - 1, number_result);
#else
   setnumV(L->top - 1, number_result);
#endif
   return 1;
}

LJLIB_ASM_(math_max)      LJLIB_REC(math_minmax IR_MAX)

LJLIB_CF(math_clamp)      LJLIB_REC(.)
{
#if LJ_DUALNUM
   lj_lib_checknumber(L, 1);
   lj_lib_checknumber(L, 2);
   lj_lib_checknumber(L, 3);

   TValue* value_tv = L->base;
   TValue* lower_tv = L->base + 1;
   TValue* upper_tv = L->base + 2;

   if (tvisint(value_tv) and tvisint(lower_tv) and tvisint(upper_tv)) {
      int32_t result = intV(value_tv);
      int32_t lower = intV(lower_tv);
      int32_t upper = intV(upper_tv);
      if (lower > upper) luaL_argerror(L, 2, "lower bound must not exceed upper bound");
      result = result > lower ? result : lower;
      result = result < upper ? result : upper;
      setintV(L->top - 1, result);
   }
   else {
      lua_Number result = tvisint(value_tv) ? lua_Number(intV(value_tv)) : numV(value_tv);
      lua_Number lower = tvisint(lower_tv) ? lua_Number(intV(lower_tv)) : numV(lower_tv);
      lua_Number upper = tvisint(upper_tv) ? lua_Number(intV(upper_tv)) : numV(upper_tv);
      if (std::isnan(lower)) luaL_argerror(L, 2, "lower bound must not be NaN");
      if (std::isnan(upper)) luaL_argerror(L, 3, "upper bound must not be NaN");
      if (lower > upper) luaL_argerror(L, 2, "lower bound must not exceed upper bound");
      if (not std::isnan(result)) {
         result = result > lower ? result : lower;
         result = result < upper ? result : upper;
      }
      setnumV(L->top - 1, result);
   }
#else
   lua_Number result = lj_lib_checknum(L, 1);
   lua_Number lower = lj_lib_checknum(L, 2);
   lua_Number upper = lj_lib_checknum(L, 3);
   if (std::isnan(lower)) luaL_argerror(L, 2, "lower bound must not be NaN");
   if (std::isnan(upper)) luaL_argerror(L, 3, "upper bound must not be NaN");
   if (lower > upper) luaL_argerror(L, 2, "lower bound must not exceed upper bound");
   if (not std::isnan(result)) {
      result = result > lower ? result : lower;
      result = result < upper ? result : upper;
   }
   setnumV(L->top - 1, result);
#endif
   return 1;
}

LJLIB_CF(math_round)
{
   if (L->base + 1 < L->top) luaL_argerror(L, 2, "unexpected argument; math.round accepts one value");
   double num = lj_lib_checknum(L, 1);
   double result = std::round(num);
   setnumV(L->top - 1, result);
   return 1;
}

LJLIB_PUSH(3.14159265358979323846) LJLIB_SET(pi)
LJLIB_PUSH(1e310) LJLIB_SET(huge)

/* This implements a Tausworthe PRNG with period 2^223. Based on:
**   Tables of maximally-equidistributed combined LFSR generators,
**   Pierre L'Ecuyer, 1991, table 3, 1st entry.
** Full-period ME-CF generator with L=64, J=4, k=223, N1=49.
*/

// Union needed for bit-pattern conversion between uint64_t and double.
typedef union { uint64_t u64; double d; } U64double;

// PRNG seeding function.
static void random_seed(PRNGState* rs, double d)
{
   uint32_t r = 0x11090601;  //  64-k[i] as four 8 bit constants.
   int i;
   for (i = 0; i < 4; i++) {
      U64double u;
      uint32_t m = 1u << (r & 255);
      r >>= 8;
      u.d = d = d * 3.14159265358979323846 + 2.7182818284590452354;
      if (u.u64 < m) u.u64 += m;  //  Ensure k[i] MSB of u[i] are non-zero.
      rs->u[i] = u.u64;
   }

   for (i = 0; i < 10; i++) (void)lj_prng_u64(rs);
}

// PRNG extract function.
LJLIB_PUSH(top-2)  //  Upvalue holds userdata with PRNGState.
LJLIB_CF(math_random)      LJLIB_REC(.)
{
   int n = (int)(L->top - L->base);
   PRNGState* rs = (PRNGState*)(uddata(udataV(lj_lib_upvalue(L, 1))));
   double r1 = 0;
   double r2 = 0;
#if LJ_DUALNUM
   int isint = 1;
#endif

   if (n > 0) {
      lj_lib_checknumber(L, 1);
#if LJ_DUALNUM
      if (tvisint(L->base)) r1 = lua_Number(intV(L->base));
      else {
         isint = 0;
         r1 = numV(L->base);
      }
#else
      r1 = numV(L->base);
#endif
      if (not std::isfinite(r1) or std::trunc(r1) != r1) {
         luaL_argerror(L, 1, n IS 1 ? "stop must be a finite integer" : "minimum must be a finite integer");
      }
      if (n IS 1 and r1 <= 0) luaL_argerror(L, 1, "stop must be greater than zero");

      if (n > 1) {
         lj_lib_checknumber(L, 2);
#if LJ_DUALNUM
         if (tvisint(L->base + 1)) r2 = lua_Number(intV(L->base + 1));
         else {
            isint = 0;
            r2 = numV(L->base + 1);
         }
#else
         r2 = numV(L->base + 1);
#endif
         if (not std::isfinite(r2) or std::trunc(r2) != r2) {
            luaL_argerror(L, 2, "maximum must be a finite integer");
         }
         if (r1 > r2) luaL_argerror(L, 1, "minimum must not exceed maximum");
      }
   }

   U64double u;
   u.u64 = lj_prng_u64d(rs);
   double d = u.d - 1.0;
   if (n > 0) {
      if (n IS 1) {
         d = lj_vm_floor(d * r1);  //  d is an int in range [0, r1)
      }
      else {
         double range = r2 - r1;
         if (std::isfinite(range)) d = lj_vm_floor(d * (range + 1.0)) + r1;
         else d = lj_vm_floor((1.0 - d) * r1 + d * r2);
      }
#if LJ_DUALNUM
      if (isint) {
         setintV(L->top - 1, lj_num2int(d));
         return 1;
      }
#endif
   }  // else: d is a double in range [0, 1]
   setnumV(L->top++, d);
   return 1;
}

// PRNG seed function.
LJLIB_PUSH(top-2)  //  Upvalue holds userdata with PRNGState.
LJLIB_CF(math_randomSeed)
{
   PRNGState* rs = (PRNGState*)(uddata(udataV(lj_lib_upvalue(L, 1))));
   random_seed(rs, lj_lib_checknum(L, 1));
   return 0;
}

// ------------------------------------------------------------------------

#include "lj_libdef.h"
#include "lj_proto_registry.h"

extern int luaopen_math(lua_State* L)
{
   PRNGState* rs = (PRNGState*)lua_newuserdata(L, sizeof(PRNGState));
   lj_prng_seed_fixed(rs);
   LJ_LIB_REG(L, LUA_MATHLIBNAME, math);

   // Register math interface prototypes for compile-time type inference
   reg_iface_prototype("math", "abs", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "floor", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "ceil", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "sqrt", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "log10", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "deg", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "rad", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "exp", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "sin", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "cos", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "tan", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "asin", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "acos", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "atan", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "sinh", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "cosh", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "tanh", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "frexp", { TiriType::Num, TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "modf", { TiriType::Num, TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "log", { TiriType::Num }, { TiriType::Num, TiriType::Num }, FProtoFlags::None,
      FProtoArity::required(1));
   reg_iface_prototype("math", "atan2", { TiriType::Num }, { TiriType::Num, TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "ldexp", { TiriType::Num }, { TiriType::Num, TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "min", { TiriType::Num }, { TiriType::Num }, FProtoFlags::Variadic,
      FProtoArity::exact());
   reg_iface_prototype("math", "max", { TiriType::Num }, { TiriType::Num }, FProtoFlags::Variadic,
      FProtoArity::exact());
   reg_iface_prototype("math", "clamp", { TiriType::Num }, { TiriType::Num, TiriType::Num, TiriType::Num },
      FProtoFlags::None, FProtoArity::exact());
   reg_iface_prototype("math", "round", { TiriType::Num }, { TiriType::Num }, FProtoFlags::None,
      FProtoArity::exact());
   reg_iface_prototype("math", "random", { TiriType::Num }, { TiriType::Num, TiriType::Num }, FProtoFlags::None,
      FProtoArity::required(0));
   reg_iface_prototype("math", "randomSeed", {}, { TiriType::Num }, FProtoFlags::None, FProtoArity::exact());

   return 1;
}
