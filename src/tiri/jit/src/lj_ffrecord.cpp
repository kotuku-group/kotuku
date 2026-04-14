// Fast function call recorder.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Conventions for fast function call handlers:
//
// The argument slots start at J->base[0]. All of them are guaranteed to be valid and type-specialized references.
// J->base[J->maxslot] is set to 0 as a sentinel. The runtime argument values start at rd->argv[0].
//
// In general fast functions should check for presence of all of their arguments and for the correct argument
// types. Some simplifications are allowed if the interpreter throws instead. But even if recording is aborted,
// the generated IR must be consistent (no zero-refs).
//
// The number of results in rd->nres is set to 1. Handlers that return a different number of results need to
// override it. A negative value prevents return processing (e.g. for pending calls).
//
// Results need to be stored starting at J->base[0]. Return processing moves them to the right slots later.
//
// The per-ffid auxiliary data is the value of the 2nd part of the LJLIB_REC() annotation. This allows handling
// similar functionality in a common handler.

#define lj_ffrecord_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_ff.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_ircall.h"
#include "lj_iropt.h"
#include "lj_trace.h"
#include "lj_record.h"
#include "lj_vmarray.h"
#include "lj_ffrecord.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lj_serialize.h"

// Some local macros to save typing. Undef'd at the end.
#define IR(ref)         (&J->cur.ir[(ref)])

// Pass IR on to next optimization in chain (FOLD).
#define emitir(ot, a, b)   (lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

// Type of handler to record a fast function.
typedef void (* RecordFunc)(jit_State* J, RecordFFData* rd);

//********************************************************************************************************************
// Get runtime value of int argument.

static int32_t argv2int(jit_State* J, TValue* o)
{
   if (!lj_strscan_numberobj(o))
      lj_trace_err(J, LJ_TRERR_BADTYPE);
   return tvisint(o) ? intV(o) : lj_num2int(numV(o));
}

//********************************************************************************************************************
// Get runtime value of string argument.

static GCstr* argv2str(jit_State* J, TValue* o)
{
   if (LJ_LIKELY(tvisstr(o))) return strV(o);
   else {
      GCstr* s;
      if (!tvisnumber(o)) lj_trace_err(J, LJ_TRERR_BADTYPE);
      s = lj_strfmt_number(J->L, o);
      setstrV(J->L, o, s);
      return s;
   }
}

//********************************************************************************************************************
// Trace stitching: add continuation below frame to start a new trace.

static void recff_stitch(jit_State* J)
{
   ASMFunction cont = lj_cont_stitch;
   lua_State* L = J->L;
   TValue* base = L->base;
   BCREG nslot = J->maxslot + 1 + LJ_FR2;
   TValue* nframe = base + 1 + LJ_FR2;
   const BCIns* pc = frame_pc(base - 1);
   TValue* pframe = frame_prevl(base - 1);

   // Check for this now. Throwing in lj_record_stop messes up the stack.
   if (J->cur.nsnap >= (MSize)J->param[JIT_P_maxsnap]) lj_trace_err(J, LJ_TRERR_SNAPOV);

   // Move func + args up in Lua stack and insert continuation.
   memmove(&base[1], &base[-1 - LJ_FR2], sizeof(TValue) * nslot);
   setframe_ftsz(nframe, ((char*)nframe - (char*)pframe) + FRAME_CONT);
   setcont(base - LJ_FR2, cont);
   setframe_pc(base, pc);
   setnilV(base - 1 - LJ_FR2);  //  Incorrect, but rec_check_slots() won't run anymore.
   L->base += 2 + LJ_FR2;
   L->top += 2 + LJ_FR2;

   // Ditto for the IR.
   memmove(&J->base[1], &J->base[-1 - LJ_FR2], sizeof(TRef) * nslot);
   J->base[2] = TREF_FRAME;
   J->base[-1] = lj_ir_k64(J, IR_KNUM, u64ptr(contptr(cont)));
   J->base[0] = lj_ir_k64(J, IR_KNUM, u64ptr(pc)) | TREF_CONT;
   J->ktrace = tref_ref((J->base[-1 - LJ_FR2] = lj_ir_ktrace(J)));
   J->base += 2 + LJ_FR2;
   J->baseslot += 2 + LJ_FR2;
   J->framedepth++;

   lj_record_stop(J, TraceLink::STITCH, 0);

   // Undo Lua stack changes.
   memmove(&base[-1 - LJ_FR2], &base[1], sizeof(TValue) * nslot);
   setframe_pc(base - 1, pc);
   L->base -= 2 + LJ_FR2;
   L->top -= 2 + LJ_FR2;
}

//********************************************************************************************************************
// Fallback handler for fast functions that are not recorded (yet).

static void recff_nyi(jit_State* J, RecordFFData* rd)
{
   if (J->cur.nins < (IRRef)J->param[JIT_P_minstitch] + REF_BASE) {
      lj_trace_err_info(J, LJ_TRERR_TRACEUV);
   }
   else {
      // Can only stitch from Lua call.
      if (J->framedepth and frame_islua(J->L->base - 1)) {
         BCOp op = bc_op(*frame_pc(J->L->base - 1));
         // Stitched trace cannot start with *M op with variable # of args.
         if (!(op == BC_CALLM or op == BC_CALLMT or
            op == BC_RETM or op == BC_TSETM)) {
            switch (J->fn->c.ffid) {
            case FF_error:
            case FF_debug_setHook:
            case FF_jit_flush:
               break;  //  Don't stitch across special builtins.
            default:
               recff_stitch(J);  //  Use trace stitching.
               rd->nres = -1;
               return;
            }
         }
      }
      // Otherwise stop trace and return to interpreter.
      lj_record_stop(J, TraceLink::RETURN, 0);
      rd->nres = -1;
   }
}

// Fallback handler for unsupported variants of fast functions.
#define recff_nyiu   recff_nyi

// Must stop the trace for classic C functions with arbitrary side-effects.
#define recff_c      recff_nyi

//********************************************************************************************************************
// Emit BUFHDR for the global temporary buffer.

static TRef recff_bufhdr(jit_State* J)
{
   return emitir(IRT(IR_BUFHDR, IRT_PGC), lj_ir_kptr(J, &J2G(J)->tmpbuf), IRBUFHDR_RESET);
}

//********************************************************************************************************************
// Emit TMPREF.

static TRef recff_tmpref(jit_State* J, TRef tr, int mode)
{
   if (!LJ_DUALNUM and tref_isinteger(tr)) tr = emitir(IRTN(IR_CONV), tr, IRCONV_NUM_INT);
   return emitir(IRT(IR_TMPREF, IRT_PGC), tr, mode);
}

//********************************************************************************************************************
// Emit IR call without varargs (Windows x64 vararg safety).

static TRef recff_ir_call_fixed(jit_State* J, IRCallID CallId, TRef Arg1, TRef Arg2, TRef Arg3, TRef Arg4)
{
   const CCallInfo* call_info = &lj_ir_callinfo[CallId];
   uint32_t nargs = CCI_NARGS(call_info);
   if (call_info->flags & CCI_L) nargs--;

   TRef carg = TREF_NIL;
   if (nargs IS 1) {
      carg = Arg1;
   }
   else if (nargs IS 2) {
      carg = emitir(IRT(IR_CARG, IRT_NIL), Arg1, Arg2);
   }
   else if (nargs IS 3) {
      carg = emitir(IRT(IR_CARG, IRT_NIL), Arg1, Arg2);
      carg = emitir(IRT(IR_CARG, IRT_NIL), carg, Arg3);
   }
   else {
      lj_assertJ(nargs IS 4, "unexpected fixed call arg count");
      carg = emitir(IRT(IR_CARG, IRT_NIL), Arg1, Arg2);
      carg = emitir(IRT(IR_CARG, IRT_NIL), carg, Arg3);
      carg = emitir(IRT(IR_CARG, IRT_NIL), carg, Arg4);
   }
   if (CCI_OP(call_info) IS IR_CALLS) J->needsnap = 1;
   return emitir(CCI_OPTYPE(call_info), carg, CallId);
}

//********************************************************************************************************************
// Base library fast functions

static void recff_assert(jit_State* J, RecordFFData* rd)
{
   // Arguments already specialized. The interpreter throws for nil/false.
   rd->nres = 0;  // Returns no values (void).
}

//********************************************************************************************************************

static void recff_type(jit_State* J, RecordFFData* rd)
{
   // Arguments already specialized. Result is a constant string. Neat, huh?
   uint32_t t;
   if (tvisnumber(&rd->argv[0])) t = ~LJ_TNUMX;
   else t = ~itype(&rd->argv[0]);

   // Check for thunk userdata with declared type

   if (t IS ~LJ_TUDATA) {  // 12 = base value for userdata
      GCudata *ud = udataV(&rd->argv[0]);
      if (ud->udtype IS UDTYPE_THUNK) {
         ThunkPayload *payload = thunk_payload(ud);
         if (payload->expected_type != 0xFF) {
            // Use the declared type instead of userdata
            t = payload->expected_type;
         }
      }
   }

   J->base[0] = lj_ir_kstr(J, strV(&J->fn->c.upvalue[t]));
   UNUSED(rd);
}

//********************************************************************************************************************

static void recff_getmetatable(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   if (tr) {
      RecordIndex ix;
      ix.tab = tr;
      copyTV(J->L, &ix.tabv, &rd->argv[0]);
      if (lj_record_mm_lookup(J, &ix, MM_metatable)) J->base[0] = ix.mobj;
      else J->base[0] = ix.mt;
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static void recff_setmetatable(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   TRef mt = J->base[1];
   if (tref_istab(tr) and (tref_istab(mt) or (mt and tref_isnil(mt)))) {
      TRef fref, mtref;
      RecordIndex ix;
      ix.tab = tr;
      copyTV(J->L, &ix.tabv, &rd->argv[0]);
      lj_record_mm_lookup(J, &ix, MM_metatable); //  Guard for no __metatable.
      fref = emitir(IRT(IR_FREF, IRT_PGC), tr, IRFL_TAB_META);
      mtref = tref_isnil(mt) ? lj_ir_knull(J, IRT_TAB) : mt;
      emitir(IRT(IR_FSTORE, IRT_TAB), fref, mtref);
      if (!tref_isnil(mt)) emitir(IRT(IR_TBAR, IRT_TAB), tr, 0);
      J->base[0] = tr;
      J->needsnap = 1;
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static void recff_rawget(jit_State* J, RecordFFData* rd)
{
   RecordIndex ix;
   ix.tab = J->base[0]; ix.key = J->base[1];
   if (tref_istab(ix.tab) and ix.key) {
      ix.val = 0; ix.idxchain = 0;
      settabV(J->L, &ix.tabv, tabV(&rd->argv[0]));
      copyTV(J->L, &ix.keyv, &rd->argv[1]);
      J->base[0] = lj_record_idx(J, &ix);
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static void recff_rawset(jit_State* J, RecordFFData* rd)
{
   RecordIndex ix;
   ix.tab = J->base[0]; ix.key = J->base[1]; ix.val = J->base[2];
   if (tref_istab(ix.tab) and ix.key and ix.val) {
      ix.idxchain = 0;
      settabV(J->L, &ix.tabv, tabV(&rd->argv[0]));
      copyTV(J->L, &ix.keyv, &rd->argv[1]);
      copyTV(J->L, &ix.valv, &rd->argv[2]);
      lj_record_idx(J, &ix);
      // Pass through table at J->base[0] as result.
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static void recff_rawequal(jit_State* J, RecordFFData* rd)
{
   TRef tra = J->base[0];
   TRef trb = J->base[1];
   if (tra and trb) {
      int diff = lj_record_objcmp(J, tra, trb, &rd->argv[0], &rd->argv[1]);
      J->base[0] = diff ? TREF_FALSE : TREF_TRUE;
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static void recff_rawlen(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   if (tref_isstr(tr)) J->base[0] = emitir(IRTI(IR_FLOAD), tr, IRFL_STR_LEN);
   else if (tref_istab(tr)) J->base[0] = emitir(IRTI(IR_ALEN), tr, TREF_NIL);
   else if (tref_isarray(tr)) J->base[0] = emitir(IRTI(IR_FLOAD), tr, IRFL_ARRAY_LEN);
   // else: Interpreter will throw.
   UNUSED(rd);
}


//********************************************************************************************************************
// Record __filter(mask, count, trailing_keep, ...)
// Filters return values based on a bitmask pattern compiled at parse time.

static void recff___filter(jit_State* J, RecordFFData* rd)
{
   TRef tr_mask = J->base[0];
   TRef tr_count = J->base[1];
   TRef tr_trailing = J->base[2];

   // All three parameters must be constants for JIT compilation
   // (they're always constant since they're emitted by the parser)

   if (!tr_mask or !tr_count or !tr_trailing) {
      recff_nyiu(J, rd);
      return;
   }

   if (!tref_isk(tr_mask) or !tref_isk(tr_count) or !tref_isk(tr_trailing)) {
      recff_nyiu(J, rd);  // NYI: non-constant filter parameters
      return;
   }

   // Extract constant values
   // The mask may be IR_KNUM (floating point) or IR_KINT (integer) depending on the value

   IRIns* ir_mask = IR(tref_ref(tr_mask));
   uint64_t mask;
   if (ir_mask->o IS IR_KNUM) mask = uint64_t(ir_knum(ir_mask)->u64);
   else mask = uint64_t(uint32_t(ir_mask->i));  // IR_KINT stores 32-bit integer

   int32_t count = IR(tref_ref(tr_count))->i;
   bool trailing_keep = !tref_isfalse(tr_trailing);

   // Calculate which values to keep

   ptrdiff_t n = ptrdiff_t(J->maxslot);
   ptrdiff_t value_start = 3;  // Values start at slot 3
   ptrdiff_t value_count = n - value_start;

   // Build output by copying kept values
   int32_t out_idx = 0;
   for (ptrdiff_t i = 0; i < value_count; i++) {
      bool keep;
      if (i < count) keep = (mask & (1ULL << i)) != 0;
      else keep = trailing_keep;

      if (keep) {
         TRef tr = J->base[value_start + i];
         if (tr) {
            J->base[out_idx] = tr;
            out_idx++;
         }
      }
   }

   rd->nres = out_idx;
}

//********************************************************************************************************************

static void recff_tonumber(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   TRef base = J->base[1];
   if (tr and !tref_isnil(base)) {
      base = lj_opt_narrow_toint(J, base);
      if (!tref_isk(base) or IR(tref_ref(base))->i != 10) {
         recff_nyiu(J, rd);
         return;
      }
   }
   if (tref_isnumber_str(tr)) {
      if (tref_isstr(tr)) {
         TValue tmp;
         if (!lj_strscan_num(strV(&rd->argv[0]), &tmp)) {
            recff_nyiu(J, rd);  //  Would need an inverted STRTO for this case.
            return;
         }
         tr = emitir(IRTG(IR_STRTO, IRT_NUM), tr, 0);
      }
   }
   else {
      tr = TREF_NIL;
   }
   J->base[0] = tr;
}

//********************************************************************************************************************

static TValue* recff_metacall_cp(lua_State* L, lua_CFunction dummy, void* ud)
{
   jit_State* J = (jit_State*)ud;
   lj_record_tailcall(J, 0, 1);
   return nullptr;
}

//********************************************************************************************************************

static int recff_metacall(jit_State* J, RecordFFData* rd, MMS mm)
{
   RecordIndex ix;
   ix.tab = J->base[0];
   copyTV(J->L, &ix.tabv, &rd->argv[0]);
   if (lj_record_mm_lookup(J, &ix, mm)) {  // Has metamethod?
      int errcode;
      TValue argv0;
      // Temporarily insert metamethod below object.
      J->base[1 + LJ_FR2] = J->base[0];
      J->base[0] = ix.mobj;
      copyTV(J->L, &argv0, &rd->argv[0]);
      copyTV(J->L, &rd->argv[1 + LJ_FR2], &rd->argv[0]);
      copyTV(J->L, &rd->argv[0], &ix.mobjv);
      // Need to protect lj_record_tailcall because it may throw.
      errcode = lj_vm_cpcall(J->L, nullptr, J, recff_metacall_cp);
      // Always undo Lua stack changes to avoid confusing the interpreter.
      copyTV(J->L, &rd->argv[0], &argv0);
      if (errcode)
         lj_err_throw(J->L, errcode);  //  Propagate errors.
      rd->nres = -1;  //  Pending call.
      return 1;  //  Tailcalled to metamethod.
   }
   return 0;
}

//********************************************************************************************************************

static void recff_tostring(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   if (tref_isstr(tr)) {
      // Ignore __tostring in the string base metatable.
      // Pass on result in J->base[0].
   }
   else if (tr and !recff_metacall(J, rd, MM_tostring)) {
      if (tref_isnumber(tr)) {
         J->base[0] = emitir(IRT(IR_TOSTR, IRT_STR), tr, tref_isnum(tr) ? IRTOSTR_NUM : IRTOSTR_INT);
      }
      else if (tref_ispri(tr)) {
         J->base[0] = lj_ir_kstr(J, lj_strfmt_obj(J->L, &rd->argv[0]));
      }
      else {
         TRef tmp_ref = recff_tmpref(J, tr, IRTMPREF_IN1);
         J->base[0] = lj_ir_call(J, IRCALL_lj_strfmt_obj, tmp_ref);
      }
   }
}

//********************************************************************************************************************

static bool array_elem_irtype(AET ElemType, IRType &ResultType)
{
   switch (ElemType) {
      case AET::BYTE:
      case AET::INT16:
      case AET::INT32:
         ResultType = LJ_DUALNUM ? IRT_INT : IRT_NUM;
         return true;
      case AET::INT64:
      case AET::FLOAT:
      case AET::DOUBLE:
         ResultType = IRT_NUM;
         return true;
      case AET::PTR:
         ResultType = IRT_LIGHTUD;
         return true;
      case AET::CSTR:
      case AET::STR_CPP:
      case AET::STR_GC:
         ResultType = IRT_STR;
         return true;
      case AET::TABLE:
         ResultType = IRT_TAB;
         return true;
      default:
         return false;
   }
}

//********************************************************************************************************************

static void recff_ipairs_aux(jit_State* J, RecordFFData* rd)
{
   RecordIndex ix;
   ix.tab = J->base[0];
   if (tref_istab(ix.tab)) {
      if (!tvisnumber(&rd->argv[1]))  //  No support for string coercion.
         lj_trace_err(J, LJ_TRERR_BADTYPE);
      setintV(&ix.keyv, numberVint(&rd->argv[1]) + 1);
      settabV(J->L, &ix.tabv, tabV(&rd->argv[0]));
      ix.val = 0;
      ix.idxchain = 0;
      ix.key = lj_opt_narrow_toint(J, J->base[1]);
      J->base[0] = ix.key = emitir(IRTI(IR_ADD), ix.key, lj_ir_kint(J, 1));
      J->base[1] = lj_record_idx(J, &ix);
      rd->nres = tref_isnil(J->base[1]) ? 0 : 2;
   }
   else if (tref_isarray(ix.tab)) {
      if (not tvisnumber(&rd->argv[1])) lj_trace_err(J, LJ_TRERR_BADTYPE);  //  No support for string coercion.

      GCarray *arr = arrayV(&rd->argv[0]);
      int32_t idx_int;
      if (tvisint(&rd->argv[1])) idx_int = intV(&rd->argv[1]) + 1;
      else idx_int = int32_t(lj_num2int(numV(&rd->argv[1]))) + 1;

      TRef idx_ref = lj_opt_narrow_toint(J, J->base[1]);
      idx_ref = emitir(IRTI(IR_ADD), idx_ref, lj_ir_kint(J, 1));
      TRef len_ref = emitir(IRTI(IR_FLOAD), ix.tab, IRFL_ARRAY_LEN);

      if (idx_int < 0 or MSize(idx_int) >= arr->len) {
         emitir(IRTGI(IR_UGE), idx_ref, len_ref);
         rd->nres = 0;
         return;
      }

      emitir(IRTGI(IR_ULT), idx_ref, len_ref);
      J->base[0] = idx_ref;

      TValue result_tv;
      lj_arr_getidx(J->L, arr, idx_int, &result_tv);
      IRType result_type;
      if (not array_elem_irtype(arr->elemtype, result_type) or tvisnil(&result_tv)) {
         result_type = itype2irt(&result_tv);
      }
      if (!LJ_DUALNUM and result_type IS IRT_INT) result_type = IRT_NUM;
      TRef tmp_ref = recff_tmpref(J, TREF_NIL, IRTMPREF_OUT1);
      recff_ir_call_fixed(J, IRCALL_lj_arr_getidx, ix.tab, idx_ref, tmp_ref, TREF_NIL);
      J->base[1] = lj_record_vload(J, tmp_ref, 0, result_type);
      rd->nres = 2;
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static void recff_xpairs(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   if (!(recff_metacall(J, rd, (MMS)(MM_pairs + rd->data)))) {
      if (tref_istab(tr)) {
         J->base[0] = lj_ir_kfunc(J, funcV(&J->fn->c.upvalue[0]));
         J->base[1] = tr;
         J->base[2] = rd->data ? lj_ir_kint(J, -1) : TREF_NIL;  // 0-based: ipairs starts at -1
         rd->nres = 3;
      }
      else if (tref_isarray(tr)) {
         J->base[0] = lj_ir_kfunc(J, funcV(&J->fn->c.upvalue[0]));
         J->base[1] = tr;
         J->base[2] = rd->data ? lj_ir_kint(J, -1) : TREF_NIL;  // 0-based: ipairs starts at -1
         rd->nres = 3;
      }  // else: Interpreter will throw.
   }
}

//********************************************************************************************************************

static void recff_next(jit_State* J, RecordFFData* rd)
{
#if LJ_BE
   // YAGNI: Disabled on big-endian due to issues with lj_vm_next, IR_HIOP, RID_RETLO/RID_RETHI and ra_destpair.
   recff_nyi(J, rd);
#else
   TRef tab = J->base[0];
   if (tref_istab(tab)) {
      RecordIndex ix;
      cTValue* keyv;
      ix.tab = tab;
      if (tref_isnil(J->base[1])) {  // Shortcut for start of traversal.
         ix.key = lj_ir_kint(J, 0);
         keyv = niltvg(J2G(J));
      }
      else {
         TRef tmp = recff_tmpref(J, J->base[1], IRTMPREF_IN1);
         ix.key = lj_ir_call(J, IRCALL_lj_tab_keyindex, tab, tmp);
         keyv = &rd->argv[1];
      }
      copyTV(J->L, &ix.tabv, &rd->argv[0]);
      ix.keyv.u32.lo = lj_tab_keyindex(tabV(&ix.tabv), keyv);
      // Omit the value, if not used by the caller.
      ix.idxchain = (J->framedepth and frame_islua(J->L->base - 1) and
         bc_b(frame_pc(J->L->base - 1)[-1]) - 1 < 2);
      ix.mobj = 0;  //  We don't need the next index.
      rd->nres = lj_record_next(J, &ix);
      J->base[0] = ix.key;
      J->base[1] = ix.val;
   }
   else if (tref_isarray(tab)) {
      GCarray *arr = arrayV(&rd->argv[0]);
      cTValue *key_tv = &rd->argv[1];
      int32_t idx_int = 0;

      if (tvisnil(key_tv)) {
         idx_int = 0;
      }
      else if (tvisint(key_tv)) {
         int32_t key_int = intV(key_tv);
         if (key_int < 0 or MSize(key_int) >= arr->len) lj_trace_err(J, LJ_TRERR_BADTYPE);
         idx_int = key_int + 1;
      }
      else if (tvisnum(key_tv)) {
         lua_Number num = numV(key_tv);
         int32_t key_int = int32_t(lj_num2int(num));
         if (not ((lua_Number)key_int IS num)) lj_trace_err(J, LJ_TRERR_BADTYPE);
         if (key_int < 0 or MSize(key_int) >= arr->len) lj_trace_err(J, LJ_TRERR_BADTYPE);
         idx_int = key_int + 1;
      }
      else {
         lj_trace_err(J, LJ_TRERR_BADTYPE);
      }

      TRef len_ref = emitir(IRTI(IR_FLOAD), tab, IRFL_ARRAY_LEN);
      TRef idx_ref;

      if (tref_isnil(J->base[1])) {
         idx_ref = lj_ir_kint(J, 0);
      }
      else {
         if (not tref_isnumber(J->base[1])) lj_trace_err(J, LJ_TRERR_BADTYPE);
         TRef key_ref = lj_opt_narrow_index(J, J->base[1]);
         emitir(IRTGI(IR_ULT), key_ref, len_ref);
         idx_ref = emitir(IRTI(IR_ADD), key_ref, lj_ir_kint(J, 1));
      }

      if (idx_int < 0 or MSize(idx_int) >= arr->len) {
         emitir(IRTGI(IR_UGE), idx_ref, len_ref);
         J->base[0] = TREF_NIL;
         rd->nres = 1;
         return;
      }

      emitir(IRTGI(IR_ULT), idx_ref, len_ref);
      J->base[0] = idx_ref;

      TValue result_tv;
      lj_arr_getidx(J->L, arr, idx_int, &result_tv);
      IRType result_type;
      if (not array_elem_irtype(arr->elemtype, result_type) or tvisnil(&result_tv)) {
         result_type = itype2irt(&result_tv);
      }
      if (!LJ_DUALNUM and result_type IS IRT_INT) result_type = IRT_NUM;
      TRef tmp_ref = recff_tmpref(J, TREF_NIL, IRTMPREF_OUT1);
      recff_ir_call_fixed(J, IRCALL_lj_arr_getidx, tab, idx_ref, tmp_ref, TREF_NIL);
      J->base[1] = lj_record_vload(J, tmp_ref, 0, result_type);
      rd->nres = 2;
   }  // else: Interpreter will throw.
#endif
}

//********************************************************************************************************************
// Math library fast functions

static void recff_math_abs(jit_State* J, RecordFFData* rd)
{
   TRef tr = lj_ir_tonum(J, J->base[0]);
   J->base[0] = emitir(IRTN(IR_ABS), tr, lj_ir_ksimd(J, LJ_KSIMD_ABS));
   UNUSED(rd);
}

//********************************************************************************************************************
// Record rounding functions math.floor and math.ceil.

static void recff_math_round(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   if (!tref_isinteger(tr)) {  // Pass through integers unmodified.
      tr = emitir(IRTN(IR_FPMATH), lj_ir_tonum(J, tr), rd->data);
      // Result is integral (or NaN/Inf), but may not fit an int32_t.
      if (LJ_DUALNUM) {  // Try to narrow using a guarded conversion to int.
         lua_Number n = lj_vm_foldfpm(numberVnum(&rd->argv[0]), rd->data);
         if (n == (lua_Number)lj_num2int(n))
            tr = emitir(IRTGI(IR_CONV), tr, IRCONV_INT_NUM | IRCONV_CHECK);
      }
      J->base[0] = tr;
   }
}

// Record unary math.* functions, mapped to IR_FPMATH opcode.
static void recff_math_unary(jit_State* J, RecordFFData* rd)
{
   J->base[0] = emitir(IRTN(IR_FPMATH), lj_ir_tonum(J, J->base[0]), rd->data);
}

//********************************************************************************************************************
// Record math.log.

static void recff_math_log(jit_State* J, RecordFFData* rd)
{
   TRef tr = lj_ir_tonum(J, J->base[0]);
   if (J->base[1]) {
#ifdef LUAJIT_NO_LOG2
      uint32_t fpm = IRFPM_LOG;
#else
      uint32_t fpm = IRFPM_LOG2;
#endif
      TRef trb = lj_ir_tonum(J, J->base[1]);
      tr = emitir(IRTN(IR_FPMATH), tr, fpm);
      trb = emitir(IRTN(IR_FPMATH), trb, fpm);
      trb = emitir(IRTN(IR_DIV), lj_ir_knum_one(J), trb);
      tr = emitir(IRTN(IR_MUL), tr, trb);
   }
   else {
      tr = emitir(IRTN(IR_FPMATH), tr, IRFPM_LOG);
   }
   J->base[0] = tr;
   UNUSED(rd);
}

//********************************************************************************************************************
// Record math.atan2.

static void recff_math_atan2(jit_State* J, RecordFFData* rd)
{
   TRef tr = lj_ir_tonum(J, J->base[0]);
   TRef tr2 = lj_ir_tonum(J, J->base[1]);
   J->base[0] = lj_ir_call(J, IRCALL_cmath_atan2, tr, tr2);
   UNUSED(rd);
}

//********************************************************************************************************************
// Record math.ldexp.

static void recff_math_ldexp(jit_State* J, RecordFFData* rd)
{
   TRef tr = lj_ir_tonum(J, J->base[0]);
#if LJ_TARGET_X86ORX64
   TRef tr2 = lj_ir_tonum(J, J->base[1]);
#else
   TRef tr2 = lj_opt_narrow_toint(J, J->base[1]);
#endif
   J->base[0] = emitir(IRTN(IR_LDEXP), tr, tr2);
}

//********************************************************************************************************************

static void recff_math_call(jit_State* J, RecordFFData* rd)
{
   TRef tr = lj_ir_tonum(J, J->base[0]);
   J->base[0] = emitir(IRTN(IR_CALLN), tr, rd->data);
}

//********************************************************************************************************************

static void recff_math_pow(jit_State* J, RecordFFData* rd)
{
   J->base[0] = lj_opt_narrow_pow(J, J->base[0], J->base[1], &rd->argv[0], &rd->argv[1]);
}

//********************************************************************************************************************

static void recff_math_minmax(jit_State* J, RecordFFData* rd)
{
   TRef tr = lj_ir_tonumber(J, J->base[0]);
   uint32_t op = rd->data;
   BCREG i;
   for (i = 1; J->base[i] != 0; i++) {
      TRef tr2 = lj_ir_tonumber(J, J->base[i]);
      IRType t = IRT_INT;
      if (!(tref_isinteger(tr) and tref_isinteger(tr2))) {
         if (tref_isinteger(tr)) tr = emitir(IRTN(IR_CONV), tr, IRCONV_NUM_INT);
         if (tref_isinteger(tr2)) tr2 = emitir(IRTN(IR_CONV), tr2, IRCONV_NUM_INT);
         t = IRT_NUM;
      }
      tr = emitir(IRT(op, t), tr, tr2);
   }
   J->base[0] = tr;
}

static void recff_math_random(jit_State* J, RecordFFData* rd)
{
   GCudata* ud = udataV(&J->fn->c.upvalue[0]);
   TRef tr, one;
   lj_ir_kgc(J, obj2gco(ud), IRT_UDATA);  //  Prevent collection.
   tr = lj_ir_call(J, IRCALL_lj_prng_u64d, lj_ir_kptr(J, uddata(ud)));
   one = lj_ir_knum_one(J);
   tr = emitir(IRTN(IR_SUB), tr, one);
   if (J->base[0]) {
      TRef tr1 = lj_ir_tonum(J, J->base[0]);
      if (J->base[1]) {  // d = floor(d*(r2-r1+1.0)) + r1
         TRef tr2 = lj_ir_tonum(J, J->base[1]);
         tr2 = emitir(IRTN(IR_SUB), tr2, tr1);
         tr2 = emitir(IRTN(IR_ADD), tr2, one);
         tr = emitir(IRTN(IR_MUL), tr, tr2);
         tr = emitir(IRTN(IR_FPMATH), tr, IRFPM_FLOOR);
         tr = emitir(IRTN(IR_ADD), tr, tr1);
      }
      else {  // d = floor(d*r1) + 1.0
         tr = emitir(IRTN(IR_MUL), tr, tr1);
         tr = emitir(IRTN(IR_FPMATH), tr, IRFPM_FLOOR);
         tr = emitir(IRTN(IR_ADD), tr, one);
      }
   }
   J->base[0] = tr;
   UNUSED(rd);
}

//********************************************************************************************************************
// Bit library fast functions

// Record bit.tobit.

static void recff_bit_tobit(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   J->base[0] = lj_opt_narrow_tobit(J, tr);
   UNUSED(rd);
}

//********************************************************************************************************************
// Record unary bit.bnot, bit.bswap.

static void recff_bit_unary(jit_State* J, RecordFFData* rd)
{
#if LJ_HASFFI
   if (recff_bit64_unary(J, rd)) return;
#endif
   J->base[0] = emitir(IRTI(rd->data), lj_opt_narrow_tobit(J, J->base[0]), 0);
}

//********************************************************************************************************************
// Record N-ary bit.band, bit.bor, bit.bxor.

static void recff_bit_nary(jit_State* J, RecordFFData* rd)
{
#if LJ_HASFFI
   if (recff_bit64_nary(J, rd))
      return;
#endif
   {
      TRef tr = lj_opt_narrow_tobit(J, J->base[0]);
      uint32_t ot = IRTI(rd->data);
      BCREG i;
      for (i = 1; J->base[i] != 0; i++)
         tr = emitir(ot, tr, lj_opt_narrow_tobit(J, J->base[i]));
      J->base[0] = tr;
   }
}

//********************************************************************************************************************
// Record bit shifts.

static void recff_bit_shift(jit_State* J, RecordFFData* rd)
{
#if LJ_HASFFI
   if (recff_bit64_shift(J, rd))
      return;
#endif
   {
      TRef tr = lj_opt_narrow_tobit(J, J->base[0]);
      TRef tsh = lj_opt_narrow_tobit(J, J->base[1]);
      IROp op = (IROp)rd->data;
      if (!(op < IR_BROL ? LJ_TARGET_MASKSHIFT : LJ_TARGET_MASKROT) and
         !tref_isk(tsh))
         tsh = emitir(IRTI(IR_BAND), tsh, lj_ir_kint(J, 31));
#ifdef LJ_TARGET_UNIFYROT
      if (op == (LJ_TARGET_UNIFYROT == 1 ? IR_BROR : IR_BROL)) {
         op = LJ_TARGET_UNIFYROT == 1 ? IR_BROL : IR_BROR;
         tsh = emitir(IRTI(IR_NEG), tsh, tsh);
      }
#endif
      J->base[0] = emitir(IRTI(op), tr, tsh);
   }
}

static void recff_bit_tohex(jit_State* J, RecordFFData* rd)
{
#if LJ_HASFFI
   TRef hdr = recff_bufhdr(J);
   TRef tr = recff_bit64_tohex(J, rd, hdr);
   J->base[0] = emitir(IRTG(IR_BUFSTR, IRT_STR), tr, hdr);
#else
   recff_nyiu(J, rd);  //  Don't bother working around this NYI.
#endif
}

//********************************************************************************************************************
// String library fast functions

// Specialize to relative starting position for string (0-based indexing).

static TRef recff_string_start(jit_State* J, GCstr* s, int32_t* st, TRef tr, TRef trlen, TRef tr0)
{
   int32_t start = *st;
   if (start < 0) {
      // Negative index: convert to 0-based (e.g., -1 -> len-1)
      emitir(IRTGI(IR_LT), tr, tr0);
      tr = emitir(IRTI(IR_ADD), trlen, tr);
      start = start + (int32_t)s->len;
      emitir(start < 0 ? IRTGI(IR_LT) : IRTGI(IR_GE), tr, tr0);
      if (start < 0) {
         tr = tr0;
         start = 0;
      }
   }
   else {
      // 0-based: positive indices are used as-is, just verify >= 0
      emitir(IRTGI(IR_GE), tr, tr0);
   }
   *st = start;
   return tr;
}

//********************************************************************************************************************
// Handle string.byte (rd->data = 0) and string.sub (rd->data = 1).

static void recff_string_range(jit_State* J, RecordFFData* rd)
{
   //static bool triggered = false;
   //if (not triggered) { printf("---recff_string_range---\n"); triggered = true; }

   TRef trstr = lj_ir_tostr(J, J->base[0]);
   TRef trlen = emitir(IRTI(IR_FLOAD), trstr, IRFL_STR_LEN);
   TRef tr0 = lj_ir_kint(J, 0);
   TRef trstart, trend;
   GCstr* str = argv2str(J, &rd->argv[0]);
   int32_t start, end;
   if (rd->data) {  // string.sub(str, start [,end]) - end is exclusive
      start = argv2int(J, &rd->argv[1]);
      trstart = lj_opt_narrow_toint(J, J->base[1]);
      trend = J->base[2];
      if (tref_isnil(trend)) {
         trend = lj_ir_kint(J, -1);
         end = -1;
      }
      else {
         trend = lj_opt_narrow_toint(J, trend);
         end = argv2int(J, &rd->argv[2]);
         // Convert exclusive end to inclusive (only for positive values)
         if (end > 0) {
            end--;
            trend = emitir(IRTI(IR_ADD), trend, lj_ir_kint(J, -1));
         }
      }
   }
   else {  // string.byte(str, [,start [,end]])
      if (tref_isnil(J->base[1])) {
         start = 0;  // 0-based: default start is 0
         trstart = lj_ir_kint(J, 0);
      }
      else {
         start = argv2int(J, &rd->argv[1]);
         trstart = lj_opt_narrow_toint(J, J->base[1]);
      }

      if (J->base[1] and !tref_isnil(J->base[2])) {
         trend = lj_opt_narrow_toint(J, J->base[2]);
         end = argv2int(J, &rd->argv[2]);
      }
      else {
         trend = trstart;
         end = start;
      }
   }
   if (end < 0) {
      // 0-based: -1 -> len-1, -2 -> len-2, etc.
      emitir(IRTGI(IR_LT), trend, tr0);
      trend = emitir(IRTI(IR_ADD), trlen, trend);
      end = end + (int32_t)str->len;
   }
   else {
      // 0-based: end is inclusive, max valid is len-1
      TRef trmax = emitir(IRTI(IR_ADD), trlen, lj_ir_kint(J, -1));
      if ((MSize)end < str->len) {
         emitir(IRTGI(IR_ULE), trend, trmax);
      }
      else {
         emitir(IRTGI(IR_UGT), trend, trmax);
         end = (int32_t)str->len - 1;
         trend = trmax;
      }
   }
   trstart = recff_string_start(J, str, &start, trstart, trlen, tr0);
   if (rd->data) {  // Return string.sub result.
      if (end - start >= 0) {
         // 0-based inclusive: length = end - start + 1
         TRef trptr, trslen = emitir(IRTI(IR_SUB), trend, trstart);
         trslen = emitir(IRTI(IR_ADD), trslen, lj_ir_kint(J, 1));
         emitir(IRTGI(IR_GE), trslen, tr0);
         trptr = emitir(IRT(IR_STRREF, IRT_PGC), trstr, trstart);
         J->base[0] = emitir(IRT(IR_SNEW, IRT_STR), trptr, trslen);
      }
      else {  // Range underflow: return empty string.
         emitir(IRTGI(IR_LT), trend, trstart);
         J->base[0] = lj_ir_kstr(J, &J2G(J)->strempty);
      }
   }
   else {  // Return string.byte result(s).
      // 0-based inclusive: count = end - start + 1
      ptrdiff_t i, count = end - start + 1;
      if (count > 0) {
         TRef trslen = emitir(IRTI(IR_SUB), trend, trstart);
         trslen = emitir(IRTI(IR_ADD), trslen, lj_ir_kint(J, 1));
         emitir(IRTGI(IR_EQ), trslen, lj_ir_kint(J, (int32_t)count));
         if (J->baseslot + count > LJ_MAX_JSLOTS)
            lj_trace_err_info(J, LJ_TRERR_STACKOV);
         rd->nres = count;
         for (i = 0; i < count; i++) {
            TRef tmp = emitir(IRTI(IR_ADD), trstart, lj_ir_kint(J, (int32_t)i));
            tmp = emitir(IRT(IR_STRREF, IRT_PGC), trstr, tmp);
            J->base[i] = emitir(IRT(IR_XLOAD, IRT_U8), tmp, IRXLOAD_READONLY);
         }
      }
      else {  // Empty range or range underflow: return no results.
         emitir(IRTGI(IR_LE), trend, trstart);
         rd->nres = 0;
      }
   }
}

//********************************************************************************************************************

static void recff_string_char(jit_State* J, RecordFFData* rd)
{
   TRef k255 = lj_ir_kint(J, 255);
   BCREG i;
   for (i = 0; J->base[i] != 0; i++) {  // Convert char values to strings.
      TRef tr = lj_opt_narrow_toint(J, J->base[i]);
      emitir(IRTGI(IR_ULE), tr, k255);
      J->base[i] = emitir(IRT(IR_TOSTR, IRT_STR), tr, IRTOSTR_CHAR);
   }
   if (i > 1) {  // Concatenate the strings, if there's more than one.
      TRef hdr = recff_bufhdr(J), tr = hdr;
      for (i = 0; J->base[i] != 0; i++)
         tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr, J->base[i]);
      J->base[0] = emitir(IRTG(IR_BUFSTR, IRT_STR), tr, hdr);
   }
   else if (i == 0) J->base[0] = lj_ir_kstr(J, &J2G(J)->strempty);
}

//********************************************************************************************************************

static void recff_string_rep(jit_State* J, RecordFFData* rd)
{
   TRef str = lj_ir_tostr(J, J->base[0]);
   TRef rep = lj_opt_narrow_toint(J, J->base[1]);
   TRef hdr, tr, str2 = 0;
   if (!tref_isnil(J->base[2])) {
      TRef sep = lj_ir_tostr(J, J->base[2]);
      int32_t vrep = argv2int(J, &rd->argv[1]);
      emitir(IRTGI(vrep > 1 ? IR_GT : IR_LE), rep, lj_ir_kint(J, 1));
      if (vrep > 1) {
         TRef hdr2 = recff_bufhdr(J);
         TRef tr2 = emitir(IRTG(IR_BUFPUT, IRT_PGC), hdr2, sep);
         tr2 = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr2, str);
         str2 = emitir(IRTG(IR_BUFSTR, IRT_STR), tr2, hdr2);
      }
   }
   tr = hdr = recff_bufhdr(J);
   if (str2) {
      tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr, str);
      str = str2;
      rep = emitir(IRTI(IR_ADD), rep, lj_ir_kint(J, -1));
   }
   tr = lj_ir_call(J, IRCALL_lj_buf_putstr_rep, tr, str, rep);
   J->base[0] = emitir(IRTG(IR_BUFSTR, IRT_STR), tr, hdr);
}

//********************************************************************************************************************

static void recff_string_op(jit_State* J, RecordFFData* rd)
{
   TRef str = lj_ir_tostr(J, J->base[0]);
   TRef hdr = recff_bufhdr(J);
   TRef tr = lj_ir_call(J, (IRCallID)rd->data, hdr, str);
   J->base[0] = emitir(IRTG(IR_BUFSTR, IRT_STR), tr, hdr);
}

//********************************************************************************************************************

static void recff_string_find(jit_State* J, RecordFFData* rd)
{
   TRef trstr = lj_ir_tostr(J, J->base[0]);
   TRef trpat = lj_ir_tostr(J, J->base[1]);
   TRef trlen = emitir(IRTI(IR_FLOAD), trstr, IRFL_STR_LEN);
   TRef tr0 = lj_ir_kint(J, 0);
   TRef trstart;
   GCstr* str = argv2str(J, &rd->argv[0]);
   GCstr* pat = argv2str(J, &rd->argv[1]);
   int32_t start;
   J->needsnap = 1;
   if (tref_isnil(J->base[2])) {
      trstart = lj_ir_kint(J, 0);  // 0-based: default start is 0
      start = 0;
   }
   else {
      trstart = lj_opt_narrow_toint(J, J->base[2]);
      start = argv2int(J, &rd->argv[2]);
   }
   trstart = recff_string_start(J, str, &start, trstart, trlen, tr0);
   if ((MSize)start <= str->len) {
      emitir(IRTGI(IR_ULE), trstart, trlen);
   }
   else {
      emitir(IRTGI(IR_UGT), trstart, trlen);
      J->base[0] = TREF_NIL;
      return;
   }
   // Fixed arg or no pattern matching chars? (Specialized to pattern string.)
   if ((J->base[2] and tref_istruecond(J->base[3])) or
      (emitir(IRTG(IR_EQ, IRT_STR), trpat, lj_ir_kstr(J, pat)),
         !lj_str_haspattern(pat))) {  // Search for fixed string.
      TRef trsptr = emitir(IRT(IR_STRREF, IRT_PGC), trstr, trstart);
      TRef trpptr = emitir(IRT(IR_STRREF, IRT_PGC), trpat, tr0);
      TRef trslen = emitir(IRTI(IR_SUB), trlen, trstart);
      TRef trplen = emitir(IRTI(IR_FLOAD), trpat, IRFL_STR_LEN);
      TRef tr = lj_ir_call(J, IRCALL_lj_str_find, trsptr, trpptr, trslen, trplen);
      TRef trp0 = lj_ir_kkptr(J, nullptr);
      if (lj_str_findsv({strdata(str) + MSize(start), str->len - MSize(start)},
         {strdata(pat), pat->len})) {
         TRef pos;
         emitir(IRTG(IR_NE, IRT_PGC), tr, trp0);
         // Recompute offset. trsptr may not point into trstr after folding.
         pos = emitir(IRTI(IR_ADD), emitir(IRTI(IR_SUB), tr, trsptr), trstart);
         // 0-based: return start position and inclusive end position
         J->base[0] = pos;
         J->base[1] = emitir(IRTI(IR_ADD), pos,
            emitir(IRTI(IR_ADD), trplen, lj_ir_kint(J, -1)));
         rd->nres = 2;
      }
      else {
         emitir(IRTG(IR_EQ, IRT_PGC), tr, trp0);
         J->base[0] = TREF_NIL;
      }
   }
   else {  // Search for pattern.
      recff_nyiu(J, rd);
      return;
   }
}

//********************************************************************************************************************

static void recff_format(jit_State* J, RecordFFData* rd, TRef hdr, int sbufx)
{
   ptrdiff_t arg = sbufx;
   TRef tr = hdr, trfmt = lj_ir_tostr(J, J->base[arg]);
   GCstr* fmt = argv2str(J, &rd->argv[arg]);
   FormatState fs;
   SFormat sf;
   // Specialize to the format string.
   emitir(IRTG(IR_EQ, IRT_STR), trfmt, lj_ir_kstr(J, fmt));
   lj_strfmt_init(&fs, strdata(fmt), fmt->len);
   while ((sf = lj_strfmt_parse(&fs)) != STRFMT_EOF) {  // Parse format.
      TRef tra = sf == STRFMT_LIT ? 0 : J->base[++arg];
      TRef trsf = lj_ir_kint(J, (int32_t)sf);
      IRCallID id;
      switch (STRFMT_TYPE(sf)) {
      case STRFMT_LIT:
         tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr, lj_ir_kstr(J, lj_str_new(J->L, fs.str, fs.len)));
         break;
      case STRFMT_INT:
         id = IRCALL_lj_strfmt_putfnum_int;
      handle_int:
         if (!tref_isinteger(tra)) {
            goto handle_num;
         }
         if (sf == STRFMT_INT) {  // Shortcut for plain %d.
            tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr,
               emitir(IRT(IR_TOSTR, IRT_STR), tra, IRTOSTR_INT));
         }
         else {
            recff_nyiu(J, rd);  //  Don't bother working around this NYI.
            return;
         }
         break;
      case STRFMT_UINT:
         id = IRCALL_lj_strfmt_putfnum_uint;
         goto handle_int;
      case STRFMT_NUM:
         id = IRCALL_lj_strfmt_putfnum;
      handle_num:
         tra = lj_ir_tonum(J, tra);
         tr = lj_ir_call(J, id, tr, trsf, tra);
         break;
      case STRFMT_STR:
         if (!tref_isstr(tra)) {
            recff_nyiu(J, rd);  //  NYI: __tostring and non-string types for %s.
            // NYI: also buffers.
            return;
         }
         if (sf == STRFMT_STR)  //  Shortcut for plain %s.
            tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr, tra);
         else if ((sf & STRFMT_T_QUOTED)) tr = lj_ir_call(J, IRCALL_lj_strfmt_putquoted, tr, tra);
         else tr = lj_ir_call(J, IRCALL_lj_strfmt_putfstr, tr, trsf, tra);
         break;
      case STRFMT_CHAR:
         tra = lj_opt_narrow_toint(J, tra);
         if (sf == STRFMT_CHAR)  //  Shortcut for plain %c.
            tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr, emitir(IRT(IR_TOSTR, IRT_STR), tra, IRTOSTR_CHAR));
         else tr = lj_ir_call(J, IRCALL_lj_strfmt_putfchar, tr, trsf, tra);
         break;
      case STRFMT_PTR:  //  NYI
      case STRFMT_ERR:
      default:
         recff_nyiu(J, rd);
         return;
      }
   }
   if (sbufx) {
      emitir(IRT(IR_USE, IRT_NIL), tr, 0);
   }
   else {
      J->base[0] = emitir(IRTG(IR_BUFSTR, IRT_STR), tr, hdr);
   }
}

//********************************************************************************************************************

static void recff_string_format(jit_State* J, RecordFFData* rd)
{
   recff_format(J, rd, recff_bufhdr(J), 0);
}

//********************************************************************************************************************
// Table library fast functions

static void recff_table_insert(jit_State* J, RecordFFData* rd)
{
   RecordIndex ix;
   ix.tab = J->base[0];
   ix.val = J->base[1];
   rd->nres = 0;
   if (tref_istab(ix.tab) and ix.val) {
      if (!J->base[2]) {  // Simple push: t[#t] = v (0-based: next index = len)
         TRef trlen = emitir(IRTI(IR_ALEN), ix.tab, TREF_NIL);
         GCtab* t = tabV(&rd->argv[0]);
         ix.key = trlen;  // 0-based: next available index is len
         settabV(J->L, &ix.tabv, t);
         setintV(&ix.keyv, lj_tab_len(t));  // 0-based: next index = len
         ix.idxchain = 0;
         lj_record_idx(J, &ix);  //  Set new value.
      }
      else {  // Complex case: insert in the middle.
         recff_nyiu(J, rd);
         return;
      }
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static void recff_table_concat(jit_State* J, RecordFFData* rd)
{
   TRef tab = J->base[0];
   if (tref_istab(tab)) {
      TRef sep = !tref_isnil(J->base[1]) ? lj_ir_tostr(J, J->base[1]) : lj_ir_knull(J, IRT_STR);
      TRef tri = (J->base[1] and !tref_isnil(J->base[2])) ? lj_opt_narrow_toint(J, J->base[2]) : lj_ir_kint(J, 0);  // 0-based: default start
      TRef tre = (J->base[1] and J->base[2] and !tref_isnil(J->base[3])) ?
         lj_opt_narrow_toint(J, J->base[3]) : emitir(IRTI(IR_ADD), emitir(IRTI(IR_ALEN), tab, TREF_NIL), lj_ir_kint(J, -1));  // 0-based: end = len - 1
      TRef hdr = recff_bufhdr(J);
      TRef tr = lj_ir_call(J, IRCALL_lj_buf_puttab, hdr, tab, sep, tri, tre);
      emitir(IRTG(IR_NE, IRT_PTR), tr, lj_ir_kptr(J, nullptr));
      J->base[0] = emitir(IRTG(IR_BUFSTR, IRT_STR), tr, hdr);
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static void recff_table_new(jit_State* J, RecordFFData* rd)
{
   TRef tra = lj_opt_narrow_toint(J, J->base[0]);
   TRef trh = lj_opt_narrow_toint(J, J->base[1]);
   J->base[0] = lj_ir_call(J, IRCALL_lj_tab_new_ah, tra, trh);
}

//********************************************************************************************************************

static void recff_table_clear(jit_State* J, RecordFFData* rd)
{
   TRef tr = J->base[0];
   if (tref_istab(tr)) {
      rd->nres = 0;
      lj_ir_call(J, IRCALL_lj_tab_clear, tr);
      J->needsnap = 1;
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************
// Debug library fast functions

static void recff_debug_getMetatable(jit_State* J, RecordFFData* rd)
{
   GCtab* mt;
   TRef mtref;
   TRef tr = J->base[0];
   if (tref_istab(tr)) {
      mt = tabref(tabV(&rd->argv[0])->metatable);
      mtref = emitir(IRT(IR_FLOAD, IRT_TAB), tr, IRFL_TAB_META);
   }
   else if (tref_isudata(tr)) {
      mt = tabref(udataV(&rd->argv[0])->metatable);
      mtref = emitir(IRT(IR_FLOAD, IRT_TAB), tr, IRFL_UDATA_META);
   }
   else {
      mt = tabref(basemt_obj(J2G(J), &rd->argv[0]));
      J->base[0] = mt ? lj_ir_ktab(J, mt) : TREF_NIL;
      return;
   }
   emitir(IRTG(mt ? IR_NE : IR_EQ, IRT_TAB), mtref, lj_ir_knull(J, IRT_TAB));
   J->base[0] = mt ? mtref : TREF_NIL;
}

//********************************************************************************************************************
// Record calls to fast functions

#include "lj_recdef.h"

static uint32_t recdef_lookup(GCfunc* fn)
{
   if (fn->c.ffid < sizeof(recff_idmap) / sizeof(recff_idmap[0])) return recff_idmap[fn->c.ffid];
   else return 0;
}

// Record entry to a fast function or C function.
void lj_ffrecord_func(jit_State* J)
{
   RecordFFData rd;
   uint32_t m = recdef_lookup(J->fn);
   rd.data = m & 0xff;
   rd.nres = 1;  //  Default is one result.
   rd.argv = J->L->base;
   J->base[J->maxslot] = 0;  //  Mark end of arguments.
   (recff_func[m >> 8])(J, &rd);  //  Call recff_* handler.
   if (rd.nres >= 0) {
      if (J->postproc == LJ_POST_NONE) J->postproc = LJ_POST_FFRETRY;
      lj_record_ret(J, 0, rd.nres);
   }
}

#undef IR
#undef emitir
