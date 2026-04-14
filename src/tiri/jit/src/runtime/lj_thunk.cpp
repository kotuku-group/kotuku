// Thunk (deferred evaluation) implementation for Tiri.
// Provides automatic lazy evaluation through userdata metatables.

#include <cmath>
#include <cstring>

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_strfmt.h"
#include "lj_str.h"
#include "lualib.h"
#include "lj_thunk.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "../../defs.h"

using std::floor;
using std::pow;

#define THUNK_METATABLE_NAME  "tiri.thunk"

//********************************************************************************************************************
// Create a new thunk userdata

void lj_thunk_new(lua_State *L, GCfunc *func, int expected_type)
{
   // Allocate userdata with ThunkPayload - use global environment for GC traversal
   GCudata *ud = lj_udata_new(L, sizeof(ThunkPayload), tabref(L->env));
   ud->udtype = UDTYPE_THUNK;

   // Initialize payload
   ThunkPayload *payload = thunk_payload(ud);
   setgcref(payload->deferred_func, obj2gco(func));
   setnilV(&payload->cached_value);
   payload->resolved = 0;
   payload->expected_type = (uint8_t)expected_type;
   payload->padding = 0;

   // Set metatable from registry
   cTValue *tv = lj_tab_getstr(tabV(registry(L)), lj_str_newz(L, THUNK_METATABLE_NAME));
   if (tv and tvistab(tv)) {
      setgcref(ud->metatable, obj2gco(tabV(tv)));
   }

   // Push userdata to stack
   setudataV(L, L->top, ud);
   incr_top(L);

   // GC barrier for the function reference
   lj_gc_objbarrier(L, obj2gco(ud), obj2gco(func));
}

//********************************************************************************************************************
// Resolve a thunk if not already resolved.
//
// IMPORTANT: Callers from VM assembler code (e.g., lj_meta_equal_thunk) must use
// VMHelperGuard to ensure L->top is valid before calling this function.
// This function handles stack reallocation during lua_pcall but does not fix L->top.
//
// JIT SAFETY: Thunk resolution aborts any ongoing trace and temporarily disables JIT
// recording by setting HOOK_GC. This prevents the JIT from recording across the lua_pcall
// boundary, which would cause snapshot_framelinks() to fail when it encounters the
// protected C frame created by lua_pcall.

TValue* lj_thunk_resolve(lua_State *L, GCudata *thunk_udata)
{
   ThunkPayload *payload = thunk_payload(thunk_udata);

   // If already resolved, return cached value
   if (payload->resolved) {
      return &payload->cached_value;
   }

   // Get the deferred function
   GCfunc *fn = gco_to_function(gcref(payload->deferred_func));

   // Abort any ongoing trace and disable JIT recording during thunk resolution.
   // The JIT cannot record across lua_pcall boundaries because the protected C frame
   // breaks the frame chain that snapshot_framelinks() expects to traverse.

   global_State *g = G(L);
   uint8_t oldh = hook_save(g);
   lj_trace_abort(g);
   hook_entergc(g);  // Disable hooks and new traces during thunk resolution.
   if (LJ_HASPROFILE and (oldh & HOOK_PROFILE)) lj_dispatch_update(g);

   // Save stack offsets - the stack may be reallocated during lua_pcall.
   // We save as byte offsets from L->stack, not raw pointers.

   ptrdiff_t base_offset = savestack(L, L->base);
   ptrdiff_t top_offset = savestack(L, L->top);

   // Push function to stack. lua_pcall expects func at L->top - nargs - 1.
   // With nargs=0, func should be at L->top - 1, so push it then call.

   setfuncV(L, L->top, fn);
   L->top++;

   // Use lua_pcall for protected call - this properly handles being called
   // from VM assembler functions like lj_meta_equal_thunk
   int status = lua_pcall(L, 0, 1, 0);

   // Restore hooks to re-enable JIT recording
   hook_restore(g, oldh);
   if (LJ_HASPROFILE and (oldh & HOOK_PROFILE)) lj_dispatch_update(g);

   // Restore base in case stack was reallocated
   L->base = restorestack(L, base_offset);

   if (status != 0) { // Error occurred - restore stack and propagate
      L->top = restorestack(L, top_offset);
      lj_err_msg(L, ErrMsg::THUNKEX);
   }

   TValue *result = L->top - 1;

   // Cache the result
   copyTV(L, &payload->cached_value, result);
   payload->resolved = 1;

   // Restore stack to original position (pop the result)
   L->top = restorestack(L, top_offset);

   // GC barrier for the cached value if it's a GC object
   if (tvisgcv(&payload->cached_value)) {
      lj_gc_objbarrier(L, obj2gco(thunk_udata), gcval(&payload->cached_value));
   }

   return &payload->cached_value;
}

//********************************************************************************************************************
// Get the current value of a thunk

cTValue* lj_thunk_getvalue(lua_State *L, cTValue *o)
{
   if (lj_is_thunk(o)) {
      GCudata *ud = udataV(o);
      ThunkPayload *payload = thunk_payload(ud);
      if (payload->resolved) {
         return &payload->cached_value;
      }
   }
   return o;
}

//********************************************************************************************************************
// Metamethod implementations

// Forward declarations for metamethods
static int thunk_add(lua_State *L);
static int thunk_sub(lua_State *L);
static int thunk_mul(lua_State *L);
static int thunk_div(lua_State *L);
static int thunk_mod(lua_State *L);
static int thunk_pow(lua_State *L);
static int thunk_unm(lua_State *L);
static int thunk_concat(lua_State *L);
static int thunk_eq(lua_State *L);
static int thunk_lt(lua_State *L);
static int thunk_le(lua_State *L);
static int thunk_index(lua_State *L);
static int thunk_newindex(lua_State *L);
static int thunk_len(lua_State *L);
static int thunk_call(lua_State *L);
static int thunk_tostring(lua_State *L);

// Helper: Get number value from TValue, handling integer and number types

inline lua_Number getnumvalue(TValue *o)
{
   if (tvisint(o)) return (lua_Number)intV(o);
   return numV(o);
}

//********************************************************************************************************************
// Binary arithmetic metamethods

static int thunk_add(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   if (tvisnumber(a) and tvisnumber(b)) {
      lua_Number result = getnumvalue(a) + getnumvalue(b);
      setnumV(L->top++, result);
      return 1;
   }

   lj_err_optype(L, tvisnumber(a) ? b : a, ErrMsg::OPARITH);
   return 0;
}

static int thunk_sub(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   if (tvisnumber(a) and tvisnumber(b)) {
      lua_Number result = getnumvalue(a) - getnumvalue(b);
      setnumV(L->top++, result);
      return 1;
   }

   lj_err_optype(L, tvisnumber(a) ? b : a, ErrMsg::OPARITH);
   return 0;
}

static int thunk_mul(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   if (tvisnumber(a) and tvisnumber(b)) {
      lua_Number result = getnumvalue(a) * getnumvalue(b);
      setnumV(L->top++, result);
      return 1;
   }

   lj_err_optype(L, tvisnumber(a) ? b : a, ErrMsg::OPARITH);
   return 0;
}

static int thunk_div(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   if (tvisnumber(a) and tvisnumber(b)) {
      lua_Number result = getnumvalue(a) / getnumvalue(b);
      setnumV(L->top++, result);
      return 1;
   }

   lj_err_optype(L, tvisnumber(a) ? b : a, ErrMsg::OPARITH);
   return 0;
}

static int thunk_mod(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   if (tvisnumber(a) and tvisnumber(b)) {
      lua_Number na = getnumvalue(a);
      lua_Number nb = getnumvalue(b);
      lua_Number result = na - floor(na / nb) * nb;
      setnumV(L->top++, result);
      return 1;
   }

   lj_err_optype(L, tvisnumber(a) ? b : a, ErrMsg::OPARITH);
   return 0;
}

static int thunk_pow(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   if (tvisnumber(a) and tvisnumber(b)) {
      lua_Number result = pow(getnumvalue(a), getnumvalue(b));
      setnumV(L->top++, result);
      return 1;
   }

   lj_err_optype(L, tvisnumber(a) ? b : a, ErrMsg::OPARITH);
   return 0;
}

// Unary minus
static int thunk_unm(lua_State *L)
{
   TValue *o = resolve_at(L, 0);

   if (tvisnumber(o)) {
      lua_Number result = -getnumvalue(o);
      setnumV(L->top++, result);
      return 1;
   }

   lj_err_optype(L, o, ErrMsg::OPARITH);
   return 0;
}

//********************************************************************************************************************
// Concatenation

static int thunk_concat(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);
   lj_assertL(a != nullptr, "Invalid LHS (null)");
   lj_assertL(b != nullptr, "Invalid RHS (null)");
   copyTV(L, L->top, a);
   L->top++;
   copyTV(L, L->top, b);
   L->top++;
   lua_concat(L, 2);
   return 1;
}

//********************************************************************************************************************
// Comparison metamethods

// Equality - compares resolved values using full Lua equality semantics

static int thunk_eq(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   // Same pointer means equal

   if (a IS b) {
      setboolV(L->top++, 1);
      return 1;
   }

   // Use lj_obj_equal for basic equality (numbers, strings, primitives, reference equality)

   if (lj_obj_equal(a, b)) {
      setboolV(L->top++, 1);
      return 1;
   }

   // For tables and userdata with same type, check __eq metamethod

   if (itype(a) IS itype(b)) {
      if (tvistab(a) or tvisudata(a)) {
         GCobj *gcobj_a = gcV(a);
         GCobj *gcobj_b = gcV(b);

         // Check for __eq metamethod
         cTValue *mo = lj_meta_fast(L, tabref(gcobj_a->gch.metatable), MM_eq);
         if (mo) {
            // Both objects need the same metamethod
            if (tabref(gcobj_a->gch.metatable) IS tabref(gcobj_b->gch.metatable) or
                lj_obj_equal(mo, lj_meta_fast(L, tabref(gcobj_b->gch.metatable), MM_eq))) {
               // Call __eq(a, b)
               copyTV(L, L->top, mo);
               copyTV(L, L->top + 1, a);
               copyTV(L, L->top + 2, b);
               L->top += 3;
               lua_call(L, 2, 1);
               // Result is at L->top - 1, convert to boolean
               int result = tvistruecond(L->top - 1);
               L->top--;
               setboolV(L->top++, result);
               return 1;
            }
         }
      }
   }

   // Different types or no metamethod - not equal
   setboolV(L->top++, 0);
   return 1;
}

// Less than - compares resolved values

static int thunk_lt(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   int result;
   if (tvisnumber(a) and tvisnumber(b)) {
      result = (getnumvalue(a) < getnumvalue(b));
   }
   else if (tvisstr(a) and tvisstr(b)) {
      // String comparison
      GCstr *sa = strV(a);
      GCstr *sb = strV(b);
      size_t len = (sa->len < sb->len) ? sa->len : sb->len;
      int cmp = memcmp(strdata(sa), strdata(sb), len);
      if (cmp IS 0) result = (sa->len < sb->len);
      else result = (cmp < 0);
   }
   else {
      lj_err_comp(L, a, b);
      return 0;
   }

   setboolV(L->top++, result);
   return 1;
}

// Less than or equal - compares resolved values
static int thunk_le(lua_State *L)
{
   TValue *a = resolve_at(L, 0);
   TValue *b = resolve_at(L, 1);

   int result;
   if (tvisnumber(a) and tvisnumber(b)) {
      result = (getnumvalue(a) <= getnumvalue(b));
   }
   else if (tvisstr(a) and tvisstr(b)) {
      // String comparison
      GCstr *sa = strV(a);
      GCstr *sb = strV(b);
      size_t len = (sa->len < sb->len) ? sa->len : sb->len;
      int cmp = memcmp(strdata(sa), strdata(sb), len);
      if (cmp IS 0) result = (sa->len <= sb->len);
      else result = (cmp <= 0);
   }
   else {
      lj_err_comp(L, a, b);
      return 0;
   }

   setboolV(L->top++, result);
   return 1;
}

//********************************************************************************************************************
// Index (field access) - resolves thunk then performs table/userdata lookup

static int thunk_index(lua_State *L)
{
   TValue *o = resolve_at(L, 0);
   TValue *key = L->base + 1;

   if (tvistab(o)) {
      GCtab *t = tabV(o);
      cTValue *res = lj_tab_get(L, t, key);

      // If not found and table has metatable, try __index

      if (tvisnil(res)) {
         GCtab *mt = tabref(t->metatable);
         if (mt) {
            cTValue *idx = lj_tab_getstr(mt, lj_str_newlit(L, "__index"));
            if (idx and not tvisnil(idx)) {
               if (tvisfunc(idx)) { // __index is a function: call __index(table, key)
                  copyTV(L, L->top, idx);
                  copyTV(L, L->top + 1, o);
                  copyTV(L, L->top + 2, key);
                  L->top += 3;
                  lua_call(L, 2, 1);
                  // Result is already at L->top-1, which is now L->top after lua_call returns
                  return 1;
               }
               else if (tvistab(idx)) { // __index is a table: look up key in that table
                  res = lj_tab_get(L, tabV(idx), key);
               }
            }
         }
      }
      copyTV(L, L->top++, res);
      return 1;
   }

   // Handle userdata - delegate to their metatable's __index

   if (tvisudata(o)) {
      GCudata *ud = udataV(o);
      GCtab *mt = tabref(ud->metatable);
      if (mt) {
         cTValue *idx = lj_tab_getstr(mt, lj_str_newlit(L, "__index"));
         if (idx and not tvisnil(idx)) {
            if (tvisfunc(idx)) {
               // __index is a function: call __index(userdata, key)
               copyTV(L, L->top, idx);
               copyTV(L, L->top + 1, o);
               copyTV(L, L->top + 2, key);
               L->top += 3;
               lua_call(L, 2, 1);
               return 1;
            }
            else if (tvistab(idx)) {
               // __index is a table: look up key in that table
               cTValue *res = lj_tab_get(L, tabV(idx), key);
               copyTV(L, L->top++, res);
               return 1;
            }
         }
      }
      // Userdata without __index metamethod
      lj_err_optype(L, o, ErrMsg::OPINDEX);
      return 0;
   }

   // Handle native GCobject (Kotuku objects) - call object_index directly
   // object_index expects the object at L->base[0], so copy the resolved value there

   if (tvisobject(o)) {
      copyTV(L, L->base, o);  // Replace thunk at base with resolved object
      return object_index(L);
   }

   // Not a table, userdata, or object - error
   lj_err_optype(L, o, ErrMsg::OPINDEX);
   return 0;
}

//********************************************************************************************************************
// Newindex (field assignment) - resolves thunk then performs table/userdata assignment

static int thunk_newindex(lua_State *L)
{
   TValue *o = resolve_at(L, 0);
   TValue *key = L->base + 1;
   TValue *val = L->base + 2;

   if (tvistab(o)) {
      GCtab *t = tabV(o);

      // Check if key exists in table - if not, check for __newindex metamethod

      cTValue *existing = lj_tab_get(L, t, key);
      if (tvisnil(existing)) {
         GCtab *mt = tabref(t->metatable);
         if (mt) {
            cTValue *newidx = lj_tab_getstr(mt, lj_str_newlit(L, "__newindex"));
            if (newidx and not tvisnil(newidx)) {
               if (tvisfunc(newidx)) {
                  // __newindex is a function: call __newindex(table, key, value)
                  copyTV(L, L->top, newidx);
                  copyTV(L, L->top + 1, o);
                  copyTV(L, L->top + 2, key);
                  copyTV(L, L->top + 3, val);
                  L->top += 4;
                  lua_call(L, 3, 0);
                  return 0;
               }
               else if (tvistab(newidx)) {
                  // __newindex is a table: set key in that table
                  GCtab *target = tabV(newidx);
                  TValue *slot = lj_tab_set(L, target, key);
                  copyTV(L, slot, val);
                  lj_gc_anybarriert(L, target);
                  return 0;
               }
            }
         }
      }

      // Key exists or no __newindex - set directly in table
      TValue *slot = lj_tab_set(L, t, key);
      copyTV(L, slot, val);
      lj_gc_anybarriert(L, t);
      return 0;
   }

   // Handle userdata (e.g., Kotuku objects) - delegate to their metatable's __newindex
   if (tvisudata(o)) {
      GCudata *ud = udataV(o);
      GCtab *mt = tabref(ud->metatable);
      if (mt) {
         cTValue *newidx = lj_tab_getstr(mt, lj_str_newlit(L, "__newindex"));
         if (newidx and not tvisnil(newidx)) {
            if (tvisfunc(newidx)) {
               // __newindex is a function: call __newindex(userdata, key, value)
               copyTV(L, L->top, newidx);
               copyTV(L, L->top + 1, o);
               copyTV(L, L->top + 2, key);
               copyTV(L, L->top + 3, val);
               L->top += 4;
               lua_call(L, 3, 0);
               return 0;
            }
            else if (tvistab(newidx)) {
               // __newindex is a table: set key in that table
               GCtab *t = tabV(newidx);
               TValue *slot = lj_tab_set(L, t, key);
               copyTV(L, slot, val);
               lj_gc_anybarriert(L, t);
               return 0;
            }
         }
      }
      // Userdata without __newindex metamethod
      lj_err_optype(L, o, ErrMsg::OPINDEX);
      return 0;
   }

   // Handle native GCobject (Kotuku objects) - call object_newindex directly
   // object_newindex expects the object at L->base[0], so copy the resolved value there

   if (tvisobject(o)) {
      copyTV(L, L->base, o);  // Replace thunk at base with resolved object
      return object_newindex(L);
   }

   // Not a table, userdata, or object - error
   lj_err_optype(L, o, ErrMsg::OPINDEX);
   return 0;
}

//********************************************************************************************************************
// Length operator - resolves thunk then gets length

static int thunk_len(lua_State *L)
{
   TValue *o = resolve_at(L, 0);

   if (tvistab(o)) {
      setintV(L->top++, (int32_t)lj_tab_len(tabV(o)));
      return 1;
   }
   else if (tvisstr(o)) {
      setintV(L->top++, (int32_t)strV(o)->len);
      return 1;
   }

   // Not a table or string - error
   lj_err_optype(L, o, ErrMsg::OPLEN);
   return 0;
}

//********************************************************************************************************************
// Call operator - resolves thunk then calls the resolved value if callable

static int thunk_call(lua_State *L)
{
   TValue *o = resolve_at(L, 0);
   int nargs = (int)(L->top - L->base - 1);  // Number of arguments (exclude thunk itself)

   if (not tvisfunc(o)) {
      // Check if it's a table with __call metamethod
      if (tvistab(o)) {
         GCtab *mt = tabref(tabV(o)->metatable);
         if (mt) {
            cTValue *call_mm = lj_tab_getstr(mt, lj_str_newlit(L, "__call"));
            if (call_mm and tvisfunc(call_mm)) {
               // Build call: __call(resolved_table, arg1, arg2, ...)
               // We need to insert the function at base and shift the table
               // Push the __call function
               copyTV(L, L->top, call_mm);
               // Push resolved table
               copyTV(L, L->top + 1, o);
               // Copy arguments
               for (int i = 0; i < nargs; i++) {
                  copyTV(L, L->top + 2 + i, L->base + 1 + i);
               }
               L->top += 2 + nargs;
               lua_call(L, 1 + nargs, LUA_MULTRET);  // __call + table + original args
               return (int)(L->top - L->base);
            }
         }
      }
      lj_err_optype(L, o, ErrMsg::OPCALL);
      return 0;
   }

   // Resolved to a function - call with the arguments
   // Push function

   copyTV(L, L->top, o);

   // Copy arguments
   for (int i = 0; i < nargs; i++) {
      copyTV(L, L->top + 1 + i, L->base + 1 + i);
   }
   L->top += 1 + nargs;
   lua_call(L, nargs, LUA_MULTRET);

   return (int)(L->top - L->base);
}

//********************************************************************************************************************
// String conversion - resolves thunk then converts to string

static int thunk_tostring(lua_State *L)
{
   TValue *o = resolve_at(L, 0);

   if (tvisstr(o)) {
      copyTV(L, L->top++, o);
      return 1;
   }
   else if (tvisnumber(o)) {
      GCstr *s = lj_strfmt_number(L, o);
      setstrV(L, L->top++, s);
      return 1;
   }
   else if (tvisnil(o)) {
      setstrV(L, L->top++, lj_str_newlit(L, "nil"));
      return 1;
   }
   else if (tvisbool(o)) {
      if (boolV(o)) setstrV(L, L->top++, lj_str_newlit(L, "true"));
      else setstrV(L, L->top++, lj_str_newlit(L, "false"));
      return 1;
   }

   // For tables and other types, call the global tostring function
   lua_getglobal(L, "tostring");
   copyTV(L, L->top, o);
   L->top++;
   lua_call(L, 1, 1);
   // Result is already at L->top-1 after lua_call
   return 1;
}

//********************************************************************************************************************
// Initialize thunk metatable

void lj_thunk_init(lua_State *L)
{
   // Create metatable
   lua_newtable(L);
   int mt_idx = lua_gettop(L);

   // Register metamethods
   lua_pushcfunction(L, thunk_add);
   lua_setfield(L, mt_idx, "__add");

   lua_pushcfunction(L, thunk_sub);
   lua_setfield(L, mt_idx, "__sub");

   lua_pushcfunction(L, thunk_mul);
   lua_setfield(L, mt_idx, "__mul");

   lua_pushcfunction(L, thunk_div);
   lua_setfield(L, mt_idx, "__div");

   lua_pushcfunction(L, thunk_mod);
   lua_setfield(L, mt_idx, "__mod");

   lua_pushcfunction(L, thunk_pow);
   lua_setfield(L, mt_idx, "__pow");

   lua_pushcfunction(L, thunk_unm);
   lua_setfield(L, mt_idx, "__unm");

   lua_pushcfunction(L, thunk_concat);
   lua_setfield(L, mt_idx, "__concat");

   lua_pushcfunction(L, thunk_eq);
   lua_setfield(L, mt_idx, "__eq");

   lua_pushcfunction(L, thunk_lt);
   lua_setfield(L, mt_idx, "__lt");

   lua_pushcfunction(L, thunk_le);
   lua_setfield(L, mt_idx, "__le");

   lua_pushcfunction(L, thunk_index);
   lua_setfield(L, mt_idx, "__index");

   lua_pushcfunction(L, thunk_newindex);
   lua_setfield(L, mt_idx, "__newindex");

   lua_pushcfunction(L, thunk_len);
   lua_setfield(L, mt_idx, "__len");

   lua_pushcfunction(L, thunk_call);
   lua_setfield(L, mt_idx, "__call");

   lua_pushcfunction(L, thunk_tostring);
   lua_setfield(L, mt_idx, "__tostring");

   // Store in registry
   lua_setfield(L, LUA_REGISTRYINDEX, THUNK_METATABLE_NAME);
}
