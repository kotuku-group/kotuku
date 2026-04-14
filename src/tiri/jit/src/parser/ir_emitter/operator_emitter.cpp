// Operator emission facade and bytecode generators.
// Copyright © 2025-2026 Paul Manias
//
// Major portions of arithmetic/comparison emission taken verbatim or adapted from LuaJIT.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Major portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#define LUA_CORE

#include "lj_obj.h"
#include "bytecode/lj_bc.h"
#include "../lexer.h"

#include <kotuku/main.h>

#include "operator_emitter.h"
#include "../parse_internal.h"
#include "../parse_control_flow.h"

//********************************************************************************************************************
// Helper: Check if operator tracing is enabled

static inline bool should_trace_operators(FuncState* fs)
{
   auto prv = (prvTiri *)fs->L->script->ChildPrivate;
   return (prv->JitOptions & JOF::TRACE_OPERATORS) != JOF::NIL;
}

//********************************************************************************************************************
// Helper: Get operator name for logging

static CSTRING get_binop_name(BinOpr opr)
{
   switch (opr) {
      case BinOpr::Add: return "+";
      case BinOpr::Sub: return "-";
      case BinOpr::Mul: return "*";
      case BinOpr::Div: return "/";
      case BinOpr::Mod: return "%";
      case BinOpr::Pow: return "**";
      case BinOpr::Concat: return "..";
      case BinOpr::Equal: return "is";
      case BinOpr::NotEqual: return "!=";
      case BinOpr::LessThan: return "<";
      case BinOpr::LessEqual: return "<=";
      case BinOpr::GreaterThan: return ">";
      case BinOpr::GreaterEqual: return ">=";
      case BinOpr::LogicalAnd: return "and";
      case BinOpr::LogicalOr: return "or";
      default: return "?";
   }
}

static CSTRING get_unop_name(BCOp op)
{
   switch (op) {
      case BC_UNM: return "unary -";
      case BC_NOT: return "not";
      case BC_LEN: return "#";
      default: return "?";
   }
}

//********************************************************************************************************************

static CSTRING get_expkind_name(ExpKind k)
{
   switch (k) {
      case ExpKind::Void: return "void";
      case ExpKind::Nil: return "nil";
      case ExpKind::True: return "true";
      case ExpKind::False: return "false";
      case ExpKind::Num: return "num";
      case ExpKind::Str: return "str";
      case ExpKind::Local: return "local";
      case ExpKind::Upval: return "upval";
      case ExpKind::Global: return "global";
      case ExpKind::Unscoped: return "unscoped";
      case ExpKind::Indexed: return "indexed";
      case ExpKind::Call: return "call";
      case ExpKind::NonReloc: return "nonreloc";
      case ExpKind::Relocable: return "relocable";
      case ExpKind::Jmp: return "jmp";
      default: return "?";
   }
}

//********************************************************************************************************************
// Bytecode emitter for operators - constant folding

// Try constant-folding of arithmetic operators.

[[nodiscard]] static int foldarith(BinOpr opr, ExpDesc* e1, ExpDesc* e2)
{
   TValue o;
   lua_Number n;
   if (!e1->is_num_constant_nojump() or !e2->is_num_constant_nojump()) [[likely]] return 0;
   n = lj_vm_foldarith(e1->number_value(), e2->number_value(), to_arith_offset(opr));
   setnumV(&o, n);
   if (tvisnan(&o) or tvismzero(&o)) [[unlikely]] return 0;  // Avoid NaN and -0 as consts.
   if (LJ_DUALNUM) {
      int32_t k = lj_num2int(n);
      if (lua_Number(k) IS n) {
         setintV(&e1->u.nval, k);
         return 1;
      }
   }
   setnumV(&e1->u.nval, n);
   return 1;
}

//********************************************************************************************************************
// Try constant-folding of bitwise operators.
// Bitwise operations in Lua/LuaJIT operate on 32-bit integers.

[[nodiscard]] static int foldbitwise(BinOpr opr, ExpDesc* e1, ExpDesc* e2)
{
   if (!e1->is_num_constant_nojump() or !e2->is_num_constant_nojump()) [[likely]] return 0;

   // Convert to 32-bit integers using lj_num2bit() to match bit library semantics
   auto k1 = lj_num2bit(e1->number_value());
   auto k2 = lj_num2bit(e2->number_value());
   int32_t result;

   switch (opr) {
      case BinOpr::BitAnd: result = k1 & k2; break;
      case BinOpr::BitOr:  result = k1 | k2; break;
      case BinOpr::BitXor: result = k1 ^ k2; break;
      case BinOpr::ShiftLeft:  result = k1 << (k2 & 31); break;  // Mask shift count to 0-31
      case BinOpr::ShiftRight:  result = int32_t(uint32_t(k1) >> (k2 & 31)); break;  // Unsigned right shift
      default: return 0;
   }

   // Store result as integer if possible, otherwise as number

   if (LJ_DUALNUM) setintV(&e1->u.nval, result);
   else setnumV(&e1->u.nval, lua_Number(result));

   e1->k = ExpKind::Num;
   return 1;
}

//********************************************************************************************************************
// Try constant-folding of unary bitwise NOT.

[[nodiscard]] static int foldbitnot(ExpDesc* e)
{
   if (!e->is_num_constant_nojump()) [[likely]] return 0;

   // Convert to 32-bit integer using lj_num2bit() and apply bitwise NOT

   auto k = lj_num2bit(e->number_value());
   int32_t result = ~k;

   // Store result as integer if possible, otherwise as number

   if (LJ_DUALNUM) setintV(&e->u.nval, result);
   else setnumV(&e->u.nval, lua_Number(result));

   e->k = ExpKind::Num;
   return 1;
}

//********************************************************************************************************************
// Emit arithmetic operator.

static void bcemit_arith(FuncState* fs, BinOpr opr, ExpDesc* e1, ExpDesc* e2)
{
   RegisterAllocator allocator(fs);
   BCREG rb, rc, t;
   uint32_t op;

   if (foldarith(opr, e1, e2)) return;

   if (opr IS BinOpr::Pow) {
      op = BC_POW;
      ExpressionValue e2_value(fs, *e2);
      rc = e2_value.discharge_to_any_reg(allocator);
      *e2 = e2_value.legacy();
      ExpressionValue e1_value(fs, *e1);
      rb = e1_value.discharge_to_any_reg(allocator);
      *e1 = e1_value.legacy();
   }
   else {
      op = to_arith_offset(opr) + BC_ADDVV;
      // Must discharge 2nd operand first since ExpKind::Indexed might free regs.
      ExpressionValue e2_toval(fs, *e2);
      e2_toval.to_val();
      *e2 = e2_toval.legacy();
      if (e2->is_num_constant() and (rc = const_num(fs, e2)) <= BCMAX_C) op -= BC_ADDVV - BC_ADDVN;
      else {
         ExpressionValue e2_value(fs, *e2);
         rc = e2_value.discharge_to_any_reg(allocator);
         *e2 = e2_value.legacy();
      }

      // 1st operand discharged by bcemit_binop_left, but need KNUM/KSHORT.

      fs_check_assert(fs,e1->is_num_constant() or e1->k IS ExpKind::NonReloc, "bad expr type %d", e1->k);
      ExpressionValue e1_toval(fs, *e1);
      e1_toval.to_val();
      *e1 = e1_toval.legacy();

      // Avoid two consts to satisfy bytecode constraints.

      if (e1->is_num_constant() and (not e2->is_num_constant()) and (t = const_num(fs, e1)) <= BCMAX_B) {
         rb = rc; rc = t; op -= BC_ADDVV - BC_ADDNV;
      }
      else {
         ExpressionValue e1_value(fs, *e1);
         rb = e1_value.discharge_to_any_reg(allocator);
         *e1 = e1_value.legacy();
      }
   }

   // Release operand registers through allocator
   allocator.release_expression(e2);
   allocator.release_expression(e1);
   e1->u.s.info = bcemit_ABC(fs, op, 0, rb, rc);
   e1->k = ExpKind::Relocable;
   e1->result_type = TiriType::Num;  // Arithmetic operations always return number
}

//********************************************************************************************************************
// Emit comparison operator.

static void bcemit_comp(FuncState* fs, BinOpr opr, ExpDesc* e1, ExpDesc* e2)
{
   RegisterAllocator allocator(fs);
   ExpDesc *eret = e1;
   BCIns ins;
   BCReg cmp_reg_a = BCReg(NO_REG), cmp_reg_b = BCReg(NO_REG);  // Track registers used by comparison
   ExpressionValue e1_toval_pre(fs, *e1);

   e1_toval_pre.to_val();
   *e1 = e1_toval_pre.legacy();
   if (opr IS BinOpr::Equal or opr IS BinOpr::NotEqual) {
      BCOp op = opr IS BinOpr::Equal ? BC_ISEQV : BC_ISNEV;
      BCReg ra;

      if (e1->is_constant()) { e1 = e2; e2 = eret; }  // Need constant in 2nd arg.
      ExpressionValue e1_value(fs, *e1);
      ra = e1_value.discharge_to_any_reg(allocator);  // First arg must be in a reg.
      *e1 = e1_value.legacy();
      cmp_reg_a = ra;
      ExpressionValue e2_toval(fs, *e2);
      e2_toval.to_val();
      *e2 = e2_toval.legacy();

      switch (e2->k) {
         case ExpKind::Nil:
         case ExpKind::False:
         case ExpKind::True:
            ins = BCINS_AD(op + (BC_ISEQP - BC_ISEQV), ra, const_pri(e2));
            break;
         case ExpKind::Str:
            ins = BCINS_AD(op + (BC_ISEQS - BC_ISEQV), ra, const_str(fs, e2));
            break;
         case ExpKind::Num:
            ins = BCINS_AD(op + (BC_ISEQN - BC_ISEQV), ra, const_num(fs, e2));
            break;
         default: {
            ExpressionValue e2_value(fs, *e2);
            BCReg rb = e2_value.discharge_to_any_reg(allocator);
            *e2 = e2_value.legacy();
            cmp_reg_b = rb;
            ins = BCINS_AD(op, ra, rb);
            break;
         }
      }
   }
   else {
      uint32_t op = (int(opr) - int(BinOpr::LessThan)) + BC_ISLT;
      BCReg ra, rd;
      if ((op - BC_ISLT) & 1) {  // GT -> LT, GE -> LE
         e1 = e2; e2 = eret;  // Swap operands.
         op = ((op - BC_ISLT) ^ 3) + BC_ISLT;
         ExpressionValue e1_toval_swap(fs, *e1);
         e1_toval_swap.to_val();
         *e1 = e1_toval_swap.legacy();
         ExpressionValue e1_value(fs, *e1);
         ra = e1_value.discharge_to_any_reg(allocator);
         *e1 = e1_value.legacy();
         ExpressionValue e2_value(fs, *e2);
         rd = e2_value.discharge_to_any_reg(allocator);
         *e2 = e2_value.legacy();
      }
      else {
         ExpressionValue e2_value(fs, *e2);
         rd = e2_value.discharge_to_any_reg(allocator);
         *e2 = e2_value.legacy();
         ExpressionValue e1_value(fs, *e1);
         ra = e1_value.discharge_to_any_reg(allocator);
         *e1 = e1_value.legacy();
      }
      cmp_reg_a = ra;
      cmp_reg_b = rd;
      ins = BCINS_AD(op, ra, rd);
   }

   // Emit the comparison instruction now that operands are prepared.
   bcemit_INS(fs, ins);

   // Explicitly release operand registers through the allocator.
   // Release in LIFO order (highest register first) to maximise the chance of
   // collapsing freereg when both operands are adjacent temporaries.
   if (cmp_reg_b != NO_REG and cmp_reg_b > cmp_reg_a) {
      allocator.release_register(cmp_reg_b);
      allocator.release_register(cmp_reg_a);
   }
   else {
      allocator.release_register(cmp_reg_a);
      if (cmp_reg_b != NO_REG) allocator.release_register(cmp_reg_b);
   }

   // Produce a Jmp expression as the result of the comparison, preserving
   // existing short-circuit and conditional semantics.
   eret->u.s.info = bcemit_jmp(fs);
   eret->k = ExpKind::Jmp;
   eret->result_type = TiriType::Bool;  // Comparison operations always return boolean
}

//********************************************************************************************************************
// Emit a call to a bit library function (bit.band, bit.bor, bit.bxor, bit.lshift, bit.rshift) at a specific base
// register.
//
// This function is used to implement C-style bitwise operators (&, |, ~, <<, >>) by translating them into calls to
// LuaJIT's bit library functions. The base register is explicitly provided to allow chaining of multiple bitwise
// operations while reusing the same register for intermediate results.
//
// Register Layout (x64 with LJ_FR2=1):
//   base     - Function to call (bit.band, bit.bor, etc.)
//   base+1   - Frame link register (LJ_FR2, not an argument)
//   base+2   - arg1: First operand
//   base+3   - arg2: Second operand
//
// BC_CALL Instruction Format:
//   - A field: base register (where function is located and result will be stored)
//   - B field: Expected result count + 1 (B=2 means 1 result, B=0 means variable/forward all)
//   - C field: Argument count + 1
//
// ExpKind::Call Handling (Multi-Return Functions):
//   When an operand is a function call returning multiple values, standard Lua binary operator semantics apply:
//   only the first return value is used. The caller (bcemit_bit_call) discharges ExpKind::Call expressions to
//   ExpKind::NonReloc BEFORE calling this function, ensuring proper register allocation and truncation of
//   multi-return values. This matches the behaviour of expressions like `x + f()` in Lua.
//
//   Note: Unlike function argument lists (which use BC_CALLM to forward all return values), binary operators
//   always restrict multi-return expressions to single values. This is a fundamental Lua language semantic.
//
// Parameters:
//   fs    - Function state for bytecode generation
//   fname - Name of bit library function (e.g., "band", "bor", "lshift")
//   lhs   - Left-hand side expression (first operand, already discharged if was ExpKind::Call)
//   rhs   - Right-hand side expression (second operand, already discharged if was ExpKind::Call)
//   base  - Base register for the call (allows register reuse for chaining)

static void bcemit_shift_call_at_base(FuncState* fs, std::string_view fname, ExpDesc* lhs, ExpDesc* rhs, BCREG base)
{
   RegisterAllocator allocator(fs);
   ExpDesc callee, key;
   auto arg1 = BCReg(base + 1  + 1);  // First argument register (after frame link if present)
   auto arg2 = arg1 + 1;            // Second argument register

   // Normalise both operands to value form first.

   ExpressionValue lhs_toval(fs, *lhs);
   lhs_toval.to_val();
   *lhs = lhs_toval.legacy();

   ExpressionValue rhs_toval(fs, *rhs);
   rhs_toval.to_val();
   *rhs = rhs_toval.legacy();

   // If base is the same as LHS or RHS register, we must move that operand first before
   // loading callee to base. This prevents clobbering.
   // Note: Only NonReloc needs checking here because:
   // - Local slots are always < varmap.size(), but base is always >= varmap.size() (from bcemit_bit_call)
   // - Relocable expressions don't have an assigned register yet
   // - Constants don't occupy registers
   bool lhs_was_base = (lhs->k IS ExpKind::NonReloc and lhs->u.s.info IS base);
   bool rhs_was_base = (rhs->k IS ExpKind::NonReloc and rhs->u.s.info IS base);

   // Save original ExpDesc values before any moves. This is critical when both operands
   // are at the base register (same expression or aliased). After moving LHS, the original
   // register value is needed to correctly move RHS.
   ExpDesc lhs_original = *lhs;
   ExpDesc rhs_original = *rhs;

   // Defensive assertions: verify our assumptions about register allocation
   fs_check_assert(fs, not (lhs->k IS ExpKind::Local and lhs->u.s.info IS base),
      "unexpected: Local at base register (base should be >= varmap.size())");
   fs_check_assert(fs, not (rhs->k IS ExpKind::Local and rhs->u.s.info IS base),
      "unexpected: Local at base register (base should be >= varmap.size())");

   if (lhs_was_base) {
      // LHS is at base, move it to arg1 first (use original value)
      ExpressionValue lhs_value(fs, lhs_original);
      lhs_value.to_reg(allocator, arg1);
      *lhs = lhs_value.legacy();
   }

   if (rhs_was_base) {
      // RHS is at base, move it to arg2 first (use original value)
      ExpressionValue rhs_value(fs, rhs_original);
      rhs_value.to_reg(allocator, BCReg(arg2));
      *rhs = rhs_value.legacy();
   }

   // Ensure freereg is past the call frame to prevent callee loading from clobbering
   if (fs->freereg <= arg2) fs->freereg = arg2 + 1;

   // Now load bit.fname to base (safe since any operand at base has been moved)
   callee.init(ExpKind::Global, 0);
   callee.u.sval = fs->ls->keepstr("bit");
   ExpressionValue callee_value(fs, callee);
   callee_value.discharge_to_any_reg(allocator);
   callee = callee_value.legacy();
   key.init(ExpKind::Str, 0);
   key.u.sval = fs->ls->keepstr(fname);
   expr_index(fs, &callee, &key);
   ExpressionValue callee_toval(fs, callee);
   callee_toval.to_val();
   callee = callee_toval.legacy();
   ExpressionValue callee_to_base(fs, callee);
   callee_to_base.to_reg(allocator, BCReg(base));
   callee = callee_to_base.legacy();

   // Now move any remaining operands that weren't at base
   if (not lhs_was_base) {
      ExpressionValue lhs_value(fs, *lhs);
      lhs_value.to_reg(allocator, arg1);
      *lhs = lhs_value.legacy();
   }

   if (not rhs_was_base) {
      ExpressionValue rhs_value(fs, *rhs);
      rhs_value.to_reg(allocator, BCReg(arg2));
      *rhs = rhs_value.legacy();
   }

   // Emit CALL instruction

   fs->freereg = arg2 + 1;  // Ensure freereg covers all arguments
   lhs->k = ExpKind::Call;
   lhs->u.s.info = bcemit_INS(fs, BCINS_ABC(BC_CALL, base, 2, fs->freereg - base  - 1));
   lhs->u.s.aux = base;
   fs->freereg = base + 1;

   ExpressionValue lhs_value_discharge(fs, *lhs);
   lhs_value_discharge.discharge();
   *lhs = lhs_value_discharge.legacy();
   fs_check_assert(fs,lhs->k IS ExpKind::NonReloc and lhs->u.s.info IS base, "bitwise result not in base register");
   lhs->result_type = TiriType::Num;  // Bitwise operations always return number
}

//********************************************************************************************************************
// Emit binary bitwise operator via bit library call.
// Note: Constant folding is performed by the caller (emit_binary_bitwise) before this function is called.

static void bcemit_bit_call(FuncState* fs, std::string_view fname, ExpDesc* lhs, ExpDesc* rhs)
{
   RegisterAllocator allocator(fs);

   // Discharge Call expressions to NonReloc first. This ensures that function calls
   // returning multiple values are properly truncated to single values before being
   // used as operands, matching Lua's standard semantics for binary operators.
   // Without this, the base register check below fails for Call expressions, causing
   // the result to go to a different register than expected.

   if (lhs->k IS ExpKind::Call) {
      ExpressionValue lhs_discharge(fs, *lhs);
      lhs_discharge.discharge();
      *lhs = lhs_discharge.legacy();
   }

   if (rhs->k IS ExpKind::Call) {
      ExpressionValue rhs_discharge(fs, *rhs);
      rhs_discharge.discharge();
      *rhs = rhs_discharge.legacy();
   }

   // Allocate a base register for the call
   // Check if either operand is already at the top of the stack to avoid orphaning registers
   // when chaining operations (e.g., 1 | 2 | 4 produces AST: (1 | 2) | 4, so LHS is the previous result)

   BCREG base;
   if (rhs->k IS ExpKind::NonReloc and rhs->u.s.info >= fs->varmap.size() and rhs->u.s.info + 1 IS fs->freereg) {
      // RHS is at the top - reuse its register to avoid orphaning
      base = rhs->u.s.info;
   }
   else if (lhs->k IS ExpKind::NonReloc and lhs->u.s.info >= fs->varmap.size() and lhs->u.s.info + 1 IS fs->freereg) {
      // LHS is at the top - reuse its register to avoid orphaning
      base = lhs->u.s.info;
   }
   else base = fs->freereg;

   allocator.reserve(BCReg(1));  // Reserve for callee
   allocator.reserve(BCReg(1));
   allocator.reserve(BCReg(2));  // Reserve for arguments
   fs_check_assert(fs,!fname.empty(), "bitlib name missing for bitwise operator");
   bcemit_shift_call_at_base(fs, fname, lhs, rhs, base);
}

//********************************************************************************************************************
// Emit unary bit library call (e.g., bit.bnot).

static void bcemit_unary_bit_call(FuncState* fs, std::string_view fname, ExpDesc* arg)
{
   RegisterAllocator allocator(fs);
   ExpDesc callee, key;
   auto base = fs->free_reg();
   BCReg arg_reg = BCReg(int(base) + 1  + 1);

   allocator.reserve(BCReg(1));  // Reserve for callee
   allocator.reserve(BCReg(1));  // Reserve for frame link on x64

   // Place argument in register.

   ExpressionValue arg_toval(fs, *arg);
   arg_toval.to_val();
   *arg = arg_toval.legacy();
   ExpressionValue arg_value(fs, *arg);
   arg_value.to_reg(allocator, arg_reg);
   *arg = arg_value.legacy();

   // Ensure freereg accounts for argument register so it's not clobbered.

   if (fs->freereg <= arg_reg) fs->freereg = arg_reg + 1;

   // Load bit.fname into base register.

   callee.init(ExpKind::Global, 0);
   callee.u.sval = fs->ls->keepstr("bit");
   ExpressionValue callee_value(fs, callee);
   callee_value.discharge_to_any_reg(allocator);
   callee = callee_value.legacy();
   key.init(ExpKind::Str, 0);
   key.u.sval = fs->ls->keepstr(fname);
   expr_index(fs, &callee, &key);
   ExpressionValue callee_toval2(fs, callee);
   callee_toval2.to_val();
   callee = callee_toval2.legacy();
   ExpressionValue callee_value2(fs, callee);
   callee_value2.to_reg(allocator, base);
   callee = callee_value2.legacy();

   // Emit CALL instruction.
   fs->freereg = arg_reg + 1;
   arg->k = ExpKind::Call;
   arg->u.s.info = bcemit_INS(fs, BCINS_ABC(BC_CALL, base, 2, fs->freereg - base  - 1));
   arg->u.s.aux = base;
   fs->freereg = base + 1;

   // Discharge result to register.
   ExpressionValue arg_value_discharge(fs, *arg);
   arg_value_discharge.discharge();
   *arg = arg_value_discharge.legacy();
   fs_check_assert(fs,arg->k IS ExpKind::NonReloc and arg->u.s.info IS base, "bitwise result not in base register");
   arg->result_type = TiriType::Num;  // Bitwise operations always return number
}

//********************************************************************************************************************
// Emit unary operator.

static void bcemit_unop(FuncState* fs, BCOp op, ExpDesc* e)
{
   RegisterAllocator allocator(fs);

   if (op IS BC_NOT) {
      // Swap true and false lists.
      { BCPos temp = BCPos(e->f); e->f = e->t; e->t = temp.raw(); }
      ControlFlowGraph cfg(fs);
      ControlFlowEdge false_edge = cfg.make_false_edge(BCPos(e->f));
      false_edge.drop_values();
      ControlFlowEdge true_edge = cfg.make_true_edge(BCPos(e->t));
      true_edge.drop_values();
      ExpressionValue e_value(fs, *e);
      e_value.discharge();
      *e = e_value.legacy();
      if (e->k IS ExpKind::Nil or e->k IS ExpKind::False) {
         e->k = ExpKind::True;
         return;
      }
      else if (e->is_constant()) {
         e->k = ExpKind::False;
         return;
      }
      else if (e->k IS ExpKind::Jmp) {
         invertcond(fs, e);
         e->result_type = TiriType::Bool;  // NOT always returns boolean
         return;
      }
      else if (e->k IS ExpKind::Relocable) {
         allocator.reserve(BCReg(1));
         setbc_a(bcptr(fs, e), fs->freereg - 1);
         e->u.s.info = fs->freereg - 1;
         e->k = ExpKind::NonReloc;
      }
      else fs_check_assert(fs,e->k IS ExpKind::NonReloc, "bad expr type %d", int(e->k));
      e->result_type = TiriType::Bool;  // NOT always returns boolean
   }
   else {
      fs_check_assert(fs,op IS BC_UNM or op IS BC_LEN, "bad unop %d", op);
      if (op IS BC_UNM and not e->has_jump()) {  // Constant-fold negations.
         if (e->is_num_constant() and !e->is_num_zero()) {  // Avoid folding to -0.
            TValue* o = e->num_tv();
            if (tvisint(o)) {
               int32_t k = intV(o);
               if (k IS -k) setnumV(o, -lua_Number(k));
               else setintV(o, -k);
               return;
            }
            else {
               o->u64 ^= U64x(80000000, 00000000);
               return;
            }
         }
      }
      ExpressionValue e_value(fs, *e);
      e_value.discharge_to_any_reg(allocator);
      *e = e_value.legacy();
   }
   expr_free(fs, e);
   e->u.s.info = bcemit_AD(fs, op, 0, e->u.s.info);
   e->k = ExpKind::Relocable;
   // BC_UNM (negate) and BC_LEN (length) always return number
   e->result_type = TiriType::Num;
}

//********************************************************************************************************************
// OperatorEmitter facade class implementation

OperatorEmitter::OperatorEmitter(FuncState* State, RegisterAllocator* Allocator, ControlFlowGraph* Cfg)
   : func_state(State), allocator(Allocator), cfg(Cfg)
{
}

//********************************************************************************************************************
// Emit unary operator

void OperatorEmitter::emit_unary(int op, ExprValue operand)
{
   if (should_trace_operators(this->func_state)) {
      pf::Log("Parser").msg("[%d] operator %s: operand kind=%s", this->func_state->ls->linenumber.lineNumber(),
         get_unop_name(BCOp(op)), get_expkind_name(operand.kind()));
   }

   bcemit_unop(this->func_state, BCOp(op), operand.raw());
}

//********************************************************************************************************************
// Emit bitwise NOT operator (~)
// Performs constant folding when possible, otherwise calls bit.bnot library function

void OperatorEmitter::emit_bitnot(ExprValue operand)
{
   ExpDesc* e = operand.raw();

   // Try constant folding first
   if (foldbitnot(e)) {
      if (should_trace_operators(this->func_state)) {
         pf::Log("Parser").msg("[%d] operator ~: constant-folded to %d", this->func_state->ls->linenumber.lineNumber(),
            int32_t(e->number_value()));
      }
      return;
   }

   if (should_trace_operators(this->func_state)) {
      pf::Log("Parser").msg("[%d] operator ~: calling bit.bnot, operand kind=%s", this->func_state->ls->linenumber.lineNumber(),
         get_expkind_name(operand.kind()));
   }

   bcemit_unary_bit_call(this->func_state, "bnot", e);
}

//********************************************************************************************************************
// Prepare left operand for binary operation
// MUST be called before evaluating right operand to prevent register clobbering
//
// Logical operators (AND, OR, IF_EMPTY, CONCAT) use specialized prepare_* methods instead.

void OperatorEmitter::emit_binop_left(BinOpr opr, ExprValue left)
{
   RegisterAllocator local_alloc(this->func_state);
   ExpDesc *e = left.raw();

   if (opr IS BinOpr::Equal or opr IS BinOpr::NotEqual) {
      // Comparison operators (EQ, NE): discharge to register unless it's a constant/jump
      if (not e->is_constant_nojump()) {
         ExpressionValue e_value(this->func_state, *e);
         e_value.discharge_to_any_reg(local_alloc);
         *e = e_value.legacy();
      }
   }
   else {
      // Arithmetic and bitwise operators: discharge to register unless it's a numeric constant/jump
      // Note: Bitwise operators use emit_bitwise_expr in IrEmitter which handles RHS internally,
      // so this code path is no longer used for bitwise ops in the IR parser.
      if (not e->is_num_constant_nojump()) {
         ExpressionValue e_value(this->func_state, *e);
         e_value.discharge_to_any_reg(local_alloc);
         *e = e_value.legacy();
      }
   }
}

//********************************************************************************************************************
// Emit arithmetic binary operator

void OperatorEmitter::emit_binary_arith(BinOpr opr, ExprValue left, ExpDesc right)
{
   if (should_trace_operators(this->func_state)) {
      pf::Log("Parser").msg("[%d] operator %s: left kind=%s, right kind=%s", this->func_state->ls->linenumber.lineNumber(),
         get_binop_name(opr), get_expkind_name(left.kind()), get_expkind_name(right.k));
   }

   bcemit_arith(this->func_state, opr, left.raw(), &right);
}

//********************************************************************************************************************
// Emit comparison operator

void OperatorEmitter::emit_comparison(BinOpr opr, ExprValue left, ExpDesc right)
{
   if (should_trace_operators(this->func_state)) {
      pf::Log("Parser").msg("[%d] operator %s: left kind=%s, right kind=%s", this->func_state->ls->linenumber.lineNumber(),
         get_binop_name(opr), get_expkind_name(left.kind()), get_expkind_name(right.k));
   }

   bcemit_comp(this->func_state, opr, left.raw(), &right);
}

//********************************************************************************************************************
// Emit bitwise binary operator
// Performs constant folding when possible, otherwise emits function calls to bit.* library

void OperatorEmitter::emit_binary_bitwise(BinOpr opr, ExprValue left, ExpDesc right)
{
   ExpDesc* lhs = left.raw();

   // Try constant folding first
   if (foldbitwise(opr, lhs, &right)) {
      if (should_trace_operators(this->func_state)) {
         pf::Log("Parser").msg("[%d] operator %s: constant-folded to %d", this->func_state->ls->linenumber.lineNumber(),
            get_binop_name(opr), int32_t(lhs->number_value()));
      }
      return;
   }

   CSTRING op_name = priority[int(opr)].name;
   size_t op_name_len = priority[int(opr)].name_len;

   if (should_trace_operators(this->func_state)) {
      pf::Log("Parser").msg("[%d] operator %s: calling bit.%.*s, left kind=%s, right kind=%s",
         this->func_state->ls->linenumber.lineNumber(), get_binop_name(opr), int(op_name_len), op_name, get_expkind_name(left.kind()),
         get_expkind_name(right.k));
   }

   bcemit_bit_call(this->func_state, std::string_view(op_name, op_name_len), lhs, &right);
}

//********************************************************************************************************************
// Bitwise operator - preparation phase (called BEFORE RHS evaluation)
// Sets up the call frame registers so that RHS is evaluated into the correct argument slot.
//
// Register layout for bit.* call with LJ_FR2=1:
//   base     - Function to call (bit.band, bit.bor, etc.)
//   base+1   - Frame link register
//   base+2   - arg1: First operand (LHS)
//   base+3   - arg2: Second operand (RHS) <- freereg positioned here so RHS goes here

void OperatorEmitter::prepare_bitwise(ExprValue left)
{
   ExpDesc* left_desc = left.raw();
   FuncState* fs = this->func_state;
   RegisterAllocator local_alloc(fs);

   // Discharge LHS to any register first (if needed)
   if (not left_desc->is_num_constant_nojump()) {
      ExpressionValue left_val(fs, *left_desc);
      left_val.discharge_to_any_reg(local_alloc);
      *left_desc = left_val.legacy();
   }

   // Calculate base register for the call frame
   BCREG frame_base = fs->freereg;

   // Reserve: callee slot
   local_alloc.reserve(BCReg(1));

   // Reserve: frame link slot
   local_alloc.reserve(BCReg(1));

   // Move LHS to arg1 slot (base+2 with LJ_FR2)
   BCREG arg1 = frame_base + 1 + LJ_FR2;
   ExpressionValue lhs_to_arg1(fs, *left_desc);
   lhs_to_arg1.to_reg(local_alloc, BCReg(arg1));
   *left_desc = lhs_to_arg1.legacy();

   // Reserve arg2 slot - freereg is now positioned at arg2
   // RHS evaluation will naturally go to this slot
   local_alloc.reserve(BCReg(1));

   // Store base in aux field and set flag so complete_bitwise can retrieve it
   ExprFlag saved_flags = left_desc->flags;
   left_desc->flags = saved_flags | ExprFlag::BitwiseBase;
   left_desc->u.s.aux = frame_base;

   if (should_trace_operators(fs)) {
      pf::Log("Parser").msg("[%d] prepare_bitwise: frame_base=%d, arg1=%d, freereg=%d (arg2 slot)",
         fs->ls->linenumber.lineNumber(), frame_base, arg1, fs->freereg);
   }
}

//********************************************************************************************************************
// Bitwise operator - completion phase (called AFTER RHS evaluation)
// Loads the callee, ensures arguments are in place, and emits the call.

void OperatorEmitter::complete_bitwise(BinOpr opr, ExprValue left, ExpDesc right)
{
   ExpDesc* lhs = left.raw();
   FuncState* fs = this->func_state;

   // Try constant folding first - if both operands are constants, we can fold
   if (foldbitwise(opr, lhs, &right)) {
      if (should_trace_operators(fs)) {
         pf::Log("Parser").msg("[%d] complete_bitwise %s: constant-folded to %d",
            fs->ls->linenumber.lineNumber(), get_binop_name(opr), int32_t(lhs->number_value()));
      }
      return;
   }

   // Get the base register from aux field (set by prepare_bitwise)

   fs_check_assert(fs, has_flag(lhs->flags, ExprFlag::BitwiseBase),
      "complete_bitwise called without prepare_bitwise (missing BitwiseBase flag)");

   (void)expr_consume_flag(lhs, ExprFlag::BitwiseBase);
   BCREG base = lhs->u.s.aux;

   BCREG arg1 = base + 1 + LJ_FR2;
   BCREG arg2 = arg1 + 1;

   RegisterAllocator local_alloc(fs);

   // Move RHS to arg2 if not already there
   ExpressionValue rhs_toval(fs, right);
   rhs_toval.to_val();
   right = rhs_toval.legacy();

   ExpressionValue rhs_to_arg2(fs, right);
   rhs_to_arg2.to_reg(local_alloc, BCReg(arg2));
   right = rhs_to_arg2.legacy();

   // Ensure freereg is past arg2 before loading callee to avoid clobbering args
   if (fs->freereg <= arg2) fs->freereg = arg2 + 1;

   // Load bit.fname into base register
   CSTRING op_name = priority[int(opr)].name;
   size_t op_name_len = priority[int(opr)].name_len;

   ExpDesc callee, key;
   callee.init(ExpKind::Global, 0);
   callee.u.sval = fs->ls->keepstr("bit");

   ExpressionValue callee_val(fs, callee);
   callee_val.discharge_to_any_reg(local_alloc);
   callee = callee_val.legacy();

   key.init(ExpKind::Str, 0);
   key.u.sval = fs->ls->keepstr(std::string_view(op_name, op_name_len));
   expr_index(fs, &callee, &key);

   ExpressionValue callee_toval(fs, callee);
   callee_toval.to_val();
   callee = callee_toval.legacy();

   ExpressionValue callee_to_base(fs, callee);
   callee_to_base.to_reg(local_alloc, BCReg(base));
   callee = callee_to_base.legacy();

   // Emit CALL instruction
   fs->freereg = arg2 + 1;  // Ensure freereg covers all arguments
   lhs->k = ExpKind::Call;
   lhs->u.s.info = bcemit_INS(fs, BCINS_ABC(BC_CALL, base, 2, fs->freereg - base - LJ_FR2));
   lhs->u.s.aux = base;
   fs->freereg = base + 1;

   // Discharge call result
   ExpressionValue lhs_discharge(fs, *lhs);
   lhs_discharge.discharge();
   *lhs = lhs_discharge.legacy();

   if (should_trace_operators(fs)) {
      pf::Log("Parser").msg("[%d] complete_bitwise %s: emitted call at base=%d", fs->ls->linenumber.lineNumber(), get_binop_name(opr), base);
   }
}

//********************************************************************************************************************
// Prepare logical AND operator (called BEFORE RHS evaluation)

void OperatorEmitter::prepare_logical_and(ExprValue left)
{
   ExpDesc* left_desc = left.raw();

   // AND short-circuit logic: if left is false, skip RHS and return left (false)
   // If left is true, evaluate RHS and return RHS result

   // Discharge left operand to appropriate form
   ExpressionValue left_val(this->func_state, *left_desc);
   left_val.discharge();
   *left_desc = left_val.legacy();

   BCPOS pc;
   bool will_skip_rhs = false;

   // Handle constant folding
   if (left_desc->k IS ExpKind::Str or left_desc->k IS ExpKind::Num or left_desc->k IS ExpKind::True) {
      // Left is truthy constant - no jump needed, will evaluate RHS
      pc = NO_JMP;
   }
   else if (left_desc->k IS ExpKind::Jmp) {
      // Left is already a jump expression - invert condition
      invertcond(this->func_state, left_desc);
      pc = left_desc->u.s.info;
   }
   else if (left_desc->k IS ExpKind::False or left_desc->k IS ExpKind::Nil) {
      // Left is falsey constant - load to register and jump to skip RHS
      expr_toreg_nobranch(this->func_state, left_desc, NO_REG);
      pc = bcemit_jmp(this->func_state);
      will_skip_rhs = true;
   }
   else { // Runtime value - emit conditional branch (jump if false)
      pc = bcemit_branch(this->func_state, left_desc, 0);
   }

   if (should_trace_operators(this->func_state)) {
      pf::Log("Parser").msg("[%d] operator and: prepare left kind=%s, %s", this->func_state->ls->linenumber.lineNumber(), get_expkind_name(left_desc->k),
         will_skip_rhs ? "will skip RHS (constant false)" : "will evaluate RHS");
   }

   // Set up CFG edges for short-circuit behavior
   // The false_edge collects jumps for "left is false" path - these will be resolved later
   // when the complete_logical_and merges paths. We release it here since it's just
   // being used to accumulate jump positions, not to be patched at this location.

   ControlFlowEdge false_edge = this->cfg->make_false_edge(BCPos(left_desc->f));
   false_edge.append(BCPos(pc));
   left_desc->f = false_edge.head().raw();
   false_edge.release();  // Mark as handled - jumps will be resolved in complete_logical_and

   ControlFlowEdge true_edge = this->cfg->make_true_edge(BCPos(left_desc->t));
   true_edge.patch_here();
   left_desc->t = NO_JMP;
}

//********************************************************************************************************************
// Complete logical AND operator (called AFTER RHS evaluation)

void OperatorEmitter::complete_logical_and(ExprValue left, ExpDesc right)
{
   ExpDesc *left_desc = left.raw();
   ExpDesc *right_desc = &right;

   // At this point:
   // - left->f contains jumps for "left is false" path
   // - right has been evaluated
   // - We need to merge the false paths and return right's result

   FuncState *fs = this->func_state;  // For lj_assertFS macro
   fs_check_assert(fs,left_desc->t IS NO_JMP, "jump list not closed");

   // Discharge right operand
   ExpressionValue right_val(this->func_state, *right_desc);
   right_val.discharge();
   *right_desc = right_val.legacy();

   if (should_trace_operators(this->func_state)) {
      pf::Log("Parser").msg("[%d] operator and: complete right kind=%s, merging false paths", this->func_state->ls->linenumber.lineNumber(),
         get_expkind_name(right_desc->k));
   }

   // Merge false paths: both "left is false" and "right is false" go to same target
   // This edge accumulates the merged jump list; release it since jumps resolve
   // when the expression result is used (discharged to register, used in condition, etc.)
   ControlFlowEdge false_edge = this->cfg->make_false_edge(BCPos(right_desc->f));
   false_edge.append(BCPos(left_desc->f));
   right_desc->f = false_edge.head().raw();
   false_edge.release();  // Mark as handled - jumps will be resolved by caller

   // Result is right's value
   *left_desc = *right_desc;
}

//********************************************************************************************************************
// Prepare logical OR operator (called BEFORE RHS evaluation)
// CFG-based implementation using ControlFlowGraph

void OperatorEmitter::prepare_logical_or(ExprValue left)
{
   ExpDesc* left_desc = left.raw();

   // OR short-circuit logic: if left is true, skip RHS and return left (true)
   // If left is false, evaluate RHS and return RHS result

   // Discharge left operand to appropriate form
   ExpressionValue left_val(this->func_state, *left_desc);
   left_val.discharge();
   *left_desc = left_val.legacy();

   BCPOS pc;
   bool will_skip_rhs = false;

   // Handle constant folding
   if (left_desc->k IS ExpKind::Nil or left_desc->k IS ExpKind::False) {
      // Left is falsey constant - no jump needed, will evaluate RHS
      pc = NO_JMP;
   }
   else if (left_desc->k IS ExpKind::Jmp) {
      // Left is already a jump expression - use as-is
      pc = left_desc->u.s.info;
   }
   else if (left_desc->k IS ExpKind::Str or left_desc->k IS ExpKind::Num or left_desc->k IS ExpKind::True) {
      // Left is truthy constant - load to register and jump to skip RHS
      expr_toreg_nobranch(this->func_state, left_desc, NO_REG);
      pc = bcemit_jmp(this->func_state);
      will_skip_rhs = true;
   }
   else { // Runtime value - emit conditional branch (jump if true)
      pc = bcemit_branch(this->func_state, left_desc, 1);
   }

   if (should_trace_operators(this->func_state)) {
      pf::Log("Parser").msg("[%d] operator or: prepare left kind=%s, %s", this->func_state->ls->linenumber.lineNumber(),
         get_expkind_name(left_desc->k), will_skip_rhs ? "will skip RHS (constant true)" : "will evaluate RHS");
   }

   // Set up CFG edges for short-circuit behavior
   // The true_edge collects jumps for "left is true" path - these will be resolved later
   // when the complete_logical_or merges paths. We release it here since it's just
   // being used to accumulate jump positions, not to be patched at this location.
   ControlFlowEdge true_edge = this->cfg->make_true_edge(BCPos(left_desc->t));
   true_edge.append(BCPos(pc));
   left_desc->t = true_edge.head().raw();
   true_edge.release();  // Mark as handled - jumps will be resolved in complete_logical_or

   ControlFlowEdge false_edge = this->cfg->make_false_edge(BCPos(left_desc->f));
   false_edge.patch_here();
   left_desc->f = NO_JMP;
}

//********************************************************************************************************************
// Complete logical OR operator (called AFTER RHS evaluation)
// CFG-based implementation using ControlFlowGraph

void OperatorEmitter::complete_logical_or(ExprValue left, ExpDesc right)
{
   ExpDesc *left_desc = left.raw();

   // At this point:
   // - left->t contains jumps for "left is true" path
   // - right has been evaluated
   // - We need to merge the true paths and return right's result

   FuncState *fs = this->func_state;  // For lj_assertFS macro
   fs_check_assert(fs,left_desc->f IS NO_JMP, "jump list not closed");

   // Discharge right operand
   ExpressionValue right_val(this->func_state, right);
   right_val.discharge();
   right = right_val.legacy();

   // Merge true paths: both "left is true" and "right is true" go to same target
   // This edge accumulates the merged jump list; release it since jumps resolve
   // when the expression result is used (discharged to register, used in condition, etc.)
   ControlFlowEdge true_edge = this->cfg->make_true_edge(BCPos(right.t));
   true_edge.append(BCPos(left_desc->t));
   right.t = true_edge.head().raw();
   true_edge.release();  // Mark as handled - jumps will be resolved by caller

   // Result is right's value
   *left_desc = right;
}

//********************************************************************************************************************
// Prepare IF_EMPTY (??) operator (called BEFORE RHS evaluation)
// CFG-based implementation with extended falsey semantics

void OperatorEmitter::prepare_if_empty(ExprValue left)
{
   ExpDesc* left_desc = left.raw();

   // IF_EMPTY short-circuit: if left is truthy, skip RHS and return left
   // Extended falsey: nil, false, 0, "" (all trigger RHS evaluation)

   // Discharge left operand
   ExpressionValue left_val(this->func_state, *left_desc);
   left_val.discharge();
   *left_desc = left_val.legacy();

   BCPOS pc;

   // Handle constant folding for known falsey values
   if (left_desc->is_constant() and left_desc->is_falsey())
      pc = NO_JMP;  // Falsey constant - will evaluate RHS
   else if (left_desc->k IS ExpKind::Jmp)
      pc = left_desc->u.s.info;
   else if (left_desc->is_constant() and not left_desc->is_falsey()) {
      // Truthy constant - load to register and skip RHS
      RegisterAllocator local_alloc(this->func_state);
      local_alloc.reserve(BCReg(1));
      expr_toreg_nobranch(this->func_state, left_desc, this->func_state->freereg - 1);
      pc = bcemit_jmp(this->func_state);
   }
   else {
      // Runtime value - emit extended falsey checks NOW (before RHS evaluation)
      // This implements proper short-circuit semantics
      if (!left_desc->is_constant_nojump()) {
         ExpressionValue left_inner(this->func_state, *left_desc);
         RegisterAllocator local_alloc(this->func_state);
         BCReg reg = left_inner.discharge_to_any_reg(local_alloc);
         *left_desc = left_inner.legacy();

         // Create test expressions for extended falsey values
         ExpDesc nilv(ExpKind::Nil);
         ExpDesc falsev(ExpKind::False);
         ExpDesc zerov(ExpKind::Num);
         setnumV(&zerov.u.nval, 0.0);
         ExpDesc emptyv(ExpKind::Str);
         emptyv.u.sval = this->func_state->ls->intern_empty_string();

         // Extended falsey check sequence
         // ISEQ* skips the JMP when values ARE equal (falsey), executes JMP when NOT equal (truthy)
         // Strategy: When value is truthy, NO checks match → all JMPs execute → skip RHS
         //          When value is falsey, ONE check matches → that JMP skipped → fall through to RHS

         bcemit_INS(this->func_state, BCINS_AD(BC_ISEQP, reg, const_pri(&nilv)));
         BCPos check_nil = BCPos(bcemit_jmp(this->func_state));

         bcemit_INS(this->func_state, BCINS_AD(BC_ISEQP, reg, const_pri(&falsev)));
         BCPos check_false = BCPos(bcemit_jmp(this->func_state));

         bcemit_INS(this->func_state, BCINS_AD(BC_ISEQN, reg, const_num(this->func_state, &zerov)));
         BCPos check_zero = BCPos(bcemit_jmp(this->func_state));

         bcemit_INS(this->func_state, BCINS_AD(BC_ISEQS, reg, const_str(this->func_state, &emptyv)));
         BCPos check_empty = BCPos(bcemit_jmp(this->func_state));

         // Empty array check (array with len == 0)
         bcemit_INS(this->func_state, BCINS_AD(BC_ISEMPTYARR, reg, 0));
         BCPos check_empty_array = BCPos(bcemit_jmp(this->func_state));

         // RHS will be emitted after this prepare phase
         // The jumps above will skip RHS when value is truthy (all JMPs execute)
         // Fall through to RHS when value is falsey (one JMP is skipped)

         // Collect all these jumps - they should skip RHS when value is truthy
         pc = check_nil.raw();
         ControlFlowEdge skip_rhs = this->cfg->make_true_edge(check_nil);
         skip_rhs.append(check_false);
         skip_rhs.append(check_zero);
         skip_rhs.append(check_empty);
         skip_rhs.append(check_empty_array);
         pc = skip_rhs.head().raw();

         // Mark that we need to preserve LHS value and reserve register for RHS
         auto rhs_reg = BCReg(this->func_state->freereg);
         ExprFlag saved_flags = left_desc->flags;
         local_alloc.reserve(BCReg(1));
         left_desc->init(ExpKind::NonReloc, reg);
         left_desc->u.s.aux = rhs_reg;
         left_desc->flags = saved_flags | ExprFlag::HasRhsReg;
      }
      else pc = NO_JMP;
   }

   // Set up CFG edges
   ControlFlowEdge true_edge = this->cfg->make_true_edge(BCPos(left_desc->t));
   true_edge.append(BCPos(pc));
   left_desc->t = true_edge.head().raw();

   ControlFlowEdge false_edge = this->cfg->make_false_edge(BCPos(left_desc->f));
   false_edge.patch_here();
   left_desc->f = NO_JMP;
}

//********************************************************************************************************************
// Complete IF_EMPTY (??) operator (called AFTER RHS evaluation)
// Extended falsey checks are now emitted in prepare phase for proper short-circuit semantics

void OperatorEmitter::complete_if_empty(ExprValue left, ExpDesc right)
{
   ExpDesc *left_desc = left.raw();

   FuncState* fs = this->func_state;
   fs_check_assert(fs,left_desc->f IS NO_JMP, "jump list not closed");

   // If left->t has jumps, those are from the extended falsey checks in prepare phase
   // They skip RHS evaluation when LHS is truthy - we need to:
   // 1. Emit RHS materialization code (for falsey path)
   // 2. Patch the truthy jumps to skip all of that

   if (left_desc->t != NO_JMP) {
      // Get the RHS register if one was reserved
      auto rhs_reg = BCReg(NO_REG);
      auto lhs_reg = BCReg(left_desc->u.s.info);
      if (expr_consume_flag(left_desc, ExprFlag::HasRhsReg)) {
         rhs_reg = BCReg(left_desc->u.s.aux);
      }

      // RHS has been evaluated - store it in the reserved register (or allocate one)
      RegisterAllocator local_alloc(fs);
      BCReg dest_reg;
      if (rhs_reg.raw() IS NO_REG) {
         dest_reg = fs->free_reg();
         local_alloc.reserve(BCReg(1));
      }
      else {
         dest_reg = rhs_reg;
         if (dest_reg >= fs->freereg) fs->freereg = dest_reg + 1;
      }

      ExpressionValue right_val(fs, right);
      right_val.to_reg(local_alloc, BCReg(dest_reg));
      right = right_val.legacy();

      // Copy RHS result to LHS register (where the result should be)
      if (dest_reg != lhs_reg) bcemit_AD(fs, BC_MOV, lhs_reg, dest_reg);

      // NOW patch the truthy-skip jumps to jump HERE (past all RHS materialization)
      ControlFlowEdge true_edge = this->cfg->make_true_edge(BCPos(left_desc->t));
      true_edge.patch_to(fs->current_pc());
      left_desc->t = NO_JMP;

      // Result is in LHS register
      ExprFlag saved_flags = left_desc->flags;
      left_desc->init(ExpKind::NonReloc, lhs_reg);
      left_desc->flags = saved_flags;

      // Clean up scratch register
      if ((dest_reg != lhs_reg) and (fs->is_temp_register(BCReg(dest_reg))) and (fs->freereg > dest_reg)) {
         fs->freereg = dest_reg;
      }

      if ((fs->is_temp_register(BCReg(lhs_reg))) and (fs->freereg > lhs_reg + 1)) {
         fs->freereg = lhs_reg + 1;
      }
   }
   else {
      // LHS is compile-time falsey - just use RHS
      ExpressionValue right_val(fs, right);
      right_val.discharge();
      right = right_val.legacy();
      *left_desc = right;
   }
}

//********************************************************************************************************************
// CONCAT operator - preparation phase
// Discharges left operand to next consecutive register for BC_CAT chaining

void OperatorEmitter::prepare_concat(ExprValue left)
{
   ExpDesc* left_desc = left.raw();
   FuncState* fs = this->func_state;

   // CONCAT requires operands in consecutive registers for BC_CAT instruction
   // The BC_CAT instruction format is: BC_CAT dest, start_reg, end_reg
   // It concatenates all values from start_reg to end_reg

   RegisterAllocator local_alloc(fs);
   ExpressionValue left_val(fs, *left_desc);
   left_val.to_next_reg(local_alloc);
   *left_desc = left_val.legacy();
}

//********************************************************************************************************************
// CONCAT operator - completion phase
// Emits BC_CAT instruction with support for chaining multiple concatenations

void OperatorEmitter::complete_concat(ExprValue left, ExpDesc right)
{
   ExpDesc *left_desc = left.raw();

   FuncState *fs = this->func_state;
   RegisterAllocator local_alloc(fs);

   // First, convert right operand to val form
   ExpressionValue right_toval(fs, right);
   right_toval.to_val();
   right = right_toval.legacy();

   // Check if right operand is already a BC_CAT instruction (for chaining)
   // If so, extend it; otherwise create new BC_CAT
   if (right.k IS ExpKind::Relocable and bc_op(*bcptr(fs, &right)) IS BC_CAT) {
      // Chaining case: "a".."b".."c"
      // The previous BC_CAT starts at e1->u.s.info and we extend it
      fs_check_assert(fs,left_desc->u.s.info IS bc_b(*bcptr(fs, &right)) - 1, "bad CAT stack layout");
      expr_free(fs, left_desc);
      setbc_b(bcptr(fs, &right), left_desc->u.s.info);
      left_desc->u.s.info = right.u.s.info;
   }
   else {
      // New concatenation: emit BC_CAT instruction
      ExpressionValue right_val(fs, right);
      right_val.to_next_reg(local_alloc);
      right = right_val.legacy();

      expr_free(fs, &right);
      expr_free(fs, left_desc);

      // Emit BC_CAT: concatenate registers from left->u.s.info to right->u.s.info
      left_desc->u.s.info = bcemit_ABC(fs, BC_CAT, 0, left_desc->u.s.info, right.u.s.info);
   }

   left_desc->k = ExpKind::Relocable;
   left_desc->result_type = TiriType::Str;  // Concatenation always returns string
}

//********************************************************************************************************************
// Presence check operator (x?)
// Returns boolean: true if value is truthy, false if falsey (nil, false, 0, "")

void OperatorEmitter::emit_presence_check(ExprValue operand)
{
   ExpDesc* e = operand.raw();
   FuncState* fs = this->func_state;

   // Discharge the operand first
   ExpressionValue e_value(fs, *e);
   e_value.discharge();
   *e = e_value.legacy();

   // Handle compile-time constants using is_falsey()
   if (e->is_constant()) {
      if (e->is_falsey()) e->init(ExpKind::False, 0);  // Falsey constant
      else e->init(ExpKind::True, 0);  // Truthy constant
      return;
   }

   // Runtime value - emit extended falsey checks
   RegisterAllocator local_alloc(fs);
   ExpressionValue e_runtime(fs, *e);
   BCReg reg = e_runtime.discharge_to_any_reg(local_alloc);
   *e = e_runtime.legacy();

   // Create test expressions
   ExpDesc nilv(ExpKind::Nil);
   ExpDesc falsev(ExpKind::False);
   ExpDesc zerov(0.0);
   ExpDesc emptyv(fs->ls->intern_empty_string());

   // Emit equality checks for extended falsey values
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, reg, const_pri(&nilv)));
   auto check_nil = BCPos(bcemit_jmp(fs));

   bcemit_INS(fs, BCINS_AD(BC_ISEQP, reg, const_pri(&falsev)));
   auto check_false = BCPos(bcemit_jmp(fs));

   bcemit_INS(fs, BCINS_AD(BC_ISEQN, reg, const_num(fs, &zerov)));
   auto check_zero = BCPos(bcemit_jmp(fs));

   bcemit_INS(fs, BCINS_AD(BC_ISEQS, reg, const_str(fs, &emptyv)));
   auto check_empty = BCPos(bcemit_jmp(fs));

   // Empty array check (array with len == 0)
   bcemit_INS(fs, BCINS_AD(BC_ISEMPTYARR, reg, 0));
   auto check_empty_array = BCPos(bcemit_jmp(fs));

   expr_free(fs, e);  // Free the expression register

   // Reserve register for result
   BCReg dest = fs->free_reg();
   local_alloc.reserve(BCReg(1));

   // Value is truthy - load true
   bcemit_AD(fs, BC_KPRI, dest, BCREG(ExpKind::True));
   auto jmp_false_branch = BCPos(bcemit_jmp(fs));

   // False branch: patch all falsey jumps here and load false
   auto false_pos = fs->current_pc();
   ControlFlowEdge nil_edge = this->cfg->make_unconditional(check_nil);
   nil_edge.patch_to(false_pos);
   ControlFlowEdge false_edge_check = this->cfg->make_unconditional(check_false);
   false_edge_check.patch_to(false_pos);
   ControlFlowEdge zero_edge = this->cfg->make_unconditional(check_zero);
   zero_edge.patch_to(false_pos);
   ControlFlowEdge empty_edge = this->cfg->make_unconditional(check_empty);
   empty_edge.patch_to(false_pos);
   ControlFlowEdge empty_array_edge = this->cfg->make_unconditional(check_empty_array);
   empty_array_edge.patch_to(false_pos);

   bcemit_AD(fs, BC_KPRI, dest, BCREG(ExpKind::False));

   // Patch skip jump to after false load
   ControlFlowEdge skip_edge = this->cfg->make_unconditional(jmp_false_branch);
   skip_edge.patch_to(fs->current_pc());

   e->init(ExpKind::NonReloc, dest);
   e->result_type = TiriType::Bool;  // Presence check always returns boolean
}
