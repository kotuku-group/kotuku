/*
** C math library wrappers for C++ compatibility.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#include <math.h>

// C++ compatibility: provide unambiguous wrappers for overloaded math functions
extern "C" {
   double cmath_log10(double x) { return log10(x); }
   double deg(double x) { return x * 57.29577951308232; }
   double rad(double x) { return x * 0.017453292519943295; }
   double cmath_exp(double x) { return exp(x); }
   double cmath_sin(double x) { return sin(x); }
   double cmath_cos(double x) { return cos(x); }
   double cmath_tan(double x) { return tan(x); }
   double cmath_asin(double x) { return asin(x); }
   double cmath_acos(double x) { return acos(x); }
   double cmath_atan(double x) { return atan(x); }
   double cmath_sinh(double x) { return sinh(x); }
   double cmath_cosh(double x) { return cosh(x); }
   double cmath_tanh(double x) { return tanh(x); }
   double cmath_sqrt(double x) { return sqrt(x); }
   double cmath_log(double x) { return log(x); }
   double cmath_log2(double x) { return log2(x); }
   double cmath_atan2(double y, double x) { return atan2(y, x); }
   double cmath_ldexp(double x, int exp) { return ldexp(x, exp); }
}
