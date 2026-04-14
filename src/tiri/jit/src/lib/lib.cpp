// Library function support.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_lib_c
#define LUA_CORE

#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lj_buf.h"
#include "../parser/lexer.h"
#include "lj_bcdump.h"
#include "lib.h"
#include "runtime/lj_thunk.h"

//********************************************************************************************************************
// Library initialization

static GCtab * lib_create_table(lua_State *L, const char *libname, int hsize)
{
   if (libname) {
      luaL_findtable(L, LUA_REGISTRYINDEX, "_LOADED", 16);
      lua_getfield(L, -1, libname);
      if (!tvistab(L->top - 1)) {
         L->top--;
         if (luaL_findtable(L, LUA_GLOBALSINDEX, libname, hsize) != nullptr) lj_err_callerv(L, ErrMsg::BADMODN, libname);
         settabV(L, L->top, tabV(L->top - 1));
         L->top++;
         lua_setfield(L, -3, libname);  //  _LOADED[libname] = new table
      }
      L->top--;
      settabV(L, L->top - 1, tabV(L->top));
   }
   else lua_createtable(L, 0, hsize);

   return tabV(L->top - 1);
}

//********************************************************************************************************************

static const uint8_t * lib_read_lfunc(lua_State *L, const uint8_t* p, GCtab* tab)
{
   int len = *p++;
   GCstr *name = lj_str_new(L, (const char*)p, len);

   LexState ls(L, (const char*)(p + len), name); // Use specialized constructor for bytecode reading

   auto pt = lj_bcread_proto(&ls);
   pt->firstline = BCLine::BUILTIN;
   auto fn = lj_func_newL_empty(L, pt, tabref(L->env));

   // NOBARRIER: See below for common barrier.

   setfuncV(L, lj_tab_setstr(L, tab, name), fn);

   auto result = (const uint8_t *)ls.p;

   return result; // Destructor will automatically clean up
}

//********************************************************************************************************************

void lj_lib_register(lua_State *L, const char* libname, const uint8_t* p, const lua_CFunction* cf)
{
   GCtab *env = tabref(L->env);
   GCfunc *ofn = nullptr;
   int ffid = *p++;
   BCIns *bcff = &L2GG(L)->bcff[*p++];
   GCtab *tab = lib_create_table(L, libname, *p++);
   ptrdiff_t tpos = L->top - L->base;

   // Avoid barriers further down.
   lj_gc_anybarriert(L, tab);
   tab->nomm = 0;

   for (;;) {
      uint32_t tag = *p++;
      MSize len = tag & LIBINIT_LENMASK;
      tag &= LIBINIT_TAGMASK;
      if (tag != LIBINIT_STRING) {
         const char *name;
         auto nuv = (MSize)(L->top - L->base - tpos);
         GCfunc *fn = lj_func_newC(L, nuv, env);
         if (nuv) {
            L->top = L->base + tpos;
            memcpy(fn->c.upvalue, L->top, sizeof(TValue) * nuv);
         }

         fn->c.ffid = (uint8_t)(ffid++);
         name = (const char*)p;
         p += len;

         if (tag IS LIBINIT_CF) setmref(fn->c.pc, &G(L)->bc_cfunc_int);
         else setmref(fn->c.pc, bcff++);

         if (tag IS LIBINIT_ASM_) fn->c.f = ofn->c.f;  //  Copy handler from previous function.
         else fn->c.f = *cf++;  //  Get cf or handler from C function table.

         if (len) { // NOBARRIER: See above for common barrier.
            setfuncV(L, lj_tab_setstr(L, tab, lj_str_new(L, name, len)), fn);
         }
         ofn = fn;
      }
      else {
         switch (tag | len) {
            case LIBINIT_LUA:
               p = lib_read_lfunc(L, p, tab);
               break;
            case LIBINIT_SET:
               L->top -= 2;
               if (tvisstr(L->top + 1) and strV(L->top + 1)->len IS 0) env = tabV(L->top);
               else  //  NOBARRIER: See above for common barrier.
                  copyTV(L, lj_tab_set(L, tab, L->top + 1), L->top);
               break;
            case LIBINIT_NUMBER:
               memcpy(&L->top->n, p, sizeof(double));
               L->top++;
               p += sizeof(double);
               break;
            case LIBINIT_COPY:
               copyTV(L, L->top, L->top - *p++);
               L->top++;
               break;
            case LIBINIT_LASTCL:
               setfuncV(L, L->top++, ofn);
               break;
            case LIBINIT_FFID:
               ffid++;
               break;
            case LIBINIT_END:
               return;
            default:
               setstrV(L, L->top++, lj_str_new(L, (const char*)p, len));
               p += len;
               break;
         }
      }
   }
}

//********************************************************************************************************************
// Push internal function on the stack.

GCfunc * lj_lib_pushcc(lua_State *L, lua_CFunction f, int id, int n)
{
   GCfunc *fn;
   lua_pushcclosure(L, f, n);
   fn = funcV(L->top - 1);
   fn->c.ffid = (uint8_t)id;
   setmref(fn->c.pc, &G(L)->bc_cfunc_int);
   return fn;
}

//********************************************************************************************************************

void lj_lib_prereg(lua_State *L, const char* name, lua_CFunction f, GCtab* env)
{
   luaL_findtable(L, LUA_REGISTRYINDEX, "_PRELOAD", 4);
   lua_pushcfunction(L, f);
   // NOBARRIER: The function is new (marked white).
   setgcref(funcV(L->top - 1)->c.env, obj2gco(env));
   lua_setfield(L, -2, name);
   L->top--;
}

//********************************************************************************************************************

int lj_lib_postreg(lua_State *L, lua_CFunction cf, int id, const char* name)
{
   GCfunc *fn = lj_lib_pushcf(L, cf, id);
   GCtab *t = tabref(curr_func(L)->c.env);  //  Reference to parent table.
   setfuncV(L, lj_tab_setstr(L, t, lj_str_newz(L, name)), fn);
   lj_gc_anybarriert(L, t);
   setfuncV(L, L->top++, fn);
   return 1;
}

//********************************************************************************************************************
// Type checks

TValue * lj_lib_checkany(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (o >= L->top) lj_err_arg(L, Arg, ErrMsg::NOVAL);
   return o;
}

//********************************************************************************************************************

GCstr * lj_lib_checkstr(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (o < L->top) {
      if (lj_is_thunk(o)) { // Resolve thunk if present
         TValue *resolved = lj_thunk_resolve(L, udataV(o));
         o = L->base + Arg - 1; // Stack may have moved, recalculate o
         copyTV(L, o, resolved);
      }

      if (tvisstr(o)) [[likely]] return strV(o);
      else if (tvisnumber(o)) {
         GCstr* s = lj_strfmt_number(L, o);
         setstrV(L, o, s);
         return s;
      }
   }

   lj_err_argt(L, Arg, LUA_TSTRING);
   return nullptr;  //  unreachable
}

//********************************************************************************************************************

GCstr * lj_lib_optstr(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   return (o < L->top and !tvisnil(o)) ? lj_lib_checkstr(L, Arg) : nullptr;
}

//********************************************************************************************************************

#if LJ_DUALNUM
void lj_lib_checknumber(lua_State *L, int Arg)
{
   TValue* o = L->base + Arg - 1;
   if (!(o < L->top and lj_strscan_numberobj(o)))
      lj_err_argt(L, Arg, LUA_TNUMBER);
}
#endif

//********************************************************************************************************************

lua_Number lj_lib_checknum(lua_State *L, int Arg)
{
   TValue* o = L->base + Arg - 1;
   if (o < L->top) {
      if (lj_is_thunk(o)) { // Resolve thunk if present
         TValue *resolved = lj_thunk_resolve(L, udataV(o));
         o = L->base + Arg - 1;  // Stack may have moved, recalculate o
         copyTV(L, o, resolved);
      }
   }

   if (!(o < L->top and (tvisnumber(o) or (tvisstr(o) and lj_strscan_num(strV(o), o))))) {
      lj_err_argt(L, Arg, LUA_TNUMBER);
   }

   if (tvisint(o)) {
      auto n = (lua_Number)intV(o);
      setnumV(o, n);
      return n;
   }
   else return numV(o);
}

//********************************************************************************************************************

int32_t lj_lib_checkint(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (!(o < L->top and lj_strscan_numberobj(o))) lj_err_argt(L, Arg, LUA_TNUMBER);
   if (tvisint(o)) return intV(o);
   else {
      int32_t i = lj_num2int(numV(o));
      if (LJ_DUALNUM) setintV(o, i);
      return i;
   }
}

//********************************************************************************************************************

int32_t lj_lib_optint(lua_State *L, int Arg, int32_t Default)
{
   TValue *o = L->base + Arg - 1;
   return (o < L->top and !tvisnil(o)) ? lj_lib_checkint(L, Arg) : Default;
}

//********************************************************************************************************************

GCfunc * lj_lib_checkfunc(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (!(o < L->top and tvisfunc(o))) lj_err_argt(L, Arg, LUA_TFUNCTION);
   return funcV(o);
}

//********************************************************************************************************************

GCtab * lj_lib_checktab(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (!(o < L->top and tvistab(o))) lj_err_argt(L, Arg, LUA_TTABLE);
   return tabV(o);
}

//********************************************************************************************************************
// Helper function to check argument is an object (nil not accepted)

GCobject * lj_lib_checkobject(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (o < L->top) {
      if (lj_is_thunk(o)) { // Resolve thunk if present
         TValue *resolved = lj_thunk_resolve(L, udataV(o));
         o = L->base + Arg - 1; // Stack may have moved, recalculate o
         copyTV(L, o, resolved); // Replace thunk with resolved value
      }

      if (tvisobject(o)) [[likely]] return objectV(o);
   }

   lj_err_argt(L, Arg, LUA_TOBJECT);
   return nullptr; // unreachable
}

//********************************************************************************************************************
// Helper function to check optional object argument (may be nil, but non-objects throw an error)

GCobject * lj_lib_optobject(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (o < L->top and not tvisnil(o)) {
      if (lj_is_thunk(o)) { // Resolve thunk if present
         TValue *resolved = lj_thunk_resolve(L, udataV(o));
         o = L->base + Arg - 1; // Stack may have moved, recalculate o
         copyTV(L, o, resolved); // Replace thunk with resolved value
      }

      if (tvisobject(o)) return objectV(o);
      else lj_err_argt(L, Arg, LUA_TOBJECT);
   }
   return nullptr;
}

//********************************************************************************************************************
// Helper function to check argument is an array

GCarray * lj_lib_checkarray(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (o < L->top) {
      if (lj_is_thunk(o)) { // Resolve thunk if present
         TValue *resolved = lj_thunk_resolve(L, udataV(o));
         o = L->base + Arg - 1; // Stack may have moved, recalculate o
         copyTV(L, o, resolved); // Replace thunk with resolved value
      }

      if (tvisarray(o)) [[likely]] return arrayV(o);
   }

   lj_err_argt(L, Arg, LUA_TARRAY);
   return nullptr;  //  unreachable
}

//********************************************************************************************************************
// Helper function to check optional array argument (may be nil, but non-arrays throw an error)

GCarray * lj_lib_optarray(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (o < L->top and not tvisnil(o)) {
      if (lj_is_thunk(o)) { // Resolve thunk if present
         TValue *resolved = lj_thunk_resolve(L, udataV(o));
         o = L->base + Arg - 1; // Stack may have moved, recalculate o
         copyTV(L, o, resolved); // Replace thunk with resolved value
      }

      if (tvisarray(o)) return arrayV(o);
      else lj_err_argt(L, Arg, LUA_TARRAY);
   }
   return nullptr;
}

//********************************************************************************************************************

GCtab * lj_lib_checktabornil(lua_State *L, int Arg)
{
   TValue *o = L->base + Arg - 1;
   if (o < L->top) {
      if (lj_is_thunk(o)) { // Resolve thunk if present
         TValue *resolved = lj_thunk_resolve(L, udataV(o));
         o = L->base + Arg - 1; // Stack may have moved, recalculate o
         copyTV(L, o, resolved); // Replace thunk with resolved value
      }

      if (tvistab(o)) return tabV(o);
      else if (tvisnil(o)) return nullptr;
   }
   lj_err_arg(L, Arg, ErrMsg::NOTABN);
   return nullptr;  //  unreachable
}

//********************************************************************************************************************

int lj_lib_checkopt(lua_State *L, int Arg, int def, const char* lst)
{
   GCstr *s = def >= 0 ? lj_lib_optstr(L, Arg) : lj_lib_checkstr(L, Arg);
   if (s) {
      CSTRING opt = strdata(s);
      auto len = s->len;
      for (int i = 0; *(const uint8_t*)lst; i++) {
         if (*(const uint8_t*)lst IS len and memcmp(opt, lst + 1, len) IS 0) return i;
         lst += 1 + *(const uint8_t*)lst;
      }
      lj_err_argv(L, Arg, ErrMsg::INVOPTM, opt);
   }
   return def;
}
