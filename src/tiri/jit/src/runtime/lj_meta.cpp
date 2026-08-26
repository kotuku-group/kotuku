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
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_vm.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lj_thunk.h"
#include "lj_object.h"
#include "lj_contract.h"
#include "lj_vmarray.h"
#include "lj_array.h"
#include "lj_proto_registry.h"
#include "stack_helpers.h"
#include "lib.h"
#include "lib_range.h"
#include "../../../defs.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

//********************************************************************************************************************
// Preserve an unknown number of returned values while user-visible cleanup handlers execute.  The ordinary Lua stack
// above a cleanup call base belongs to its callee, so keeping only MULTRES or reserving caller registers is unsafe.

void lj_meta_multres_save(lua_State *L, TValue *Base, uint32_t Count)
{
   auto &saved_frame = L->saved_multres.emplace_back();
   saved_frame.frame_base = L->base - tvref(L->stack);
   saved_frame.values.resize(Count);
   for (uint32_t i = 0; i < Count; ++i) saved_frame.values[i] = Base[i].u64;
}

//********************************************************************************************************************
// Restore values after cleanup and return MULTRES (the result count plus one) to the VM.

uint32_t lj_meta_multres_restore(lua_State *L, TValue *Base)
{
   lj_assertL(not L->saved_multres.empty(), "missing saved multi-result values");
   auto &saved_frame = L->saved_multres.back();
   lj_assertL(saved_frame.frame_base IS L->base - tvref(L->stack),
      "saved multi-result values belong to another frame");
   uint32_t count = uint32_t(saved_frame.values.size());
   ptrdiff_t base_slot = Base - tvref(L->stack);
   ptrdiff_t required_slots = base_slot + ptrdiff_t(count);
   ptrdiff_t available_slots = tvref(L->maxstack) - tvref(L->stack);
   if (required_slots > available_slots) {
      lj_state_growstack(L, MSize(required_slots - available_slots));
      Base = tvref(L->stack) + base_slot;
   }
   for (uint32_t i = 0; i < count; ++i) Base[i].u64 = saved_frame.values[i];
   L->saved_multres.pop_back();
   return count + 1;
}

//********************************************************************************************************************
// Discard saves owned by frames abandoned during error unwinding. Saves below TargetBase belong to callers that may
// still resume after a nested protected error.

void lj_meta_multres_unwind(lua_State *L, TValue *TargetBase)
{
   ptrdiff_t target_base = TargetBase - tvref(L->stack);
   while (not L->saved_multres.empty() and L->saved_multres.back().frame_base >= target_base) {
      L->saved_multres.pop_back();
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
   else if (tvisstruct(o)) {
      // Check per-instance metatable first, then fall back to base metatable
      mt = tabref(structV(o)->metatable);
      if (not mt) mt = tabref(basemt_it(G(L), LJ_TSTRUCT));
   }
   else mt = tabref(basemt_obj(G(L), o));

   if (mt) {
      cTValue *mo = lj_tab_getstr(mt, mmname_str(G(L), mm));
      if (mo) return mo;
   }
   return niltv(L);
}

//********************************************************************************************************************
// Return a non-empty string from the raw __name metatable slot, without invoking Tiri code.

GCstr *lj_meta_type_name(lua_State *L, cTValue *Value) noexcept
{
   cTValue *name = lj_meta_lookup(L, Value, MM_name);
   if (name and tvisstr(name) and strV(name)->len > 0) return strV(name);
   return nullptr;
}

//********************************************************************************************************************
// Return the user-facing name for a concrete value, falling back to its built-in runtime category.

const char *lj_meta_display_name(lua_State *L, cTValue *Value) noexcept
{
   GCstr *name = lj_meta_type_name(L, Value);
   return name ? strdata(name) : lj_typename(Value);
}

//********************************************************************************************************************

[[nodiscard]] static CSTRING raw_type_fixed_name(lua_State *L, cTValue *Value) noexcept
{
   if (tvisnil(Value)) return "nil";
   if (tvisbool(Value)) return "bool";
   if (tvisnumber(Value)) return "num";
   if (tvisstr(Value)) return "str";
   if (tvistab(Value)) return "table";
   if (tvisfunc(Value)) return "func";
   if (tvislightud(Value)) return "userdata";
   if (tvisudata(Value)) {
      GCudata *userdata = udataV(Value);
      if (userdata->udtype IS UDTYPE_THUNK) return "thunk";
      if (is_range_userdata(L, userdata)) return "range";
      return "userdata";
   }
   return nullptr;
}

void lj_meta_raw_type_text(lua_State *L, cTValue *Value, char *Buffer, size_t Size) noexcept
{
   if (not Size) return;

   if (CSTRING name = raw_type_fixed_name(L, Value)) {
      std::snprintf(Buffer, Size, "%s", name);
      return;
   }

   if (tvisarray(Value)) {
      GCarray *array = arrayV(Value);
      if (gcref(array->type_identity)) {
         GCstr *identity = strref(array->type_identity);
         std::snprintf(Buffer, Size, "%.*s", int(identity->len), strdata(identity));
      }
      else if (array->elemtype IS AET::STRUCT and array->structdef) {
         std::snprintf(Buffer, Size, "array<struct<%s>>", array->structdef->Name.c_str());
      }
      else std::snprintf(Buffer, Size, "array<%s>", lj_array_elemtype_name(array->elemtype));
      return;
   }

   if (tvisobject(Value)) {
      GCobject *object = objectV(Value);
      if (object->classptr) std::snprintf(Buffer, Size, "obj<%s>", object->classptr->ClassName.c_str());
      else std::snprintf(Buffer, Size, "obj");
      return;
   }

   if (tvisstruct(Value)) {
      GCstruct *structure = structV(Value);
      if (structure->def) std::snprintf(Buffer, Size, "struct<%s>", structure->def->Name.c_str());
      else std::snprintf(Buffer, Size, "struct");
      return;
   }

   std::snprintf(Buffer, Size, "userdata");
}

//********************************************************************************************************************

GCstr *lj_meta_raw_type_name(lua_State *L, cTValue *Value) noexcept
{
   char name[280];
   lj_meta_raw_type_text(L, Value, name, sizeof(name));
   return lj_str_newz(L, name);
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
   setframe_gc(top, obj2gco(L), LJ_TSTRUCT);  //  Dummy frame object.
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
// Setup a metamethod call to be run by the assembler VM. Table receivers are captured as context and omitted from
// the visible argument area; non-table receivers retain their native ABI.

enum class MetamethodCallKind : uint8_t { Index, NewIndex, Unary };

static TValue * mmcall(lua_State *L, ASMFunction cont, cTValue *Method, cTValue *Receiver, cTValue *Argument,
   MetamethodCallKind Kind)
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
   copyTV(L, top++, Method);
   setnilV(top++);
   TValue *base = top;
   uint32_t visible_count = Kind IS MetamethodCallKind::Unary ? 0 : 1;
   uint32_t native_count = 2;
   [[maybe_unused]] uint32_t argument_count = lj_context_prepare_metamethod_call(
      L, Receiver, base, visible_count, native_count);
   if (tvistab(Receiver)) {
      if (visible_count) copyTV(L, base, Argument);
   }
   else {
      copyTV(L, base, Receiver);
      copyTV(L, base + 1, Argument);
   }
   lj_assertL(argument_count IS L->metamethod_argument_count, "metamethod argument count was not retained");
   return base;
}

// Setup a receiver-first binary metamethod call.  This remains separate from mmcall() so receiver-stable binary
// metamethods retain their two-argument ABI.

static TValue * mmcall_binop3(lua_State *L, ASMFunction cont, cTValue *Method, cTValue *Receiver,
   cTValue *Other, bool LhsDispatch)
{
   //           |-- framesize -> top       top+1       top+2 top+3      top+4      top+5
   // before:   [func slots ...]
   // mm setup: [func slots ...] [cont|?]  [mo|tmtype] [receiver] [other] [lhs dispatch]
   // in asm:   [func slots ...] [cont|PC] [mo|delta]  [receiver] [other] [lhs dispatch]
   //           ^-- func base                          ^-- mm base

   TValue *top = L->top;
   if (curr_funcisL(L)) top = curr_topL(L);
   setcont(top++, cont);
   setnilV(top++);
   copyTV(L, top++, Method);
   setnilV(top++);
   TValue *base = top;
   [[maybe_unused]] uint32_t argument_count = lj_context_prepare_metamethod_call(L, Receiver, base, 2, 3);
   if (tvistab(Receiver)) {
      copyTV(L, top++, Other);
   }
   else {
      copyTV(L, top++, Receiver);
      copyTV(L, top++, Other);
   }
   setboolV(top, LhsDispatch);
   lj_assertL(uint32_t(top - base + 1) IS argument_count, "binary metamethod prepared the wrong arguments");
   return base;
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
      else if (tvisstruct(o)) {
         mo = lj_meta_fast(L, tabref(basemt_it(G(L), LJ_TSTRUCT)), MM_index);
         if (not mo or tvisnil(mo)) {
            lj_err_optype(L, o, ErrMsg::OPINDEX);
            return nullptr;
         }
      }
      else if (tvisnil(mo = lj_meta_lookup(L, o, MM_index))) {
         lj_err_optype(L, o, ErrMsg::OPINDEX);
         return nullptr;  //  unreachable
      }

      if (tvisfunc(mo)) {
         L->top = mmcall(L, lj_cont_ra, mo, o, k, MetamethodCallKind::Index);
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
            // Overwriting a live slot commits a store, so classify here: the returning paths below bypass
            // lj_tab_newkey() because the node already carries the key.
            lj_tab_classify_store(t, k);
            lj_gc_anybarriert(L, t);
            return (TValue *)tv;
         }
         else if (not (mo = lj_meta_fast(L, tabref(t->metatable), MM_newindex))) {
            t->nomm = 0;  //  Invalidate negative metamethod cache.
            lj_gc_anybarriert(L, t);
            if (tv != niltv(L)) {  // Resurrecting a dead node that still holds the key.
               lj_tab_classify_store(t, k);
               return (TValue *)tv;
            }
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
      else if (tvisstruct(o)) {
         mo = lj_meta_fast(L, tabref(basemt_it(G(L), LJ_TSTRUCT)), MM_newindex);
         if (not mo or tvisnil(mo)) {
            lj_err_optype(L, o, ErrMsg::OPINDEX);
            return nullptr;
         }
      }
      else if (tvisnil(mo = lj_meta_lookup(L, o, MM_newindex))) {
         lj_err_optype(L, o, ErrMsg::OPINDEX);
         return nullptr;  //  unreachable
      }

      if (tvisfunc(mo)) {
         L->top = mmcall(L, lj_cont_nop, mo, o, k, MetamethodCallKind::NewIndex);
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
   // String-to-number coercion is intentionally not performed for arithmetic; callers must use
   // tonumber() explicitly, e.g. 1 + tonumber('2').  Non-numeric operands fall through to the
   // metamethod path and, failing that, raise an arithmetic type error.
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
      cTValue *receiver = rb;
      cTValue *other = rc;
      bool lhs_dispatch = true;
      if (tvisnil(mo)) {
         mo = lj_meta_lookup(L, rc, mm);
         receiver = rc;
         other = rb;
         lhs_dispatch = false;
         if (tvisnil(mo)) {
            if (str2num(rb, &tempb) IS nullptr) rc = rb;
            lj_err_optype(L, rc, ErrMsg::OPARITH);
            return nullptr;  //  unreachable
         }
      }
      if (mm IS MM_unm) return mmcall(L, lj_cont_ra, mo, rb, rc, MetamethodCallKind::Unary);
      return mmcall_binop3(L, lj_cont_ra, mo, receiver, other, lhs_dispatch);
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
         cTValue *receiver = top - 1;
         cTValue *other = top;
         bool lhs_dispatch = true;
         if (tvisnil(mo)) {
            mo = lj_meta_lookup(L, top, MM_concat);
            receiver = top;
            other = top - 1;
            lhs_dispatch = false;
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
         // setup mm:  [...][CAT stack ...] [cont|?]  [mo|tmtype] [visible arguments ...]
         // in asm:    [...][CAT stack ...] [cont|PC] [mo|delta]  [visible arguments ...]
         //            ^-- func base                    ^-- mm base
         // after mm:  [...][CAT stack ...] <--push-- [result]
         // next step: [...][CAT stack .............]

         TValue receiver_copy, other_copy;
         copyTV(L, &receiver_copy, receiver);
         copyTV(L, &other_copy, other);
         copyTV(L, top + LJ_FR2, mo);
         setcont(top - 1, lj_cont_cat);
         setnilV(top);
         setnilV(top + 2);
         TValue *base = top + 2 * LJ_FR2 + 1;
         [[maybe_unused]] uint32_t argument_count = lj_context_prepare_metamethod_call(L, &receiver_copy, base, 2, 3);
         if (tvistab(&receiver_copy)) {
            copyTV(L, base, &other_copy);
            setboolV(base + 1, lhs_dispatch);
         }
         else {
            copyTV(L, base, &receiver_copy);
            copyTV(L, base + 1, &other_copy);
            setboolV(base + 2, lhs_dispatch);
         }
         lj_assertL(argument_count IS L->metamethod_argument_count,
            "concatenation metamethod argument count was not retained");
         return base;  // Trigger metamethod call.
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

TValue * lj_meta_len_result(lua_State *L, TValue *Result)
{
   if (not tvisnumber(Result)) lj_err_msg(L, ErrMsg::LENMM);
   return Result;
}

//********************************************************************************************************************

TValue * lj_meta_len(lua_State *L, cTValue *o)
{
   cTValue *mo = lj_meta_lookup(L, o, MM_len);
   if (tvisnil(mo)) {
      if (tvistab(o)) tabref(tabV(o)->metatable)->nomm |= (uint8_t)(1u << MM_len);
      else if (tvisarray(o)) return nullptr;  // Arrays have first-class length support.
      else lj_err_optype(L, o, ErrMsg::OPLEN);
      return nullptr;
   }
   return mmcall(L, lj_cont_len, mo, o, o, MetamethodCallKind::Unary);
}

//********************************************************************************************************************
// Helper for membership tests.  `Candidate in Target` dispatches only on Target.  Tables without a handler use a
// raw key lookup, while every other target must provide __contains.

TValue * lj_meta_contains(lua_State *L, cTValue *Candidate, cTValue *Target, int Inverted)
{
   cTValue *candidate = Candidate;
   cTValue *target = Target;
   TValue candidate_copy, target_copy;

   // Thunk resolution may execute arbitrary code and relocate the stack.  Preserve both values before resolving
   // either one, then use the rooted copies for the rest of this helper.
   if (lj_is_thunk(Candidate) or lj_is_thunk(Target)) {
      copyTV(L, &candidate_copy, Candidate);
      copyTV(L, &target_copy, Target);

      // The guard repairs the VM-owned top while a thunk resolves, but must be destroyed before mmcall() publishes
      // its continuation frame on that stack.
      VMHelperGuard guard(L);
      if (lj_is_thunk(&candidate_copy)) {
         cTValue *resolved = lj_thunk_resolve(L, udataV(&candidate_copy));
         copyTV(L, &candidate_copy, resolved);
      }
      if (lj_is_thunk(&target_copy)) {
         cTValue *resolved = lj_thunk_resolve(L, udataV(&target_copy));
         copyTV(L, &target_copy, resolved);
      }

      candidate = &candidate_copy;
      target = &target_copy;
   }

   cTValue *method = lj_meta_lookup(L, target, MM_contains);
   if (not tvisnil(method)) {
      if (tvisarray(target) and lj_arr_is_contains_handler(method)) {
         int32_t result = lj_arr_contains(L, arrayV(target), candidate);
         return (TValue *)(intptr_t)(bool(result) != bool(Inverted));
      }
      return mmcall(L, Inverted ? lj_cont_condf : lj_cont_condt, method, target, candidate,
         MetamethodCallKind::Index);
   }

   if (tvistab(target)) {
      cTValue *value = lj_tab_get(L, tabV(target), candidate);
      return (TValue *)(intptr_t)(bool(not tvisnil(value)) != bool(Inverted));
   }

   lj_err_optype(L, target, ErrMsg::OPCONTAINS);
   return nullptr;
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
      TValue receiver, other;
      setgcV(L, &receiver, o1, it);
      setgcV(L, &other, o2, it);
      [[maybe_unused]] uint32_t argument_count = lj_context_prepare_metamethod_call(L, &receiver, top, 1, 2);
      if (tvistab(&receiver)) copyTV(L, top, &other);
      else {
         copyTV(L, top, &receiver);
         copyTV(L, top + 1, &other);
      }
      lj_assertL(argument_count IS L->metamethod_argument_count,
         "equality metamethod argument count was not retained");
      return top;  // Trigger metamethod call.
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

   // Snapshot both operands before resolving either thunk.  Resolution can execute arbitrary Tiri code and relocate
   // the Lua stack, while the original slots keep copied GC values rooted.

   TValue o1_copy, o2_copy;
   copyTV(L, &o1_copy, o1);
   copyTV(L, &o2_copy, o2);
   cTValue *resolved_o1 = &o1_copy;
   cTValue *resolved_o2 = &o2_copy;

   if (lj_is_thunk(resolved_o1)) resolved_o1 = lj_thunk_resolve(L, udataV(resolved_o1));
   if (lj_is_thunk(resolved_o2)) resolved_o2 = lj_thunk_resolve(L, udataV(resolved_o2));

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
// Resolve a thunk before applying the extended falsey test.  BC_ISFALSEY handles ordinary values inline in the VM
// and calls this helper only for userdata, since a thunk's container is userdata but its logical value may be falsey.

int lj_meta_isfalsey(lua_State *L, BCIns Ins)
{
   VMHelperGuard guard(L);

   cTValue *value = &L->base[bc_a(Ins)];
   if (lj_is_thunk(value)) value = lj_thunk_resolve(L, udataV(value));
   uint32_t mask = bc_d(Ins);

   if (tvisnil(value)) return 1;
   if (tvisfalse(value)) return (mask & ISFALSEY_FALSE) != 0;
   if (tvisnumber(value)) {
      return (mask & ISFALSEY_ZERO) and (tvisint(value) ? intV(value) IS 0 : tviszero(value));
   }
   if (tvisstr(value)) return (mask & ISFALSEY_EMPTY_STR) and strV(value)->len IS 0;
   if (tvisarray(value)) return (mask & ISFALSEY_EMPTY_COLL) and arrayV(value)->len IS 0;
   if (tvistab(value)) return (mask & ISFALSEY_EMPTY_COLL) and lj_tab_empty(tabV(value));
   return 0;
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
         cTValue *receiver = o1;
         cTValue *other = o2;
         bool lhs_dispatch = true;
         if (tvisnil(mo)) {
            mo = lj_meta_lookup(L, o2, mm);
            receiver = o2;
            other = o1;
            lhs_dispatch = false;
         }
         if (tvisnil(mo)) {
            if (op & 2) {  // MM_le not found: retry with MM_lt.
               cTValue *ot = o1; o1 = o2; o2 = ot;  //  Swap operands.
               op ^= 3;  //  Use LT and flip condition.
               continue;
            }
            lj_err_comp(L, o1, o2);
            return nullptr;
         }
         return mmcall_binop3(L, cont, mo, receiver, other, lhs_dispatch);
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
   // `tp` is the zero-based type-map index.  ISTypeOperand::Number is deliberately one past the ordinary range.
   lj_assertL(tp <= uint32_t(~LJ_TNUMX) + 1, "tp out of range for ISTYPE: %u (max %u)", tp, uint32_t(~LJ_TNUMX) + 1);
   lj_assertL(LJ_DUALNUM or tp != ~LJ_TNUMX, "bad type for ISTYPE");
   if (LJ_DUALNUM and tp + 1 IS istype_operand_value(ISTypeOperand::Integer)) lj_lib_checkint(L, ra);
   else if (tp + 1 IS istype_operand_value(ISTypeOperand::Number)) lj_lib_checknum(L, ra);
   else if (tp + 1 IS istype_operand_value(ISTypeOperand::String)) lj_lib_checkstr(L, ra);
   else if (tp IS ~LJ_TTRUE) {
      // Boolean check: accept both true and false
      // LJ_TFALSE = ~1, LJ_TTRUE = ~2
      TValue* o = L->base + ra - 1;
      if (not tvisbool(o)) lj_err_assigntype(L, int(ra) - 1, "bool");
   }
   else lj_err_assigntype(L, int(ra) - 1, lj_obj_itypename[tp]);
}

//********************************************************************************************************************
// Exact runtime type contracts.  The descriptor is an interned byte string, so it remains portable across bytecode
// dump/write/read boundaries.  Descriptors use this layout:
//
//   boundary, descriptor_flags, static_value_count, contract_count,
//   repeated { type, entry_flags, position, object_class_id_uleb32,
//              struct_name_length, struct_name_bytes, optional array member and structure name,
//              label_length, label_bytes }

bool lj_meta_object_class_matches(const GCobject *Object, CLASSID ExpectedClassId) noexcept
{
   if (not Object or not Object->classptr) return false;
   return ExpectedClassId IS CLASSID::NIL or Object->classptr->ClassID IS ExpectedClassId or
      Object->classptr->BaseClassID IS ExpectedClassId;
}

namespace {

[[nodiscard]] static bool contract_is_range(lua_State *L, cTValue *Value)
{
   if (not tvisudata(Value)) return false;
   GCtab *metatable = tabref(udataV(Value)->metatable);
   if (not metatable) return false;

   cTValue *registered = lj_tab_getstr(tabV(registry(L)), lj_str_newz(L, RANGE_METATABLE));
   return registered and tvistab(registered) and tabV(registered) IS metatable;
}

[[nodiscard]] static bool contract_is_callable(lua_State *L, cTValue *Value)
{
   if (tvisfunc(Value)) return true;
   cTValue *call = lj_meta_lookup(L, Value, MM_call);
   return call and tvisfunc(call);
}

[[nodiscard]] static bool contract_is_userdata(lua_State *L, cTValue *Value)
{
   if (tvislightud(Value)) return true;
   if (not tvisudata(Value) or lj_is_thunk(Value)) return false;
   return not contract_is_range(L, Value);
}

[[nodiscard]] static bool contract_array_element_matches(
   lua_State *L, const GCarray *Array, const RuntimeContractEntry &Entry)
{
   if (Entry.array_element_type IS AET::ANY) return true;
   AET actual = Array->elemtype;
   AET expected = Entry.array_element_type;
   bool string_match = expected IS AET::STR_GC and
      (actual IS AET::STR_GC or actual IS AET::CSTR or actual IS AET::STR_CPP);
   if (not string_match and actual != expected) return false;
   if (expected IS AET::STRUCT) {
      struct_record *definition = find_struct(L, Entry.constraint_name);
      return definition and Array->structdef IS definition;
   }
   if (expected IS AET::ARRAY and not Entry.constraint_name.empty()) {
      return lj_array_member_identity_matches(Array, Entry.constraint_name);
   }
   return true;
}

[[nodiscard]] static bool contract_matches(lua_State *L, cTValue *Value, const RuntimeContractEntry &Entry)
{
   bool nullable = (Entry.flags & contract_flag(ContractEntryFlag::Nullable)) != 0;
   bool required = (Entry.flags & contract_flag(ContractEntryFlag::Required)) != 0;
   if (tvisnil(Value)) return nullable and not required;

   switch (Entry.type) {
      case TiriType::Any:
      case TiriType::Unknown: return true;
      case TiriType::Nil:     return false;
      case TiriType::Bool:    return tvisbool(Value);
      case TiriType::Num:     return tvisnumber(Value);
      case TiriType::Str:     return tvisstr(Value);
      case TiriType::Table:   return tvistab(Value);
      case TiriType::Array:
         return tvisarray(Value) and contract_array_element_matches(L, arrayV(Value), Entry);
      case TiriType::Func:    return contract_is_callable(L, Value);
      case TiriType::Struct: {
         if (not tvisstruct(Value)) return false;
         if (Entry.constraint_name.empty()) return true;
         struct_record *definition = find_struct(L, Entry.constraint_name);
         return definition and structV(Value)->def IS definition;
      }
      case TiriType::Object: {
         if (not tvisobject(Value)) return false;
         return lj_meta_object_class_matches(objectV(Value), Entry.object_class_id);
      }
      case TiriType::Range:    return contract_is_range(L, Value);
      case TiriType::Userdata: return contract_is_userdata(L, Value);
   }
   return false;
}

[[nodiscard]] static const char * contract_boundary_name(ContractBoundary Boundary)
{
   switch (Boundary) {
      case ContractBoundary::Parameter: return "parameter";
      case ContractBoundary::Result:    return "result";
      case ContractBoundary::Local:     return "local";
      case ContractBoundary::Upvalue:   return "upvalue";
      case ContractBoundary::Global:    return "global";
   }
   return "value";
}

static void contract_type_name(char *Buffer, size_t Size, const RuntimeContractEntry &Entry)
{
   if (Entry.type IS TiriType::Object and Entry.object_class_id != CLASSID::NIL) {
      if (CSTRING class_name = ResolveClassID(Entry.object_class_id)) {
         std::snprintf(Buffer, Size, "obj<%s>", class_name);
      }
      else std::snprintf(Buffer, Size, "obj<#%08x>", uint32_t(Entry.object_class_id));
      return;
   }

   if (Entry.type IS TiriType::Struct and not Entry.constraint_name.empty()) {
      std::snprintf(Buffer, Size, "struct<%.*s>", int(Entry.constraint_name.size()), Entry.constraint_name.data());
      return;
   }

   if (Entry.type IS TiriType::Array) {
      if (Entry.array_element_type IS AET::STRUCT and not Entry.constraint_name.empty()) {
         std::snprintf(Buffer, Size, "array<struct<%.*s>>", int(Entry.constraint_name.size()),
            Entry.constraint_name.data());
         return;
      }
      if (Entry.array_element_type IS AET::ARRAY and not Entry.constraint_name.empty()) {
         std::snprintf(Buffer, Size, "array<%.*s>", int(Entry.constraint_name.size()),
            Entry.constraint_name.data());
         return;
      }
      std::snprintf(Buffer, Size, "array<%s>", lj_array_elemtype_name(Entry.array_element_type));
      return;
   }

   const char *name = "any";
   switch (Entry.type) {
      case TiriType::Nil:     name = "nil"; break;
      case TiriType::Bool:    name = "bool"; break;
      case TiriType::Num:     name = "num"; break;
      case TiriType::Str:     name = "str"; break;
      case TiriType::Table:   name = "table"; break;
      case TiriType::Array:   name = "array"; break;
      case TiriType::Func:    name = "func"; break;
      case TiriType::Struct:   name = "struct"; break;
      case TiriType::Object:   name = "obj"; break;
      case TiriType::Range:    name = "range"; break;
      case TiriType::Userdata: name = "userdata"; break;
      case TiriType::Any:
      case TiriType::Unknown: break;
   }
   std::snprintf(Buffer, Size, "%s", name);
}

[[noreturn]] static void contract_error(lua_State *L, cTValue *Value, ContractBoundary Boundary,
   const RuntimeContractEntry &Entry, uint32_t Position)
{
   // Stamp the error code before raising.  Without this the exception inherits whatever CaughtError was left behind
   // by an earlier operation, which allows an unrelated 'except when' filter to intercept a type contract failure.

   L->CaughtError = ERR::TypeMismatch;

   char expected[280];
   char actual[280];
   contract_type_name(expected, sizeof(expected), Entry);

   if (tvisobject(Value) and not objectV(Value)->classptr) {
      std::snprintf(actual, sizeof(actual), "obj<invalid>");
   }
   else if (contract_is_range(L, Value)) {
      lj_meta_raw_type_text(L, Value, actual, sizeof(actual));
   }
   else if (GCstr *display_name = lj_meta_type_name(L, Value)) {
      std::snprintf(actual, sizeof(actual), "%s", strdata(display_name));
   }
   else if (tvisobject(Value) or tvisstruct(Value) or tvisarray(Value)) {
      lj_meta_raw_type_text(L, Value, actual, sizeof(actual));
   }
   else std::snprintf(actual, sizeof(actual), "%s", lj_meta_display_name(L, Value));

   if (Boundary IS ContractBoundary::Global and contract_entry_retains_value(Entry) and not Entry.label.empty()) {
      CSTRING message = lj_strfmt_pushf(L, "cannot declare global '%.*s' as %s: existing value has type %s",
         int(Entry.label.size()), Entry.label.data(), expected, actual);
      lj_err_callermsg(L, message);
   }

   const char *boundary = contract_boundary_name(Boundary);
   if (not Entry.label.empty()) {
      CSTRING message = lj_strfmt_pushf(L, "type contract failed for %s %u ('%.*s'): expected %s, got %s",
         boundary, Position, int(Entry.label.size()), Entry.label.data(), expected, actual);
      if (Boundary IS ContractBoundary::Local) lj_err_currentmsg(L, message);
      lj_err_callermsg(L, message);
   }
   else {
      CSTRING message = lj_strfmt_pushf(L, "type contract failed for %s %u: expected %s, got %s",
         boundary, Position, expected, actual);
      if (Boundary IS ContractBoundary::Local) lj_err_currentmsg(L, message);
      lj_err_callermsg(L, message);
   }
}

static void decode_contract_or_error(lua_State *L, GCstr *Descriptor, RuntimeContractDescriptor &Result)
{
   RuntimeContractDecodeError decode_error;
   if (decode_runtime_contract(Descriptor, Result, &decode_error)) return;

   if (decode_error IS RuntimeContractDecodeError::Entry) {
      lj_err_callermsg(L, "invalid runtime type-contract entry");
   }
   else lj_err_callermsg(L, "invalid runtime type-contract descriptor");
}

[[nodiscard]] static uint16_t cached_contract_text_offset(
   const GCstr *Descriptor, std::string_view Text) noexcept
{
   if (Text.empty()) return 0;
   ptrdiff_t offset = Text.data() - strdata(Descriptor);
   lj_assertX(offset > 0 and uint64_t(offset) <= UINT16_MAX, "runtime contract text offset is out of range");
   return uint16_t(offset);
}

[[nodiscard]] static std::string_view cached_contract_text(
   const GCstr *Descriptor, uint16_t Offset) noexcept
{
   if (not Offset) return {};
   const char *text = strdata(Descriptor) + Offset;
   return std::string_view(text, uint8_t(text[-1]));
}

[[nodiscard]] static const CachedRuntimeContractRecord * find_cached_contract(
   const GCproto *Prototype, uint32_t BytecodePosition, const CachedRuntimeContractEntry *&Entries) noexcept
{
   const RuntimeContractCache *cache = proto_contract_cache(Prototype);
   if (not cache) return nullptr;

   const CachedRuntimeContractRecord *records = runtime_contract_cache_records(cache);
   uint32_t lower = 0;
   uint32_t upper = cache->record_count;
   while (lower < upper) {
      uint32_t middle = lower + ((upper - lower) >> 1);
      if (records[middle].bytecode_position < BytecodePosition) lower = middle + 1;
      else upper = middle;
   }
   if (lower >= cache->record_count or records[lower].bytecode_position != BytecodePosition) return nullptr;

   const CachedRuntimeContractRecord *record = &records[lower];
   Entries = runtime_contract_cache_entries(cache) + record->entry_index;
   return record;
}

static void apply_cached_contract(lua_State *L, TValue *Base, uint32_t DynamicCount, GCstr *Descriptor,
   const CachedRuntimeContractRecord &Record, const CachedRuntimeContractEntry *Entries)
{
   L->top = curr_topL(L);
   uint32_t available_value_count = (Record.flags & contract_flag(ContractDescriptorFlag::DynamicCount)) ?
      DynamicCount + Record.static_value_count : Record.static_value_count;
   uint32_t value_count = available_value_count > Record.contract_count ?
      available_value_count : Record.contract_count;
   bool variadic = (Record.flags & contract_flag(ContractDescriptorFlag::Variadic)) != 0;
   ptrdiff_t base_offset = savestack(L, Base);

   for (uint32_t i = 0; i < value_count; ++i) {
      uint32_t entry_index;
      if (i < Record.contract_count) entry_index = i;
      else if (variadic and Record.contract_count > 0) entry_index = Record.contract_count - 1;
      else continue;

      const CachedRuntimeContractEntry &cached = Entries[entry_index];
      RuntimeContractEntry entry{
         .type = cached.type,
         .array_element_type = cached.type IS TiriType::Array ? cached.array_element_type : AET::MAX,
         .flags = cached.flags,
         .position = cached.position,
         .object_class_id = cached.type IS TiriType::Object ? CLASSID(cached.object_class_id) : CLASSID::NIL,
         .constraint_name = (cached.type IS TiriType::Struct or
            (cached.type IS TiriType::Array and
             (cached.array_element_type IS AET::STRUCT or cached.array_element_type IS AET::ARRAY))) ?
            cached_contract_text(Descriptor, cached.constraint_offset) : std::string_view{},
         .label = cached_contract_text(Descriptor, cached.label_offset)
      };
      if ((entry.type IS TiriType::Any or entry.type IS TiriType::Unknown) and
          (entry.flags & contract_flag(ContractEntryFlag::Required)) IS 0) continue;

      Base = restorestack(L, base_offset);
      TValue *value = i < available_value_count ? Base + i : niltv(L);
      if (lj_is_thunk(value)) {
         TValue *resolved = lj_thunk_resolve(L, udataV(value));
         Base = restorestack(L, base_offset);
         value = Base + i;
         copyTV(L, value, resolved);
      }

      if (not contract_matches(L, value, entry)) {
         uint32_t position = Record.boundary IS ContractBoundary::Result ? i + 1 : entry.position;
         contract_error(L, value, Record.boundary, entry, position);
      }
   }
}

[[noreturn]] static void const_global_error(lua_State *L, const GCstr *Name)
{
   CSTRING message = lj_strfmt_pushf(L, "cannot assign to const global '%s'", strdata(Name));
   lj_err_callermsg(L, message);
}

[[noreturn]] static void global_contract_redeclaration_error(lua_State *L, const GCstr *Name,
   const RuntimeContractEntry &Existing, const RuntimeContractEntry &Incoming)
{
   L->CaughtError = ERR::TypeMismatch;
   char existing[280];
   char incoming[280];
   contract_type_name(existing, sizeof(existing), Existing);
   contract_type_name(incoming, sizeof(incoming), Incoming);
   CSTRING message = lj_strfmt_pushf(L,
      "type contract failed for global '%s': cannot redeclare %s%s as %s%s",
      strdata(Name), contract_entry_is_const(Existing) ? "const " : "", existing,
      contract_entry_is_const(Incoming) ? "const " : "", incoming);
   lj_err_callermsg(L, message);
}

} // namespace

extern "C" void lj_meta_type_test_pc(lua_State *L, const BCIns *PC)
{
   GCproto *prototype = funcproto(curr_func(L));
   BCIns instruction = PC[-1];
   if (bc_op(instruction) != BC_TYPETEST) lj_err_callermsg(L, "invalid type-test resume point");

   GCstr *encoded = gco_to_string(proto_kgc(prototype, ~(ptrdiff_t)bc_d(instruction)));
   RuntimeContractEntry decoded_entry;
   const RuntimeContractEntry *entry;
   const CachedRuntimeContractEntry *cached_entries = nullptr;
   uint32_t bytecode_position = uint32_t((PC - 1) - proto_bc(prototype));
   const CachedRuntimeContractRecord *cached_record =
      find_cached_contract(prototype, bytecode_position, cached_entries);
   if (cached_record and cached_record->contract_count IS 1) {
      const CachedRuntimeContractEntry &cached = cached_entries[0];
      decoded_entry.type = cached.type;
      decoded_entry.array_element_type = cached.array_element_type;
      decoded_entry.flags = cached.flags;
      decoded_entry.position = cached.position;
      if (cached.type IS TiriType::Object) decoded_entry.object_class_id = CLASSID(cached.object_class_id);
      else if (cached.type IS TiriType::Struct or
               (cached.type IS TiriType::Array and
                (cached.array_element_type IS AET::STRUCT or cached.array_element_type IS AET::ARRAY))) {
         decoded_entry.constraint_name = cached_contract_text(encoded, cached.constraint_offset);
      }
      decoded_entry.label = cached_contract_text(encoded, cached.label_offset);
      entry = &decoded_entry;
   }
   else {
      RuntimeContractDescriptor descriptor;
      decode_contract_or_error(L, encoded, descriptor);
      if (descriptor.contract_count != 1) lj_err_callermsg(L, "invalid type-test descriptor");
      decoded_entry = descriptor.entries[0];
      entry = &decoded_entry;
   }

   TValue *value = L->base + bc_a(instruction);
   bool matched;
   if (lj_is_thunk(value)) {
      ThunkPayload *payload = thunk_payload(udataV(value));
      if (entry->type IS TiriType::Any) matched = true;
      else if (payload->resolved) matched = contract_matches(L, &payload->cached_value, *entry);
      else if (payload->expected_type != 0xff and
          entry->object_class_id IS CLASSID::NIL and entry->constraint_name.empty() and
          (entry->type != TiriType::Array or entry->array_element_type IS AET::ANY)) {
         matched = entry->type IS TiriType(payload->expected_type);
      }
      else {
         TValue *resolved = lj_thunk_resolve(L, udataV(value));
         value = L->base + bc_a(instruction);
         matched = contract_matches(L, resolved, *entry);
      }
   }
   else matched = contract_matches(L, value, *entry);
   if ((entry->flags & contract_flag(ContractEntryFlag::Negated)) != 0) matched = not matched;
   setboolV(value, matched);
}

void lj_contract_build_cache(lua_State *L, GCproto *Prototype)
{
   lj_assertX(not proto_contract_cache(Prototype), "runtime contract cache was built twice");

   uint16_t record_count = 0;
   uint16_t entry_count = 0;
   for (uint32_t i = 1; i < Prototype->sizebc; ++i) {
      BCIns instruction = proto_bc(Prototype)[i];
      if (bc_op(instruction) != BC_CONTRACT and bc_op(instruction) != BC_TYPETEST) continue;

      GCobj *constant = proto_kgc(Prototype, ~(ptrdiff_t)bc_d(instruction));
      if (constant->gch.gct != uint8_t(~LJ_TSTR)) continue;
      RuntimeContractDescriptor descriptor;
      if (not decode_runtime_contract(gco_to_string(constant), descriptor) or not descriptor.contract_count or
          descriptor.boundary IS ContractBoundary::Global) {
         continue;
      }
      if (record_count IS UINT16_MAX or entry_count > UINT16_MAX - descriptor.contract_count) return;
      record_count++;
      entry_count += descriptor.contract_count;
   }
   if (not record_count) return;

   size_t byte_size = sizeof(RuntimeContractCache) + record_count * sizeof(CachedRuntimeContractRecord) +
      entry_count * sizeof(CachedRuntimeContractEntry);
   if (byte_size > UINT32_MAX) return;

   auto cache = (RuntimeContractCache *)lj_mem_new(L, byte_size);
   memset(cache, 0, byte_size);
   cache->byte_size = uint32_t(byte_size);
   cache->record_count = record_count;
   cache->entry_count = entry_count;

   CachedRuntimeContractRecord *records = runtime_contract_cache_records(cache);
   CachedRuntimeContractEntry *entries = runtime_contract_cache_entries(cache);
   uint16_t record_index = 0;
   uint16_t entry_index = 0;
   for (uint32_t i = 1; i < Prototype->sizebc; ++i) {
      BCIns instruction = proto_bc(Prototype)[i];
      if (bc_op(instruction) != BC_CONTRACT and bc_op(instruction) != BC_TYPETEST) continue;

      GCobj *constant = proto_kgc(Prototype, ~(ptrdiff_t)bc_d(instruction));
      if (constant->gch.gct != uint8_t(~LJ_TSTR)) continue;
      GCstr *encoded = gco_to_string(constant);
      RuntimeContractDescriptor descriptor;
      if (not decode_runtime_contract(encoded, descriptor) or not descriptor.contract_count or
          descriptor.boundary IS ContractBoundary::Global) {
         continue;
      }

      records[record_index++] = CachedRuntimeContractRecord{
         .bytecode_position = i,
         .boundary = descriptor.boundary,
         .flags = descriptor.flags,
         .static_value_count = descriptor.static_value_count,
         .contract_count = descriptor.contract_count,
         .entry_index = entry_index
      };
      for (uint8_t j = 0; j < descriptor.contract_count; ++j) {
         const RuntimeContractEntry &source = descriptor.entries[j];
         CachedRuntimeContractEntry &target = entries[entry_index++];
         if (source.type IS TiriType::Object) target.object_class_id = uint32_t(source.object_class_id);
         else if (source.type IS TiriType::Struct or
                  (source.type IS TiriType::Array and
                   (source.array_element_type IS AET::STRUCT or source.array_element_type IS AET::ARRAY))) {
            target.constraint_offset = cached_contract_text_offset(encoded, source.constraint_name);
         }
         else target.object_class_id = 0;
         target.label_offset = cached_contract_text_offset(encoded, source.label);
         target.type = source.type;
         target.array_element_type = source.array_element_type;
         target.flags = source.flags;
         target.position = source.position;
      }
   }

   lj_assertX(record_index IS record_count and entry_index IS entry_count,
      "runtime contract cache sizing changed while building");
   setmref(Prototype->contract_cache, cache);
}

extern "C" void lj_meta_contract(lua_State *L, TValue *Base, uint32_t DynamicCount, GCstr *Descriptor)
{
   L->top = curr_topL(L);
   RuntimeContractDescriptor descriptor;
   decode_contract_or_error(L, Descriptor, descriptor);

   GCtab *environment = nullptr;
   GCstr *global_name = nullptr;
   bool publish_global_contract = false;

   if (descriptor.boundary IS ContractBoundary::Global and descriptor.contract_count IS 1) {
      const RuntimeContractEntry &incoming = descriptor.entries[0];
      if (not incoming.label.empty()) {
         environment = tabref(curr_func(L)->c.env);
         global_name = lj_str_new(L, incoming.label.data(), incoming.label.size());

         GCstr *current = lj_tab_get_global_contract(environment, global_name);
         if (current and contract_entry_is_global_hint(incoming)) {
            // A same-chunk hint only bootstraps policy before declaration publication.  Once any environment policy
            // exists, BC_GSET owns validation against that authoritative descriptor.
            return;
         }

         if (current) {
            RuntimeContractDescriptor current_descriptor;
            decode_contract_or_error(L, current, current_descriptor);
            if (current_descriptor.contract_count >= 1 and
                contract_entry_is_const(current_descriptor.entries[0])) {
               const_global_error(L, global_name);
            }
            if (current_descriptor.contract_count != 1 or
                not global_contract_entries_equivalent(current_descriptor.entries[0], incoming)) {
               global_contract_redeclaration_error(
                  L, global_name, current_descriptor.entries[0], incoming);
            }
            descriptor = current_descriptor;
         }

         if (contract_entry_is_const(incoming) and not contract_entry_is_initialising(incoming)) {
            // This descriptor follows a successful global store.  Publishing here makes const initialisation
            // transactional: a failed store never reaches this instruction and leaves no policy behind.
            lj_tab_set_global_contract(L, environment, global_name, Descriptor);
            return;
         }

         if (contract_entry_is_initialising(incoming)) {
            // Validate the declaration that will be published while leaving the current environment contract in
            // place.  BC_GSET validates the same value against that existing policy before the post-store descriptor
            // publishes a first contract.  This keeps declaration publication transactional when an environment
            // __newindex handler rejects the store.
         }
         else if (contract_entry_is_global_hint(incoming)) {
            // No environment policy exists yet.  Validate and publish the hint so type-guided reads and indirect
            // stores retain the same runtime invariant even when the declaration has not executed.
            publish_global_contract = true;
         }
         else if (not current) publish_global_contract = true;
      }
   }

   uint32_t available_value_count = descriptor.dynamic_count() ?
      DynamicCount + descriptor.static_value_count : descriptor.static_value_count;
   uint32_t value_count = available_value_count > descriptor.contract_count ?
      available_value_count : descriptor.contract_count;
   ptrdiff_t base_offset = savestack(L, Base);

   for (uint32_t i = 0; i < value_count; ++i) {
      const RuntimeContractEntry *entry = descriptor.entry_for(i);
      if (not entry) continue;
      if ((entry->type IS TiriType::Any or entry->type IS TiriType::Unknown) and
          (entry->flags & contract_flag(ContractEntryFlag::Required)) IS 0) continue;

      Base = restorestack(L, base_offset);
      TValue *value = i < available_value_count ? Base + i : niltv(L);
      if (lj_is_thunk(value)) {
         TValue *resolved = lj_thunk_resolve(L, udataV(value));
         Base = restorestack(L, base_offset);
         value = Base + i;
         copyTV(L, value, resolved);
      }

      if (not contract_matches(L, value, *entry)) {
         uint32_t position = descriptor.boundary IS ContractBoundary::Result ? i + 1 : entry->position;
         contract_error(L, value, descriptor.boundary, *entry, position);
      }
   }

   if (publish_global_contract) lj_tab_set_global_contract(L, environment, global_name, Descriptor);
}

extern "C" void lj_meta_contract_pc(lua_State *L, const BCIns *PC, uint32_t DynamicCount)
{
   GCproto *prototype = funcproto(curr_func(L));
   const BCIns *begin = proto_bc(prototype) + 1;
   const BCIns *cursor = PC;
   while (cursor > begin) {
      BCIns instruction = *--cursor;
      if (bc_op(instruction) IS BC_CONTRACT) {
         GCstr *descriptor = gco_to_string(proto_kgc(prototype, ~(ptrdiff_t)bc_d(instruction)));
         const CachedRuntimeContractEntry *entries = nullptr;
         uint32_t bytecode_position = uint32_t(cursor - proto_bc(prototype));
         if (const CachedRuntimeContractRecord *record =
               find_cached_contract(prototype, bytecode_position, entries)) {
            apply_cached_contract(L, L->base + bc_a(instruction), DynamicCount, descriptor, *record, entries);
            return;
         }
         lj_meta_contract(L, L->base + bc_a(instruction), DynamicCount, descriptor);
         return;
      }
   }
   lj_err_callermsg(L, "invalid runtime type-contract resume point");
}

//********************************************************************************************************************
// Central environment mutation policy check.  Every runtime write to a marked global environment (VM store fast
// paths, rawset(), the C API and JIT-recorded stores) must pass through here so sticky contracts and protected
// built-ins are enforced regardless of source syntax or table aliasing.  Checks run strictly before mutation, so a
// failed store leaves both the stored value and the persisted policy unchanged.
//
// Value must reference a rooted Lua stack slot.  The recorder materialises JIT values in their corresponding Lua stack
// slot before emitting this call; branches that allocate or raise synchronise the interpreter stack on demand.

static void env_check_contract(lua_State *L, GCtab *Environment, GCstr *Name, cTValue *Value)
{
   if ((Name->flags & STRFLAG_PROTECTED_GLOBAL) != 0) {
      JITStackSync stack_sync(L);
      CSTRING message = lj_strfmt_pushf(L, "cannot override built-in '%s'", strdata(Name));
      lj_err_callermsg(L, message);
   }

   RuntimeContractEntry cached_entry;
   RuntimeContractDescriptor descriptor;
   const RuntimeContractEntry *entry = nullptr;
   if (const CachedGlobalContractRecord *cached =
         lj_tab_get_cached_global_contract(Environment, Name)) {
      lj_assertX(lj_tab_get_global_contract(Environment, Name) IS cached->descriptor,
         "decoded global contract cache does not match persisted policy");
      cached_entry = RuntimeContractEntry{
         .type = cached->entry.type,
         .array_element_type = cached->entry.type IS TiriType::Array ?
            cached->entry.array_element_type : AET::MAX,
         .flags = cached->entry.flags,
         .position = cached->entry.position,
         .object_class_id = cached->entry.type IS TiriType::Object ?
            CLASSID(cached->entry.object_class_id) : CLASSID::NIL,
         .constraint_name = (cached->entry.type IS TiriType::Struct or
            (cached->entry.type IS TiriType::Array and
             (cached->entry.array_element_type IS AET::STRUCT or
              cached->entry.array_element_type IS AET::ARRAY))) ?
            cached_contract_text(cached->descriptor, cached->entry.constraint_offset) : std::string_view{},
         .label = cached_contract_text(cached->descriptor, cached->entry.label_offset)
      };
      entry = &cached_entry;
   }
   else if (GCstr *persisted = lj_tab_get_global_contract(Environment, Name)) {
      JITStackSync stack_sync(L);
      decode_contract_or_error(L, persisted, descriptor);
      stack_sync.restore();
      if (descriptor.contract_count >= 1) entry = &descriptor.entries[0];
   }

   if (entry) {
      if (contract_entry_is_const(*entry) and not contract_entry_is_initialising(*entry)) {
         JITStackSync stack_sync(L);
         const_global_error(L, Name);
      }
      if (entry->type != TiriType::Any and entry->type != TiriType::Unknown) {
         cTValue *checked = Value;
         TValue resolved_value;
         if (lj_is_thunk(Value)) {
            // Validate the resolved value, but store the original thunk to preserve lazy evaluation on read.
            JITStackSync stack_sync(L);
            {
               VMHelperGuard guard(L);
               cTValue *resolved = lj_thunk_resolve(L, udataV(Value));
               copyTV(L, &resolved_value, resolved);
            }
            checked = &resolved_value;
            stack_sync.restore();
         }
         if (not contract_matches(L, checked, *entry)) {
            JITStackSync stack_sync(L);
            contract_error(L, checked, ContractBoundary::Global, *entry, entry->position);
         }
      }
   }
}

//********************************************************************************************************************
// Validate a global environment store.
//
// The recorder lowers environment stores to lj_env_check(), so this runs from compiled traces as well as from the
// interpreter and the C API.  Successful checks deliberately leave the interpreter stack untouched.  Individual
// allocating and error branches synchronise immediately before they need a valid trace frame, keeping ordinary global
// stores out of this stack-management path.

extern "C" void lj_env_check(lua_State *L, GCtab *Environment, GCstr *Name, cTValue *Value)
{
   env_check_contract(L, Environment, Name, Value);
}

//********************************************************************************************************************
// Checked raw environment store.  Value must reference a Lua stack slot so it can be recovered after policy checks
// and table allocation resize the stack.  Non-raw paths call lj_env_check() and then resume normal metamethod dispatch.

extern "C" void lj_env_store(lua_State *L, GCtab *Environment, GCstr *Name, cTValue *Value)
{
   ptrdiff_t value_offset = savestack(L, Value);
   lj_env_check(L, Environment, Name, Value);
   Environment->nomm = 0;  //  Invalidate negative metamethod cache, matching the diverted VM store path.
   TValue *slot = lj_tab_setstr(L, Environment, Name);
   copyTV(L, slot, restorestack(L, value_offset));
   lj_gc_anybarriert(L, Environment);
}

//********************************************************************************************************************
// VM entry point for the environment mutation check, reached when BC_TSETS_Z encounters a marked environment.  The
// assembly resumes its ordinary store path afterwards so __newindex behaviour remains unchanged.  Returning Name
// lets each architecture restore the caller-clobbered key register without introducing another saved register.

extern "C" GCstr * lj_vm_envcheck(lua_State *L, GCtab *Environment, GCstr *Name, TValue *Value)
{
   L->top = curr_topL(L);
   lj_env_check(L, Environment, Name, Value);
   return Name;
}

//********************************************************************************************************************
// Helper for calls. __call metamethod.

int lj_meta_call(lua_State *L, TValue *func, TValue *top, bool TailTransfer)
{
   cTValue *mo = lj_meta_lookup(L, func, MM_call);
   if (not tvisfunc(mo)) lj_err_optype_call(L, func);
   if (tvistab(func)) {
      TValue *owner_base = TailTransfer ? L->base : func + 2;
      uint32_t visible_count = uint32_t(top - (func + 2));
      lj_context_prepare_metamethod_call(L, func, owner_base, visible_count, visible_count + 1, TailTransfer);
      copyTV(L, func, mo);
      return 0;
   }
   TValue *p;
   for (p = top; p > func + 2; p--) copyTV(L, p, p - 1);
   copyTV(L, func + 2, func);
   copyTV(L, func, mo);
   return 1;
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
   [[maybe_unused]] size_t context_depth = lj_context_depth(L);
   const ptrdiff_t object_offset = savestack(L, o);
   const bool has_error = err != nullptr;
   const ptrdiff_t error_offset = has_error ? savestack(L, err) : 0;

   lj_state_checkstack(L, 4);
   o = restorestack(L, object_offset);
   err = has_error ? restorestack(L, error_offset) : nullptr;

   cTValue *mo = lj_meta_lookup(L, o, MM_close);
   if (tvisnil(mo)) return 0;  // No __close metamethod, nothing to do.

   global_State *g = G(L);
   const BCIns *saved_try_handler = L->try_handler_pc;
   int saved_try_depth = L->try_stack.depth;
   int saved_checkall_depth = L->checkall_stack->depth;
   std::array<TryFrame, LJ_MAX_TRY_DEPTH> saved_try_frames;
   std::array<CheckallFrame, LJ_MAX_CHECKALL_DEPTH> saved_checkall_frames;
   std::copy_n(L->try_stack.frames, saved_try_depth, saved_try_frames.begin());
   std::copy_n(L->checkall_stack->frames, saved_checkall_depth, saved_checkall_frames.begin());
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
      [[maybe_unused]] uint32_t argument_count = lj_context_prepare_metamethod_call(L, o, argbase, 1, 2);
      if (not tvistab(o)) copyTV(L, top++, o);
      if (err) copyTV(L, top++, err); // Push error value (second argument)
      else setnilV(top++);            // Push nil for normal scope exit
      lj_assertL(uint32_t(top - argbase) IS argument_count,
         "__close call prepared an unexpected argument count");
      L->top = top;

      // Call __close with protection. nres1=1 means 0 results expected.
      // Suspend the caller's Tiri try handlers so an error from __close unwinds to this protected-call frame.
      // The caller will then continue running any remaining close handlers with the replacement error.
      L->try_stack.depth = 0;
      L->checkall_stack->depth = 0;
      L->try_handler_pc = nullptr;
      errcode = lj_vm_pcall(L, argbase, 1, -1);
      lj_assertL(lj_context_depth(L) IS context_depth,
         "__close returned with unbalanced contextual activations");
      std::copy_n(saved_try_frames.begin(), saved_try_depth, L->try_stack.frames);
      std::copy_n(saved_checkall_frames.begin(), saved_checkall_depth, L->checkall_stack->frames);
      L->try_stack.depth = saved_try_depth;
      L->checkall_stack->depth = saved_checkall_depth;
      L->try_handler_pc = saved_try_handler;
   }  // GC threshold automatically restored here

   hook_restore(g, oldh);
   if (LJ_HASPROFILE and (oldh & HOOK_PROFILE)) lj_dispatch_update(g);

   // Unlike __gc, we return the error code instead of propagating.
   // The caller decides how to handle errors from __close.
   return errcode;
}

//********************************************************************************************************************
// Invoke one snapshotted defer registration through a protected call.  Consumption immediately before the call makes
// the registration exactly-once even when its handler throws and starts a nested unwind.

int lj_meta_defer(lua_State *L, TValue *OwnerBase, const DeferRegistration &Registration)
{
   [[maybe_unused]] size_t context_depth = lj_context_depth(L);
   ptrdiff_t owner_offset = savestack(L, OwnerBase);
   if (not lj_defer_consume(L, OwnerBase, Registration.callable_slot)) return 0;
   lj_state_checkstack(L, MSize(Registration.argument_count + 1 + LJ_FR2));

   OwnerBase = restorestack(L, owner_offset);
   const BCIns *saved_try_handler = L->try_handler_pc;
   int saved_try_depth = L->try_stack.depth;
   int saved_checkall_depth = L->checkall_stack->depth;
   std::array<TryFrame, LJ_MAX_TRY_DEPTH> saved_try_frames;
   std::array<CheckallFrame, LJ_MAX_CHECKALL_DEPTH> saved_checkall_frames;
   std::copy_n(L->try_stack.frames, saved_try_depth, saved_try_frames.begin());
   std::copy_n(L->checkall_stack->frames, saved_checkall_depth, saved_checkall_frames.begin());

   global_State *g = G(L);
   uint8_t oldh = hook_save(g);
   int errcode;
   lj_trace_abort(g);
   hook_entergc(g);
   if (LJ_HASPROFILE and (oldh & HOOK_PROFILE)) lj_dispatch_update(g);

   {
      GCPauseGuard pause_gc(g);
      TValue *top = L->top;
      copyTV(L, top++, OwnerBase + Registration.callable_slot);
      if (LJ_FR2) setnilV(top++);
      TValue *argbase = top;
      for (uint32_t i = 0; i < Registration.argument_count; ++i) {
         copyTV(L, top++, OwnerBase + Registration.callable_slot + i + 1);
      }
      L->top = top;

      L->try_stack.depth = 0;
      L->checkall_stack->depth = 0;
      L->try_handler_pc = nullptr;
      errcode = lj_vm_pcall(L, argbase, 1, -1);
      lj_assertL(lj_context_depth(L) IS context_depth,
         "defer returned with unbalanced contextual activations");
      std::copy_n(saved_try_frames.begin(), saved_try_depth, L->try_stack.frames);
      std::copy_n(saved_checkall_frames.begin(), saved_checkall_depth, L->checkall_stack->frames);
      L->try_stack.depth = saved_try_depth;
      L->checkall_stack->depth = saved_checkall_depth;
      L->try_handler_pc = saved_try_handler;
   }

   hook_restore(g, oldh);
   if (LJ_HASPROFILE and (oldh & HOOK_PROFILE)) lj_dispatch_update(g);
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
