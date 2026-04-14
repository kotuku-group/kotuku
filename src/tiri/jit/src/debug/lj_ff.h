// Fast function IDs.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

// Fast function ID.

enum class FastFunc : unsigned int {
   LUA_ = FF_LUA,   //  Lua function (must be 0) - see lj_obj.h
   C_ = FF_C,       //  Regular C function (must be 1) - see lj_obj.h
#define FFDEF(name)   name,
#include "lj_ffdef.h"
   _MAX
};

// Backward compatibility aliases
inline constexpr unsigned int FF_LUA_ = unsigned(FastFunc::LUA_);
inline constexpr unsigned int FF_C_ = unsigned(FastFunc::C_);
#define FFDEF(name)   inline constexpr unsigned int FF_##name = unsigned(FastFunc::name);
#include "lj_ffdef.h"
inline constexpr unsigned int FF__MAX = unsigned(FastFunc::_MAX);
