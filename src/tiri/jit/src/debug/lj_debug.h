/*
** Debugging and introspection.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lj_obj.h"

struct lj_Debug {
   // Common fields. Must be in the same order as in lua.h.
   int event;
   const char* name;
   const char* namewhat;
   const char* what;
   const char* source;
   int currentline;
   int nups;
   int linedefined;
   int lastlinedefined;
   char short_src[LUA_IDSIZE];
   int i_ci;
   // Extended fields. Only valid if lj_debug_getinfo() is called with ext = 1.
   int nparams;
   int isvararg;
};

extern cTValue* lj_debug_frame(lua_State* L, int level, int* size);
extern BCLine lj_debug_line(GCproto* pt, BCPOS pc);
extern const char* lj_debug_uvname(GCproto* pt, uint32_t idx);
extern const char* lj_debug_uvnamev(cTValue* o, uint32_t idx, TValue** tvp, GCobj** op);
extern const char* lj_debug_slotname(GCproto* pt, const BCIns* pc, BCREG slot, const char** name);
extern const char* lj_debug_funcname(lua_State* L, cTValue* frame, const char** name);
extern void lj_debug_shortname(char* out, GCstr* str, BCLine line);
extern void lj_debug_addloc(lua_State* L, const char* msg, cTValue* frame, cTValue* nextframe);
extern void lj_debug_pushloc(lua_State* L, GCproto* pt, BCPOS pc);
extern int lj_debug_getinfo(lua_State* L, const char* what, lj_Debug* ar, int ext);

// Fixed internal variable names.
#define VARNAMEDEF(_) \
  _(FOR_IDX, "(for index)") \
  _(FOR_STOP, "(for limit)") \
  _(FOR_STEP, "(for step)") \
  _(FOR_GEN, "(for generator)") \
  _(FOR_STATE, "(for state)") \
  _(FOR_CTL, "(for control)")

enum {
   VARNAME_END,
#define VARNAMEENUM(name, str)   VARNAME_##name,
VARNAMEDEF(VARNAMEENUM)
#undef VARNAMEENUM
   VARNAME__MAX
};

//********************************************************************************************************************
// Stack trace capture for try<trace>

inline constexpr int LJ_MAX_TRACE_FRAMES = 32;

struct CapturedFrame {
   GCstr *source;      // Source file name (may be nullptr)
   GCstr *funcname;    // Function name (may be nullptr)
   BCLine line;        // Line number (0 if unknown)
};

struct CapturedStackTrace {
   CapturedFrame frames[LJ_MAX_TRACE_FRAMES];
   uint16_t frame_count;
};

LJ_FUNC CapturedStackTrace* lj_debug_capture_trace(lua_State *L, int skip_levels);
LJ_FUNC void lj_debug_free_trace(lua_State *L, CapturedStackTrace *trace);
