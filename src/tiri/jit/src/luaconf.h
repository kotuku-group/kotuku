// Configuration header.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#ifndef WINVER
#define WINVER 0x0501
#endif
#include <limits.h>
#include <stddef.h>
#include <cstdio>

#define IS ==

// IntelliSense support for unity build files (parser/*.cpp).
// Requires .vscode/c_cpp_properties.json with forcedInclude set to this file.
#ifdef __INTELLISENSE__
#define LUA_CORE
#include "runtime/lj_obj.h"
#include "runtime/lj_gc.h"
#include "runtime/lj_err.h"
#include "runtime/lj_debug.h"
#include "runtime/filesource.h"
#include "runtime/lj_buf.h"
#include "runtime/lj_str.h"
#include "runtime/lj_tab.h"
#include "runtime/lj_func.h"
#include "runtime/lj_state.h"
#include "runtime/lj_bc.h"
#include "runtime/lj_strfmt.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "runtime/lj_vm.h"
#include "runtime/lj_vmevent.h"
#include "parser/dump_bytecode.h"
#include "parser/token_types.h"
#include "parser/parse_types.h"
#include "parser/parse_internal.h"
#include "parser/parser_profiler.h"
#include "parser/value_categories.h"
#endif

// Quoting in error messages.
#define LUA_QL(x)   "'" x "'"
#define LUA_QS      LUA_QL("%s")

// Various tunables.
constexpr int LUAI_MAXSTACK = 65500;   //  Max. # of stack slots for a thread (<64K).
constexpr int LUAI_MAXCSTACK = 8000;   //  Max. # of stack slots for a C func (<10K).
constexpr int LUAI_GCPAUSE = 200;      //  Pause GC until memory is at 200%.
constexpr int LUAI_GCMUL = 200;        //  Run GC at 200% of allocation speed.
constexpr int LUA_MAXCAPTURES = 32;    //  Max. pattern captures.

// Note: changing the following defines breaks the Lua 5.1 ABI.
#define LUA_INTEGER   ptrdiff_t
constexpr int LUA_IDSIZE = 60;   //  Size of lua_Debug.short_src.

// Size of lauxlib and io.* on-stack buffers. Weird workaround to avoid using
// unreasonable amounts of stack space, but still retain ABI compatibility.
// Blame Lua for depending on BUFSIZ in the ABI, blame **** for wrecking it.

#define LUAL_BUFFERSIZE   (BUFSIZ > 16384 ? 8192 : BUFSIZ)

// The following defines are here only for compatibility with luaconf.h
// from the standard Lua distribution. They must not be changed for LuaJIT.

#define LUA_NUMBER_DOUBLE
#define LUA_NUMBER         double
#define LUAI_UACNUMBER     double
#define LUA_NUMBER_SCAN    "%lf"
#define LUA_NUMBER_FMT     "%.14g"

inline int lua_number2str(char *S, double N) {
   return sprintf(S, LUA_NUMBER_FMT, N);
}

constexpr int LUAI_MAXNUMBER2STR = 32;
#define LUA_INTFRMLEN      "l"
#define LUA_INTFRM_T      long

#define LUA_API      extern "C"
#define LUALIB_API   extern "C"

// Compatibility support for assertions.
#if defined(LUA_USE_ASSERT) || defined(LUA_USE_APICHECK)
#include <cassert>
#endif

#ifdef LUA_USE_ASSERT
#define lua_assert(x)      assert(x)
#endif

#ifdef LUA_USE_APICHECK
#define luai_apicheck(L, o)   do { (void)(L); assert(o); } while(0)
#else
#define luai_apicheck(L, o)   do { (void)(L); (void)(o); } while(0)
#endif
