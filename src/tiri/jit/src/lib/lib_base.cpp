// Base library.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Major portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2011 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#include <stdio.h>
#include <cctype>

#define lib_base_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_ff.h"
#include "lj_dispatch.h"
#include "lj_char.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lib.h"
#include "lib_utils.h"
#include "lj_array.h"
#include "lj_vmarray.h"
#include "runtime/stack_utils.h"
#include "runtime/lj_thunk.h"
#include "runtime/lj_object.h"
#include "runtime/lj_proto_registry.h"
#include "debug/error_guard.h"

#define LJLIB_MODULE_base

//********************************************************************************************************************
// The implementation of assert() is a little strange in that it is specifically geared towards being optimised by
// the parser in optimise_assert().  The design relies on the message parameter being wrapped into a thunk, and then
// line and column numbers are appended as additional arguments for runtime formatting.
//
// It is not the intention that this implementation of assert() is called directly from the client (which should be
// impossible if the parser is functioning as expected).

LJLIB_ASM(assert)      LJLIB_REC(.)
{
   lj_lib_checkany(L, 1);
   if (L->top IS L->base + 1) {
      // No message provided - use default
      lj_err_caller(L, ErrMsg::ASSERT);
   }
   else {
      // Check for line/column arguments (args 3 and 4) added by optimise_assert()

      int32_t line = 0, column = 0;
      if (L->top >= L->base + 3 and tvisnum(L->base + 2)) line = int32_t(numV(L->base + 2));
      if (L->top >= L->base + 4 and tvisnum(L->base + 3)) column = int32_t(numV(L->base + 3));

      // Resolve the message if it's a thunk (lazy evaluation)
      TValue *msg_tv = L->base + 1;
      if (lj_is_thunk(msg_tv)) {
         TValue *resolved = lj_thunk_resolve(L, udataV(msg_tv));
         if (resolved) msg_tv = resolved;
      }

      if (line > 0) { // Format message with location prefix if line/column provided
         SBuf *sb = lj_buf_tmp_(L);
         lj_buf_putchar(sb, '[');
         lj_strfmt_putint(sb, line);
         lj_buf_putchar(sb, ':');
         lj_strfmt_putint(sb, column);
         lj_buf_putmem(sb, "] ", 2);

         // Append original message (handle nil as empty)
         if (tvisstr(msg_tv)) {
            GCstr *msg = strV(msg_tv);
            lj_buf_putmem(sb, strdata(msg), msg->len);
         }
         else if (tvisnum(msg_tv)) lj_strfmt_putfnum(sb, STRFMT_G14, numV(msg_tv));

         // nil or other types: append nothing (empty message after prefix)

         GCstr *formatted = lj_buf_str(L, sb);

         if (not L->sent_traceback) {
            // Inject traceback information into the first serious error message
            // Further tracebacks are not injected because it makes the log unnecessarily noisy.
            luaL_traceback(L, L, strdata(formatted), 1); // level 1 = skip assert itself
            L->sent_traceback = true;
         }
         else setstrV(L, L->top++, formatted);
      }
      else if (tvisstr(msg_tv) or tvisnumber(msg_tv)) {
         // No location info, use message as-is
         GCstr *msg = lj_lib_checkstr(L, 2);

         if (not L->sent_traceback) {
           luaL_traceback(L, L, strdata(msg), 1);
           L->sent_traceback = true;
         }
         else setstrV(L, L->top++, msg);
      }
      else { // No location info and message is nil or non-string - use default error
         lj_err_caller(L, ErrMsg::ASSERT);
      }
      lj_err_run(L);
   }
   return FFH_UNREACHABLE;
}

//********************************************************************************************************************
// ORDER LJ_T

LJLIB_PUSH("nil")
LJLIB_PUSH("boolean")
LJLIB_PUSH(top-1)  //  boolean
LJLIB_PUSH("userdata")
LJLIB_PUSH("string")
LJLIB_PUSH("upval")
LJLIB_PUSH("thread") // DEPRECATED
LJLIB_PUSH("proto")
LJLIB_PUSH("function")
LJLIB_PUSH("trace")
LJLIB_PUSH("object")
LJLIB_PUSH("table")
LJLIB_PUSH(top-9)  //  userdata
LJLIB_PUSH("array")
LJLIB_PUSH("number")
LJLIB_ASM(type)      LJLIB_REC(.)
{
   // C fallback for type() - handles thunks with declared types
   TValue *o = L->base;
   if (tvisudata(o)) {
      GCudata *ud = udataV(o);
      if (ud->udtype IS UDTYPE_THUNK) {
         ThunkPayload *payload = thunk_payload(ud);
         if (payload->expected_type != 0xFF) {
            // Use the declared type string from the upvalue array
            GCfunc *fn = funcV(L->base - 1 - LJ_FR2);
            GCstr *type_str = strV(&fn->c.upvalue[payload->expected_type]);
            setstrV(L, L->base - 1 - LJ_FR2, type_str);
            return FFH_RES(1);
         }
      }
   }
   // For non-thunk userdata, return "userdata" string (upvalue index 3)
   GCfunc *fn = funcV(L->base - 1 - LJ_FR2);
   setstrV(L, L->base - 1 - LJ_FR2, strV(&fn->c.upvalue[3]));
   return FFH_RES(1);
}

// Recycle the lj_lib_checkany(L, 1) from assert.

//********************************************************************************************************************
// Base library: iterators

static_assert((int)FF_next IS FF_next_N); // This solves a circular dependency problem -- change FF_next_N as needed.

LJLIB_ASM(next) LJLIB_REC(.) // Use of '.' indicates the function name in the recorder is unchanged
{
   TValue *o = lj_lib_checkany(L, 1);
   if (not (tvistab(o) or tvisarray(o))) lj_err_argt(L, 1, LUA_TTABLE);
   lj_err_msg(L, ErrMsg::NEXTIDX);
   return FFH_UNREACHABLE;
}

//********************************************************************************************************************

static int ffh_pairs(lua_State* L, MMS mm)
{
   TValue *o = lj_lib_checkany(L, 1);
   cTValue *mo = lj_meta_lookup(L, o, mm);
   if (not tvisnil(mo)) {
      L->top = o + 1;  //  Only keep one argument.
      copyTV(L, L->base - 2, mo);  //  Replace callable.
      return FFH_TAILCALL;
   }

   if ((tvisarray(o)) or (tvistab(o))) {
      copyTV(L, o - 1, o);
      o--;
      setfuncV(L, o - 1, funcV(lj_lib_upvalue(L, 1)));
      if (mm IS MM_pairs) setnilV(o + 1);
      else setintV(o + 1, -1);  // ipairs starts at -1, increments to 0
      return FFH_RES(3);
   }
   else if (tvisobject(o)) {
      // Direct integration for LJ_TOBJECT - iterate over field dictionary
      int nres = (mm IS MM_pairs) ? lj_object_pairs(L) : lj_object_ipairs(L);
      return FFH_RES(nres);
   }
   else lj_err_argt(L, 1, LUA_TTABLE);
}

//********************************************************************************************************************

LJLIB_PUSH(lastcl)
LJLIB_ASM(pairs)      LJLIB_REC(xpairs 0)
{
   return ffh_pairs(L, MM_pairs);
}

//********************************************************************************************************************

LJLIB_NOREGUV LJLIB_ASM(ipairs_aux)   LJLIB_REC(.)
{
   TValue* o = lj_lib_checkany(L, 1);
   if (not (tvistab(o) or tvisarray(o))) lj_err_argt(L, 1, LUA_TTABLE);
   lj_lib_checkint(L, 2);
   return FFH_UNREACHABLE;
}

//********************************************************************************************************************

LJLIB_PUSH(lastcl)
LJLIB_ASM(ipairs)      LJLIB_REC(xpairs 1)
{
   return ffh_pairs(L, MM_ipairs);
}

//********************************************************************************************************************
// values() iterator - iterates over table values only, discarding keys
// Usage: for v in values(tbl) do ... end
// Equivalent to: for _, v in pairs(tbl) do ... end

static int values_iterator_next(lua_State* L)
{
   // Upvalue 1: the table being iterated
   // Upvalue 2: state table containing the current key at index 0
   GCfunc *fn       = curr_func(L);
   GCtab *t         = tabV(&fn->c.upvalue[0]);
   GCtab *state     = tabV(&fn->c.upvalue[1]);
   TValue *key_slot = lj_tab_setint(L, state, 0);  // Get mutable slot

   TValue result[2];
   if (lj_tab_next(t, key_slot, result)) {
      copyTV(L, key_slot, &result[0]); // Update the key in state table for next iteration
      copyTV(L, L->top, &result[1]); // Return only the value
      L->top++;
      return 1;
   }
   return 0;  // End of iteration
}

//********************************************************************************************************************
// values() iterator for arrays - iterates over array values only
// Upvalue 1: the array being iterated
// Upvalue 2: current index (stored as integer, mutable)

static int values_array_iterator_next(lua_State* L)
{
   GCfunc *fn = curr_func(L);
   GCarray *arr = arrayV(&fn->c.upvalue[0]);
   TValue *idx_tv = &fn->c.upvalue[1];

   int32_t idx = numberVint(idx_tv);
   if (idx < 0 or MSize(idx) >= arr->len) return 0;  // End of iteration

   // Get the element value
   lj_arr_getidx(L, arr, idx, L->top);
   L->top++;

   setintV(idx_tv, idx + 1);  // Advance index for next iteration
   return 1;
}

LJLIB_CF(values)
{
   TValue* o = lj_lib_checkany(L, 1);

   if (tvistab(o)) {
      GCtab *t = tabV(o);

      settabV(L, L->top, t); // Push the table as upvalue 1
      L->top++;

      // Create state table to hold the mutable key (upvalue 2)
      GCtab* state = lj_tab_new(L, 0, 1);
      settabV(L, L->top, state);
      TValue* key_slot = lj_tab_setint(L, state, 0);
      setnilV(key_slot);
      L->top++;
      lua_pushcclosure(L, values_iterator_next, 2); // Create closure with 2 upvalues
   }
   else if (tvisarray(o)) {
      GCarray *arr = arrayV(o);
      setarrayV(L, L->top++, arr); // Push the array as upvalue 1
      setintV(L->top++, 0); // Push the starting index as upvalue 2
      lua_pushcclosure(L, values_array_iterator_next, 2); // Create closure with 2 upvalues
   }
   else lj_err_argt(L, 1, LUA_TTABLE);  // Expected table or array

   lua_pushnil(L);  // State (not used)
   lua_pushnil(L);  // Initial control variable
   return 3;
}

//********************************************************************************************************************
// keys() iterator - iterates over table keys only, discarding values
// Usage: for k in keys(tbl) do ... end
// Equivalent to: for k, _ in pairs(tbl) do ... end

static int keys_iterator_next(lua_State* L)
{
   // Upvalue 1: the table being iterated
   // Upvalue 2: state table containing the current key at index 0
   GCfunc *fn       = curr_func(L);
   GCtab *t         = tabV(&fn->c.upvalue[0]);
   GCtab *state     = tabV(&fn->c.upvalue[1]);
   TValue *key_slot = lj_tab_setint(L, state, 0);  // Get mutable slot

   TValue result[2];
   if (lj_tab_next(t, key_slot, result)) {
      copyTV(L, key_slot, &result[0]); // Update the key in state table for next iteration
      copyTV(L, L->top++, &result[0]); // Return only the key
      return 1;
   }
   return 0;  // End of iteration
}

LJLIB_CF(keys)
{
   GCtab *t = lj_lib_checktab(L, 1);

   // Push the table as upvalue 1
   settabV(L, L->top, t);
   L->top++;

   // Create state table to hold the mutable key (upvalue 2)
   GCtab *state = lj_tab_new(L, 0, 1);
   settabV(L, L->top, state);
   TValue *key_slot = lj_tab_setint(L, state, 0);
   setnilV(key_slot);
   L->top++;

   // Create closure with 2 upvalues
   lua_pushcclosure(L, keys_iterator_next, 2);
   lua_pushnil(L);  // State (not used)
   lua_pushnil(L);  // Initial control variable
   return 3;
}

//********************************************************************************************************************
// Base library: getters and setters

LJLIB_ASM_(getmetatable)   LJLIB_REC(.)

//********************************************************************************************************************
// Recycle the lj_lib_checkany(L, 1) from assert.

LJLIB_ASM(setmetatable)      LJLIB_REC(.)
{
   GCtab* t = lj_lib_checktab(L, 1);
   GCtab* mt = lj_lib_checktabornil(L, 2);
   if (!tvisnil(lj_meta_lookup(L, L->base, MM_metatable))) lj_err_caller(L, ErrMsg::PROTMT);
   setgcref(t->metatable, obj2gco(mt));
   if (mt) { lj_gc_objbarriert(L, t, mt); }
   settabV(L, L->base - 2, t);
   return FFH_RES(1);
}

//********************************************************************************************************************

LJLIB_ASM(rawget)      LJLIB_REC(.)
{
   lj_lib_checktab(L, 1);
   lj_lib_checkany(L, 2);
   return FFH_UNREACHABLE;
}

//********************************************************************************************************************

LJLIB_CF(rawset)      LJLIB_REC(.)
{
   lj_lib_checktab(L, 1);
   lj_lib_checkany(L, 2);
   L->top = 1 + lj_lib_checkany(L, 3);
   lua_rawset(L, 1);
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(rawequal)      LJLIB_REC(.)
{
   cTValue *o1 = lj_lib_checkany(L, 1);
   cTValue *o2 = lj_lib_checkany(L, 2);
   setboolV(L->top - 1, lj_obj_equal(o1, o2));
   return 1;
}

//********************************************************************************************************************

LJLIB_CF(rawlen)      LJLIB_REC(.)
{
   cTValue *o = L->base;
   int32_t len;
   if (L->top > o and tvisstr(o)) len = (int32_t)strV(o)->len;
   else len = (int32_t)lj_tab_len(lj_lib_checktab(L, 1));
   setintV(L->top - 1, len);
   return 1;
}

//********************************************************************************************************************
// __filter(mask, count, trailing_keep, ...)
// Filters return values based on a bitmask pattern.
// mask: uint64 bitmask where bit N=1 means keep value at position N
// count: number of explicitly specified positions in the pattern
// trailing_keep: true if excess values should be kept, false to drop
//
// RAII Pattern: Uses StackFrame to ensure L->top is restored on error paths.
// The frame automatically cleans up if an error is thrown, preventing stack leaks.

LJLIB_CF(__filter)      LJLIB_REC(.)
{
   StackFrame frame(L);

   int32_t nargs = int32_t(L->top - L->base);
   require_arg_count(L, 3);

   // Extract filter parameters
   uint64_t mask = uint64_t(lj_lib_checknum(L, 1));
   int32_t count = lj_lib_checkint(L, 2);
   bool trailing_keep = tvistruecond(L->base + 2);

   // Values to filter start at position 4 (index 3, 0-based)
   int32_t value_count = nargs - 3;

   // First pass: count how many values we'll keep (for stack check)

   int32_t out_count = 0;
   for (int32_t i = 0; i < value_count; i++) {
      bool keep = (i < count) ? ((mask & (1ULL << i)) != 0) : trailing_keep;
      if (keep) out_count++;
   }

   // Ensure we have enough stack space

   if (out_count > 0 and !lua_checkstack(L, out_count)) {
      lj_err_caller(L, ErrMsg::STKOV);
      return 0;  // StackFrame destructor will restore L->top
   }

   // Move kept values into position at L->base (overwriting the args)

   TValue *src = L->base + 3;  // Values start after mask, count, trailing_keep
   TValue *dst = L->base;      // Overwrite from the start

   int32_t written = 0;
   for (int32_t i = 0; i < value_count; i++) {
      bool keep = (i < count) ? ((mask & (1ULL << i)) != 0) : trailing_keep;
      if (keep) {
         if (dst + written != src + i) copyTV(L, dst + written, src + i);
         written++;
      }
   }

   // Adjust L->top to reflect the number of returns
   // Note: We manually set L->top here because the results start at L->base, not at the saved_top position.

   L->top = L->base + written;
   frame.disarm();  // Disarm the guard since we manually set L->top

   return written;
}

//********************************************************************************************************************
// Base library: conversions

LJLIB_ASM(tonumber)      LJLIB_REC(.)
{
   int32_t base = lj_lib_optint(L, 2, 10);
   if (base IS 10) {
      TValue* o = lj_lib_checkany(L, 1);
      if (lj_strscan_numberobj(o)) {
         copyTV(L, L->base - 2, o);
         return FFH_RES(1);
      }
   }
   else {
      CSTRING p = strdata(lj_lib_checkstr(L, 1));
      char* ep;
      unsigned int neg = 0;
      unsigned long ul;
      LJ_CHECK_RANGE(L, 2, base, 2, 36, ErrMsg::BASERNG);
      while (isspace((unsigned char)(*p))) p++;
      if (*p IS '-') { p++; neg = 1; }
      else if (*p IS '+') { p++; }
      if (isalnum((unsigned char)(*p))) {
         ul = strtoul(p, &ep, base);
         if (p != ep) {
            while (isspace((unsigned char)(*ep))) ep++;
            if (*ep IS '\0') {
               if (LJ_DUALNUM and LJ_LIKELY(ul < 0x80000000u + neg)) {
                  if (neg) ul = (unsigned long)-(long)ul;
                  setintV(L->base - 2, (int32_t)ul);
               }
               else {
                  lua_Number n = (lua_Number)ul;
                  if (neg) n = -n;
                  setnumV(L->base - 2, n);
               }
               return FFH_RES(1);
            }
         }
      }
   }
   setnilV(L->base - 2);
   return FFH_RES(1);
}

//********************************************************************************************************************

LJLIB_ASM(tostring)      LJLIB_REC(.)
{
   StackFrame frame(L);

   TValue *o = lj_lib_checkany(L, 1);
   cTValue *mo;
   L->top = o + 1;  //  Only keep one argument.

   if (!tvisnil(mo = lj_meta_lookup(L, o, MM_tostring))) {
      copyTV(L, L->base - 2, mo);  //  Replace callable.
      frame.disarm();  // Disarm before tail call
      return FFH_TAILCALL;
   }

   lj_gc_check(L);
   setstrV(L, L->base - 2, lj_strfmt_obj(L, L->base));
   frame.disarm();  // Disarm - result already in place
   return FFH_RES(1);
}

//********************************************************************************************************************
// Base library: throw and catch errors

LJLIB_CF(error)
{
   int32_t level = lj_lib_optint(L, 2, 1);
   lua_settop(L, 1);

   // Handle exception tables (as received by 'except' keyword) by extracting the message field
   // The error code will remain in the lua_State, so does not require management.
   // TODO: There is room for improvement (e.g. retaining stack traces if a stack trace is present) but this will do for
   // now - a rewrite of exception management is probably in order later.

   if (lua_istable(L, 1)) {
      lua_getfield(L, 1, "message");
      if (lua_isstring(L, -1)) lua_replace(L, 1);  // Replace the table with the message string
      else lua_pop(L, 1);  // Pop the nil/non-string value, keep original table
   }

   // Handle regular string errors.
   if (lua_isstring(L, 1) and level > 0) {
      luaL_where(L, level);
      lua_pushvalue(L, 1);
      lua_concat(L, 2);
   }
   return lua_error(L);
}

//********************************************************************************************************************

LJLIB_CF(collectgarbage)
{
   pf::Log("collectgarbage").warning("DEPRECATED - Use processing.collect()");
   return 0;
}

//********************************************************************************************************************
// Base library: miscellaneous functions

LJLIB_PUSH(top-2)  //  Upvalue holds weak table.
LJLIB_CF(newproxy)
{
   lua_settop(L, 1);
   lua_newuserdata(L, 0);
   if (lua_toboolean(L, 1) IS 0) {  // newproxy(): without metatable.
      return 1;
   }
   else if (lua_isboolean(L, 1)) {  // newproxy(true): with metatable.
      lua_newtable(L);
      lua_pushvalue(L, -1);
      lua_pushboolean(L, 1);
      lua_rawset(L, lua_upvalueindex(1));  //  Remember mt in weak table.
   }
   else {  // newproxy(proxy): inherit metatable.
      int validproxy = 0;
      if (lua_getmetatable(L, 1)) {
         lua_rawget(L, lua_upvalueindex(1));
         validproxy = lua_toboolean(L, -1);
         lua_pop(L, 1);
      }
      if (!validproxy) lj_err_arg(L, 1, ErrMsg::NOPROXY);
      lua_getmetatable(L, 1);
   }
   lua_setmetatable(L, 2);
   return 1;
}

//********************************************************************************************************************
// RAII Pattern: Uses StackFrame to ensure L->top is restored if tostring conversion
// fails or triggers an error during the print loop, preventing stack corruption.

LJLIB_PUSH("tostring")
LJLIB_CF(print)
{
   StackFrame frame(L);

   ptrdiff_t i, nargs = L->top - L->base;
   cTValue* tv = lj_tab_getstr(tabref(L->env), strV(lj_lib_upvalue(L, 1)));
   int shortcut;
   if (tv and !tvisnil(tv)) {
      copyTV(L, L->top++, tv);
   }
   else {
      setstrV(L, L->top++, strV(lj_lib_upvalue(L, 1)));
      lua_gettable(L, LUA_GLOBALSINDEX);
      tv = L->top - 1;
   }

   shortcut = (tvisfunc(tv) and funcV(tv)->c.ffid IS FF_tostring) and !gcrefu(basemt_it(G(L), LJ_TNUMX));

   for (i = 0; i < nargs; i++) {
      cTValue* o = &L->base[i];
      CSTRING str;
      size_t size;
      MSize len;

      if (shortcut and (str = lj_strfmt_wstrnum(L, o, &len)) != nullptr) {
         size = len;
      }
      else {
         copyTV(L, L->top + 1, o);
         copyTV(L, L->top, L->top - 1);
         L->top += 2;
         lua_call(L, 1, 1);
         str = lua_tolstring(L, -1, &size);
         if (!str) lj_err_caller(L, ErrMsg::PRTOSTR);  // StackFrame will restore L->top
         L->top--;
      }

      if (i) putchar('\t');
      fwrite(str, 1, size, stdout);
   }
   putchar('\n');
   frame.commit(0);  // No return values
   return 0;
}

LJLIB_PUSH(top-3)
LJLIB_SET(_VERSION)

//********************************************************************************************************************
// Check if a value is an unresolved thunk

LJLIB_CF(isthunk)
{
   cTValue *o = lj_lib_checkany(L, 1);
   setboolV(L->top++, lj_is_thunk(o));
   return 1;
}

//********************************************************************************************************************
// Explicitly resolve a thunk (returns the value unchanged if not a thunk)

LJLIB_CF(resolve)
{
   cTValue *o = lj_lib_checkany(L, 1);

   if (lj_is_thunk(o)) {
      GCudata *ud = udataV(o);
      TValue *resolved = lj_thunk_resolve(L, ud);
      copyTV(L, L->top++, resolved);
      return 1;
   }

   // Not a thunk - return as-is
   copyTV(L, L->top++, o);
   return 1;
}

//********************************************************************************************************************
// Internal function for creating thunk userdata (called by IR emitter)
// Args: (closure:function, expected_type:number)
// Returns: thunk userdata

LJLIB_CF(__create_thunk)
{
   GCfunc *fn = lj_lib_checkfunc(L, 1);
   int expected_type = (int)lj_lib_checkint(L, 2);
   lj_thunk_new(L, fn, expected_type);
   return 1;
}

//********************************************************************************************************************
// ltr() - Lua pattern To Regex conversion
// Converts a Lua string pattern to an equivalent regex expression.
// Usage: regex_str = ltr("%d+%s*%w+")  -- Returns "[0-9]+[ \t\n\r\f\v]*[A-Za-z0-9_]+"

static void ltr_emit_class(SBuf *sb, int cl, bool negated)
{
   // Emit regex character class for Lua pattern class character
   // cl is the lowercase class character (a, c, d, g, l, p, s, u, w, x, z)
   // negated is true for uppercase versions (A, C, D, etc.)

   if (negated) lj_buf_putmem(sb, "[^", 2);
   else lj_buf_putchar(sb, '[');

   switch (cl) {
      case 'a': lj_buf_putmem(sb, "A-Za-z", 6); break;
      case 'c': lj_buf_putmem(sb, "\\x00-\\x1f\\x7f", 13); break;
      case 'd': lj_buf_putmem(sb, "\\d", 2); break;
      case 'g': lj_buf_putmem(sb, "\\x21-\\x7e", 9); break;
      case 'l': lj_buf_putmem(sb, "a-z", 3); break;
      case 'p': lj_buf_putmem(sb, "!\"#$%&'()*+,\\-./:;<=>?@\\[\\\\\\]^_`{|}~", sizeof("!\"#$%&'()*+,\\-./:;<=>?@\\[\\\\\\]^_`{|}~")-1); break;
      case 's': lj_buf_putmem(sb, " \\t\\n\\r\\f\\v", sizeof(" \\t\\n\\r\\f\\v")-1); break;
      case 'u': lj_buf_putmem(sb, "A-Z", 3); break;
      case 'w': lj_buf_putmem(sb, "\\w", 2); break;
      case 'x': lj_buf_putmem(sb, "0-9A-Fa-f", 9); break;
      case 'z': lj_buf_putmem(sb, "\\x00", 4); break;
      default: lj_buf_putchar(sb, cl); break;  // Unknown class, output as literal
   }

   lj_buf_putchar(sb, ']');
}

static bool ltr_is_regex_special(int c)
{
   // Characters that need escaping in regex (but not necessarily in Lua patterns)
   return c IS '\\' or c IS '|' or c IS '{' or c IS '}';
}

LJLIB_CF(ltr)
{
   GCstr *input = lj_lib_checkstr(L, 1);
   const char *p = strdata(input);
   const char *end = p + input->len;

   SBuf *sb = lj_buf_tmp_(L);
   lj_buf_reset(sb);

   while (p < end) {
      int c = (uint8_t)*p++;

      if (c IS '%') { // Escape sequence
         if (p >= end) {
            lj_err_caller(L, ErrMsg::STRPATE);  // Pattern ends with '%'
            return 0;
         }
         int cl = (uint8_t)*p++;

         // Check for unsupported patterns

         if (cl IS 'b') {
            lj_err_callermsg(L, "Unsupported Lua pattern: %b (balanced matching) has no regex equivalent");
            return 0;
         }

         if (cl IS 'f') {
            lj_err_callermsg(L, "Unsupported Lua pattern: %f (frontier pattern) has no regex equivalent");
            return 0;
         }

         // Handle character classes
         int lower_cl = std::tolower(cl);
         if (lower_cl IS 'a' or lower_cl IS 'c' or lower_cl IS 'd' or lower_cl IS 'g' or
             lower_cl IS 'l' or lower_cl IS 'p' or lower_cl IS 's' or lower_cl IS 'u' or
             lower_cl IS 'w' or lower_cl IS 'x' or lower_cl IS 'z') {
            ltr_emit_class(sb, lower_cl, std::isupper(cl));
         }
         else if (cl IS '%') { // %% -> literal %
            lj_buf_putchar(sb, '%');
         }
         else if (cl IS '-') { // %- -> literal hyphen (no escape needed outside char class)
            lj_buf_putchar(sb, '-');
         }
         else {
            // Escaped special character - emit as regex escape
            // Lua escapes: ( ) . % + - * ? [ ] ^ $
            // In regex, we need to escape these with backslash
            lj_buf_putchar(sb, '\\');
            lj_buf_putchar(sb, cl);
         }
      }
      else if (c IS '-') {
         // Lua's non-greedy quantifier - convert to *?
         lj_buf_putmem(sb, "*?", 2);
      }
      else if (c IS '[') {
         // Character class - copy through, handling nested escapes
         lj_buf_putchar(sb, '[');
         bool first = true;
         bool found_close = false;
         while (p < end) {
            c = (uint8_t)*p++;
            if (c IS ']' and not first) {
               lj_buf_putchar(sb, ']');
               found_close = true;
               break;
            }

            if (c IS '%' and p < end) { // Escaped character inside class
               int esc = (uint8_t)*p++;
               int lower_esc = std::tolower(esc);
               if (lower_esc IS 'a' or lower_esc IS 'c' or lower_esc IS 'd' or lower_esc IS 'g' or
                   lower_esc IS 'l' or lower_esc IS 'p' or lower_esc IS 's' or lower_esc IS 'u' or
                   lower_esc IS 'w' or lower_esc IS 'x' or lower_esc IS 'z') {
                  // Expand class inside character class (without brackets)
                  bool neg = std::isupper(esc);
                  if (neg) { // Can't easily negate inside a character class, emit as-is with warning
                     lj_buf_putchar(sb, '\\');
                     lj_buf_putchar(sb, esc);
                  }
                  else {
                     switch (lower_esc) {
                        case 'a': lj_buf_putmem(sb, "A-Za-z", 6); break;
                        case 'c': lj_buf_putmem(sb, "\\x00-\\x1f\\x7f", 13); break;
                        case 'd': lj_buf_putmem(sb, "\\d", 2); break;
                        case 'g': lj_buf_putmem(sb, "\\x21-\\x7e", 9); break;
                        case 'l': lj_buf_putmem(sb, "a-z", 3); break;
                        case 'p': lj_buf_putmem(sb, "!\"#$%&'()*+,\\-./:;<=>?@\\[\\\\\\]^_`{|}~", sizeof("!\"#$%&'()*+,\\-./:;<=>?@\\[\\\\\\]^_`{|}~")-1); break;
                        case 's': lj_buf_putmem(sb, " \\t\\n\\r\\f\\v", sizeof(" \\t\\n\\r\\f\\v")-1); break;
                        case 'u': lj_buf_putmem(sb, "A-Z", 3); break;
                        case 'w': lj_buf_putmem(sb, "\\w", 2); break;
                        case 'x': lj_buf_putmem(sb, "0-9A-Fa-f", 9); break;
                        case 'z': lj_buf_putmem(sb, "\\x00", 4); break;
                        default: lj_buf_putchar(sb, lower_esc); break;
                     }
                  }
               }
               else if (esc IS '%') {
                  lj_buf_putchar(sb, '%');
               }
               else { // Other escaped char
                  lj_buf_putchar(sb, '\\');
                  lj_buf_putchar(sb, esc);
               }
            }
            else { // Regular character inside class
               // Escape '-' when it's a literal hyphen (at start or end of class), not a range operator
               bool escape_hyphen = (c IS '-') and (first or (p < end and *p IS ']'));
               if (ltr_is_regex_special(c) or escape_hyphen) lj_buf_putchar(sb, '\\');
               lj_buf_putchar(sb, c);
            }
            first = false;
         }
         if (not found_close) lj_err_caller(L, ErrMsg::STRPATM);
      }
      else if (ltr_is_regex_special(c)) { // Escape regex-special chars that aren't Lua-special
         lj_buf_putchar(sb, '\\');
         lj_buf_putchar(sb, c);
      }
      else { // Regular character - pass through
         lj_buf_putchar(sb, c);
      }
   }

   setstrV(L, L->top - 1, lj_buf_str(L, sb));
   lj_gc_check(L);
   return 1;
}

#include "lj_libdef.h"

//********************************************************************************************************************

static void newproxy_weaktable(lua_State* L)
{
   // NOBARRIER: The table is new (marked white).
   GCtab *t = lj_tab_new(L, 0, 1);
   settabV(L, L->top++, t);
   setgcref(t->metatable, obj2gco(t));
   setstrV(L, lj_tab_setstr(L, t, lj_str_newlit(L, "__mode")), lj_str_newlit(L, "kv"));
   t->nomm = (uint8_t)(~(1u << MM_mode));
}

//********************************************************************************************************************

extern int luaopen_base(lua_State* L)
{
   // NOBARRIER: Table and value are the same.
   GCtab *env = tabref(L->env);
   settabV(L, lj_tab_setstr(L, env, lj_str_newlit(L, "_G")), env);
   lua_pushliteral(L, "5.4");  //  top-3. // Lua version number, set as _VERSION
   newproxy_weaktable(L);  //  top-2.
   LJ_LIB_REG(L, "_G", base);

   // Register function prototypes for compile-time type inference
   reg_func_prototype("print", { }, {}, FProtoFlags::Variadic);
   reg_func_prototype("assert", { TiriType::Any }, { TiriType::Any, TiriType::Str });
   reg_func_prototype("type", { TiriType::Str }, { TiriType::Any });
   reg_func_prototype("tonumber", { TiriType::Num }, { TiriType::Any, TiriType::Num });
   reg_func_prototype("tostring", { TiriType::Str }, { TiriType::Any });
   reg_func_prototype("pairs", { TiriType::Func, TiriType::Table, TiriType::Nil }, { TiriType::Any });
   reg_func_prototype("ipairs", { TiriType::Func, TiriType::Table, TiriType::Num }, { TiriType::Any });
   reg_func_prototype("values", { TiriType::Func, TiriType::Table, TiriType::Num }, { TiriType::Any });
   reg_func_prototype("rawget", { TiriType::Any }, { TiriType::Table, TiriType::Any });
   reg_func_prototype("rawset", { TiriType::Table }, { TiriType::Table, TiriType::Any, TiriType::Any });
   reg_func_prototype("error", { }, { TiriType::Any }, FProtoFlags::NoNil);
   reg_func_prototype("getmetatable", { TiriType::Any }, { TiriType::Any });
   reg_func_prototype("setmetatable", { TiriType::Table }, { TiriType::Table, TiriType::Table });
   reg_func_prototype("select", { TiriType::Any }, { TiriType::Any }, FProtoFlags::Variadic);
   reg_func_prototype("next", { TiriType::Any, TiriType::Any }, { TiriType::Table, TiriType::Any });
   reg_func_prototype("newproxy", { TiriType::Any }, { TiriType::Any });
   reg_func_prototype("__create_thunk", { TiriType::Any }, { TiriType::Func, TiriType::Num });
   reg_func_prototype("ltr", { TiriType::Str }, { TiriType::Str });

   return 2;
}
