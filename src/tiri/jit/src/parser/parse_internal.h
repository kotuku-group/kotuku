// Copyright © 2025-2026 Paul Manias
//
// NOT TO BE INCLUDED OUTSIDE THE PARSER.

#pragma once

#include <iterator>
#include <ranges>
#include <string_view>
#include <cstdint>

#include "parse_concepts.h"  // Must be early for concept-constrained templates

enum class TokenKind : uint16_t;

// Constants (lj_parse_constants.cpp)

// Exported for use by OperatorEmitter facade
extern BCREG const_num(FuncState *, ExpDesc *);
extern BCREG const_str(FuncState *, ExpDesc *);
static BCREG const_gc(FuncState *, GCobj *, uint32_t);

// Jump list handling (lj_parse_constants.cpp)

class JumpListView : public std::ranges::view_interface<JumpListView> {
public:
   class Iterator {
   public:
      using iterator_category = std::forward_iterator_tag;
      using difference_type = ptrdiff_t;
      using value_type = BCPos;

      Iterator(FuncState *State, BCPos Position) : func_state(State), position(Position) { }

      [[nodiscard]] BCPos operator*() const { return this->position; }
      Iterator& operator++() { this->position = next(this->func_state, this->position); return *this; }
      [[nodiscard]] bool operator==(const Iterator& Other) const { return position.raw() IS Other.position.raw(); }
      [[nodiscard]] bool operator!=(const Iterator& Other) const { return not(position.raw() IS Other.position.raw()); }

      private:
      FuncState* func_state;
      BCPos position;
   };

   JumpListView(FuncState* State, BCPOS Head) : func_state(State), list_head(Head) { }

   [[nodiscard]] inline Iterator begin() const { return Iterator(func_state, BCPos(list_head)); }
   [[nodiscard]] inline Iterator end() const { return Iterator(func_state, BCPos(NO_JMP)); }
   [[nodiscard]] inline bool empty() const { return list_head IS NO_JMP; }
   [[nodiscard]] inline BCPOS head() const { return list_head; }
   [[nodiscard]] inline BCPos next(BCPos Position) const { return next(func_state, Position); }

   // Enable range-based algorithms via ADL
   friend auto begin(const JumpListView& v) { return v.begin(); }
   friend auto end(const JumpListView& v) { return v.end(); }

   [[nodiscard]] static inline BCPos next(FuncState* State, BCPos Position) {
      ptrdiff_t delta = bc_j(State->bcbase[Position.raw()].ins);
      if (BCPOS(delta) IS NO_JMP) return BCPos(NO_JMP);
      return BCPos(BCPOS((ptrdiff_t(Position.raw()) + 1) + delta));
   }

   [[nodiscard]] bool produces_values() const;
   [[nodiscard]] bool patch_test_register(BCPOS Position, BCREG Register) const;
   void drop_values() const;
   [[nodiscard]] BCPOS append(BCPOS Other) const;
   void patch_with_value(BCPOS ValueTarget, BCREG Register, BCPOS DefaultTarget) const;
   void patch_to_here() const;
   void patch_to(BCPOS Target) const;
   void patch_head(BCPOS Destination) const;

private:
   void patch_instruction(BCPOS Position, BCPOS Destination) const;

   FuncState* func_state;
   BCPOS list_head;
};

// Consume a flag from an expression, clearing it and returning whether it was set.
// Use this when an operator takes ownership of a flagged value.

[[nodiscard]] static inline bool expr_consume_flag(ExpDesc* Expression, ExprFlag Flag)
{
   if (has_flag(Expression->flags, Flag)) {
      Expression->flags &= ~Flag;
      return true;
   }
   return false;
}

// Register allocation (lj_parse_regalloc.cpp)

static void bcreg_bump(FuncState *, BCREG n);
static void bcreg_reserve(FuncState *, BCREG n);
static void bcreg_free(FuncState *, BCREG reg);
static void expr_free(FuncState *, ExpDesc* e);

// Bytecode emission (lj_parse_regalloc.cpp)

// Exported for use by OperatorEmitter facade
extern BCPOS bcemit_INS(FuncState *, BCIns ins);

// Bytecode emission helper functions.
// Note: These templates remain on raw types for compatibility with C macros (BCINS_*)
// Call-sites should wrap results with BCPos() when needed
// Templates constrained with BytecodeOpcode concept for type safety

template<BytecodeOpcode Op>
static inline BCPOS bcemit_ABC(FuncState *fs, Op o, BCREG a, BCREG b, BCREG c) {
   return bcemit_INS(fs, BCINS_ABC(o, a, b, c));
}

template<BytecodeOpcode Op>
static inline BCPOS bcemit_AD(FuncState *fs, Op o, BCREG a, BCREG d) {
   return bcemit_INS(fs, BCINS_AD(o, a, d));
}

template<BytecodeOpcode Op>
static inline BCPOS bcemit_AJ(FuncState *fs, Op o, BCREG a, BCPOS j) {
   return bcemit_INS(fs, BCINS_AJ(o, a, j));
}

// Emit BC_TGETS with overflow protection. When the string constant index exceeds 255 (the 8-bit C field limit),
// falls back to BC_KSTR + BC_TGETV to avoid bytecode corruption.

static inline void bcemit_tgets(FuncState *fs, BCREG Dest, BCREG Table, BCREG StrConstIdx)
{
   if (StrConstIdx <= BCMAX_C) {
      bcemit_ABC(fs, BC_TGETS, Dest, Table, StrConstIdx);
   }
   else {
      BCREG key_reg = fs->freereg;
      bcreg_reserve(fs, 1);
      bcemit_AD(fs, BC_KSTR, key_reg, StrConstIdx);
      bcemit_ABC(fs, BC_TGETV, Dest, Table, key_reg);
      fs->freereg--;
   }
}

static void expr_discharge(FuncState *, ExpDesc* e);
static void bcemit_nil(FuncState *, BCREG from, BCREG n);
static void expr_toreg_nobranch(FuncState *, ExpDesc* e, BCREG reg);
static void expr_toreg(FuncState *, ExpDesc* e, BCREG reg);
static void expr_tonextreg(FuncState *, ExpDesc* e);
static BCREG expr_toanyreg(FuncState *, ExpDesc* e);
static void expr_toval(FuncState *, ExpDesc* e);
static void bcemit_store(FuncState *, ExpDesc* var, ExpDesc* e);
static void bcemit_method(FuncState *, ExpDesc* e, ExpDesc* key);
// These are now exported (non-static) for use by OperatorEmitter facade
extern BCPOS bcemit_jmp(FuncState *);
extern void invertcond(FuncState *, ExpDesc* e);
extern BCPOS bcemit_branch(FuncState *, ExpDesc* e, int cond);

// Extended 64-bit BCIns pointer helpers for ABCP/ADP and AP formats.
// For ABCP/ADP format (32-bit upper field)
static inline void bcemit_set_p32(FuncState *fs, BCPOS pc, uint32_t val) {
   setbc_p32(&fs->bcbase[pc].ins, val);
}

// For AP format (48-bit pointer)
static inline void bcemit_set_ptr(FuncState *fs, BCPOS pc, void *ptr) {
   setbc_ptr(&fs->bcbase[pc].ins, ptr);
}

// These remain static (legacy parser only)
static void bcemit_branch_t(FuncState *, ExpDesc* e);

// Operators are implemented in operator_emitter.cpp via the OperatorEmitter class

// Variables and scope (lj_parse_scope.cpp)

[[nodiscard]] static int is_blank_identifier(GCstr* name);
[[nodiscard]] static std::optional<BCREG> var_lookup_local(FuncState *, GCstr* n);
[[nodiscard]] static MSize var_lookup_uv(FuncState *, MSize vidx, ExpDesc* e);

// Function scope (lj_parse_scope.cpp)

static void fscope_begin(FuncState *, FuncScope* bl, FuncScopeFlag flags);
static void execute_defers(FuncState *, BCREG limit);
static void execute_closes(FuncState *, BCREG limit);
static void fscope_end(FuncState *);
static void fscope_uvmark(FuncState *, BCREG level);

#include "parse_raii.h"
#include "parse_regalloc.h"

// Function state (lj_parse_scope.cpp)

static void fs_fixup_bc(FuncState *, GCproto* pt, BCIns* bc, MSize n);
static void fs_fixup_uv2(FuncState *, GCproto* pt);
static void fs_fixup_k(FuncState *, GCproto* pt, void* kptr);
static void fs_fixup_uv1(FuncState *, GCproto* pt, uint16_t* uv);
[[nodiscard]] static size_t fs_prep_line(FuncState *);
static void fs_fixup_line(FuncState *, GCproto*, void*, BCLine);
[[nodiscard]] static int bcopisret(BCOp op);
static void fs_fixup_ret(FuncState *);

// Expressions (lj_parse_expr.cpp)

static void expr_index(FuncState *, ExpDesc* t, ExpDesc* e);
static void expr_kvalue(FuncState *, TValue* v, ExpDesc* e);
