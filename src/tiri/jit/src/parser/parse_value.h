// Expression value wrapper for parser emission pipeline.
//
// Copyright (C) 2025-2026 Paul Manias

#pragma once

#include "parser/parse_types.h"
#include "parser/parse_value.h"
#include "parser/parse_control_flow.h"
#include "parser/parse_internal.h"
#include "parser/parse_regalloc.h"

class RegisterAllocator;
class ControlFlowGraph;
class ControlFlowEdge;

class ExpressionValue {
public:
   ExpressionValue(const ExpressionValue&) = default;
   ExpressionValue& operator=(const ExpressionValue&) = default;
   ExpressionValue(ExpressionValue&&) noexcept = default;
   ExpressionValue& operator=(ExpressionValue&&) noexcept = default;

   inline ExpressionValue() : func_state(nullptr), descriptor(ExpDesc(ExpKind::Nil)) {}
   inline ExpressionValue(const ExpDesc& Descriptor) : func_state(nullptr), descriptor(Descriptor) {}
   inline ExpressionValue(FuncState* State, const ExpDesc& Descriptor) : func_state(State), descriptor(Descriptor) { }

   inline ExpressionValue make_nil(FuncState *State) { return ExpressionValue(State, ExpDesc(ExpKind::Nil)); }
   inline ExpressionValue make_bool(FuncState* State, bool Value) { return ExpressionValue(State, ExpDesc(Value)); }
   inline ExpressionValue make_number(FuncState* State, lua_Number Value) { return ExpressionValue(State, ExpDesc(Value)); }
   inline ExpressionValue make_string(FuncState* State, GCstr* Value) { return ExpressionValue(State, ExpDesc(Value)); }

   inline bool has_jump() const { return this->descriptor.has_jump(); }
   inline bool is_constant() const { return this->descriptor.is_constant(); }
   inline bool is_constant_nojump() const { return this->descriptor.is_constant_nojump(); }
   inline bool is_number_constant() const { return this->descriptor.is_num_constant(); }
   inline bool is_number_constant_nojump() const { return this->descriptor.is_num_constant_nojump(); }
   inline bool is_string_constant() const { return this->descriptor.is_str_constant(); }
   inline bool has_flag(ExprFlag Flag) const { return (this->descriptor.flags & Flag) != ExprFlag::None; }
   inline void set_flag(ExprFlag Flag) { this->descriptor.flags |= Flag; }
   inline void clear_flag(ExprFlag Flag) { this->descriptor.flags &= ~Flag; }

   inline bool consume_flag(ExprFlag Flag) {
      if ((this->descriptor.flags & Flag) != ExprFlag::None) {
         this->descriptor.flags &= ~Flag;
         return true;
      }
      return false;
   }

   inline ControlFlowEdge true_jumps(ControlFlowGraph& Graph) const { return Graph.make_true_edge(BCPos(this->descriptor.t)); }
   inline ControlFlowEdge false_jumps(ControlFlowGraph& Graph) const { return Graph.make_false_edge(BCPos(this->descriptor.f)); }
   inline void set_true_jumps(const ControlFlowEdge& Edge) { this->descriptor.t = Edge.head().raw(); }
   inline void set_false_jumps(const ControlFlowEdge& Edge) { this->descriptor.f = Edge.head().raw(); }

   inline void set_jump_heads(BCPos TrueHead, BCPos FalseHead) {
      this->descriptor.t = TrueHead.raw();
      this->descriptor.f = FalseHead.raw();
   }

   inline BCReg to_reg(RegisterAllocator& Allocator, BCReg Slot) {
      expr_toreg(Allocator.state(), &this->descriptor, Slot.raw());
      return BCReg(this->descriptor.u.s.info);
   }

   inline BCReg to_any_reg(RegisterAllocator& Allocator) { return BCReg(expr_toanyreg(Allocator.state(), &this->descriptor)); }

   inline BCReg to_next_reg(RegisterAllocator& Allocator) {
      expr_tonextreg(Allocator.state(), &this->descriptor);
      return BCReg(this->descriptor.u.s.info);
   }

   inline void discharge() { expr_discharge(this->func_state, &this->descriptor); }
   inline void discharge(ControlFlowGraph& Graph) { expr_discharge(Graph.state(), &this->descriptor); }
   inline void release(RegisterAllocator& Allocator) { Allocator.release_expression(&this->descriptor); }
   inline ExpDesc & legacy() { return this->descriptor; }
   inline const ExpDesc & legacy() const { return this->descriptor; }
   inline FuncState * state() const { return this->func_state; }
   inline operator ExpDesc&() { return this->descriptor; }
   inline operator const ExpDesc&() const { return this->descriptor; }

   inline void to_val() { expr_toval(this->func_state, &this->descriptor); }

   inline void discharge_nobranch(RegisterAllocator& Allocator, BCReg Reg) {
      expr_toreg_nobranch(Allocator.state(), &this->descriptor, Reg.raw());
   }

   inline void store_to(RegisterAllocator& Allocator, ExpressionValue& Target) {
      bcemit_store(Allocator.state(), &Target.descriptor, &this->descriptor);
   }

   inline BCReg discharge_to_any_reg(RegisterAllocator& Allocator) {
      this->discharge();
      return this->to_any_reg(Allocator);
   }


private:
   FuncState* func_state;
   ExpDesc descriptor;
};
