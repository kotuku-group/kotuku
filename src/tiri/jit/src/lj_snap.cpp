/*
** Snapshot handling.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
**
** Snapshots capture the interpreter state at specific points during trace recording.
** When a trace exits (guard fails), the snapshot is used to restore the interpreter
** state so execution can continue correctly.
**
** SNAPSHOT STRUCTURE:
** Each snapshot (SnapShot) contains:
**   - mapofs: Offset into snapmap where this snapshot's entries begin
**   - nent:   Number of slot entries (NOT including frame links)
**   - ref:    IR reference at which this snapshot was created
**   - nslots: Total number of stack slots
**   - topslot: Top slot for stack sizing
**
** SNAPMAP LAYOUT:
** The snapmap is a contiguous array of SnapEntry values. For each snapshot:
**   snapmap[mapofs + 0..nent-1] = Slot entries (which slots to restore and their IR refs)
**   snapmap[mapofs + nent..nent+1] = PC + frame links (64-bit value packed as 2 SnapEntry)
**
** The PC is stored as: pcbase = (pc_pointer << 8) | (baseslot - 2)
** Use snap_pc() to extract the PC pointer from the frame links.
**
** IMPORTANT INVARIANTS:
** - Snapshots are stored contiguously: snap[N+1].mapofs >= snap[N].mapofs + snap[N].nent + 2
** - The loop snapshot (last snapshot before loop optimization) may have its PC replaced
**   with a sentinel value during lj_opt_loop processing
** - lj_snap_shrink() can reduce nent and move the PC data, updating nsnapmap accordingly
*/

#define lj_snap_c
#define LUA_CORE

#include <kotuku/main.h>
#include "lj_obj.h"

#include "lj_gc.h"
#include "lj_tab.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_target.h"

// Pass IR on to next optimization in chain (FOLD).
#define emitir(ot, a, b)   (lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

// Emit raw IR without passing through optimizations.
#define emitir_raw(ot, a, b)   (lj_ir_set(J, (ot), (a), (b)), lj_ir_emit(J))

//********************************************************************************************************************
// Snapshot buffer allocation

// Grow snapshot buffer.

void lj_snap_grow_buf_(jit_State* J, MSize need)
{
   MSize maxsnap = (MSize)J->param[JIT_P_maxsnap];
   if (need > maxsnap) lj_trace_err(J, LJ_TRERR_SNAPOV);
   lj_mem_growvec(J->L, J->snapbuf, J->sizesnap, maxsnap, SnapShot);
   J->cur.snap = J->snapbuf;
}

// Grow snapshot map buffer.

void lj_snap_grow_map_(jit_State* J, MSize need)
{
   if (need < 2 * J->sizesnapmap) need = 2 * J->sizesnapmap;
   else if (need < 64) need = 64;
   J->snapmapbuf = (SnapEntry*)lj_mem_realloc(J->L, J->snapmapbuf, J->sizesnapmap * sizeof(SnapEntry), need * sizeof(SnapEntry));
   J->cur.snapmap = J->snapmapbuf;
   J->sizesnapmap = need;
}

//********************************************************************************************************************
// Snapshot generation

// Add all modified slots to the snapshot.

static MSize snapshot_slots(jit_State *J, SnapEntry *map, BCREG nslots)
{
   IRRef retf = J->chain[IR_RETF];  //  Limits SLOAD restore elimination.
   BCREG s;
   MSize n = 0;
   for (s = 0; s < nslots; s++) {
      TRef tr = J->slot[s];
      IRRef ref = tref_ref(tr);
      if (s IS 1) {  // Ignore slot 1 except if tailcalled.
         if ((tr & TREF_FRAME)) map[n++] = SNAP(1, SNAP_FRAME | SNAP_NORESTORE, REF_NIL);
         continue;
      }

      if ((tr & (TREF_FRAME | TREF_CONT)) and !ref) {
         cTValue* base = J->L->base - J->baseslot;
         tr = J->slot[s] = (tr & 0xff0000) | lj_ir_k64(J, IR_KNUM, base[s].u64);
         ref = tref_ref(tr);
      }

      if (ref) {
         SnapEntry sn = SNAP_TR(s, tr);
         IRIns* ir = &J->cur.ir[ref];
         if ((!(sn & (SNAP_CONT | SNAP_FRAME))) and ir->o IS IR_SLOAD and ir->op1 IS s and ref > retf) {
            // No need to snapshot unmodified non-inherited slots.
            // But always snapshot the function below a frame

            if (!(ir->op2 & IRSLOAD_INHERIT) and (!LJ_FR2 or s IS 0 or s + 1 IS nslots or
                  !(J->slot[s + 1] & (TREF_CONT | TREF_FRAME))))
               continue;
            // No need to restore readonly slots and unmodified non-parent slots.
            if (!(LJ_DUALNUM and (ir->op2 & IRSLOAD_CONVERT))  and
               (ir->op2 & (IRSLOAD_READONLY | IRSLOAD_PARENT)) != IRSLOAD_PARENT)
               sn |= SNAP_NORESTORE;
         }
         map[n++] = sn;
      }
   }
   return n;
}

//********************************************************************************************************************
// Add frame links at the end of the snapshot.

static MSize snapshot_framelinks(jit_State* J, SnapEntry* map, uint8_t* topslot)
{
   cTValue* frame = J->L->base - 1;
   cTValue* lim = J->L->base - J->baseslot + LJ_FR2;
   GCfunc* fn = frame_func(frame);
   cTValue* ftop = isluafunc(fn) ? (frame + funcproto(fn)->framesize) : J->L->top;
   uint64_t pcbase = (u64ptr(J->pc) << 8) | (J->baseslot - 2);
   lj_assertJ(2 <= J->baseslot and J->baseslot <= 257, "bad baseslot");
   memcpy(map, &pcbase, sizeof(uint64_t));

   lj_assertJ(!J->pt or (J->pc >= proto_bc(J->pt) and J->pc < proto_bc(J->pt) + J->pt->sizebc), "bad snapshot PC");
   while (frame > lim) {  // Backwards traversal of all frames above base.
      if (frame_islua(frame)) frame = frame_prevl(frame);
      else if (frame_iscont(frame)) frame = frame_prevd(frame);
      else {
         lj_assertJ(!frame_isc(frame), "broken frame chain");
         frame = frame_prevd(frame);
         continue;
      }

      if (frame + funcproto(frame_func(frame))->framesize > ftop)
         ftop = frame + funcproto(frame_func(frame))->framesize;
   }
   *topslot = (uint8_t)(ftop - lim);
   lj_assertJ(sizeof(SnapEntry) * 2 IS sizeof(uint64_t), "bad SnapEntry def");
   return 2;
}

//********************************************************************************************************************
// Take a snapshot of the current stack.

static void snapshot_stack(jit_State* J, SnapShot* snap, MSize nsnapmap)
{
   BCREG nslots = J->baseslot + J->maxslot;
   MSize nent;
   SnapEntry* p;
   // Conservative estimate.
   lj_snap_grow_map(J, nsnapmap + nslots + (MSize)(LJ_FR2 ? 2 : J->framedepth + 1));
   p = &J->cur.snapmap[nsnapmap];
   nent = snapshot_slots(J, p, nslots);
   snap->nent = (uint8_t)nent;
   nent += snapshot_framelinks(J, p + nent, &snap->topslot);
   snap->mapofs = (uint32_t)nsnapmap;
   snap->ref = (IRRef1)J->cur.nins;
   snap->mcofs = 0;
   snap->nslots = (uint8_t)nslots;
   snap->count = 0;
   J->cur.nsnapmap = (uint32_t)(nsnapmap + nent);
}

//********************************************************************************************************************
// Add or merge a snapshot.

void lj_snap_add(jit_State* J)
{
   MSize nsnap = J->cur.nsnap;
   MSize nsnapmap = J->cur.nsnapmap;

   pf::Log log(__FUNCTION__);
   log.msg(VLF::BRANCH|VLF::DETAIL, "Adding snapshot %d, baseslot=%d, maxslot=%d, retdepth=%d, ByteCode: %d", nsnap, J->baseslot, J->maxslot, J->retdepth, bc_op(*J->pc));

   // Merge if no ins. inbetween or if requested and no guard inbetween.
   if ((nsnap > 0 and J->cur.snap[nsnap - 1].ref IS J->cur.nins) or
      (J->mergesnap and !irt_isguard(J->guardemit))) {
      if (nsnap IS 1) {  // But preserve snap #0 PC.
         emitir_raw(IRT(IR_NOP, IRT_NIL), 0, 0);
         goto nomerge;
      }
      nsnapmap = J->cur.snap[--nsnap].mapofs;
   }
   else {
   nomerge:
      lj_snap_grow_buf(J, nsnap + 1);
      J->cur.nsnap = (uint16_t)(nsnap + 1);
   }
   J->mergesnap = 0;
   J->guardemit.irt = 0;
   snapshot_stack(J, &J->cur.snap[nsnap], nsnapmap);
}

//********************************************************************************************************************
// Snapshot modification

#define SNAP_USEDEF_SLOTS   (LJ_MAX_JSLOTS+LJ_STACK_EXTRA)

// Find unused slots with reaching-definitions bytecode data-flow analysis.

static BCREG snap_usedef(jit_State *J, uint8_t *udf, const BCIns *pc, BCREG maxslot)
{
   BCREG s;
   GCobj *o;

   if (maxslot IS 0) return 0;

#ifdef LUAJIT_USE_VALGRIND
   // Avoid errors for harmless reads beyond maxslot.
   memset(udf, 1, SNAP_USEDEF_SLOTS);
#else
   memset(udf, 1, maxslot);
#endif

   // Treat open upvalues as used.
   o = gcref(J->L->openupval);
   while (o) {
      if (uvval(gco_to_upval(o)) < J->L->base) break;
      udf[uvval(gco_to_upval(o)) - J->L->base] = 0;
      o = gcref(o->gch.nextgc);
   }

#define USE_SLOT(s)      udf[(s)] &= ~1
#define DEF_SLOT(s)      udf[(s)] *= 3

   // Scan through following bytecode and check for uses/defs.

   lj_assertJ(pc >= proto_bc(J->pt) and pc < proto_bc(J->pt) + J->pt->sizebc, "snapshot PC out of range");

   while (true) {
      BCIns ins = *pc++;
      BCOp op = bc_op(ins);
      switch (bcmode_b(op)) {
         case BCMvar: USE_SLOT(bc_b(ins)); break;
         default: break;
      }

      switch (bcmode_c(op)) {
         case BCMvar: USE_SLOT(bc_c(ins)); break;
         case BCMrbase:
            lj_assertJ(op IS BC_CAT, "unhandled op %d with RC rbase", op);
            for (s = bc_b(ins); s <= bc_c(ins); s++) USE_SLOT(s);
            for (; s < maxslot; s++) DEF_SLOT(s);
            break;
         case BCMjump:
         handle_jump: {
            BCREG minslot = bc_a(ins);
            if (bc_is_for_loop(op)) minslot += FORL_EXT;
            else if (bc_is_iter_loop(op)) minslot += bc_b(pc[-2]) - 1;
            else if (op IS BC_UCLO) {
               ptrdiff_t delta = bc_j(ins);
               if (delta < 0) return maxslot;  //  Prevent loop.
               pc += delta;
               break;
            }
            for (s = minslot; s < maxslot; s++) DEF_SLOT(s);
            return minslot < maxslot ? minslot : maxslot;
         }
         case BCMlit:
            if (bc_is_jloop(op)) {
               goto handle_jump;
            }
            else if (bc_isret(op)) {
               BCREG top = op IS BC_RETM ? maxslot : (bc_a(ins) + bc_d(ins) - 1);
               for (s = 0; s < bc_a(ins); s++) DEF_SLOT(s);
               for (; s < top; s++) USE_SLOT(s);
               for (; s < maxslot; s++) DEF_SLOT(s);
               return 0;
            }
            break;
         case BCMfunc: return maxslot;  //  NYI: will abort, anyway.
         default: break;
      }

      switch (bcmode_a(op)) {
         case BCMvar: USE_SLOT(bc_a(ins)); break;
         case BCMdst:
            if (!(op IS BC_ISTC or op IS BC_ISFC)) DEF_SLOT(bc_a(ins));
            break;
         case BCMbase:
            if (bc_is_call_or_iter(op)) {
               BCREG top = (op IS BC_CALLM or op IS BC_CALLMT or bc_c(ins) IS 0) ?
                  maxslot : (bc_a(ins) + bc_c(ins) + LJ_FR2);
               DEF_SLOT(bc_a(ins) + 1);
               s = bc_a(ins) - ((op IS BC_ITERC or op IS BC_ITERN or op IS BC_ITERA) ? 3 : 0);
               for (; s < top; s++) USE_SLOT(s);
               for (; s < maxslot; s++) DEF_SLOT(s);
               if (op IS BC_CALLT or op IS BC_CALLMT) {
                  for (s = 0; s < bc_a(ins); s++) DEF_SLOT(s);
                  return 0;
               }
            }
            else if (op IS BC_VARG) {
               return maxslot;  //  NYI: punt.
            }
            else if (op IS BC_KNIL) {
               for (s = bc_a(ins); s <= bc_d(ins); s++) DEF_SLOT(s);
            }
            else if (op IS BC_TSETM) {
               for (s = bc_a(ins) - 1; s < maxslot; s++) USE_SLOT(s);
            }
            break;
         default: break;
      }

      lj_assertJ(pc >= proto_bc(J->pt) and pc < proto_bc(J->pt) + J->pt->sizebc, "use/def analysis PC out of range");
   } // while

#undef USE_SLOT
#undef DEF_SLOT

   return 0;  //  unreachable
}

//********************************************************************************************************************
// Mark slots used by upvalues of child prototypes as used.

static void snap_useuv(GCproto* pt, uint8_t* udf)
{
   // This is a coarse check, because it's difficult to correlate the lifetime of slots and closures. But the
   // number of false positives is quite low.  A false positive may cause a slot not to be purged, which is just
   // a missed optimization.

   if ((pt->flags & PROTO_CHILD)) {
      ptrdiff_t i, j, n = pt->sizekgc;
      GCRef* kr = mref<GCRef>(pt->k) - 1;
      for (i = 0; i < n; i++, kr--) {
         GCobj *o = gcref(*kr);
         if (o->gch.gct IS ~LJ_TPROTO) {
            for (j = 0; j < gco_to_proto(o)->sizeuv; j++) {
               uint32_t v = proto_uv(gco_to_proto(o))[j];
               if ((v & PROTO_UV_LOCAL)) {
                  udf[(v & 0xff)] = 0;
               }
            }
         }
      }
   }
}

//********************************************************************************************************************
// Purge dead slots before the next snapshot.

void lj_snap_purge(jit_State *J)
{
   uint8_t udf[SNAP_USEDEF_SLOTS];
   BCREG s, maxslot = J->maxslot;
   if (bc_op(*J->pc) IS BC_FUNCV and maxslot > J->pt->numparams) maxslot = J->pt->numparams;
   s = snap_usedef(J, udf, J->pc, maxslot);
   if (s < maxslot) {
      snap_useuv(J->pt, udf);
      for (; s < maxslot; s++)
         if (udf[s] != 0) J->base[s] = 0;  //  Purge dead slots.
   }
}

//********************************************************************************************************************
// Shrink last snapshot by removing unused slot entries.
//
// This function performs dead slot elimination on the most recent snapshot. It uses
// reaching-definitions analysis (snap_usedef) to determine which slots are actually
// needed for correct restoration.
//
// IMPORTANT: This modifies both snap->nent AND J->cur.nsnapmap.
//
// Before shrink (example with 4 slots):
//   snapmap: [slot0][slot1][slot2][slot3][PC_lo][PC_hi]
//            ^mapofs                      ^mapofs+nent
//   nent = 4, nsnapmap = mapofs + 6
//
// After shrink (if slots 1 and 2 are unused):
//   snapmap: [slot0][slot3][PC_lo][PC_hi]
//            ^mapofs       ^mapofs+nent
//   nent = 2, nsnapmap = mapofs + 4
//
// The PC + frame links (2 SnapEntry = 64 bits) are moved down to immediately follow
// the remaining slot entries. This compacts the snapmap and frees space for future
// snapshots.
//
// NOTE: The next snapshot must be created AFTER this shrink completes, otherwise
// it would start at the old nsnapmap position and overlap with this snapshot's data.

void lj_snap_shrink(jit_State* J)
{
   SnapShot* snap = &J->cur.snap[J->cur.nsnap - 1];
   SnapEntry* map = &J->cur.snapmap[snap->mapofs];
   MSize n, m, nlim, nent = snap->nent;
   uint8_t udf[SNAP_USEDEF_SLOTS];
   BCREG maxslot = J->maxslot;
   BCREG baseslot = J->baseslot;
   BCREG minslot = snap_usedef(J, udf, snap_pc(&map[nent]), maxslot);
   if (minslot < maxslot) snap_useuv(J->pt, udf);
   maxslot += baseslot;
   minslot += baseslot;
   snap->nslots = (uint8_t)maxslot;
   for (n = m = 0; n < nent; n++) {  // Remove unused slots from snapshot.
      BCREG s = snap_slot(map[n]);
      if (s < minslot or (s < maxslot and udf[s - baseslot] IS 0))
         map[m++] = map[n];  //  Only copy used slots.
   }
   snap->nent = (uint8_t)m;
   nlim = J->cur.nsnapmap - snap->mapofs - 1;
   while (n <= nlim) map[m++] = map[n++];  //  Move PC + frame links down.
   J->cur.nsnapmap = (uint32_t)(snap->mapofs + m);  //  Free up space in map.
}

//********************************************************************************************************************
// Snapshot access

// Initialize a Bloom Filter with all renamed refs.
// There are very few renames (often none), so the filter has
// very few bits set. This makes it suitable for negative filtering.

static BloomFilter snap_renamefilter(GCtrace* T, SnapNo lim)
{
   BloomFilter rfilt = 0;
   IRIns* ir;
   for (ir = &T->ir[T->nins - 1]; ir->o IS IR_RENAME; ir--) {
      if (ir->op2 <= lim) bloomset(rfilt, ir->op1);
   }
   return rfilt;
}

// Process matching renames to find the original RegSP.
static RegSP snap_renameref(GCtrace* T, SnapNo lim, IRRef ref, RegSP rs)
{
   IRIns* ir;
   for (ir = &T->ir[T->nins - 1]; ir->o IS IR_RENAME; ir--)
      if (ir->op1 IS ref and ir->op2 <= lim) rs = ir->prev;
   return rs;
}

//********************************************************************************************************************
// Copy RegSP from parent snapshot to the parent links of the IR.

IRIns * lj_snap_regspmap(jit_State *J, GCtrace *T, SnapNo snapno, IRIns *ir)
{
   SnapShot* snap = &T->snap[snapno];
   SnapEntry* map = &T->snapmap[snap->mapofs];
   BloomFilter rfilt = snap_renamefilter(T, snapno);
   MSize n = 0;
   IRRef ref = 0;

   for (; ; ir++) {
      uint32_t rs;
      if (ir->o IS IR_SLOAD) {
         if (!(ir->op2 & IRSLOAD_PARENT)) break;
         for (; ; n++) {
            lj_assertJ(n < snap->nent, "slot %d not found in snapshot", ir->op1);
            if (snap_slot(map[n]) IS ir->op1) {
               ref = snap_ref(map[n++]);
               break;
            }
         }
      }
      else if (ir->o IS IR_PVAL) ref = ir->op1 + REF_BIAS;
      else break;

      rs = T->ir[ref].prev;
      if (bloomtest(rfilt, ref)) rs = snap_renameref(T, snapno, ref, rs);
      ir->prev = (uint16_t)rs;
      lj_assertJ(regsp_used(rs), "unused IR %04d in snapshot", ref - REF_BIAS);
   }
   return ir;
}

//********************************************************************************************************************
// Snapshot replay

// Replay constant from parent trace.

static TRef snap_replay_const(jit_State* J, IRIns* ir)
{
   // Only have to deal with constants that can occur in stack slots.
   switch ((IROp)ir->o) {
      case IR_KPRI: return TREF_PRI(irt_type(ir->t));
      case IR_KINT: return lj_ir_kint(J, ir->i);
      case IR_KGC: return lj_ir_kgc(J, ir_kgc(ir), irt_t(ir->t));
      case IR_KNUM:
      case IR_KINT64:
         return lj_ir_k64(J, (IROp)ir->o, ir_k64(ir)->u64);
      case IR_KPTR: return lj_ir_kptr(J, ir_kptr(ir));  //  Continuation.
      default: lj_assertJ(0, "bad IR constant op %d", ir->o); return TREF_NIL;
   }
}

//********************************************************************************************************************
// De-duplicate parent reference.

static TRef snap_dedup(jit_State* J, SnapEntry* map, MSize nmax, IRRef ref)
{
   MSize j;
   for (j = 0; j < nmax; j++)
      if (snap_ref(map[j]) IS ref)
         return J->slot[snap_slot(map[j])] & ~(SNAP_KEYINDEX | SNAP_CONT | SNAP_FRAME);
   return 0;
}

//********************************************************************************************************************
// Emit parent reference with de-duplication.

static TRef snap_pref(jit_State* J, GCtrace* T, SnapEntry* map, MSize nmax, BloomFilter seen, IRRef ref)
{
   IRIns *ir = &T->ir[ref];
   TRef tr;
   if (irref_isk(ref)) tr = snap_replay_const(J, ir);
   else if (!regsp_used(ir->prev)) tr = 0;
   else if (!bloomtest(seen, ref) or (tr = snap_dedup(J, map, nmax, ref)) IS 0)
      tr = emitir(IRT(IR_PVAL, irt_type(ir->t)), ref - REF_BIAS, 0);
   return tr;
}

//********************************************************************************************************************
// Check whether a sunk store corresponds to an allocation. Slow path.

static int snap_sunk_store2(GCtrace* T, IRIns* ira, IRIns* irs)
{
   if (irs->o IS IR_ASTORE or irs->o IS IR_HSTORE or irs->o IS IR_FSTORE or irs->o IS IR_XSTORE) {
      IRIns* irk = &T->ir[irs->op1];
      if (irk->o IS IR_AREF or irk->o IS IR_HREFK) irk = &T->ir[irk->op1];
      return (&T->ir[irk->op1] IS ira);
   }
   return 0;
}

//********************************************************************************************************************
// Check whether a sunk store corresponds to an allocation. Fast path.

static LJ_AINLINE int snap_sunk_store(GCtrace* T, IRIns* ira, IRIns* irs)
{
   if (irs->s != 255) return (ira + irs->s IS irs);  //  Fast check.
   return snap_sunk_store2(T, ira, irs);
}

//********************************************************************************************************************
// Replay snapshot state to setup side trace.

void lj_snap_replay(jit_State* J, GCtrace* T)
{
   SnapShot* snap = &T->snap[J->exitno];
   SnapEntry* map = &T->snapmap[snap->mapofs];
   MSize n, nent = snap->nent;
   BloomFilter seen = 0;
   int pass23 = 0;
   J->framedepth = 0;

   // Emit IR for slots inherited from parent snapshot.

   for (n = 0; n < nent; n++) {
      SnapEntry sn = map[n];
      BCREG s = snap_slot(sn);
      IRRef ref = snap_ref(sn);
      IRIns* ir = &T->ir[ref];
      TRef tr;
      // The bloom filter avoids O(nent^2) overhead for de-duping slots.
      if (bloomtest(seen, ref) and (tr = snap_dedup(J, map, n, ref)) != 0) goto setslot;
      bloomset(seen, ref);

      if (irref_isk(ref)) {
         // See special treatment of LJ_FR2 slot 1 in snapshot_slots() above.
         if ((sn IS SNAP(1, SNAP_FRAME | SNAP_NORESTORE, REF_NIL))) tr = 0;
         else tr = snap_replay_const(J, ir);
      }
      else if (!regsp_used(ir->prev)) {
         pass23 = 1;
         lj_assertJ(s != 0, "unused slot 0 in snapshot");
         tr = s;
      }
      else {
         IRType t = irt_type(ir->t);
         uint32_t mode = IRSLOAD_INHERIT | IRSLOAD_PARENT;
         if (ir->o IS IR_SLOAD) mode |= (ir->op2 & IRSLOAD_READONLY);
         if ((sn & SNAP_KEYINDEX)) mode |= IRSLOAD_KEYINDEX;
         tr = emitir_raw(IRT(IR_SLOAD, t), s, mode);
      }
   setslot:
      // Same as TREF_* flags.
      J->slot[s] = tr | (sn & (SNAP_KEYINDEX | SNAP_CONT | SNAP_FRAME));
      J->framedepth += ((sn & (SNAP_CONT | SNAP_FRAME)) and (s != LJ_FR2));
      if (sn & SNAP_FRAME) J->baseslot = s + 1;
   }

   if (pass23) {
      IRIns* irlast = &T->ir[snap->ref];
      pass23 = 0;
      // Emit dependent PVALs.
      for (n = 0; n < nent; n++) {
         SnapEntry sn = map[n];
         IRRef refp = snap_ref(sn);
         IRIns* ir = &T->ir[refp];
         if (regsp_reg(ir->r) IS RID_SUNK) {
            if (J->slot[snap_slot(sn)] != snap_slot(sn)) continue;
            pass23 = 1;
            lj_assertJ(ir->o IS IR_TNEW or ir->o IS IR_TDUP or
               ir->o IS IR_CNEW or ir->o IS IR_CNEWI,
               "sunk parent IR %04d has bad op %d", refp - REF_BIAS, ir->o);
            if (ir->op1 >= T->nk) snap_pref(J, T, map, nent, seen, ir->op1);
            if (ir->op2 >= T->nk) snap_pref(J, T, map, nent, seen, ir->op2);

            IRIns* irs;
            for (irs = ir + 1; irs < irlast; irs++) {
               if (irs->r IS RID_SINK and snap_sunk_store(T, ir, irs)) {
                  if (snap_pref(J, T, map, nent, seen, irs->op2) IS 0) snap_pref(J, T, map, nent, seen, T->ir[irs->op2].op1);
               }
            }
         }
         else if (!irref_isk(refp) and !regsp_used(ir->prev)) {
            lj_assertJ(ir->o IS IR_CONV and ir->op2 IS IRCONV_NUM_INT,
               "sunk parent IR %04d has bad op %d", refp - REF_BIAS, ir->o);
            J->slot[snap_slot(sn)] = snap_pref(J, T, map, nent, seen, ir->op1);
         }
      }

      // Replay sunk instructions.

      for (n = 0; pass23 and n < nent; n++) {
         SnapEntry sn = map[n];
         IRRef refp = snap_ref(sn);
         IRIns* ir = &T->ir[refp];
         if (regsp_reg(ir->r) IS RID_SUNK) {
            TRef op1, op2;
            if (J->slot[snap_slot(sn)] != snap_slot(sn)) {  // De-dup allocs.
               J->slot[snap_slot(sn)] = J->slot[J->slot[snap_slot(sn)]];
               continue;
            }
            op1 = ir->op1;
            if (op1 >= T->nk) op1 = snap_pref(J, T, map, nent, seen, op1);
            op2 = ir->op2;
            if (op2 >= T->nk) op2 = snap_pref(J, T, map, nent, seen, op2);

            IRIns *irs;
            TRef tr = emitir(ir->ot, op1, op2);
            J->slot[snap_slot(sn)] = tr;
            for (irs = ir + 1; irs < irlast; irs++) {
               if (irs->r IS RID_SINK and snap_sunk_store(T, ir, irs)) {
                  IRIns* irr = &T->ir[irs->op1];
                  TRef val, key = irr->op2, tmp = tr;
                  if (irr->o != IR_FREF) {
                     IRIns* irk = &T->ir[key];
                     if (irr->o IS IR_HREFK) key = lj_ir_kslot(J, snap_replay_const(J, &T->ir[irk->op1]), irk->op2);
                     else key = snap_replay_const(J, irk);
                     if (irr->o IS IR_HREFK or irr->o IS IR_AREF) {
                        IRIns* irf = &T->ir[irr->op1];
                        tmp = emitir(irf->ot, tmp, irf->op2);
                     }
                  }
                  tmp = emitir(irr->ot, tmp, key);
                  val = snap_pref(J, T, map, nent, seen, irs->op2);
                  if (val IS 0) {
                     IRIns* irc = &T->ir[irs->op2];
                     lj_assertJ(irc->o IS IR_CONV and irc->op2 IS IRCONV_NUM_INT,
                        "sunk store for parent IR %04d with bad op %d",
                        refp - REF_BIAS, irc->o);
                     val = snap_pref(J, T, map, nent, seen, irc->op1);
                     val = emitir(IRTN(IR_CONV), val, IRCONV_NUM_INT);
                  }
                  tmp = emitir(irs->ot, tmp, val);
               }
            }
         }
      }
   }
   J->base = J->slot + J->baseslot;
   J->maxslot = snap->nslots - J->baseslot;
   lj_snap_add(J);
   if (pass23)  //  Need explicit GC step _after_ initial snapshot.
      emitir_raw(IRTG(IR_GCSTEP, IRT_NIL), 0, 0);
}

//********************************************************************************************************************
// Snapshot restore

static void snap_unsink(jit_State* J, GCtrace* T, ExitState* ex,
   SnapNo snapno, BloomFilter rfilt,
   IRIns* ir, TValue* o);

// Restore a value from the trace exit state.

static void snap_restoreval(jit_State* J, GCtrace* T, ExitState* ex, SnapNo snapno, BloomFilter rfilt, IRRef ref, TValue* o)
{
   IRIns *ir = &T->ir[ref];
   IRType1 t = ir->t;
   RegSP rs = ir->prev;
   if (irref_isk(ref)) {  // Restore constant slot.
      if (ir->o IS IR_KPTR) o->u64 = (uint64_t)(uintptr_t)ir_kptr(ir);
      else {
         lj_assertJ(!(ir->o IS IR_KKPTR or ir->o IS IR_KNULL),
            "restore of const from IR %04d with bad op %d", ref - REF_BIAS, ir->o);
         lj_ir_kvalue(J->L, o, ir);
      }
      return;
   }

   if (bloomtest(rfilt, ref)) rs = snap_renameref(T, snapno, ref, rs);

   if (ra_hasspill(regsp_spill(rs))) {  // Restore from spill slot.
      int32_t* sps = &ex->spill[regsp_spill(rs)];
      if (irt_isinteger(t)) setintV(o, *sps);
      else if (irt_isnum(t)) o->u64 = *(uint64_t*)sps;
      else {
         lj_assertJ(!irt_ispri(t), "PRI ref with spill slot");
         setgcV(J->L, o, (GCobj*)(uintptr_t) * (GCSize*)sps, irt_toitype(t));
      }
   }
   else {  // Restore from register.
      Reg r = regsp_reg(rs);
      if (ra_noreg(r)) {
         lj_assertJ(ir->o IS IR_CONV and ir->op2 IS IRCONV_NUM_INT, "restore from IR %04d has no reg", ref - REF_BIAS);
         snap_restoreval(J, T, ex, snapno, rfilt, ir->op1, o);
         if (LJ_DUALNUM) setnumV(o, (lua_Number)intV(o));
         return;
      }
      else if (irt_isinteger(t))  setintV(o, (int32_t)ex->gpr[r - RID_MIN_GPR]);
      else if (irt_isnum(t)) setnumV(o, ex->fpr[r - RID_MIN_FPR]);
      else if (irt_ispri(t)) setpriV(o, uint64_t(irt_toitype(t)));
      else setgcV(J->L, o, (GCobj*)ex->gpr[r - RID_MIN_GPR], irt_toitype(t));
   }
}

//********************************************************************************************************************
// Unsink allocation from the trace exit state. Unsink sunk stores.

static void snap_unsink(jit_State *J, GCtrace *T, ExitState *ex, SnapNo snapno, BloomFilter rfilt,
   IRIns* ir, TValue* o)
{
   lj_assertJ(ir->o IS IR_TNEW or ir->o IS IR_TDUP or ir->o IS IR_CNEW or ir->o IS IR_CNEWI,
      "sunk allocation with bad op %d", ir->o);

   {
      IRIns* irs, * irlast;
      GCtab* t = ir->o IS IR_TNEW ? lj_tab_new(J->L, ir->op1, ir->op2) :
         lj_tab_dup(J->L, ir_ktab(&T->ir[ir->op1]));
      settabV(J->L, o, t);
      irlast = &T->ir[T->snap[snapno].ref];
      for (irs = ir + 1; irs < irlast; irs++)
         if (irs->r IS RID_SINK and snap_sunk_store(T, ir, irs)) {
            IRIns* irk = &T->ir[irs->op1];
            TValue tmp, * val;
            lj_assertJ(irs->o IS IR_ASTORE or irs->o IS IR_HSTORE or irs->o IS IR_FSTORE, "sunk store with bad op %d", irs->o);
            if (irk->o IS IR_FREF) {
               lj_assertJ(irk->op2 IS IRFL_TAB_META, "sunk store with bad field %d", irk->op2);
               snap_restoreval(J, T, ex, snapno, rfilt, irs->op2, &tmp);
               // NOBARRIER: The table is new (marked white).
               setgcref(t->metatable, obj2gco(tabV(&tmp)));
            }
            else {
               irk = &T->ir[irk->op2];
               if (irk->o IS IR_KSLOT) irk = &T->ir[irk->op1];
               lj_ir_kvalue(J->L, &tmp, irk);
               val = lj_tab_set(J->L, t, &tmp);
               // NOBARRIER: The table is new (marked white).
               snap_restoreval(J, T, ex, snapno, rfilt, irs->op2, val);
            }
         }
   }
}

//********************************************************************************************************************
// Restore interpreter state from exit state with the help of a snapshot.

const BCIns * lj_snap_restore(jit_State *J, void *exptr)
{
   ExitState *ex = (ExitState*)exptr;
   SnapNo snapno = J->exitno;  //  For now, snapno IS exitno.
   GCtrace *T = traceref(J, J->parent);
   SnapShot *snap = &T->snap[snapno];
   MSize n, nent = snap->nent;
   SnapEntry *map = &T->snapmap[snap->mapofs];
#ifdef LUA_USE_ASSERT
   SnapEntry *flinks = &T->snapmap[snap_nextofs(T, snap) - 1 - LJ_FR2];
#endif
   TValue* frame;
   BloomFilter rfilt = snap_renamefilter(T, snapno);
   const BCIns* pc = snap_pc(&map[nent]);
   lua_State* L = J->L;

   pf::Log log(__FUNCTION__);
   log.traceBranch("Restoring snapshot %d for trace %d", snapno, J->parent);
   log.trace("Snapshot: nent=%d, nslots=%d, topslot=%d, mapofs=%d", nent, snap->nslots, snap->topslot, snap->mapofs);
   log.trace("Before restore: L->base=%p, L->top=%p, jit_base=%p", L->base, L->top, tvref(G(L)->jit_base));

   // Set interpreter PC to the next PC to get correct error messages.
   setcframe_pc(cframe_raw(L->cframe), pc + 1);

   // Make sure the stack is big enough for the slots from the snapshot.
   if (L->base + snap->topslot >= tvref(L->maxstack)) [[unlikely]] {
      L->top = curr_topL(L);
      lj_state_growstack(L, snap->topslot - curr_proto(L)->framesize);
   }

   // Fill stack slots with data from the registers and spill slots.
   frame = L->base - 1 - LJ_FR2;

#if 0
   // Debug: dump stack BEFORE restoration to see what JIT left
   log.detail("Stack BEFORE restoration (frame=%p):", frame);
   for (int i = 0; i < 20; i++) {
      TValue* slot = &frame[i];
      if (tvisgcv(slot)) {
         GCobj *gc = gcval(slot);
         log.detail("  frame[%d] %p: type=%d gcobj=%p (gct=%d)", i, slot, itype(slot), gc, gc ? gc->gch.gct : -1);
      }
      else log.detail("  frame[%d] %p: type=%d val=0x%llx", i, slot, itype(slot), (unsigned long long)slot->u64);
   }

   // Log all snapshot entries first
   log.detail("Snapshot entries (nent=%d):", nent);
   for (n = 0; n < nent; n++) {
      SnapEntry sn = map[n];
      BCREG slot = snap_slot(sn);
      bool norestore = (sn & SNAP_NORESTORE) != 0;
      bool is_frame = (sn & SNAP_FRAME) != 0;
      bool is_cont = (sn & SNAP_CONT) != 0;
      log.detail("  entry[%d]: slot=%d, norestore=%d, frame=%d, cont=%d, ref=%d", n, slot, norestore, is_frame, is_cont, snap_ref(sn));
   }
#endif

   for (n = 0; n < nent; n++) {
      SnapEntry sn = map[n];
      if (!(sn & SNAP_NORESTORE)) {
         TValue* o = &frame[snap_slot(sn)];
         IRRef ref = snap_ref(sn);
         IRIns* ir = &T->ir[ref];
         if (ir->r IS RID_SUNK) {
            MSize j;
            for (j = 0; j < n; j++)
               if (snap_ref(map[j]) IS ref) {  // De-duplicate sunk allocations.
                  copyTV(L, o, &frame[snap_slot(map[j])]);
                  goto dupslot;
               }
            snap_unsink(J, T, ex, snapno, rfilt, ir, o);
         dupslot:
            continue;
         }

         snap_restoreval(J, T, ex, snapno, rfilt, ref, o);

         if ((sn & SNAP_KEYINDEX)) {
            // A IRT_INT key index slot is restored as a number. Undo this.
            o->u32.lo = (uint32_t)(LJ_DUALNUM ? intV(o) : lj_num2int(numV(o)));
            o->u32.hi = LJ_KEYINDEX;
         }
      }
      else log.detail("Slot %d: NORESTORE (skipped)", snap_slot(sn));
   }

   uint8_t base_adj = (map[nent + LJ_BE] & 0xff);

   L->base += base_adj;
   lj_assertJ(map + nent IS flinks, "inconsistent frames in snapshot");

   // Compute current stack top.
   BCOp op = bc_op(*pc);
   switch (op) {
   default:
      if (bc_is_func_header(op)) L->top = frame + snap->nslots;
      else L->top = curr_topL(L);
      break;
   case BC_CALLM: case BC_CALLMT: case BC_RETM: case BC_TSETM:
      L->top = frame + snap->nslots;
      break;
   }

   log.trace("Final: L->base=%p, L->top=%p, slots=%d", L->base, L->top, (int)(L->top - L->base));
   return pc;
}

#undef emitir_raw
#undef emitir
