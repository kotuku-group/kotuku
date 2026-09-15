// Unit tests for JIT frame management abstractions and upvalue range guards.
// Copyright © 2025-2026 Paul Manias
//
// These tests verify the correctness of the FrameManager class and FRC constants used in the JIT trace recorder. They
// complement the existing Tiri integration tests by providing low-level verification of frame arithmetic operations.

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "frame_manager.h"
#include "../debug/lj_jit.h"
#include "../lj_ircall.h"
#include "../lj_dispatch.h"
#include <array>
#include <cstring>
#include <memory>
#include "../lauxlib.h"
#include "../lualib.h"
#include "../runtime/lj_state.h"
#include "../../../defs.h"

namespace {

// For unit tests, we use the real jit_State structure but only initialize the fields
// we need. This avoids layout mismatch issues with mocks.
// Verify that jit_State has the expected layout for the fields we access:

static_assert(offsetof(jit_State, base) IS offsetof(jit_State, cur) + sizeof(GCtrace) + sizeof(void*) * 5,
   "jit_State::base offset unexpected");
static_assert(offsetof(jit_State, baseslot) IS offsetof(jit_State, base) + sizeof(TRef*) + sizeof(uint32_t) + sizeof(BCREG),
   "jit_State::baseslot offset unexpected");

// Helper to initialize a jit_State for testing frame operations only
static void init_test_jit_state(jit_State& J) {
   memset(&J, 0, sizeof(J));
   J.baseslot = FRC::MIN_BASESLOT;
   J.base = J.slot + J.baseslot;
   J.maxslot = 0;
   J.framedepth = 0;
}

//********************************************************************************************************************
// Test that FRC constants match the expected values for LJ_FR2=1

static bool test_frc_constants(kt::Log& log)
{
   // In 2-slot frame mode (LJ_FR2=1):
   // HEADER_SIZE = 1 + LJ_FR2 = 2
   static_assert(FRC::HEADER_SIZE IS 2, "HEADER_SIZE should be 2");

   // CONT_FRAME_SIZE = 2 << LJ_FR2 = 4
   static_assert(FRC::CONT_FRAME_SIZE IS 4, "CONT_FRAME_SIZE should be 4");

   // FUNC_SLOT_OFFSET = -1 - LJ_FR2 = -2
   static_assert(FRC::FUNC_SLOT_OFFSET IS -2, "FUNC_SLOT_OFFSET should be -2");

   // MIN_BASESLOT = 1 + LJ_FR2 = 2
   static_assert(FRC::MIN_BASESLOT IS 2, "MIN_BASESLOT should be 2");

   return true;
}

//********************************************************************************************************************
// Test push/pop symmetry for call frames

static bool test_frame_push_pop_symmetry(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   FrameManager fm(&J);

   BCREG initial_baseslot = J.baseslot;

   // Push frame at slot 5: base moves by 5 + 2 (header) = 7
   fm.push_call_frame(5);
   if (J.baseslot != initial_baseslot + 5 + FRC::HEADER_SIZE) {
      log.error("push_call_frame: expected baseslot=%u, got %u", initial_baseslot + 5 + FRC::HEADER_SIZE, J.baseslot);
      return false;
   }

   // Pop Lua frame with cbase=5: base moves back by 5 + 2 = 7
   fm.pop_lua_frame(5);
   if (J.baseslot != initial_baseslot) {
      log.error("pop_lua_frame: expected baseslot=%u, got %u", initial_baseslot, J.baseslot);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test delta-only pop (for vararg/pcall frames)

static bool test_delta_frame_pop(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);

   // Start at base + some offset
   J.baseslot = 10;
   J.base = J.slot + J.baseslot;

   FrameManager fm(&J);

   // Push frame at slot 3
   fm.push_call_frame(3);  // Now at 10 + 3 + 2 = 15

   if (J.baseslot != 15) {
      log.error("push_call_frame: expected baseslot=15, got %u", J.baseslot);
      return false;
   }

   // Pop delta-only (vararg frames use just the delta, no header adjustment)
   fm.pop_delta_frame(3);  // Back by 3 only = 12

   if (J.baseslot != 12) {
      log.error("pop_delta_frame: expected baseslot=12, got %u", J.baseslot);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test func_slot accessor

static bool test_func_slot_access(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   FrameManager fm(&J);

   // Set a value at the function slot position
   J.base[FRC::FUNC_SLOT_OFFSET] = 0x12345678;

   TRef result = fm.func_slot();
   if (result != 0x12345678) {
      log.error("func_slot: expected 0x12345678, got 0x%x", result);
      return false;
   }

   // Test writing through func_slot
   fm.func_slot() = 0xDEADBEEF;
   if (J.base[FRC::FUNC_SLOT_OFFSET] != 0xDEADBEEF) {
      log.error("func_slot write: expected 0xDEADBEEF, got 0x%x",
              J.base[FRC::FUNC_SLOT_OFFSET]);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test overflow detection

static bool test_overflow_detection(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.baseslot = LJ_MAX_JSLOTS - 10;
   J.base = J.slot + J.baseslot;

   FrameManager fm(&J);

   // Should detect overflow
   if (not fm.would_overflow(15)) {
      log.error("would_overflow: should detect overflow for 15 slots");
      return false;
   }

   // Should not overflow
   if (fm.would_overflow(5)) {
      log.error("would_overflow: should not overflow for 5 slots");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test at_root_baseslot

static bool test_root_baseslot_detection(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   FrameManager fm(&J);

   // At initialization, should be at root
   if (not fm.at_root_baseslot()) {
      log.error("at_root_baseslot: should be true at init");
      return false;
   }

   // After pushing a frame, should not be at root
   fm.push_call_frame(0);
   if (fm.at_root_baseslot()) {
      log.error("at_root_baseslot: should be false after push");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test compact_tailcall memory move

static bool test_compact_tailcall(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.baseslot = 10;
   J.base = J.slot + J.baseslot;

   // Set up some test values
   J.base[5] = 0xAAAA;  // func at slot 5
   J.base[6] = 0xBBBB;  // frame marker
   J.base[7] = 0xCCCC;  // arg 1
   J.base[8] = 0xDDDD;  // arg 2

   FrameManager fm(&J);
   fm.compact_tailcall(5, 2);  // Move func + 2 args + header

   // Check that values moved to function slot position
   if (J.base[FRC::FUNC_SLOT_OFFSET] != 0xAAAA or
       J.base[FRC::FUNC_SLOT_OFFSET + 1] != 0xBBBB or
       J.base[FRC::FUNC_SLOT_OFFSET + 2] != 0xCCCC or
       J.base[FRC::FUNC_SLOT_OFFSET + 3] != 0xDDDD) {
      log.error("compact_tailcall: values not moved correctly");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// RAII Scope Guards

// Test FrameDepthGuard auto-increment and auto-decrement
static bool test_frame_depth_guard_auto(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.framedepth = 0;

   {
      FrameDepthGuard fdg(&J);  // Auto-increment
      if (J.framedepth != 1) {
         log.error("FrameDepthGuard: expected framedepth=1 after construct, got %d", J.framedepth);
         return false;
      }
   }  // Auto-decrement on scope exit

   if (J.framedepth != 0) {
      log.error("FrameDepthGuard: expected framedepth=0 after destruct, got %d", J.framedepth);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test FrameDepthGuard release (no auto-decrement)

static bool test_frame_depth_guard_release(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.framedepth = 5;

   {
      FrameDepthGuard fdg(&J);  // framedepth becomes 6
      fdg.release();            // Disable auto-decrement
   }

   if (J.framedepth != 6) {
      log.error("FrameDepthGuard release: expected framedepth=6, got %d", J.framedepth);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test FrameDepthGuard manual decrement with check

static bool test_frame_depth_guard_decrement(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.framedepth = 2;

   FrameDepthGuard fdg(&J, false);  // No auto-increment

   if (J.framedepth != 2) {
      log.error("FrameDepthGuard no-increment: expected framedepth=2, got %d", J.framedepth);
      return false;
   }

   int32_t depth = fdg.decrement_and_check();
   if (depth != 1 or J.framedepth != 1) {
      log.error("FrameDepthGuard decrement_and_check: expected 1, got %d", depth);
      return false;
   }

   fdg.release();  // Prevent double-decrement
   return true;
}

//********************************************************************************************************************
// Test FrameDepthGuard helper methods

static bool test_frame_depth_guard_helpers(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.framedepth = 0;
   J.retdepth = 0;

   FrameDepthGuard fdg(&J, false);

   if (not fdg.at_root()) {
      log.error("at_root: should be true when framedepth IS 0");
      return false;
   }

   J.framedepth = 1;
   if (fdg.at_root()) {
      log.error("at_root: should be false when framedepth IS 1");
      return false;
   }

   J.framedepth = 2;
   J.retdepth = 3;
   if (fdg.combined_depth() != 5) {
      log.error("combined_depth: expected 5, got %d", fdg.combined_depth());
      return false;
   }

   fdg.release();
   return true;
}

//********************************************************************************************************************
// Test IRRollbackPoint basic functionality

static bool test_ir_rollback_point_basic(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.cur.nins = 100;
   J.guardemit.irt = 42;

   IRRollbackPoint rbp;

   // Initially unmarked
   if (rbp.is_marked()) {
      log.error("IRRollbackPoint: should not be marked initially");
      return false;
   }

   // Mark the rollback point
   rbp.mark(&J);

   if (not rbp.is_marked()) {
      log.error("IRRollbackPoint: should be marked after mark()");
      return false;
   }

   if (rbp.ref != 100) {
      log.error("IRRollbackPoint: expected ref=100, got %u", rbp.ref);
      return false;
   }

   if (rbp.guardemit.irt != 42) {
      log.error("IRRollbackPoint: expected guardemit.irt=42, got %u", rbp.guardemit.irt);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test IRRollbackPoint needs_rollback logic

static bool test_ir_rollback_point_needs_rollback(kt::Log& log)
{
   IRRollbackPoint rbp;
   rbp.ref = 100;
   rbp.guardemit.irt = 0;

   // Result ref less than rollback point - needs rollback (forwarding occurred)
   // Use TREF() to construct a TRef with specific ref value (50 < 100)
   TRef result_forwarded = TREF(50, IRT_INT);
   if (not rbp.needs_rollback(result_forwarded)) {
      log.error("needs_rollback: should return true when result.ref < rbp.ref");
      return false;
   }

   // Result ref greater than rollback point - no rollback needed
   TRef result_not_forwarded = TREF(150, IRT_INT);  // ref = 150 > 100
   if (rbp.needs_rollback(result_not_forwarded)) {
      log.error("needs_rollback: should return false when result.ref > rbp.ref");
      return false;
   }

   // Unmarked rollback point - never needs rollback
   IRRollbackPoint rbp_unmarked;
   if (rbp_unmarked.needs_rollback(result_forwarded)) {
      log.error("needs_rollback: should return false when rollback point unmarked");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// SlotView Tests

// Test SlotView basic read/write access
static bool test_slotview_basic_access(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   SlotView slots(&J);

   // Write to slot 0
   slots[0] = 0x12345678;
   if (J.base[0] != 0x12345678) {
      log.error("SlotView write: expected 0x12345678, got 0x%x", J.base[0]);
      return false;
   }

   // Read from slot 0
   TRef result = slots[0];
   if (result != 0x12345678) {
      log.error("SlotView read: expected 0x12345678, got 0x%x", result);
      return false;
   }

   // Write to negative slot (function slot)
   slots[FRC::FUNC_SLOT_OFFSET] = 0xDEADBEEF;
   if (J.base[FRC::FUNC_SLOT_OFFSET] != 0xDEADBEEF) {
      log.error("SlotView negative write: expected 0xDEADBEEF, got 0x%x", J.base[FRC::FUNC_SLOT_OFFSET]);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test SlotView func() accessor
static bool test_slotview_func_accessor(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   SlotView slots(&J);

   // Set function slot through direct base access
   J.base[FRC::FUNC_SLOT_OFFSET] = 0xCAFEBABE;

   // Read through func()
   if (slots.func() != 0xCAFEBABE) {
      log.error("SlotView func() read: expected 0xCAFEBABE, got 0x%x", slots.func());
      return false;
   }

   // Write through func()
   slots.func() = 0xFEEDFACE;
   if (J.base[FRC::FUNC_SLOT_OFFSET] != 0xFEEDFACE) {
      log.error("SlotView func() write: expected 0xFEEDFACE, got 0x%x", J.base[FRC::FUNC_SLOT_OFFSET]);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test SlotView is_loaded() helper
static bool test_slotview_is_loaded(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   SlotView slots(&J);

   // Initially slot should be empty (0)
   J.base[5] = 0;
   if (slots.is_loaded(5)) {
      log.error("is_loaded: should be false for empty slot");
      return false;
   }

   // After setting a value, should be loaded
   J.base[5] = 0x123;
   if (not slots.is_loaded(5)) {
      log.error("is_loaded: should be true for non-empty slot");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test SlotView clear operations
static bool test_slotview_clear(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   SlotView slots(&J);

   // Test single slot clear
   J.base[3] = 0xABCD;
   slots.clear(3);
   if (J.base[3] != 0) {
      log.error("clear: slot should be 0, got 0x%x", J.base[3]);
      return false;
   }

   // Test range clear
   J.base[0] = 0x111;
   J.base[1] = 0x222;
   J.base[2] = 0x333;
   J.base[3] = 0x444;
   slots.clear_range(0, 4);
   for (int i = 0; i < 4; i++) {
      if (J.base[i] != 0) {
         log.error("clear_range: slot %d should be 0, got 0x%x", i, J.base[i]);
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Test SlotView copy operation
static bool test_slotview_copy(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   SlotView slots(&J);

   // Set up source slots
   J.base[10] = 0xAAAA;
   J.base[11] = 0xBBBB;
   J.base[12] = 0xCCCC;

   // Copy to different location
   slots.copy(0, 10, 3);

   if (J.base[0] != 0xAAAA or J.base[1] != 0xBBBB or J.base[2] != 0xCCCC) {
      log.error("copy: values not copied correctly");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test SlotView maxslot operations
static bool test_slotview_maxslot(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.maxslot = 5;
   SlotView slots(&J);

   // Test maxslot getter
   if (slots.maxslot() != 5) {
      log.error("maxslot: expected 5, got %u", slots.maxslot());
      return false;
   }

   // Test set_maxslot
   slots.set_maxslot(10);
   if (J.maxslot != 10) {
      log.error("set_maxslot: expected 10, got %u", J.maxslot);
      return false;
   }

   // Test ensure_slot (should expand if needed)
   slots.ensure_slot(15);
   if (J.maxslot != 16) {  // ensure_slot sets maxslot to slot + 1
      log.error("ensure_slot: expected 16, got %u", J.maxslot);
      return false;
   }

   // Test ensure_slot (should not shrink)
   slots.ensure_slot(5);
   if (J.maxslot != 16) {
      log.error("ensure_slot: should not shrink, expected 16, got %u", J.maxslot);
      return false;
   }

   // Test shrink_to
   slots.shrink_to(8);
   if (J.maxslot != 8) {
      log.error("shrink_to: expected 8, got %u", J.maxslot);
      return false;
   }

   // Test shrink_to (should not expand)
   slots.shrink_to(20);
   if (J.maxslot != 8) {
      log.error("shrink_to: should not expand, expected 8, got %u", J.maxslot);
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test SlotView ptr() accessor
static bool test_slotview_ptr(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   SlotView slots(&J);

   // Verify ptr returns correct address
   TRef* ptr = slots.ptr(5);
   if (ptr != &J.base[5]) {
      log.error("ptr: returned incorrect address");
      return false;
   }

   // Verify can write through ptr
   *ptr = 0x9999;
   if (J.base[5] != 0x9999) {
      log.error("ptr: write through ptr failed");
      return false;
   }

   // Test negative index ptr
   TRef* func_ptr = slots.ptr(FRC::FUNC_SLOT_OFFSET);
   if (func_ptr != &J.base[FRC::FUNC_SLOT_OFFSET]) {
      log.error("ptr: negative index returned incorrect address");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// IRBuilder Tests

// Test IRBuilder construction and state accessor
static bool test_irbuilder_construction(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   J.cur.nins = 100;
   J.cur.nk = 50;

   IRBuilder ir(&J);

   // Verify state accessor
   if (ir.state() != &J) {
      log.error("IRBuilder: state() returned wrong pointer");
      return false;
   }

   // Verify nins accessor
   if (ir.nins() != 100) {
      log.error("IRBuilder: nins() expected 100, got %u", ir.nins());
      return false;
   }

   // Verify nk accessor
   if (ir.nk() != 50) {
      log.error("IRBuilder: nk() expected 50, got %u", ir.nk());
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test IRBuilder at() method for IR access
static bool test_irbuilder_at(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);

   // Allocate a local IR buffer large enough to handle REF_BIAS indexing.
   // J.cur.ir is accessed as ir[REF_BIAS + offset], so we need the buffer to be large enough.
   // We use a static buffer to avoid stack overflow and allow safe pointer arithmetic.
   static std::array<IRIns, REF_BIAS + 64> ir_buffer{};

   // Point ir to the start of the buffer - ir[REF_BIAS] will access element REF_BIAS
   J.cur.ir = ir_buffer.data();

   // Set up a mock IR instruction at REF_BIAS + 10
   IRRef test_ref = REF_BIAS + 10;
   J.cur.ir[test_ref].ot = IRT(IR_ADD, IRT_INT);
   J.cur.ir[test_ref].op1 = 5;
   J.cur.ir[test_ref].op2 = 6;

   IRBuilder ir(&J);
   IRIns* ins = ir.at(test_ref);

   // Verify we got the right instruction
   if (ins != &J.cur.ir[test_ref]) {
      log.error("IRBuilder at(): returned wrong pointer");
      return false;
   }

   // Verify we can read through it
   if (ins->op1 != 5 or ins->op2 != 6) {
      log.error("IRBuilder at(): wrong operand values");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test IRBuilder constant emission wrappers
static bool test_irbuilder_constants(kt::Log& log)
{
   // Note: We can only test that the methods compile and the wrapper pattern is correct.
   // Full constant emission requires a properly initialised JIT state which is complex to mock.
   // The actual functionality is tested via integration tests.

   // Static assertions to verify method signatures exist
   jit_State J;
   init_test_jit_state(J);
   IRBuilder ir(&J);

   // Verify the builder compiles with the expected method signatures
   // (These would crash if actually called without proper JIT initialisation,
   // but the tests verify the interface is correct)

   // Type check: kint returns TRef
   auto kint_fn = &IRBuilder::kint;
   (void)kint_fn;

   // Type check: knum returns TRef
   auto knum_fn = &IRBuilder::knum;
   (void)knum_fn;

   // Type check: knull returns TRef
   auto knull_fn = &IRBuilder::knull;
   (void)knull_fn;

   return true;
}

//********************************************************************************************************************
// Test IRBuilder typed emission helper signatures
static bool test_irbuilder_typed_helpers(kt::Log& log)
{
   // Verify the typed helper method signatures compile correctly
   jit_State J;
   init_test_jit_state(J);
   IRBuilder ir(&J);

   // Verify emit_int signature: (IROp, TRef, TRef) -> TRef
   auto emit_int_fn = static_cast<TRef (IRBuilder::*)(IROp, TRef, TRef)>(&IRBuilder::emit_int);
   (void)emit_int_fn;

   // Verify emit_num signature
   auto emit_num_fn = static_cast<TRef (IRBuilder::*)(IROp, TRef, TRef)>(&IRBuilder::emit_num);
   (void)emit_num_fn;

   // Verify guard signature: (IROp, IRType, TRef, TRef) -> TRef
   auto guard_fn = static_cast<TRef (IRBuilder::*)(IROp, IRType, TRef, TRef)>(&IRBuilder::guard);
   (void)guard_fn;

   // Verify guard_int signature
   auto guard_int_fn = static_cast<TRef (IRBuilder::*)(IROp, TRef, TRef)>(&IRBuilder::guard_int);
   (void)guard_int_fn;

   return true;
}

//********************************************************************************************************************
// Test IRBuilder fload helper signatures
static bool test_irbuilder_fload_helpers(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   IRBuilder ir(&J);

   // Verify fload signatures compile
   auto fload_fn = &IRBuilder::fload;
   (void)fload_fn;

   auto fload_int_fn = &IRBuilder::fload_int;
   (void)fload_int_fn;

   auto fload_ptr_fn = &IRBuilder::fload_ptr;
   (void)fload_ptr_fn;

   auto fload_tab_fn = &IRBuilder::fload_tab;
   (void)fload_tab_fn;

   return true;
}

//********************************************************************************************************************
// Test IRBuilder conversion helper signatures
static bool test_irbuilder_conv_helpers(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   IRBuilder ir(&J);

   // Verify conv signatures compile
   auto conv_fn = &IRBuilder::conv;
   (void)conv_fn;

   auto conv_num_int_fn = &IRBuilder::conv_num_int;
   (void)conv_num_int_fn;

   auto conv_int_num_fn = &IRBuilder::conv_int_num;
   (void)conv_int_num_fn;

   return true;
}

//********************************************************************************************************************
// Test IRBuilder guard helper signatures
static bool test_irbuilder_guard_helpers(kt::Log& log)
{
   jit_State J;
   init_test_jit_state(J);
   IRBuilder ir(&J);

   // Verify guard helper signatures compile
   auto guard_eq_fn = &IRBuilder::guard_eq;
   (void)guard_eq_fn;

   auto guard_ne_fn = &IRBuilder::guard_ne;
   (void)guard_ne_fn;

   auto guard_eq_int_fn = &IRBuilder::guard_eq_int;
   (void)guard_eq_int_fn;

   auto guard_ne_int_fn = &IRBuilder::guard_ne_int;
   (void)guard_ne_int_fn;

   return true;
}

//********************************************************************************************************************
// A valid UCLO range that the parser cannot currently emit: capture before loop entry, close inside the loop.
// This isolates the compiled range guard from the still-unsupported FNEW recorder path.

static bool test_uclo_range_guard(kt::Log& Log)
{
   struct ScriptOwner {
      objTiri* object = nullptr;
      ~ScriptOwner() { if (object) FreeResource(object); }
   } script;
   if (NewObject(CLASSID::TIRI, &script.object) != ERR::Okay) return false;
   if (script.object->setStatement("") != ERR::Okay or Action(AC::Init, script.object, nullptr) != ERR::Okay)
      return false;
   std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate((extTiri*)script.object), lua_close);
   if (not state) return false;
   lua_State* lua = state.get();
   luaL_openlibs(lua);
   lua_pushcfunction(lua, [](lua_State* Lua) {
      lj_state_growstack(Lua, 128);
      lua_gc(Lua, LUA_GCCOLLECT, 0);
      return 0;
   });
   lua_setglobal(lua, "range_resize");
   constexpr auto source = R"tiri(
global glGrow = false
global glReadBefore = false
global function range_worker(Mode:num):num
   local boundary = 11
   local above = 22
   local payload = { value=31 }
   local read:func = nil
   local twin:func = nil
   if Mode is 1 then
      read = function(Delta:num):num boundary += Delta; return boundary end
      twin = (() => boundary)
   elseif Mode is 2 then
      read = function(Delta:num):num above += Delta; return above end
      twin = (() => above)
   elseif Mode is 3 then
      read = function(Delta:num):num boundary += Delta; return boundary + above end
      twin = (() => boundary + above)
   elseif Mode is 4 then
      read = function(Delta:num):num payload.value += Delta; return payload.value end
      twin = (() => payload.value)
   end
   if glGrow then range_resize() end
   local index = 0
   while index < 1000 do
      if index < (glReadBefore and 4 or 1) then
         index++
         continue
      end
      boundary += 1
      above += 2
      payload = { value=index + 40 }
      if glReadBefore then
         assert(read(0) is twin(), 'Open references must see the latest SSA values')
      end
      do
         local marker = index
         if Mode < 0 then
            local function unused():num return marker end
         end
      end
      if index is (glReadBefore and 4 or 1) and read then
         local previous = read(0)
         assert(twin() is previous)
         assert(read(1) is previous + 1)
         assert(twin() is previous + 1, 'Closures must share the cell after closing')
      end
      index++
   end
   boundary = 111
   above = 222
   if read then
      local result = read(0)
      assert(twin() is result)
      assert(read(1) is result + 1)
      assert(twin() is result + 1)
      range_resize()
      assert(twin() is result + 1, 'Closed captures must survive GC and stack-slot reuse')
      return result
   end
   return index
end
global function range_caller(Mode:num):num
   local lower = 7
   local function read():num return lower end
   local result = range_worker(Mode)
   lower = 9
   assert(read() is 9, 'A caller capture below the range must remain open')
   return result
end
)tiri";
   if (lua_load(lua, source, "=uclo-range-functions") or lua_pcall(lua, 0, 0, 0)) {
      Log.error("UCLO fixture setup: %s", lua_tostring(lua, -1));
      return false;
   }
   lua_getglobal(lua, "range_worker");
   GCproto* prototype = funcproto(funcV(lua->top - 1));
   BCIns* bytecode = proto_bc(prototype);
   unsigned patched = 0;
   const BCIns* close_pc = nullptr;
   bool boundary_slot = false;
   bool above_slot = false;
   for (BCPOS pc = 1; pc < prototype->sizebc; ++pc) {
      BCIns ins = bytecode[pc];
      if (bc_op(ins) IS BC_KSHORT and bc_a(ins) IS 1 and bc_d(ins) IS 11) boundary_slot = true;
      if (bc_op(ins) IS BC_KSHORT and bc_a(ins) IS 2 and bc_d(ins) IS 22) above_slot = true;
      if (bc_op(ins) IS BC_UCLO and bc_a(ins) > 3) {
         setbc_a(&bytecode[pc], 1);
         close_pc = &bytecode[pc];
         ++patched;
      }
   }
   lua_pop(lua, 1);
   if (patched != 1 or not boundary_slot or not above_slot) {
      Log.error("UCLO fixture bytecode layout changed");
      return false;
   }
   constexpr auto checks = R"tiri(
global glRangeTraces = {}
global glRangeTrace = 0
global glRangeExit = -1
global glRangeFailures = 0
global glRangeRestarts = 0
global glRangeFallbacks = 0
global function range_trace(Event, Trace, Func, Pc, ParentOrReason, ExitOrInfo)
   if Event is 'start' then
      glRangeTraces[Trace] = { func=Func }
      if ParentOrReason is glRangeTrace and ExitOrInfo is glRangeExit then
         local ins = jit.util.funcBC(Func, Pc)
         assert(Func is range_worker and (ins & 255) is 51 and ((ins >> 8) & 255) is 1,
            'The range guard must restore the exact UCLO closing level')
         glRangeRestarts++
      end
   elseif Event is 'stop' then glRangeTraces[Trace].done = true
   elseif Event is 'abort' and Func is range_worker and ParentOrReason is 7 and ExitOrInfo is 51 then
      glRangeFallbacks++
   end
end
global function range_exit(Trace, Exit)
   if Trace is glRangeTrace and Exit is glRangeExit then glRangeFailures++ end
end
jit.off()
assert(range_caller(0) is 1000)
assert(range_caller(1) is 13)
assert(range_caller(2) is 25)
assert(range_caller(3) is 37)
assert(range_caller(4) is 42)
jit.on()
jit.flush()
jit.opt.start('hotloop=3', 'hotexit=1000')
jit.attach(range_trace, 'trace')
assert(range_caller(0) is 1000)
jit.attach(range_trace)
for trace, entry in pairs(glRangeTraces) do
   local info = jit.util.traceInfo(trace)
   if entry.done and entry.func is range_worker and info.linktype is 'loop' then
      glRangeTrace = trace
      local comparison = 0
      for index in {1 into info.nins} do
         local _, ot = jit.util.traceIR(trace, index)
         if ((ot >> 8) is 4 or (ot >> 8) is 7) and comparison is 0 then comparison = index end
      end
      assert(comparison > 0, 'The original loop must contain an unsigned range guard')
      for exit in {0 to info.nexit} do
         local snap = jit.util.traceSnap(trace, exit)
         if snap[0] <= comparison then glRangeExit = exit end
      end
   end
end
assert(glRangeTrace > 0 and glRangeExit >= 0, 'The intended range-guard trace must compile')
jit.opt.start('hotexit=1')
jit.attach(range_trace, 'trace')
jit.attach(range_exit, 'texit')
glGrow = true
assert(range_caller(1) is 13, 'A capture exactly at the level must close before its slot is reused')
assert(glRangeFailures > 0 and glRangeRestarts > 0 and glRangeFallbacks is 0,
   f'Boundary: failures={glRangeFailures}, restarts={glRangeRestarts}, fallbacks={glRangeFallbacks}')
glRangeFailures = 0
glRangeRestarts = 0
glRangeFallbacks = 0
assert(range_caller(2) is 25, 'A capture above the level must close before its slot is reused')
assert(glRangeFallbacks is 0,
   f'Above: failures={glRangeFailures}, restarts={glRangeRestarts}, fallbacks={glRangeFallbacks}')
assert(range_caller(3) is 37, 'The highest cell must guard closing of every cell in the range')
assert(range_caller(1) is 13, 'Re-entering a close trace must use fresh cells')
assert(range_caller(2) is 25)
assert(range_caller(3) is 37)
assert(range_caller(4) is 42, 'Closing must materialise a newly allocated SSA table')
assert(range_caller(4) is 42, 'A repeated GC-value close must use the new cell and table')
jit.attach(range_trace)
jit.attach(range_exit)
)tiri";
   if (lua_load(lua, checks, "=uclo-range-checks") or lua_pcall(lua, 0, 0, 0)) {
      Log.error("UCLO range guard: %s", lua_tostring(lua, -1));
      return false;
   }
   bool close_call = false;
   jit_State* jit = L2J(lua);
   for (TraceNo number = 1; number < jit->sizetrace; ++number) {
      GCtrace* trace = traceref(jit, number);
      if (not trace) continue;
      for (IRRef ref = REF_BASE; ref < trace->nins; ++ref) {
         IRIns* ins = &trace->ir[ref];
         if (ins->o IS IR_CALLS and ins->op2 IS IRCALL_lj_func_closeuv) close_call = true;
      }
   }
   if (not close_call) {
      Log.error("No compiled trace contains the actual UCLO close helper");
      return false;
   }
   // Start with open cells so allocation, open UREFs, SSA updates and closing share one trace.
   for (int mode = 1; mode <= 4; ++mode) {
      const int expected[] = { 0, 13, 25, 37, 45 };
      char check[512];
      snprintf(check, sizeof(check),
         "jit.flush(); jit.opt.start('hotloop=1', 'hotexit=1'); glReadBefore=true; "
         "assert(range_caller(%d) is %d); assert(range_caller(%d) is %d)",
         mode, expected[mode], mode, expected[mode]);
      if (lua_load(lua, check, "=uclo-close-root") or lua_pcall(lua, 0, 0, 0)) {
         Log.error("UCLO actual-close root mode %d: %s", mode, lua_tostring(lua, -1));
         return false;
      }
      bool close_trace = false;
      for (TraceNo number = 1; number < jit->sizetrace; ++number) {
         GCtrace* trace = traceref(jit, number);
         if (not trace or gco_to_proto(gcref(trace->startpt)) != prototype) continue;
         bool allocated = false;
         bool open_ref = false;
         for (IRRef ref = REF_BASE; ref < trace->nins; ++ref) {
            IRIns* ins = &trace->ir[ref];
            if (ins->o IS IR_TNEW or ins->o IS IR_TDUP) allocated = true;
            if (ins->o IS IR_UREFO) open_ref = true;
            if (ins->o IS IR_CALLS and ins->op2 IS IRCALL_lj_func_closeuv and allocated and open_ref) {
               bool pre_snapshot = false;
               bool post_snapshot = false;
               bool closed_ref = false;
               for (SnapNo snapshot_index = 0; snapshot_index < trace->nsnap; ++snapshot_index) {
                  SnapShot* snapshot = &trace->snap[snapshot_index];
                  const BCIns* resume = snap_pc(&trace->snapmap[snapshot->mapofs + snapshot->nent]);
                  if (snapshot->ref <= ref) {
                     pre_snapshot = resume IS close_pc;
                     continue;
                  }
                  // Snapshot merging may advance over pure bytecodes after the jump destination.
                  post_snapshot = resume >= close_pc + 1 + bc_j(*close_pc)
                     and resume < bytecode + prototype->sizebc;
                  break;
               }
               for (IRRef next = ref + 1; next < trace->nins; ++next) {
                  if (trace->ir[next].o IS IR_UREFC) closed_ref = true;
               }
               close_trace = close_trace or (pre_snapshot and post_snapshot and closed_ref);
            }
         }
      }
      if (not close_trace) {
         Log.error("Mode %d lacks a close trace with allocation, open/closed references and close snapshots", mode);
         return false;
      }
   }
   return true;
}

struct TestCase {
   const char* name;
   bool (*fn)(kt::Log&);
};

} // anonymous namespace

//********************************************************************************************************************
// Public test entry point - matches parser_unit_tests signature

extern void jit_frame_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 28> tests = { {
      // FrameManager and FRC constants
      { "frc_constants", test_frc_constants },
      { "frame_push_pop_symmetry", test_frame_push_pop_symmetry },
      { "delta_frame_pop", test_delta_frame_pop },
      { "func_slot_access", test_func_slot_access },
      { "overflow_detection", test_overflow_detection },
      { "root_baseslot_detection", test_root_baseslot_detection },
      { "compact_tailcall", test_compact_tailcall },
      // Scope guards
      { "frame_depth_guard_auto", test_frame_depth_guard_auto },
      { "frame_depth_guard_release", test_frame_depth_guard_release },
      { "frame_depth_guard_decrement", test_frame_depth_guard_decrement },
      { "frame_depth_guard_helpers", test_frame_depth_guard_helpers },
      { "ir_rollback_point_basic", test_ir_rollback_point_basic },
      { "ir_rollback_point_needs_rollback", test_ir_rollback_point_needs_rollback },
      // SlotView
      { "slotview_basic_access", test_slotview_basic_access },
      { "slotview_func_accessor", test_slotview_func_accessor },
      { "slotview_is_loaded", test_slotview_is_loaded },
      { "slotview_clear", test_slotview_clear },
      { "slotview_copy", test_slotview_copy },
      { "slotview_maxslot", test_slotview_maxslot },
      { "slotview_ptr", test_slotview_ptr },
      // IRBuilder
      { "irbuilder_construction", test_irbuilder_construction },
      { "irbuilder_at", test_irbuilder_at },
      { "irbuilder_constants", test_irbuilder_constants },
      { "irbuilder_typed_helpers", test_irbuilder_typed_helpers },
      { "irbuilder_fload_helpers", test_irbuilder_fload_helpers },
      { "irbuilder_conv_helpers", test_irbuilder_conv_helpers },
      { "irbuilder_guard_helpers", test_irbuilder_guard_helpers },
      { "uclo_range_guard", test_uclo_range_guard }
   } };

   for (const TestCase& test : tests) {
      kt::Log log("JitFrameTests");
      log.branch("Running %s", test.name);
      ++Total;
      if (test.fn(log)) {
         ++Passed;
         log.msg("%s passed", test.name);
      }
      else log.error("%s failed", test.name);
   }
}

#endif // UNIT_TESTS
