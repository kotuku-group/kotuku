//********************************************************************************************************************
// Unified Expression Value Hierarchy
//
// This module provides type-safe abstractions for representing expression values at different stages
// of the parsing pipeline, eliminating cognitive overhead from three overlapping abstractions.
//
// Base class: ExprValue
//   - Provides common interface for all expression values
//   - Manages ExpDesc lifetime and type queries
//   - No discharge semantics (pure data structure)
//
// RValue (readable expression)
//   - Extends ExprValue with discharge operations (to register, to any register, etc.)
//   - Used when evaluating expressions as operands
//
// LValue (assignable expression)
//   - Extends ExprValue with storage operations
//   - Used for assignment targets and update operations

#pragma once

#include <variant>

//********************************************************************************************************************
// Base Expression Value - Common interface for all expression values
//
// ExprValue provides the minimum interface needed to work with expression results:
// - Type queries (is_local, is_constant, etc.)
// - Flag management for control flow
// - Direct access to the underlying ExpDesc
//
// This base class does NOT own the ExpDesc; the caller is responsible for lifetime management.

class ExprValue {
public:
   ExprValue(const ExprValue&) = default;
   ExprValue& operator=(const ExprValue&) = default;
   ExprValue(ExprValue&&) noexcept = default;
   ExprValue& operator=(ExprValue&&) noexcept = default;

   explicit ExprValue(ExpDesc* Desc) noexcept : desc_(Desc) {}

   // Query expression kind
   [[nodiscard]] inline ExpKind kind() const { return desc_->k; }
   [[nodiscard]] inline bool is_constant() const { return desc_->is_constant(); }
   [[nodiscard]] inline bool is_constant_nojump() const { return desc_->is_constant_nojump(); }
   [[nodiscard]] inline bool is_num_constant() const { return desc_->is_num_constant(); }
   [[nodiscard]] inline bool is_str_constant() const { return desc_->is_str_constant(); }
   [[nodiscard]] inline bool is_falsey() const { return desc_->is_falsey(); }

   // Query storage categories
   [[nodiscard]] inline bool is_local() const { return kind() IS ExpKind::Local; }
   [[nodiscard]] inline bool is_upvalue() const { return kind() IS ExpKind::Upval; }
   [[nodiscard]] inline bool is_global() const { return kind() IS ExpKind::Global; }
   [[nodiscard]] inline bool is_indexed() const { return kind() IS ExpKind::Indexed; }
   [[nodiscard]] inline bool is_temp() const {
      return kind() IS ExpKind::NonReloc or kind() IS ExpKind::Relocable;
   }

   // Control flow query
   [[nodiscard]] inline bool has_jump() const { return desc_->has_jump(); }

   // Flag management
   [[nodiscard]] inline bool has_flag(ExprFlag Flag) const { return (desc_->flags & Flag) != ExprFlag::None; }
   inline void set_flag(ExprFlag Flag) { desc_->flags |= Flag; }
   inline void clear_flag(ExprFlag Flag) { desc_->flags &= ~Flag; }
   inline bool consume_flag(ExprFlag Flag) {
      if ((desc_->flags & Flag) != ExprFlag::None) {
         desc_->flags &= ~Flag;
         return true;
      }
      return false;
   }

   // Jump manipulation
   [[nodiscard]] inline BCPOS true_jump_head() const { return BCPos(desc_->t); }
   [[nodiscard]] inline BCPOS false_jump_head() const { return BCPos(desc_->f); }
   inline void set_jump_heads(BCPos TrueHead, BCPos FalseHead) {
      desc_->t = TrueHead.raw();
      desc_->f = FalseHead.raw();
   }

   // Access underlying descriptor
   [[nodiscard]] inline ExpDesc* raw() { return desc_; }
   [[nodiscard]] inline const ExpDesc* raw() const { return desc_; }
   [[nodiscard]] inline ExpDesc& descriptor() { return *desc_; }
   [[nodiscard]] inline const ExpDesc& descriptor() const { return *desc_; }

protected:
   ExpDesc* desc_;
};

//********************************************************************************************************************
// RValue - Readable/Evaluable Expression
//
// RValue extends ExprValue with discharge operations, used when evaluating an expression
// as the source/operand in an operation. It requires FuncState for bytecode emission.

class RValue : public ExprValue {
public:
   explicit RValue(ExpDesc* Desc, FuncState* State = nullptr) noexcept
      : ExprValue(Desc), func_state_(State) {}

   RValue(const RValue&) = default;
   RValue& operator=(const RValue&) = default;
   RValue(RValue&&) noexcept = default;
   RValue& operator=(RValue&&) noexcept = default;

   [[nodiscard]] inline FuncState* state() const { return func_state_; }

   inline void discharge() {
      RegisterAllocator allocator(this->func_state_);
      allocator.discharge(*this->desc_);
   }

   [[nodiscard]] inline BCReg to_register(BCReg Target) {
      RegisterAllocator allocator(this->func_state_);
      allocator.discharge_to_register(*this->desc_, Target);
      return BCReg(this->desc_->u.s.info);
   }

   [[nodiscard]] inline BCReg to_next_register() {
      RegisterAllocator allocator(this->func_state_);
      allocator.discharge_to_next_register(*this->desc_);
      return BCReg(this->desc_->u.s.info);
   }

   [[nodiscard]] inline BCReg to_any_register() {
      RegisterAllocator allocator(this->func_state_);
      return allocator.discharge_to_any_register(*this->desc_);
   }

   inline void to_value() {
      RegisterAllocator allocator(this->func_state_);
      allocator.discharge_to_value(*this->desc_);
   }

protected:
   FuncState* func_state_;
};

//********************************************************************************************************************
// LValue - Assignable/Writable Expression
//
// LValue represents an assignable location (variable, field, index) using a strongly-typed
// variant structure. Unlike RValue, LValue doesn't have discharge semantics; instead, it provides
// storage operations for assignment and mutation.
//
// Value categories:
// - LocalLValue: Local variable
// - UpvalueLValue: Upvalue
// - GlobalLValue: Global variable
// - IndexedLValue: Table slot (table expression + key expression)
// - MemberLValue: Table member (table expression + constant key)

// Strongly-typed alternatives for l-value categories
struct LocalLValue { BCREG reg; };
struct UpvalueLValue { uint32_t index; };
struct GlobalLValue { GCstr* name; };
struct IndexedLValue { BCREG table_reg; BCREG key_reg; };
struct MemberLValue { BCREG table_reg; uint32_t key_const; };

class LValue {
public:
   LValue() noexcept : data_(std::monostate{}) {}
   LValue(const LValue&) = default;
   LValue& operator=(const LValue&) = default;
   LValue(LValue&&) noexcept = default;
   LValue& operator=(LValue&&) noexcept = default;

   // Factory methods for constructing l-values (noexcept for optimization)
   [[nodiscard]] static LValue make_local(BCREG Register) noexcept {
      LValue lval;
      lval.data_ = LocalLValue{Register};
      return lval;
   }

   [[nodiscard]] static LValue make_upvalue(uint32_t Index) noexcept {
      LValue lval;
      lval.data_ = UpvalueLValue{Index};
      return lval;
   }

   [[nodiscard]] static LValue make_global(GCstr* Name) noexcept {
      LValue lval;
      lval.data_ = GlobalLValue{Name};
      return lval;
   }

   [[nodiscard]] static LValue make_indexed(BCREG TableReg, BCREG KeyReg) noexcept {
      LValue lval;
      lval.data_ = IndexedLValue{TableReg, KeyReg};
      return lval;
   }

   [[nodiscard]] static LValue make_member(BCREG TableReg, uint32_t KeyConst) noexcept {
      LValue lval;
      lval.data_ = MemberLValue{TableReg, KeyConst};
      return lval;
   }

   // Create l-value from ExpDesc (conversion utility)
   static LValue from_expdesc(const ExpDesc* Desc);

   // Query l-value kind
   [[nodiscard]] inline bool is_local() const noexcept { return std::holds_alternative<LocalLValue>(data_); }
   [[nodiscard]] inline bool is_upvalue() const noexcept { return std::holds_alternative<UpvalueLValue>(data_); }
   [[nodiscard]] inline bool is_global() const noexcept { return std::holds_alternative<GlobalLValue>(data_); }
   [[nodiscard]] inline bool is_indexed() const noexcept { return std::holds_alternative<IndexedLValue>(data_); }
   [[nodiscard]] inline bool is_member() const noexcept { return std::holds_alternative<MemberLValue>(data_); }

   // Accessors for variant data
   [[nodiscard]] inline BCREG get_local_reg() const { return std::get<LocalLValue>(data_).reg; }
   [[nodiscard]] inline uint32_t get_upvalue_index() const { return std::get<UpvalueLValue>(data_).index; }
   [[nodiscard]] inline GCstr* get_global_name() const { return std::get<GlobalLValue>(data_).name; }
   [[nodiscard]] inline BCREG get_table_reg() const {
      // Optimized: check most common case first (indexed is more common than member)
      if (is_indexed()) return std::get<IndexedLValue>(data_).table_reg;
      return std::get<MemberLValue>(data_).table_reg;
   }
   [[nodiscard]] inline BCREG get_key_reg() const { return std::get<IndexedLValue>(data_).key_reg; }
   [[nodiscard]] inline uint32_t get_key_const() const { return std::get<MemberLValue>(data_).key_const; }

private:
   std::variant<std::monostate, LocalLValue, UpvalueLValue, GlobalLValue, IndexedLValue, MemberLValue> data_;
};
