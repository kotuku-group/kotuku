// Metamethod handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#define lj_meta_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_vm.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lj_thunk.h"
#include "lj_object.h"
#include "stack_helpers.h"
#include "lib.h"

//********************************************************************************************************************
// Convert LuaJIT internal type tag to TiriType for runtime type inference.
// Called by BC_TYPEFIX to fix a function's return type based on actual value.

static TiriType lj_tag_to_tiri_type(uint32_t tag)
{
   switch (tag) {
      case LJ_TNIL:    return TiriType::Nil;
      case LJ_TFALSE:
      case LJ_TTRUE:   return TiriType::Bool;
      case LJ_TSTR:    return TiriType::Str;
      case LJ_TTHREAD: return TiriType::Thread;
      case LJ_TFUNC:   return TiriType::Func;
      case LJ_TOBJECT: return TiriType::Object;
      case LJ_TTAB:    return TiriType::Table;
      case LJ_TUDATA:  return TiriType::Object;
      case LJ_TARRAY:  return TiriType::Array;
      default:         return TiriType::Num;  // Numbers have itype < LJ_TISNUM
   }
}

//********************************************************************************************************************
// String interning of metamethod names for fast indexing.

void lj_meta_init(lua_State *L)
{
#define MMNAME(name)   "__" #name
   const char * metanames = MMDEF(MMNAME);
#undef MMNAME
   global_State *g = G(L);
   const char *p, *q;
   uint32_t mm;
   for (mm = 0, p = metanames; *p; mm++, p = q) {
      GCstr *s;
      for (q = p + 2; *q and *q != '_'; q++);
      s = lj_str_new(L, p, (size_t)(q - p));
      // NOBARRIER: g->gcroot[] is a GC root.
      setgcref(g->gcroot[GCROOT_MMNAME + mm], obj2gco(s));
   }
}

//********************************************************************************************************************
// Negative caching of a few fast metamethods. See the lj_meta_fast() macro.

cTValue *lj_meta_cache(GCtab* mt, MMS mm, GCstr *name)
{
   cTValue *mo = lj_tab_getstr(mt, name);
   lj_assertX(mm <= MM_FAST, "bad metamethod %d", mm);
   if (not mo or tvisnil(mo)) {  // No metamethod?
      mt->nomm |= (uint8_t)(1u << mm);  //  Set negative cache flag.
      return nullptr;
   }
   return mo;
}

//********************************************************************************************************************
// Lookup metamethod for object.

cTValue * lj_meta_lookup(lua_State *L, cTValue *o, MMS mm)
{
   GCtab *mt;
   if (tvistab(o)) mt = tabref(tabV(o)->metatable);
   else if (tvisudata(o)) mt = tabref(udataV(o)->metatable);
   else if (tvisarray(o)) {
      // Check per-instance metatable first, then fall back to base metatable
      mt = tabref(arrayV(o)->metatable);
      if (not mt) mt = tabref(basemt_it(G(L), LJ_TARRAY));
   }
   else if (tvisobject(o)) {
      // Check per-instance metatable first, then fall back to base metatable
      mt = tabref(objectV(o)->metatable);
      if (not mt) mt = tabref(basemt_it(G(L), LJ_TOBJECT));
   }
   else mt = tabref(basemt_obj(G(L), o));

   if (mt) {
      cTValue *mo = lj_tab_getstr(mt, mmname_str(G(L), mm));
      if (mo) return mo;
   }
   return niltv(L);
}

//********************************************************************************************************************

#if LJ_HASFFI
// Tailcall from C function.
int lj_meta_tailcall(lua_State *L, cTValue *tv)
{
   TValue *base = L->base;
   TValue *top = L->top;
   const BCIns *pc = frame_pc(base - 1);  //  Preserve old PC from frame.
   copyTV(L, base - 1 - LJ_FR2, tv);  //  Replace frame with new object.
   (top++)->u64 = LJ_CONT_TAILCALL;
   setframe_pc(top++, pc);
   setframe_gc(top, obj2gco(L), LJ_TTHREAD);  //  Dummy frame object.
   top++;
   setframe_ftsz(top, ((char*)(top + 1) - (char*)base) + FRAME_CONT);
   L->base = L->top = top + 1;
   /*
   ** before:   [old_mo|PC]    [... ...]
   **                         ^base     ^top
   ** after:    [new_mo|itype] [... ...] [nullptr|PC] [dummy|delta]
   **                                                           ^base/top
   ** tailcall: [new_mo|PC]    [... ...]
   **                         ^base     ^top
   */
   return 0;
}
#endif

//********************************************************************************************************************
// Setup call to metamethod to be run by Assembler VM.

TValue * mmcall(lua_State *L, ASMFunction cont, cTValue *mo, cTValue *a, cTValue *b)
{
   //           |-- framesize -> top       top+1       top+2 top+3
   // before:   [func slots ...]
   // mm setup: [func slots ...] [cont|?]  [mo|tmtype] [a]   [b]
   // in asm:   [func slots ...] [cont|PC] [mo|delta]  [a]   [b]
   //           ^-- func base                          ^-- mm base
   // after mm: [func slots ...]           [result]
   //                ^-- copy to base[PC_RA] --/     for lj_cont_ra
   //                          istruecond + branch   for lj_cont_cond*
   //                                       ignore   for lj_cont_nop
   // next PC:  [func slots ...]

   TValue *top = L->top;
   if (curr_funcisL(L)) top = curr_topL(L);
   setcont(top++, cont);  //  Assembler VM stores PC in upper word or FR2.
   setnilV(top++);
   copyTV(L, top++, mo);  //  Store metamethod and two arguments.
   setnilV(top++);
   copyTV(L, top, a);
   copyTV(L, top + 1, b);
   return top;  //  Return new base.
}

//********************************************************************************************************************
// Helpers for some instructions, called from assembler VM

// Helper for TGET*. __index chain and metamethod.

cTValue *lj_meta_tget(lua_State *L, cTValue *o, cTValue *k)
{
   // Resolve thunk keys before table lookup.
   if (lj_is_thunk(k)) {
      VMHelperGuard guard(L);
      ptrdiff_t o_ofs = savestack(L, o);
      k = lj_thunk_resolve(L, udataV(k));
      o = restorestack(L, o_ofs);
   }

   int loop;
   for (loop = 0; loop < LJ_MAX_IDXCHAIN; loop++) {
      cTValue *mo;
      if (tvistab(o)) [[likely]] {
         GCtab *t = tabV(o);
         cTValue *tv = lj_tab_get(L, t, k);
         if (not tvisnil(tv) or !(mo = lj_meta_fast(L, tabref(t->metatable), MM_index))) return tv;
      }
      else if (tvisobject(o)) {
         mo = lj_meta_fast(L, tabref(basemt_it(G(L), LJ_TOBJECT)), MM_index);
         if (tvisnil(mo)) {
            lj_err_optype(L, o, ErrMsg::OPINDEX);
            return nullptr;
         }
      }
      else if (tvisnil(mo = lj_meta_lookup(L, o, MM_index))) {
         lj_err_optype(L, o, ErrMsg::OPINDEX);
         return nullptr;  //  unreachable
      }

      if (tvisfunc(mo)) {
         L->top = mmcall(L, lj_cont_ra, mo, o, k);
         return nullptr;  //  Trigger metamethod call.
      }
      o = mo;
   }
   lj_err_msg(L, ErrMsg::GETLOOP);
   return nullptr;  //  unreachable
}

//********************************************************************************************************************
// Helper for TSET*. __newindex chain and metamethod.

TValue * lj_meta_tset(lua_State *L, cTValue *o, cTValue *k)
{
   // Resolve thunk keys before table lookup.
   if (lj_is_thunk(k)) {
      VMHelperGuard guard(L);
      ptrdiff_t o_ofs = savestack(L, o);
      k = lj_thunk_resolve(L, udataV(k));
      o = restorestack(L, o_ofs);
   }

   TValue tmp;
   int loop;
   for (loop = 0; loop < LJ_MAX_IDXCHAIN; loop++) {
      cTValue *mo;
      if (tvistab(o)) [[likely]] {
         GCtab *t = tabV(o);
         cTValue *tv = lj_tab_get(L, t, k);
         if (not tvisnil(tv)) [[likely]] {
            t->nomm = 0;  //  Invalidate negative metamethod cache.
            lj_gc_anybarriert(L, t);
            return (TValue *)tv;
         }
         else if (not (mo = lj_meta_fast(L, tabref(t->metatable), MM_newindex))) {
            t->nomm = 0;  //  Invalidate negative metamethod cache.
            lj_gc_anybarriert(L, t);
            if (tv != niltv(L)) return (TValue *)tv;
            if (tvisnil(k)) lj_err_msg(L, ErrMsg::NILIDX);
            else if (tvisint(k)) { setnumV(&tmp, (lua_Number)intV(k)); k = &tmp; }
            else if (tvisnum(k) and tvisnan(k)) lj_err_msg(L, ErrMsg::NANIDX);
            return lj_tab_newkey(L, t, k);
         }
      }
      else if (tvisobject(o)) {
         mo = lj_meta_fast(L, tabref(basemt_it(G(L), LJ_TOBJECT)), MM_newindex);
         if (tvisnil(mo)) {
            lj_err_optype(L, o, ErrMsg::OPINDEX);
            return nullptr;
         }
      }
      else if (tvisnil(mo = lj_meta_lookup(L, o, MM_newindex))) {
         lj_err_optype(L, o, ErrMsg::OPINDEX);
         return nullptr;  //  unreachable
      }

      if (tvisfunc(mo)) {
         L->top = mmcall(L, lj_cont_nop, mo, o, k);
         // L->top+2 = v filled in by caller.
         return nullptr;  //  Trigger metamethod call.
      }

      copyTV(L, &tmp, mo);
      o = &tmp;
   }

   lj_err_msg(L, ErrMsg::SETLOOP);
   return nullptr;  //  unreachable
}

//********************************************************************************************************************

static cTValue * str2num(cTValue *o, TValue *n)
{
   if (tvisnum(o)) return o;
   else if (tvisint(o)) return (setnumV(n, (lua_Number)intV(o)), n);
   else if (tvisstr(o) and lj_strscan_num(strV(o), n)) return n;
   else return nullptr;
}

//********************************************************************************************************************
// Helper for arithmetic instructions. Coercion, metamethod.

TValue * lj_meta_arith(lua_State *L, TValue *ra, cTValue *rb, cTValue *rc, BCREG op)
{
   MMS mm = bcmode_mm(op);
   TValue tempb, tempc;
   cTValue *b, * c;
   if ((b = str2num(rb, &tempb)) != nullptr and (c = str2num(rc, &tempc)) != nullptr) {  // Try coercion first.
      setnumV(ra, lj_vm_foldarith(numV(b), numV(c), (int)mm - MM_add));
      return nullptr;
   }
   else {
      cTValue *mo = lj_meta_lookup(L, rb, mm);
      if (tvisnil(mo)) {
         mo = lj_meta_lookup(L, rc, mm);
         if (tvisnil(mo)) {
            if (str2num(rb, &tempb) IS nullptr) rc = rb;
            lj_err_optype(L, rc, ErrMsg::OPARITH);
            return nullptr;  //  unreachable
         }
      }
      return mmcall(L, lj_cont_ra, mo, rb, rc);
   }
}

//********************************************************************************************************************
// Helper for CAT. Coercion, iterative concat, __concat metamethod.

TValue * lj_meta_cat(lua_State *L, TValue *top, int left)
{
   int fromc = 0;
   if (left < 0) { left = -left; fromc = 1; }

   // Convert nil to empty string for non-first operands only.
   // The first operand (leftmost in source) must be a valid string/number to establish
   // that we're doing string concatenation. Subsequent nil values become empty strings.

   GCstr *empty_str = &G(L)->strempty;

   do {
      // Always allow nil-to-empty for the right operand (not the first value)
      if (tvisnil(top)) setstrV(L, top, empty_str);
      // Only convert left operand if it's not the first value in the chain
      if (tvisnil(top - 1) and left > 1) setstrV(L, top - 1, empty_str);

      if (not (tvisstr(top) or tvisnumber(top)) or !(tvisstr(top - 1) or tvisnumber(top - 1))) {
         cTValue *mo = lj_meta_lookup(L, top - 1, MM_concat);
         if (tvisnil(mo)) {
            mo = lj_meta_lookup(L, top, MM_concat);
            if (tvisnil(mo)) {
               if (tvisstr(top - 1) or tvisnumber(top - 1)) top++;
               lj_err_optype(L, top - 1, ErrMsg::OPCAT);
               return nullptr;  //  unreachable
            }
         }

         // One of the top two elements is not a string, call __cat metamethod:
         //
         // before:    [...][CAT stack .........................]
         //                                 top-1     top         top+1 top+2
         // pick two:  [...][CAT stack ...] [o1]      [o2]
         // setup mm:  [...][CAT stack ...] [cont|?]  [mo|tmtype] [o1]  [o2]
         // in asm:    [...][CAT stack ...] [cont|PC] [mo|delta]  [o1]  [o2]
         //            ^-- func base                              ^-- mm base
         // after mm:  [...][CAT stack ...] <--push-- [result]
         // next step: [...][CAT stack .............]

         copyTV(L, top + 2 * LJ_FR2 + 2, top);  //  Carefully ordered stack copies!
         copyTV(L, top + 2 * LJ_FR2 + 1, top - 1);
         copyTV(L, top + LJ_FR2, mo);
         setcont(top - 1, lj_cont_cat);
         setnilV(top);
         setnilV(top + 2);
         top += 2;
         return top + 1;  //  Trigger metamethod call.
      }
      else {
         // Pick as many strings as possible from the top and concatenate them:
         //
         // before:    [...][CAT stack ...........................]
         // pick str:  [...][CAT stack ...] [...... strings ......]
         // concat:    [...][CAT stack ...] [result]
         // next step: [...][CAT stack ............]

         TValue *e, * o = top;
         uint64_t tlen = tvisstr(o) ? strV(o)->len : STRFMT_MAXBUF_NUM;
         SBuf* sb;
         do {
            o--;
            tlen += tvisstr(o) ? strV(o)->len : STRFMT_MAXBUF_NUM;
         } while (--left > 0 and (tvisstr(o - 1) or tvisnumber(o - 1)));

         if (tlen >= LJ_MAX_STR) lj_err_msg(L, ErrMsg::STROV);
         sb = lj_buf_tmp_(L);
         (void)lj_buf_more(sb, (MSize)tlen);

         for (e = top, top = o; o <= e; o++) {
            if (tvisstr(o)) {
               GCstr *s = strV(o);
               MSize len = s->len;
               lj_buf_putmem(sb, strdata(s), len);
            }
            else if (tvisint(o)) lj_strfmt_putint(sb, intV(o));
            else lj_strfmt_putfnum(sb, STRFMT_G14, numV(o));
         }

         setstrV(L, top, lj_buf_str(L, sb));
      }
   } while (left >= 1);

   if (G(L)->gc.total >= G(L)->gc.threshold) {
      if (not fromc) L->top = curr_topL(L);
      gc(G(L)).step(L);
   }
   return nullptr;
}

//********************************************************************************************************************
// Helper for LEN. __len metamethod.

TValue * lj_meta_len(lua_State *L, cTValue *o)
{
   cTValue *mo = lj_meta_lookup(L, o, MM_len);
   if (tvisnil(mo)) {
      if (tvistab(o)) tabref(tabV(o)->metatable)->nomm |= (uint8_t)(1u << MM_len);
      else if (tvisarray(o)) return nullptr;  // Arrays have first-class length support.
      else lj_err_optype(L, o, ErrMsg::OPLEN);
      return nullptr;
   }
   return mmcall(L, lj_cont_ra, mo, o, o);
}

//********************************************************************************************************************
// Helper for equality comparisons. __eq metamethod.

TValue * lj_meta_equal(lua_State *L, GCobj* o1, GCobj* o2, int ne)
{
   // Field metatable must be at same offset for GCtab and GCudata!
   cTValue *mo = lj_meta_fast(L, tabref(o1->gch.metatable), MM_eq);
   if (mo) {
      TValue *top;
      uint32_t it;
      if (tabref(o1->gch.metatable) != tabref(o2->gch.metatable)) {
         cTValue *mo2 = lj_meta_fast(L, tabref(o2->gch.metatable), MM_eq);
         if (mo2 IS nullptr or !lj_obj_equal(mo, mo2)) return (TValue *)(intptr_t)ne;
      }

      top = curr_top(L);
      setcont(top++, ne ? lj_cont_condf : lj_cont_condt);
      setnilV(top++);
      copyTV(L, top++, mo);
      setnilV(top++);
      it = ~(uint32_t)o1->gch.gct;
      setgcV(L, top, o1, it);
      setgcV(L, top + 1, o2, it);
      return top;  //  Trigger metamethod call.
   }
   return (TValue *)(intptr_t)ne;
}

//********************************************************************************************************************
// Helper for thunk equality comparisons. Resolves thunk and compares with any type.
// Called from VM assembler (vmeta_equal_thunk) which does NOT set L->top.

TValue * lj_meta_equal_thunk(lua_State *L, BCIns ins)
{
   // VMHelperGuard fixes L->top (VM assembler doesn't set it) and saves/restores
   // stack state in case thunk resolution triggers nested Lua calls with GC.
   VMHelperGuard guard(L);

   int op = (int)bc_op(ins) & ~1;
   TValue tv;
   cTValue *o1 = &L->base[bc_a(ins)];
   cTValue *o2;

   // Decode o2 based on bytecode operation
   if (op IS BC_ISEQV) {
      o2 = &L->base[bc_d(ins)];
   }
   else if (op IS BC_ISEQS) {
      setstrV(L, &tv, gco_to_string(proto_kgc(curr_proto(L), ~(ptrdiff_t)bc_d(ins))));
      o2 = &tv;
   }
   else if (op IS BC_ISEQN) {
      o2 = &mref<cTValue>(curr_proto(L)->k)[bc_d(ins)];
   }
   else {
      lj_assertL(op IS BC_ISEQP, "bad bytecode op %d", op);
      setpriV(&tv, ~bc_d(ins));
      o2 = &tv;
   }

   // Resolve thunks if present

   cTValue *resolved_o1 = o1;
   cTValue *resolved_o2 = o2;

   if (lj_is_thunk(o1)) resolved_o1 = lj_thunk_resolve(L, udataV(o1));
   if (lj_is_thunk(o2)) resolved_o2 = lj_thunk_resolve(L, udataV(o2));

   // Now compare the resolved values using standard Lua equality semantics
   // Return semantics: 0 = don't branch, 1 = branch
   // For ISEQV (ne=0): return 1 if equal (branch to target), 0 if not equal
   // For ISNEV (ne=1): return 1 if not equal (branch to target), 0 if equal

   int ne = bc_op(ins) & 1;

   // Check for same TValue pointer first
   if (resolved_o1 IS resolved_o2) {
      return (TValue *)(intptr_t)(ne ? 0 : 1);  // Equal: ISEQ branches, ISNE doesn't
   }

   // Use lj_obj_equal for basic equality (handles numbers, strings, primitives, reference equality)
   if (lj_obj_equal(resolved_o1, resolved_o2)) {
      return (TValue *)(intptr_t)(ne ? 0 : 1);  // Equal: ISEQ branches, ISNE doesn't
   }

   // For tables and userdata with same type, check __eq metamethod
   if (itype(resolved_o1) IS itype(resolved_o2)) {
      if (tvistab(resolved_o1) or tvisudata(resolved_o1)) {
         // Delegate to lj_meta_equal which handles __eq metamethods
         GCobj *gcobj1 = gcV(resolved_o1);
         GCobj *gcobj2 = gcV(resolved_o2);
         return lj_meta_equal(L, gcobj1, gcobj2, ne);
      }
   }

   // Different types or no metamethod - not equal
   return (TValue *)(intptr_t)(ne ? 1 : 0);  // Not equal: ISNE branches, ISEQ doesn't
}

//********************************************************************************************************************
// Helper for ordered comparisons. String compare, __lt/__le metamethods.

TValue * lj_meta_comp(lua_State *L, cTValue *o1, cTValue *o2, int op)
{
   // Never called with two numbers.
   if (tvisstr(o1) and tvisstr(o2)) {
      int32_t res = lj_str_cmp(strV(o1), strV(o2));
      return (TValue *)(intptr_t)(((op & 2) ? res <= 0 : res < 0) ^ (op & 1));
   }
   else {
      while (true) {
         ASMFunction cont = (op & 1) ? lj_cont_condf : lj_cont_condt;
         MMS mm = (op & 2) ? MM_le : MM_lt;
         cTValue *mo = lj_meta_lookup(L, o1, mm);
         if (tvisnil(mo) and tvisnil((mo = lj_meta_lookup(L, o2, mm)))) {
            if (op & 2) {  // MM_le not found: retry with MM_lt.
               cTValue *ot = o1; o1 = o2; o2 = ot;  //  Swap operands.
               op ^= 3;  //  Use LT and flip condition.
               continue;
            }
            lj_err_comp(L, o1, o2);
            return nullptr;
         }
         return mmcall(L, cont, mo, o1, o2);
      }
   }
}

//********************************************************************************************************************
// Helper for ISTYPE and ISNUM. Implicit coercion or error.

void lj_meta_istype(lua_State *L, BCREG ra, BCREG tp)
{
   L->top = curr_topL(L);
   ra++;
   tp--;
   // tp range is 0 to ~LJ_TNUMX (14) for lj_obj_itypename array access; and ~LJ_TNUMX + 1 (15) for number coercion
   // (handled specially, doesn't access array)
   lj_assertL(tp <= uint32_t(~LJ_TNUMX) + 1, "tp out of range for ISTYPE: %u (max %u)", tp, uint32_t(~LJ_TNUMX) + 1);
   lj_assertL(LJ_DUALNUM or tp != ~LJ_TNUMX, "bad type for ISTYPE");
   if (LJ_DUALNUM and tp IS ~LJ_TNUMX) lj_lib_checkint(L, ra);
   else if (tp IS ~LJ_TNUMX + 1) lj_lib_checknum(L, ra);
   else if (tp IS ~LJ_TSTR) lj_lib_checkstr(L, ra);
   else if (tp IS ~LJ_TTRUE) {
      // Boolean check: accept both true and false
      // LJ_TFALSE = ~1, LJ_TTRUE = ~2
      TValue* o = L->base + ra - 1;
      if (not tvisbool(o)) lj_err_assigntype(L, int(ra) - 1, "boolean");
   }
   else lj_err_assigntype(L, int(ra) - 1, lj_obj_itypename[tp]);
}

//********************************************************************************************************************
// Helper for calls. __call metamethod.

void lj_meta_call(lua_State *L, TValue *func, TValue *top)
{
   cTValue *mo = lj_meta_lookup(L, func, MM_call);
   TValue *p;
   if (not tvisfunc(mo)) lj_err_optype_call(L, func);
   for (p = top; p > func + 2; p--) copyTV(L, p, p - 1);
   copyTV(L, func + 2, func);
   copyTV(L, func, mo);
}

//********************************************************************************************************************
// Helper for __close metamethod. Called during scope exit for to-be-closed variables.
// Returns error code: 0 = success, non-zero = error during __close call.
//
// NOTE: This function is called from C error handling code (lj_err.cpp)
// When an error occurs in __close, the error value is left at L->top - 1 and we must NOT restore L->top (which
// would hide the error).

int lj_meta_close(lua_State *L, TValue *o, TValue *err)
{
   cTValue *mo = lj_meta_lookup(L, o, MM_close);
   if (tvisnil(mo)) return 0;  // No __close metamethod, nothing to do.

   global_State *g = G(L);
   uint8_t oldh = hook_save(g);
   int errcode;
   TValue *top;

   lj_trace_abort(g);
   hook_entergc(g);  // Disable hooks and new traces during __close.
   if (LJ_HASPROFILE and (oldh & HOOK_PROFILE)) lj_dispatch_update(g);

   {
      GCPauseGuard pause_gc(g);  // Prevent GC steps during __close call

      top = L->top;
      copyTV(L, top++, mo);         // Push __close function
      setnilV(top++);               // Frame slot for LJ_FR2
      TValue *argbase = top;        // First argument position (for lj_vm_pcall base)
      copyTV(L, top++, o);          // Push object (first argument)
      if (err) copyTV(L, top++, err); // Push error value (second argument)
      else setnilV(top++);            // Push nil for normal scope exit
      L->top = top;

      // Call __close(obj, err) with protection. nres1=1 means 0 results expected.
      errcode = lj_vm_pcall(L, argbase, 1, -1);
   }  // GC threshold automatically restored here

   hook_restore(g, oldh);
   if (LJ_HASPROFILE and (oldh & HOOK_PROFILE)) lj_dispatch_update(g);

   // Unlike __gc, we return the error code instead of propagating.
   // The caller decides how to handle errors from __close.
   return errcode;
}

//********************************************************************************************************************
// Helper for FORI. Coercion.

void lj_meta_for(lua_State *L, TValue *o)
{
   if (not lj_strscan_numberobj(o)) lj_err_msg(L, ErrMsg::FORINIT);
   if (not lj_strscan_numberobj(o + 1)) lj_err_msg(L, ErrMsg::FORLIM);
   if (not lj_strscan_numberobj(o + 2)) lj_err_msg(L, ErrMsg::FORSTEP);

   if (LJ_DUALNUM) {
      // Ensure all slots are integers or all slots are numbers.
      int32_t k[3];
      int nint = 0;
      ptrdiff_t i;
      for (i = 0; i <= 2; i++) {
         if (tvisint(o + i)) {
            k[i] = intV(o + i);
            nint++;
         }
         else {
            k[i] = lj_num2int(numV(o + i));
            nint += ((lua_Number)k[i] IS numV(o + i));
         }
      }

      if (nint IS 3) {  // Narrow to integers.
         setintV(o, k[0]);
         setintV(o + 1, k[1]);
         setintV(o + 2, k[2]);
      }
      else if (nint != 0) {  // Widen to numbers.
         if (tvisint(o)) setnumV(o, (lua_Number)intV(o));
         if (tvisint(o + 1)) setnumV(o + 1, (lua_Number)intV(o + 1));
         if (tvisint(o + 2)) setnumV(o + 2, (lua_Number)intV(o + 2));
      }
   }
}

//********************************************************************************************************************
// Helper for BC_TYPEFIX. Fix function return types based on actual returned values.
// Called when a function without explicit return types returns values for the first time.
//
// Parameters:
//   L     - Lua state
//   base  - Base register containing first return value
//   count - Number of return values to fix (1-8)

void lj_meta_typefix(lua_State *L, TValue *base, uint32_t count)
{
   // Get the current function's prototype
   GCfunc *fn = curr_func(L);
   if (not isluafunc(fn)) return;  // C functions don't have prototypes

   GCproto *pt = funcproto(fn);

   // Only process if PROTO_TYPEFIX is set (function has no explicit return types)
   if (not (pt->flags & PROTO_TYPEFIX)) return;

   // Process each return value position
   for (uint32_t pos = 0; pos < count and pos < PROTO_MAX_RETURN_TYPES; ++pos) {
      // Only fix if type is currently Unknown
      if (pt->result_types[pos] != TiriType::Unknown) continue;

      // Get the value being returned
      TValue *val = base + pos;

      // Don't fix type for nil - nil is always allowed as a return value
      if (tvisnil(val)) continue;

      // Determine the type from the value

      TiriType inferred;
      if (tvisnumber(val)) inferred = TiriType::Num;
      else inferred = lj_tag_to_tiri_type(itype(val));

      // Fix the type in the prototype
      // Note: This is a mutation of the prototype. For thread safety, this relies on
      // the fact that the write is atomic at the byte level and idempotent (same value
      // would be written by any thread inferring the same type).

      pt->result_types[pos] = inferred;
   }
}
