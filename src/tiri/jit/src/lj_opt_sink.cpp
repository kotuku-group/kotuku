/*
** SINK: Allocation Sinking and Store Sinking.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_opt_sink_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_target.h"

// Some local macros to save typing. Undef'd at the end.
#define IR(ref)      (&J->cur.ir[(ref)])

// Check whether the store ref points to an eligible allocation.
static IRIns* sink_checkalloc(jit_State* J, IRIns* irs)
{
   IRIns* ir = IR(irs->op1);
   if (!irref_isk(ir->op2))
      return nullptr;  //  Non-constant key.
   if (ir->o == IR_HREFK or ir->o == IR_AREF)
      ir = IR(ir->op1);
   else if (!(ir->o == IR_HREF or ir->o == IR_NEWREF ||
      ir->o == IR_FREF or ir->o == IR_ADD))
      return nullptr;  //  Unhandled reference type (for XSTORE).
   ir = IR(ir->op1);
   if (!(ir->o == IR_TNEW or ir->o == IR_TDUP or ir->o == IR_CNEW))
      return nullptr;  //  Not an allocation.
   return ir;  //  Return allocation.
}

// Recursively check whether a value depends on a PHI.
static int sink_phidep(jit_State* J, IRRef ref, int* workp)
{
   IRIns* ir = IR(ref);
   if (!*workp) return 1;  //  Give up and pretend it does.
   (*workp)--;
   if (irt_isphi(ir->t)) return 1;
   if (ir->op1 >= REF_FIRST and sink_phidep(J, ir->op1, workp)) return 1;
   if (ir->op2 >= REF_FIRST and sink_phidep(J, ir->op2, workp)) return 1;
   return 0;
}

// Check whether a value is a sinkable PHI or loop-invariant.
static int sink_checkphi(jit_State* J, IRIns* ira, IRRef ref)
{
   if (ref >= REF_FIRST) {
      IRIns* ir = IR(ref);
      if (irt_isphi(ir->t) or (ir->o == IR_CONV and ir->op2 == IRCONV_NUM_INT &&
         irt_isphi(IR(ir->op1)->t))) {
         ira->prev++;
         return 1;  //  Sinkable PHI.
      }
      // Otherwise the value must be loop-invariant.
      if (ref < J->loopref) {
         // Check for PHI dependencies, but give up after reasonable effort.
         int work = 64;
         return !sink_phidep(J, ref, &work);
      }
      else {
         return 0;  //  Loop-variant.
      }
   }
   return 1;  //  Constant (non-PHI).
}

/* Mark non-sinkable allocations using single-pass backward propagation.
**
** Roots for the marking process are:
** - Some PHIs or snapshots (see below).
** - Non-PHI, non-constant values stored to PHI allocations.
** - All guards.
** - Any remaining loads not eliminated by store-to-load forwarding.
** - Stores with non-constant keys.
** - All stored values.
*/
static void sink_mark_ins(jit_State* J)
{
   IRIns* ir, * irlast = IR(J->cur.nins - 1);
   for (ir = irlast; ; ir--) {
      switch (ir->o) {
      case IR_BASE:
         return;  //  Finished.
      case IR_ALOAD: case IR_HLOAD: case IR_XLOAD: case IR_TBAR: case IR_ALEN:
         irt_setmark(IR(ir->op1)->t);  //  Mark ref for remaining loads.
         break;
      case IR_FLOAD:
         if (irt_ismarked(ir->t) or ir->op2 == IRFL_TAB_META)
            irt_setmark(IR(ir->op1)->t);  //  Mark table for remaining loads.
         break;
      case IR_ASTORE: case IR_HSTORE: case IR_FSTORE: case IR_XSTORE: {
         IRIns* ira = sink_checkalloc(J, ir);
         if (!ira or (irt_isphi(ira->t) and !sink_checkphi(J, ira, ir->op2)))
            irt_setmark(IR(ir->op1)->t);  //  Mark ineligible ref.
         irt_setmark(IR(ir->op2)->t);  //  Mark stored value.
         break;
      }
      case IR_USTORE:
         irt_setmark(IR(ir->op2)->t);  //  Mark stored value.
         break;
      case IR_CALLS:
         irt_setmark(IR(ir->op1)->t);  //  Mark (potentially) stored values.
         break;
      case IR_PHI: {
         IRIns* irl = IR(ir->op1), * irr = IR(ir->op2);
         irl->prev = irr->prev = 0;  //  Clear PHI value counts.
         if (irl->o == irr->o && (irl->o == IR_TNEW or irl->o == IR_TDUP)) break;
         irt_setmark(irl->t);
         irt_setmark(irr->t);
         break;
      }
      default:
         if (irt_ismarked(ir->t) or irt_isguard(ir->t)) {  // Propagate mark.
            if (ir->op1 >= REF_FIRST) irt_setmark(IR(ir->op1)->t);
            if (ir->op2 >= REF_FIRST) irt_setmark(IR(ir->op2)->t);
         }
         break;
      }
   }
}

// Mark all instructions referenced by a snapshot.
static void sink_mark_snap(jit_State* J, SnapShot* snap)
{
   SnapEntry* map = &J->cur.snapmap[snap->mapofs];
   MSize n, nent = snap->nent;
   for (n = 0; n < nent; n++) {
      IRRef ref = snap_ref(map[n]);
      if (!irref_isk(ref))
         irt_setmark(IR(ref)->t);
   }
}

// Iteratively remark PHI refs with differing marks or PHI value counts.
static void sink_remark_phi(jit_State* J)
{
   IRIns* ir;
   int remark;
   do {
      remark = 0;
      for (ir = IR(J->cur.nins - 1); ir->o == IR_PHI; ir--) {
         IRIns* irl = IR(ir->op1), * irr = IR(ir->op2);
         if (!((irl->t.irt ^ irr->t.irt) & IRT_MARK) and irl->prev == irr->prev)
            continue;
         remark |= (~(irl->t.irt & irr->t.irt) & IRT_MARK);
         irt_setmark(IR(ir->op1)->t);
         irt_setmark(IR(ir->op2)->t);
      }
   } while (remark);
}

// Sweep instructions and tag sunken allocations and stores.
static void sink_sweep_ins(jit_State* J)
{
   IRIns* ir, * irbase = IR(REF_BASE);
   for (ir = IR(J->cur.nins - 1); ir >= irbase; ir--) {
      switch (ir->o) {
      case IR_ASTORE: case IR_HSTORE: case IR_FSTORE: case IR_XSTORE: {
         IRIns* ira = sink_checkalloc(J, ir);
         if (ira and !irt_ismarked(ira->t)) {
            int delta = (int)(ir - ira);
            ir->prev = REGSP(RID_SINK, delta > 255 ? 255 : delta);
         }
         else {
            ir->prev = REGSP_INIT;
         }
         break;
      }
      case IR_NEWREF:
         if (!irt_ismarked(IR(ir->op1)->t)) {
            ir->prev = REGSP(RID_SINK, 0);
         }
         else {
            irt_clearmark(ir->t);
            ir->prev = REGSP_INIT;
         }
         break;
      case IR_TNEW: case IR_TDUP:
         if (!irt_ismarked(ir->t)) {
            ir->t.irt &= ~IRT_GUARD;
            ir->prev = REGSP(RID_SINK, 0);
            J->cur.sinktags = 1;  //  Signal present SINK tags to assembler.
         }
         else {
            irt_clearmark(ir->t);
            ir->prev = REGSP_INIT;
         }
         break;
      case IR_PHI: {
         IRIns* ira = IR(ir->op2);
         if (!irt_ismarked(ira->t) && (ira->o == IR_TNEW or ira->o == IR_TDUP)) {
            ir->prev = REGSP(RID_SINK, 0);
         }
         else ir->prev = REGSP_INIT;
         break;
      }
      default:
         irt_clearmark(ir->t);
         ir->prev = REGSP_INIT;
         break;
      }
   }
   for (ir = IR(J->cur.nk); ir < irbase; ir++) {
      irt_clearmark(ir->t);
      ir->prev = REGSP_INIT;
      // The false-positive of irt_is64() for ASMREF_L (REF_NIL) is OK here.
      if (irt_is64(ir->t) and ir->o != IR_KNULL)
         ir++;
   }
}

/* Allocation sinking and store sinking.
**
** 1. Mark all non-sinkable allocations.
** 2. Then sink all remaining allocations and the related stores.
*/

void lj_opt_sink(jit_State* J)
{
   const uint32_t need = (JIT_F_OPT_SINK | JIT_F_OPT_FWD |
      JIT_F_OPT_DCE | JIT_F_OPT_CSE | JIT_F_OPT_FOLD);
   if ((J->flags & need) == need and (J->chain[IR_TNEW] or J->chain[IR_TDUP])) {
      if (!J->loopref) sink_mark_snap(J, &J->cur.snap[J->cur.nsnap - 1]);
      sink_mark_ins(J);
      if (J->loopref) sink_remark_phi(J);
      sink_sweep_ins(J);
   }
}

#undef IR

#endif
