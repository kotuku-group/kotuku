// Public Lua/C API.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Major portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#define lj_api_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_object.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_frame.h"
#include "lj_trace.h"
#include "lj_vm.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lib/lib_utils.h"
#include "lj_array.h"
#include "runtime/lj_thunk.h"
#include "runtime/stack_helpers.h"

#define lj_checkapi_slot(idx) lj_checkapi((idx) <= (L->top - L->base), "stack slot %d out of range", (idx))

//********************************************************************************************************************
// Stack index to address conversion

static TValue * index2adr(lua_State *L, int idx)
{
   if (idx > 0) {
      TValue *o = L->base + (idx - 1);
      return o < L->top ? o : niltv(L);
   }
   else if (idx > LUA_REGISTRYINDEX) {
      lj_checkapi(idx != 0 and -idx <= L->top - L->base, "bad stack slot %d", idx);
      return L->top + idx;
   }
   else if (idx IS LUA_GLOBALSINDEX) {
      TValue *o = &G(L)->tmptv;
      settabV(L, o, tabref(L->env));
      return o;
   }
   else if (idx IS LUA_REGISTRYINDEX) {
      return registry(L);
   }
   else {
      GCfunc *fn = curr_func(L);
      lj_checkapi(fn->c.gct IS ~LJ_TFUNC and !isluafunc(fn), "calling frame is not a C function");
      if (idx IS LUA_ENVIRONINDEX) {
         TValue *o = &G(L)->tmptv;
         settabV(L, o, tabref(fn->c.env));
         return o;
      }
      else {
         idx = LUA_GLOBALSINDEX - idx;
         return idx <= fn->c.nupvalues ? &fn->c.upvalue[idx - 1] : niltv(L);
      }
   }
}

//********************************************************************************************************************
// Checked index to address conversion

static LJ_AINLINE TValue * index2adr_check(lua_State *L, int idx)
{
   TValue *o = index2adr(L, idx);
   lj_checkapi(o != niltv(L), "invalid stack slot %d", idx);
   return o;
}

//********************************************************************************************************************
// Stack index to address conversion for stack slots only

static TValue * index2adr_stack(lua_State *L, int idx)
{
   if (idx > 0) {
      TValue * o = L->base + (idx - 1);
      if (o < L->top) return o;
      else {
         lj_checkapi(0, "invalid stack slot %d", idx);
         return niltv(L);
      }
      return o < L->top ? o : niltv(L);
   }
   else {
      lj_checkapi(idx != 0 and -idx <= L->top - L->base, "invalid stack slot %d", idx);
      return L->top + idx;
   }
}

//********************************************************************************************************************
// Get current environment table

static GCtab * getcurrenv(lua_State *L)
{
   GCfunc *fn = curr_func(L);
   return fn->c.gct IS ~LJ_TFUNC ? tabref(fn->c.env) : tabref(L->env);
}

//********************************************************************************************************************
// Index resolution helpers
//
// For use in resolve stack indexes that may contain a thunk. This ensures that when C code calls lua_tostring,
// lua_tonumber, etc., it receives the resolved value rather than the thunk userdata.

extern TValue * resolve_index(lua_State *L, int idx)
{
   // For positive indices, check if slot exists before accessing

   if (idx > 0) {
      TValue *o = L->base + (idx - 1);
      if (o >= L->top) return const_cast<TValue*>(niltv(L));  // Slot doesn't exist, return nil
   }

   TValue *o = index2adr_stack(L, idx);
   if (o and lj_is_thunk(o) and not L->resolving_thunk) {
      GCudata *ud = udataV(o);
      ThunkPayload *payload = thunk_payload(ud);

      // If already resolved, just return the cached value pointer

      if (payload->resolved) return &payload->cached_value;

      ptrdiff_t slot_offset = savestack(L, o); // Track slot position (may move during resolution)

      // Set flag to prevent infinite recursion

      L->resolving_thunk = 1;
      TValue *result = lj_thunk_resolve(L, ud);
      L->resolving_thunk = 0;

      o = restorestack(L, slot_offset); // Restore slot pointer (stack may have been reallocated)

      // If resolution failed (e.g., error in thunk function), return the original slot
      // which still contains the thunk userdata - let caller handle the error

      if (not result) return o;

      copyTV(L, o, result); // Copy resolved value to stack slot for consistency
      return o;
   }
   return o;
}

// Const variant for read-only access - resolves but returns const pointer

static cTValue * resolve_index_const(lua_State *L, int idx)
{
   if (idx <= LUA_REGISTRYINDEX) return index2adr(L, idx);  // Pseudo-indices can't be thunks

   // For positive indices, check if slot exists before attempting resolution
   if (idx > 0) {
      TValue *o = L->base + (idx - 1);
      if (o >= L->top) return niltv(L);  // Slot doesn't exist, return nil
   }
   return resolve_index(L, idx);
}

//********************************************************************************************************************
// Check if stack can accommodate additional space

extern int lua_checkstack(lua_State *L, int size)
{
   if (size > LUAI_MAXCSTACK or (L->top - L->base + size) > LUAI_MAXCSTACK) {
      return 0;  //  Stack overflow.
   }
   else if (size > 0) lj_state_checkstack(L, (MSize)size);
   return 1;
}

//********************************************************************************************************************
// Check stack availability with error message

extern void luaL_checkstack(lua_State *L, int size, CSTRING msg)
{
   if (!lua_checkstack(L, size)) lj_err_callerv(L, ErrMsg::STKOVM, msg);
}

//********************************************************************************************************************
// Transfer values between Lua states

extern void lua_xmove(lua_State *L, lua_State* to, int n)
{
   if (L IS to) return;
   lj_checkapi_slot(n);
   lj_checkapi(G(L) IS G(to), "move across global states");
   lj_state_checkstack(to, (MSize)n);
   copy_range(to, to->top, L->top - n, n);
   L->top -= n;
   to->top += n;
}

//********************************************************************************************************************
// Stack manipulation

extern int lua_gettop(lua_State *L)
{
   return (int)(L->top - L->base);
}

//********************************************************************************************************************
// Set stack top position

extern void lua_settop(lua_State *L, int idx)
{
   if (idx >= 0) {
      lj_checkapi(idx <= tvref(L->maxstack) - L->base, "bad stack slot %d", idx);
      if (L->base + idx > L->top) {
         if (L->base + idx >= tvref(L->maxstack)) lj_state_growstack(L, (MSize)idx - (MSize)(L->top - L->base));
         size_t count = (L->base + idx) - L->top;
         set_range_nil(L->top, count);
         L->top += count;
      }
      else L->top = L->base + idx;
   }
   else {
      lj_checkapi(-(idx + 1) <= (L->top - L->base), "bad stack slot %d", idx);
      L->top += idx + 1;  //  Shrinks top (idx < 0).
   }
}

//********************************************************************************************************************
// Remove value at stack index

extern void lua_remove(lua_State *L, int idx)
{
   TValue *p = index2adr_stack(L, idx);
   while (++p < L->top) copyTV(L, p - 1, p);
   L->top--;
}

//********************************************************************************************************************
// Insert value at stack index

extern void lua_insert(lua_State *L, int idx)
{
   TValue *q, *p = index2adr_stack(L, idx);
   for (q = L->top; q > p; q--) copyTV(L, q, q - 1);
   copyTV(L, p, L->top);
}

//********************************************************************************************************************
// Rotate stack elements between idx and the top by n positions.
// A positive n rotates towards the top (rightward); a negative n rotates towards the bottom (leftward).
// For example, lua_rotate(L, 1, -4) moves elements at indices 1..4 to the top and shifts the rest down.

extern void lua_rotate(lua_State *L, int idx, int n)
{
   TValue *p = index2adr_stack(L, idx);
   TValue *t = L->top - 1;
   int count = int(t - p) + 1;
   lj_checkapi(count > 0, "empty stack segment for rotate");

   // Normalise n into 0..count-1

   n = ((n % count) + count) % count;
   if (n IS 0) return;

   // Three-reverse algorithm: reverse [p..t], reverse [p..p+n-1], reverse [p+n..t]

   TValue tmp;
   auto reverse = [&](TValue *lo, TValue *hi) {
      for (; lo < hi; lo++, hi--) {
         copyTV(L, &tmp, lo);
         copyTV(L, lo, hi);
         copyTV(L, hi, &tmp);
      }
   };

   reverse(p, t);
   reverse(p, p + n - 1);
   reverse(p + n, t);
}

//********************************************************************************************************************
// Copy value to stack slot with environment handling

static void copy_slot(lua_State *L, TValue * f, int idx)
{
   if (idx IS LUA_GLOBALSINDEX) {
      lj_checkapi(tvistab(f), "stack slot %d is not a table", idx);
      // NOBARRIER: A thread (i.e. L) is never black.
      setgcref(L->env, obj2gco(tabV(f)));
   }
   else if (idx IS LUA_ENVIRONINDEX) {
      GCfunc* fn = curr_func(L);
      if (fn->c.gct != ~LJ_TFUNC) lj_err_msg(L, ErrMsg::NOENV);
      lj_checkapi(tvistab(f), "stack slot %d is not a table", idx);
      setgcref(fn->c.env, obj2gco(tabV(f)));
      lj_gc_barrier(L, fn, f);
   }
   else {
      TValue * o = index2adr_check(L, idx);
      copyTV(L, o, f);
      if (idx < LUA_GLOBALSINDEX)  //  Need a barrier for upvalues.
         lj_gc_barrier(L, curr_func(L), f);
   }
}

//********************************************************************************************************************
// Replace value at stack index

extern void lua_replace(lua_State *L, int idx)
{
   lj_checkapi_slot(1);
   copy_slot(L, L->top - 1, idx);
   L->top--;
}

//********************************************************************************************************************
// Copy value from one stack index to another

extern void lua_copy(lua_State *L, int fromidx, int toidx)
{
   copy_slot(L, index2adr(L, fromidx), toidx);
}

//********************************************************************************************************************
// Push copy of value at stack index

extern void lua_pushvalue(lua_State *L, int idx)
{
   copyTV(L, L->top, index2adr(L, idx));
   incr_top(L);
}

//********************************************************************************************************************
// Stack getters

extern int lua_type(lua_State *L, int idx)
{
   cTValue* o = index2adr(L, idx);
   if (tvisnumber(o)) return LUA_TNUMBER;
   else if (o IS niltv(L)) return LUA_TNONE;
   else {  // Magic internal/external tag conversion. ORDER LJ_T
      uint32_t t = ~itype(o);
      // Lookup table: position 13 = LUA_TARRAY (11)
      int tt = (int)((U64x(b75a06, 98042110) >> 4 * t) & 15u);
      lj_assertL(tt != LUA_TNIL or tvisnil(o), "bad tag conversion");
      return tt;
   }
}

//********************************************************************************************************************
// Check value type at stack index

extern void luaL_checktype(lua_State *L, int idx, int tt)
{
   if (lua_type(L, idx) != tt) lj_err_argt(L, idx, tt);
}

//********************************************************************************************************************
// Check that stack slot contains a value

extern void luaL_checkany(lua_State *L, int idx)
{
   if (index2adr(L, idx) IS niltv(L)) lj_err_arg(L, idx, ErrMsg::NOVAL);
}

//********************************************************************************************************************
// Get string representation of type

extern CSTRING lua_typename(lua_State *L, int t)
{
   return lj_obj_typename[t + 1];
}

//********************************************************************************************************************
// Test if value is a C function

extern int lua_iscfunction(lua_State *L, int idx)
{
   cTValue* o = index2adr(L, idx);
   return tvisfunc(o) and !isluafunc(funcV(o));
}

//********************************************************************************************************************
// Test if value is a number or numeric string

extern int lua_isnumber(lua_State *L, int idx)
{
   cTValue *o = index2adr(L, idx);
   TValue tmp;
   return (tvisnumber(o) or (tvisstr(o) and lj_strscan_number(strV(o), &tmp)));
}

//********************************************************************************************************************
// Test if value is a string or number

extern int lua_isstring(lua_State *L, int idx)
{
   cTValue *o = index2adr(L, idx);
   return (tvisstr(o) or tvisnumber(o));
}

//********************************************************************************************************************
// Test if value is userdata or light userdata

extern int lua_isuserdata(lua_State *L, int idx)
{
   cTValue *o = index2adr(L, idx);
   return (tvisudata(o) or tvislightud(o));
}

//********************************************************************************************************************
// Test raw equality without metamethods

extern int lua_rawequal(lua_State *L, int idx1, int idx2)
{
   cTValue *o1 = index2adr(L, idx1);
   cTValue *o2 = index2adr(L, idx2);
   return (o1 IS niltv(L) or o2 IS niltv(L)) ? 0 : lj_obj_equal(o1, o2);
}

//********************************************************************************************************************
// Test equality with metamethods

extern int lua_equal(lua_State *L, int idx1, int idx2)
{
   cTValue *o1 = index2adr(L, idx1);
   cTValue *o2 = index2adr(L, idx2);

   if (tvisint(o1) and tvisint(o2)) return intV(o1) IS intV(o2);
   else if (tvisnumber(o1) and tvisnumber(o2)) return numberVnum(o1) IS numberVnum(o2);
   else if (itype(o1) != itype(o2)) return 0;
   else if (tvispri(o1)) return o1 != niltv(L) && o2 != niltv(L);
   else if (gcrefeq(o1->gcr, o2->gcr)) return 1;
   else if (!tvistabud(o1)) return 0;
   else {
      TValue *base = lj_meta_equal(L, gcV(o1), gcV(o2), 0);
      if ((uintptr_t)base <= 1) return (int)(uintptr_t)base;
      return tvistruecond(MetaCall::invoke(L, base, 2, 1));
   }
}

//********************************************************************************************************************
// Test less-than comparison

extern int lua_lessthan(lua_State *L, int idx1, int idx2)
{
   cTValue *o1 = index2adr(L, idx1);
   cTValue *o2 = index2adr(L, idx2);

   if (o1 IS niltv(L) or o2 IS niltv(L)) return 0;
   else if (tvisint(o1) and tvisint(o2)) return intV(o1) < intV(o2);
   else if (tvisnumber(o1) and tvisnumber(o2)) return numberVnum(o1) < numberVnum(o2);
   else {
      TValue *base = lj_meta_comp(L, o1, o2, 0);
      if ((uintptr_t)base <= 1) return (int)(uintptr_t)base;
      return tvistruecond(MetaCall::invoke(L, base, 2, 1));
   }
}

//********************************************************************************************************************
// Convert value to number

extern lua_Number lua_tonumber(lua_State *L, int idx)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   TValue tmp;
   if (auto num = try_to_number(o, &tmp)) return *num;
   return 0;
}

//********************************************************************************************************************
// Convert value to number with success indicator

extern lua_Number lua_tonumberx(lua_State *L, int idx, int* ok)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   TValue tmp;
   if (auto num = try_to_number(o, &tmp)) {
      if (ok) *ok = 1;
      return *num;
   }
   if (ok) *ok = 0;
   return 0;
}

//********************************************************************************************************************
// Check and convert value to number with error

extern lua_Number luaL_checknumber(lua_State *L, int idx)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   TValue tmp;
   if (auto num = try_to_number(o, &tmp)) return *num;
   lj_err_argt(L, idx, LUA_TNUMBER);
}

//********************************************************************************************************************
// Convert value to number with default

extern lua_Number luaL_optnumber(lua_State *L, int idx, lua_Number def)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   TValue tmp;
   if (tvisnil(o)) return def;
   if (auto num = try_to_number(o, &tmp)) return *num;
   lj_err_argt(L, idx, LUA_TNUMBER);
}

//********************************************************************************************************************
// Convert value to integer

extern lua_Integer lua_tointeger(lua_State *L, int idx)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   TValue tmp;
   if (auto i = try_to_integer(o, &tmp)) return *i;
   return 0;
}

//********************************************************************************************************************
// Convert value to integer with success indicator

extern lua_Integer lua_tointegerx(lua_State *L, int idx, int* ok)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   TValue tmp;
   if (auto i = try_to_integer(o, &tmp)) {
      if (ok) *ok = 1;
      return *i;
   }
   if (ok) *ok = 0;
   return 0;
}

//********************************************************************************************************************
// Check and convert value to integer with error

extern lua_Integer luaL_checkinteger(lua_State *L, int idx)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   TValue tmp;
   if (auto i = try_to_integer(o, &tmp)) return *i;
   lj_err_argt(L, idx, LUA_TNUMBER);
}

//********************************************************************************************************************
// Convert value to integer with default

extern lua_Integer luaL_optinteger(lua_State *L, int idx, lua_Integer def)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   TValue tmp;
   if (tvisnil(o)) return def;
   if (auto i = try_to_integer(o, &tmp)) return *i;
   lj_err_argt(L, idx, LUA_TNUMBER);
}

//********************************************************************************************************************
// Convert value to boolean

extern int lua_toboolean(lua_State *L, int idx)
{
   cTValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index_const(L, idx) : index2adr(L, idx);
   return tvistruecond(o);
}

//********************************************************************************************************************
// Return array value (does not perform any conversion)

extern GCarray * lua_toarray(lua_State *L, int Arg)
{
   TValue *o = (Arg > LUA_REGISTRYINDEX) ? resolve_index(L, Arg) : index2adr(L, Arg);
   if (tvisarray(o)) return &gcval(o)->arr;
   lj_err_argt(L, Arg, LUA_TARRAY);
}

//********************************************************************************************************************
// Return object value (validates but does not perform any conversion)
// Handles thunk resolution

extern GCobject * lua_toobject(lua_State *L, int Arg)
{
   TValue *o = (Arg > LUA_REGISTRYINDEX) ? resolve_index(L, Arg) : index2adr(L, Arg);
   if (tvisobject(o)) return &gcval(o)->obj;
   lj_err_argt(L, Arg, LUA_TOBJECT);
}

extern GCobject * lua_optobject(lua_State *L, int Arg)
{
   TValue *o = (Arg > LUA_REGISTRYINDEX) ? resolve_index(L, Arg) : index2adr(L, Arg);
   if (tvisobject(o)) return &gcval(o)->obj;
   else if (tvisnil(o)) return nullptr;
   lj_err_argt(L, Arg, LUA_TOBJECT);
}

//********************************************************************************************************************
// Convert value to string with length

extern CSTRING lua_tolstring(lua_State *L, int idx, size_t* len)
{
   TValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index(L, idx) : index2adr(L, idx);
   GCstr *s;
   if (tvisstr(o)) s = strV(o);
   else if (tvisnumber(o)) {
      lj_gc_check(L);
      o = (idx > LUA_REGISTRYINDEX) ? index2adr_stack(L, idx) : index2adr(L, idx);
      s = lj_strfmt_number(L, o);
      setstrV(L, o, s);
   }
   else {
      if (len != nullptr) *len = 0;
      return nullptr;
   }
   if (len != nullptr) *len = s->len;
   return strdata(s);
}

//********************************************************************************************************************
// Check and convert value to string with error

extern const char * luaL_checklstring(lua_State *L, int idx, size_t* len)
{
   TValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index(L, idx) : index2adr(L, idx);
   GCstr *s;
   if (tvisstr(o)) [[likely]] {
      s = strV(o);
   }
   else if (tvisnumber(o)) {
      lj_gc_check(L);
      o = (idx > LUA_REGISTRYINDEX) ? index2adr_stack(L, idx) : index2adr(L, idx);
      s = lj_strfmt_number(L, o);
      setstrV(L, o, s);
   }
   else lj_err_argt(L, idx, LUA_TSTRING);

   if (len != nullptr) *len = s->len;
   return strdata(s);
}

//********************************************************************************************************************
// Works as for luaL_checklstring but returns string hash.  Throws if type is not string compatible.

extern uint32_t luaL_checkstringhash(lua_State *L, int idx)
{
   TValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index(L, idx) : index2adr(L, idx);
   GCstr *s;
   if (tvisstr(o)) [[likely]] {
      s = strV(o);
   }
   else if (tvisnumber(o)) {
      lj_gc_check(L);
      o = (idx > LUA_REGISTRYINDEX) ? index2adr_stack(L, idx) : index2adr(L, idx);
      s = lj_strfmt_number(L, o);
      setstrV(L, o, s);
   }
   else lj_err_argt(L, idx, LUA_TSTRING);

   return s->hash;
}

//********************************************************************************************************************
// Convert value to string with default

extern CSTRING luaL_optlstring(lua_State *L, int idx, CSTRING def, size_t* len)
{
   TValue *o = (idx > LUA_REGISTRYINDEX) ? resolve_index(L, idx) : index2adr(L, idx);
   GCstr *s;

   if (tvisstr(o)) [[likely]] {
      s = strV(o);
   }
   else if (tvisnil(o)) {
      if (len != nullptr) *len = def ? strlen(def) : 0;
      return def;
   }
   else if (tvisnumber(o)) {
      lj_gc_check(L);
      o = (idx > LUA_REGISTRYINDEX) ? index2adr_stack(L, idx) : index2adr(L, idx);
      s = lj_strfmt_number(L, o);
      setstrV(L, o, s);
   }
   else lj_err_argt(L, idx, LUA_TSTRING);

   if (len != nullptr) *len = s->len;
   return strdata(s);
}

//********************************************************************************************************************
// Check value matches one of the option strings

extern int luaL_checkoption(lua_State *L, int idx, CSTRING def, CSTRING const lst[])
{
   ptrdiff_t i;
   auto s = lua_tolstring(L, idx, nullptr);
   if (s IS nullptr and (s = def) IS nullptr) lj_err_argt(L, idx, LUA_TSTRING);
   for (i = 0; lst[i]; i++) {
      if (strcmp(lst[i], s) IS 0) return (int)i;
   }
   lj_err_argv(L, idx, ErrMsg::INVOPTM, s);
}

//********************************************************************************************************************
// Get length of value

extern size_t lua_objlen(lua_State *L, int idx)
{
   TValue *o = index2adr(L, idx);
   if (tvisstr(o)) return strV(o)->len;
   else if (tvistab(o)) return (size_t)lj_tab_len(tabV(o));
   else if (tvisarray(o)) return arrayV(o)->len;
   else if (tvisudata(o)) return udataV(o)->len;
   else if (tvisnumber(o)) {
      GCstr *s = lj_strfmt_number(L, o);
      setstrV(L, o, s);
      return s->len;
   }
   else return 0;
}

//********************************************************************************************************************
// Get C function pointer if value is a C function

extern lua_CFunction lua_tocfunction(lua_State *L, int idx)
{
   cTValue *o = index2adr(L, idx);
   if (tvisfunc(o)) {
      BCOp op = bc_op(*mref<BCIns>(funcV(o)->c.pc));
      if (op IS BC_FUNCC or op IS BC_FUNCCW) return funcV(o)->c.f;
   }
   return nullptr;
}

//********************************************************************************************************************
// Get pointer to userdata

extern void * lua_touserdata(lua_State *L, int idx)
{
   cTValue *o = index2adr(L, idx);
   if (tvisudata(o)) return uddata(udataV(o));
   else if (tvislightud(o)) return lightudV(G(L), o);
   else return nullptr;
}

//********************************************************************************************************************
// Get thread if value is a coroutine

extern lua_State * lua_tothread(lua_State *L, int idx)
{
   cTValue* o = index2adr(L, idx);
   return (!tvisthread(o)) ? nullptr : threadV(o);
}

//********************************************************************************************************************
// Get pointer representation of value

extern const void* lua_topointer(lua_State *L, int idx)
{
   return lj_obj_ptr(G(L), index2adr(L, idx));
}

//********************************************************************************************************************
// Stack setters (object creation)

extern void lua_pushnil(lua_State *L)
{
   setnilV(L->top);
   incr_top(L);
}

//********************************************************************************************************************
// Push number onto stack

extern void lua_pushnumber(lua_State *L, lua_Number n)
{
   setnumV(L->top, n);
   if (LJ_UNLIKELY(tvisnan(L->top))) setnanV(L->top);  //  Canonicalize injected NaNs.
   incr_top(L);
}

//********************************************************************************************************************
// Push integer onto stack

extern void lua_pushinteger(lua_State *L, lua_Integer n)
{
   setintptrV(L->top, n);
   incr_top(L);
}

//********************************************************************************************************************
// Push string of specified length onto stack

extern void lua_pushlstring(lua_State *L, CSTRING str, size_t len)
{
   lj_gc_check(L);
   auto s = lj_str_new(L, str, len);
   setstrV(L, L->top, s);
   incr_top(L);
}

//********************************************************************************************************************
// Push null-terminated string onto stack

extern void lua_pushstring(lua_State *L, CSTRING str)
{
   if (str IS nullptr) setnilV(L->top);
   else {
      lj_gc_check(L);
      auto s = lj_str_newz(L, str);
      setstrV(L, L->top, s);
   }
   incr_top(L);
}

//********************************************************************************************************************
// Push formatted string onto stack with varargs

extern CSTRING lua_pushvfstring(lua_State *L, CSTRING fmt, va_list argp)
{
   lj_gc_check(L);
   return lj_strfmt_pushvf(L, fmt, argp);
}

//********************************************************************************************************************
// Push formatted string onto stack

extern CSTRING lua_pushfstring(lua_State *L, CSTRING fmt, ...)
{
   va_list argp;
   lj_gc_check(L);
   va_start(argp, fmt);
   auto ret = lj_strfmt_pushvf(L, fmt, argp);
   va_end(argp);
   return ret;
}

//********************************************************************************************************************
// Push C closure with upvalues onto stack

extern void lua_pushcclosure(lua_State *L, lua_CFunction f, int n)
{
   lj_gc_check(L);
   lj_checkapi_slot(n);
   auto fn = lj_func_newC(L, (MSize)n, getcurrenv(L));
   fn->c.f = f;
   L->top -= n;
   while (n--) copyTV(L, &fn->c.upvalue[n], L->top + n);
   setfuncV(L, L->top, fn);
   lj_assertL(iswhite(obj2gco(fn)), "new GC object is not white");
   incr_top(L);
}

//********************************************************************************************************************
// Push boolean value onto stack

extern void lua_pushboolean(lua_State *L, int b)
{
   setboolV(L->top, (b != 0));
   incr_top(L);
}

//********************************************************************************************************************
// Push light userdata pointer onto stack

extern void lua_pushlightuserdata(lua_State *L, void* p)
{
   p = lj_lightud_intern(L, p);
   setrawlightudV(L->top, p);
   incr_top(L);
}

//********************************************************************************************************************
// Create table and push onto stack

extern void lua_createtable(lua_State *L, int narray, int nrec)
{
   lj_gc_check(L);
   settabV(L, L->top, lj_tab_new_ah(L, narray, nrec));
   incr_top(L);
}

//********************************************************************************************************************
// Create array and push onto stack

extern void lua_createarray(lua_State *L, uint32_t Length, AET Type, void *Data, uint8_t Flags, std::string_view StructName)
{
   lj_gc_check(L);
   setarrayV(L, L->top, lj_array_new(L, Length, Type, Data, Flags, StructName));
   incr_top(L);
}

//********************************************************************************************************************
// Create native Kotuku object and push onto stack. Returns pointer for additional configuration.

extern GCobject * lua_pushobject(lua_State *L, OBJECTID UID, OBJECTPTR Ptr, objMetaClass *ClassPtr, uint8_t Flags)
{
   lj_gc_check(L);
   auto obj = lj_object_new(L, UID, Ptr, ClassPtr, Flags);
   setobjectV(L, L->top, obj);
   incr_top(L);
   return obj;
}

//********************************************************************************************************************
// Create new metatable in registry

extern int luaL_newmetatable(lua_State *L, CSTRING tname)
{
   GCtab *regt = tabV(registry(L));
   TValue *tv = lj_tab_setstr(L, regt, lj_str_newz(L, tname));
   if (tvisnil(tv)) {
      GCtab *mt = lj_tab_new(L, 0, 1);
      settabV(L, tv, mt);
      settabV(L, L->top++, mt);
      lj_gc_anybarriert(L, regt);
      return 1;
   }
   else {
      copyTV(L, L->top++, tv);
      return 0;
   }
}

//********************************************************************************************************************
// Push current thread onto stack

extern int lua_pushthread(lua_State *L)
{
   setthreadV(L, L->top, L);
   incr_top(L);
   return (mainthread(G(L)) IS L);
}

//********************************************************************************************************************
// Create userdata and push onto stack

extern void * lua_newuserdata(lua_State *L, size_t size)
{
   GCudata *ud;
   lj_gc_check(L);
   if (size > LJ_MAX_UDATA) lj_err_msg(L, ErrMsg::UDATAOV);
   ud = lj_udata_new(L, (MSize)size, getcurrenv(L));
   setudataV(L, L->top, ud);
   incr_top(L);
   return uddata(ud);
}

//********************************************************************************************************************
// Concatenate top n stack values

extern void lua_concat(lua_State *L, int n)
{
   lj_checkapi_slot(n);
   if (n >= 2) {
      n--;
      do {
         TValue *top = lj_meta_cat(L, L->top - 1, -n);
         if (top IS nullptr) {
            L->top -= n;
            break;
         }
         n -= MetaCall::invokeConcat(L, top);
      } while (--n > 0);
   }
   else if (n IS 0) {  // Push empty string.
      setstrV(L, L->top, &G(L)->strempty);
      incr_top(L);
   }
   // else n IS 1: nothing to do.
}

//********************************************************************************************************************
// Object getters

extern void lua_gettable(lua_State *L, int idx)
{
   cTValue *t = index2adr_check(L, idx);
   cTValue *v = lj_meta_tget(L, t, L->top - 1);
   if (v IS nullptr) v = MetaCall::invokeGet(L);
   copyTV(L, L->top - 1, v);
}

//********************************************************************************************************************
// Get table field by string key

extern void lua_getfield(lua_State *L, int idx, CSTRING k)
{
   cTValue *t = index2adr_check(L, idx);
   TValue key;
   setstrV(L, &key, lj_str_newz(L, k));
   cTValue *v = lj_meta_tget(L, t, &key);
   if (v IS nullptr) v = MetaCall::invokeGet(L);
   copyTV(L, L->top, v);
   incr_top(L);
}

//********************************************************************************************************************
// Get raw table value without metamethods

extern void lua_rawget(lua_State *L, int idx)
{
   cTValue *t = index2adr(L, idx);
   lj_checkapi(tvistab(t), "stack slot %d is not a table", idx);
   copyTV(L, L->top - 1, lj_tab_get(L, tabV(t), L->top - 1));
}

//********************************************************************************************************************
// Get raw table value by integer index

extern void lua_rawgeti(lua_State *L, int idx, int n)
{
   cTValue *v, *t = index2adr(L, idx);
   lj_checkapi(tvistab(t), "stack slot %d is not a table", idx);
   v = lj_tab_getint(tabV(t), n);
   copy_or_nil(L, L->top, v);
   incr_top(L);
}

//********************************************************************************************************************
// Get metatable of value

extern int lua_getmetatable(lua_State *L, int idx)
{
   cTValue *o = index2adr(L, idx);
   GCtab *mt = nullptr;
   if (tvistab(o)) mt = tabref(tabV(o)->metatable);
   else if (tvisudata(o)) mt = tabref(udataV(o)->metatable);
   else if (tvisarray(o)) mt = tabref(arrayV(o)->metatable);
   else mt = tabref(basemt_obj(G(L), o));
   if (mt IS nullptr) return 0;
   settabV(L, L->top, mt);
   incr_top(L);
   return 1;
}

//********************************************************************************************************************
// Get metatable field by string key

extern int luaL_getmetafield(lua_State *L, int idx, CSTRING field)
{
   if (lua_getmetatable(L, idx)) {
      cTValue *tv = lj_tab_getstr(tabV(L->top - 1), lj_str_newz(L, field));
      if (tv and !tvisnil(tv)) {
         copyTV(L, L->top - 1, tv);
         return 1;
      }
      L->top--;
   }
   return 0;
}

//********************************************************************************************************************
// Get function/userdata/thread environment table

extern void lua_getfenv(lua_State *L, int idx)
{
   cTValue *o = index2adr_check(L, idx);
   if (tvisfunc(o)) settabV(L, L->top, tabref(funcV(o)->c.env));
   else if (tvisudata(o)) settabV(L, L->top, tabref(udataV(o)->env));
   else if (tvisthread(o)) settabV(L, L->top, tabref(threadV(o)->env));
   else setnilV(L->top);
   incr_top(L);
}

//********************************************************************************************************************
// Get next table key-value pair

extern int lua_next(lua_State *L, int idx)
{
   cTValue *t = index2adr(L, idx);
   int more;
   lj_checkapi(tvistab(t), "stack slot %d is not a table", idx);
   more = lj_tab_next(tabV(t), L->top - 1, L->top - 1);
   if (more > 0) incr_top(L);  //  Return new key and value slot.
   else if (!more) {  // End of traversal.
      L->top--;  //  Remove key slot.
   }
   else lj_err_msg(L, ErrMsg::NEXTIDX);
   return more;
}

//********************************************************************************************************************
// Get function upvalue by index

extern CSTRING lua_getupvalue(lua_State *L, int idx, int n)
{
   TValue *val;
   GCobj *o;
   auto name = lj_debug_uvnamev(index2adr(L, idx), (uint32_t)(n - 1), &val, &o);
   if (name) {
      copyTV(L, L->top, val);
      incr_top(L);
   }
   return name;
}

//********************************************************************************************************************
// Get unique identifier for upvalue

extern void * lua_upvalueid(lua_State *L, int idx, int n)
{
   GCfunc *fn = funcV(index2adr(L, idx));
   n--;
   lj_checkapi((uint32_t)n < fn->l.nupvalues, "bad upvalue %d", n);
   return isluafunc(fn) ? (void*)gcref(fn->l.uvptr[n]) : (void*)&fn->c.upvalue[n];
}

//********************************************************************************************************************
// Join two function upvalues

extern void lua_upvaluejoin(lua_State *L, int idx1, int n1, int idx2, int n2)
{
   GCfunc *fn1 = funcV(index2adr(L, idx1));
   GCfunc *fn2 = funcV(index2adr(L, idx2));
   n1--; n2--;
   lj_checkapi(isluafunc(fn1), "stack slot %d is not a Lua function", idx1);
   lj_checkapi(isluafunc(fn2), "stack slot %d is not a Lua function", idx2);
   lj_checkapi((uint32_t)n1 < fn1->l.nupvalues, "bad upvalue %d", n1 + 1);
   lj_checkapi((uint32_t)n2 < fn2->l.nupvalues, "bad upvalue %d", n2 + 1);
   setgcrefr(fn1->l.uvptr[n1], fn2->l.uvptr[n2]);
   lj_gc_objbarrier(L, fn1, gcref(fn1->l.uvptr[n1]));
}

//********************************************************************************************************************
// Test if value is userdata with metatable

extern void* luaL_testudata(lua_State *L, int idx, const char *tname)
{
   cTValue *o = index2adr(L, idx);
   if (tvisudata(o)) {
      GCudata *ud = udataV(o);
      cTValue *tv = lj_tab_getstr(tabV(registry(L)), lj_str_newz(L, tname));
      if (tv and tvistab(tv) and tabV(tv) IS tabref(ud->metatable)) return uddata(ud);
   }
   return nullptr;  //  value is not a userdata with a metatable
}

//********************************************************************************************************************
// Check and return userdata with metatable

extern void * luaL_checkudata(lua_State *L, int idx, const char *tname)
{
   void *p = luaL_testudata(L, idx, tname);
   if (!p) lj_err_argtype(L, idx, tname);
   return p;
}

//********************************************************************************************************************
// Set table value by key

extern void lua_settable(lua_State *L, int idx)
{
   cTValue *t = index2adr_check(L, idx);
   lj_checkapi_slot(2);
   TValue *o = lj_meta_tset(L, t, L->top - 2);
   if (o) {
      // NOBARRIER: lj_meta_tset ensures the table is not black.
      L->top -= 2;
      copyTV(L, o, L->top + 1);
   }
   else {
      MetaCall::invokeSetTable(L, L->top);
   }
}

//********************************************************************************************************************
// Set table field by string key

extern void lua_setfield(lua_State *L, int idx, CSTRING k)
{
   TValue key;
   cTValue *t = index2adr_check(L, idx);
   lj_checkapi_slot(1);
   setstrV(L, &key, lj_str_newz(L, k));
   TValue *o = lj_meta_tset(L, t, &key);
   if (o) {
      // NOBARRIER: lj_meta_tset ensures the table is not black.
      copyTV(L, o, --L->top);
   }
   else {
      MetaCall::invokeSetField(L, L->top);
   }
}

//********************************************************************************************************************
// Set raw table value without metamethods

extern void lua_rawset(lua_State *L, int idx)
{
   GCtab* t = tabV(index2adr(L, idx));
   TValue* dst, * key;
   lj_checkapi_slot(2);
   key = L->top - 2;
   dst = lj_tab_set(L, t, key);
   copyTV(L, dst, key + 1);
   lj_gc_anybarriert(L, t);
   L->top = key;
}

//********************************************************************************************************************
// Set raw table value by integer index

extern void lua_rawseti(lua_State *L, int idx, int n)
{
   GCtab* t = tabV(index2adr(L, idx));
   TValue* dst, * src;
   lj_checkapi_slot(1);
   dst = lj_tab_setint(L, t, n);
   src = L->top - 1;
   copyTV(L, dst, src);
   lj_gc_barriert(L, t, dst);
   L->top = src;
}

//********************************************************************************************************************
// Set metatable of value

extern int lua_setmetatable(lua_State *L, int idx)
{
   GCtab *mt;
   cTValue *o = index2adr_check(L, idx);

   lj_checkapi_slot(1);
   if (tvisnil(L->top - 1)) {
      mt = nullptr;
   }
   else {
      lj_checkapi(tvistab(L->top - 1), "top stack slot is not a table");
      mt = tabV(L->top - 1);
   }

   auto g = G(L);
   if (tvistab(o)) {
      setgcref(tabV(o)->metatable, obj2gco(mt));
      if (mt) lj_gc_objbarriert(L, tabV(o), mt);
   }
   else if (tvisudata(o)) {
      setgcref(udataV(o)->metatable, obj2gco(mt));
      if (mt) lj_gc_objbarrier(L, udataV(o), mt);
   }
   else if (tvisarray(o)) {
      setgcref(arrayV(o)->metatable, obj2gco(mt));
      if (mt) lj_gc_objbarrier(L, arrayV(o), mt);
   }
   else {
      // Flush cache, since traces specialize to basemt. But not during __gc.
      if (lj_trace_flushall(L)) lj_err_caller(L, ErrMsg::NOGCMM);
      if (tvisbool(o)) {
         // NOBARRIER: basemt is a GC root.
         setgcref(basemt_it(g, LJ_TTRUE), obj2gco(mt));
         setgcref(basemt_it(g, LJ_TFALSE), obj2gco(mt));
      }
      else {
         // NOBARRIER: basemt is a GC root.
         setgcref(basemt_obj(g, o), obj2gco(mt));
      }
   }
   L->top--;
   return 1;
}

//********************************************************************************************************************
// Set metatable from registry

extern void luaL_setmetatable(lua_State *L, CSTRING tname)
{
   lua_getfield(L, LUA_REGISTRYINDEX, tname);
   lua_setmetatable(L, -2);
}

//********************************************************************************************************************
// Set base metatable for a type (used for custom native types like LJ_TOBJECT)
// Takes the metatable from the top of the stack and pops it.

extern void lua_setbasemetatable(lua_State *L, uint32_t itype)
{
   lj_checkapi_slot(1);
   lj_checkapi(tvistab(L->top - 1), "top stack slot is not a table");

   auto mt = tabV(L->top - 1);
   auto g = G(L);

   if (lj_trace_flushall(L)) lj_err_caller(L, ErrMsg::NOGCMM);

   // NOBARRIER: basemt is a GC root.
   setgcref(basemt_it(g, itype), obj2gco(mt));

   L->top--;
}

//********************************************************************************************************************
// Set function/userdata/thread environment table

extern int lua_setfenv(lua_State *L, int idx)
{
   cTValue *o = index2adr_check(L, idx);
   GCtab *t;
   lj_checkapi_slot(1);
   lj_checkapi(tvistab(L->top - 1), "top stack slot is not a table");
   t = tabV(L->top - 1);
   if (tvisfunc(o)) setgcref(funcV(o)->c.env, obj2gco(t));
   else if (tvisudata(o)) setgcref(udataV(o)->env, obj2gco(t));
   else if (tvisthread(o)) setgcref(threadV(o)->env, obj2gco(t));
   else {
      L->top--;
      return 0;
   }
   lj_gc_objbarrier(L, gcV(o), t);
   L->top--;
   return 1;
}

//********************************************************************************************************************
// Set function upvalue by index

extern CSTRING lua_setupvalue(lua_State *L, int idx, int n)
{
   cTValue *f = index2adr(L, idx);
   TValue *val;
   GCobj *o;

   lj_checkapi_slot(1);
   auto name = lj_debug_uvnamev(f, (uint32_t)(n - 1), &val, &o);
   if (name) {
      L->top--;
      copyTV(L, val, L->top);
      lj_gc_barrier(L, o, L->top);
   }
   return name;
}

//********************************************************************************************************************
// Prepare base for function call

static TValue * api_call_base(lua_State *L, int nargs)
{
   TValue * o = L->top, * base = o - nargs;
   L->top = o + 1;
   for (; o > base; o--) copyTV(L, o, o - 1);
   setnilV(o);
   L->sent_traceback = false;
   return o + 1;
}

//********************************************************************************************************************
// Call Lua function synchronously

extern void lua_call(lua_State *L, int nargs, int nresults)
{
   lj_checkapi(L->status IS LUA_OK or L->status IS LUA_ERRERR, "thread called in wrong state %d", L->status);
   lj_checkapi_slot(nargs + 1);

   // Stack integrity checks - catch issues from VM helpers that don't set L->top.
   // See VMHelperGuard in stack_helpers.h for the proper fix pattern.
   lj_checkapi(L->base >= tvref(L->stack), "stack base before stack start");
   lj_checkapi(L->top >= L->base, "stack top before base - VM helper may need VMHelperGuard");
   lj_checkapi(L->top <= tvref(L->maxstack), "stack overflow");

   lj_vm_call(L, api_call_base(L, nargs), nresults + 1);
}

//********************************************************************************************************************
// Call Lua function with error handling

extern int lua_pcall(lua_State *L, int nargs, int nresults, int errfunc)
{
   global_State* g = G(L);
   uint8_t oldh = hook_save(g);
   ptrdiff_t ef;
   int status;

   lj_checkapi(L->status IS LUA_OK or L->status IS LUA_ERRERR, "thread called in wrong state %d", L->status);
   lj_checkapi_slot(nargs + 1);

   // Stack integrity checks - catch issues from VM helpers that don't set L->top.
   // See VMHelperGuard in stack_helpers.h for the proper fix pattern.
   lj_checkapi(L->base >= tvref(L->stack), "stack base before stack start");
   lj_checkapi(L->top >= L->base, "stack top before base - VM helper may need VMHelperGuard");
   lj_checkapi(L->top <= tvref(L->maxstack), "stack overflow");

   if (errfunc IS 0) ef = 0;
   else {
      auto o = index2adr_stack(L, errfunc);
      ef = savestack(L, o);
   }
   status = lj_vm_pcall(L, api_call_base(L, nargs), nresults + 1, ef);
   if (status) hook_restore(g, oldh);
   return status;
}

//********************************************************************************************************************
// Prepare C function call with userdata argument

static TValue* cpcall(lua_State *L, lua_CFunction func, void* ud)
{
   GCfunc* fn = lj_func_newC(L, 0, getcurrenv(L));
   TValue* top = L->top;
   fn->c.f = func;
   setfuncV(L, top++, fn);
   setnilV(top++);
   ud = lj_lightud_intern(L, ud);
   setrawlightudV(top++, ud);
   cframe_nres(L->cframe) = 1 + 0;  //  Zero results.
   L->top = top;
   return top - 1;  //  Now call the newly allocated C function.
}

//********************************************************************************************************************
// Call C function with error handling

extern int lua_cpcall(lua_State *L, lua_CFunction func, void *ud)
{
   global_State *g = G(L);
   uint8_t oldh = hook_save(g);
   int status;
   lj_checkapi(L->status IS LUA_OK or L->status IS LUA_ERRERR, "thread called in wrong state %d", L->status);
   status = lj_vm_cpcall(L, func, ud, cpcall);
   if (status) hook_restore(g, oldh);
   return status;
}

//********************************************************************************************************************
// Call metamethod function

extern int luaL_callmeta(lua_State *L, int idx, const char *field)
{
   if (luaL_getmetafield(L, idx, field)) {
      TValue* top = L->top--;
      setnilV(top++);
      copyTV(L, top++, index2adr(L, idx));
      L->top = top;
      lj_vm_call(L, top - 1, 1 + 1);
      return 1;
   }
   return 0;
}

//********************************************************************************************************************
// Control garbage collection.  The `what` parameter selects the operation:
//
//   LUA_GCSTOP        Stop the garbage collector.
//   LUA_GCRESTART     Restart the collector; `data` is currently unused.
//   LUA_GCCOLLECT     Perform a full collection cycle.
//   LUA_GCCOUNT       Return total managed memory in kibibytes (value >> 10).
//   LUA_GCCOUNTB      Return the remainder bytes of total memory (value & 0x3ff).
//   LUA_GCSTEP        Perform an incremental step.  `data` is the step size in kibibytes.
//                     Returns 1 if a full cycle was completed during the step.
//   LUA_GCSETPAUSE    Set the pause parameter (controls interval between cycles).  Returns the previous value.
//   LUA_GCSETSTEPMUL  Set the step multiplier (controls collector aggressiveness).  Returns the previous value.
//   LUA_GCISRUNNING   Returns 1 if the collector is running, otherwise 0.
//
// Returns -1 if `what` is not a recognised option.

extern int lua_gc(lua_State *L, int what, int data)
{
   global_State* g = G(L);
   GarbageCollector collector = gc(g);
   int res = 0;
   switch (what) {
      case LUA_GCSTOP:    collector.stop(); break;
      case LUA_GCRESTART: collector.restart(data); break;
      case LUA_GCCOLLECT: collector.fullCycle(L); break;
      case LUA_GCCOUNT:   res = (int)(collector.totalMemory() >> 10); break;
      case LUA_GCCOUNTB:  res = (int)(collector.totalMemory() & 0x3ff); break;
      case LUA_GCSTEP: {
         GCSize a = (GCSize)data << 10;
         g->gc.threshold = (a <= g->gc.total) ? (g->gc.total - a) : 0;
         while (g->gc.total >= g->gc.threshold)
            if (collector.step(L) > 0) {
               res = 1;
               break;
            }
         break;
      }
      case LUA_GCSETPAUSE:
         res = (int)collector.setPause((MSize)data);
         break;
      case LUA_GCSETSTEPMUL:
         res = (int)collector.setStepMultiplier((MSize)data);
         break;
      case LUA_GCISRUNNING:
         res = collector.isRunning() ? 1 : 0;
         break;
      default:
         res = -1;  //  Invalid option.
   }
   return res;
}

//********************************************************************************************************************
// Get memory allocator function and userdata

extern lua_Alloc lua_getallocf(lua_State *L, void** ud)
{
   global_State* g = G(L);
   if (ud) *ud = g->allocd;
   return g->allocf;
}

//********************************************************************************************************************
// Set memory allocator function and userdata

extern void lua_setallocf(lua_State *L, lua_Alloc f, void* ud)
{
   global_State* g = G(L);
   g->allocd = ud;
   g->allocf = f;
}
