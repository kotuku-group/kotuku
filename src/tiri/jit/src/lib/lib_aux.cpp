// Auxiliary library for the Lua/C API.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Major parts taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#define lib_aux_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_state.h"
#include "lj_trace.h"
#include "lib.h"

#if LJ_TARGET_POSIX
#include <sys/wait.h>
#endif

//********************************************************************************************************************
// Traverses a dot-separated path (e.g., "foo.bar.baz") in a table hierarchy, creating intermediate tables as needed.
// Returns nullptr on success, or a pointer to the problematic part of the path if a non-table value is encountered.
//
// Parameters:
//   L:      Lua state
//   idx:    Stack index of the root table to search from
//   fname:  Dot-separated field path (e.g., "package.loaded")
//   szhint: Size hint for creating the final table (ignored for intermediate tables)
//
// On success: Leaves the final table on the stack and returns nullptr.
// On failure: Pops intermediate values and returns pointer to the conflicting path segment.

extern CSTRING luaL_findtable(lua_State* L, int idx, CSTRING fname, int szhint)
{
   CSTRING e;
   lua_pushvalue(L, idx);
   do {
      e = strchr(fname, '.');
      if (e == nullptr) e = fname + strlen(fname);
      lua_pushlstring(L, fname, (size_t)(e - fname));
      lua_rawget(L, -2);
      if (lua_isnil(L, -1)) {  // no such field?
         lua_pop(L, 1);  //  remove this nil
         lua_createtable(L, 0, (*e == '.' ? 1 : szhint)); //  new table for field
         lua_pushlstring(L, fname, (size_t)(e - fname));
         lua_pushvalue(L, -2);
         lua_settable(L, -4);  //  set new table into field
      }
      else if (!lua_istable(L, -1)) {  // field has a non-table value?
         lua_pop(L, 2);  //  remove table and value
         return fname;  //  return problematic part of the name
      }
      lua_remove(L, -2);  //  remove previous table
      fname = e + 1;
   } while (*e == '.');
   return nullptr;
}

//********************************************************************************************************************

static int libsize(const luaL_Reg* l)
{
   int size = 0;
   for (; l and l->name; l++) size++;
   return size;
}

//********************************************************************************************************************
// Pushes a module table onto the stack.
//
// Parameters:
//   L:          Lua state
//   modname:    Name of the module to push
//   sizehint:   Size hint for creating the module table (ignored for intermediate tables)

extern void luaL_pushmodule(lua_State* L, CSTRING modname, int sizehint)
{
   luaL_findtable(L, LUA_REGISTRYINDEX, "_LOADED", 16);
   lua_getfield(L, -1, modname);
   if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      if (luaL_findtable(L, LUA_GLOBALSINDEX, modname, sizehint) != nullptr) lj_err_callerv(L, ErrMsg::BADMODN, modname);
      lua_pushvalue(L, -1);
      lua_setfield(L, -3, modname);  //  _LOADED[modname] = new table.
   }
   lua_remove(L, -2);  //  Remove _LOADED table.
}

//********************************************************************************************************************
// Opens a library table and sets its functions.

extern void luaL_openlib(lua_State* L, CSTRING libname, const luaL_Reg* l, int nup)
{
   lj_lib_checkfpu(L);
   if (libname) {
      luaL_pushmodule(L, libname, libsize(l));
      lua_insert(L, -(nup + 1));  //  Move module table below upvalues.
   }

   if (l) luaL_setfuncs(L, l, nup);
   else lua_pop(L, nup);  //  Remove upvalues.
}

//********************************************************************************************************************

extern void luaL_register(lua_State* L, CSTRING libname, const luaL_Reg* l)
{
   luaL_openlib(L, libname, l, 0);
}

//********************************************************************************************************************

extern void luaL_setfuncs(lua_State* L, const luaL_Reg* l, int nup)
{
   luaL_checkstack(L, nup, "too many upvalues");
   for (; l->name; l++) {
      int i;
      for (i = 0; i < nup; i++)  //  Copy upvalues to the top.
         lua_pushvalue(L, -nup);
      lua_pushcclosure(L, l->func, nup);
      lua_setfield(L, -(nup + 2), l->name);
   }
   lua_pop(L, nup);  //  Remove upvalues.
}

//********************************************************************************************************************

extern CSTRING luaL_gsub(lua_State* L, CSTRING s, CSTRING p, CSTRING r)
{
   CSTRING wild;
   size_t l = strlen(p);
   luaL_Buffer b;
   luaL_buffinit(L, &b);
   while ((wild = strstr(s, p)) != nullptr) {
      luaL_addlstring(&b, s, (size_t)(wild - s));  //  push prefix
      luaL_addstring(&b, r);  //  push replacement in place of pattern
      s = wild + l;  //  continue after `p'
   }
   luaL_addstring(&b, s);  //  push last suffix
   luaL_pushresult(&b);
   return lua_tostring(L, -1);
}

//********************************************************************************************************************
// Buffer handling

#define bufflen(B)   ((size_t)((B)->p - (B)->buffer))
#define bufffree(B)   ((size_t)(LUAL_BUFFERSIZE - bufflen(B)))

static int emptybuffer(luaL_Buffer* B)
{
   size_t l = bufflen(B);
   if (l == 0)
      return 0;  //  put nothing on stack
   lua_pushlstring(B->L, B->buffer, l);
   B->p = B->buffer;
   B->lvl++;
   return 1;
}

//********************************************************************************************************************

static void adjuststack(luaL_Buffer* B)
{
   if (B->lvl > 1) {
      lua_State* L = B->L;
      int toget = 1;  //  number of levels to concat
      size_t toplen = lua_strlen(L, -1);
      do {
         size_t l = lua_strlen(L, -(toget + 1));
         if (!(B->lvl - toget + 1 >= LUA_MINSTACK / 2 or toplen > l))
            break;
         toplen += l;
         toget++;
      } while (toget < B->lvl);
      lua_concat(L, toget);
      B->lvl = B->lvl - toget + 1;
   }
}

//********************************************************************************************************************

extern char* luaL_prepbuffer(luaL_Buffer* B)
{
   if (emptybuffer(B)) adjuststack(B);
   return B->buffer;
}

//********************************************************************************************************************

extern void luaL_addlstring(luaL_Buffer* B, CSTRING s, size_t l)
{
   if (l <= bufffree(B)) {
      memcpy(B->p, s, l);
      B->p += l;
   }
   else {
      emptybuffer(B);
      lua_pushlstring(B->L, s, l);
      B->lvl++;
      adjuststack(B);
   }
}

//********************************************************************************************************************

extern void luaL_addstring(luaL_Buffer* B, CSTRING s)
{
   luaL_addlstring(B, s, strlen(s));
}

//********************************************************************************************************************

extern void luaL_pushresult(luaL_Buffer* B)
{
   emptybuffer(B);
   lua_concat(B->L, B->lvl);
   B->lvl = 1;
}

//********************************************************************************************************************

extern void luaL_addvalue(luaL_Buffer* B)
{
   lua_State* L = B->L;
   size_t vl;
   CSTRING s = lua_tolstring(L, -1, &vl);
   if (vl <= bufffree(B)) {  // fit into buffer?
      memcpy(B->p, s, vl);  //  put it there
      B->p += vl;
      lua_pop(L, 1);  //  remove from stack
   }
   else {
      if (emptybuffer(B)) lua_insert(L, -2);  //  put buffer before new value
      B->lvl++;  //  add new value into B stack
      adjuststack(B);
   }
}

//********************************************************************************************************************

extern void luaL_buffinit(lua_State* L, luaL_Buffer* B)
{
   B->L = L;
   B->p = B->buffer;
   B->lvl = 0;
}

//********************************************************************************************************************
// Reference management

#define FREELIST_REF   0

//********************************************************************************************************************
// Convert a stack index to an absolute index.

#define abs_index(L, i) ((i) > 0 or (i) <= LUA_REGISTRYINDEX ? (i) : lua_gettop(L) + (i) + 1)

extern int luaL_ref(lua_State* L, int t)
{
   int ref;
   t = abs_index(L, t);
   if (lua_isnil(L, -1)) {
      lua_pop(L, 1);  //  remove from stack
      return LUA_REFNIL;  //  `nil' has a unique fixed reference
   }
   lua_rawgeti(L, t, FREELIST_REF);  //  get first free element
   ref = (int)lua_tointeger(L, -1);  //  ref = t[FREELIST_REF]
   lua_pop(L, 1);  //  remove it from stack
   if (ref != 0) {  // any free element?
      lua_rawgeti(L, t, ref);  //  remove it from list
      lua_rawseti(L, t, FREELIST_REF);  //  (t[FREELIST_REF] = t[ref])
   }
   else {  // no free elements
      // 0-based: objlen returns count (e.g., 3 means indices 0,1,2 used)
      // Next free index is simply the count itself (not count+1 as in 1-based)
      ref = (int)lua_objlen(L, t);
   }
   lua_rawseti(L, t, ref);
   return ref;
}

//********************************************************************************************************************

extern void luaL_unref(lua_State* L, int t, int ref)
{
   if (ref >= 0) {
      t = abs_index(L, t);
      lua_rawgeti(L, t, FREELIST_REF);
      lua_rawseti(L, t, ref);  //  t[ref] = t[FREELIST_REF]
      lua_pushinteger(L, ref);
      lua_rawseti(L, t, FREELIST_REF);  //  t[FREELIST_REF] = ref
   }
}

//********************************************************************************************************************
// Default allocator and panic function

static int panic(lua_State* L)
{
   CSTRING s = lua_tostring(L, -1);
   fputs("PANIC: unprotected error in call to Lua API (", stderr);
   fputs(s ? s : "?", stderr);
   fputc(')', stderr); fputc('\n', stderr);
   fflush(stderr);
   return 0;
}

#ifdef LUAJIT_USE_SYSMALLOC

static void* mem_alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
   (void)ud;
   (void)osize;
   if (nsize == 0) {
      free(ptr);
      return nullptr;
   }
   else {
      return realloc(ptr, nsize);
   }
}

extern lua_State* luaL_newstate(void)
{
   lua_State* L = lua_newstate(mem_alloc, nullptr);
   if (L) G(L)->panic = panic;
   return L;
}

#else

extern lua_State* luaL_newstate(class objScript *Script)
{
   lua_State* L;
   L = lua_newstate(LJ_ALLOCF_INTERNAL, nullptr);
   L->script = Script;
   if (L) G(L)->panic = panic;
   return L;
}

#endif
