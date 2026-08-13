/*
** State and stack handling.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lj_obj.h"

#ifdef LUA_USE_ASSERT
#include <array>
#include <unordered_map>
#endif

#define incr_top(L) (++L->top >= tvref(L->maxstack) and (lj_state_growstack1(L), 0))

[[nodiscard]] inline ptrdiff_t savestack(lua_State* L, const TValue* p) noexcept
{
   return (const char*)(p) - mref<char>(L->stack);
}

[[nodiscard]] inline TValue* restorestack(lua_State* L, ptrdiff_t n) noexcept
{
   return (TValue*)(mref<char>(L->stack) + n);
}

LJ_FUNC void lj_state_relimitstack(lua_State* L);
LJ_FUNC void lj_state_shrinkstack(lua_State* L, MSize used);
LJ_FUNCA void lj_state_growstack(lua_State* L, MSize need);
LJ_FUNC void lj_state_growstack1(lua_State* L);

static LJ_AINLINE void lj_state_checkstack(lua_State* L, MSize need)
{
   if ((mref<char>(L->maxstack) - (char*)L->top) <=
      (ptrdiff_t)need * (ptrdiff_t)sizeof(TValue))
      lj_state_growstack(L, need);
}

LJ_FUNC void lj_state_free(global_State* g, lua_State* L);

// State-local table context.  The root is represented by an empty override stack and always resolves through L->env.

// Only a table permanently designated contextual establishes context.  Ordinary tables - including script namespaces,
// benchmark harnesses, callback registries and internal library namespaces - inherit the caller's context instead,
// because ordinary is the default and requires no exemption metadata.  The interpreter and recorder share this
// predicate to keep their behaviour aligned.

[[nodiscard]] inline bool lj_context_receiver_establishes(const TValue *Receiver) noexcept
{
   return tvistab(Receiver) and lj_tab_is_contextual(tabV(Receiver));
}

LJ_FUNC [[nodiscard]] GCtab * lj_context_current(lua_State *L) noexcept;
extern "C" LJ_FUNC void lj_context_load(lua_State *L, TValue *Destination) noexcept;
extern "C" LJ_FUNC GCtab * lj_context_current_jit(lua_State *L) noexcept;
extern "C" LJ_FUNC void lj_context_enter_jit(lua_State *L, GCtab *Table, TValue *OwnerBase);
extern "C" LJ_FUNC void lj_context_leave_jit(lua_State *L, TValue *OwnerBase) noexcept;
extern "C" LJ_FUNC void lj_context_tail_jit(
   lua_State *L, GCtab *Table, TValue *PreparedOwner, TValue *OutgoingOwner);
extern "C" LJ_FUNC void lj_tab_designate_contextual(lua_State *L, uint32_t Slot);
extern "C" LJ_FUNC void lj_context_begin_block(lua_State *L, uint32_t Slot, uint32_t BlockIndex);
extern "C" LJ_FUNC void lj_context_end_block(lua_State *L, uint32_t BlockIndex) noexcept;
extern "C" LJ_FUNC void lj_context_begin_block_jit(
   lua_State *L, GCtab *Table, uint32_t BlockIndex, uint32_t EntrySlots);
extern "C" LJ_FUNC void lj_context_end_block_jit(lua_State *L, uint32_t BlockIndex) noexcept;
extern "C" LJ_FUNC void lj_close_arm(lua_State *L, uint32_t Slot);
extern "C" LJ_FUNC void lj_close_consume(lua_State *L, uint32_t Slot);
LJ_FUNC uint64_t lj_close_take_armed(
   lua_State *L, const TValue *OwnerBase, uint32_t LowerSlot, uint32_t UpperSlot) noexcept;
extern "C" LJ_FUNC void lj_context_enter_call(lua_State *L, uint32_t CallBase);
extern "C" LJ_FUNC uint32_t lj_context_prepare_call(lua_State *L, uint32_t CallBase, uint32_t ArgumentCount);
extern "C" LJ_FUNC void lj_context_leave_call(
   lua_State *L, uint32_t CallBase, const BCIns *CallInstruction, uint32_t ResultCountWithSentinel) noexcept;
extern "C" LJ_FUNC uint32_t lj_context_prepare_tail_call(
   lua_State *L, uint32_t CallBase, uint32_t ArgumentCount);
LJ_FUNC void lj_context_push(lua_State *L, GCtab *Table, const TValue *OwnerBase);
LJ_FUNC void lj_context_pop(lua_State *L, const TValue *OwnerBase) noexcept;
extern "C" LJ_FUNC uint32_t lj_context_leave_frame(
   lua_State *L, TValue *FrameBase, uint32_t ReturnState) noexcept;
extern "C" LJ_FUNC uint32_t lj_context_leave_native_frame(
   lua_State *L, TValue *FrameBase, uint32_t ReturnState) noexcept;
LJ_FUNC void lj_context_unwind(lua_State *L, const TValue *SurvivingBase) noexcept;
LJ_FUNC void lj_context_restore_depth(lua_State *L, size_t Depth) noexcept;
LJ_FUNC [[nodiscard]] size_t lj_context_depth(const lua_State *L) noexcept;

enum class ContextMaterialisationReason : uint8_t {
   NestedLuaCall,
   NativeCall,
   TailCall,
   Snapshot,
   SideExit,
   UnsupportedBoundary,
   Count
};

#ifdef LUA_USE_ASSERT
struct ContextDebugCounters {
   uint64_t virtual_activations_created = 0;
   uint64_t virtual_activations_retired = 0;
   uint64_t physical_context_entries = 0;
   uint64_t physical_context_leaves = 0;
   uint64_t jit_enter_calls = 0;
   uint64_t jit_leave_calls = 0;
   uint64_t jit_current_calls = 0;
   uint64_t trace_starts = 0;
   uint64_t trace_compilations = 0;
   uint64_t trace_aborts = 0;
   uint64_t side_exits = 0;
   uint32_t maximum_virtual_depth = 0;
   uint32_t maximum_physical_depth = 0;
   std::array<uint64_t, size_t(ContextMaterialisationReason::Count)> materialisations{};
   std::array<uint64_t, 256> native_fast_functions{};
   std::unordered_map<uintptr_t, uint64_t> native_c_functions;
};

LJ_FUNC ContextDebugCounters *lj_context_debug_get(lua_State *L, bool Create);
LJ_FUNC void lj_context_debug_reset(lua_State *L);
LJ_FUNC void lj_context_debug_virtual_enter(lua_State *L, uint32_t Depth);
LJ_FUNC void lj_context_debug_virtual_leave(lua_State *L);
LJ_FUNC void lj_context_debug_materialise(
   lua_State *L, ContextMaterialisationReason Reason, const GCfunc *Callable = nullptr);
LJ_FUNC void lj_context_debug_physical_enter(lua_State *L);
LJ_FUNC void lj_context_debug_physical_leave(lua_State *L, size_t Count = 1);
LJ_FUNC void lj_context_debug_trace_start(lua_State *L);
LJ_FUNC void lj_context_debug_trace_compiled(lua_State *L);
LJ_FUNC void lj_context_debug_trace_abort(lua_State *L);
LJ_FUNC void lj_context_debug_side_exit(lua_State *L);
#else
inline void lj_context_debug_virtual_enter(lua_State *, uint32_t) {}
inline void lj_context_debug_virtual_leave(lua_State *) {}
inline void lj_context_debug_materialise(lua_State *, ContextMaterialisationReason, const GCfunc * = nullptr) {}
inline void lj_context_debug_physical_enter(lua_State *) {}
inline void lj_context_debug_physical_leave(lua_State *, size_t = 1) {}
inline void lj_context_debug_trace_start(lua_State *) {}
inline void lj_context_debug_trace_compiled(lua_State *) {}
inline void lj_context_debug_trace_abort(lua_State *) {}
inline void lj_context_debug_side_exit(lua_State *) {}
#endif

// Asynchronous callbacks are deliberately unbound.  Temporarily expose the dynamic root while retaining suspended
// activations owned by the interrupted synchronous call chain.

class LuaContextRootGuard {
public:
   explicit LuaContextRootGuard(lua_State *State) noexcept : state_(State)
   {
      lj_assertX(this->state_, "missing state for root context guard");
      this->state_->context_root_floors.push_back(this->state_->context_stack.size());
      this->state_->context_active = 0;
   }

   ~LuaContextRootGuard()
   {
      lj_assertG_(G(this->state_), not this->state_->context_root_floors.empty(),
         "asynchronous callback lost its root context boundary");
      size_t floor = this->state_->context_root_floors.back();
      lj_assertG_(G(this->state_), this->state_->context_stack.size() IS floor,
         "asynchronous callback leaked a contextual activation");
      size_t removed = this->state_->context_stack.size() - floor;
      this->state_->context_stack.resize(floor);
      lj_context_debug_physical_leave(this->state_, removed);
      this->state_->context_root_floors.pop_back();
      size_t outer_floor = this->state_->context_root_floors.empty() ? 0 :
         this->state_->context_root_floors.back();
      this->state_->context_active = this->state_->context_stack.size() > outer_floor;
   }

   LuaContextRootGuard(const LuaContextRootGuard &) = delete;
   LuaContextRootGuard & operator=(const LuaContextRootGuard &) = delete;

private:
   lua_State *state_;
};

// Function name registry for tostring() support on named functions.
LJ_FUNC void lj_funcname_register(global_State* g, const GCproto* pt, const char* name, size_t len);
LJ_FUNC const char* lj_funcname_lookup(global_State* g, const GCproto* pt, size_t* len);

#define LJ_ALLOCF_INTERNAL   ((lua_Alloc)(void *)(uintptr_t)(1237<<4))
