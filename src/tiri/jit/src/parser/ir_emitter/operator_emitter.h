// Copyright © 2025-2026 Paul Manias
//
// Operator emission facade that translates AST operator payloads into allocator/CFG-aware bytecode emission.
//
// This class manages register allocation via RegisterAllocator and control flow via ControlFlowGraph, eliminating
// direct freereg manipulation.
//
// OPERAND CONTRACT:
// - Left operand (ExprValue): Mutable reference - may be rewritten by operator methods
// - Right operand (ExpDesc): Passed by value - logically read-only from caller's perspective
//   (though internal copies may be modified during emission)
//
// EXTENDED FALSEY SEMANTICS:
// Tiri's falsey semantics differ from standard Lua:
// - Falsey values: nil, false, 0 (numeric zero), "" (empty string)
// - All other values are truthy
// - This affects the ?? (if-empty) operator and ? (presence check) operator
// - Use ExpDesc::is_falsey() for compile-time constant checks

#pragma once

#include <optional>

#include "../parse_types.h"
#include "../value_categories.h"

class RegisterAllocator;
class ControlFlowGraph;

class OperatorEmitter {
public:
   explicit OperatorEmitter(FuncState* State, RegisterAllocator* Allocator, ControlFlowGraph* Cfg);

   // Emit unary operator (negate, not, length)
   // Accepts operand as ExprValue, emits bytecode, modifies operand in-place
   void emit_unary(int op, ExprValue operand);

   // Emit bitwise NOT operator (~)
   // Calls bit.bnot library function, modifies operand in-place
   void emit_bitnot(ExprValue operand);

   // Prepare left operand for binary operation
   // Must be called BEFORE evaluating right operand
   // Discharges LHS to appropriate form (register for comparisons, numeric constant or register for arithmetic)
   void emit_binop_left(BinOpr opr, ExprValue left);

   // Emit arithmetic binary operator (add, sub, mul, div, mod, pow, concat)
   // Accepts left/right operands, emits bytecode, stores result in left
   // Uses constant folding when possible
   void emit_binary_arith(BinOpr opr, ExprValue left, ExpDesc right);

   // Emit comparison operator (eq, ne, lt, le, gt, ge)
   // Emits comparison bytecode with jump, stores jump in left operand
   void emit_comparison(BinOpr opr, ExprValue left, ExpDesc right);

   // Emit bitwise binary operator (band, bor, bxor, shl, shr)
   // Treated as arithmetic operators with integer coercion
   void emit_binary_bitwise(BinOpr opr, ExprValue left, ExpDesc right);

   // Logical short-circuit operators - preparation phase
   // These are called BEFORE evaluating RHS to set up short-circuit jumps
   // CFG-based implementation with structured control flow edges
   void prepare_logical_and(ExprValue left);
   void prepare_logical_or(ExprValue left);
   void prepare_if_empty(ExprValue left);

   // Logical short-circuit operators - completion phase
   // These are called AFTER evaluating RHS to complete the operation
   // CFG-based implementation with edge merging and result handling
   void complete_logical_and(ExprValue left, ExpDesc right);
   void complete_logical_or(ExprValue left, ExpDesc right);
   void complete_if_empty(ExprValue left, ExpDesc right);

   // CONCAT operator - preparation phase
   // Called BEFORE evaluating RHS to discharge left to consecutive register
   void prepare_concat(ExprValue left);

   // CONCAT operator - completion phase
   // Called AFTER evaluating RHS to emit BC_CAT instruction with chaining support
   void complete_concat(ExprValue left, ExpDesc right);

   // Bitwise operator - preparation phase
   // Called BEFORE evaluating RHS to reserve call frame registers for bit.* library call
   // This ensures RHS is evaluated into the correct argument register
   void prepare_bitwise(ExprValue left);

   // Bitwise operator - completion phase
   // Called AFTER evaluating RHS to emit bit.* library call
   void complete_bitwise(BinOpr opr, ExprValue left, ExpDesc right);

   // Presence check operator (x?)
   // Emits bytecode to check if value is truthy (extended falsey semantics)
   // Returns boolean: true if truthy, false if falsey (nil, false, 0, "")
   void emit_presence_check(ExprValue operand);

private:
   FuncState *func_state;
   RegisterAllocator *allocator;
   ControlFlowGraph *cfg;
};
