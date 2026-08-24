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
// The result count defaults to 1. Handlers that return a different number of results need to override it. Handlers
// that stop recording or leave a call pending set the recording disposition instead.
//
// Results need to be stored starting at J->base[0]. Return processing moves them to the right slots later.
//
// The per-ffid auxiliary data is the value of the 2nd part of the LJLIB_REC() annotation. This allows handling
// similar functionality in a common handler.

#define lj_ffrecord_c
#define LUA_CORE

#include <bit>
#include <cstring>

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
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
#include "runtime/lj_array.h"
#include "runtime/lj_proto_registry.h"
#include "jit/frame_manager.h"
#include "lj_strfmt.h"
#include "lj_serialize.h"
#include "lib_range.h"

// Some local macros to save typing. Undef'd at the end.
#define IR(ref)         (&J->cur.ir[(ref)])

// Pass IR on to next optimization in chain (FOLD).
#define emitir(ot, a, b)   (lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

// Type of handler to record a fast function.
typedef void (* RecordFunc)(jit_State* J, RecordFFData* rd);

static uint64_t low_bit_mask(int32_t Count)
{
   if (Count <= 0) return 0;
   if (Count >= 64) return ~uint64_t(0);
   return (uint64_t(1) << Count) - 1;
}

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
         // Stitched trace cannot start with an op that consumes MULTRES, because recff_stitch() rewrites the frame
         // and the continuation re-enters at the caller's next instruction with MULTRES no longer valid.
         if (!(op IS BC_CALLM or op IS BC_CALLMT or
            op IS BC_RETM or op IS BC_TSETM)) {
            switch (J->fn->c.ffid) {
               case FF_error:
               case FF_debug_setHook:
               case FF_jit_flush:
                  break;  //  Don't stitch across special builtins.
               default:
                  recff_stitch(J);  //  Use trace stitching.
                  rd->disposition = RecordFFDisposition::Stopped;
                  return;
            }
         }
      }
      // Otherwise stop trace and return to interpreter.
      lj_record_stop(J, TraceLink::RETURN, 0);
      rd->disposition = RecordFFDisposition::Stopped;
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

   if ((call_info->flags & CCI_T) and J->trydepth > 0) lj_record_try_materialise(J);

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
   rd->result_count = 0;  // Returns no values (void).
}

//********************************************************************************************************************

constexpr uint32_t TYPE_NAME_USERDATA = 3;
constexpr uint32_t TYPE_NAME_RANGE = 15;

static uint32_t recff_type_name_index(TiriType Type)
{
   switch (Type) {
      case TiriType::Nil:      return 0;
      case TiriType::Bool:     return 2;
      case TiriType::Str:      return 4;
      case TiriType::Struct:   return 6;
      case TiriType::Func:     return 8;
      case TiriType::Object:   return 10;
      case TiriType::Table:    return 11;
      case TiriType::Array:    return 13;
      case TiriType::Num:      return 14;
      case TiriType::Range:    return TYPE_NAME_RANGE;
      case TiriType::Userdata: return TYPE_NAME_USERDATA;
      case TiriType::Any:
      case TiriType::Unknown:  return TYPE_NAME_USERDATA;
   }
   return TYPE_NAME_USERDATA;
}

static bool recff_is_range_userdata(jit_State *J, GCudata *Userdata)
{
   return is_range_userdata(J->L, Userdata);
}

static void recff_type(jit_State* J, RecordFFData* rd)
{
   uint32_t t;
   if (tvisnumber(&rd->argv[0])) t = ~LJ_TNUMX;
   else t = ~itype(&rd->argv[0]);

   if (t IS ~LJ_TUDATA) {
      GCudata *ud = udataV(&rd->argv[0]);
      if (recff_is_range_userdata(J, ud)) {
         J->base[0] = lj_ir_kstr(J, strV(&J->fn->c.upvalue[TYPE_NAME_RANGE]));
         return;
      }
      else if (ud->udtype IS UDTYPE_THUNK) {
         ThunkPayload *payload = thunk_payload(ud);
         if (payload->expected_type != 0xFF) {
            t = recff_type_name_index(TiriType(payload->expected_type));
            J->base[0] = lj_ir_kstr(J, strV(&J->fn->c.upvalue[t]));
            return;
         }
      }

      // Metatables on special userdata are treated as immutable by the generic recorder. Leave their display-name
      // lookup to the interpreter, while rawtype() remains recordable for these containers.
      if (ud->udtype != UDTYPE_USERDATA) lj_trace_err(J, LJ_TRERR_NYIFFU);
   }

   // GCobject and GCstruct have per-instance metatables that are not represented by the generic metamethod recorder.
   // Abort this fast-function recording until those IR field loads are available rather than folding an unsafe name.
   if (tvisobject(&rd->argv[0]) or tvisstruct(&rd->argv[0])) lj_trace_err(J, LJ_TRERR_NYIFFU);

   RecordIndex ix{};
   ix.tab = J->base[0];
   copyTV(J->L, &ix.tabv, &rd->argv[0]);
   bool has_name = lj_record_mm_lookup(J, &ix, MM_name);

   // lj_meta_lookup() falls back to the array base metatable when an instance has none, whereas the generic recorder
   // stops after guarding the per-instance metatable as nil. Mirror the interpreter's fallback for type().

   if (not has_name and tvisarray(&rd->argv[0]) and not tabref(arrayV(&rd->argv[0])->metatable)) {
      GCtab *base_mt = tabref(basemt_it(J2G(J), LJ_TARRAY));
      if (base_mt) {
         // Load the base table from gcroot and record its raw __name access so later table mutations leave the trace
         // through the lookup guards instead of preserving a stale folded result.

         GCstr *name_key = mmname_str(J2G(J), MM_name);
         cTValue *observed = lj_tab_getstr(base_mt, name_key);
         int base_mt_offset = GG_OFS(g.gcroot) + int((GCROOT_BASEMT + ~LJ_TARRAY) * sizeof(GCRef));
         ix.tab = lj_ir_ggfload(J, IRT_TAB, base_mt_offset);
         settabV(J->L, &ix.tabv, base_mt);
         ix.key = lj_ir_kstr(J, name_key);
         setstrV(J->L, &ix.keyv, name_key);
         ix.val = 0;
         ix.idxchain = 0;
         ix.mobj = lj_record_idx(J, &ix);
         if (observed and not tvisnil(observed)) copyTV(J->L, &ix.mobjv, observed);
         has_name = not tref_isnil(ix.mobj);
      }
   }

   if (has_name) {
      cTValue *observed = &ix.mobjv;
      if (tvisstr(observed)) {
         GCstr *name = strV(observed);
         TRef name_ref = lj_ir_kstr(J, name);
         emitir(IRTG(IR_EQ, IRT_STR), ix.mobj, name_ref);
         if (name->len > 0) {
            J->base[0] = name_ref;
            return;
         }
      }
   }

   J->base[0] = lj_ir_kstr(J, strV(&J->fn->c.upvalue[t]));
}

//********************************************************************************************************************

static void recff_rawtype(jit_State* J, RecordFFData* Data)
{
   cTValue *value = &Data->argv[0];
   TRef value_ref = J->base[0];

   if (tvisarray(value)) {
      GCarray *array = arrayV(value);
      IRBuilder ir(J);
      TRef element_type_ref = ir.fload(value_ref, IRFL_ARRAY_ELEMTYPE, IRT_U8);
      ir.guard_eq_int(element_type_ref, ir.kint(int32_t(array->elemtype)));
      if (gcref(array->type_identity)) {
         GCstr *identity = strref(array->type_identity);
         TRef identity_ref = ir.fload(value_ref, IRFL_ARRAY_IDENTITY, IRT_STR);
         ir.guard_eq(identity_ref, ir.kstr(identity), IRT_STR);
      }
      else if (array->elemtype IS AET::STRUCT) {
         TRef definition_ref = ir.fload(value_ref, IRFL_ARRAY_STRUCTDEF, IRT_PTR);
         TRef definition = array->structdef ? ir.kkptr(array->structdef) : ir.knull(IRT_PTR);
         ir.guard_eq(definition_ref, definition, IRT_PTR);
      }
   }
   else if (tvisobject(value)) {
      GCobject *object = objectV(value);
      IRBuilder ir(J);
      TRef class_ref = ir.fload(value_ref, IRFL_OBJ_CLASSPTR, IRT_PTR);
      TRef class_pointer = object->classptr ? ir.kkptr(object->classptr) : ir.knull(IRT_PTR);
      ir.guard_eq(class_ref, class_pointer, IRT_PTR);
   }
   else if (tvisstruct(value)) {
      GCstruct *structure = structV(value);
      IRBuilder ir(J);
      TRef definition_ref = ir.fload(value_ref, IRFL_STRUCT_DEF, IRT_PTR);
      TRef definition = structure->def ? ir.kkptr(structure->def) : ir.knull(IRT_PTR);
      ir.guard_eq(definition_ref, definition, IRT_PTR);
   }
   else if (tvisudata(value)) {
      GCudata *userdata = udataV(value);
      IRBuilder ir(J);
      TRef userdata_type = ir.fload(value_ref, IRFL_UDATA_UDTYPE, IRT_U8);
      if (userdata->udtype IS UDTYPE_THUNK) {
         ir.guard_eq_int(userdata_type, ir.kint(UDTYPE_THUNK));
      }
      else {
         ir.guard_ne_int(userdata_type, ir.kint(UDTYPE_THUNK));
         TRef range_metatable = lj_record_range_metatable(J);
         // The debug registry is mutable, so its guarded lookup can return nil or another non-table value.
         if (tref_istab(range_metatable)) {
            TRef metatable_ref = ir.fload_tab(value_ref, IRFL_UDATA_META);
            if (recff_is_range_userdata(J, userdata)) ir.guard_eq(metatable_ref, range_metatable, IRT_TAB);
            else ir.guard_ne(metatable_ref, range_metatable, IRT_TAB);
         }
      }
   }
   else {
      uint32_t type_index = tvisnumber(value) ? ~LJ_TNUMX : ~itype(value);
      J->base[0] = lj_ir_kstr(J, strV(&J->fn->c.upvalue[type_index]));
      return;
   }

   GCstr *name = lj_meta_raw_type_name(J->L, value);
   J->base[0] = lj_ir_kstr(J, name);
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
      ix.val_slot = 2;
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
// Record the common bare-table preparation path.  Metamethod-bearing and dynamic targets remain interpreted.

static void recff___tiri_iter_prepare(jit_State *J, RecordFFData *Data)
{
   TRef target = J->base[0];
   if (J->maxslot != 1 or not tref_istab(target)) {
      recff_nyiu(J, Data);
      return;
   }

   RecordIndex index;
   index.tab = target;
   copyTV(J->L, &index.tabv, &Data->argv[0]);
   if (lj_record_mm_lookup(J, &index, MM_iter) or lj_record_mm_lookup(J, &index, MM_pairs)) {
      recff_nyiu(J, Data);
      return;
   }

   GCtab *environment = tabref(J->fn->c.env);
   cTValue *next = lj_tab_getstr(environment, lj_str_newlit(J->L, "next"));
   if (not tvisfunc(next)) {
      recff_nyiu(J, Data);
      return;
   }

   J->base[0] = lj_ir_kfunc(J, funcV(next));
   J->base[1] = target;
   J->base[2] = TREF_NIL;
   Data->result_count = 3;
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
   int32_t masked_count = count;
   ptrdiff_t trailing_start = count;
   if (masked_count < 0) masked_count = 0;
   if (masked_count > value_count) masked_count = int32_t(value_count);
   if (masked_count > 64) masked_count = 64;
   if (trailing_start < 0) trailing_start = 0;
   if (trailing_start > value_count) trailing_start = value_count;
   uint64_t active_mask = mask & low_bit_mask(masked_count);

   // Build output by copying kept values
   int32_t out_idx = 0;
   uint64_t pending_mask = active_mask;
   while (pending_mask) {
      ptrdiff_t i = ptrdiff_t(std::countr_zero(pending_mask));
      TRef tr = J->base[value_start + i];
      if (tr) {
         J->base[out_idx] = tr;
         out_idx++;
      }
      pending_mask &= pending_mask - 1;
   }
   if (trailing_keep) {
      for (ptrdiff_t i = trailing_start; i < value_count; i++) {
         TRef tr = J->base[value_start + i];
         if (tr) {
            J->base[out_idx] = tr;
            out_idx++;
         }
      }
   }

   rd->result_count = out_idx;
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

struct RecordMetamethodTailCall {
   jit_State *state;
   TRef receiver;
   ptrdiff_t argument_count;
};

static TValue* recff_metacall_cp(lua_State* L, lua_CFunction dummy, void* ud)
{
   RecordMetamethodTailCall *call = (RecordMetamethodTailCall*)ud;
   lj_record_metamethod_tailcall(call->state, 0, call->argument_count, call->receiver);
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
      bool table_receiver = tref_istab(ix.tab);
      RecordMetamethodTailCall call { J, ix.tab, table_receiver ? 0 : 1 };
      // Temporarily insert metamethod below object.
      if (not table_receiver) J->base[1 + LJ_FR2] = J->base[0];
      J->base[0] = ix.mobj;
      copyTV(J->L, &argv0, &rd->argv[0]);
      if (not table_receiver) copyTV(J->L, &rd->argv[1 + LJ_FR2], &rd->argv[0]);
      copyTV(J->L, &rd->argv[0], &ix.mobjv);
      // Need to protect lj_record_tailcall because it may throw.
      errcode = lj_vm_cpcall(J->L, nullptr, &call, recff_metacall_cp);
      // Always undo Lua stack changes to avoid confusing the interpreter.
      copyTV(J->L, &rd->argv[0], &argv0);
      if (errcode)
         lj_err_throw(J->L, errcode);  //  Propagate errors.
      rd->disposition = RecordFFDisposition::PendingCall;
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

static bool array_elemtype_from_string(GCstr *TypeStr, AET *Result)
{
   CSTRING type_name = strdata(TypeStr);

   if (TypeStr->len IS 3 and memcmp(type_name, "int", 3) IS 0) *Result = AET::INT32;
   else if (TypeStr->len IS 4 and memcmp(type_name, "byte", 4) IS 0) *Result = AET::BYTE;
   else if (TypeStr->len IS 4 and memcmp(type_name, "char", 4) IS 0) *Result = AET::BYTE;
   else if (TypeStr->len IS 4 and memcmp(type_name, "int8", 4) IS 0) *Result = AET::INT8;
   else if (TypeStr->len IS 5 and memcmp(type_name, "int16", 5) IS 0) *Result = AET::INT16;
   else if (TypeStr->len IS 5 and memcmp(type_name, "int64", 5) IS 0) *Result = AET::INT64;
   else if (TypeStr->len IS 5 and memcmp(type_name, "uint8", 5) IS 0) *Result = AET::UINT8;
   else if (TypeStr->len IS 6 and memcmp(type_name, "uint16", 6) IS 0) *Result = AET::UINT16;
   else if (TypeStr->len IS 4 and memcmp(type_name, "uint", 4) IS 0) *Result = AET::UINT32;
   else if (TypeStr->len IS 6 and memcmp(type_name, "uint64", 6) IS 0) *Result = AET::UINT64;
   else if (TypeStr->len IS 5 and memcmp(type_name, "float", 5) IS 0) *Result = AET::FLOAT;
   else if (TypeStr->len IS 6 and memcmp(type_name, "double", 6) IS 0) *Result = AET::DOUBLE;
   else if (TypeStr->len IS 6 and memcmp(type_name, "string", 6) IS 0) *Result = AET::STR_GC;
   else if (TypeStr->len IS 6 and memcmp(type_name, "struct", 6) IS 0) *Result = AET::STRUCT;
   else if (TypeStr->len IS 7 and memcmp(type_name, "pointer", 7) IS 0) *Result = AET::PTR;
   // Source syntax uses "obj", but array:type() returns the canonical runtime name "object".  Accept both so that
   // array.new(Size, Existing:type()) records consistently with interpreted execution.
   else if (TypeStr->len IS 3 and memcmp(type_name, "obj", 3) IS 0) *Result = AET::OBJECT;
   else if (TypeStr->len IS 6 and memcmp(type_name, "object", 6) IS 0) *Result = AET::OBJECT;
   else if (TypeStr->len IS 5 and memcmp(type_name, "table", 5) IS 0) *Result = AET::TABLE;
   else if (TypeStr->len IS 5 and memcmp(type_name, "array", 5) IS 0) *Result = AET::ARRAY;
   else if (TypeStr->len IS 3 and memcmp(type_name, "any", 3) IS 0) *Result = AET::ANY;
   else return false;

   return true;
}

//********************************************************************************************************************
// Record array.new(size, literal_type).  String-initialised byte arrays are left for the interpreter for now.

static void recff_array_new(jit_State* J, RecordFFData* rd)
{
   TRef size_ref = J->base[0];
   TRef type_ref = size_ref ? J->base[1] : 0;

   if (!type_ref) {
      recff_nyi(J, rd);
      return;
   }

   if (not tvisnumber(&rd->argv[0])) {
      recff_nyi(J, rd);
      return;
   }

   if (not tvisstr(&rd->argv[1]) or not tref_isk(type_ref)) {
      recff_nyi(J, rd);
      return;
   }

   AET elem_type;
   if (not array_elemtype_from_string(strV(&rd->argv[1]), &elem_type) or elem_type IS AET::PTR or
       elem_type IS AET::STRUCT) {
      recff_nyi(J, rd);
      return;
   }

   size_ref = lj_opt_narrow_toint(J, size_ref);
   emitir(IRTGI(IR_GE), size_ref, lj_ir_kint(J, 0));

   J->base[0] = recff_ir_call_fixed(J, IRCALL_lj_arr_new_jit, size_ref, lj_ir_kint(J, int32_t(elem_type)),
      TREF_NIL, TREF_NIL);
}

//********************************************************************************************************************
// Record array.clear(receiver).

static void recff_array_clear(jit_State* J, RecordFFData* rd)
{
   TRef array_ref = J->base[0];
   if (tref_isarray(array_ref)) {
      recff_ir_call_fixed(J, IRCALL_lj_arr_clear, array_ref, TREF_NIL, TREF_NIL, TREF_NIL);
      J->base[0] = 0;
      rd->result_count = 0;
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************
// Record array.resize(receiver, new_size).

static void recff_array_resize(jit_State* J, RecordFFData* rd)
{
   TRef array_ref = J->base[0];
   TRef size_ref = array_ref ? J->base[1] : 0;

   if (tref_isarray(array_ref) and size_ref) {
      if (not tvisnumber(&rd->argv[1])) {
         recff_nyi(J, rd);
         return;
      }

      size_ref = lj_opt_narrow_toint(J, size_ref);
      J->base[0] = recff_ir_call_fixed(J, IRCALL_lj_arr_resize, array_ref, size_ref, TREF_NIL, TREF_NIL);
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************
// Record array.getString(receiver, start, len).

static void recff_array_getString(jit_State* J, RecordFFData* rd)
{
   TRef array_ref = J->base[0];
   if (not tref_isarray(array_ref)) return;  // Interpreter will throw.

   GCarray *arr = arrayV(&rd->argv[0]);
   if (not (arr->elemtype IS AET::BYTE)) {
      recff_nyi(J, rd);
      return;
   }

   TRef elem_type_ref = emitir(IRT(IR_FLOAD, IRT_U8), array_ref, IRFL_ARRAY_ELEMTYPE);
   emitir(IRTGI(IR_EQ), elem_type_ref, lj_ir_kint(J, int32_t(AET::BYTE)));

   TRef start_ref = lj_ir_kint(J, 0);
   if (J->base[1] and not tref_isnil(J->base[1])) {
      if (not tvisnumber(&rd->argv[1])) {
         recff_nyi(J, rd);
         return;
      }
      start_ref = lj_opt_narrow_toint(J, J->base[1]);
   }

   TRef len_ref = lj_ir_kint(J, -1);
   if (J->base[2] and not tref_isnil(J->base[2])) {
      if (not tvisnumber(&rd->argv[2])) {
         recff_nyi(J, rd);
         return;
      }
      len_ref = lj_opt_narrow_toint(J, J->base[2]);
      emitir(IRTGI(IR_GE), len_ref, lj_ir_kint(J, 0));
   }

   J->base[0] = recff_ir_call_fixed(J, IRCALL_lj_arr_getstring, array_ref, start_ref, len_ref, TREF_NIL);
}

//********************************************************************************************************************

static bool array_push_recordable(GCarray *Array, cTValue *Val)
{
   if (Array->elemtype IS AET::BYTE and tvisstr(Val)) return true;
   return lj_array_validate_element(Array, Val) IS ArrayElementResult::OK;
}

static int recff_arg_count(jit_State *J)
{
   return int(J->L->top - J->L->base);
}

static TRef recff_concat_tostr(jit_State *J, TRef Value)
{
   if (tref_isnumber(Value)) {
      return emitir(IRT(IR_TOSTR, IRT_STR), Value, tref_isnum(Value) ? IRTOSTR_NUM : IRTOSTR_INT);
   }
   return Value;
}

static bool recff_is_number_str_args(jit_State *J, int Start, int Stop)
{
   for (int i = Start; i < Stop; i++) {
      if (not tref_isnumber_str(J->base[i])) return false;
   }
   return true;
}

//********************************************************************************************************************
// Record array.push(receiver, value).  Multi-value push falls back to the interpreter.

static void recff_array_push(jit_State* J, RecordFFData* rd)
{
   TRef array_ref = J->base[0];
   TRef val_ref = array_ref ? J->base[1] : 0;
   if (not tref_isarray(array_ref) or not val_ref or J->base[2]) {
      recff_nyi(J, rd);
      return;
   }

   GCarray *arr = arrayV(&rd->argv[0]);
   cTValue *val = &rd->argv[1];
   if (not array_push_recordable(arr, val)) {
      recff_nyi(J, rd);
      return;
   }

   TRef elem_type_ref = emitir(IRT(IR_FLOAD, IRT_U8), array_ref, IRFL_ARRAY_ELEMTYPE);
   emitir(IRTGI(IR_EQ), elem_type_ref, lj_ir_kint(J, int32_t(arr->elemtype)));

   TRef tmp_ref = recff_tmpref(J, val_ref, IRTMPREF_IN1);
   recff_ir_call_fixed(J, IRCALL_lj_arr_push1, array_ref, tmp_ref, TREF_NIL, TREF_NIL);
   J->base[0] = emitir(IRTI(IR_FLOAD), array_ref, IRFL_ARRAY_LEN);
}

//********************************************************************************************************************
// Record array.append(receiver, value, ...) - the helper behind compound concatenation (..=).

static bool recff_byte_array_append_piece_recordable(cTValue *Value, TRef Piece)
{
   return (tvisstr(Value) and tref_isstr(Piece)) or (tvisnumber(Value) and tref_isnumber(Piece));
}

static void recff_byte_array_append_piece(jit_State *J, TRef ArrayRef, cTValue *Value, TRef Piece)
{
   if (tvisstr(Value) and tref_isstr(Piece)) {
      recff_ir_call_fixed(J, IRCALL_lj_arr_putstr, ArrayRef, Piece, TREF_NIL, TREF_NIL);
   }
   else {
      TRef tmp_ref = recff_tmpref(J, Piece, IRTMPREF_IN1);
      recff_ir_call_fixed(J, IRCALL_lj_arr_putnumtv, ArrayRef, tmp_ref, TREF_NIL, TREF_NIL);
   }
}

static void recff_array_append(jit_State* J, RecordFFData* rd)
{
   TRef left = J->base[0];
   int arg_count = left ? recff_arg_count(J) : 0;
   if (arg_count < 2) {
      recff_nyi(J, rd);
      return;
   }

   if (tref_isarray(left)) {
      GCarray *arr = arrayV(&rd->argv[0]);

      // Guard the element type so the trace only runs for arrays matching the recorded semantics.
      TRef et_ref = emitir(IRT(IR_FLOAD, IRT_U8), left, IRFL_ARRAY_ELEMTYPE);
      emitir(IRTGI(IR_EQ), et_ref, lj_ir_kint(J, int32_t(arr->elemtype)));

      if (arr->elemtype IS AET::BYTE) {
         for (int i = 1; i < arg_count; i++) {
            cTValue *val = &rd->argv[i];
            TRef piece = J->base[i];
            if (not recff_byte_array_append_piece_recordable(val, piece)) {
               recff_nyi(J, rd);
               return;
            }
         }

         if (arg_count IS 2) {
            cTValue *val = &rd->argv[1];
            TRef piece = J->base[1];
            recff_byte_array_append_piece(J, left, val, piece);
         }
         else {
            TRef buf_ref = recff_bufhdr(J);
            for (int i = 1; i < arg_count; i++) {
               buf_ref = emitir(IRTG(IR_BUFPUT, IRT_PGC), buf_ref, recff_concat_tostr(J, J->base[i]));
            }
            recff_ir_call_fixed(J, IRCALL_lj_arr_putsbuf, left, buf_ref, TREF_NIL, TREF_NIL);
         }
      }
      else {
         if (arg_count != 2 or not array_push_recordable(arr, &rd->argv[1])) {
            recff_nyi(J, rd);
            return;
         }

         TRef tmp_ref = recff_tmpref(J, J->base[1], IRTMPREF_IN1);
         recff_ir_call_fixed(J, IRCALL_lj_arr_push1, left, tmp_ref, TREF_NIL, TREF_NIL);
      }

      J->base[0] = left;  // append() returns the receiver array.
   }
   else if (tref_isnumber_str(left) and recff_is_number_str_args(J, 1, arg_count)) {
      TRef hdr = recff_bufhdr(J);
      TRef rhs = recff_concat_tostr(J, J->base[1]);

      if (arg_count > 2) {
         TRef rhs_tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), hdr, rhs);
         for (int i = 2; i < arg_count; i++) {
            rhs_tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), rhs_tr, recff_concat_tostr(J, J->base[i]));
         }
         rhs = emitir(IRTG(IR_BUFSTR, IRT_STR), rhs_tr, hdr);
         hdr = recff_bufhdr(J);
      }

      TRef tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), hdr, recff_concat_tostr(J, left));
      tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr, rhs);
      J->base[0] = emitir(IRTG(IR_BUFSTR, IRT_STR), tr, hdr);
   }
   else {
      recff_nyi(J, rd);  // Tables and objects with __concat metamethods fall back to the interpreter.
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
      rd->result_count = tref_isnil(J->base[1]) ? 0 : 2;
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
         rd->result_count = 0;
         return;
      }

      emitir(IRTGI(IR_ULT), idx_ref, len_ref);
      J->base[0] = idx_ref;

      J->base[1] = lj_record_array_iter_load(J, ix.tab, idx_ref, arr, idx_int);
      rd->result_count = 2;
   }  // else: Interpreter will throw.
}

//********************************************************************************************************************

static TRef recff_range_state_load(jit_State *J, RecordFFData *rd, int32_t Index)
{
   RecordIndex ix;
   ix.tab = J->base[0];
   ix.key = lj_ir_kint(J, Index);
   ix.val = 0;
   ix.idxchain = 0;
   settabV(J->L, &ix.tabv, tabV(&rd->argv[0]));
   setintV(&ix.keyv, Index);
   return lj_record_idx(J, &ix);
}

static void recff_range_iterator_next(jit_State *J, RecordFFData *rd)
{
   if (not tref_istab(J->base[0]) or not J->base[1] or not tvistab(&rd->argv[0])) {
      lj_trace_err(J, LJ_TRERR_BADTYPE);
   }

   GCtab *state = tabV(&rd->argv[0]);
   cTValue *runtime_count_value = lj_tab_getint(state, RANGE_ITERATOR_COUNT);
   cTValue *runtime_integer_value = lj_tab_getint(state, RANGE_ITERATOR_INTEGER_VALUES);
   if (not tvisnumber(runtime_count_value) or not tvisnumber(runtime_integer_value)) {
      lj_trace_err(J, LJ_TRERR_BADTYPE);
   }

   TRef start = lj_ir_tonum(J, recff_range_state_load(J, rd, RANGE_ITERATOR_START));
   TRef step = lj_ir_tonum(J, recff_range_state_load(J, rd, RANGE_ITERATOR_STEP));
   TRef count = lj_ir_tonum(J, recff_range_state_load(J, rd, RANGE_ITERATOR_COUNT));
   TRef integer_values = lj_ir_tonum(J, recff_range_state_load(J, rd, RANGE_ITERATOR_INTEGER_VALUES));

   TRef ordinal;
   lua_Number runtime_ordinal;
   if (tvisnil(&rd->argv[1])) {
      ordinal = lj_ir_knum_zero(J);
      runtime_ordinal = 0.0;
   }
   else {
      if (not tvisnumber(&rd->argv[1])) lj_trace_err(J, LJ_TRERR_BADTYPE);
      TRef control = lj_ir_tonum(J, J->base[1]);
      ordinal = lj_ir_call(J, IRCALL_lj_range_iterator_next_ordinal, control, start, step);
      runtime_ordinal = lj_range_iterator_next_ordinal(
         numberVnum(&rd->argv[1]), numberVnum(lj_tab_getint(state, RANGE_ITERATOR_START)),
         numberVnum(lj_tab_getint(state, RANGE_ITERATOR_STEP)));
   }

   bool in_bounds = runtime_ordinal < numberVnum(runtime_count_value);
   emitir(IRTG(in_bounds ? IR_LT : IR_GE, IRT_NUM), ordinal, count);
   if (not in_bounds) {
      rd->result_count = 0;
      return;
   }

   int32_t integer_result = numberVnum(runtime_integer_value) != 0.0;
   emitir(IRTG(IR_EQ, IRT_NUM), integer_values, lj_ir_knum(J, lua_Number(integer_result)));

   TRef result;
   if (integer_result) {
      result = emitir(IRTN(IR_ADD), start, emitir(IRTN(IR_MUL), ordinal, step));
      result = emitir(IRTGI(IR_CONV), result, IRCONV_INT_NUM | IRCONV_CHECK);
   }
   else {
      result = lj_ir_call(J, IRCALL_lj_range_value, ordinal, start, step);
   }
   J->base[0] = result;
   rd->result_count = 1;
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
         rd->result_count = 3;
      }
      else if (tref_isarray(tr)) {
         J->base[0] = lj_ir_kfunc(J, funcV(&J->fn->c.upvalue[0]));
         J->base[1] = tr;
         J->base[2] = rd->data ? lj_ir_kint(J, -1) : TREF_NIL;  // 0-based: ipairs starts at -1
         rd->result_count = 3;
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
      rd->result_count = lj_record_next(J, &ix);
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
         rd->result_count = 1;
         return;
      }

      emitir(IRTGI(IR_ULT), idx_ref, len_ref);
      J->base[0] = idx_ref;

      J->base[1] = lj_record_array_iter_load(J, tab, idx_ref, arr, idx_int);
      rd->result_count = 2;
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
         if (n IS (lua_Number)lj_num2int(n))
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

static void recff_math_clamp(jit_State* J, RecordFFData* rd)
{
   TRef tr = lj_ir_tonumber(J, J->base[0]);
   TRef lower = lj_ir_tonumber(J, J->base[1]);
   TRef upper = lj_ir_tonumber(J, J->base[2]);
   IRType t = IRT_INT;

   if (!(tref_isinteger(tr) and tref_isinteger(lower) and tref_isinteger(upper))) {
      if (tref_isinteger(tr)) tr = emitir(IRTN(IR_CONV), tr, IRCONV_NUM_INT);
      if (tref_isinteger(lower)) lower = emitir(IRTN(IR_CONV), lower, IRCONV_NUM_INT);
      if (tref_isinteger(upper)) upper = emitir(IRTN(IR_CONV), upper, IRCONV_NUM_INT);
      t = IRT_NUM;
   }

   tr = emitir(IRT(IR_MAX, t), tr, lower);
   tr = emitir(IRT(IR_MIN, t), tr, upper);
   J->base[0] = tr;
   UNUSED(rd);
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
      else {  // d = floor(d*r1)
         tr = emitir(IRTN(IR_MUL), tr, tr1);
         tr = emitir(IRTN(IR_FPMATH), tr, IRFPM_FLOOR);
      }
   }
   J->base[0] = tr;
   UNUSED(rd);
}

//********************************************************************************************************************
// Bit library fast functions

// Bitwise operations do not coerce strings to numbers (mirroring Tiri arithmetic).  A string operand
// aborts the trace so execution falls back to the interpreter, which raises the type error.

static void recff_bit_checknumber(jit_State* J, BCREG arg)
{
   if (tref_isstr(J->base[arg])) lj_trace_err(J, LJ_TRERR_BADTYPE);
}

// Record bit.tobit.

static void recff_bit_tobit(jit_State* J, RecordFFData* rd)
{
   recff_bit_checknumber(J, 0);
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
   recff_bit_checknumber(J, 0);
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
      recff_bit_checknumber(J, 0);
      TRef tr = lj_opt_narrow_tobit(J, J->base[0]);
      uint32_t ot = IRTI(rd->data);
      BCREG i;
      for (i = 1; J->base[i] != 0; i++) {
         recff_bit_checknumber(J, i);
         tr = emitir(ot, tr, lj_opt_narrow_tobit(J, J->base[i]));
      }
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
      recff_bit_checknumber(J, 0);
      recff_bit_checknumber(J, 1);
      TRef tr = lj_opt_narrow_tobit(J, J->base[0]);
      TRef tsh = lj_opt_narrow_tobit(J, J->base[1]);
      IROp op = (IROp)rd->data;
      if (!(op < IR_BROL ? LJ_TARGET_MASKSHIFT : LJ_TARGET_MASKROT) and
         !tref_isk(tsh))
         tsh = emitir(IRTI(IR_BAND), tsh, lj_ir_kint(J, 31));
#ifdef LJ_TARGET_UNIFYROT
      if (op IS (LJ_TARGET_UNIFYROT IS 1 ? IR_BROR : IR_BROL)) {
         op = LJ_TARGET_UNIFYROT IS 1 ? IR_BROL : IR_BROR;
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
// Handle string.byte (rd->data = 0) and string.sub/string.substr (rd->data = 1).

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
   if (rd->data) {  // string.sub(str, start [,stop]) - stop is exclusive
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
         if (end IS 0) {
            J->base[0] = lj_ir_kstr(J, &J2G(J)->strempty);
            return;
         }
         if (end IS INT32_MIN) {
            emitir(IRTGI(IR_EQ), trend, lj_ir_kint(J, INT32_MIN));
            J->base[0] = lj_ir_kstr(J, &J2G(J)->strempty);
            return;
         }
         // The recorder uses an inclusive endpoint internally.  Convert every non-zero exclusive stop.
         end--;
         trend = emitir(IRTI(IR_ADD), trend, lj_ir_kint(J, -1));
      }
   }
   else {  // string.byte(str, [,start [,stop]])
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
         if (end IS 0) {
            rd->result_count = 0;
            return;
         }
         if (end IS INT32_MIN) {
            emitir(IRTGI(IR_EQ), trend, lj_ir_kint(J, INT32_MIN));
            rd->result_count = 0;
            return;
         }
         end--;
         trend = emitir(IRTI(IR_ADD), trend, lj_ir_kint(J, -1));
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
      // The recorder uses an inclusive endpoint internally, whose maximum valid value is len-1.
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
   if (rd->data) {  // Return string.sub/string.substr result.
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
         rd->result_count = count;
         for (i = 0; i < count; i++) {
            TRef tmp = emitir(IRTI(IR_ADD), trstart, lj_ir_kint(J, (int32_t)i));
            tmp = emitir(IRT(IR_STRREF, IRT_PGC), trstr, tmp);
            J->base[i] = emitir(IRT(IR_XLOAD, IRT_U8), tmp, IRXLOAD_READONLY);
         }
      }
      else {  // Empty range or range underflow: return no results.
         emitir(IRTGI(IR_LE), trend, trstart);
         rd->result_count = 0;
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
   else if (i IS 0) J->base[0] = lj_ir_kstr(J, &J2G(J)->strempty);
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
         // 0-based: return a half-open span.
         J->base[0] = pos;
         J->base[1] = emitir(IRTI(IR_ADD), pos, trplen);
         rd->result_count = 2;
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
      TRef tra = sf IS STRFMT_LIT ? 0 : J->base[++arg];
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
         if (sf IS STRFMT_INT) {  // Shortcut for plain %d.
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
         if (sf IS STRFMT_STR)  //  Shortcut for plain %s.
            tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr, tra);
         else if ((sf & STRFMT_T_QUOTED)) tr = lj_ir_call(J, IRCALL_lj_strfmt_putquoted, tr, tra);
         else tr = lj_ir_call(J, IRCALL_lj_strfmt_putfstr, tr, trsf, tra);
         break;
      case STRFMT_CHAR:
         tra = lj_opt_narrow_toint(J, tra);
         if (sf IS STRFMT_CHAR)  //  Shortcut for plain %c.
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

// Guard the public sequence-domain contract used by table functions that infer a numerical boundary.  A classified
// table records through the interpreter so the library can raise its detailed diagnostic.  A compatible table gets
// a flags guard so later classification side-exits before any raw IR_ALEN operation is executed.

static bool recff_guard_sequence(jit_State *J, RecordFFData *RecordData, TRef TableRef, GCtab *Table)
{
   if (not lj_tab_is_sequence(Table)) {
      recff_nyiu(J, RecordData);
      return false;
   }

   TRef flags = emitir(IRT(IR_FLOAD, IRT_U8), TableRef, IRFL_TAB_FLAGS);
   TRef classified = emitir(IRTI(IR_BAND), flags, lj_ir_kint(J, TAB_NOT_SEQUENCE));
   emitir(IRTGI(IR_EQ), classified, lj_ir_kint(J, 0));
   return true;
}

//********************************************************************************************************************

static void recff_table_insert(jit_State* J, RecordFFData* rd)
{
   RecordIndex ix;
   ix.tab = J->base[0];
   ix.val = J->base[1];
   rd->result_count = 0;
   if (tref_istab(ix.tab) and ix.val) {
      if (!J->base[2]) {  // Simple push: t[#t] = v (0-based: next index = len)
         GCtab* t = tabV(&rd->argv[0]);
         if (not recff_guard_sequence(J, rd, ix.tab, t)) return;
         TRef trlen = emitir(IRTI(IR_ALEN), ix.tab, TREF_NIL);
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
      const bool explicit_stop = J->base[1] and J->base[2] and not tref_isnil(J->base[3]);
      if (not explicit_stop and not recff_guard_sequence(J, rd, tab, tabV(&rd->argv[0]))) return;
      TRef sep = !tref_isnil(J->base[1]) ? lj_ir_tostr(J, J->base[1]) : lj_ir_knull(J, IRT_STR);
      TRef tri = (J->base[1] and !tref_isnil(J->base[2])) ? lj_opt_narrow_toint(J, J->base[2]) : lj_ir_kint(J, 0);  // 0-based: default start
      TRef tre = explicit_stop ? emitir(IRTI(IR_SUB), lj_opt_narrow_toint(J, J->base[3]), lj_ir_kint(J, 1)) :
         emitir(IRTI(IR_ADD), emitir(IRTI(IR_ALEN), tab, TREF_NIL), lj_ir_kint(J, -1));  // Internal endpoint is inclusive.
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
      if (lj_tab_is_environment(tabV(&rd->argv[0]))) lj_trace_err(J, LJ_TRERR_NYIFFU);
      TRef marker = emitir(IRT(IR_FLOAD, IRT_TAB), tr, IRFL_TAB_GCONTRACTS);
      emitir(IRTG(IR_EQ, IRT_TAB), marker, lj_ir_knull(J, IRT_TAB));
      if (recff_metacall(J, rd, MM_clear)) return;
      rd->result_count = 0;
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
   rd.result_count = 1;
   rd.disposition = RecordFFDisposition::Completed;
   rd.argv = J->L->base;
   J->base[J->maxslot] = 0;  //  Mark end of arguments.
   (recff_func[m >> 8])(J, &rd);  //  Call recff_* handler.
   if (rd.disposition IS RecordFFDisposition::Completed) {
      if (J->postproc IS LJ_POST_NONE) J->postproc = LJ_POST_FFRETRY;
      lj_record_ret(J, 0, rd.result_count);
   }
}

#undef IR
#undef emitir
