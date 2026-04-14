// Frame management abstractions for JIT trace recorder.
// Copyright © 2025-2026 Paul Manias

#pragma once

#include "../debug/lj_jit.h"
#include "../runtime/lj_frame.h"
#include "../lj_iropt.h"
#include <cassert>

// Named constants for frame layout (2-slot frame mode, LJ_FR2=1)

namespace FRC { // 'FRame Const'
   // Frame header size: function slot + return continuation slot.  In 2-slot mode (LJ_FR2=1): 1 + LJ_FR2 = 2
   inline constexpr BCREG HEADER_SIZE = 2;

   // Continuation frame size: 2 << LJ_FR2 = 4 slots.  Used for metamethod continuation frames
   inline constexpr BCREG CONT_FRAME_SIZE = 4;

   // Offset from base to function slot: -1 - LJ_FR2 = -2.  The function is stored at base[-2] in 2-slot mode
   inline constexpr int32_t FUNC_SLOT_OFFSET = -2;

   // Minimum baseslot value: 1 + LJ_FR2 = 2.  The invoking function is at base[-1-LJ_FR2] = base[-2]
   inline constexpr BCREG MIN_BASESLOT = 2;

   // Check if trace is at root level (no nested frames or pending returns)
   // This is a common check: framedepth + retdepth IS 0
   [[nodiscard]] inline bool at_trace_root(jit_State *J) {
      return J->framedepth + J->retdepth IS 0;
   }

   // Check if currently at root frame depth (framedepth IS 0, ignoring retdepth)
   [[nodiscard]] inline bool at_root_depth(jit_State *J) {
      return J->framedepth IS 0;
   }

   // Increment frame depth (for call recording)
   inline void inc_depth(jit_State *J) {
      J->framedepth++;
   }

   // Decrement frame depth and return result for error checking
   [[nodiscard]] inline int32_t dec_depth(jit_State *J) {
      return --J->framedepth;
   }

   // Decrement frame depth by amount and return result
   [[nodiscard]] inline int32_t dec_depth_by(jit_State *J, int32_t amount) {
      J->framedepth -= amount;
      return J->framedepth;
   }
}

//********************************************************************************************************************
// FrameManager - Encapsulates frame push/pop arithmetic for the JIT recorder.
//
// The JIT recorder maintains J->base and J->baseslot which must stay in sync.  This class provides methods that
// correctly adjust both together, avoiding off-by-one errors in the frame header size calculations.
//
// Frame layout in 2-slot mode:
//   base[-2]  = function slot (func)
//   base[-1]  = frame marker (TREF_FRAME or PC/delta)
//   base[0]   = first argument/local slot
//   ...
//
// When pushing a call frame at slot 'func': new_base = old_base + func + HEADER_SIZE
// When popping a Lua frame with cbase:      new_base = old_base - cbase - HEADER_SIZE

class FrameManager {
   jit_State *J;

public:
   explicit FrameManager(jit_State *j) : J(j) {}

   // Push a new call frame (adjusts base by func_slot + header)
   // Used after setting up call with rec_call_setup
   void push_call_frame(BCREG func_slot) {
      BCREG offset = func_slot + FRC::HEADER_SIZE;
      J->base += offset;
      J->baseslot += offset;
   }

   // Pop vararg/pcall/continuation frame (delta-based, no header adjustment)
   // These frames use frame_delta() which already accounts for slot layout
   void pop_delta_frame(BCREG cbase) {
      J->baseslot -= cbase;
      J->base -= cbase;
   }

   // Pop Lua return frame (includes 2-slot header)
   // Used when returning from a Lua function call
   void pop_lua_frame(BCREG cbase) {
      BCREG offset = cbase + FRC::HEADER_SIZE;
      J->baseslot -= offset;
      J->base -= offset;
   }

   // Access frame function slot (base[-2] in 2-slot mode)
   [[nodiscard]] TRef& func_slot() { return J->base[FRC::FUNC_SLOT_OFFSET]; }
   [[nodiscard]] TRef func_slot() const { return J->base[FRC::FUNC_SLOT_OFFSET]; }

   // Check stack overflow before pushing
   [[nodiscard]] bool would_overflow(BCREG additional_slots) const {
      return (J->baseslot + additional_slots >= LJ_MAX_JSLOTS);
   }

   // Move slots for tail call (compact stack)
   // Moves func + args from source position to frame function slot position
   void compact_tailcall(BCREG func_slot, BCREG slot_count) {
      memmove(&J->base[FRC::FUNC_SLOT_OFFSET], &J->base[func_slot], sizeof(TRef) * (slot_count + FRC::HEADER_SIZE));
   }

   // Copy results during return
   void copy_results(int32_t dest_offset, int32_t src_offset, ptrdiff_t count) {
      memmove(&J->base[dest_offset], &J->base[src_offset], sizeof(TRef) * count);
   }

   // Clear frame slots (set to zero)
   void clear_frame(int32_t start_offset, BCREG count) {
      memset(&J->base[start_offset], 0, sizeof(TRef) * count);
   }

   // Get current baseslot value
   [[nodiscard]] BCREG baseslot() const { return J->baseslot; }

   // Check if at minimum baseslot (root frame)
   [[nodiscard]] bool at_root_baseslot() const {
      return J->baseslot IS FRC::MIN_BASESLOT;
   }

   // Get underlying jit_State (for SlotView construction)
   [[nodiscard]] jit_State* state() const { return J; }
};

//********************************************************************************************************************
// FrameDepthGuard - RAII guard for tracking frame depth during call recording.
//
// Frame depth tracks how many frames deep we are in the trace being recorded.  This guard ensures that framedepth is
// decremented on scope exit, unless explicitly released.
//
// Usage patterns:
//   1. Simple increment/auto-decrement:
//        FrameDepthGuard fdg(J);  // framedepth++
//        // ... on scope exit, framedepth-- automatically
//
//   2. Manual decrement with error check (for returns):
//        FrameDepthGuard fdg(J, false);  // Don't auto-increment
//        if (fdg.decrement_and_check() < 0) error();  // Manual decrement + check
//
//   3. Release to keep incremented depth:
//        FrameDepthGuard fdg(J);  // framedepth++
//        fdg.release();  // Don't decrement on exit

class FrameDepthGuard {
   jit_State *J;
   bool released = false;

public:
   // Construct and increment framedepth
   explicit FrameDepthGuard(jit_State *j, bool auto_increment = true) : J(j) {
      if (auto_increment) J->framedepth++;
   }

   ~FrameDepthGuard() {
      if (not released) J->framedepth--;
   }

   // Non-copyable
   FrameDepthGuard(const FrameDepthGuard&) = delete;
   FrameDepthGuard& operator=(const FrameDepthGuard&) = delete;

   // Decrement and return new depth (for error checking)
   // Does NOT release - destructor will still decrement unless release() called
   [[nodiscard]] int32_t decrement_and_check() {
      return --J->framedepth;
   }

   // Decrement by specific amount (for continuation frames that subtract 2)
   [[nodiscard]] int32_t decrement_by(int32_t amount) {
      J->framedepth -= amount;
      return J->framedepth;
   }

   // Release guard - framedepth won't be decremented on destruction
   void release() { released = true; }

   // Check current depth
   [[nodiscard]] int32_t depth() const { return J->framedepth; }

   // Check if at root (framedepth IS 0)
   [[nodiscard]] bool at_root() const { return J->framedepth IS 0; }

   // Check combined depth (framedepth + retdepth)
   [[nodiscard]] int32_t combined_depth() const { return J->framedepth + J->retdepth; }
};

//********************************************************************************************************************
// IRRollbackPoint - Captures state for potential IR rollback.
//
// Some IR emission paths speculatively emit guards that may need to be rolled back if a later optimization
// discovers the guard was unnecessary.  This struct captures the rollback point (nins) and guardemit state.
//
// Usage:
//   IRRollbackPoint rbp;
//   rbp.mark(J);              // Capture current state
//   // ... emit speculative IR ...
//   if (need_rollback) rbp.rollback(J);

struct IRRollbackPoint {
   IRRef ref = 0;            // IR instruction reference for rollback point
   IRType1 guardemit{};      // Saved guardemit state

   // Mark current position as a potential rollback point
   void mark(jit_State *J) {
      ref = J->cur.nins;
      guardemit = J->guardemit;
   }

   // Check if a rollback point was marked
   [[nodiscard]] bool is_marked() const { return ref != 0; }

   // Perform rollback to the marked point
   void rollback(jit_State *J) {
      lj_ir_rollback(J, ref);
      J->guardemit = guardemit;
   }

   // Check if rollback is needed (when forwarding occurred)
   [[nodiscard]] bool needs_rollback(TRef result) const {
      return ref != 0 and tref_ref(result) < ref;
   }

   // Conditionally rollback if needed
   void rollback_if_forwarded(jit_State *J, TRef result) {
      if (needs_rollback(result)) rollback(J);
   }
};

//********************************************************************************************************************
// SlotView - Type-safe view into slot array with bounds checking.
//
// The JIT recorder maintains a slot array (J->slot) with J->base pointing into it.  Valid slot indices relative to
// J->base range from FRC::FUNC_SLOT_OFFSET (-2) to J->maxslot-1.  This class provides bounds-checked access in debug
// builds while maintaining zero overhead in release builds.
//
// Usage:
//   SlotView slots(J);
//   TRef val = slots[0];           // Access slot 0 (bounds checked in debug)
//   slots[ra] = result;            // Write to slot ra
//   TRef func = slots.func();      // Access function slot (base[-2])
//
// The bounds checking uses assert() which is only active in debug builds, so there is no runtime overhead in
// release builds.

class SlotView {
   jit_State *J;

   // Internal bounds check helper (only active in debug builds)
   // Note: idx can be more negative than FUNC_SLOT_OFFSET when upvalues alias parent frame slots
   void check_bounds([[maybe_unused]] int32_t idx) const {
      int32_t abs_slot = int32_t(J->baseslot) + idx;
      assert(abs_slot >= 0 && "absolute slot index below zero");
      assert(abs_slot < int32_t(LJ_MAX_JSLOTS) && "absolute slot index exceeds maximum");
   }

public:
   explicit SlotView(jit_State *j) : J(j) {}

   // Bounds-checked read access
   [[nodiscard]] TRef operator[](int32_t idx) const {
      check_bounds(idx);
      return J->base[idx];
   }

   // Bounds-checked write access (returns reference)
   [[nodiscard]] TRef& operator[](int32_t idx) {
      check_bounds(idx);
      return J->base[idx];
   }

   // Access function slot directly (common operation)
   [[nodiscard]] TRef func() const { return J->base[FRC::FUNC_SLOT_OFFSET]; }
   [[nodiscard]] TRef& func() { return J->base[FRC::FUNC_SLOT_OFFSET]; }

   // Check if a slot has been loaded (non-zero TRef)
   [[nodiscard]] bool is_loaded(int32_t idx) const { return (*this)[idx] != 0; }

   // Clear a single slot
   void clear(int32_t idx) { (*this)[idx] = 0; }

   // Clear a range of slots [start, start+count)
   void clear_range(int32_t start, BCREG count) {
      assert(start >= FRC::FUNC_SLOT_OFFSET && "clear_range start below minimum");
      assert(start + int32_t(count) <= int32_t(LJ_MAX_JSLOTS - J->baseslot) && "clear_range exceeds maximum");
      memset(&J->base[start], 0, sizeof(TRef) * count);
   }

   // Copy slots: copy count slots from src to dest (handles overlapping regions)
   void copy(int32_t dest, int32_t src, ptrdiff_t count) {
      assert(dest >= FRC::FUNC_SLOT_OFFSET && src >= FRC::FUNC_SLOT_OFFSET && "copy indices below minimum");
      assert(dest + count <= int32_t(LJ_MAX_JSLOTS - J->baseslot) && "copy dest exceeds maximum");
      assert(src + count <= int32_t(LJ_MAX_JSLOTS - J->baseslot) && "copy src exceeds maximum");
      memmove(&J->base[dest], &J->base[src], sizeof(TRef) * count);
   }

   // Get pointer to slot (for passing to functions that need TRef*)
   [[nodiscard]] TRef* ptr(int32_t idx) {
      assert(idx >= FRC::FUNC_SLOT_OFFSET && "ptr index below minimum");
      return &J->base[idx];
   }

   [[nodiscard]] const TRef* ptr(int32_t idx) const {
      assert(idx >= FRC::FUNC_SLOT_OFFSET && "ptr index below minimum");
      return &J->base[idx];
   }

   // Get current maxslot value
   [[nodiscard]] BCREG maxslot() const { return J->maxslot; }

   // Set maxslot (updates J->maxslot)
   void set_maxslot(BCREG val) { J->maxslot = val; }

   // Expand maxslot if needed (common pattern: ensure slot is within range)
   void ensure_slot(BCREG slot) {
      if (slot >= J->maxslot) J->maxslot = slot + 1;
   }

   // Shrink maxslot if slot is below current max (common pattern for dead slot elimination)
   void shrink_to(BCREG slot) {
      if (slot < J->maxslot) J->maxslot = slot;
   }
};

//********************************************************************************************************************
// IRBuilder - Type-safe wrapper for IR emission.
//
// The JIT recorder emits IR instructions using macros like `emitir(ot, a, b)` and `emitir_raw(ot, a, b)`.  IRBuilder
// provides a type-safe class interface that makes the code more readable and easier to maintain.
//
// Usage:
//   IRBuilder ir(J);
//
//   // Emit with optimization pass (equivalent to emitir macro)
//   TRef result = ir.emit(IRT(IR_ADD, IRT_INT), a, b);
//
//   // Emit raw without optimization (equivalent to emitir_raw macro)
//   TRef raw = ir.emit_raw(IRT(IR_SLOAD, t), slot, mode);
//
//   // Emit guard instruction
//   ir.guard(IR_EQ, a, b, IRT_INT);
//
//   // Access IR instruction by reference (equivalent to IR(ref) macro)
//   IRIns* ins = ir.at(ref);
//
// The class maintains a pointer to jit_State and delegates to the existing lj_ir_* functions.

class IRBuilder {
   jit_State *J;

public:
   explicit IRBuilder(jit_State *j) : J(j) {}

   // Access IR instruction by reference (replaces IR(ref) macro)
   [[nodiscard]] IRIns* at(IRRef ref) const { return &J->cur.ir[ref]; }

   // Emit IR with optimization pass (replaces emitir macro)
   // Passes the instruction through lj_opt_fold for CSE and constant folding
   TRef emit(uint32_t ot, TRef a, TRef b) {
      lj_ir_set(J, ot, a, b);
      return lj_opt_fold(J);
   }

   // Single-operand emission (common case)
   TRef emit(uint32_t ot, TRef a) { return emit(ot, a, 0); }

   // Emit raw IR without optimization (replaces emitir_raw macro)
   // Bypasses folding, used for SLOADs and other cases where optimization is unwanted
   TRef emit_raw(uint32_t ot, TRef a, TRef b) {
      lj_ir_set(J, ot, a, b);
      return lj_ir_emit(J);
   }

   // Single-operand raw emission
   TRef emit_raw(uint32_t ot, TRef a) { return emit_raw(ot, a, 0); }

   //=================================================================================================================
   // Typed emission helpers - provide clearer semantics than raw IRT() macros

   // Emit integer-typed instruction: IRT(op, IRT_INT)
   TRef emit_int(IROp op, TRef a, TRef b = 0) { return emit(IRTI(op), a, b); }

   // Emit number-typed instruction: IRT(op, IRT_NUM)
   TRef emit_num(IROp op, TRef a, TRef b = 0) { return emit(IRTN(op), a, b); }

   // Emit guard instruction: IRTG(op, type)
   TRef guard(IROp op, IRType t, TRef a, TRef b = 0) { return emit(IRTG(op, t), a, b); }

   // Emit integer guard: IRTGI(op)
   TRef guard_int(IROp op, TRef a, TRef b = 0) { return emit(IRTGI(op), a, b); }

   //=================================================================================================================
   // Common IR patterns - encapsulate frequently used instruction sequences

   // Load field from object: FLOAD
   TRef fload(TRef obj, IRFieldID field, IRType t) {
      return emit(IRT(IR_FLOAD, t), obj, field);
   }

   // Integer field load (common case)
   TRef fload_int(TRef obj, IRFieldID field) {
      return emit(IRTI(IR_FLOAD), obj, field);
   }

   // Pointer field load
   TRef fload_ptr(TRef obj, IRFieldID field) {
      return emit(IRT(IR_FLOAD, IRT_PGC), obj, field);
   }

   // Table field load
   TRef fload_tab(TRef obj, IRFieldID field) {
      return emit(IRT(IR_FLOAD, IRT_TAB), obj, field);
   }

   // Type conversion: CONV
   TRef conv(TRef val, uint32_t convmode) { return emit(IRT(IR_CONV, convmode >> IRCONV_DSH), val, convmode); }

   // Integer to number conversion
   TRef conv_num_int(TRef val) { return emit(IRTN(IR_CONV), val, IRCONV_NUM_INT); }

   // Number to integer conversion with check
   TRef conv_int_num(TRef val) { return emit(IRTGI(IR_CONV), val, IRCONV_INT_NUM | IRCONV_CHECK); }

   // Emit NOP instruction (used to prevent snapshot coalescing)
   void nop() { emit_raw(IRT(IR_NOP, IRT_NIL), 0, 0); }

   //=================================================================================================================
   // Guard emission helpers

   // Emit equality guard
   void guard_eq(TRef a, TRef b, IRType t) { guard(IR_EQ, t, a, b); }

   // Emit inequality guard
   void guard_ne(TRef a, TRef b, IRType t) { guard(IR_NE, t, a, b); }

   // Integer equality guard
   void guard_eq_int(TRef a, TRef b) { guard_int(IR_EQ, a, b); }

   // Integer inequality guard
   void guard_ne_int(TRef a, TRef b) { guard_int(IR_NE, a, b); }

   //=================================================================================================================
   // Constant emission (delegates to lj_ir_k* functions)

   TRef kint(int32_t k) { return lj_ir_kint(J, k); }
   TRef knum(lua_Number n) { return lj_ir_knum(J, n); }
   TRef kstr(GCstr *str) { return lj_ir_kstr(J, str); }
   TRef ktab(GCtab *tab) { return lj_ir_ktab(J, tab); }
   TRef kfunc(GCfunc *fn) { return lj_ir_kfunc(J, fn); }
   TRef kptr(void *ptr) { return lj_ir_kptr(J, ptr); }
   TRef kkptr(void *ptr) { return lj_ir_kkptr(J, ptr); }
   TRef knull(IRType t) { return lj_ir_knull(J, t); }

   //=================================================================================================================
   // State accessors

   // Get current number of IR instructions
   [[nodiscard]] IRRef nins() const { return J->cur.nins; }

   // Get number of constants
   [[nodiscard]] IRRef nk() const { return J->cur.nk; }

   // Get underlying jit_State (for advanced usage)
   [[nodiscard]] jit_State* state() const { return J; }
};
