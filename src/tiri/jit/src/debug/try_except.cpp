// Bytecode-level try-except runtime functions.
// These are called by the BC_TRYENTER and BC_TRYLEAVE handlers and by the error unwinding system.
//
// Copyright (C) 2025-2026 Paul Manias.

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE

#include <kotuku/main.h>

#include "lua.h"
#include "lj_gc.h"
#include "lj_array.h"
#include "lj_obj.h"
#include "lj_debug.h"
#include "lj_err.h"
#include "lj_func.h"
#include "lj_frame.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lj_tab.h"
#include "lj_trace.h"

//********************************************************************************************************************
// Native bytecode helpers for BC_CHECK and BC_RAISE opcodes.
// These are called from VM assembly after type checking and L->CaughtError is already set.
// Both functions are noreturn - they always throw an exception.

extern "C" LJ_NORET void lj_raise(lua_State *L, int32_t ErrorCode)
{
   luaL_error(L, ERR(ErrorCode));
}

extern "C" LJ_NORET void lj_raise_with_msg(lua_State *L, int32_t ErrorCode, TValue *Msg)
{
   if (tvisstr(Msg)) luaL_error(L, ERR(ErrorCode), "%s", strdata(strV(Msg)));
   else luaL_error(L, ERR(ErrorCode));
}

//********************************************************************************************************************
// Called by BC_TRYENTER to push an exception frame onto the try stack.
//
// Parameters:
//   L              - The lua_State pointer
//   Func          - The current Lua function (passed explicitly for JIT compatibility)
//   Base          - The current base pointer (passed explicitly for JIT compatibility)
//   TryBlockIndex - Index into the function's try_blocks array
//
// Note: Both Func and Base are passed explicitly rather than computed from L->base because in JIT-compiled
// code, L->base is not synchronized with the actual base (which is kept in a CPU register).
// The interpreter passes its BASE register value. The JIT passes REF_BASE which resolves to the actual base.

extern "C" void lj_try_enter(lua_State *L, GCfunc *Func, TValue *Base, uint16_t TryBlockIndex)
{
   // Keep the entirety of this function as simple as possible - no allocations, no throwing in production.

   lj_assertL(Func != nullptr, "lj_try_enter: Func is null");
   lj_assertL(isluafunc(Func), "lj_try_enter: Func is not a Lua function");
   lj_assertL(Base >= tvref(L->stack), "lj_try_enter: Base below stack start");
   lj_assertL(Base <= tvref(L->maxstack), "lj_try_enter: Base above maxstack");

   if (L->try_stack.depth >= LJ_MAX_TRY_DEPTH) lj_err_msg(L, ErrMsg::XNEST);  // "try blocks nested too deeply"

   pf::Log log(__FUNCTION__);
   log.trace("Entering try block %u: L->base=%p, Base(VM)=%p, L->top=%p, depth=%u", TryBlockIndex, L->base, Base, L->top, L->try_stack.depth);

   // Sync L->base with the passed Base pointer.  This is critical for JIT mode where L->base may be stale (the JIT keeps the
   // base in a CPU register). If an error occurs after this call, the error handling code uses L->base to walk frames - it
   // must be valid.  Note: Do NOT modify L->top here - it was synced by the VM before this call, and modifying it would
   // truncate the live stack.

   if (L->base != Base) {
      log.detail("L->base != Base; syncing L->base for try-enter");
      L->base = Base;
   }

   ptrdiff_t frame_base_offset = savestack(L, Base);
   TValue *safe_top = L->top;
   if (safe_top < Base) safe_top = Base;
   ptrdiff_t saved_top_offset = savestack(L, safe_top);
   lj_assertL(saved_top_offset >= frame_base_offset, "lj_try_enter: saved_top below base (top=%p base=%p)", safe_top, Base);

   // Note: We leave L->top at safe_top. In JIT mode, the JIT will restore state from snapshots if needed. In
   // interpreter mode, the VM will continue with the correct top. This ensures L->top is always valid if an
   // error occurs.

   GCproto *proto = funcproto(Func); // Retrieve for try metadata
   lj_assertL(TryBlockIndex < proto->try_block_count, "lj_try_enter: TryBlockIndex %u >= try_block_count %u", TryBlockIndex, proto->try_block_count);
   lj_assertL(proto->try_blocks != nullptr, "lj_try_enter: try_blocks is null");
   TryBlockDesc *block_desc = &proto->try_blocks[TryBlockIndex];

   TryFrame *try_frame = &L->try_stack.frames[L->try_stack.depth++];
   try_frame->try_block_index = TryBlockIndex;
   try_frame->frame_base      = frame_base_offset;
   try_frame->saved_top       = saved_top_offset;
   try_frame->saved_nactvar   = BCREG(block_desc->entry_slots);
   try_frame->func            = Func;
   try_frame->flags           = block_desc->flags;
}

//********************************************************************************************************************
// Called by BC_TRYLEAVE to pop an exception frame from the try stack.  Note that this operation is also replicated
// in the *.dasc files when JIT optimised, so it may be shadowed.

extern "C" void lj_try_leave(lua_State *L)
{
   pf::Log(__FUNCTION__).trace("Stack Depth: %d, Base: %p, Top: %p", L->try_stack.depth, L->base, L->top);

   // NB: The setup_try_handler() also decrements the depth, so the check prevents a repeat
   if (L->try_stack.depth > 0) L->try_stack.depth--;
}

//********************************************************************************************************************
// Check if a filter matches an error code.
// PackedFilter contains up to 4 16-bit error codes packed into a 64-bit integer.
// A filter of 0 means catch-all.

static bool filter_matches(uint64_t PackedFilter, ERR ErrorCode)
{
   if (PackedFilter IS 0) return true;  // Catch-all

   // Only ERR codes at or above ExceptionThreshold can match specific filters
   if (ErrorCode < ERR::ExceptionThreshold) return false;

   // Unpack and check each 16-bit code
   for (int shift = 0; shift < 64; shift += 16) {
      uint16_t filter_code = (PackedFilter >> shift) & 0xFFFF;
      if (filter_code IS 0) break;  // No more codes in this filter
      if (filter_code IS uint16_t(ErrorCode)) return true;
   }
   return false;
}

//********************************************************************************************************************
// Find a matching handler for the given error in the current try frame.
// Returns true if a handler was found, with handler PC and exception register set.

extern "C" bool lj_try_find_handler(lua_State *L, const TryFrame *Frame, ERR ErrorCode, const BCIns **HandlerPc,
   BCREG *ExceptionReg)
{
   lj_assertL(Frame != nullptr, "lj_try_find_handler: Frame is null");
   lj_assertL(HandlerPc != nullptr, "lj_try_find_handler: HandlerPc output is null");
   lj_assertL(ExceptionReg != nullptr, "lj_try_find_handler: ExceptionReg output is null");

   GCfunc *func = Frame->func;
   lj_assertL(func != nullptr, "lj_try_find_handler: Frame->func is null");
   if (not isluafunc(func)) return false;

   GCproto *proto = funcproto(func);
   lj_assertL(proto != nullptr, "lj_try_find_handler: proto is null for Lua function");
   if (not proto->try_blocks or Frame->try_block_index >= proto->try_block_count) return false;

   const TryBlockDesc *try_block = &proto->try_blocks[Frame->try_block_index];

   // A try block with no handlers (no except clause) silently swallows exceptions
   if (try_block->handler_count IS 0) return false;

   // Only access try_handlers if there are handlers to check
   lj_assertL(proto->try_handlers != nullptr, "lj_try_find_handler: try_handlers is null but handler_count > 0");

   // Validate handler indices are within bounds
   lj_assertL(try_block->first_handler + try_block->handler_count <= proto->try_handler_count,
      "lj_try_find_handler: handler indices out of bounds (first=%u, count=%u, total=%u)",
      try_block->first_handler, try_block->handler_count, proto->try_handler_count);

   for (uint8_t index = 0; index < try_block->handler_count; ++index) {
      const TryHandlerDesc *handler = &proto->try_handlers[try_block->first_handler + index];

      if (not filter_matches(handler->filter_packed, ErrorCode)) continue;

      // Validate handler PC is within bytecode bounds
      lj_assertL(handler->handler_pc < proto->sizebc,
         "lj_try_find_handler: handler_pc %u >= sizebc %u", handler->handler_pc, proto->sizebc);

      // Found a matching handler
      *HandlerPc = proto_bc(proto) + handler->handler_pc;
      *ExceptionReg = handler->exception_reg;
      return true;
   }

   return false;
}

//********************************************************************************************************************
// Build an exception table and place it in the specified register.
// The exception table has fields: code, message, line, trace, stackTrace

extern "C" void lj_try_build_exception_table(lua_State *L, ERR ErrorCode, CSTRING Message, int Line, BCREG ExceptionReg, CapturedStackTrace *Trace)
{
   if (ExceptionReg IS 0xff) { // No exception variable - just free the trace and return
      if (Trace) lj_debug_free_trace(L, Trace);
      return;
   }

   lj_assertL(L->base >= tvref(L->stack), "lj_try_build_exception_table: L->base below stack start");
   lj_assertL(L->base <= tvref(L->maxstack), "lj_try_build_exception_table: L->base above maxstack");

   TValue *target_slot = L->base + ExceptionReg;
   lj_assertL(target_slot >= tvref(L->stack), "lj_try_build_exception_table: target slot below stack start");
   lj_assertL(target_slot < tvref(L->maxstack), "lj_try_build_exception_table: target slot at or above maxstack");

   // Create exception table and store immediately at target_slot to root it.
   // This protects it from GC during subsequent allocations without modifying L->top.

   GCtab *t = lj_tab_new(L, 0, 5);
   lj_assertL(t != nullptr, "lj_try_build_exception_table: table allocation failed");
   settabV(L, target_slot, t);  // Root immediately - don't modify L->top

   TValue *slot;

   // Set e.code

   slot = lj_tab_setstr(L, t, lj_str_newlit(L, "code"));
   if (ErrorCode >= ERR::ExceptionThreshold) setintV(slot, int(ErrorCode));
   else setnilV(slot);

   // Set e.message

   slot = lj_tab_setstr(L, t, lj_str_newlit(L, "message"));
   if (Message) setstrV(L, slot, lj_str_newz(L, Message));
   else if (ErrorCode != ERR::Okay) setstrV(L, slot, lj_str_newz(L, GetErrorMsg(ErrorCode)));
   else setstrV(L, slot, lj_str_newlit(L, "<No message>"));

   // Set e.line

   slot = lj_tab_setstr(L, t, lj_str_newlit(L, "line"));
   setintV(slot, Line);

   // NB: We do not get the "trace" and "stackTrace" slots here because subsequent allocations (lj_array_new,
   // lj_tab_new, lj_str_new) can cause table t to be rehashed, which would invalidate any slot pointers.
   // We get the slots right before storing values into them.

   if (Trace and Trace->frame_count > 0) {
      // Build native array of frame tables: [{source, line, func}, ...]
      // The array is rooted in the exception table t (at the "trace" field) after creation.
      GCarray *frames = lj_array_new(L, Trace->frame_count, AET::TABLE);
      GCRef *frame_refs = (GCRef *)frames->arraydata();

      // Build formatted traceback string at the same time
      std::string traceback = "stack traceback:";

      for (uint16_t i = 0; i < Trace->frame_count; i++) {
         CapturedFrame *cf = &Trace->frames[i];

         // Create frame table - it will be rooted in the frames array immediately
         GCtab *frame = lj_tab_new(L, 0, 3);

         // Store table reference in array first (roots it for GC)
         setgcref(frame_refs[i], obj2gco(frame));

         TValue *frame_slot = lj_tab_setstr(L, frame, lj_str_newlit(L, "source"));
         if (cf->source) setstrV(L, frame_slot, cf->source);
         else setnilV(frame_slot);

         frame_slot = lj_tab_setstr(L, frame, lj_str_newlit(L, "line"));
         setintV(frame_slot, cf->line);

         frame_slot = lj_tab_setstr(L, frame, lj_str_newlit(L, "func"));
         if (cf->funcname) setstrV(L, frame_slot, cf->funcname);
         else setnilV(frame_slot);

         lj_gc_anybarriert(L, frame);

         // Build traceback string entry
         traceback += "\n\t";
         if (cf->source) traceback += strdata(cf->source);
         else traceback += "?";

         if (cf->line > 0) {
            traceback += ":";
            traceback += std::to_string(cf->line);
         }

         if (cf->funcname) {
            traceback += ": in function '";
            traceback += strdata(cf->funcname);
            traceback += "'";
         }
      }

      // Now that all allocations are done, get the slots and store values knowing that
      // the table won't be rehashed.

      slot = lj_tab_setstr(L, t, lj_str_newlit(L, "trace"));
      setarrayV(L, slot, frames);

      // Set stackTrace string - get slot first, then create string
      // (avoids allocation window where the string would be unrooted)
      TValue *stacktrace_slot = lj_tab_setstr(L, t, lj_str_newlit(L, "stackTrace"));
      setstrV(L, stacktrace_slot, lj_str_new(L, traceback.data(), traceback.size()));

      lj_debug_free_trace(L, Trace);
   }
   else {
      // Get slots right before storing nil values
      slot = lj_tab_setstr(L, t, lj_str_newlit(L, "trace"));
      TValue *stacktrace_slot = lj_tab_setstr(L, t, lj_str_newlit(L, "stackTrace"));
      setnilV(slot);
      setnilV(stacktrace_slot);
      if (Trace) lj_debug_free_trace(L, Trace);
   }

   lj_gc_anybarriert(L, t);  // Final barrier check
   // Note: t is already stored at target_slot (done at the start)
}
