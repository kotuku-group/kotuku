// Trace recorder (bytecode -> SSA IR).
// Copyright © 2025-2026 Paul Manias
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_record_c
#define LUA_CORE

#include "lj_obj.h"
#include <kotuku/main.h>

#include "lj_err.h"
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
#include "lj_ffrecord.h"
#include "lj_snap.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_vmarray.h"
#include "runtime/lj_array.h"
#include "lj_prng.h"
#include "jit/frame_manager.h"
#include "runtime/lj_object.h"

// Some local macros to save typing. Undef'd at the end.
#define IR(ref)         (&J->cur.ir[(ref)])

// Pass IR on to next optimisation in chain (FOLD).
#define emitir(ot, a, b)   (lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

// Emit raw IR without passing through optimisations.
#define emitir_raw(ot, a, b)   (lj_ir_set(J, (ot), (a), (b)), lj_ir_emit(J))

//********************************************************************************************************************
// Emit TMPREF.

static TRef rec_tmpref(jit_State *J, TRef tr, int mode)
{
   if (!LJ_DUALNUM and tref_isinteger(tr)) tr = emitir(IRTN(IR_CONV), tr, IRCONV_NUM_INT);
   return emitir(IRT(IR_TMPREF, IRT_PGC), tr, mode);
}

//********************************************************************************************************************
// Record loop ops

// Loop event.
typedef enum {
   LOOPEV_LEAVE,      //  Loop is left or not entered.
   LOOPEV_ENTERLO,   //  Loop is entered with a low iteration count left.
   LOOPEV_ENTER      //  Loop is entered.
} LoopEvent;

// Operand decoding context for bytecode recording.

struct RecordOps {
   TRef ra, rb, rc;       // Decoded operand references
   RecordIndex ix;        // Index structure for table/metamethod operations
   BCIns ins;             // Current bytecode instruction
   BCOp op;               // Current opcode

   // Convenient accessors for value copies (aliases into ix)
   TValue *rav() { return &ix.valv; }
   TValue *rbv() { return &ix.tabv; }
   TValue *rcv() { return &ix.keyv; }
};

//********************************************************************************************************************
// Sanity checks

#ifdef LUA_USE_ASSERT
// Sanity check the whole IR -- sloooow.
static void rec_check_ir(jit_State *J)
{
   IRRef i, nins = J->cur.nins, nk = J->cur.nk;
   lj_assertJ(nk <= REF_BIAS and nins >= REF_BIAS and nins < 65536, "inconsistent IR layout");

   for (i = nk; i < nins; i++) {
      IRIns* ir = IR(i);
      uint32_t mode = lj_ir_mode[ir->o];
      IRRef op1 = ir->op1;
      IRRef op2 = ir->op2;
      const char* err = nullptr;
      switch (irm_op1(mode)) {
         case IRMnone:
            if (op1 != 0) err = "IRMnone op1 used";
            break;
         case IRMref:
            if (op1 < nk or (i >= REF_BIAS ? op1 >= i : op1 <= i)) err = "IRMref op1 out of range";
            break;
         case IRMlit: break;
         case IRMcst:
            if (i >= REF_BIAS) { err = "constant in IR range"; break; }
            if (irt_is64(ir->t) and ir->o != IR_KNULL) i++;
            continue;
      }
      switch (irm_op2(mode)) {
         case IRMnone:
            if (op2) err = "IRMnone op2 used";
            break;
         case IRMref:
            if (op2 < nk or (i >= REF_BIAS ? op2 >= i : op2 <= i))
               err = "IRMref op2 out of range";
            break;
         case IRMlit: break;
         case IRMcst: err = "IRMcst op2"; break;
      }

      if (not err and ir->prev) {
         if (ir->prev < nk or (i >= REF_BIAS ? ir->prev >= i : ir->prev <= i)) err = "chain out of range";
         else if (ir->o != IR_NOP and IR(ir->prev)->o != ir->o) err = "chain to different op";
      }

      lj_assertJ(not err, "bad IR %04d op %d(%04d,%04d): %s", i - REF_BIAS, ir->o,
         irm_op1(mode) IS IRMref ? op1 - REF_BIAS : op1, irm_op2(mode) IS IRMref ? op2 - REF_BIAS : op2, err);
   }
}

//********************************************************************************************************************
// Compare stack slots and frames of the recorder and the VM.

static void rec_check_slots(jit_State *J)
{
   BCREG s, nslots = J->baseslot + J->maxslot;
   int32_t depth = 0;
   cTValue *base = J->L->base - J->baseslot;
   lj_assertJ(J->baseslot >= FRC::MIN_BASESLOT, "bad baseslot");
   lj_assertJ(J->baseslot IS FRC::MIN_BASESLOT or (J->slot[J->baseslot - 1] & TREF_FRAME), "baseslot does not point to frame");
   lj_assertJ(nslots <= LJ_MAX_JSLOTS, "slot overflow");
   for (s = 0; s < nslots; s++) {
      TRef tr = J->slot[s];
      if (tr) {
         cTValue *tv = &base[s];
         IRRef ref = tref_ref(tr);
         IRIns* ir = nullptr;  //  Silence compiler.
         if (ref or !(tr & (TREF_FRAME | TREF_CONT))) {  // LJ_FR2 is always 1
            lj_assertJ(ref >= J->cur.nk and ref < J->cur.nins, "slot %d ref %04d out of range", s, ref - REF_BIAS);
            ir = IR(ref);
            lj_assertJ(irt_t(ir->t) IS tref_t(tr), "slot %d IR type mismatch", s);
         }

         if (s IS 0) lj_assertJ(tref_isfunc(tr), "frame slot 0 is not a function");
         else if (s IS 1) lj_assertJ((tr & ~TREF_FRAME) IS 0, "bad frame slot 1");
         else if ((tr & TREF_FRAME)) {
            // Check if this is a valid frame slot or a stale marker from a returned call.
            // Stale TREF_FRAME can remain in slots after an inlined call returns and the slot
            // is reused by subsequent code. Only validate frame slots that are actually part
            // of the current frame chain (reachable from baseslot-1 via frame_delta).
            GCobj* frame_obj = frame_gc(tv);
            if (frame_obj and frame_obj->gch.gct IS ~LJ_TFUNC) {
               // Valid frame - perform full validation
               GCfunc* fn = gco_to_function(frame_obj);
               BCREG delta = (BCREG)(tv - frame_prev(tv));
               lj_assertJ(not ref or ir_knum(ir)->u64 IS tv->u64, "frame slot %d PC mismatch", s);
               tr = J->slot[s - 1];
               ir = IR(tref_ref(tr));
               lj_assertJ(tref_isfunc(tr), "frame slot %d is not a function", s - 1);
               lj_assertJ(not tref_isk(tr) or fn IS ir_kfunc(ir), "frame slot %d function mismatch", s - 1);
               lj_assertJ(s > delta + 1 ? (J->slot[s - delta] & TREF_FRAME)
                  : (s IS delta + 1), "frame slot %d broken chain", s - 1);
               depth++;
            }
            else {
               // Stale TREF_FRAME from a returned inlined call. Clear it to avoid confusion.
               J->slot[s] = 0;
            }
         }
         else if ((tr & TREF_CONT)) {
            lj_assertJ(not ref or ir_knum(ir)->u64 IS tv->u64, "cont slot %d continuation mismatch", s);
            lj_assertJ((J->slot[s + FRC::HEADER_SIZE] & TREF_FRAME), "cont slot %d not followed by frame", s);
            depth++;
         }
         else if ((tr & TREF_KEYINDEX)) {
            lj_assertJ(tref_isint(tr), "keyindex slot %d bad type %d", s, tref_type(tr));
         }
         else {
            // Number repr. may differ, but other types must be the same.
            lj_assertJ(tvisnumber(tv) ? tref_isnumber(tr) : itype2irt(tv) IS tref_type(tr),
               "slot %d type mismatch: stack type %d vs IR type %d", s, itypemap(tv), tref_type(tr));
            if (tref_isk(tr)) {  // Compare constants.
               TValue tvk;
               lj_ir_kvalue(J->L, &tvk, ir);
               lj_assertJ((tvisnum(&tvk) and tvisnan(&tvk)) ?
                  (tvisnum(tv) and tvisnan(tv)) :
                  lj_obj_equal(tv, &tvk), "slot %d const mismatch: stack %016llx vs IR %016llx", s, tv->u64, tvk.u64);
            }
         }
      }
   }

   lj_assertJ(J->framedepth IS depth, "frame depth mismatch %d vs %d", J->framedepth, depth);
}
#endif

//********************************************************************************************************************
// Specialise a slot to a specific type. Note: slot can be negative!

static TRef sloadt(jit_State *J, int32_t slot, IRType t, int mode)
{
   // Caller may set IRT_GUARD in t.
   SlotView slots(J);
   IRBuilder ir(J);
   TRef ref = ir.emit_raw(IRT(IR_SLOAD, t), (int32_t)J->baseslot + slot, mode);
   slots[slot] = ref;
   return ref;
}

//********************************************************************************************************************
// Specialise a slot to the runtime type. Note: slot can be negative!

static TRef sload(jit_State *J, int32_t slot)
{
   SlotView slots(J);
   IRBuilder ir(J);
   IRType t = itype2irt(&J->L->base[slot]);
   int32_t abs_slot = (int32_t)J->baseslot + slot;
   TRef ref = ir.emit_raw(IRTG(IR_SLOAD, t), abs_slot, IRSLOAD_TYPECHECK);
   if (irtype_ispri(t)) ref = TREF_PRI(t);  //  Canonicalise primitive refs.
   slots[slot] = ref;
   return ref;
}

//********************************************************************************************************************
// Get TRef from slot. Load slot and specialise if not done already.

#define getslot(J, s)   (J->base[(s)] ? J->base[(s)] : sload(J, (int32_t)(s)))
// Note: getslot macro retained for compatibility; SlotView can be used for new code:
//   SlotView slots(J); TRef tr = slots.is_loaded(s) ? slots[s] : sload(J, s);

//********************************************************************************************************************
// Get TRef for current function.

static TRef getcurrf(jit_State *J)
{
   SlotView slots(J);
   if (slots.func()) return slots.func();
   // Non-base frame functions ought to be loaded already.
   lj_assertJ(J->baseslot IS FRC::MIN_BASESLOT, "bad baseslot");
   return sloadt(J, FRC::FUNC_SLOT_OFFSET, IRT_FUNC, IRSLOAD_READONLY);
}

//********************************************************************************************************************
// Compare for raw object equality.
// Returns 0 if the objects are the same.
// Returns 1 if they are different, but the same type.
// Returns 2 for two different types.
// Comparisons between primitives always return 1 -- no caller cares about it.

int lj_record_objcmp(jit_State *J, TRef a, TRef b, cTValue *av, cTValue *bv)
{
   int diff = !lj_obj_equal(av, bv);
   if (not tref_isk2(a, b)) {  // Shortcut, also handles primitives.
      IRBuilder ir(J);
      IRType ta = tref_isinteger(a) ? IRT_INT : tref_type(a);
      IRType tb = tref_isinteger(b) ? IRT_INT : tref_type(b);
      if (ta != tb) {
         // Widen mixed number/int comparisons to number/number comparison.
         if (ta IS IRT_INT and tb IS IRT_NUM) {
            a = ir.conv_num_int(a);
            ta = IRT_NUM;
         }
         else if (ta IS IRT_NUM and tb IS IRT_INT) {
            b = ir.conv_num_int(b);
         }
         else return 2;  //  Two different types are never equal.
      }
      ir.guard(diff ? IR_NE : IR_EQ, ta, a, b);
   }
   return diff;
}

//********************************************************************************************************************
// Constify a value. Returns 0 for non-representable object types.

TRef lj_record_constify(jit_State *J, cTValue *o)
{
   if (tvisgcv(o)) return lj_ir_kgc(J, gcV(o), itype2irt(o));
   else if (tvisint(o)) return lj_ir_kint(J, intV(o));
   else if (tvisnum(o)) return lj_ir_knumint(J, numV(o));
   else if (tvisbool(o)) return TREF_PRI(itype2irt(o));
   else return 0;  //  Can't represent lightuserdata (pointless).
}

//********************************************************************************************************************
// Emit a VLOAD with the correct type.

TRef lj_record_vload(jit_State *J, TRef ref, MSize idx, IRType t)
{
   IRBuilder ir(J);
   TRef tr = ir.guard(IR_VLOAD, t, ref, idx);
   if (irtype_ispri(t)) tr = TREF_PRI(t);  //  Canonicalise primitives.
   return tr;
}

//********************************************************************************************************************
// Canonicalise slots: convert integers to numbers.

static void canonicalise_slots(jit_State *J)
{
   BCREG s;
   if (LJ_DUALNUM) return;
   IRBuilder ir(J);
   for (s = J->baseslot + J->maxslot - 1; s >= 1; s--) {
      TRef tr = J->slot[s];
      if (tref_isinteger(tr) and !(tr & TREF_KEYINDEX)) {
         IRIns* ins = ir.at(tref_ref(tr));
         if (not (ins->o IS IR_SLOAD and (ins->op2 & (IRSLOAD_READONLY)))) J->slot[s] = ir.conv_num_int(tr);
      }
   }
}

//********************************************************************************************************************
// Stop recording.

void lj_record_stop(jit_State *J, TraceLink linktype, TraceNo lnk)
{
#ifdef LUAJIT_ENABLE_TABLE_BUMP
   if (J->retryrec) lj_trace_err(J, LJ_TRERR_RETRY);
#endif
   lj_trace_end(J);
   J->cur.linktype = linktype;
   J->cur.link = (uint16_t)lnk;
   // Looping back at the same stack level?
   if (lnk IS J->cur.traceno and FRC::at_trace_root(J)) {
      if ((J->flags & JIT_F_OPT_LOOP))  //  Shall we try to create a loop?
         goto nocanon;  //  Do not canonicalise or we lose the narrowing.
      if (J->cur.root)  //  Otherwise ensure we always link to the root trace.
         J->cur.link = J->cur.root;
   }
   canonicalise_slots(J);
nocanon:
   // Note: all loop ops must set J->pc to the following instruction!
   lj_snap_add(J);  //  Add loop snapshot.
   J->needsnap = 0;
   J->mergesnap = 1;  //  In case recording continues.
}

//********************************************************************************************************************
// Search bytecode backwards for a int/num constant slot initialiser.

static TRef find_kinit(jit_State *J, const BCIns *endpc, BCREG slot, IRType t)
{
   // This algorithm is rather simplistic and assumes quite a bit about how the bytecode is generated.
   // It works fine for FORI initialisers, but it won't necessarily work in other cases (e.g. iterator arguments).
   // It doesn't do anything fancy, either (like backpropagating MOVs).

   const BCIns *pc, * startpc = proto_bc(J->pt);
   for (pc = endpc - 1; pc > startpc; pc--) {
      BCIns ins = *pc;
      BCOp op = bc_op(ins);
      // First try to find the last instruction that stores to this slot.
      if (bcmode_a(op) IS BCMbase and bc_a(ins) <= slot) {
         return 0;  //  Multiple results, e.g. from a CALL or KNIL.
      }
      else if (bcmode_a(op) IS BCMdst and bc_a(ins) IS slot) {
         if (op IS BC_KSHORT or op IS BC_KNUM) {  // Found const. initialiser.
            // Now try to verify there's no forward jump across it.
            const BCIns *kpc = pc;
            for (; pc > startpc; pc--)
               if (bc_op(*pc) IS BC_JMP) {
                  const BCIns *target = pc + bc_j(*pc) + 1;
                  if (target > kpc and target <= endpc) return 0;  //  Conditional assignment.
               }
            if (op IS BC_KSHORT) {
               int32_t k = (int32_t)(int16_t)bc_d(ins);
               return t IS IRT_INT ? lj_ir_kint(J, k) : lj_ir_knum(J, (lua_Number)k);
            }
            else {
               cTValue *tv = proto_knumtv(J->pt, bc_d(ins));
               if (t IS IRT_INT) {
                  int32_t k = numberVint(tv);
                  if (tvisint(tv) or numV(tv) IS (lua_Number)k)  //  -0 is ok here.
                     return lj_ir_kint(J, k);
                  return 0;  //  Type mismatch.
               }
               else return lj_ir_knum(J, numberVnum(tv));
            }
         }
         return 0;  //  Non-constant initialiser.
      }
   }
   return 0;  //  No assignment to this slot found?
}

//********************************************************************************************************************
// Load and optionally convert a FORI argument from a slot.

static TRef fori_load(jit_State *J, BCREG slot, IRType t, int mode)
{
   int conv = (tvisint(&J->L->base[slot]) != (t IS IRT_INT)) ? IRSLOAD_CONVERT : 0;
   return sloadt(J, (int32_t)slot,
      IRType(t + (((mode & IRSLOAD_TYPECHECK) or (conv and t IS IRT_INT and !(mode >> 16))) ? IRT_GUARD : 0)),
      mode + conv);
}

//********************************************************************************************************************
// Peek before FORI to find a const initialiser. Otherwise load from slot.

static TRef fori_arg(jit_State *J, const BCIns *fori, BCREG slot, IRType t, int mode)
{
   TRef tr = J->base[slot];
   if (not tr) {
      tr = find_kinit(J, fori, slot, t);
      if (not tr) tr = fori_load(J, slot, t, mode);
   }
   return tr;
}

//********************************************************************************************************************
// Return the direction of the FOR loop iterator.
// It's important to exactly reproduce the semantics of the interpreter.

static int rec_for_direction(cTValue *o)
{
   return (tvisint(o) ? intV(o) : (int32_t)o->u32.hi) >= 0;
}

//********************************************************************************************************************
// Simulate the runtime behavior of the FOR loop iterator.

static LoopEvent rec_for_iter(IROp* op, cTValue *o, int isforl)
{
   lua_Number stopv = numberVnum(&o[FORL_STOP]);
   lua_Number idxv = numberVnum(&o[FORL_IDX]);
   lua_Number stepv = numberVnum(&o[FORL_STEP]);
   if (isforl) idxv += stepv;
   if (rec_for_direction(&o[FORL_STEP])) {
      if (idxv <= stopv) {
         *op = IR_LE;
         return idxv + 2 * stepv > stopv ? LOOPEV_ENTERLO : LOOPEV_ENTER;
      }
      *op = IR_GT; return LOOPEV_LEAVE;
   }
   else {
      if (stopv <= idxv) {
         *op = IR_GE;
         return idxv + 2 * stepv < stopv ? LOOPEV_ENTERLO : LOOPEV_ENTER;
      }
      *op = IR_LT; return LOOPEV_LEAVE;
   }
}

//********************************************************************************************************************
// Record checks for FOR loop overflow and step direction.

static void rec_for_check(jit_State *J, IRType t, int dir, TRef stop, TRef step, int init)
{
   IRBuilder ir(J);
   if (not tref_isk(step)) {
      // Non-constant step: need a guard for the direction.
      TRef zero = (t IS IRT_INT) ? ir.kint(0) : lj_ir_knum_zero(J);
      ir.guard(dir ? IR_GE : IR_LT, t, step, zero);
      // Add hoistable overflow checks for a narrowed FORL index.
      if (init and t IS IRT_INT) {
         if (tref_isk(stop)) {
            // Constant stop: optimise check away or to a range check for step.
            int32_t k = ir.at(tref_ref(stop))->i;
            if (dir) {
               if (k > 0) ir.guard_int(IR_LE, step, ir.kint((int32_t)0x7fffffff - k));
            }
            else if (k < 0) ir.guard_int(IR_GE, step, ir.kint((int32_t)0x80000000 - k));
         }
         else {
            // Stop+step variable: need full overflow check.
            TRef tr = ir.guard_int(IR_ADDOV, step, stop);
            ir.emit_int(IR_USE, tr, 0);  //  ADDOV is weak. Avoid dead result.
         }
      }
   }
   else if (init and t IS IRT_INT and !tref_isk(stop)) {
      // Constant step: optimise overflow check to a range check for stop.
      int32_t k = ir.at(tref_ref(step))->i;
      k = (int32_t)(dir ? 0x7fffffff : 0x80000000) - k;
      ir.guard_int(dir ? IR_LE : IR_GE, stop, ir.kint(k));
   }
}

//********************************************************************************************************************
// Record a FORL instruction.

static void rec_for_loop(jit_State *J, const BCIns *fori, ScEvEntry* scev, int init)
{
   IRBuilder ir(J);
   BCREG ra = bc_a(*fori);
   cTValue *tv = &J->L->base[ra];
   TRef idx = J->base[ra + FORL_IDX];
   IRType t = idx ? tref_type(idx) : (init or LJ_DUALNUM) ? lj_opt_narrow_forl(J, tv) : IRT_NUM;

   int mode = IRSLOAD_INHERIT + ((not LJ_DUALNUM or tvisint(tv) IS (t IS IRT_INT)) ? IRSLOAD_READONLY : 0);
   TRef stop = fori_arg(J, fori, ra + FORL_STOP, t, mode);
   TRef step = fori_arg(J, fori, ra + FORL_STEP, t, mode);
   int tc, dir = rec_for_direction(&tv[FORL_STEP]);

   lj_assertJ(bc_op(*fori) IS BC_FORI or bc_op(*fori) IS BC_JFORI, "bad bytecode %d instead of FORI/JFORI", bc_op(*fori));

   scev->t.irt = t;
   scev->dir = dir;
   scev->stop = tref_ref(stop);
   scev->step = tref_ref(step);
   rec_for_check(J, t, dir, stop, step, init);
   scev->start = tref_ref(find_kinit(J, fori, ra + FORL_IDX, IRT_INT));
   tc = (LJ_DUALNUM and !(scev->start and irref_isk(scev->stop) and irref_isk(scev->step) and
         tvisint(&tv[FORL_IDX]) IS (t IS IRT_INT))) ? IRSLOAD_TYPECHECK : 0;

   if (tc) {
      J->base[ra + FORL_STOP] = stop;
      J->base[ra + FORL_STEP] = step;
   }

   if (not idx) idx = fori_load(J, ra + FORL_IDX, t, IRSLOAD_INHERIT + tc + (J->scev.start << 16));
   if (not init) J->base[ra + FORL_IDX] = idx = ir.emit(IRT(IR_ADD, t), idx, step);

   J->base[ra + FORL_EXT] = idx;
   scev->idx = tref_ref(idx);
   setmref(scev->pc, fori);
   J->maxslot = ra + FORL_EXT + 1;
}

//********************************************************************************************************************
// Record FORL/JFORL or FORI/JFORI.

static LoopEvent rec_for(jit_State *J, const BCIns *fori, int isforl)
{
   IRBuilder ir(J);
   BCREG ra = bc_a(*fori);
   TValue* tv = &J->L->base[ra];
   TRef* tr = &J->base[ra];
   IROp op;
   LoopEvent ev;
   TRef stop;
   IRType t;
   if (isforl) {  // Handle FORL/JFORL opcodes.
      TRef idx = tr[FORL_IDX];
      if (mref<const BCIns>(J->scev.pc) IS fori and tref_ref(idx) IS J->scev.idx) {
         t = IRType(J->scev.t.irt);
         stop = J->scev.stop;
         idx = ir.emit(IRT(IR_ADD, t), idx, J->scev.step);
         tr[FORL_EXT] = tr[FORL_IDX] = idx;
      }
      else {
         ScEvEntry scev;
         rec_for_loop(J, fori, &scev, 0);
         t = IRType(scev.t.irt);
         stop = scev.stop;
      }
   }
   else {  // Handle FORI/JFORI opcodes.
      BCREG i;
      lj_meta_for(J->L, tv);
      t = (LJ_DUALNUM or tref_isint(tr[FORL_IDX])) ? lj_opt_narrow_forl(J, tv) : IRT_NUM;
      for (i = FORL_IDX; i <= FORL_STEP; i++) {
         if (not tr[i]) sload(J, ra + i);
         lj_assertJ(tref_isnumber_str(tr[i]), "bad FORI argument type");
         if (tref_isstr(tr[i])) tr[i] = ir.guard(IR_STRTO, IRT_NUM, tr[i], 0);
         if (t IS IRT_INT) {
            if (not tref_isinteger(tr[i])) tr[i] = ir.conv_int_num(tr[i]);
         }
         else if (not tref_isnum(tr[i])) tr[i] = ir.conv_num_int(tr[i]);
      }
      tr[FORL_EXT] = tr[FORL_IDX];
      stop = tr[FORL_STOP];
      rec_for_check(J, t, rec_for_direction(&tv[FORL_STEP]), stop, tr[FORL_STEP], 1);
   }

   ev = rec_for_iter(&op, tv, isforl);
   if (ev IS LOOPEV_LEAVE) {
      J->maxslot = ra + FORL_EXT + 1;
      J->pc = fori + 1;
   }
   else {
      J->maxslot = ra;
      J->pc = fori + bc_j(*fori) + 1;
   }

   lj_snap_add(J);

   ir.guard(op, t, tr[FORL_IDX], stop);

   if (ev IS LOOPEV_LEAVE) {
      J->maxslot = ra;
      J->pc = fori + bc_j(*fori) + 1;
   }
   else {
      J->maxslot = ra + FORL_EXT + 1;
      J->pc = fori + 1;
   }

   J->needsnap = 1;
   return ev;
}

//********************************************************************************************************************
// Record ITERL/JITERL.

static LoopEvent rec_iterl(jit_State *J, const BCIns iterins)
{
   BCREG ra = bc_a(iterins);
   if (not tref_isnil(getslot(J, ra))) {  // Looping back?
      J->base[ra - 1] = J->base[ra];  //  Copy result of ITERC to control var.
      J->maxslot = ra - 1 + bc_b(J->pc[-1]);
      J->pc += bc_j(iterins) + 1;
      return LOOPEV_ENTER;
   }
   else {
      J->maxslot = ra - 3;
      J->pc++;
      return LOOPEV_LEAVE;
   }
}

//********************************************************************************************************************
// Record LOOP/JLOOP. Now, that was easy.

static LoopEvent rec_loop(jit_State *J, BCREG ra, int skip)
{
   if (ra < J->maxslot) J->maxslot = ra;
   J->pc += skip;
   return LOOPEV_ENTER;
}

//********************************************************************************************************************
// Check if a loop repeatedly failed to trace because it didn't loop back.

static int innerloopleft(jit_State *J, const BCIns *pc)
{
   ptrdiff_t i;
   for (i = 0; i < PENALTY_SLOTS; i++)
      if (mref<const BCIns>(J->penalty[i].pc) IS pc) {
         if ((J->penalty[i].reason IS LJ_TRERR_LLEAVE or J->penalty[i].reason IS LJ_TRERR_LINNER) and J->penalty[i].val >= 2 * PENALTY_MIN)
            return 1;
         break;
      }
   return 0;
}

//********************************************************************************************************************
// Handle the case when an interpreted loop op is hit.

static void rec_loop_interp(jit_State *J, const BCIns *pc, LoopEvent ev)
{
   if (J->parent IS 0 and J->exitno IS 0) {
      if (pc IS J->startpc and FRC::at_trace_root(J)) {
         if (bc_op(J->cur.startins) IS BC_ITERN or bc_op(J->cur.startins) IS BC_ITERA) return;  //  See rec_itern()/rec_itera().
         // Same loop?
         if (ev IS LOOPEV_LEAVE)  //  Must loop back to form a root trace.
            lj_trace_err(J, LJ_TRERR_LLEAVE);
         lj_record_stop(J, TraceLink::LOOP, J->cur.traceno);  //  Looping trace.
      }
      else if (ev != LOOPEV_LEAVE) {  // Entering inner loop?
         // It's usually better to abort here and wait until the inner loop
         // is traced. But if the inner loop repeatedly didn't loop back,
         // this indicates a low trip count. In this case try unrolling
         // an inner loop even in a root trace. But it's better to be a bit
         // more conservative here and only do it for very short loops.

         if (bc_j(*pc) != -1 and !innerloopleft(J, pc)) lj_trace_err(J, LJ_TRERR_LINNER);  //  Root trace hit an inner loop.
         if ((ev != LOOPEV_ENTERLO and J->loopref and J->cur.nins - J->loopref > 24) or --J->loopunroll < 0)
            lj_trace_err(J, LJ_TRERR_LUNROLL);  //  Limit loop unrolling.
         J->loopref = J->cur.nins;
      }
   }
   else if (ev != LOOPEV_LEAVE) {  // Side trace enters an inner loop.
      J->loopref = J->cur.nins;
      if (--J->loopunroll < 0) lj_trace_err(J, LJ_TRERR_LUNROLL);  //  Limit loop unrolling.
   }  // Side trace continues across a loop that's left or not entered.
}

//********************************************************************************************************************
// Handle the case when an already compiled loop op is hit.

static void rec_loop_jit(jit_State *J, TraceNo lnk, LoopEvent ev)
{
   if (J->parent IS 0 and J->exitno IS 0) {  // Root trace hit an inner loop.
      // Better let the inner loop spawn a side trace back here.
      lj_trace_err(J, LJ_TRERR_LINNER);
   }
   else if (ev != LOOPEV_LEAVE) {  // Side trace enters a compiled loop.
      J->instunroll = 0;  //  Cannot continue across a compiled loop op.
      if (J->pc IS J->startpc and FRC::at_trace_root(J)) lj_record_stop(J, TraceLink::LOOP, J->cur.traceno);  //  Form extra loop.
      else lj_record_stop(J, TraceLink::ROOT, lnk);  //  Link to the loop.
   }  // Side trace continues across a loop that's left or not entered.
}

//********************************************************************************************************************
// Record ITERN.

static LoopEvent rec_itern(jit_State *J, BCREG ra, BCREG rb)
{
#if LJ_BE
   // YAGNI: Disabled on big-endian due to issues with lj_vm_next, IR_HIOP, RID_RETLO/RID_RETHI and ra_destpair.
   setintV(&J->errinfo, (int32_t)BC_ITERN);
   lj_trace_err_info(J, LJ_TRERR_NYIBC);
#else
   RecordIndex ix;

   // Since ITERN is recorded at the start, we need our own loop detection.

   if (J->pc IS J->startpc and
      (J->cur.nins > REF_FIRST + 1 or (J->cur.nins IS REF_FIRST + 1 and J->cur.ir[REF_FIRST].o != IR_PROF)) and
      FRC::at_trace_root(J) and J->parent IS 0 and J->exitno IS 0) {
      J->instunroll = 0;  //  Cannot continue unrolling across an ITERN.
      lj_record_stop(J, TraceLink::LOOP, J->cur.traceno);  //  Looping trace.
      return LOOPEV_ENTER;
   }

   J->maxslot = ra;
   lj_snap_add(J);  //  Required to make JLOOP the first ins in a side-trace.
   ix.tab = getslot(J, ra - 2);
   ix.key = J->base[ra - 1] ? J->base[ra - 1] : sloadt(J, (int32_t)(ra - 1), IRT_INT, IRSLOAD_KEYINDEX);
   copyTV(J->L, &ix.tabv, &J->L->base[ra - 2]);
   copyTV(J->L, &ix.keyv, &J->L->base[ra - 1]);
   ix.idxchain = (rb < 3);  //  Omit value type check, if unused.
   ix.mobj = 1;  //  We need the next index, too.
   J->maxslot = ra + lj_record_next(J, &ix);
   J->needsnap = 1;

   if (not tref_isnil(ix.key)) {  // Looping back?
      J->base[ra - 1] = ix.mobj | TREF_KEYINDEX;  //  Control var has next index.
      J->base[ra] = ix.key;
      J->base[ra + 1] = ix.val;
      J->pc += bc_j(J->pc[1]) + 2;
      return LOOPEV_ENTER;
   }
   else {
      J->maxslot = ra - 3;
      J->pc += 2;
      return LOOPEV_LEAVE;
   }
#endif
}

static TRef rec_array_xload(jit_State *J, TRef ArrayRef, TRef IdxRef, GCarray *Arr, int32_t IdxInt);

//********************************************************************************************************************
// Record ITERA.

static LoopEvent rec_itera(jit_State *J, BCREG ra, BCREG rb)
{
#if LJ_BE
   setintV(&J->errinfo, (int32_t)BC_ITERA);
   lj_trace_err_info(J, LJ_TRERR_NYIBC);
#else
   IRBuilder ir(J);

   if (J->pc IS J->startpc and
      (J->cur.nins > REF_FIRST + 1 or (J->cur.nins IS REF_FIRST + 1 and J->cur.ir[REF_FIRST].o != IR_PROF)) and
      FRC::at_trace_root(J) and J->parent IS 0 and J->exitno IS 0) {
      J->instunroll = 0;
      lj_record_stop(J, TraceLink::LOOP, J->cur.traceno);
      return LOOPEV_ENTER;
   }

   J->maxslot = ra;
   lj_snap_add(J);  // Required to make JLOOP the first ins in a side-trace.

   TRef arr_ref = getslot(J, ra - 2);
   if (not tref_isarray(arr_ref)) lj_trace_err(J, LJ_TRERR_BADTYPE);

   TValue *ctrl_tv = &J->L->base[ra - 1];
   GCarray *arr = arrayV(&J->L->base[ra - 2]);
   int32_t idx_int;
   if (tvisnil(ctrl_tv)) idx_int = 0;
   else if (tvisint(ctrl_tv)) idx_int = intV(ctrl_tv) + 1;
   else idx_int = int32_t(lj_num2int(numV(ctrl_tv))) + 1;

   TRef ctrl_ref = getslot(J, ra - 1);
   TRef idx_ref = tref_isnil(ctrl_ref) ? ir.kint(0) : lj_opt_narrow_index(J, ctrl_ref);
   if (not tref_isnil(ctrl_ref)) idx_ref = ir.emit_int(IR_ADD, idx_ref, ir.kint(1));

   TRef len_ref = ir.fload_int(arr_ref, IRFL_ARRAY_LEN);

   if (idx_int < 0 or MSize(idx_int) >= arr->len) {
      ir.guard_int(IR_UGE, idx_ref, len_ref);
      J->maxslot = ra - 3;
      J->pc += 2;
      return LOOPEV_LEAVE;
   }

   int nres = (rb < 3) ? 1 : 2;
   TRef value_ref = TREF_NIL;

   ir.guard_int(IR_ULT, idx_ref, len_ref);
   if (nres IS 2) {
      // Try inline path for numeric types
      value_ref = rec_array_xload(J, arr_ref, idx_ref, arr, idx_int);
      if (not value_ref) {
         // Fallback: C call for non-inline types
         TValue result_tv;
         lj_arr_getidx(J->L, arr, idx_int, &result_tv);
         IRType result_type = itype2irt(&result_tv);
         if (!LJ_DUALNUM and result_type IS IRT_INT) result_type = IRT_NUM;
         TRef tmp_ref = rec_tmpref(J, TREF_NIL, IRTMPREF_OUT1);
         lj_ir_call(J, IRCALL_lj_arr_getidx, arr_ref, idx_ref, tmp_ref);
         value_ref = lj_record_vload(J, tmp_ref, 0, result_type);
      }
   }

   J->base[ra - 1] = idx_ref;
   J->base[ra] = idx_ref;
   J->base[ra + 1] = value_ref;
   J->maxslot = ra + nres;
   J->needsnap = 1;
   J->pc += bc_j(J->pc[1]) + 2;
   return LOOPEV_ENTER;
#endif
}

//********************************************************************************************************************
// Record ISNEXT.

static void rec_isnext(jit_State *J, BCREG ra)
{
   cTValue *b = &J->L->base[ra - 3];
   if (tvisfunc(b) and funcV(b)->c.ffid IS FF_next and
      tvistab(b + 1) and tvisnil(b + 2)) {
      // These checks are folded away for a compiled pairs().
      IRBuilder ir(J);
      TRef func = getslot(J, ra - 3);
      TRef trid = ir.fload(func, IRFL_FUNC_FFID, IRT_U8);
      ir.guard_eq_int(trid, ir.kint(FF_next));
      (void)getslot(J, ra - 2); //  Type check for table.
      (void)getslot(J, ra - 1); //  Type check for nil key.
      J->base[ra - 1] = ir.kint(0) | TREF_KEYINDEX;
      J->maxslot = ra;
   }
   else {  // Abort trace. Interpreter will despecialise bytecode.
      lj_trace_err(J, LJ_TRERR_RECERR);
   }
}

//********************************************************************************************************************
// Record ISARR.

static void rec_isarr(jit_State *J, BCREG ra)
{
   TRef arr_ref = getslot(J, ra - 2);
   TRef ctrl_ref = getslot(J, ra - 1);

   if (not tref_isarray(arr_ref) or not tref_isnil(ctrl_ref)) {
      lj_trace_err(J, LJ_TRERR_RECERR);
   }

   // Keep control var nil so BC_ITERA can initialise the index.
   J->maxslot = ra;
}

//********************************************************************************************************************
// Record calls and returns

// Specialise to the runtime value of the called function or its prototype.

static TRef rec_call_specialise(jit_State *J, GCfunc* fn, TRef tr)
{
   IRBuilder ir(J);
   TRef kfunc;
   if (isluafunc(fn)) {
      GCproto* pt = funcproto(fn);
      // Too many closures created? Probably not a monomorphic function.
      if (pt->flags >= PROTO_CLC_POLY) {  // Specialise to prototype instead.
         TRef trpt = ir.fload_ptr(tr, IRFL_FUNC_PC);
         ir.guard_eq(trpt, ir.kptr(proto_bc(pt)), IRT_PGC);
         (void)lj_ir_kgc(J, obj2gco(pt), IRT_PROTO);  //  Prevent GC of proto.
         return tr;
      }
   }
   // Otherwise specialise to the function (closure) value itself.
   kfunc = ir.kfunc(fn);
   ir.guard_eq(tr, kfunc, IRT_FUNC);
   return kfunc;
}

//********************************************************************************************************************
// Record call setup.

static void rec_call_setup(jit_State *J, BCREG func, ptrdiff_t nargs)
{
   RecordIndex ix;
   TValue* functv = &J->L->base[func];
   TRef kfunc, * fbase = &J->base[func];
   ptrdiff_t i;

   (void)getslot(J, func); //  Ensure func has a reference.
   for (i = 1; i <= nargs; i++) (void)getslot(J, func + FRC::HEADER_SIZE + i - 1);  //  Ensure all args have a reference (args start at func+2).

   if (not tref_isfunc(fbase[0])) {  // Resolve __call metamethod.
      ix.tab = fbase[0];
      copyTV(J->L, &ix.tabv, functv);
      if (not lj_record_mm_lookup(J, &ix, MM_call) or !tref_isfunc(ix.mobj)) lj_trace_err(J, LJ_TRERR_NOMM);
      for (i = ++nargs; i > 1; i--) fbase[i + 1] = fbase[i];
      fbase[2] = fbase[0];
      fbase[0] = ix.mobj;  //  Replace function.
      functv = &ix.mobjv;
   }

   kfunc = rec_call_specialise(J, funcV(functv), fbase[0]);
   fbase[0] = kfunc;
   fbase[1] = TREF_FRAME;
   J->maxslot = (BCREG)nargs;
}

//********************************************************************************************************************
// Record call.

void lj_record_call(jit_State *J, BCREG func, ptrdiff_t nargs)
{
   rec_call_setup(J, func, nargs);
   FrameManager fm(J);
   // Bump frame.
   FRC::inc_depth(J);
   fm.push_call_frame(func);
   if (fm.would_overflow(J->maxslot)) lj_trace_err(J, LJ_TRERR_STACKOV);
}

//********************************************************************************************************************
// Record tail call.

void lj_record_tailcall(jit_State *J, BCREG func, ptrdiff_t nargs)
{
   rec_call_setup(J, func, nargs);
   FrameManager fm(J);
   if (frame_isvarg(J->L->base - 1)) {
      BCREG cbase = (BCREG)frame_delta(J->L->base - 1);
      if (FRC::dec_depth(J) < 0) lj_trace_err(J, LJ_TRERR_NYIRETL);
      fm.pop_delta_frame(cbase);
      func += cbase;
   }

   // Move func + args down.

   if (fm.at_root_baseslot()) J->base[func + 1] = TREF_FRAME;
   fm.compact_tailcall(func, J->maxslot);

   // Note: the new TREF_FRAME is now at J->base[-1] (even for slot #0).
   // Tailcalls can form a loop, so count towards the loop unroll limit.
   if (++J->tailcalled > J->loopunroll) lj_trace_err(J, LJ_TRERR_LUNROLL);
}

//********************************************************************************************************************
// Check unroll limits for down-recursion.

static int check_downrec_unroll(jit_State *J, GCproto* pt)
{
   IRRef ptref;
   for (ptref = J->chain[IR_KGC]; ptref; ptref = IR(ptref)->prev)
      if (ir_kgc(IR(ptref)) IS obj2gco(pt)) {
         int count = 0;
         IRRef ref;
         for (ref = J->chain[IR_RETF]; ref; ref = IR(ref)->prev)
            if (IR(ref)->op1 IS ptref) count++;
         if (count) {
            if (J->pc IS J->startpc) {
               if (count + J->tailcalled > J->param[JIT_P_recunroll]) return 1;
            }
            else lj_trace_err(J, LJ_TRERR_DOWNREC);
         }
      }
   return 0;
}

static TRef rec_cat(jit_State *J, BCREG baseslot, BCREG topslot);

//********************************************************************************************************************
// Record return.

void lj_record_ret(jit_State *J, BCREG rbase, ptrdiff_t gotresults)
{
   TValue* frame = J->L->base - 1;
   ptrdiff_t i;
   FrameManager fm(J);
   SlotView slots(J);
   for (i = 0; i < gotresults; i++) (void)getslot(J, rbase + i);  //  Ensure all results have a reference.

   while (frame_ispcall(frame)) {  // Immediately resolve pcall() returns.
      BCREG cbase = (BCREG)frame_delta(frame);
      if (FRC::dec_depth(J) <= 0) lj_trace_err(J, LJ_TRERR_NYIRETL);
      lj_assertJ(J->baseslot > FRC::MIN_BASESLOT, "bad baseslot for return");
      gotresults++;
      rbase += cbase;
      fm.pop_delta_frame(cbase);
      slots[--rbase] = TREF_TRUE;  //  Prepend true to results.
      frame = frame_prevd(frame);
      J->needsnap = 1;  //  Stop catching on-trace errors.
   }

   // Return to lower frame via interpreter for unhandled cases.
   if (FRC::at_root_depth(J) and J->pt and bc_isret(bc_op(*J->pc)) and
      (not frame_islua(frame) or (J->parent IS 0 and J->exitno IS 0 and !bc_isret(bc_op(J->cur.startins))))) {
      // NYI: specialise to frame type and return directly, not via RET*.
      slots.clear_range(0, rbase);  //  Purge dead slots.
      slots.set_maxslot(rbase + (BCREG)gotresults);
      lj_record_stop(J, TraceLink::RETURN, 0);  //  Return to interpreter.
      return;
   }

   if (frame_isvarg(frame)) {
      BCREG cbase = (BCREG)frame_delta(frame);
      if (FRC::dec_depth(J) < 0)  //  NYI: return of vararg func to lower frame.
         lj_trace_err(J, LJ_TRERR_NYIRETL);
      lj_assertJ(J->baseslot > FRC::MIN_BASESLOT, "bad baseslot for return");
      rbase += cbase;
      fm.pop_delta_frame(cbase);
      frame = frame_prevd(frame);
   }

   if (frame_islua(frame)) {  // Return to Lua frame.
      BCIns callins = *(frame_pc(frame) - 1);
      ptrdiff_t nresults = bc_b(callins) ? (ptrdiff_t)bc_b(callins) - 1 : gotresults;
      BCREG cbase = bc_a(callins);
      GCproto* pt = funcproto(frame_func(frame - (cbase + FRC::HEADER_SIZE)));
      if (pt->flags & PROTO_NOJIT) lj_trace_err(J, LJ_TRERR_CJITOFF);
      if (FRC::at_root_depth(J) and J->pt and frame IS J->L->base - 1) {
         if (check_downrec_unroll(J, pt)) {
            slots.set_maxslot((BCREG)(rbase + gotresults));
            lj_snap_purge(J);
            lj_record_stop(J, TraceLink::DOWNREC, J->cur.traceno);  //  Down-rec.
            return;
         }
         lj_snap_add(J);
      }

      for (i = 0; i < nresults; i++)  //  Adjust results.
         slots[i + FRC::FUNC_SLOT_OFFSET] = i < gotresults ? slots[rbase + i] : TREF_NIL;
      slots.set_maxslot(cbase + (BCREG)nresults);
      if (J->framedepth > 0) {  // Return to a frame that is part of the trace.
         (void)FRC::dec_depth(J);
         lj_assertJ(J->baseslot > cbase + FRC::HEADER_SIZE, "bad baseslot for return");
         fm.pop_lua_frame(cbase);
      }
      else if (J->parent IS 0 and J->exitno IS 0 and
         !bc_isret(bc_op(J->cur.startins))) {
         // Return to lower frame would leave the loop in a root trace.
         lj_trace_err(J, LJ_TRERR_LLEAVE);
      }
      else if (J->needsnap) {  // Tailcalled to ff with side-effects.
         lj_trace_err(J, LJ_TRERR_NYIRETL);  //  No way to insert snapshot here.
      }
      else {  // Return to lower frame. Guard for the target we return to.
         IRBuilder ir(J);
         TRef trpt = lj_ir_kgc(J, obj2gco(pt), IRT_PROTO);
         TRef trpc = ir.kptr((void*)frame_pc(frame));
         ir.guard(IR_RETF, IRT_PGC, trpt, trpc);
         J->retdepth++;
         J->needsnap = 1;
         lj_assertJ(fm.at_root_baseslot(), "bad baseslot for return");
         // Shift result slots up and clear the slots of the new frame below.
         slots.copy(cbase, FRC::FUNC_SLOT_OFFSET, nresults);
         slots.clear_range(FRC::FUNC_SLOT_OFFSET, cbase + FRC::HEADER_SIZE);
      }
   }
   else if (frame_iscont(frame)) {  // Return to continuation frame.
      ASMFunction cont = frame_contf(frame);
      BCREG cbase = (BCREG)frame_delta(frame);
      if (FRC::dec_depth_by(J, 2) < 0) lj_trace_err(J, LJ_TRERR_NYIRETL);
      fm.pop_delta_frame(cbase);
      slots.set_maxslot(cbase - FRC::CONT_FRAME_SIZE);
      if (cont IS lj_cont_ra) {
         // Copy result to destination slot.
         BCREG dst = bc_a(*(frame_contpc(frame) - 1));
         slots[dst] = gotresults ? slots[cbase + rbase] : TREF_NIL;
         slots.ensure_slot(dst);
      }
      else if (cont IS lj_cont_nop) {
         // Nothing to do here.
      }
      else if (cont IS lj_cont_cat) {
         BCREG bslot = bc_b(*(frame_contpc(frame) - 1));
         TRef tr = gotresults ? slots[cbase + rbase] : TREF_NIL;
         if (bslot != slots.maxslot()) {  // Concatenate the remainder.
            TValue* b = J->L->base, save;  //  Simulate lower frame and result.
            // Can't handle MM_concat + CALLT + fast func side-effects.
            if (J->postproc != LJ_POST_NONE) lj_trace_err(J, LJ_TRERR_NYIRETL);
            slots[slots.maxslot()] = tr;
            copyTV(J->L, &save, b - FRC::CONT_FRAME_SIZE);
            if (gotresults) copyTV(J->L, b - FRC::CONT_FRAME_SIZE, b + rbase);
            else setnilV(b - FRC::CONT_FRAME_SIZE);
            J->L->base = b - cbase;
            tr = rec_cat(J, bslot, cbase - FRC::CONT_FRAME_SIZE);
            b = J->L->base + cbase;  //  Undo.
            J->L->base = b;
            copyTV(J->L, b - FRC::CONT_FRAME_SIZE, &save);
         }

         if (tr) {  // Store final result.
            BCREG dst = bc_a(*(frame_contpc(frame) - 1));
            slots[dst] = tr;
            slots.ensure_slot(dst);
         }  // Otherwise continue with another __concat call.
      }
      else { // Result type already specialised.
         lj_assertJ(cont IS lj_cont_condf or cont IS lj_cont_condt, "bad continuation type");
      }
   }
   else lj_trace_err(J, LJ_TRERR_NYIRETL);  //  NYI: handle return to C frame.

   lj_assertJ(J->baseslot >= FRC::MIN_BASESLOT, "bad baseslot for return");
}

//********************************************************************************************************************
// Prepare to record call to metamethod.

static BCREG rec_mm_prep(jit_State *J, ASMFunction cont)
{
   SlotView slots(J);
   BCREG top = cont IS lj_cont_cat ? slots.maxslot() : curr_proto(J->L)->framesize;
   slots[top] = lj_ir_k64(J, IR_KNUM, u64ptr(contptr(cont)));
   slots[top + 1] = TREF_CONT;
   FRC::inc_depth(J);
   slots.clear_range(slots.maxslot(), top - slots.maxslot());  //  Clear frame gap to avoid resurrecting previous refs.
   return top + FRC::HEADER_SIZE;
}

//********************************************************************************************************************
// Record metamethod lookup.

int lj_record_mm_lookup(jit_State *J, RecordIndex* ix, MMS mm)
{
   IRBuilder ir(J);
   RecordIndex mix;
   GCtab* mt;
   int udtype = 0;  //  Declare before break/continue
   cTValue *mo = nullptr;  //  Declare before break/continue

   if (tref_istab(ix->tab)) {
      mt = tabref(tabV(&ix->tabv)->metatable);
      mix.tab = ir.fload_tab(ix->tab, IRFL_TAB_META);
   }
   else if (tref_isudata(ix->tab)) {
      udtype = udataV(&ix->tabv)->udtype;
      mt = tabref(udataV(&ix->tabv)->metatable);
      // The metatables of special userdata objects are treated as immutable.
      if (udtype != UDTYPE_USERDATA) {
         {
            // Specialise to the type of userdata.
            TRef tr = ir.fload(ix->tab, IRFL_UDATA_UDTYPE, IRT_U8);
            ir.guard_eq_int(tr, ir.kint(udtype));
         }

         mo = lj_tab_getstr(mt, mmname_str(J2G(J), mm));
         if (not mo or tvisnil(mo)) return 0;  //  No metamethod.
         // Treat metamethod or index table as immutable, too.
         if (not (tvisfunc(mo) or tvistab(mo))) lj_trace_err(J, LJ_TRERR_BADTYPE);
         copyTV(J->L, &ix->mobjv, mo);
         ix->mobj = lj_ir_kgc(J, gcV(mo), tvisfunc(mo) ? IRT_FUNC : IRT_TAB);
         ix->mtv = mt;
         ix->mt = TREF_NIL;  //  Dummy value for comparison semantics.
         return 1;  //  Got metamethod or index table.
      }
      mix.tab = ir.fload_tab(ix->tab, IRFL_UDATA_META);
   }
   else {
      // Specialise to base metatable. Must flush mcode in lua_setmetatable().
      mt = tabref(basemt_obj(J2G(J), &ix->tabv));
      if (mt IS nullptr) {
         ix->mt = TREF_NIL;
         return 0;  //  No metamethod.
      }
      ix->mt = mix.tab = lj_ir_ggfload(J, IRT_TAB, GG_OFS(g.gcroot) + (int)((GCROOT_BASEMT + itypemap(&ix->tabv)) * sizeof(GCRef)));
      goto nocheck;
   }
   ix->mt = mt ? mix.tab : TREF_NIL;
   ir.guard(mt ? IR_NE : IR_EQ, IRT_TAB, mix.tab, ir.knull(IRT_TAB));
nocheck:
   if (mt) {
      GCstr* mmstr = mmname_str(J2G(J), mm);
      cTValue *mo = lj_tab_getstr(mt, mmstr);
      if (mo and !tvisnil(mo)) copyTV(J->L, &ix->mobjv, mo);
      ix->mtv = mt;
      settabV(J->L, &mix.tabv, mt);
      setstrV(J->L, &mix.keyv, mmstr);
      mix.key = ir.kstr(mmstr);
      mix.val = 0;
      mix.idxchain = 0;
      ix->mobj = lj_record_idx(J, &mix);
      return !tref_isnil(ix->mobj);  //  1 if metamethod found, 0 if not.
   }
   return 0;  //  No metamethod.
}

//********************************************************************************************************************
// Record call to arithmetic metamethod.

static TRef rec_mm_arith(jit_State *J, RecordIndex* ix, MMS mm)
{
   // Set up metamethod call first to save ix->tab and ix->tabv.
   BCREG func = rec_mm_prep(J, mm IS MM_concat ? lj_cont_cat : lj_cont_ra);
   TRef* base = J->base + func;
   TValue* basev = J->L->base + func;
   base[FRC::HEADER_SIZE] = ix->tab; base[FRC::HEADER_SIZE + 1] = ix->key;  // Args at base[2], base[3]
   copyTV(J->L, basev + FRC::HEADER_SIZE, &ix->tabv);
   copyTV(J->L, basev + FRC::HEADER_SIZE + 1, &ix->keyv);
   if (not lj_record_mm_lookup(J, ix, mm)) {  // Lookup mm on 1st operand.
      if (mm != MM_unm) {
         ix->tab = ix->key;
         copyTV(J->L, &ix->tabv, &ix->keyv);
         if (lj_record_mm_lookup(J, ix, mm))  //  Lookup mm on 2nd operand.
            goto ok;
      }
      lj_trace_err(J, LJ_TRERR_NOMM);
   }
ok:
   base[0] = ix->mobj;
   base[1] = 0;
   copyTV(J->L, basev + 0, &ix->mobjv);
   lj_record_call(J, func, 2);
   return 0;  //  No result yet.
}

//********************************************************************************************************************
// Record call to __len metamethod.

static TRef rec_mm_len(jit_State *J, TRef tr, TValue* tv)
{
   RecordIndex ix;
   ix.tab = tr;
   copyTV(J->L, &ix.tabv, tv);
   if (lj_record_mm_lookup(J, &ix, MM_len)) {
      BCREG func = rec_mm_prep(J, lj_cont_ra);
      TRef* base = J->base + func;
      TValue* basev = J->L->base + func;
      base[0] = ix.mobj; copyTV(J->L, basev + 0, &ix.mobjv);
      // Args start at base[2] (after func slot and frame marker)
      base[FRC::HEADER_SIZE] = tr; copyTV(J->L, basev + FRC::HEADER_SIZE, tv);
      base[FRC::HEADER_SIZE + 1] = tr; copyTV(J->L, basev + FRC::HEADER_SIZE + 1, tv);
      lj_record_call(J, func, 2);
   }
   else {
      if (tref_istab(tr)) {
         IRBuilder ir(J);
         return ir.emit_int(IR_ALEN, tr, TREF_NIL); //equiv to: rc = emitir(IRTI(IR_ALEN), rc, TREF_NIL);
      }
      else if (tref_isarray(tr)) {
         IRBuilder ir(J);
         return ir.emit_int(IR_FLOAD, tr, IRFL_ARRAY_LEN); //equiv to: rc = emitir(IRTI(IR_FLOAD), rc, IRFL_ARRAY_LEN);
      }

      lj_trace_err(J, LJ_TRERR_NOMM);
   }
   return 0;  //  No result yet.
}

//********************************************************************************************************************
// Call a comparison metamethod.

static void rec_mm_callcomp(jit_State *J, RecordIndex* ix, int op)
{
   BCREG func = rec_mm_prep(J, (op & 1) ? lj_cont_condf : lj_cont_condt);
   // base points to first arg slot (after frame header)
   TRef* base = J->base + func + 1;
   TValue* tv = J->L->base + func + 1;
   base[-1] = ix->mobj; base[1] = ix->val; base[2] = ix->key;
   copyTV(J->L, tv - 1, &ix->mobjv);
   copyTV(J->L, tv + 1, &ix->valv);
   copyTV(J->L, tv + 2, &ix->keyv);
   lj_record_call(J, func, 2);
}

//********************************************************************************************************************
// Record call to equality comparison metamethod (for tab and udata only).

static void rec_mm_equal(jit_State *J, RecordIndex* ix, int op)
{
   ix->tab = ix->val;
   copyTV(J->L, &ix->tabv, &ix->valv);
   if (lj_record_mm_lookup(J, ix, MM_eq)) {  // Lookup mm on 1st operand.
      IRBuilder ir(J);
      cTValue *bv;
      TRef mo1 = ix->mobj;
      TValue mo1v;
      copyTV(J->L, &mo1v, &ix->mobjv);
      // Avoid the 2nd lookup and the objcmp if the metatables are equal.
      bv = &ix->keyv;
      if (tvistab(bv) and tabref(tabV(bv)->metatable) IS ix->mtv) {
         TRef mt2 = ir.fload_tab(ix->key, IRFL_TAB_META);
         ir.guard_eq(mt2, ix->mt, IRT_TAB);
      }
      else if (tvisudata(bv) and tabref(udataV(bv)->metatable) IS ix->mtv) {
         TRef mt2 = ir.fload_tab(ix->key, IRFL_UDATA_META);
         ir.guard_eq(mt2, ix->mt, IRT_TAB);
      }
      else {  // Lookup metamethod on 2nd operand and compare both.
         ix->tab = ix->key;
         copyTV(J->L, &ix->tabv, bv);
         if (not lj_record_mm_lookup(J, ix, MM_eq) or lj_record_objcmp(J, mo1, ix->mobj, &mo1v, &ix->mobjv))
            return;
      }
      rec_mm_callcomp(J, ix, op);
   }
}

//********************************************************************************************************************
// Record call to ordered comparison metamethods (for arbitrary objects).

static void rec_mm_comp(jit_State *J, RecordIndex* ix, int op)
{
   ix->tab = ix->val;
   copyTV(J->L, &ix->tabv, &ix->valv);
   while (true) {
      MMS mm = (op & 2) ? MM_le : MM_lt;  //  Try __le + __lt or only __lt.
      if (not lj_record_mm_lookup(J, ix, mm)) {  // Lookup mm on 1st operand.
         ix->tab = ix->key;
         copyTV(J->L, &ix->tabv, &ix->keyv);
         if (not lj_record_mm_lookup(J, ix, mm))  //  Lookup mm on 2nd operand.
            goto nomatch;
      }
      rec_mm_callcomp(J, ix, op);
      return;

   nomatch:
      // Lookup failed. Retry with  __lt and swapped operands.
      if (not (op & 2)) break;  //  Already at __lt. Interpreter will throw.
      ix->tab = ix->key; ix->key = ix->val; ix->val = ix->tab;
      copyTV(J->L, &ix->tabv, &ix->keyv);
      copyTV(J->L, &ix->keyv, &ix->valv);
      copyTV(J->L, &ix->valv, &ix->tabv);
      op ^= 3;
   }
}

//********************************************************************************************************************
// Indexed access

#ifdef LUAJIT_ENABLE_TABLE_BUMP
// Bump table allocations in bytecode when they grow during recording.
static void rec_idx_bump(jit_State *J, RecordIndex* ix)
{
   RBCHashEntry* rbc = &J->rbchash[(ix->tab & (RBCHASH_SLOTS - 1))];
   if (tref_ref(ix->tab) IS rbc->ref) {
      const BCIns *pc = mref<const BCIns>(rbc->pc);
      GCtab* tb = tabV(&ix->tabv);
      uint32_t nhbits;
      IRIns* ir;
      if (not tvisnil(&ix->keyv)) (void)lj_tab_set(J->L, tb, &ix->keyv);  //  Grow table right now.
      nhbits = tb->hmask > 0 ? lj_fls(tb->hmask) + 1 : 0;
      ir = IR(tref_ref(ix->tab));
      if (ir->o IS IR_TNEW) {
         uint32_t ah = bc_d(*pc);
         uint32_t asize = ah & 0x7ff, hbits = ah >> 11;
         if (nhbits > hbits) hbits = nhbits;
         if (tb->asize > asize) asize = tb->asize <= 0x7ff ? tb->asize : 0x7ff;

         if ((asize | (hbits << 11)) != ah) {  // Has the size changed?
            // Patch bytecode, but continue recording (for more patching).
            setbc_d(pc, (asize | (hbits << 11)));
            // Patching TNEW operands is only safe if the trace is aborted.
            ir->op1 = asize; ir->op2 = hbits;
            J->retryrec = 1;  //  Abort the trace at the end of recording.
         }
      }
      else if (ir->o IS IR_TDUP) {
         GCtab* tpl = gco_to_table(proto_kgc(&gcref(rbc->pt)->pt, ~(ptrdiff_t)bc_d(*pc)));
         // Grow template table, but preserve keys with nil values.
         if ((tb->asize > tpl->asize and (1u << nhbits) - 1 IS tpl->hmask) or
            (tb->asize IS tpl->asize and (1u << nhbits) - 1 > tpl->hmask)) {
            Node* node = noderef(tpl->node);
            uint32_t i, hmask = tpl->hmask, asize;
            TValue* array;
            for (i = 0; i <= hmask; i++) {
               if (not tvisnil(&node[i].key) and tvisnil(&node[i].val))
                  settabV(J->L, &node[i].val, tpl);
            }
            if (not tvisnil(&ix->keyv) and tref_isk(ix->key)) {
               TValue* o = lj_tab_set(J->L, tpl, &ix->keyv);
               if (tvisnil(o)) settabV(J->L, o, tpl);
            }
            lj_tab_resize(J->L, tpl, tb->asize, nhbits);
            node = noderef(tpl->node);
            hmask = tpl->hmask;
            for (i = 0; i <= hmask; i++) {
               // This is safe, since template tables only hold immutable values.
               if (tvistab(&node[i].val)) setnilV(&node[i].val);
            }
            // The shape of the table may have changed. Clean up array part, too.
            asize = tpl->asize;
            array = tvref(tpl->array);
            for (i = 0; i < asize; i++) {
               if (tvistab(&array[i])) setnilV(&array[i]);
            }
            J->retryrec = 1;  //  Abort the trace at the end of recording.
         }
      }
   }
}
#endif

//********************************************************************************************************************
// Record bounds-check. 0-based indexing: valid indices are [0, asize).

static void rec_idx_abc(jit_State *J, TRef asizeref, TRef ikey, uint32_t asize)
{
   // 0-based: no lower bound check needed (unsigned comparison handles negative indices)
   // Try to emit invariant bounds checks.

   if ((J->flags & (JIT_F_OPT_LOOP | JIT_F_OPT_ABC)) IS (JIT_F_OPT_LOOP | JIT_F_OPT_ABC)) {
      IRRef ref = tref_ref(ikey);
      IRIns* ins = IR(ref);
      int32_t ofs = 0;
      IRRef ofsref = 0;

      // Handle constant offsets.

      if (ins->o IS IR_ADD and irref_isk(ins->op2)) {
         ofsref = ins->op2;
         ofs = IR(ofsref)->i;
         ref = ins->op1;
         ins = IR(ref);
      }

      // Got scalar evolution analysis results for this reference?

      if (ref IS J->scev.idx) {
         IRBuilder ir(J);
         int32_t stop;
         lj_assertJ(irt_isint(J->scev.t) and ins->o IS IR_SLOAD, "only int SCEV supported");
         stop = numberVint(&(J->L->base - J->baseslot)[ins->op1 + FORL_STOP]);
         // Runtime value for stop of loop is within bounds?
         if ((uint64_t)stop + ofs < (uint64_t)asize) {
            // Emit invariant bounds check for stop.
            ir.guard(IR_ABC, IRT_P32, asizeref, ofs IS 0 ? J->scev.stop : ir.emit_int(IR_ADD, J->scev.stop, ofsref));
            // Emit invariant bounds check for start, if not const or negative.
            if (not (J->scev.dir and J->scev.start and (int64_t)ir.at(J->scev.start)->i + ofs >= 0))
               ir.guard(IR_ABC, IRT_P32, asizeref, ikey);
            return;
         }
      }
   }

   IRBuilder ir(J);
   ir.guard_int(IR_ABC, asizeref, ikey);  //  Emit regular bounds check.
}

//********************************************************************************************************************
// Record indexed key lookup.

static TRef rec_idx_key(jit_State *J, RecordIndex* ix, IRRollbackPoint *rbp)
{
   IRBuilder ir(J);
   TRef key;
   GCtab* t = tabV(&ix->tabv);
   ix->oldv = lj_tab_get(J->L, t, &ix->keyv);  //  Lookup previous value.
   *rbp = {};  // Initialise rollback point to unmarked state

   // Integer keys are looked up in the array part first.
   key = ix->key;
   if (tref_isnumber(key)) {
      int32_t k = numberVint(&ix->keyv);
      if (not tvisint(&ix->keyv) and numV(&ix->keyv) != (lua_Number)k) k = LJ_MAX_ASIZE;

      if (k >= 0 and (MSize)k < LJ_MAX_ASIZE) {  // 0-based: potential array key?
         TRef ikey = lj_opt_narrow_index(J, key);
         TRef asizeref = ir.fload_int(ix->tab, IRFL_TAB_ASIZE);
         if ((MSize)k < t->asize) {  // 0-based: currently an array key?
            TRef arrayref;
            rec_idx_abc(J, asizeref, ikey, t->asize);
            arrayref = ir.fload_ptr(ix->tab, IRFL_TAB_ARRAY);
            return ir.emit(IRT(IR_AREF, IRT_PGC), arrayref, ikey);
         }
         else {  // Currently not in array (may be an array extension)?
            ir.guard_int(IR_ULE, asizeref, ikey);  //  Inv. bounds check.
            if (k IS 0 and tref_isk(key)) key = lj_ir_knum_zero(J);  //  Canonicalize 0 or +-0.0 to +0.0.
            // And continue with the hash lookup.
         }
      }
      else if (not tref_isk(key)) {
         // We can rule out const numbers which failed the integerness test above. But all other numbers are potential array keys.

         if (t->asize IS 0) {  // True sparse tables have an empty array part.
            // Guard that the array part stays empty.
            TRef tmp = ir.fload_int(ix->tab, IRFL_TAB_ASIZE);
            ir.guard_eq_int(tmp, ir.kint(0));
         }
         else lj_trace_err(J, LJ_TRERR_NYITMIX);
      }
   }

   // Otherwise the key is located in the hash part.
   if (t->hmask IS 0) {  // Shortcut for empty hash part.
      // Guard that the hash part stays empty.
      TRef tmp = ir.fload_int(ix->tab, IRFL_TAB_HMASK);
      ir.guard_eq_int(tmp, ir.kint(0));
      return ir.kkptr(niltvg(J2G(J)));
   }

   if (tref_isinteger(key)) { //  Hash keys are based on numbers, not ints.
      key = ir.conv_num_int(key);
   }

   if (tref_isk(key)) { // Optimise lookup of constant hash keys.
      MSize hslot = (MSize)((char*)ix->oldv - (char*)&noderef(t->node)[0].val);
      if (t->hmask > 0 and hslot <= t->hmask * (MSize)sizeof(Node) and
         hslot <= 65535 * (MSize)sizeof(Node)) {
         TRef node, kslot, hm;
         rbp->mark(J);  //  Mark possible rollback point.
         hm = ir.fload_int(ix->tab, IRFL_TAB_HMASK);
         ir.guard_eq_int(hm, ir.kint((int32_t)t->hmask));
         node = ir.fload_ptr(ix->tab, IRFL_TAB_NODE);
         kslot = lj_ir_kslot(J, key, hslot / sizeof(Node));
         return ir.guard(IR_HREFK, IRT_PGC, node, kslot);
      }
   }

   // Fall back to a regular hash lookup.
   return ir.emit(IRT(IR_HREF, IRT_PGC), ix->tab, key);
}

//********************************************************************************************************************
// Determine whether a key is NOT one of the fast metamethod names.

static int nommstr(jit_State *J, TRef key)
{
   if (tref_isstr(key)) {
      if (tref_isk(key)) {
         GCstr* str = ir_kstr(IR(tref_ref(key)));
         uint32_t mm;
         for (mm = 0; mm <= MM_FAST; mm++) {
            if (mmname_str(J2G(J), mm) IS str) return 0;  //  MUST be one the fast metamethod names.
         }
      }
      else return 0;  //  Variable string key MAY be a metamethod name.
   }
   return 1;  //  CANNOT be a metamethod name.
}

//********************************************************************************************************************
// Record indexed load/store.

TRef lj_record_idx(jit_State *J, RecordIndex* ix)
{
   TRef xref;
   IROp xrefop, loadop;
   IRRollbackPoint rbp;
   cTValue *oldv;

   while (not tref_istab(ix->tab)) {  // Handle non-table lookup.
      // Never call raw lj_record_idx() on non-table.
      lj_assertJ(ix->idxchain != 0, "bad usage");
      if (not lj_record_mm_lookup(J, ix, ix->val ? MM_newindex : MM_index)) lj_trace_err(J, LJ_TRERR_NOMM);

handlemm:
      if (tref_isfunc(ix->mobj)) {  // Handle metamethod call.
         BCREG func = rec_mm_prep(J, ix->val ? lj_cont_nop : lj_cont_ra);
         SlotView slots(J);
         TValue* tv = J->L->base + func + 1;
         // Setup call frame: slots[func] = mobj, slots[func+2..] = args
         slots[func] = ix->mobj;
         slots[func + FRC::HEADER_SIZE] = ix->tab;
         slots[func + FRC::HEADER_SIZE + 1] = ix->key;
         setfuncV(J->L, tv - 1, funcV(&ix->mobjv));
         copyTV(J->L, tv + 1, &ix->tabv);
         copyTV(J->L, tv + 2, &ix->keyv);
         if (ix->val) {
            slots[func + FRC::HEADER_SIZE + 2] = ix->val;
            copyTV(J->L, tv + 3, &ix->valv);
            lj_record_call(J, func, 3);  //  mobj(tab, key, val)
            return 0;
         }
         else {
            lj_record_call(J, func, 2);  //  res = mobj(tab, key)
            return 0;  //  No result yet.
         }
      }

      // Otherwise retry lookup with metaobject.
      ix->tab = ix->mobj;
      copyTV(J->L, &ix->tabv, &ix->mobjv);
      if (--ix->idxchain IS 0) lj_trace_err(J, LJ_TRERR_IDXLOOP);
   }

   // First catch nil and NaN keys for tables.
   if (tvisnil(&ix->keyv) or (tvisnum(&ix->keyv) and tvisnan(&ix->keyv))) {
      if (ix->val) lj_trace_err(J, LJ_TRERR_STORENN); //  Better fail early.

      if (tref_isk(ix->key)) {
         if (ix->idxchain and lj_record_mm_lookup(J, ix, MM_index)) goto handlemm;
         return TREF_NIL;
      }
   }

   // Record the key lookup.
   xref = rec_idx_key(J, ix, &rbp);
   xrefop = (IROp)IR(tref_ref(xref))->o;
   loadop = xrefop IS IR_AREF ? IR_ALOAD : IR_HLOAD;
   // The lj_meta_tset() inconsistency is gone, but better play safe.
   oldv = xrefop IS IR_KKPTR ? (cTValue*)ir_kptr(IR(tref_ref(xref))) : ix->oldv;

   IRBuilder ir(J);
   if (ix->val IS 0) {  // Indexed load
      IRType t = itype2irt(oldv);
      TRef res;
      if (oldv IS niltvg(J2G(J))) {
         ir.guard_eq(xref, ir.kkptr(niltvg(J2G(J))), IRT_PGC);
         res = TREF_NIL;
      }
      else res = ir.guard(loadop, t, xref, 0);

      rbp.rollback_if_forwarded(J, res);  // Rollback hmask guard if HREFK + load forwarded.

      if (t IS IRT_NIL and ix->idxchain and lj_record_mm_lookup(J, ix, MM_index)) goto handlemm;
      if (irtype_ispri(t)) res = TREF_PRI(t);  //  Canonicalise primitives.
      return res;
   }
   else {  // Indexed store.
      GCtab* mt = tabref(tabV(&ix->tabv)->metatable);
      int keybarrier = tref_isgcv(ix->key) and !tref_isnil(ix->val);
      rbp.rollback_if_forwarded(J, xref);  // Rollback hmask guard if HREFK forwarded.

      if (tvisnil(oldv)) {  // Previous value was nil?
         // Need to duplicate the hasmm check for the early guards.
         int hasmm = 0;
         if (ix->idxchain and mt) {
            cTValue *mo = lj_tab_getstr(mt, mmname_str(J2G(J), MM_newindex));
            hasmm = mo and !tvisnil(mo);
         }

         if (hasmm) ir.guard(loadop, IRT_NIL, xref, 0);  //  Guard for nil value.
         else if (xrefop IS IR_HREF) ir.guard(oldv IS niltvg(J2G(J)) ? IR_EQ : IR_NE, IRT_PGC, xref, ir.kkptr(niltvg(J2G(J))));

         if (ix->idxchain and lj_record_mm_lookup(J, ix, MM_newindex)) {
            lj_assertJ(hasmm, "inconsistent metamethod handling");
            goto handlemm;
         }

         lj_assertJ(not hasmm, "inconsistent metamethod handling");

         if (oldv IS niltvg(J2G(J))) {  // Need to insert a new key.
            TRef key = ix->key;
            if (tref_isinteger(key)) key = ir.conv_num_int(key); //  NEWREF needs a TValue as a key.
            xref = ir.emit(IRT(IR_NEWREF, IRT_PGC), ix->tab, key);
            keybarrier = 0;  //  NEWREF already takes care of the key barrier.
#ifdef LUAJIT_ENABLE_TABLE_BUMP
            if ((J->flags & JIT_F_OPT_SINK))  //  Avoid a separate flag.
               rec_idx_bump(J, ix);
#endif
         }
      }
      else if (not lj_opt_fwd_wasnonnil(J, loadop, tref_ref(xref))) {
         // Cannot derive that the previous value was non-nil, must do checks.
         if (xrefop IS IR_HREF)  //  Guard against store to niltv.
            ir.guard_ne(xref, ir.kkptr(niltvg(J2G(J))), IRT_PGC);
         if (ix->idxchain) {  // Metamethod lookup required?
            // A check for nullptr metatable is cheaper (hoistable) than a load.
            if (not mt) {
               TRef mtref = ir.fload_tab(ix->tab, IRFL_TAB_META);
               ir.guard_eq(mtref, ir.knull(IRT_TAB), IRT_TAB);
            }
            else {
               IRType t = itype2irt(oldv);
               ir.guard(loadop, t, xref, 0);  //  Guard for non-nil value.
            }
         }
      }
      else keybarrier = 0;  //  Previous non-nil value kept the key alive.

      // Convert int to number before storing.

      if (not LJ_DUALNUM and tref_isinteger(ix->val)) ix->val = ir.conv_num_int(ix->val);
      ir.emit(IRT(loadop + IRDELTA_L2S, tref_type(ix->val)), xref, ix->val);
      if (keybarrier or tref_isgcv(ix->val)) ir.emit(IRT(IR_TBAR, IRT_NIL), ix->tab, 0);

      // Invalidate neg. metamethod cache for stores with certain string keys.

      if (not nommstr(J, ix->key)) {
         TRef fref = ir.emit(IRT(IR_FREF, IRT_PGC), ix->tab, IRFL_TAB_NOMM);
         ir.emit(IRT(IR_FSTORE, IRT_U8), fref, ir.kint(0));
      }
      J->needsnap = 1;
      return 0;
   }
}

//********************************************************************************************************************
// Determine result type of table traversal.

static IRType rec_next_types(GCtab* t, uint32_t idx)
{
   for (; idx < t->asize; idx++) {
      cTValue *a = arrayslot(t, idx);
      if (LJ_LIKELY(not tvisnil(a))) return (IRType)((LJ_DUALNUM ? IRT_INT : IRT_NUM) + (itype2irt(a) << 8));
   }
   idx -= t->asize;
   for (; idx <= t->hmask; idx++) {
      Node* n = &noderef(t->node)[idx];
      if (not tvisnil(&n->val)) return (IRType)(itype2irt(&n->key) + (itype2irt(&n->val) << 8));
   }
   return (IRType)(IRT_NIL + (IRT_NIL << 8));
}

//********************************************************************************************************************
// Record a table traversal step aka next().

int lj_record_next(jit_State *J, RecordIndex* ix)
{
   IRBuilder ir(J);
   IRType t, tkey, tval;
   TRef trvk;

   t = rec_next_types(tabV(&ix->tabv), ix->keyv.u32.lo);
   tkey = (IRType)(t & 0xff); tval = (IRType)(t >> 8);
   trvk = lj_ir_call(J, IRCALL_lj_vm_next, ix->tab, ix->key);
   if (ix->mobj or tkey IS IRT_NIL) {
      TRef idx = ir.emit_int(IR_HIOP, trvk, trvk);
      // Always check for invalid key from next() for nil result.
      if (not ix->mobj) ir.guard_ne_int(idx, ir.kint(-1));
      ix->mobj = idx;
   }

   ix->key = lj_record_vload(J, trvk, 1, tkey);
   if (tkey IS IRT_NIL or ix->idxchain) {  // Omit value type check.
      ix->val = TREF_NIL;
      return 1;
   }
   else {  // Need value.
      ix->val = lj_record_vload(J, trvk, 0, tval);
      return 2;
   }
}

//********************************************************************************************************************

static void rec_tsetm(jit_State *J, BCREG ra, BCREG rn, int32_t i)
{
   RecordIndex ix;
   cTValue *basev = J->L->base;
   GCtab* t = tabV(&basev[ra - 1]);
   settabV(J->L, &ix.tabv, t);
   ix.tab = getslot(J, ra - 1);
   ix.idxchain = 0;

#ifdef LUAJIT_ENABLE_TABLE_BUMP
   if ((J->flags & JIT_F_OPT_SINK)) {
      if (t->asize < i + rn - ra) lj_tab_reasize(J->L, t, i + rn - ra);
      setnilV(&ix.keyv);
      rec_idx_bump(J, &ix);
   }
#endif

   for (; ra < rn; i++, ra++) {
      setintV(&ix.keyv, i);
      ix.key = lj_ir_kint(J, i);
      copyTV(J->L, &ix.valv, &basev[ra]);
      ix.val = getslot(J, ra);
      lj_record_idx(J, &ix);
   }
}

//********************************************************************************************************************
// Check whether upvalue is immutable and ok to constify.

static int rec_upvalue_constify(jit_State *J, GCupval* uvp)
{
   if (uvp->immutable) {
      cTValue *o = uvval(uvp);
      // Don't constify objects that may retain large amounts of memory.
      if (not (tvistab(o) or tvisudata(o) or tvisthread(o))) return 1;
   }
   return 0;
}

//********************************************************************************************************************
// Record upvalue load/store.

static TRef rec_upvalue(jit_State *J, uint32_t uv, TRef val)
{
   IRBuilder ir(J);
   GCupval* uvp = &gcref(J->fn->l.uvptr[uv])->uv;
   TRef fn = getcurrf(J);
   IRRef uref;

   int needbarrier = 0;
   if (rec_upvalue_constify(J, uvp)) {  // Try to constify immutable upvalue.
      TRef tr, kfunc;
      lj_assertJ(val IS 0, "bad usage");
      if (not tref_isk(fn)) {  // Late specialisation of current function.
         if (J->pt->flags >= PROTO_CLC_POLY) goto noconstify;
         kfunc = ir.kfunc(J->fn);
         ir.guard_eq(fn, kfunc, IRT_FUNC);
         J->base[-2] = kfunc;
         fn = kfunc;
      }
      tr = lj_record_constify(J, uvval(uvp));
      if (tr) return tr;
   }

noconstify:

   // Note: this effectively limits LJ_MAX_UPVAL to 127.

   uv = (uv << 8) | (hashrot(uvp->dhash, uvp->dhash + HASH_BIAS) & 0xff);
   if (not uvp->closed) {
      uref = tref_ref(ir.guard(IR_UREFO, IRT_PGC, fn, uv));
      // In current stack?
      if (uvval(uvp) >= tvref(J->L->stack) and
         uvval(uvp) < tvref(J->L->maxstack)) {
         int32_t slot = (int32_t)(uvval(uvp) - (J->L->base - J->baseslot));
         if (slot >= 0) {  // Aliases an SSA slot?
            ir.guard_eq(REF_BASE, ir.emit(IRT(IR_ADD, IRT_PGC), uref, ir.kint((slot + FRC::FUNC_SLOT_OFFSET) * -8)), IRT_PGC);
            slot -= (int32_t)J->baseslot;  //  Note: slot number may be negative!
            if (val IS 0) return getslot(J, slot);
            else {
               J->base[slot] = val;
               if (slot >= (int32_t)J->maxslot) J->maxslot = (BCREG)(slot + 1);
               return 0;
            }
         }
      }
      ir.guard(IR_UGT, IRT_PGC, ir.emit(IRT(IR_SUB, IRT_PGC), uref, REF_BASE), ir.kint((J->baseslot + J->maxslot) * 8));
   }
   else {
      needbarrier = 1;
      uref = tref_ref(ir.guard(IR_UREFC, IRT_PGC, fn, uv));
   }
   if (val IS 0) {  // Upvalue load
      IRType t = itype2irt(uvval(uvp));
      TRef res = ir.guard(IR_ULOAD, t, uref, 0);
      if (irtype_ispri(t)) res = TREF_PRI(t);  //  Canonicalise primitive refs.
      return res;
   }
   else {  // Upvalue store.
      // Convert int to number before storing.
      if (not LJ_DUALNUM and tref_isinteger(val)) val = ir.conv_num_int(val);
      ir.emit(IRT(IR_USTORE, tref_type(val)), uref, val);
      if (needbarrier and tref_isgcv(val)) ir.emit(IRT(IR_OBAR, IRT_NIL), uref, val);
      J->needsnap = 1;
      return 0;
   }
}

//********************************************************************************************************************
// Record calls to Lua functions

// Check unroll limits for calls.

static void check_call_unroll(jit_State *J, TraceNo lnk)
{
   cTValue *frame = J->L->base - 1;
   void* pc = mref<void>(frame_func(frame)->l.pc);
   int32_t depth = J->framedepth;
   int32_t count = 0;
   if ((J->pt->flags & PROTO_VARARG)) depth--;  //  Vararg frame still missing.
   for (; depth > 0; depth--) {  // Count frames with same prototype.
      if (frame_iscont(frame)) depth--;
      frame = frame_prev(frame);
      if (mref<void>(frame_func(frame)->l.pc) IS pc) count++;
   }
   if (J->pc IS J->startpc) {
      if (count + J->tailcalled > J->param[JIT_P_recunroll]) {
         J->pc++;
         if (FRC::at_trace_root(J)) lj_record_stop(J, TraceLink::TAILREC, J->cur.traceno);  //  Tail-rec.
         else lj_record_stop(J, TraceLink::UPREC, J->cur.traceno);  //  Up-recursion.
      }
   }
   else {
      if (count > J->param[JIT_P_callunroll]) {
         if (lnk) {  // Possible tail- or up-recursion.
            lj_trace_flush(J, lnk);  //  Flush trace that only returns.
            // Set a small, pseudo-random hotcount for a quick retry of JFUNC*.
            hotcount_set(J2GG(J), J->pc + 1, lj_prng_u64(&J2G(J)->prng) & 15u);
         }
         lj_trace_err(J, LJ_TRERR_CUNROLL);
      }
   }
}

//********************************************************************************************************************
// Record Lua function setup.

static void rec_func_setup(jit_State *J)
{
   GCproto* pt = J->pt;
   BCREG s, numparams = pt->numparams;
   if ((pt->flags & PROTO_NOJIT)) lj_trace_err(J, LJ_TRERR_CJITOFF);
   if (J->baseslot + pt->framesize >= LJ_MAX_JSLOTS) lj_trace_err(J, LJ_TRERR_STACKOV);
   // Fill up missing parameters with nil.
   for (s = J->maxslot; s < numparams; s++) J->base[s] = TREF_NIL;
   // The remaining slots should never be read before they are written.
   J->maxslot = numparams;
}

//********************************************************************************************************************
// Record Lua vararg function setup.

static void rec_func_vararg(jit_State *J)
{
   GCproto* pt = J->pt;
   BCREG s, fixargs, vframe = J->maxslot + FRC::HEADER_SIZE;
   lj_assertJ((pt->flags & PROTO_VARARG), "FUNCV in non-vararg function");
   if (J->baseslot + vframe + pt->framesize >= LJ_MAX_JSLOTS) lj_trace_err(J, LJ_TRERR_STACKOV);
   J->base[vframe + FRC::FUNC_SLOT_OFFSET] = J->base[FRC::FUNC_SLOT_OFFSET];  //  Copy function up.
   J->base[vframe - 1] = TREF_FRAME;

   // Copy fixarg slots up and set their original slots to nil.

   fixargs = pt->numparams < J->maxslot ? pt->numparams : J->maxslot;
   for (s = 0; s < fixargs; s++) {
      J->base[vframe + s] = J->base[s];
      J->base[s] = TREF_NIL;
   }

   J->maxslot = fixargs;
   FRC::inc_depth(J);
   J->base += vframe;
   J->baseslot += vframe;
}

//********************************************************************************************************************
// Record entry to a Lua function.

static void rec_func_lua(jit_State *J)
{
   rec_func_setup(J);
   check_call_unroll(J, 0);
}

//********************************************************************************************************************
// Record entry to an already compiled function.

static void rec_func_jit(jit_State *J, TraceNo lnk)
{
   GCtrace* T;
   rec_func_setup(J);
   T = traceref(J, lnk);
   if (T->linktype IS TraceLink::RETURN) {  // Trace returns to interpreter?
      check_call_unroll(J, lnk);
      // Temporarily unpatch JFUNC* to continue recording across function.
      J->patchins = *J->pc;
      J->patchpc = (BCIns*)J->pc;
      *J->patchpc = T->startins;
      return;
   }
   J->instunroll = 0;  //  Cannot continue across a compiled function.
   if (J->pc IS J->startpc and FRC::at_trace_root(J)) {
      lj_record_stop(J, TraceLink::TAILREC, J->cur.traceno);  //  Extra tail-rec.
   }
   else lj_record_stop(J, TraceLink::ROOT, lnk);  //  Link to the function.
}

//********************************************************************************************************************
// Vararg handling

// Record vararg instruction.

static void rec_varg(jit_State *J, BCREG dst, ptrdiff_t nresults)
{
   SlotView slots(J);
   int32_t numparams = J->pt->numparams;
   ptrdiff_t nvararg = frame_delta(J->L->base - 1) - numparams - FRC::HEADER_SIZE;
   lj_assertJ(frame_isvarg(J->L->base - 1), "VARG in non-vararg frame");
   if (dst > slots.maxslot()) slots.clear(dst - 1);  //  Prevent resurrection of unrelated slot.
   if (J->framedepth > 0) {  // Simple case: varargs defined on-trace.
      ptrdiff_t i;
      if (nvararg < 0) nvararg = 0;
      if (nresults IS -1) {
         nresults = nvararg;
         slots.set_maxslot(dst + (BCREG)nvararg);
      }
      else if (dst + nresults > slots.maxslot()) slots.set_maxslot(dst + (BCREG)nresults);

      for (i = 0; i < nresults; i++) slots[dst + i] = i < nvararg ? getslot(J, i - nvararg + FRC::FUNC_SLOT_OFFSET) : TREF_NIL;
   }
   else {  // Unknown number of varargs passed to trace.
      TRef fr = emitir(IRTI(IR_SLOAD), 1, IRSLOAD_READONLY | IRSLOAD_FRAME);
      int32_t frofs = 8 * (FRC::HEADER_SIZE + numparams) + FRAME_VARG;
      if (nresults >= 0) {  // Known fixed number of results.
         ptrdiff_t i;
         if (nvararg > 0) {
            ptrdiff_t nload = nvararg >= nresults ? nresults : nvararg;
            TRef vbase;
            if (nvararg >= nresults) emitir(IRTGI(IR_GE), fr, lj_ir_kint(J, frofs + 8 * (int32_t)nresults));
            else emitir(IRTGI(IR_EQ), fr, lj_ir_kint(J, (int32_t)frame_ftsz(J->L->base - 1)));
            vbase = emitir(IRT(IR_SUB, IRT_IGC), REF_BASE, fr);
            vbase = emitir(IRT(IR_ADD, IRT_PGC), vbase, lj_ir_kint(J, frofs - 8));
            for (i = 0; i < nload; i++) {
               IRType t = itype2irt(&J->L->base[i + FRC::FUNC_SLOT_OFFSET - nvararg]);
               slots[dst + i] = lj_record_vload(J, vbase, i, t);
            }
         }
         else {
            emitir(IRTGI(IR_LE), fr, lj_ir_kint(J, frofs));
            nvararg = 0;
         }
         for (i = nvararg; i < nresults; i++) slots[dst + i] = TREF_NIL;
         if (dst + (BCREG)nresults > slots.maxslot()) slots.set_maxslot(dst + (BCREG)nresults);
      }
      else {
         setintV(&J->errinfo, BC_VARG);
         lj_trace_err_info(J, LJ_TRERR_NYIBC);
      }
   }

   if (J->baseslot + slots.maxslot() >= LJ_MAX_JSLOTS) lj_trace_err(J, LJ_TRERR_STACKOV);
}

//********************************************************************************************************************
// Record allocations

static TRef rec_tnew(jit_State *J, uint32_t ah)
{
   uint32_t asize = ah & 0x7ff;
   uint32_t hbits = ah >> 11;
   TRef tr;
   if (asize IS 0x7ff) asize = 0x801;
   tr = emitir(IRTG(IR_TNEW, IRT_TAB), asize, hbits);
#ifdef LUAJIT_ENABLE_TABLE_BUMP
   J->rbchash[(tr & (RBCHASH_SLOTS - 1))].ref = tref_ref(tr);
   setmref(J->rbchash[(tr & (RBCHASH_SLOTS - 1))].pc, J->pc);
   setgcref(J->rbchash[(tr & (RBCHASH_SLOTS - 1))].pt, obj2gco(J->pt));
#endif
   return tr;
}

//********************************************************************************************************************
// Concatenation

static TRef rec_cat(jit_State *J, BCREG baseslot, BCREG topslot)
{
   TRef* top = &J->base[topslot];
   TValue savetv[5];
   BCREG s;
   RecordIndex ix;
   lj_assertJ(baseslot < topslot, "bad CAT arg");
   for (s = baseslot; s <= topslot; s++) (void)getslot(J, s);  //  Ensure all arguments have a reference.
   if (tref_isnumber_str(top[0]) and tref_isnumber_str(top[-1])) {
      TRef tr, hdr, * trp, * xbase, * base = &J->base[baseslot];
      // First convert numbers to strings.
      for (trp = top; trp >= base; trp--) {
         if (tref_isnumber(*trp)) *trp = emitir(IRT(IR_TOSTR, IRT_STR), *trp, tref_isnum(*trp) ? IRTOSTR_NUM : IRTOSTR_INT);
         else if (not tref_isstr(*trp)) break;
      }
      xbase = ++trp;
      tr = hdr = emitir(IRT(IR_BUFHDR, IRT_PGC), lj_ir_kptr(J, &J2G(J)->tmpbuf), IRBUFHDR_RESET);
      do {
         tr = emitir(IRTG(IR_BUFPUT, IRT_PGC), tr, *trp++);
      } while (trp <= top);
      tr = emitir(IRTG(IR_BUFSTR, IRT_STR), tr, hdr);
      J->maxslot = (BCREG)(xbase - J->base);
      if (xbase IS base) return tr;  //  Return simple concatenation result.
      // Pass partial result.
      topslot = J->maxslot--;
      *xbase = tr;
      top = xbase;
      setstrV(J->L, &ix.keyv, &J2G(J)->strempty);  //  Simulate string result.
   }
   else {
      J->maxslot = topslot - 1;
      copyTV(J->L, &ix.keyv, &J->L->base[topslot]);
   }
   copyTV(J->L, &ix.tabv, &J->L->base[topslot - 1]);
   ix.tab = top[-1];
   ix.key = top[0];
   memcpy(savetv, &J->L->base[topslot - 1], sizeof(savetv));  //  Save slots.
   rec_mm_arith(J, &ix, MM_concat);  //  Call __concat metamethod.
   memcpy(&J->L->base[topslot - 1], savetv, sizeof(savetv));  //  Restore slots.
   return 0;  //  No result yet.
}

//********************************************************************************************************************
// Record bytecode ops

// Prepare for comparison.

static void rec_comp_prep(jit_State *J)
{
   // Prevent merging with snapshot #0 (GC exit) since we fixup the PC.
   if (J->cur.nsnap IS 1 and J->cur.snap[0].ref IS J->cur.nins) emitir_raw(IRT(IR_NOP, IRT_NIL), 0, 0);
   lj_snap_add(J);
}

//********************************************************************************************************************
// Fixup comparison.
//
// After recording a comparison guard, this function modifies the snapshot's PC to point to the "opposite target"
// (the path not taken by the guard). This optimization prevents re-recording the same comparison instruction
// when a side trace starts from this guard's exit.
//
// IMPORTANT: This PC modification must be SKIPPED inside try blocks.
//
// When a guard is inside a try block and leads to break/continue, the bytecode sequence is:
//   ISEQN i, 3      ; Guard recorded here (snapshot created with this PC)
//   JMP   +N        ; Jump to break cleanup
//   ...
//   BC_TRYLEAVE     ; Pop try frame
//   JMP   loop_exit ; Exit the loop
//
// If we modify the snapshot PC to point past the comparison (e.g., to BC_TRYLEAVE), then when the guard exits:
// 1. The snapshot is restored with PC pointing to BC_TRYLEAVE
// 2. But the try stack still has the try frame pushed (from the JIT-compiled TRYENTER)
// 3. The interpreter executes BC_TRYLEAVE which pops the already-correct try stack
// 4. This causes try stack corruption and subsequent slot corruption
//
// By skipping the PC fixup inside try blocks, the guard exit correctly resumes at the comparison instruction,
// allowing the interpreter to re-execute and follow the correct break/continue path with proper cleanup.

static void rec_comp_fixup(jit_State *J, const BCIns *pc, int cond)
{
   BCIns jmpins = pc[1];
   const BCIns *npc = pc + 2 + (cond ? bc_j(jmpins) : 0);
   SnapShot *snap = &J->cur.snap[J->cur.nsnap - 1];

   // Skip PC modification inside try blocks to prevent snapshot restoration issues.
   // See function header comment for detailed explanation.

   if (J->L->try_stack.depth > 0) {
      J->needsnap = 1;
      return;
   }

   // Set PC to opposite target to avoid re-recording the comparison in side trace.
   // The PC is stored in the snapmap at position (mapofs + nent) as a 64-bit value:
   //   pcbase = (pc_pointer << 8) | baseslot

   SnapEntry *flink = &J->cur.snapmap[snap->mapofs + snap->nent];
   uint64_t pcbase;
   memcpy(&pcbase, flink, sizeof(uint64_t));
   pcbase = (pcbase & 0xff) | (u64ptr(npc) << 8);
   memcpy(flink, &pcbase, sizeof(uint64_t));

   J->needsnap = 1;
   if (bc_a(jmpins) < J->maxslot) J->maxslot = bc_a(jmpins);
   lj_snap_shrink(J);  //  Shrink last snapshot if possible.
}

//********************************************************************************************************************
// Handle post-processing actions from the previous instruction.
// Returns true if recording should continue, false if we should return early.

static bool rec_handle_postproc(jit_State *J)
{
   if (J->postproc IS LJ_POST_NONE) return true;

   switch (J->postproc) {
      case LJ_POST_FIXCOMP: {  // Fixup comparison.
         const BCIns *pc = (const BCIns*)(uintptr_t)J2G(J)->tmptv.u64;
         rec_comp_fixup(J, pc, (not tvistruecond(&J2G(J)->tmptv2) ^ (bc_op(*pc) & 1)));
      }
      [[fallthrough]];

      case LJ_POST_FIXGUARD:      // Fixup and emit pending guard.
      case LJ_POST_FIXGUARDSNAP:  // Fixup and emit pending guard and snapshot.
         if (not tvistruecond(&J2G(J)->tmptv2)) {
            J->fold.ins.o ^= 1;  // Flip guard to opposite.
            if (J->postproc IS LJ_POST_FIXGUARDSNAP) {
               SnapShot *snap = &J->cur.snap[J->cur.nsnap - 1];
               J->cur.snapmap[snap->mapofs + snap->nent - 1]--;  // False -> true.
            }
         }
         lj_opt_fold(J);  // Emit pending guard.
         [[fallthrough]];

      case LJ_POST_FIXBOOL:
         if (not tvistruecond(&J2G(J)->tmptv2)) {
            TValue *tv = J->L->base;
            for (BCREG s = 0; s < J->maxslot; s++) {  // Fixup stack slot (if any).
               if (J->base[s] IS TREF_TRUE and tvisfalse(&tv[s])) {
                  J->base[s] = TREF_FALSE;
                  break;
               }
            }
         }
         break;

      case LJ_POST_FIXCONST: {
         TValue *tv = J->L->base;
         for (BCREG s = 0; s < J->maxslot; s++) {  // Constify stack slots (if any).
            if (J->base[s] IS TREF_NIL and !tvisnil(&tv[s]))
               J->base[s] = lj_record_constify(J, &tv[s]);
         }
         break;
      }

      case LJ_POST_FFRETRY:  // Suppress recording of retried fast function.
         if (bc_op(*J->pc) >= BC__MAX) return false;
         break;

      default:
         lj_assertJ(0, "bad post-processing mode");
         break;
   }

   J->postproc = LJ_POST_NONE;
   return true;
}

//********************************************************************************************************************
// Decode bytecode operands based on their modes.
// Populates ops with decoded references and copies runtime values as needed.

static void rec_decode_operands(jit_State *J, cTValue *lbase, RecordOps *ops)
{
   BCIns ins = ops->ins;
   BCOp op = ops->op;

   // Decode 'A' operand
   ops->ra = bc_a(ins);
   ops->ix.val = 0;

   switch (bcmode_a(op)) {
      case BCMvar:
         copyTV(J->L, ops->rav(), &lbase[ops->ra]);
         ops->ix.val = ops->ra = getslot(J, ops->ra);
         break;
      default: break;  // Handled later by opcode-specific code.
   }

   // Decode 'B' and 'C' operands
   ops->rb = bc_b(ins);
   ops->rc = bc_c(ins);

   switch (bcmode_b(op)) {
      case BCMnone:
         ops->rb = 0;
         ops->rc = bc_d(ins);  // Upgrade rc to 'rd'.
         break;
      case BCMvar:
         copyTV(J->L, ops->rbv(), &lbase[ops->rb]);
         ops->ix.tab = ops->rb = getslot(J, ops->rb);
         break;
      default: break;  // Handled later by opcode-specific code.
   }

   // Decode 'C' operand based on its mode
   switch (bcmode_c(op)) {
      case BCMvar:
         copyTV(J->L, ops->rcv(), &lbase[ops->rc]);
         ops->ix.key = ops->rc = getslot(J, ops->rc);
         break;
      case BCMpri:
         setpriV(ops->rcv(), ~uint64_t(ops->rc));
         ops->ix.key = ops->rc = TREF_PRI(IRT_NIL + ops->rc);
         break;
      case BCMnum: {
         cTValue *tv = proto_knumtv(J->pt, ops->rc);
         copyTV(J->L, ops->rcv(), tv);
         ops->ix.key = ops->rc = tvisint(tv) ? lj_ir_kint(J, intV(tv)) : lj_ir_knumint(J, numV(tv));
         break;
      }
      case BCMstr: {
         GCstr *s = gco_to_string(proto_kgc(J->pt, ~(ptrdiff_t)ops->rc));
         setstrV(J->L, ops->rcv(), s);
         ops->ix.key = ops->rc = lj_ir_kstr(J, s);
         break;
      }
      default: break;  // Handled later by opcode-specific code.
   }
}

//********************************************************************************************************************
// Handle ordered comparison ops: BC_ISLT, BC_ISGE, BC_ISLE, BC_ISGT

static void rec_comp_ordered(jit_State *J, RecordOps *ops)
{
   TRef ra = ops->ra, rc = ops->rc;
   BCOp op = ops->op;
   RecordIndex *ix = &ops->ix;
   TValue *rav = ops->rav(), *rcv = ops->rcv();

   // Emit nothing for two numeric or string consts.
   if (tref_isk2(ra, rc) and tref_isnumber_str(ra) and tref_isnumber_str(rc))
      return;

   IRType ta = tref_isinteger(ra) ? IRT_INT : tref_type(ra);
   IRType tc = tref_isinteger(rc) ? IRT_INT : tref_type(rc);

   if (ta != tc) {
      // Widen mixed number/int comparisons to number/number comparison.
      if (ta IS IRT_INT and tc IS IRT_NUM) {
         ra = emitir(IRTN(IR_CONV), ra, IRCONV_NUM_INT);
         ta = IRT_NUM;
      }
      else if (ta IS IRT_NUM and tc IS IRT_INT) {
         rc = emitir(IRTN(IR_CONV), rc, IRCONV_NUM_INT);
      }
      else {
         ta = IRT_NIL;  // Force metamethod for different types.
      }
   }

   rec_comp_prep(J);
   int irop = (int)op - (int)BC_ISLT + (int)IR_LT;

   if (ta IS IRT_NUM) {
      if ((irop & 1)) irop ^= 4;  // ISGE/ISGT are unordered.
      if (not lj_ir_numcmp(numberVnum(rav), numberVnum(rcv), (IROp)irop))
         irop ^= 5;
   }
   else if (ta IS IRT_INT) {
      if (not lj_ir_numcmp(numberVnum(rav), numberVnum(rcv), (IROp)irop))
         irop ^= 1;
   }
   else if (ta IS IRT_STR) {
      if (not lj_ir_strcmp(strV(rav), strV(rcv), (IROp)irop)) irop ^= 1;
      ra = lj_ir_call(J, IRCALL_lj_str_cmp, ra, rc);
      rc = lj_ir_kint(J, 0);
      ta = IRT_INT;
   }
   else {
      rec_mm_comp(J, ix, (int)op);
      return;
   }

   emitir(IRTG(irop, ta), ra, rc);
   rec_comp_fixup(J, J->pc, ((int)op ^ irop) & 1);
}

//********************************************************************************************************************
// Handle equality comparison ops: BC_ISEQV, BC_ISNEV, BC_ISEQS, BC_ISNES, etc.

static void rec_comp_equality(jit_State *J, RecordOps *ops)
{
   TRef ra = ops->ra, rc = ops->rc;
   BCOp op = ops->op;
   RecordIndex *ix = &ops->ix;
   TValue *rav = ops->rav(), *rcv = ops->rcv();

   // Emit nothing for two non-table, non-udata consts.

   if (tref_isk2(ra, rc) and !(tref_istab(ra) or tref_isudata(ra))) return;

   rec_comp_prep(J);
   int diff = lj_record_objcmp(J, ra, rc, rav, rcv);

   if (diff IS 2 or !(tref_istab(ra) or tref_isudata(ra))) {
      rec_comp_fixup(J, J->pc, ((int)op & 1) IS !diff);
   }
   else if (diff IS 1) { // Only check __eq if different, but same type.
      rec_mm_equal(J, ix, (int)op);
   }
}

//********************************************************************************************************************
// Handle arithmetic ops: BC_UNM, BC_ADD*, BC_SUB*, BC_MUL*, BC_DIV*, BC_MOD*, BC_POW

static TRef rec_arith_op(jit_State *J, RecordOps *ops)
{
   TRef rb = ops->rb, rc = ops->rc;
   BCOp op = ops->op;
   RecordIndex *ix = &ops->ix;
   TValue *rav = ops->rav(), *rbv = ops->rbv(), *rcv = ops->rcv();

   switch (op) {
      case BC_UNM:
         if (tref_isnumber_str(rc)) return lj_opt_narrow_unm(J, rc, rcv);
         ix->tab = rc;
         copyTV(J->L, &ix->tabv, rcv);
         return rec_mm_arith(J, ix, MM_unm);

      case BC_ADDNV: case BC_SUBNV: case BC_MULNV: case BC_DIVNV: case BC_MODNV:
         // Swap rb/rc and rbv/rcv. rav is temp.
         ix->tab = rc; ix->key = rc = rb; rb = ix->tab;
         copyTV(J->L, rav, rbv);
         copyTV(J->L, rbv, rcv);
         copyTV(J->L, rcv, rav);
         if (op IS BC_MODNV) {
            if (tref_isnumber_str(rb) and tref_isnumber_str(rc))
               return lj_opt_narrow_mod(J, rb, rc, rbv, rcv);
            return rec_mm_arith(J, ix, MM_mod);
         }
         [[fallthrough]];

      case BC_ADDVN: case BC_SUBVN: case BC_MULVN: case BC_DIVVN:
      case BC_ADDVV: case BC_SUBVV: case BC_MULVV: case BC_DIVVV: {
         MMS mm = bcmode_mm(op);
         if (tref_isnumber_str(rb) and tref_isnumber_str(rc))
            return lj_opt_narrow_arith(J, rb, rc, rbv, rcv, (IROp)((int)mm - (int)MM_add + (int)IR_ADD));
         return rec_mm_arith(J, ix, mm);
      }

      case BC_MODVN: case BC_MODVV:
         if (tref_isnumber_str(rb) and tref_isnumber_str(rc))
            return lj_opt_narrow_mod(J, rb, rc, rbv, rcv);
         return rec_mm_arith(J, ix, MM_mod);

      case BC_POW:
         if (tref_isnumber_str(rb) and tref_isnumber_str(rc))
            return lj_opt_narrow_pow(J, rb, rc, rbv, rcv);
         return rec_mm_arith(J, ix, MM_pow);

      default:
         return 0;
   }
}

//********************************************************************************************************************
// Inline XLOAD for primitive numeric array element access.
//
// Emits IR_XLOAD directly for numeric and GC-type array elements, bypassing the opaque lj_arr_getidx
// C call.  This exposes the load to CSE, load forwarding, and alias analysis.
//
// Supported element types: BYTE, INT16, INT32, INT64, FLOAT, DOUBLE, STR_GC, TABLE.
// Returns 0 for unsupported types or nil GC elements (caller falls back to the C call).
//
// For GC types (STR_GC, TABLE), the element is a GCRef (8 bytes).  At recording time, if the element
// is nil (null GCRef), we return 0 to fall back to the C call — nil elements in hot loops are uncommon.
// For non-nil elements, we emit an XLOAD with a non-null guard; if the element becomes nil at runtime,
// the guard exits to the interpreter.

static TRef rec_array_xload(jit_State *J, TRef ArrayRef, TRef IdxRef, GCarray *Arr, int32_t IdxInt)
{
   IRBuilder ir(J);
   AET et = Arr->elemtype;
   int shift;

   switch (et) {
      case AET::BYTE:   shift = 0; break;
      case AET::INT16:  shift = 1; break;
      case AET::INT32:  shift = 2; break;
      case AET::FLOAT:  shift = 2; break;
      case AET::INT64:  shift = 3; break;
      case AET::DOUBLE: shift = 3; break;
      case AET::STR_GC: shift = 3; break;  // GCRef is 8 bytes
      case AET::TABLE:  shift = 3; break;  // GCRef is 8 bytes
      default: return 0; // Unsupported type, fall back to C call
   }

   // For GC types, check at recording time whether the element is nil.  If nil, fall back to the
   // C call — nil elements in hot paths are uncommon and a separate trace will form if needed.
   if (et IS AET::STR_GC or et IS AET::TABLE) {
      auto *elem = (GCRef *)lj_array_index(Arr, (uint32_t)IdxInt);
      if (not gcref(*elem)) return 0;  // Nil at recording time → fall back
   }

   // Guard: elemtype matches recorded value (CSE'd across accesses to the same array)
   TRef et_ref = ir.fload(ArrayRef, IRFL_ARRAY_ELEMTYPE, IRT_U8);
   ir.guard_eq_int(et_ref, ir.kint(int32_t(et)));

   // Load storage pointer
   TRef storage_ref = ir.fload_ptr(ArrayRef, IRFL_ARRAY_STORAGE);

   // Compute element address: storage + (idx << shift)
   TRef offset_ref = (shift > 0) ? ir.emit_int(IR_BSHL, IdxRef, ir.kint(shift)) : IdxRef;
   // Extend offset to pointer width for 64-bit platforms
   TRef offset_ptr = emitir(IRT(IR_CONV, IRT_INTP), offset_ref,
      (IRT_INTP << IRCONV_DSH) | IRT_INT | IRCONV_SEXT);
   TRef addr = emitir(IRT(IR_ADD, IRT_PTR), storage_ref, offset_ptr);

   // Emit XLOAD with the appropriate type and convert to Lua-visible type
   TRef val;
   switch (et) {
      case AET::BYTE:
         val = emitir(IRT(IR_XLOAD, IRT_U8), addr, 0);
         if (not LJ_DUALNUM)
            val = emitir(IRTN(IR_CONV), val, IRCONV_NUM_INT);
         break;
      case AET::INT16:
         val = emitir(IRT(IR_XLOAD, IRT_I16), addr, 0);
         if (not LJ_DUALNUM)
            val = emitir(IRTN(IR_CONV), val, IRCONV_NUM_INT);
         break;
      case AET::INT32:
         val = emitir(IRT(IR_XLOAD, IRT_INT), addr, 0);
         if (not LJ_DUALNUM)
            val = emitir(IRTN(IR_CONV), val, IRCONV_NUM_INT);
         break;
      case AET::INT64:
         val = emitir(IRT(IR_XLOAD, IRT_I64), addr, 0);
         val = emitir(IRTN(IR_CONV), val, (IRT_NUM << IRCONV_DSH) | IRT_I64);
         break;
      case AET::FLOAT:
         val = emitir(IRT(IR_XLOAD, IRT_FLOAT), addr, 0);
         val = emitir(IRTN(IR_CONV), val, (IRT_NUM << IRCONV_DSH) | IRT_FLOAT);
         break;
      case AET::DOUBLE:
         val = emitir(IRT(IR_XLOAD, IRT_NUM), addr, 0);
         break;
      case AET::STR_GC:
         val = emitir(IRT(IR_XLOAD, IRT_STR), addr, 0);
         // Guard: loaded GCRef is non-null (exit to interpreter if element becomes nil at runtime)
         emitir(IRTG(IR_NE, IRT_STR), val, ir.knull(IRT_STR));
         break;
      case AET::TABLE:
         val = emitir(IRT(IR_XLOAD, IRT_TAB), addr, 0);
         // Guard: loaded GCRef is non-null (exit to interpreter if element becomes nil at runtime)
         emitir(IRTG(IR_NE, IRT_TAB), val, ir.knull(IRT_TAB));
         break;
      default:
         return 0;
   }
   return val;
}

//********************************************************************************************************************
// Inline XSTORE for array element access.
//
// Emits IR_XSTORE directly for numeric types (BYTE, INT16, INT32, INT64, FLOAT, DOUBLE) and GC-reference
// types (STR_GC, TABLE).  Returns true if the store was handled inline, false to fall back to the C call.
//
// For GC types, the store value must type-match (tref_isstr for STR_GC, tref_istab for TABLE) or be nil.
// Nil stores emit a null GCRef (clearing the slot) without a write barrier.  Non-nil stores emit XSTORE
// followed by IR_TBAR on the array container for the GC write barrier.
//
// Note: The read-only flag is never checked by intention; invalid attempts to write to read-only arrays are thrown
// before the JIT optimisation stage.

static bool rec_array_xstore(jit_State *J, TRef ArrayRef, TRef IdxRef, TRef ValRef, GCarray *Arr)
{
   IRBuilder ir(J);
   AET et = Arr->elemtype;
   int shift;
   bool is_gc_type = false;

   switch (et) {
      case AET::BYTE:   shift = 0; break;
      case AET::INT16:  shift = 1; break;
      case AET::INT32:  shift = 2; break;
      case AET::FLOAT:  shift = 2; break;
      case AET::INT64:  shift = 3; break;
      case AET::DOUBLE: shift = 3; break;
      case AET::STR_GC: shift = 3; is_gc_type = true; break;
      case AET::TABLE:  shift = 3; is_gc_type = true; break;
      default: return false; // Unsupported type, fall back to C call
   }

   // For numeric types, the value must be a number
   if (not is_gc_type and not tref_isnumber(ValRef)) return false;

   // For GC types, validate the value type at recording time
   if (is_gc_type and not tref_isnil(ValRef)) {
      if (et IS AET::STR_GC and not tref_isstr(ValRef)) return false;
      if (et IS AET::TABLE and not tref_istab(ValRef)) return false;
   }

   // Guard: elemtype matches recorded value (CSE'd across accesses to the same array)
   TRef et_ref = ir.fload(ArrayRef, IRFL_ARRAY_ELEMTYPE, IRT_U8);
   ir.guard_eq_int(et_ref, ir.kint(int32_t(et)));

   // Load storage pointer
   TRef storage_ref = ir.fload_ptr(ArrayRef, IRFL_ARRAY_STORAGE);

   // Compute element address: storage + (idx << shift)
   TRef offset_ref = (shift > 0) ? ir.emit_int(IR_BSHL, IdxRef, ir.kint(shift)) : IdxRef;
   TRef offset_ptr = emitir(IRT(IR_CONV, IRT_INTP), offset_ref,
      (IRT_INTP << IRCONV_DSH) | IRT_INT | IRCONV_SEXT);
   TRef addr = emitir(IRT(IR_ADD, IRT_PTR), storage_ref, offset_ptr);

   // Convert and emit XSTORE
   TRef store_val;
   switch (et) {
      case AET::BYTE:
         store_val = tref_isnum(ValRef)
            ? emitir(IRTI(IR_CONV), ValRef, IRCONV_INT_NUM | IRCONV_ANY)
            : ValRef;
         emitir(IRT(IR_XSTORE, IRT_U8), addr, store_val);
         break;
      case AET::INT16:
         store_val = tref_isnum(ValRef)
            ? emitir(IRTI(IR_CONV), ValRef, IRCONV_INT_NUM | IRCONV_ANY)
            : ValRef;
         emitir(IRT(IR_XSTORE, IRT_I16), addr, store_val);
         break;
      case AET::INT32:
         store_val = tref_isnum(ValRef)
            ? emitir(IRTI(IR_CONV), ValRef, IRCONV_INT_NUM | IRCONV_ANY)
            : ValRef;
         emitir(IRT(IR_XSTORE, IRT_INT), addr, store_val);
         break;
      case AET::INT64:
         if (tref_isint(ValRef))
            store_val = emitir(IRT(IR_CONV, IRT_I64), ValRef,
               (IRT_I64 << IRCONV_DSH) | IRT_INT | IRCONV_SEXT);
         else
            store_val = emitir(IRT(IR_CONV, IRT_I64), ValRef,
               (IRT_I64 << IRCONV_DSH) | IRT_NUM | IRCONV_ANY);
         emitir(IRT(IR_XSTORE, IRT_I64), addr, store_val);
         break;
      case AET::FLOAT:
         store_val = tref_isint(ValRef)
            ? emitir(IRTN(IR_CONV), ValRef, IRCONV_NUM_INT) : ValRef;
         store_val = emitir(IRT(IR_CONV, IRT_FLOAT), store_val,
            (IRT_FLOAT << IRCONV_DSH) | IRT_NUM);
         emitir(IRT(IR_XSTORE, IRT_FLOAT), addr, store_val);
         break;
      case AET::DOUBLE:
         store_val = tref_isint(ValRef)
            ? emitir(IRTN(IR_CONV), ValRef, IRCONV_NUM_INT) : ValRef;
         emitir(IRT(IR_XSTORE, IRT_NUM), addr, store_val);
         break;
      case AET::STR_GC:
      case AET::TABLE:
         if (tref_isnil(ValRef)) {
            // Nil store: write a null GCRef to clear the slot.  No write barrier needed.
            emitir(IRT(IR_XSTORE, IRT_P64), addr, ir.knull(IRT_P64));
         }
         else {
            // Non-nil store: write the GC reference and emit a write barrier on the container
            IRType gc_irt = (et IS AET::STR_GC) ? IRT_STR : IRT_TAB;
            emitir(IRT(IR_XSTORE, gc_irt), addr, ValRef);
            ir.emit(IRT(IR_TBAR, IRT_NIL), ArrayRef, 0);
         }
         break;
      default:
         return false;
   }
   return true;
}

//********************************************************************************************************************
// Handle native array ops: BC_AGETV, BC_AGETB, BC_ASETV, BC_ASETB
//
// Native arrays (GCarray) are different from tables - they have typed elements and 0-based indexing internally.
// We emit calls to helper functions that handle the element type conversion.

static TRef rec_array_op(jit_State *J, RecordOps *ops)
{
   IRBuilder ir(J);
   TRef array_ref = ops->rb;
   if (not tref_isarray(array_ref)) lj_trace_err(J, LJ_TRERR_BADTYPE);

   bool is_get = (ops->op IS BC_AGETV or ops->op IS BC_AGETB);
   bool is_lit = (ops->op IS BC_AGETB or ops->op IS BC_ASETB);
   int32_t idx_int = 0;
   TRef idx_ref = 0;

   if (is_lit) {
      idx_int = int32_t(ops->rc);
      idx_ref = ir.kint(idx_int);
   }
   else {
      cTValue *key_tv = ops->rcv();
      if (tvisint(key_tv)) idx_int = intV(key_tv);
      else if (tvisnum(key_tv)) {
         lua_Number num = numV(key_tv);
         idx_int = int32_t(lj_num2int(num));
         if (not ((lua_Number)idx_int IS num)) lj_trace_err(J, LJ_TRERR_BADTYPE);
      }
      else lj_trace_err(J, LJ_TRERR_BADTYPE);

      if (not tref_isnumber(ops->rc)) lj_trace_err(J, LJ_TRERR_BADTYPE);
      idx_ref = lj_opt_narrow_index(J, ops->rc);
   }

   TRef len_ref = ir.fload_int(array_ref, IRFL_ARRAY_LEN);
   ir.guard_int(IR_ULT, idx_ref, len_ref);

   if (is_get) {
      GCarray *arr = arrayV(ops->rbv());
      if (idx_int < 0 or MSize(idx_int) >= arr->len) lj_trace_err(J, LJ_TRERR_BADTYPE);

      // Try inline path for numeric and GC types
      TRef result = rec_array_xload(J, array_ref, idx_ref, arr, idx_int);
      if (result) return result;

      // Fallback: C call for non-inline types
      TValue result_tv;
      lj_arr_getidx(J->L, arr, idx_int, &result_tv);
      IRType result_type = itype2irt(&result_tv);
      if (!LJ_DUALNUM and result_type IS IRT_INT) result_type = IRT_NUM;
      TRef tmp_ref = rec_tmpref(J, TREF_NIL, IRTMPREF_OUT1);
      lj_ir_call(J, IRCALL_lj_arr_getidx, array_ref, idx_ref, tmp_ref);
      return lj_record_vload(J, tmp_ref, 0, result_type);
   }

   // Try inline path for numeric and GC types
   if (rec_array_xstore(J, array_ref, idx_ref, ops->ra, arrayV(ops->rbv())))
      return 0;

   // Fallback: C call for non-inline types
   TRef tmp_ref = rec_tmpref(J, ops->ra, IRTMPREF_IN1);
   lj_ir_call(J, IRCALL_lj_arr_setidx, array_ref, idx_ref, tmp_ref);
   return 0;
}

//********************************************************************************************************************
// Handle native object ops: BC_OBGETF, BC_OBSETF
//
// Native objects (GCobject) use field handler lookup tables.  We emit calls to helper functions that handle the field
// access.  Type inference uses MetaClass field definitions - no probing/side effects.

static TRef rec_object_get(jit_State *J, RecordOps *ops)
{
   TRef obj_ref = ops->rb;
   if (not tref_isobject(obj_ref)) lj_trace_err(J, LJ_TRERR_BADTYPE);

   TRef key_ref = ops->rc;  // Get the string key reference - BC_OBGETF/OBSETF use ~RC for the constant index

   // Look up the field type from MetaClass definitions
   GCobject *obj = objectV(ops->rbv());
   GCstr *key = strV(ops->rcv()); // Field name

   int field_offset;
   uint32_t field_flags = 0;
   int field_type = ir_object_field_type(obj, key, field_offset, field_flags); // Returns an IRT or -1
   if (field_type < 0) lj_trace_err(J, LJ_TRERR_BADTYPE);  // Unknown field type (could be a action/method) - abort recording

   if (field_offset) {
      constexpr uint32_t unsupported = FD_ARRAY|FD_STRUCT|FD_UNSIGNED|FD_CPP;
      if (not (field_flags & unsupported)) {
         // Guard: object is alive (uid != 0)
         TRef uid_ref = emitir(IRTI(IR_FLOAD), obj_ref, IRFL_OBJ_UID);
         emitir(IRTGI(IR_NE), uid_ref, lj_ir_kint(J, 0));

         // Guard: object is not detached (flags & GCOBJ_DETACHED == 0)
         TRef flags_ref = emitir(IRT(IR_FLOAD, IRT_U8), obj_ref, IRFL_OBJ_FLAGS);
         TRef detached = emitir(IRTI(IR_BAND), flags_ref, lj_ir_kint(J, GCOBJ_DETACHED));
         emitir(IRTGI(IR_EQ), detached, lj_ir_kint(J, 0));

         // Guard: object has a valid ptr (ptr != nullptr)
         TRef ptr_ref = emitir(IRT(IR_FLOAD, IRT_PTR), obj_ref, IRFL_OBJ_PTR);
         emitir(IRTG(IR_NE, IRT_PTR), ptr_ref, lj_ir_knull(J, IRT_PTR));

         if (field_flags & FD_STRING) {
            // String fields: lock, read CSTRING pointer, unlock, intern via jit_object_getstr.
            // Result is written to a TValue slot (may be string or nil for null CSTRING pointers).
            TRef tmp_ref = rec_tmpref(J, TREF_NIL, IRTMPREF_OUT1);
            TRef offset_ref = lj_ir_kint(J, field_offset);
            lj_ir_call(J, IRCALL_jit_object_getstr, obj_ref, offset_ref, tmp_ref);
            return lj_record_vload(J, tmp_ref, 0, (IRType)field_type);
         }

         if ((field_flags & (FD_OBJECT|FD_LOCAL)) and (field_flags & FD_POINTER)) {
            // Object pointer fields (FDF_OBJECT): lock parent, read OBJECTPTR, unlock, create
            // detached GCobject wrapper via jit_object_getobj.  load_include_for_class() is not
            // needed because the interpreter will have loaded class definitions in prior cycles.
            TRef tmp_ref = rec_tmpref(J, TREF_NIL, IRTMPREF_OUT1);
            TRef offset_ref = lj_ir_kint(J, field_offset);
            lj_ir_call(J, IRCALL_jit_object_getobj, obj_ref, offset_ref, tmp_ref);
            return lj_record_vload(J, tmp_ref, 0, (IRType)field_type);
         }

         // Numeric scalar fields: lock, XLOAD at offset, unlock.
         constexpr uint32_t supported_numeric = FD_DOUBLE|FD_INT64|FD_INT;
         if (not (field_flags & FD_POINTER)) {
            if (field_flags & supported_numeric) {
               TRef objptr = lj_ir_call(J, IRCALL_jit_object_lock, obj_ref);

               TRef addr = emitir(IRT(IR_ADD, IRT_PTR), objptr, lj_ir_kintp(J, field_offset));
               TRef val;

               if (field_flags & FD_DOUBLE) {
                  val = emitir(IRT(IR_XLOAD, IRT_NUM), addr, 0);
               }
               else if (field_flags & FD_INT64) {
                  val = emitir(IRT(IR_XLOAD, IRT_I64), addr, 0);
                  val = emitir(IRTN(IR_CONV), val, (IRT_NUM << IRCONV_DSH) | IRT_I64);
               }
               else { // FD_INT (signed int32) - guaranteed by supported_numeric check above
                  val = emitir(IRT(IR_XLOAD, IRT_INT), addr, 0);
                  if (not LJ_DUALNUM) {
                     val = emitir(IRTN(IR_CONV), val, IRCONV_NUM_INT);
                  }
               }

               lj_ir_call(J, IRCALL_jit_object_unlock, obj_ref);
               return val;
            }
         }
      }
   }

   // Fallback: call bc_object_getfield for complex types (arrays, structs, etc.)
   TRef tmp_ref = rec_tmpref(J, TREF_NIL, IRTMPREF_OUT1);
   TRef null_ref = lj_ir_kkptr(J, nullptr);  // No inline caching for JIT traces
   lj_ir_call(J, IRCALL_bc_object_getfield, obj_ref, key_ref, tmp_ref, null_ref);
   return lj_record_vload(J, tmp_ref, 0, (IRType)field_type);
}

static TRef rec_object_set(jit_State *J, RecordOps *ops)
{
   TRef obj_ref = ops->rb;
   if (not tref_isobject(obj_ref)) lj_trace_err(J, LJ_TRERR_BADTYPE);
   TRef key_ref = ops->rc;  // Get the string key reference - BC_OBGETF/OBSETF use ~RC for the constant index
   TRef val_ref = ops->ra;  // The value being written

   // Look up the field type from MetaClass definitions (write-side)
   GCobject *obj = objectV(ops->rbv());
   GCstr *key = strV(ops->rcv());

   int field_offset;
   uint32_t field_flags = 0;
   int field_type = ir_object_field_type_write(obj, key, field_offset, field_flags);

   if (field_type >= 0 and field_offset) {
      constexpr uint32_t unsupported = FD_ARRAY|FD_STRUCT|FD_UNSIGNED|FD_CPP;
      if (not (field_flags & unsupported)) {
         constexpr uint32_t supported_numeric = FD_DOUBLE|FD_INT64|FD_INT;
         if ((field_flags & supported_numeric) and not (field_flags & FD_POINTER) and tref_isnumber(val_ref)) {
            // Guard: object is alive (uid != 0)
            TRef uid_ref = emitir(IRTI(IR_FLOAD), obj_ref, IRFL_OBJ_UID);
            emitir(IRTGI(IR_NE), uid_ref, lj_ir_kint(J, 0));

            // Guard: object is not detached (flags & GCOBJ_DETACHED == 0)
            TRef flags_ref = emitir(IRT(IR_FLOAD, IRT_U8), obj_ref, IRFL_OBJ_FLAGS);
            TRef detached = emitir(IRTI(IR_BAND), flags_ref, lj_ir_kint(J, GCOBJ_DETACHED));
            emitir(IRTGI(IR_EQ), detached, lj_ir_kint(J, 0));

            // Guard: object has a valid ptr (ptr != nullptr)
            TRef ptr_ref = emitir(IRT(IR_FLOAD, IRT_PTR), obj_ref, IRFL_OBJ_PTR);
            emitir(IRTG(IR_NE, IRT_PTR), ptr_ref, lj_ir_knull(J, IRT_PTR));

            // Lock the object and get C++ pointer
            TRef objptr = lj_ir_call(J, IRCALL_jit_object_lock, obj_ref);

            // Compute the field address: object base + field offset
            TRef addr = emitir(IRT(IR_ADD, IRT_PTR), objptr, lj_ir_kintp(J, field_offset));

            // Convert the value to the target type and emit the store
            if (field_flags & FD_DOUBLE) {
               TRef store_val = val_ref;
               if (tref_isint(val_ref))
                  store_val = emitir(IRTN(IR_CONV), val_ref, IRCONV_NUM_INT);
               emitir(IRT(IR_XSTORE, IRT_NUM), addr, store_val);
            }
            else if (field_flags & FD_INT64) {
               TRef store_val;
               if (tref_isint(val_ref))
                  store_val = emitir(IRT(IR_CONV, IRT_I64), val_ref,
                     (IRT_I64 << IRCONV_DSH) | IRT_INT | IRCONV_SEXT);
               else
                  store_val = emitir(IRT(IR_CONV, IRT_I64), val_ref,
                     (IRT_I64 << IRCONV_DSH) | IRT_NUM | IRCONV_ANY);
               emitir(IRT(IR_XSTORE, IRT_I64), addr, store_val);
            }
            else { // FD_INT (signed int32) - guaranteed by supported_numeric check above
               TRef store_val = val_ref;
               if (tref_isnum(val_ref))
                  store_val = emitir(IRTI(IR_CONV), val_ref, IRCONV_INT_NUM | IRCONV_ANY);
               emitir(IRT(IR_XSTORE, IRT_INT), addr, store_val);
            }

            lj_ir_call(J, IRCALL_jit_object_unlock, obj_ref);
            return 0;
         }
      }
   }

   // Fallback: call bc_object_setfield for non-numeric types, virtual setters, etc.
   TRef tmp_ref = rec_tmpref(J, ops->ra, IRTMPREF_IN1);
   TRef null_ref = lj_ir_kkptr(J, nullptr);  // No inline caching for JIT traces
   lj_ir_call(J, IRCALL_bc_object_setfield, obj_ref, key_ref, tmp_ref, null_ref);
   return 0;
}

//********************************************************************************************************************
// Handle table access ops: BC_GGET, BC_GSET, BC_TGET*, BC_TSET*, BC_TNEW, BC_TDUP

static TRef rec_table_op(jit_State *J, RecordOps *ops, const BCIns *pc)
{
   TRef rc = ops->rc;
   BCOp op = ops->op;
   RecordIndex *ix = &ops->ix;

   switch (op) {
      case BC_GGET: case BC_GSET:
         settabV(J->L, &ix->tabv, tabref(J->fn->l.env));
         ix->tab = emitir(IRT(IR_FLOAD, IRT_TAB), getcurrf(J), IRFL_FUNC_ENV);
         ix->idxchain = LJ_MAX_IDXCHAIN;
         return lj_record_idx(J, ix);

      case BC_TGETB: case BC_TSETB:
         setintV(&ix->keyv, (int32_t)rc);
         ix->key = lj_ir_kint(J, (int32_t)rc);
         ix->idxchain = LJ_MAX_IDXCHAIN;
         return lj_record_idx(J, ix);

      case BC_TGETV: case BC_TGETS: case BC_TSETV: case BC_TSETS:
         ix->idxchain = LJ_MAX_IDXCHAIN;
         return lj_record_idx(J, ix);

      case BC_TGETR: case BC_TSETR:
         ix->idxchain = 0;
         return lj_record_idx(J, ix);

      case BC_TNEW:
         return rec_tnew(J, rc);

      case BC_TDUP: {
         TRef result = emitir(IRTG(IR_TDUP, IRT_TAB), lj_ir_ktab(J, gco_to_table(proto_kgc(J->pt, ~(ptrdiff_t)rc))), 0);
#ifdef LUAJIT_ENABLE_TABLE_BUMP
         J->rbchash[(result & (RBCHASH_SLOTS - 1))].ref = tref_ref(result);
         setmref(J->rbchash[(result & (RBCHASH_SLOTS - 1))].pc, pc);
         setgcref(J->rbchash[(result & (RBCHASH_SLOTS - 1))].pt, obj2gco(J->pt));
#else
         (void)pc;
#endif
         return result;
      }

      default:
         return 0;
   }
}

//********************************************************************************************************************
// Handle loop ops: BC_FORI, BC_FORL, BC_ITERL, BC_ITERN, BC_ITERA, BC_LOOP, BC_J*, BC_I*

static void rec_loop_op(jit_State *J, RecordOps *ops, const BCIns *pc)
{
   TRef ra = ops->ra, rb = ops->rb, rc = ops->rc;
   BCOp op = ops->op;

   switch (op) {
      case BC_FORI:
         if (rec_for(J, pc, 0) != LOOPEV_LEAVE) J->loopref = J->cur.nins;
         break;

      case BC_JFORI:
         lj_assertJ(bc_op(pc[(ptrdiff_t)rc - BCBIAS_J]) IS BC_JFORL, "JFORI does not point to JFORL");
         if (rec_for(J, pc, 0) != LOOPEV_LEAVE) lj_record_stop(J, TraceLink::ROOT, bc_d(pc[(ptrdiff_t)rc - BCBIAS_J]));
         break;

      case BC_FORL:
         rec_loop_interp(J, pc, rec_for(J, pc + ((ptrdiff_t)rc - BCBIAS_J), 1));
         break;

      case BC_ITERL:
         rec_loop_interp(J, pc, rec_iterl(J, *pc));
         break;

      case BC_ITERN:
         rec_loop_interp(J, pc, rec_itern(J, ra, rb));
         break;

      case BC_ITERA:
         rec_loop_interp(J, pc, rec_itera(J, ra, rb));
         break;

      case BC_LOOP:
         rec_loop_interp(J, pc, rec_loop(J, ra, 1));
         break;

      case BC_JFORL:
         rec_loop_jit(J, rc, rec_for(J, pc + bc_j(traceref(J, rc)->startins), 1));
         break;

      case BC_JITERL:
         rec_loop_jit(J, rc, rec_iterl(J, traceref(J, rc)->startins));
         break;

      case BC_JLOOP:
         rec_loop_jit(J, rc, rec_loop(J, ra, !bc_isret(bc_op(traceref(J, rc)->startins)) and bc_op(traceref(J, rc)->startins) != BC_ITERN and bc_op(traceref(J, rc)->startins) != BC_ITERA));
         break;

      case BC_IFORL:
      case BC_IITERL:
      case BC_ILOOP:
      case BC_IFUNCF:
      case BC_IFUNCV:
         lj_trace_err(J, LJ_TRERR_BLACKL);
         break;

      default:
         break;
   }
}

//********************************************************************************************************************
// Record the next bytecode instruction (_before_ it's executed).

void lj_record_ins(jit_State *J)
{
   cTValue *lbase;
   RecordOps ops;
   const BCIns *pc;

   // Perform post-processing action before recording the next instruction.
   if (not rec_handle_postproc(J)) return;

   // Need snapshot before recording next bytecode (e.g. after a store).
   if (J->needsnap) {
      J->needsnap = 0;
      if (J->pt) lj_snap_purge(J);
      lj_snap_add(J);
      J->mergesnap = 1;
   }

   // Skip some bytecodes.
   if (J->bcskip > 0) {
      J->bcskip--;
      return;
   }

   // Record only closed loops for root traces.
   pc = J->pc;
   if (FRC::at_root_depth(J) and (MSize)((char*)pc - (char*)J->bc_min) >= J->bc_extent) {
      lj_trace_err(J, LJ_TRERR_LLEAVE);
   }

#ifdef LUA_USE_ASSERT
   rec_check_slots(J);
   rec_check_ir(J);
#endif

   // Decode bytecode operands.
   lbase = J->L->base;
   ops.ins = *pc;
   ops.op = bc_op(ops.ins);
   rec_decode_operands(J, lbase, &ops);

   // Convenient aliases for the switch statement below.

#define ix    (ops.ix)
#define ins   (ops.ins)
#define ra    (ops.ra)
#define rb    (ops.rb)
#define rc    (ops.rc)
#define op    (ops.op)
#define rav   (ops.rav())
#define rbv   (ops.rbv())
#define rcv   (ops.rcv())

   switch (op) { // Comparison ops

   case BC_ISLT: case BC_ISGE: case BC_ISLE: case BC_ISGT:
      rec_comp_ordered(J, &ops);
      break;

   case BC_ISEQV: case BC_ISNEV:
   case BC_ISEQS: case BC_ISNES:
   case BC_ISEQN: case BC_ISNEN:
   case BC_ISEQP: case BC_ISNEP:
      rec_comp_equality(J, &ops);
      break;

      // Unary test and copy ops

   case BC_ISTC: case BC_ISFC:
      if ((op & 1) IS tref_istruecond(rc)) rc = 0;  //  Don't store if condition is not true.
      [[fallthrough]];
   case BC_IST: case BC_ISF:  //  Type specialisation suffices.
      if (bc_a(pc[1]) < J->maxslot) J->maxslot = bc_a(pc[1]);  //  Shrink used slots.
      break;

   case BC_ISTYPE: case BC_ISNUM:
      // These coercions need to correspond with lj_meta_istype().
      if (LJ_DUALNUM and rc IS ~LJ_TNUMX + 1) ra = lj_opt_narrow_toint(J, ra);
      else if (rc IS ~LJ_TNUMX + 2) ra = lj_ir_tonum(J, ra);
      else if (rc IS ~LJ_TSTR + 1) ra = lj_ir_tostr(J, ra);
      // else: type specialisation suffices.
      J->base[bc_a(ins)] = ra;
      break;

   case BC_ISEMPTYARR: {
      // Empty array check for ?? operator.
      // If value is an array, we must guard on its length being 0 or non-zero.
      // For non-array types, type specialisation suffices (they are always truthy for this check).
      if (bc_a(pc[1]) < J->maxslot) J->maxslot = bc_a(pc[1]);  // Shrink used slots.
      if (tref_isarray(ra)) {
         // Load array length and compare to 0
         TRef arrlen = emitir(IRTI(IR_FLOAD), ra, IRFL_ARRAY_LEN);
         TRef zero = lj_ir_kint(J, 0);
         // Determine if array is empty at recording time
         GCarray *arr = arrayV(rav);
         int is_empty = (arr->len IS 0);
         rec_comp_prep(J);
         // Emit EQ comparison (arrlen == 0)
         // If array was empty when recorded, guard that it stays empty
         // If array was non-empty when recorded, guard that it stays non-empty
         emitir(IRTG(is_empty ? IR_EQ : IR_NE, IRT_INT), arrlen, zero);
         rec_comp_fixup(J, J->pc, is_empty);
      }
      // For non-arrays, no additional guard needed - type specialisation handles it
      break;
   }

   // Unary ops

   case BC_NOT:
      // Type specialisation already forces const result.
      rc = tref_istruecond(rc) ? TREF_FALSE : TREF_TRUE;
      break;

   case BC_LEN:
      if (tref_isstr(rc)) rc = emitir(IRTI(IR_FLOAD), rc, IRFL_STR_LEN);
      else rc = rec_mm_len(J, rc, rcv);
      break;

   // Arithmetic ops

   case BC_UNM:
   case BC_ADDNV: case BC_SUBNV: case BC_MULNV: case BC_DIVNV: case BC_MODNV:
   case BC_ADDVN: case BC_SUBVN: case BC_MULVN: case BC_DIVVN:
   case BC_ADDVV: case BC_SUBVV: case BC_MULVV: case BC_DIVVV:
   case BC_MODVN: case BC_MODVV:
   case BC_POW:
      rc = rec_arith_op(J, &ops);
      break;

   // Miscellaneous ops

   case BC_CAT:
      rc = rec_cat(J, rb, rc);
      break;

   // Constant and move ops

   case BC_MOV:
      // Clear gap of method call to avoid resurrecting previous refs.
      if (ra > J->maxslot) {
         SlotView slots(J);
         slots.clear_range(slots.maxslot(), ra - slots.maxslot());
      }
      break;

   case BC_KSTR: case BC_KNUM: case BC_KPRI:
      break;

   case BC_KSHORT:
      rc = lj_ir_kint(J, (int32_t)(int16_t)rc);
      break;

   case BC_KNIL:
      {
         SlotView slots(J);
         if (ra > slots.maxslot()) slots.clear(ra - 1);
         while (ra <= rc) slots[ra++] = TREF_NIL;
         if (rc >= slots.maxslot()) slots.set_maxslot(rc + 1);
      }
      break;

   // Upvalue and function ops

   case BC_UGET:
      rc = rec_upvalue(J, rc, 0);
      break;

   case BC_USETV: case BC_USETS: case BC_USETN: case BC_USETP:
      rec_upvalue(J, ra, rc);
      break;

      // Table ops

   case BC_GGET: case BC_GSET:
   case BC_TGETB: case BC_TSETB:
   case BC_TGETV: case BC_TGETS: case BC_TSETV: case BC_TSETS:
   case BC_TGETR: case BC_TSETR:
   case BC_TNEW:
   case BC_TDUP:
      rc = rec_table_op(J, &ops, pc);
      break;

   case BC_TSETM:
      rec_tsetm(J, ra, (BCREG)(J->L->top - J->L->base), (int32_t)rcv->u32.lo);
      break;

   // Array ops - native array access
   case BC_AGETV: case BC_AGETB:
      rc = rec_array_op(J, &ops);
      break;

   case BC_ASETV: case BC_ASETB:
      rec_array_op(J, &ops);
      break;

   // Object ops - native object field access

   case BC_OBGETF:
      rc = rec_object_get(J, &ops);
      break;

   case BC_OBSETF:
      rec_object_set(J, &ops);
      break;

   // Calls and vararg handling

   case BC_ITERC:
      {
         SlotView slots(J);
         slots[ra] = getslot(J, ra - 3);
         slots[ra + FRC::HEADER_SIZE] = getslot(J, ra - 2);
         slots[ra + FRC::HEADER_SIZE + 1] = getslot(J, ra - 1);
         // Do the actual copy now because lj_record_call needs the values.
         TValue* b = &J->L->base[ra];
         copyTV(J->L, b, b - 3);
         copyTV(J->L, b + FRC::HEADER_SIZE, b - 2);
         copyTV(J->L, b + FRC::HEADER_SIZE + 1, b - 1);
      }
      lj_record_call(J, ra, (ptrdiff_t)rc - 1);
      break;

      // L->top is set to L->base+ra+rc+NARGS-1+1. See lj_dispatch_ins().
   case BC_CALLM:
      rc = (BCREG)(J->L->top - J->L->base) - ra - 1;
      [[fallthrough]];

   case BC_CALL:
      lj_record_call(J, ra, (ptrdiff_t)rc - 1);
      break;

   case BC_CALLMT:
      rc = (BCREG)(J->L->top - J->L->base) - ra - 1;
      [[fallthrough]];

   case BC_CALLT:
      lj_record_tailcall(J, ra, (ptrdiff_t)rc - 1);
      break;

   case BC_VARG:
      rec_varg(J, ra, (ptrdiff_t)rb - 1);
      break;

      // Returns

   case BC_RETM:
      // L->top is set to L->base+ra+rc+NRESULTS-1, see lj_dispatch_ins().
      rc = (BCREG)(J->L->top - J->L->base) - ra + 1;
      [[fallthrough]];

   case BC_RET: case BC_RET0: case BC_RET1:
      lj_record_ret(J, ra, (ptrdiff_t)rc - 1);
      break;

      // Type fixing

   case BC_TYPEFIX: {
      // BC_TYPEFIX is a one-time operation that mutates the prototype.
      // After first execution, it becomes a no-op. For JIT recording:
      // - If types are already fixed (common case), treat as no-op
      // - If types not fixed, skip during recording (mutation is safe for interpreter)

      GCproto *pt = funcproto(curr_func(J->L));
      if (pt->result_types[0] IS TiriType::Unknown) {
         // Types not yet fixed.  We can leave it for the interpreter and keep recording
         // TODO: Need to consider if it is viable to mutate the function prototype (set the result types) here during recording.
         // It could also be considered a red-flag if the interpreter hasn't mutated the function by this point, even if only
         // setting the result type to TiriType::Any.
         break;
      }
      else break; // Types already fixed - no-op, continue recording
   }

      // Loops and branches

   case BC_FORI:
   case BC_JFORI:
   case BC_FORL:
   case BC_ITERL:
   case BC_ITERN:
   case BC_ITERA:
   case BC_LOOP:
   case BC_JFORL:
   case BC_JITERL:
   case BC_JLOOP:
   case BC_IFORL:
   case BC_IITERL:
   case BC_ILOOP:
   case BC_IFUNCF:
   case BC_IFUNCV:
      rec_loop_op(J, &ops, pc);
      break;

   case BC_JMP:
      if (ra < J->maxslot) J->maxslot = ra;  //  Shrink used slots.
      break;

   case BC_ISNEXT:
      rec_isnext(J, ra);
      break;

   case BC_ISARR:
      rec_isarr(J, ra);
      break;

      // Function headers

   case BC_FUNCF:
      rec_func_lua(J);
      break;

   case BC_JFUNCF:
      rec_func_jit(J, rc);
      break;

   case BC_FUNCV:
      rec_func_vararg(J);
      rec_func_lua(J);
      break;
   case BC_JFUNCV:
      // Cannot happen. No hotcall counting for varag funcs.
      lj_assertJ(0, "unsupported vararg hotcall");
      break;

   case BC_FUNCC:
   case BC_FUNCCW:
      lj_ffrecord_func(J);
      break;

   case BC_TRYENTER: {
      // Inlined frames use a virtual base pointer. Compute the correct base below so
      // try-enter can still record properly inside inlined calls.

      // Add snapshot before try block to enable on-trace error catching.
      // This allows the JIT to exit at this point when an exception occurs,
      // letting the interpreter handle the try-except recovery.

      uint16_t try_index = (uint16_t)bc_d(ins);
      lj_snap_add(J);

      // Emit call to lj_try_enter(L, Func, Base, TryBlockIndex)
      // L is implicit (CCI_L flag)
      // Func is the current function for this frame (may differ from J->fn)
      // Base must point at the current frame. For inlined frames, REF_BASE is the root base,
      // so add the baseslot offset to reach the virtual frame base.

      TRef tr_func = getcurrf(J);
      TRef tr_base = REF_BASE;
      if (J->baseslot > FRC::MIN_BASESLOT) {
         IRBuilder ir(J);
         int32_t slot_delta = (int32_t)J->baseslot - (int32_t)FRC::MIN_BASESLOT;
         int32_t byte_delta = slot_delta * 8;
         tr_base = ir.emit(IRT(IR_ADD, IRT_PGC), REF_BASE, ir.kint(byte_delta));
      }
      TRef tr_index = lj_ir_kint(J, (int32_t)try_index);
      lj_ir_call(J, IRCALL_lj_try_enter, tr_func, tr_base, tr_index);

      J->needsnap = 1; // Snapshot after try_enter to create a restore point for trace exits
      break;
   }

   case BC_TRYLEAVE: {
      // Emit call to lj_try_leave(L)
      // Note: vm_exit_interp in vm_x64.dasc must correctly dispatch BC_TRYLEAVE to its handler,
      // not to the C function path. See the bytecode dispatch logic after trace exit.
      lj_ir_call(J, IRCALL_lj_try_leave); // L is implicit (CCI_L flag), no explicit args needed
      J->needsnap = 1; // Snapshot after try_leave to mark the end of the try block scope
      break;
   }

   case BC_CHECK:
   case BC_RAISE:
      // These bytecodes throw exceptions and cannot be compiled into traces.
      // Exit to interpreter to handle them. This avoids trace abort and ensures
      // clean handoff without corrupting interpreter state.
      lj_snap_add(J);
      lj_record_stop(J, TraceLink::INTERP, 0);
      break;

   default:
      if (op >= BC__MAX) {
         lj_ffrecord_func(J);
         break;
      }
      [[fallthrough]];
   case BC_UCLO:
   case BC_FNEW:
      setintV(&J->errinfo, (int32_t)op);
      lj_trace_err_info(J, LJ_TRERR_NYIBC);
      break;
   }

   // rc IS 0 if we have no result yet, e.g. pending __index metamethod call.
   if (bcmode_a(op) IS BCMdst and rc) {
      SlotView slots(J);
      slots[ra] = rc;
      if (ra >= slots.maxslot()) {
         if (ra > slots.maxslot()) slots.clear(ra - 1);
         slots.set_maxslot(ra + 1);
      }
   }

#undef ix
#undef ins
#undef ra
#undef rb
#undef rc
#undef op
#undef rav
#undef rbv
#undef rcv

   // Limit the number of recorded IR instructions and constants.
   if (J->cur.nins > REF_FIRST + (IRRef)J->param[JIT_P_maxrecord] or
      J->cur.nk < REF_BIAS - (IRRef)J->param[JIT_P_maxirconst])
      lj_trace_err(J, LJ_TRERR_TRACEOV);
}

//********************************************************************************************************************
// Setup recording for a root trace started by a hot loop.

static const BCIns *rec_setup_root(jit_State *J)
{
   // Determine the next PC and the bytecode range for the loop.
   const BCIns *pcj, * pc = J->pc;
   BCIns ins = *pc;
   BCREG ra = bc_a(ins);
   switch (bc_op(ins)) {
      case BC_FORL:
         J->bc_extent = (MSize)(-bc_j(ins)) * sizeof(BCIns);
         pc += 1 + bc_j(ins);
         J->bc_min = pc;
         break;
      case BC_ITERL:
         if (bc_op(pc[-1]) IS BC_JLOOP) lj_trace_err(J, LJ_TRERR_LINNER);
         lj_assertJ(bc_op(pc[-1]) IS BC_ITERC, "no ITERC before ITERL");
         J->maxslot = ra + bc_b(pc[-1]) - 1;
         J->bc_extent = (MSize)(-bc_j(ins)) * sizeof(BCIns);
         pc += 1 + bc_j(ins);
         lj_assertJ(bc_op(pc[-1]) IS BC_JMP, "ITERL does not point to JMP+1");
         J->bc_min = pc;
         break;
      case BC_ITERN:
         lj_assertJ(bc_op(pc[1]) IS BC_ITERL, "no ITERL after ITERN");
         J->maxslot = ra;
         J->bc_extent = (MSize)(-bc_j(pc[1])) * sizeof(BCIns);
         J->bc_min = pc + 2 + bc_j(pc[1]);
         J->state = TraceState::RECORD_1ST;  //  Record the first ITERN, too.
         break;
      case BC_ITERA:
         lj_assertJ(bc_op(pc[1]) IS BC_ITERL, "no ITERL after ITERA");
         J->maxslot = ra;
         J->bc_extent = (MSize)(-bc_j(pc[1])) * sizeof(BCIns);
         J->bc_min = pc + 2 + bc_j(pc[1]);
         J->state = TraceState::RECORD_1ST;  //  Record the first ITERA, too.
         break;
      case BC_LOOP:
         // Only check BC range for real loops, but not for "repeat until true".
         pcj = pc + bc_j(ins);
         ins = *pcj;
         if (bc_op(ins) IS BC_JMP and bc_j(ins) < 0) {
            J->bc_min = pcj + 1 + bc_j(ins);
            J->bc_extent = (MSize)(-bc_j(ins)) * sizeof(BCIns);
         }
         J->maxslot = ra;
         pc++;
         break;
      case BC_RET:
      case BC_RET0:
      case BC_RET1:
         // No bytecode range check for down-recursive root traces.
         J->maxslot = ra + bc_d(ins) - 1;
         break;
      case BC_FUNCF:
         // No bytecode range check for root traces started by a hot call.
         J->maxslot = J->pt->numparams;
         pc++;
         break;
      case BC_CALLM:
      case BC_CALL:
      case BC_ITERC:
         // No bytecode range check for stitched traces.
         pc++;
         break;
      default:
         lj_assertJ(0, "bad root trace start bytecode %d", bc_op(ins));
         break;
   }
   return pc;
}

//********************************************************************************************************************
// Setup for recording a new trace.

void lj_record_setup(jit_State *J)
{
   uint32_t i;

   // Initialise state related to current trace.
   memset(J->slot, 0, sizeof(J->slot));
   memset(J->chain, 0, sizeof(J->chain));
#ifdef LUAJIT_ENABLE_TABLE_BUMP
   memset(J->rbchash, 0, sizeof(J->rbchash));
#endif
   memset(J->bpropcache, 0, sizeof(J->bpropcache));
   J->scev.idx = REF_NIL;
   setmref(J->scev.pc, nullptr);

   J->baseslot   = FRC::MIN_BASESLOT;  //  Invoking function is at base[FRC::FUNC_SLOT_OFFSET].
   J->base       = J->slot + J->baseslot;
   J->maxslot    = 0;
   J->framedepth = 0;
   J->retdepth   = 0;
   J->instunroll = J->param[JIT_P_instunroll];
   J->loopunroll = J->param[JIT_P_loopunroll];
   J->tailcalled = 0;
   J->loopref    = 0;
   J->bc_min     = nullptr;  //  Means no limit.
   J->bc_extent  = ~(MSize)0;

   // Emit instructions for fixed references. Also triggers initial IR alloc.
   emitir_raw(IRT(IR_BASE, IRT_PGC), J->parent, J->exitno);
   for (i = 0; i <= 2; i++) {
      IRIns *ir = IR(REF_NIL - i);
      ir->i     = 0;
      ir->t.irt = (uint8_t)(IRT_NIL + i);
      ir->o     = IR_KPRI;
      ir->prev  = 0;
   }
   J->cur.nk = REF_TRUE;

   J->startpc = J->pc;
   setmref(J->cur.startpc, J->pc);
   if (J->parent) {  // Side trace.
      GCtrace *T = traceref(J, J->parent);
      TraceNo root = T->root ? T->root : J->parent;
      J->cur.root = (uint16_t)root;
      J->cur.startins = BCINS_AD(BC_JMP, 0, 0);
      // Check whether we could at least potentially form an extra loop.
      if (J->exitno IS 0 and T->snap[0].nent IS 0) {
         // We can narrow a FORL for some side traces, too.
         if (J->pc > proto_bc(J->pt) and bc_op(J->pc[-1]) IS BC_JFORI and
            bc_d(J->pc[bc_j(J->pc[-1]) - 1]) IS root) {
            lj_snap_add(J);
            rec_for_loop(J, J->pc - 1, &J->scev, 1);
            goto sidecheck;
         }
      }
      else J->startpc = nullptr;  //  Prevent forming an extra loop.

      lj_snap_replay(J, T);
   sidecheck:
      if ((traceref(J, J->cur.root)->nchild >= J->param[JIT_P_maxside] or
            T->snap[J->exitno].count >= J->param[JIT_P_hotexit] + J->param[JIT_P_tryside])) {
         if (bc_op(*J->pc) IS BC_JLOOP) {
            BCIns startins = traceref(J, bc_d(*J->pc))->startins;
            if (bc_op(startins) IS BC_ITERN) rec_itern(J, bc_a(startins), bc_b(startins));
            else if (bc_op(startins) IS BC_ITERA) rec_itera(J, bc_a(startins), bc_b(startins));
         }
         lj_record_stop(J, TraceLink::INTERP, 0);
      }
   }
   else {  // Root trace.
      J->cur.root = 0;
      J->cur.startins = *J->pc;
      J->pc = rec_setup_root(J);

      // Note: the loop instruction itself is recorded at the end and not
      // at the start! So snapshot #0 needs to point to the *next* instruction.
      // The exceptions are BC_ITERN and BC_ITERA, which set LJ_TRACE_RECORD_1ST.

      lj_snap_add(J);

      if (bc_op(J->cur.startins) IS BC_FORL) rec_for_loop(J, J->pc - 1, &J->scev, 1);
      else if (bc_op(J->cur.startins) IS BC_ITERC) J->startpc = nullptr;

      if (1 + J->pt->framesize >= LJ_MAX_JSLOTS) lj_trace_err(J, LJ_TRERR_STACKOV);
   }

#ifdef LUAJIT_ENABLE_CHECKHOOK
   // Regularly check for instruction/line hooks from compiled code and
   // exit to the interpreter if the hooks are set.
   //
   // This is a compile-time option and disabled by default, since the
   // hook checks may be quite expensive in tight loops.
   //
   // Note this is only useful if hooks are *not* set most of the time.
   // Use this only if you want to *asynchronously* interrupt the execution.
   //
   // You can set the instruction hook via lua_sethook() with a count of 1
   // from a signal handler or another native thread. Please have a look
   // at the first few functions in luajit.c for an example (Ctrl-C handler).

   {
      TRef tr = emitir(IRT(IR_XLOAD, IRT_U8), lj_ir_kptr(J, &J2G(J)->hookmask), IRXLOAD_VOLATILE);
      tr = emitir(IRTI(IR_BAND), tr, lj_ir_kint(J, (LUA_MASKLINE | LUA_MASKCOUNT)));
      emitir(IRTGI(IR_EQ), tr, lj_ir_kint(J, 0));
   }
#endif
}

#undef IR
#undef emitir_raw
#undef emitir
