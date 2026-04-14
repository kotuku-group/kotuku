/*
** Client for the GDB JIT API.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GDBJIT_H
#define _LJ_GDBJIT_H

#include "lj_obj.h"
#include "lj_jit.h"

#if defined(LUAJIT_USE_GDBJIT)

extern "C" void lj_gdbjit_addtrace(jit_State *J, GCtrace *T);
extern "C" void lj_gdbjit_deltrace(jit_State *J, GCtrace *T);

#else
inline void lj_gdbjit_addtrace([[maybe_unused]] jit_State *J, [[maybe_unused]] GCtrace *T) noexcept {}
inline void lj_gdbjit_deltrace([[maybe_unused]] jit_State *J, [[maybe_unused]] GCtrace *T) noexcept {}
#endif

#endif
