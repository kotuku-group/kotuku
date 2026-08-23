// State and stack handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#define lj_state_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_ff.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_prng.h"
#include "../bytecode/lj_bc.h"
#include "../parser/lexer.h"
#include "../parser/parser_diagnostics.h"
#include "../parser/parser_symbols.h"
#include "../parser/parser_tips.h"
#include "lj_alloc.h"
#include "luajit.h"

#include <string>
#include "ankerl/unordered_dense.h"

// Type alias for the function names map (GCproto * -> function name string)
using FuncNameMap = ankerl::unordered_dense::map<const GCproto *, std::string>;

#ifdef LUA_USE_ASSERT
ContextDebugCounters *lj_context_debug_get(lua_State *L, bool Create)
{
   if (not L->context_debug_counters and Create) L->context_debug_counters = new ContextDebugCounters();
   return L->context_debug_counters;
}

void lj_context_debug_reset(lua_State *L)
{
   ContextDebugCounters *counters = lj_context_debug_get(L, true);
   *counters = ContextDebugCounters();
}

void lj_context_debug_virtual_enter(lua_State *L, uint32_t Depth)
{
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) {
      counters->virtual_activations_created++;
      if (Depth > counters->maximum_virtual_depth) counters->maximum_virtual_depth = Depth;
   }
}

void lj_context_debug_virtual_leave(lua_State *L)
{
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->virtual_activations_retired++;
}

void lj_context_debug_materialise(
   lua_State *L, ContextMaterialisationReason Reason, const GCfunc *Callable)
{
   ContextDebugCounters *counters = lj_context_debug_get(L, false);
   if (not counters) return;
   counters->materialisations[size_t(Reason)]++;
   if (Reason != ContextMaterialisationReason::NativeCall or not Callable) return;
   if (Callable->c.ffid < counters->native_fast_functions.size()) {
      counters->native_fast_functions[Callable->c.ffid]++;
   }
   if (Callable->c.ffid IS FF_C) counters->native_c_functions[uintptr_t(Callable->c.f)]++;
}

void lj_context_debug_physical_enter(lua_State *L)
{
   ContextDebugCounters *counters = lj_context_debug_get(L, false);
   if (not counters) return;
   counters->physical_context_entries++;
   size_t floor = L->context_root_floors.empty() ? 0 : L->context_root_floors.back();
   uint32_t depth = uint32_t(L->context_stack.size() - floor);
   if (depth > counters->maximum_physical_depth) counters->maximum_physical_depth = depth;
}

void lj_context_debug_physical_leave(lua_State *L, size_t Count)
{
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->physical_context_leaves += Count;
}

void lj_context_debug_trace_start(lua_State *L)
{
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->trace_starts++;
}

void lj_context_debug_trace_compiled(lua_State *L)
{
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->trace_compilations++;
}

void lj_context_debug_trace_abort(lua_State *L)
{
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->trace_aborts++;
}

void lj_context_debug_side_exit(lua_State *L)
{
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->side_exits++;
}
#endif

//********************************************************************************************************************
// Function name registry - maps GCproto pointers to their declared names for tostring() support.

// Register a function name for a prototype. Called from the parser when finishing a named function.

void lj_funcname_register(global_State *g, const GCproto *pt, CSTRING name, size_t len)
{
   if (not g or not pt or not name or len IS 0) return;
   if (not g->funcnames) {
      g->funcnames = new FuncNameMap();
   }
   auto* map = static_cast<FuncNameMap *>(g->funcnames);
   map->emplace(pt, std::string(name, len));
}

// Look up a registered function name. Returns nullptr if not found.

CSTRING lj_funcname_lookup(global_State *g, const GCproto *pt, size_t *len)
{
   if (not g or not pt or not g->funcnames) return nullptr;
   auto* map = static_cast<FuncNameMap *>(g->funcnames);
   auto it = map->find(pt);
   if (it IS map->end()) return nullptr;
   if (len) *len = it->second.size();
   return it->second.c_str();
}

// Free the function names map. Called when closing the Lua state.

static void funcnames_free(global_State* g)
{
   if (g->funcnames) {
      delete static_cast<FuncNameMap*>(g->funcnames);
      g->funcnames = nullptr;
   }
}

//********************************************************************************************************************
// Stack handling

#define LJ_STACK_MIN   LUA_MINSTACK   //  Min. stack size.
#define LJ_STACK_MAX   LUAI_MAXSTACK   //  Max. stack size.
#define LJ_STACK_START   (2*LJ_STACK_MIN)   //  Starting stack size.
#define LJ_STACK_MAXEX   (LJ_STACK_MAX + 1 + LJ_STACK_EXTRA)

// Explanation of LJ_STACK_EXTRA:
//
// Calls to metamethods store their arguments beyond the current top
// without checking for the stack limit. This avoids stack resizes which
// would invalidate passed TValue pointers. The stack check is performed
// later by the function header. This can safely resize the stack or raise
// an error. Thus we need some extra slots beyond the current stack limit.
//
// Most metamethods need 4 slots above top (cont, mobj, arg1, arg2) plus
// one extra slot if mobj is not a function. Only lj_meta_tset needs 5
// slots above top, but then mobj is always a function. So we can get by
// with 5 extra slots.
// LJ_FR2: We need 2 more slots for the frame PC and the continuation PC.

// Resize stack slots and adjust pointers in state.
static void resizestack(lua_State *L, MSize n)
{
   TValue* st, * oldst = tvref(L->stack);
   ptrdiff_t delta;
   MSize oldsize = L->stacksize;
   const size_t old_byte_size = size_t(oldsize) * sizeof(TValue);
   MSize realsize = n + 1 + LJ_STACK_EXTRA;
   GCobj* up;

   lj_assertL((MSize)(tvref(L->maxstack) - oldst) IS L->stacksize - LJ_STACK_EXTRA - 1, "inconsistent stack size");

   st = (TValue*)lj_mem_realloc(L, tvref(L->stack), (MSize)(oldsize * sizeof(TValue)), (MSize)(realsize * sizeof(TValue)));
   setmref(L->stack, st);
   delta = (char*)st - (char*)oldst;
   setmref(L->maxstack, st + n);
   while (oldsize < realsize) setnilV(st + oldsize++); // Clear new slots.

   L->stacksize = realsize;

   // Relocate the running trace's base when it refers to the buffer that has just moved.  The containment test must
   // use the *old* byte size: upstream compares a byte offset against a slot count, so any jit_base beyond the first
   // few slots is left pointing into freed memory, and lj_vm_exit_handler() then restores L->base from it on the
   // next trace exit.  oldsize is consumed by the slot-clearing loop above, hence the size captured on entry.

   if (size_t(mref<char>(G(L)->jit_base) - (char*)oldst) < old_byte_size) {
      setmref(G(L)->jit_base, mref<char>(G(L)->jit_base) + delta);
   }

   L->base = (TValue*)((char*)L->base + delta);
   L->top = (TValue*)((char*)L->top + delta);
   for (up = gcref(L->openupval); up != nullptr; up = gcnext(up)) {
      setmref(gco_to_upval(up)->v, (TValue*)((char*)uvval(gco_to_upval(up)) + delta));
   }
}

//********************************************************************************************************************
// Relimit stack after error, in case the limit was overdrawn.

void lj_state_relimitstack(lua_State *L)
{
   if (L->stacksize > LJ_STACK_MAXEX and L->top - tvref(L->stack) < LJ_STACK_MAX - 1) {
      resizestack(L, LJ_STACK_MAX);
   }
}

//********************************************************************************************************************
// Try to shrink the stack (called from GC).

void lj_state_shrinkstack(lua_State *L, MSize used)
{
   if (L->stacksize > LJ_STACK_MAXEX) return;  //  Avoid stack shrinking while handling stack overflow.

   if (4 * used < L->stacksize and
      2 * (LJ_STACK_START + LJ_STACK_EXTRA) < L->stacksize and
      // Don't shrink stack of live trace.
      (tvref(G(L)->jit_base) IS nullptr or obj2gco(L) != gcref(G(L)->cur_L)))
      resizestack(L, L->stacksize >> 1);
}

//********************************************************************************************************************
// Try to grow stack.

void lj_state_growstack(lua_State *L, MSize need)
{
   MSize n;
   lj_assertL(need < 1024, "Stack growth request exceeds reasonable 1K limit");

   if (L->stacksize > LJ_STACK_MAXEX) lj_err_throw(L, LUA_ERRERR);  //  Overflow while handling overflow?

   n = L->stacksize + need;
   if (n > LJ_STACK_MAX) n += 2 * LUA_MINSTACK;
   else if (n < 2 * L->stacksize) {
      n = 2 * L->stacksize;
      if (n >= LJ_STACK_MAX) n = LJ_STACK_MAX;
   }
   resizestack(L, n);
   if (L->stacksize > LJ_STACK_MAXEX) lj_err_msg(L, ErrMsg::STKOV);
}

//********************************************************************************************************************

void lj_state_growstack1(lua_State *L)
{
   lj_state_growstack(L, 1);
}

//********************************************************************************************************************
// Table context management.  Stack-relative activation ownership avoids retaining TValue pointers across stack moves.

static bool context_has_visible_override(const lua_State *L) noexcept
{
   size_t floor = L->context_root_floors.empty() ? 0 : L->context_root_floors.back();
   return L->context_stack.size() > floor;
}

GCtab * lj_context_current(lua_State *L) noexcept
{
   lj_assertL(L, "missing state for context lookup");
   size_t floor = L->context_root_floors.empty() ? 0 : L->context_root_floors.back();
   GCtab *context = L->context_stack.size() IS floor ?
      tabref(L->env) : tabref(L->context_stack.back().table);
   lj_assertL(context, "current context is not a table");
   return context;
}

extern "C" void lj_context_load(lua_State *L, TValue *Destination) noexcept
{
   lj_assertL(Destination >= tvref(L->stack) and Destination < tvref(L->maxstack),
      "context destination is outside the state stack");
   settabV(L, Destination, lj_context_current(L));
}

extern "C" GCtab * lj_context_current_jit(lua_State *L) noexcept
{
#ifdef LUA_USE_ASSERT
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->jit_current_calls++;
#endif
   return lj_context_current(L);
}

extern "C" void lj_context_enter_jit(lua_State *L, GCtab *Table, TValue *OwnerBase)
{
#ifdef LUA_USE_ASSERT
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->jit_enter_calls++;
#endif
   lj_assertL(L and Table and OwnerBase, "invalid recorded contextual activation");
   ptrdiff_t owner_base = savestack(L, OwnerBase);
   if (not L->context_stack.empty() and
       L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
       L->context_stack.back().owner_base IS owner_base) {
      lj_assertL(tabref(L->context_stack.back().table) IS Table,
         "recorded context does not match the active activation");
      return;
   }
   lj_context_push(L, Table, OwnerBase);
}

extern "C" void lj_context_leave_jit(lua_State *L, TValue *OwnerBase) noexcept
{
#ifdef LUA_USE_ASSERT
   if (ContextDebugCounters *counters = lj_context_debug_get(L, false)) counters->jit_leave_calls++;
#endif
   lj_assertL(L and OwnerBase, "invalid recorded contextual activation owner");
   ptrdiff_t owner_base = savestack(L, OwnerBase);
   if (not L->context_stack.empty() and
       L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
       L->context_stack.back().owner_base IS owner_base) {
      lj_context_pop(L, OwnerBase);
   }
}

extern "C" void lj_context_tail_jit(
   lua_State *L, GCtab *Table, TValue *PreparedOwner, TValue *OutgoingOwner)
{
   lj_assertL(L and Table and PreparedOwner and OutgoingOwner, "invalid recorded contextual tail transfer");
   ptrdiff_t prepared_owner = savestack(L, PreparedOwner);
   ptrdiff_t outgoing_owner = savestack(L, OutgoingOwner);
   lj_assertL(not L->context_stack.empty() and
      L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
      L->context_stack.back().owner_base IS prepared_owner,
      "recorded contextual tail call has no prepared activation");
   if (L->context_stack.empty() or
       L->context_stack.back().owner_kind != lua_State::ContextFrame::OwnerKind::Call or
       L->context_stack.back().owner_base != prepared_owner) return;
   lj_assertL(tabref(L->context_stack.back().table) IS Table,
      "recorded tail context does not match the prepared activation");

   L->context_stack.pop_back();
   lj_context_debug_physical_leave(L);
   if (not L->context_stack.empty() and
       L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
       L->context_stack.back().owner_base IS outgoing_owner) {
      L->context_stack.pop_back();
      lj_context_debug_physical_leave(L);
   }
   lj_context_push(L, Table, OutgoingOwner);
   L->context_stack.back().tail_transfer = true;
}

extern "C" void lj_context_prepare_metamethod_tail_jit(lua_State *L, GCtab *Table, TValue *OwnerBase)
{
   lj_assertL(L and Table and OwnerBase, "invalid recorded metamethod tail activation");
   ptrdiff_t owner_base = savestack(L, OwnerBase);
   if (not L->context_stack.empty() and
       L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
       L->context_stack.back().owner_base IS owner_base) {
      L->context_stack.pop_back();
      lj_context_debug_physical_leave(L);
   }
   lj_context_push(L, Table, OwnerBase);
   L->context_stack.back().tail_transfer = true;
}

// Permanently designate the table in a register as contextual.  The parser only emits BC_TCTX against a freshly
// materialised table constructor, so the slot is a table by construction; the assertion documents that contract for
// hand-written or malformed bytecode, which the reader rejects separately.

extern "C" void lj_tab_designate_contextual(lua_State *L, uint32_t Slot)
{
   TValue *target = L->base + Slot;
   lj_assertL(tvistab(target), "contextual designation applied to a non-table register");
   if (tvistab(target)) lj_tab_mark_contextual(tabV(target));
}

// Recorded form of the designation above.  The recorder already holds the constructor's table reference, so no stack
// slot is involved and the helper never reallocates or raises.

extern "C" void lj_tab_mark_contextual_jit(GCtab *Table) noexcept
{
   lj_assertX(Table, "contextual designation of a null table");
   lj_tab_mark_contextual(Table);
}

extern "C" void lj_context_begin_block(lua_State *L, uint32_t Slot, uint32_t BlockIndex)
{
   TValue *reference = L->base + Slot;
   if (not tvistab(reference)) lj_err_msgv(L, ErrMsg::BADTYPE, "table", lj_typename(reference));

   GCfunc *function = frame_func(L->base - 1);
   lj_assertL(isluafunc(function), "temporary context block has no Lua owner");
   GCproto *proto = funcproto(function);
   lj_assertL(BlockIndex < proto->context_block_count, "temporary context descriptor is out of range");
   if (BlockIndex >= proto->context_block_count) return;

   lj_context_push(L, tabV(reference), L->base);
   lua_State::ContextFrame &frame = L->context_stack.back();
   frame.owner_kind = lua_State::ContextFrame::OwnerKind::Block;
   frame.block_index = uint16_t(BlockIndex);
   frame.entry_slots = proto->context_blocks[BlockIndex].entry_slots;
}

static void context_end_block(lua_State *L, const TValue *OwnerBase, uint32_t BlockIndex) noexcept
{
   lj_assertL(not L->context_stack.empty(), "temporary context block stack underflow");
   if (L->context_stack.empty()) return;
   ptrdiff_t owner_base = savestack(L, OwnerBase);
   lua_State::ContextFrame &frame = L->context_stack.back();
   lj_assertL(frame.owner_kind IS lua_State::ContextFrame::OwnerKind::Block and
      frame.owner_base IS owner_base and frame.block_index IS BlockIndex,
      "unbalanced temporary context block");
   if (frame.owner_kind IS lua_State::ContextFrame::OwnerKind::Block and
       frame.owner_base IS owner_base and frame.block_index IS BlockIndex) {
      L->context_stack.pop_back();
      lj_context_debug_physical_leave(L);
      L->context_active = context_has_visible_override(L);
   }
}

extern "C" void lj_context_end_block(lua_State *L, uint32_t BlockIndex) noexcept
{
   context_end_block(L, L->base, BlockIndex);
}

extern "C" void lj_context_begin_block_jit(
   lua_State *L, GCtab *Table, TValue *OwnerBase, uint32_t BlockIndex, uint32_t EntrySlots)
{
   lj_context_push(L, Table, OwnerBase);
   lua_State::ContextFrame &frame = L->context_stack.back();
   frame.owner_kind = lua_State::ContextFrame::OwnerKind::Block;
   frame.block_index = uint16_t(BlockIndex);
   frame.entry_slots = BCREG(EntrySlots);
}

extern "C" void lj_context_end_block_jit(
   lua_State *L, TValue *OwnerBase, uint32_t BlockIndex) noexcept
{
   context_end_block(L, OwnerBase, BlockIndex);
}

extern "C" void lj_close_arm(lua_State *L, uint32_t Slot)
{
   lj_assertL(Slot < 64, "close arm slot exceeds bitmap capacity");
   if (Slot >= 64) return;
   ptrdiff_t owner_base = savestack(L, L->base);
   auto state = std::find_if(L->close_frames.rbegin(), L->close_frames.rend(), [owner_base](const auto &Frame) {
      return Frame.owner_base IS owner_base;
   });
   if (state IS L->close_frames.rend()) {
      L->close_frames.push_back(lua_State::CloseFrameState{ .owner_base = owner_base, .armed_slots = 0 });
      state = L->close_frames.rbegin();
   }
   state->armed_slots |= uint64_t(1) << Slot;
}

//********************************************************************************************************************
// Register a defer only after its callable and snapshotted arguments have been materialised.  The values remain rooted
// in the owning Lua frame; this side state records only stable stack offsets and compact slot metadata.

extern "C" void lj_defer_arm(lua_State *L, TValue *OwnerBase, uint32_t CallableSlot,
   uint32_t ArgumentCount, uint32_t ScopeBase)
{
   lj_assertL(L and OwnerBase, "invalid defer registration owner");
   if (not L or not OwnerBase) return;

   TValue *stack = tvref(L->stack);
   TValue *stack_end = tvref(L->maxstack);
   lj_assertL(OwnerBase >= stack and OwnerBase <= stack_end, "defer owner is outside the state stack allocation");
   lj_assertL(CallableSlot < LJ_MAX_SLOTS, "defer callable slot exceeds the frame limit");
   lj_assertL(ArgumentCount < LJ_MAX_SLOTS, "defer argument count exceeds the frame limit");
   lj_assertL(CallableSlot + ArgumentCount < LJ_MAX_SLOTS, "defer arguments exceed the frame limit");
   lj_assertL(ScopeBase <= CallableSlot, "defer scope begins after its callable slot");
   if (OwnerBase < stack or OwnerBase > stack_end or CallableSlot >= LJ_MAX_SLOTS or
       ArgumentCount >= LJ_MAX_SLOTS or CallableSlot + ArgumentCount >= LJ_MAX_SLOTS or ScopeBase > CallableSlot) {
      return;
   }

   ptrdiff_t owner_base = savestack(L, OwnerBase);
   auto frame = std::find_if(L->defer_frames.rbegin(), L->defer_frames.rend(),
      [owner_base](const DeferFrameState &Frame) { return Frame.owner_base IS owner_base; });
   if (frame IS L->defer_frames.rend()) {
      L->defer_frames.push_back(DeferFrameState{ .owner_base = owner_base });
      frame = L->defer_frames.rbegin();
   }

   auto duplicate = std::find_if(frame->registrations.rbegin(), frame->registrations.rend(),
      [CallableSlot](const DeferRegistration &Registration) {
         return Registration.callable_slot IS CallableSlot;
      });
   lj_assertL(duplicate IS frame->registrations.rend(), "defer callable slot armed more than once");
   if (duplicate != frame->registrations.rend()) return;

   frame->registrations.push_back(DeferRegistration{
      .callable_slot = uint8_t(CallableSlot),
      .argument_count = uint8_t(ArgumentCount),
      .scope_base = uint8_t(ScopeBase)
   });
}

//********************************************************************************************************************
// Consume before entering user code so a throwing normal-exit handler cannot be discovered again during unwinding.

extern "C" bool lj_defer_consume(lua_State *L, TValue *OwnerBase, uint32_t CallableSlot)
{
   lj_assertL(L and OwnerBase, "invalid defer consumption owner");
   if (not L or not OwnerBase) return false;

   TValue *stack = tvref(L->stack);
   TValue *stack_end = tvref(L->maxstack);
   lj_assertL(OwnerBase >= stack and OwnerBase <= stack_end,
      "defer consumption owner is outside the state stack allocation");
   lj_assertL(CallableSlot < LJ_MAX_SLOTS, "defer consumption slot exceeds the frame limit");
   if (OwnerBase < stack or OwnerBase > stack_end or CallableSlot >= LJ_MAX_SLOTS) return false;

   ptrdiff_t owner_base = savestack(L, OwnerBase);
   auto frame = std::find_if(L->defer_frames.rbegin(), L->defer_frames.rend(),
      [owner_base](const DeferFrameState &Frame) { return Frame.owner_base IS owner_base; });
   lj_assertL(frame != L->defer_frames.rend(), "defer consumption has no owning frame state");
   if (frame IS L->defer_frames.rend()) return false;

   auto registration = std::find_if(frame->registrations.rbegin(), frame->registrations.rend(),
      [CallableSlot](const DeferRegistration &Registration) {
         return Registration.callable_slot IS CallableSlot;
      });
   lj_assertL(registration != frame->registrations.rend(), "defer callable slot is not armed");
   if (registration IS frame->registrations.rend()) return false;

   lj_assertL(registration IS frame->registrations.rbegin(), "defer registrations consumed out of LIFO order");
   frame->registrations.erase(std::next(registration).base());
   if (frame->registrations.empty()) L->defer_frames.erase(std::next(frame).base());
   return true;
}

//********************************************************************************************************************
// Find the innermost active lexical defer scope in a callable-slot range.

bool lj_defer_find_scope(lua_State *State, const TValue *OwnerBase, uint32_t LowerCallableSlot,
   uint32_t UpperCallableSlot, uint32_t *ScopeBase) noexcept
{
   if (not State or not OwnerBase or not ScopeBase or LowerCallableSlot > UpperCallableSlot) return false;

   ptrdiff_t owner_base = savestack(State, OwnerBase);
   auto frame = std::find_if(State->defer_frames.rbegin(), State->defer_frames.rend(),
      [owner_base](const DeferFrameState &Frame) { return Frame.owner_base IS owner_base; });
   if (frame IS State->defer_frames.rend()) return false;

   bool found = false;
   uint32_t scope_base = 0;
   for (const DeferRegistration &registration : frame->registrations) {
      if (registration.callable_slot < LowerCallableSlot or registration.callable_slot >= UpperCallableSlot) continue;
      if (not found or registration.scope_base > scope_base) {
         found = true;
         scope_base = registration.scope_base;
      }
   }
   if (found) *ScopeBase = scope_base;
   return found;
}

//********************************************************************************************************************
// Copy the newest active registration in one lexical scope.  The caller must consume it before invoking user code.

bool lj_defer_peek_scope(lua_State *State, const TValue *OwnerBase, uint32_t LowerCallableSlot,
   uint32_t UpperCallableSlot, uint32_t ScopeBase, DeferRegistration *Registration) noexcept
{
   if (not State or not OwnerBase or not Registration or LowerCallableSlot > UpperCallableSlot) return false;

   ptrdiff_t owner_base = savestack(State, OwnerBase);
   auto frame = std::find_if(State->defer_frames.rbegin(), State->defer_frames.rend(),
      [owner_base](const DeferFrameState &Frame) { return Frame.owner_base IS owner_base; });
   if (frame IS State->defer_frames.rend()) return false;

   auto registration = std::find_if(frame->registrations.rbegin(), frame->registrations.rend(),
      [LowerCallableSlot, UpperCallableSlot, ScopeBase](const DeferRegistration &Candidate) {
         return Candidate.callable_slot >= LowerCallableSlot and Candidate.callable_slot < UpperCallableSlot and
            Candidate.scope_base IS ScopeBase;
      });
   if (registration IS frame->registrations.rend()) return false;
   *Registration = *registration;
   return true;
}

//********************************************************************************************************************
// Report whether a protected boundary is abandoning any frame that owns active defers.

bool lj_defer_has_owner_above(lua_State *State, const TValue *SurvivingBase) noexcept
{
   if (not State or not SurvivingBase) return false;
   ptrdiff_t surviving_base = savestack(State, SurvivingBase);
   return std::any_of(State->defer_frames.begin(), State->defer_frames.end(),
      [surviving_base](const DeferFrameState &Frame) { return Frame.owner_base > surviving_base; });
}

//********************************************************************************************************************
// Discard registrations belonging to an abandoned lexical range after its cleanup handlers have had an opportunity
// to run.  The owner frame can survive when a try handler resumes within the same function.

void lj_defer_discard(lua_State *State, const TValue *OwnerBase, uint32_t MinimumCallableSlot) noexcept
{
   if (not State or not OwnerBase) return;
   TValue *stack = tvref(State->stack);
   TValue *stack_end = tvref(State->maxstack);
   lj_assertG_(G(State), OwnerBase >= stack and OwnerBase <= stack_end,
      "defer discard owner is outside the state stack allocation");
   lj_assertG_(G(State), MinimumCallableSlot <= LJ_MAX_SLOTS,
      "defer discard slot exceeds the frame limit");
   if (OwnerBase < stack or OwnerBase > stack_end or MinimumCallableSlot > LJ_MAX_SLOTS) return;

   ptrdiff_t owner_base = savestack(State, OwnerBase);
   auto frame = std::find_if(State->defer_frames.begin(), State->defer_frames.end(),
      [owner_base](const DeferFrameState &Frame) { return Frame.owner_base IS owner_base; });
   if (frame IS State->defer_frames.end()) return;

   frame->registrations.erase(std::remove_if(frame->registrations.begin(), frame->registrations.end(),
      [MinimumCallableSlot](const DeferRegistration &Registration) {
         return Registration.callable_slot >= MinimumCallableSlot;
      }), frame->registrations.end());
   if (frame->registrations.empty()) State->defer_frames.erase(frame);
}

void lj_defer_unwind(lua_State *State, const TValue *SurvivingBase) noexcept
{
   if (not State or not SurvivingBase) return;
   TValue *stack = tvref(State->stack);
   TValue *stack_end = tvref(State->maxstack);
   lj_assertG_(G(State), SurvivingBase >= stack and SurvivingBase <= stack_end,
      "defer unwind boundary is outside the state stack allocation");
   if (SurvivingBase < stack or SurvivingBase > stack_end) return;

   ptrdiff_t surviving_base = savestack(State, SurvivingBase);
   State->defer_frames.erase(std::remove_if(State->defer_frames.begin(), State->defer_frames.end(),
      [surviving_base](const DeferFrameState &Frame) { return Frame.owner_base > surviving_base; }),
      State->defer_frames.end());
}

extern "C" void lj_array_view_mode(lua_State *L, uint32_t Enabled)
{
   if (Enabled) {
      if (L->array_view_depth >= 64) lj_err_msg(L, ErrMsg::XNEST);
      L->array_view_scopes |= uint64_t(1) << L->array_view_depth++;
   }
   else if (L->array_view_depth > 0) {
      L->array_view_depth--;
      L->array_view_scopes &= ~(uint64_t(1) << L->array_view_depth);
   }
}

bool lj_array_take_view_mode(lua_State *L) noexcept
{
   if (L->array_view_depth IS 0) return false;
   uint64_t scope = uint64_t(1) << (L->array_view_depth - 1);
   bool armed = (L->array_view_scopes & scope) != 0;
   L->array_view_scopes &= ~scope;
   return armed;
}

uint64_t lj_close_take_armed(
   lua_State *L, const TValue *OwnerBase, uint32_t LowerSlot, uint32_t UpperSlot) noexcept
{
   ptrdiff_t owner_base = savestack(L, OwnerBase);
   auto state = std::find_if(L->close_frames.rbegin(), L->close_frames.rend(), [owner_base](const auto &Frame) {
      return Frame.owner_base IS owner_base;
   });
   if (state IS L->close_frames.rend()) return 0;

   uint64_t lower_mask = LowerSlot >= 64 ? UINT64_MAX : ((uint64_t(1) << LowerSlot) - 1);
   uint64_t upper_mask = UpperSlot >= 64 ? UINT64_MAX : ((uint64_t(1) << UpperSlot) - 1);
   uint64_t selected = state->armed_slots & upper_mask & ~lower_mask;
   state->armed_slots &= ~selected;
   if (state->armed_slots IS 0) L->close_frames.erase(std::next(state).base());
   return selected;
}

extern "C" void lj_close_consume(lua_State *L, uint32_t Slot)
{
   uint64_t selected = lj_close_take_armed(L, L->base, Slot, Slot + 1);
   if (not selected) return;
   TValue *object = L->base + Slot;
   if (tvisnil(object) or tvisfalse(object)) return;
   L->top = curr_topL(L);
   int error = lj_meta_close(L, object, nullptr);
   if (error) lj_err_throw(L, error);
   L->top = curr_topL(L);
}

extern "C" void lj_context_enter_call(lua_State *L, uint32_t CallBase)
{
   ptrdiff_t owner_base = savestack(L, L->base + CallBase + 1 + LJ_FR2);
   if (not L->context_stack.empty() and
       L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
       L->context_stack.back().owner_base IS owner_base) return;

   TValue *receiver = L->base + CallBase - 1;
   if (lj_context_receiver_establishes(receiver)) {
      lj_context_push(L, tabV(receiver), L->base + CallBase + 1 + LJ_FR2);
   }
}

extern "C" uint32_t lj_context_prepare_call(lua_State *L, uint32_t CallBase, uint32_t ArgumentCount)
{
   TValue *receiver = L->base + CallBase - 1;
   if (lj_context_receiver_establishes(receiver)) {
      lj_context_enter_call(L, CallBase);
   }
   return ArgumentCount;
}

extern "C" void lj_context_leave_call(
   lua_State *L, uint32_t CallBase, const BCIns *CallInstruction, uint32_t ResultCountWithSentinel) noexcept
{
   ptrdiff_t owner_base = savestack(L, L->base + CallBase + 1 + LJ_FR2);
   if (not L->context_stack.empty() and
       L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
       L->context_stack.back().owner_base IS owner_base) {
      L->context_stack.pop_back();
      lj_context_debug_physical_leave(L);
      L->context_active = context_has_visible_override(L);
   }

   uint32_t encoded_results = bc_b(*CallInstruction);
   lj_assertL(encoded_results or ResultCountWithSentinel,
      "dynamic contextual call lost its result-count sentinel");
   uint32_t result_count = encoded_results ? encoded_results - 1 :
      (ResultCountWithSentinel ? ResultCountWithSentinel - 1 : 0);
   // D records every receiver/lookup temporary removed from the expression result layout. Older contextual chunks
   // encoded zero and used the original single retained-receiver slot.
   uint32_t result_shift = bc_d(CallInstruction[1]);
   if (result_shift IS 0) result_shift = 1;
   TValue *results = L->base + CallBase;
   for (uint32_t i = 0; i < result_count; i++) copyTV(L, results + i - result_shift, results + i);
}

extern "C" uint32_t lj_context_prepare_tail_call(lua_State *L, uint32_t CallBase, uint32_t ArgumentCount)
{
   TValue *receiver = L->base + CallBase - 1;
   if (not lj_context_receiver_establishes(receiver)) return ArgumentCount;

   ptrdiff_t prepared_owner = savestack(L, L->base + CallBase + 1 + LJ_FR2);
   lj_assertL(not L->context_stack.empty() and
      L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
      L->context_stack.back().owner_base IS prepared_owner,
      "contextual tail call has no prepared activation");
   if (L->context_stack.empty() or
       L->context_stack.back().owner_kind != lua_State::ContextFrame::OwnerKind::Call or
       L->context_stack.back().owner_base != prepared_owner) return ArgumentCount;

   GCtab *context = tabref(L->context_stack.back().table);
   lj_assertL(tabV(receiver) IS context, "tail context does not match its receiver");
   L->context_stack.pop_back();
   lj_context_debug_physical_leave(L);

   ptrdiff_t outgoing_owner = savestack(L, L->base);
   if (not L->context_stack.empty() and
       L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
       L->context_stack.back().owner_base IS outgoing_owner) {
      L->context_stack.pop_back();
      lj_context_debug_physical_leave(L);
   }
   lj_context_push(L, context, L->base);
   L->context_stack.back().tail_transfer = true;
   return ArgumentCount;
}

uint32_t lj_context_prepare_metamethod_call(lua_State *L, cTValue *Receiver, TValue *OwnerBase,
   uint32_t VisibleArgumentCount, uint32_t NativeArgumentCount, bool TailTransfer)
{
   lj_assertL(L and Receiver and OwnerBase, "invalid metamethod contextual activation");
   uint32_t argument_count = NativeArgumentCount;
   if (tvistab(Receiver)) {
      ptrdiff_t owner_base = savestack(L, OwnerBase);
      if (TailTransfer and not L->context_stack.empty() and
          L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
          L->context_stack.back().owner_base IS owner_base) {
         L->context_stack.pop_back();
         lj_context_debug_physical_leave(L);
      }
      if (L->context_stack.empty() or
          L->context_stack.back().owner_kind != lua_State::ContextFrame::OwnerKind::Call or
          L->context_stack.back().owner_base != owner_base) {
         lj_context_push(L, tabV(Receiver), OwnerBase);
      }
      else {
         lj_assertL(tabref(L->context_stack.back().table) IS tabV(Receiver),
            "retried metamethod call selected a different receiver");
      }
      L->context_stack.back().tail_transfer = TailTransfer;
      argument_count = VisibleArgumentCount;
   }
   L->metamethod_argument_count = uint8_t(argument_count);
   return argument_count;
}

void lj_context_push(lua_State *L, GCtab *Table, const TValue *OwnerBase)
{
   lj_assertL(L and Table and OwnerBase, "invalid contextual activation");
   // Initial entries take Table from L's guarded operand stack; transfers take it from L's rooted context stack.
   lj_assertL(Table->gct IS ~LJ_TTAB, "context is not a table");
   TValue *stack = tvref(L->stack);
   // A call frame base may temporarily occupy LuaJIT's reserved stack area before the function header grows the
   // soft stack limit. Context owners are offsets only and remain valid throughout that interval.
   [[maybe_unused]] TValue *stack_end = stack + L->stacksize;
   lj_assertL(OwnerBase >= stack and OwnerBase < stack_end, "context owner is outside the state stack allocation");

   lua_State::ContextFrame frame;
   setgcref(frame.table, obj2gco(Table));
   frame.owner_base = savestack(L, OwnerBase);
   L->context_stack.push_back(frame);
   L->context_active = 1;
   lj_context_debug_physical_enter(L);
   lj_gc_barriercontext(L, Table);
}

void lj_context_pop(lua_State *L, const TValue *OwnerBase) noexcept
{
   lj_assertL(L and OwnerBase, "invalid contextual activation owner");
   lj_assertL(not L->context_stack.empty(), "attempt to pop the permanent root context");
   if (L->context_stack.empty()) return;

   ptrdiff_t owner_base = savestack(L, OwnerBase);
   lj_assertL(L->context_stack.back().owner_base IS owner_base, "unbalanced contextual activation");
   if (L->context_stack.back().owner_base IS owner_base) {
      L->context_stack.pop_back();
      lj_context_debug_physical_leave(L);
      L->context_active = context_has_visible_override(L);
   }
}

extern "C" uint32_t lj_context_leave_frame(
   lua_State *L, TValue *FrameBase, uint32_t ReturnState) noexcept
{
   lj_assertL(L and FrameBase, "invalid contextual activation frame");
   ptrdiff_t frame_base = savestack(L, FrameBase);
   if (not L->context_stack.empty() and frame_isvarg(FrameBase - 1)) {
      TValue *prepared_frame = (TValue *)((char *)FrameBase - frame_sized(FrameBase - 1));
      ptrdiff_t prepared_base = savestack(L, prepared_frame);
      if (L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
          L->context_stack.back().owner_base IS prepared_base) frame_base = prepared_base;
   }
   if (not L->context_stack.empty() and
       L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
       L->context_stack.back().owner_base IS frame_base) {
      L->context_stack.pop_back();
      lj_context_debug_physical_leave(L);
      L->context_active = context_has_visible_override(L);
   }
   return ReturnState;
}

extern "C" uint32_t lj_context_leave_native_frame(
   lua_State *L, TValue *FrameBase, uint32_t ReturnState) noexcept
{
   ptrdiff_t frame_base = savestack(L, FrameBase);
   if (not L->context_stack.empty() and
      L->context_stack.back().owner_kind IS lua_State::ContextFrame::OwnerKind::Call and
      L->context_stack.back().owner_base IS frame_base) {
      return lj_context_leave_frame(L, FrameBase, ReturnState);
   }
   return lj_context_leave_frame(L, FrameBase + 1 + LJ_FR2, ReturnState);
}

void lj_context_unwind(lua_State *L, const TValue *SurvivingBase) noexcept
{
   lj_assertL(L and SurvivingBase, "invalid contextual unwind boundary");
   ptrdiff_t surviving_base = savestack(L, SurvivingBase);
   size_t floor = L->context_root_floors.empty() ? 0 : L->context_root_floors.back();
   while (L->context_stack.size() > floor and L->context_stack.back().owner_base > surviving_base) {
      L->context_stack.pop_back();
      lj_context_debug_physical_leave(L);
   }
   for (auto state = L->close_frames.begin(); state != L->close_frames.end();) {
      if (state->owner_base > surviving_base) state = L->close_frames.erase(state);
      else ++state;
   }
   L->context_active = context_has_visible_override(L);
}

void lj_context_restore_depth(lua_State *L, size_t Depth) noexcept
{
   size_t floor = L->context_root_floors.empty() ? 0 : L->context_root_floors.back();
   if (Depth < floor) Depth = floor;
   lj_assertL(Depth <= L->context_stack.size(), "context unwind depth exceeds the active stack");
   if (Depth <= L->context_stack.size()) {
      size_t removed = L->context_stack.size() - Depth;
      L->context_stack.resize(Depth);
      lj_context_debug_physical_leave(L, removed);
   }
   L->context_active = context_has_visible_override(L);
}

size_t lj_context_depth(const lua_State *L) noexcept
{
   if (not L) return 0;
   size_t floor = L->context_root_floors.empty() ? 0 : L->context_root_floors.back();
   return L->context_stack.size() - floor + 1;
}

//********************************************************************************************************************
// Allocate basic stack for new state.

static void stack_init(lua_State *L1, lua_State *L)
{
   TValue* stend, * st = lj_mem_newvec(L, LJ_STACK_START + LJ_STACK_EXTRA, TValue);
   setmref(L1->stack, st);
   L1->stacksize = LJ_STACK_START + LJ_STACK_EXTRA;
   stend = st + L1->stacksize;
   setmref(L1->maxstack, stend - LJ_STACK_EXTRA - 1);
   setthreadV(L1, st++, L1);  //  Needed for curr_funcisL() on empty stack.
   if (LJ_FR2) setnilV(st++);
   L1->base = L1->top = st;
   while (st < stend) setnilV(st++); //  Clear new slots.
}

//********************************************************************************************************************
// Open parts that may cause memory-allocation errors.

static TValue * cpluaopen(lua_State *Lua, lua_CFunction dummy, void* ud)
{
   global_State *g = G(Lua);
   stack_init(Lua, Lua);
   Lua->checkall_stack = lj_mem_newt(Lua, sizeof(CheckallFrameStack), CheckallFrameStack);
   new (Lua->checkall_stack) CheckallFrameStack;

   // NOBARRIER: State initialization, all objects are white.

   setgcref(Lua->env, obj2gco(lj_tab_new(Lua, 0, LJ_MIN_GLOBAL)));
   settabV(Lua, registry(Lua), lj_tab_new(Lua, 0, LJ_MIN_REGISTRY));
   lj_str_init(Lua);
   lj_meta_init(Lua);
   lj_reserve_words(Lua);
   fixstring(lj_err_str(Lua, ErrMsg::ERRMEM));  //  Preallocate memory error msg.
   g->gc.threshold = 4 * g->gc.total;
   lj_trace_initstate(g);
   lj_err_verify();
   return nullptr;
}

//********************************************************************************************************************

static void close_state(lua_State *L)
{
   global_State *g = G(L);
   // Teardown also covers states terminated while an activation or asynchronous root boundary is still live.
   L->defer_frames.clear();
   L->context_stack.clear();
   L->context_active = 0;
   L->context_root_floors.clear();
#ifdef LUA_USE_ASSERT
   delete L->context_debug_counters;
   L->context_debug_counters = nullptr;
#endif
   if (L->parser_diagnostics) { delete (ParserDiagnostics*)L->parser_diagnostics; L->parser_diagnostics = nullptr; }
   if (L->parser_tips) { delete L->parser_tips; L->parser_tips = nullptr; }
   if (L->parser_symbols) { delete L->parser_symbols; L->parser_symbols = nullptr; }
   funcnames_free(g);
   lj_func_closeuv(L, tvref(L->stack));

   // Free all GC objects during shutdown
   GarbageCollector collector = gc(g);
   collector.freeAll();

   lj_assertG(gcref(g->gc.root) IS obj2gco(L), "main thread is not first GC object");
   lj_assertG(g->str.num IS 0, "leaked %d strings", g->str.num);
   lj_trace_freestate(g);
   lj_str_freetab(g);
   lj_buf_free(g, &g->tmpbuf);
   lj_mem_freevec(g, tvref(L->stack), L->stacksize, TValue);
   if (L->checkall_stack) {
      lj_mem_freet(g, L->checkall_stack);
      L->checkall_stack = nullptr;
   }

   if (mref<uint32_t>(g->gc.lightudseg)) {
      MSize segnum = g->gc.lightudnum ? (2 << lj_fls(g->gc.lightudnum)) : 2;
      lj_mem_freevec(g, mref<uint32_t>(g->gc.lightudseg), segnum, uint32_t);
   }

   L->~lua_State();  // Destructor required to free C++ containers (file_sources, file_index_map).

   lj_assertG(g->gc.total IS sizeof(GG_State), "memory leak of %lld bytes", (long long)(g->gc.total - sizeof(GG_State)));
#ifndef LUAJIT_USE_SYSMALLOC
   if (g->allocf IS lj_alloc_f)
      lj_alloc_destroy(g->allocd);
   else
#endif
      g->allocf(g->allocd, G2GG(g), sizeof(GG_State), 0);
}

//********************************************************************************************************************

extern lua_State * lua_newstate(lua_Alloc allocf, void* allocd)
{
   PRNGState prng;
   GG_State *GG;
   lua_State *L;
   global_State *g;

   // We need the PRNG for the memory allocator, so initialize this first.

   if (!lj_prng_seed_secure(&prng)) {
      lj_assertX(0, "secure PRNG seeding failed");
      // Can only return nullptr here, so this errors with "not enough memory".
      return nullptr;
   }

#ifndef LUAJIT_USE_SYSMALLOC
   if (allocf IS LJ_ALLOCF_INTERNAL) {
      allocd = lj_alloc_create(&prng);
      if (!allocd) return nullptr;
      allocf = lj_alloc_f;
   }
#endif

   GG = (GG_State*)allocf(allocd, nullptr, 0, sizeof(GG_State));
   if (GG IS nullptr or !checkptrGC(GG)) return nullptr;
   memset((void *)GG, 0, sizeof(GG_State));
   L = &GG->L;

   new (L) lua_State;

   g = &GG->g;
   L->gct = ~LJ_TSTRUCT;  // Tag shared with GCstruct; this is the only lua_State per interpreter.
   L->marked = LJ_GC_WHITE0 | LJ_GC_FIXED | LJ_GC_SFIXED;  //  Prevent free.
   L->dummy_ffid = FF_C;
   setnilV(&L->pending_close_error);
   L->try_handler_pc = nullptr;
   setmref(L->glref, g);
   g->gc.currentwhite = LJ_GC_WHITE0 | LJ_GC_FIXED;
   g->strempty.marked = LJ_GC_WHITE0;
   g->strempty.gct = ~LJ_TSTR;
   g->allocf = allocf;
   g->allocd = allocd;
   g->prng = prng;

#ifndef LUAJIT_USE_SYSMALLOC
   if (allocf IS lj_alloc_f) {
      lj_alloc_setprng(allocd, &g->prng);
   }
#endif

   setgcref(g->mainthref, obj2gco(L));
   setgcref(g->uvhead.prev, obj2gco(&g->uvhead));
   setgcref(g->uvhead.next, obj2gco(&g->uvhead));
   g->str.mask = ~(MSize)0;
   setnilV(registry(L));
   setnilV(&g->nilnode.val);
   setnilV(&g->nilnode.key);
   lj_buf_init(nullptr, &g->tmpbuf);
   g->gc.state = GCPhase::Pause;
   setgcref(g->gc.root, obj2gco(L));
   setgcrefnull(g->gc.finobj);
   setmref(g->gc.sweep, &g->gc.root);
   setmref(g->gc.sweepfin, &g->gc.finobj);
   g->gc.total = sizeof(GG_State);
   g->gc.pause = LUAI_GCPAUSE;
   g->gc.stepmul = LUAI_GCMUL;
   lj_dispatch_init((GG_State*)L);
   L->status = LUA_ERRERR + 1;  //  Avoid touching the stack upon memory error.
   if (lj_vm_cpcall(L, nullptr, nullptr, cpluaopen) != 0) {
      // Memory allocation error: free partial state.
      close_state(L);
      return nullptr;
   }
   L->status = LUA_OK;
   return L;
}

//********************************************************************************************************************

extern void lua_close(lua_State *L)
{
   global_State* g = G(L);
   GarbageCollector collector = gc(g);
   L = mainthread(g);  //  Only the main thread can be closed.
   setgcrefnull(g->cur_L);
   lj_func_closeuv(L, tvref(L->stack));

   // Snapshot pre-shutdown registrations once. Finalisers created while this queue is drained remain in finobj and are
   // freed by close_state() without being run during this shutdown.
   collector.prepareFinalisersForShutdown();

   G2J(g)->flags &= ~JIT_F_ON;
   G2J(g)->state = TraceState::IDLE;
   lj_dispatch_update(g);
   hook_enter(g);
   L->status = LUA_OK;
   L->base = L->top = tvref(L->stack) + 1 + LJ_FR2;
   L->cframe = nullptr;

   // Metamethod failures are contained by gc_call_finaliser(), so every object in the snapshot is drained.
   collector.finalizePending(L);
   lj_assertG(gcref(g->gc.mmudata) IS nullptr, "pending finaliser queue was not drained during shutdown");
   close_state(L);
}

//********************************************************************************************************************

void lj_state_free(global_State* g, lua_State *L)
{
   lj_assertG(L != mainthread(g), "free of main thread");
   if (obj2gco(L) IS gcref(g->cur_L)) setgcrefnull(g->cur_L);
   lj_assertG(L->context_stack.empty(), "state freed with active contextual activations");
   lj_assertG(L->context_root_floors.empty(), "state freed with active asynchronous root boundaries");
   L->defer_frames.clear();

   if (L->parser_diagnostics) { delete (ParserDiagnostics*)L->parser_diagnostics; L->parser_diagnostics = nullptr; }
   if (L->parser_tips) { delete L->parser_tips; L->parser_tips = nullptr; }
   if (L->parser_symbols) { delete L->parser_symbols; L->parser_symbols = nullptr; }
#ifdef LUA_USE_ASSERT
   delete L->context_debug_counters;
   L->context_debug_counters = nullptr;
#endif

   lj_func_closeuv(L, tvref(L->stack));
   lj_assertG(gcref(L->openupval) IS nullptr, "stale open upvalues");
   lj_mem_freevec(g, tvref(L->stack), L->stacksize, TValue);
   if (L->checkall_stack) {
      lj_mem_freet(g, L->checkall_stack);
      L->checkall_stack = nullptr;
   }
   L->~lua_State();
   lj_mem_freet(g, L);
}
