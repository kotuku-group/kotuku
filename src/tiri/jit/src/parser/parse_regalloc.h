// Register allocation interface and RAII helpers for LuaJIT parser.
// Copyright (C) 2025-2026 Paul Manias.

#pragma once

struct ExpDesc;
struct FuncState;

class RegisterAllocator;

//********************************************************************************************************************

class RegisterSpan {
public:
   RegisterSpan() : allocator_(nullptr), start_(BCReg(0)), count_(BCReg(0)), expected_top_(BCReg(0)) {}
   RegisterSpan(RegisterSpan&& Other) noexcept
      : allocator_(Other.allocator_), start_(Other.start_), count_(Other.count_), expected_top_(Other.expected_top_)
   {
      Other.allocator_ = nullptr;
   }
   RegisterSpan& operator=(RegisterSpan&& Other) noexcept
   {
      if (this IS &Other) return *this;

      this->release();
      allocator_ = Other.allocator_;
      start_ = Other.start_;
      count_ = Other.count_;
      expected_top_ = Other.expected_top_;
      Other.allocator_ = nullptr;
      return *this;
   }
   ~RegisterSpan() { this->release(); }

   inline void release();

   [[nodiscard]] bool empty() const { return count_.raw() IS 0; }
   [[nodiscard]] BCReg start() const { return start_; }
   [[nodiscard]] BCReg count() const { return count_; }
   [[nodiscard]] BCReg expected_top() const { return expected_top_; }

private:
   friend class RegisterAllocator;

   RegisterSpan(RegisterAllocator* Allocator, BCReg Start, BCReg Count, BCReg ExpectedTop)
      : allocator_(Allocator), start_(Start), count_(Count), expected_top_(ExpectedTop) {}

   RegisterAllocator* allocator_;
   BCReg start_;
   BCReg count_;
   BCReg expected_top_;
};

//********************************************************************************************************************

class AllocatedRegister {
public:
   AllocatedRegister() : allocator_(nullptr), index_(BCReg(NO_REG)), expected_top_(BCReg(0)) {}
   AllocatedRegister(AllocatedRegister&& Other) noexcept
      : allocator_(Other.allocator_), index_(Other.index_), expected_top_(Other.expected_top_)
   {
      Other.allocator_ = nullptr;
   }
   AllocatedRegister& operator=(AllocatedRegister &&Other) noexcept
   {
      if (this IS &Other) return *this;

      this->release();
      allocator_ = Other.allocator_;
      index_ = Other.index_;
      expected_top_ = Other.expected_top_;
      Other.allocator_ = nullptr;
      return *this;
   }
   ~AllocatedRegister() { this->release(); }

   inline void release();

   [[nodiscard]] bool valid() const { return allocator_ != nullptr; }
   [[nodiscard]] BCReg index() const { return index_; }
   [[nodiscard]] BCReg expected_top() const { return expected_top_; }

private:
   friend class RegisterAllocator;

   AllocatedRegister(RegisterAllocator* Allocator, BCReg Index, BCReg ExpectedTop)
      : allocator_(Allocator), index_(Index), expected_top_(ExpectedTop) {}

   RegisterAllocator* allocator_;
   BCReg index_;
   BCReg expected_top_;
};

//********************************************************************************************************************

struct TableOperandCopies {
   ExpDesc duplicated;
   RegisterSpan reserved;
};

//********************************************************************************************************************

class RegisterAllocator {
public:
   inline explicit RegisterAllocator(FuncState* State);

   void bump(BCReg Count);
   inline void reserve(BCReg Count);

   [[nodiscard]] inline AllocatedRegister acquire();
   // Reserve a strict RAII span: the allocator expects the span to be released
   // while freereg still equals the top of the span. Used when callers rely on
   // RegisterSpan to pop temporaries in LIFO order.
   [[nodiscard]] RegisterSpan reserve_span(BCReg Count);

   // Reserve a "soft" span: the allocator tracks the range but does not enforce
   // RAII invariants or adjust freereg when the span is released. This is used
   // in patterns where callers explicitly manage freereg (e.g. assignment
   // emitters that duplicate table operands and later collapse freereg to
   // varmap.size()).
   [[nodiscard]] RegisterSpan reserve_span_soft(BCReg Count);

   void release(AllocatedRegister& Handle);
   void release(RegisterSpan& Span);
   void release_register(BCReg Register);
   void release_expression(ExpDesc* Expression);

   void collapse_freereg(BCReg ResultReg);

   [[nodiscard]] TableOperandCopies duplicate_table_operands(const ExpDesc& Expression);

   [[nodiscard]] FuncState* state() const { return func_state; }

   // Expression management methods (migrated from static functions)
   // These provide type-safe discharge and storage operations for expressions

   // Discharge expression to any available form (non-constant to register)
   void discharge(ExpDesc& Expression);

   // Discharge expression to specific register with branch handling
   void discharge_to_register(ExpDesc& Expression, BCReg Target);

   // Discharge expression to specific register without branch handling
   void discharge_to_register_nobranch(ExpDesc& Expression, BCReg Target);

   // Discharge expression to next free register
   void discharge_to_next_register(ExpDesc& Expression);

   // Discharge expression to any register (allocates if needed)
   [[nodiscard]] BCReg discharge_to_any_register(ExpDesc& Expression);

   // Partially discharge expression to value form
   void discharge_to_value(ExpDesc& Expression);

   // Store expression value into variable location
   void store_value(ExpDesc& Variable, ExpDesc& Value);

   // Emit bytecode to set register range to nil
   void emit_nil_range(BCReg Start, BCReg Count);

   // Debug verification methods
   void verify_no_leaks(const char* Context) const;
   void trace_allocation(BCReg Start, BCReg Count, const char* Context) const;
   void trace_release(BCReg Start, BCReg Count, const char* Context) const;

private:
   BCReg reserve_slots(BCReg Count);
   void release_span_internal(BCReg Start, BCReg Count, BCReg ExpectedTop);

   FuncState* func_state;
};

//********************************************************************************************************************

inline RegisterAllocator::RegisterAllocator(FuncState* State) : func_state(State) { }

inline void RegisterAllocator::reserve(BCReg Count) { this->reserve_slots(Count); }

inline AllocatedRegister RegisterAllocator::acquire() {
   BCReg start = this->reserve_slots(BCReg(1));
   return AllocatedRegister(this, start, BCReg(start.raw() + 1));
}

inline void RegisterSpan::release() { if (allocator_) allocator_->release(*this); }

inline void AllocatedRegister::release() { if (allocator_) allocator_->release(*this); }

// Get pointer to bytecode instruction for expression.

[[nodiscard]] inline BCIns * bcptr(FuncState *fs, const ExpDesc *e) { return &fs->bcbase[e->u.s.info].ins; }
