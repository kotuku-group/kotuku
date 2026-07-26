// Side-effect-free evaluation of primitive constant AST expressions.
// Copyright © 2026 Paul Manias

#include "constant_evaluator.h"

#include "lj_vm.h"
#include "lj_str.h"

#include <string>

//********************************************************************************************************************

CompileTimeValue CompileTimeValue::from_literal(const LiteralValue &Literal)
{
   CompileTimeValue result;
   result.kind = Literal.kind;
   result.bool_value = Literal.bool_value;
   result.number_value = Literal.number_value;
   result.string_value = Literal.string_value;
   return result;
}

//********************************************************************************************************************

LiteralValue CompileTimeValue::to_literal() const
{
   switch (this->kind) {
      case LiteralKind::Nil:     return LiteralValue::nil();
      case LiteralKind::Boolean: return LiteralValue::boolean(this->bool_value);
      case LiteralKind::Number:  return LiteralValue::number(this->number_value);
      case LiteralKind::String:  return LiteralValue::string(this->string_value);
   }
   return LiteralValue::nil();
}

//********************************************************************************************************************

bool CompileTimeValue::is_truthy() const
{
   return this->kind != LiteralKind::Nil and
      not (this->kind IS LiteralKind::Boolean and not this->bool_value);
}

//********************************************************************************************************************

bool CompileTimeValue::is_extended_falsey() const
{
   switch (this->kind) {
      case LiteralKind::Nil: return true;
      case LiteralKind::Boolean: return not this->bool_value;
      case LiteralKind::Number: return this->number_value IS 0.0;
      case LiteralKind::String: return not this->string_value or this->string_value->len IS 0;
   }
   return false;
}

//********************************************************************************************************************

ConstantEvaluator::ConstantEvaluator(LexState &State, CompileTimeResolver Resolver)
   : lex_state(State), resolver(std::move(Resolver))
{
}

//********************************************************************************************************************

static std::optional<CompileTimeValue> checked_number(lua_Number Number)
{
   TValue value;
   setnumV(&value, Number);
   if (tvisnan(&value) or tvismzero(&value)) return std::nullopt;
   return CompileTimeValue::from_literal(LiteralValue::number(Number));
}

//********************************************************************************************************************

static std::optional<CompileTimeValue> evaluate_arithmetic(
   AstBinaryOperator Operator, const CompileTimeValue &Left, const CompileTimeValue &Right)
{
   if (Left.kind != LiteralKind::Number or Right.kind != LiteralKind::Number) return std::nullopt;

   int offset;
   switch (Operator) {
      case AstBinaryOperator::Add:      offset = 0; break;
      case AstBinaryOperator::Subtract: offset = 1; break;
      case AstBinaryOperator::Multiply: offset = 2; break;
      case AstBinaryOperator::Divide:   offset = 3; break;
      case AstBinaryOperator::Modulo:   offset = 4; break;
      case AstBinaryOperator::Power:    offset = 5; break;
      default: return std::nullopt;
   }

   return checked_number(lj_vm_foldarith(Left.number_value, Right.number_value, offset));
}

//********************************************************************************************************************

static std::optional<CompileTimeValue> evaluate_bitwise(
   AstBinaryOperator Operator, const CompileTimeValue &Left, const CompileTimeValue &Right)
{
   if (Left.kind != LiteralKind::Number or Right.kind != LiteralKind::Number) return std::nullopt;

   int32_t lhs = lj_num2bit(Left.number_value);
   int32_t rhs = lj_num2bit(Right.number_value);
   int32_t result;

   switch (Operator) {
      case AstBinaryOperator::BitAnd: result = lhs & rhs; break;
      case AstBinaryOperator::BitOr: result = lhs | rhs; break;
      case AstBinaryOperator::BitXor: result = lhs ^ rhs; break;
      case AstBinaryOperator::ShiftLeft:
         result = int32_t(uint32_t(lhs) << (uint32_t(rhs) & 31));
         break;
      case AstBinaryOperator::ShiftRight:
         result = int32_t(uint32_t(lhs) >> (uint32_t(rhs) & 31));
         break;
      default: return std::nullopt;
   }

   return CompileTimeValue::from_literal(LiteralValue::number(lua_Number(result)));
}

//********************************************************************************************************************

static bool primitive_equal(const CompileTimeValue &Left, const CompileTimeValue &Right)
{
   if (Left.kind != Right.kind) return false;

   switch (Left.kind) {
      case LiteralKind::Nil: return true;
      case LiteralKind::Boolean: return Left.bool_value IS Right.bool_value;
      case LiteralKind::Number: return Left.number_value IS Right.number_value;
      case LiteralKind::String: return Left.string_value IS Right.string_value;
   }
   return false;
}

//********************************************************************************************************************

static std::optional<bool> primitive_order(
   AstBinaryOperator Operator, const CompileTimeValue &Left, const CompileTimeValue &Right)
{
   if (Left.kind IS LiteralKind::Number and Right.kind IS LiteralKind::Number) {
      switch (Operator) {
         case AstBinaryOperator::LessThan: return Left.number_value < Right.number_value;
         case AstBinaryOperator::LessEqual: return Left.number_value <= Right.number_value;
         case AstBinaryOperator::GreaterThan: return Left.number_value > Right.number_value;
         case AstBinaryOperator::GreaterEqual: return Left.number_value >= Right.number_value;
         default: return std::nullopt;
      }
   }
   else if (Left.kind IS LiteralKind::String and Right.kind IS LiteralKind::String) {
      int comparison = lj_str_cmp(Left.string_value, Right.string_value);
      switch (Operator) {
         case AstBinaryOperator::LessThan: return comparison < 0;
         case AstBinaryOperator::LessEqual: return comparison <= 0;
         case AstBinaryOperator::GreaterThan: return comparison > 0;
         case AstBinaryOperator::GreaterEqual: return comparison >= 0;
         default: return std::nullopt;
      }
   }
   return std::nullopt;
}

//********************************************************************************************************************

std::optional<CompileTimeValue> ConstantEvaluator::evaluate(const ExprNode &Expression) const
{
   switch (Expression.kind) {
      case AstNodeKind::LiteralExpr:
         return CompileTimeValue::from_literal(std::get<LiteralValue>(Expression.data));
      case AstNodeKind::IdentifierExpr:
         if (this->resolver) return this->resolver(std::get<NameRef>(Expression.data));
         return std::nullopt;
      case AstNodeKind::UnaryExpr:
         return this->evaluate_unary(std::get<UnaryExprPayload>(Expression.data));
      case AstNodeKind::BinaryExpr:
         return this->evaluate_binary(std::get<BinaryExprPayload>(Expression.data));
      case AstNodeKind::TernaryExpr:
         return this->evaluate_ternary(std::get<TernaryExprPayload>(Expression.data));
      default:
         return std::nullopt;
   }
}

//********************************************************************************************************************

std::optional<CompileTimeValue> ConstantEvaluator::evaluate_unary(const UnaryExprPayload &Payload) const
{
   if (not Payload.operand) return std::nullopt;
   auto operand = this->evaluate(*Payload.operand);
   if (not operand) return std::nullopt;

   switch (Payload.op) {
      case AstUnaryOperator::Negate:
         if (operand->kind != LiteralKind::Number) return std::nullopt;
         return checked_number(-operand->number_value);
      case AstUnaryOperator::Not:
         return CompileTimeValue::from_literal(LiteralValue::boolean(not operand->is_truthy()));
      case AstUnaryOperator::BitNot:
         if (operand->kind != LiteralKind::Number) return std::nullopt;
         return CompileTimeValue::from_literal(
            LiteralValue::number(lua_Number(int32_t(~uint32_t(lj_num2bit(operand->number_value))))));
      case AstUnaryOperator::Length:
         return std::nullopt;
   }
   return std::nullopt;
}

//********************************************************************************************************************

std::optional<CompileTimeValue> ConstantEvaluator::evaluate_binary(const BinaryExprPayload &Payload) const
{
   if (not Payload.left or not Payload.right) return std::nullopt;

   auto lhs = this->evaluate(*Payload.left);
   if (not lhs) return std::nullopt;

   if (Payload.op IS AstBinaryOperator::LogicalAnd) {
      auto rhs = this->evaluate(*Payload.right);
      if (not rhs) return std::nullopt;
      return lhs->is_truthy() ? rhs : lhs;
   }
   if (Payload.op IS AstBinaryOperator::LogicalOr) {
      auto rhs = this->evaluate(*Payload.right);
      if (not rhs) return std::nullopt;
      return lhs->is_truthy() ? lhs : rhs;
   }
   if (Payload.op IS AstBinaryOperator::IfEmpty) {
      auto rhs = this->evaluate(*Payload.right);
      if (not rhs) return std::nullopt;
      return lhs->is_extended_falsey() ? rhs : lhs;
   }

   auto rhs = this->evaluate(*Payload.right);
   if (not rhs) return std::nullopt;

   if (auto arithmetic = evaluate_arithmetic(Payload.op, *lhs, *rhs)) return arithmetic;
   if (auto bitwise = evaluate_bitwise(Payload.op, *lhs, *rhs)) return bitwise;

   if (Payload.op IS AstBinaryOperator::Concat) {
      if (lhs->kind != LiteralKind::String or rhs->kind != LiteralKind::String) return std::nullopt;
      std::string text;
      text.reserve(lhs->string_value->len + rhs->string_value->len);
      text.append(strdata(lhs->string_value), lhs->string_value->len);
      text.append(strdata(rhs->string_value), rhs->string_value->len);
      return CompileTimeValue::from_literal(LiteralValue::string(this->lex_state.keepstr(text)));
   }

   if (Payload.op IS AstBinaryOperator::Equal or Payload.op IS AstBinaryOperator::NotEqual) {
      bool result = primitive_equal(*lhs, *rhs);
      if (Payload.op IS AstBinaryOperator::NotEqual) result = not result;
      return CompileTimeValue::from_literal(LiteralValue::boolean(result));
   }

   if (auto ordered = primitive_order(Payload.op, *lhs, *rhs)) {
      return CompileTimeValue::from_literal(LiteralValue::boolean(*ordered));
   }

   if (Payload.op IS AstBinaryOperator::Approx) {
      if (lhs->kind != LiteralKind::Number or rhs->kind != LiteralKind::Number) return std::nullopt;
      lua_Number delta = lhs->number_value - rhs->number_value;
      bool result = delta <= TIRI_APPROX_TOLERANCE and delta >= -TIRI_APPROX_TOLERANCE;
      return CompileTimeValue::from_literal(LiteralValue::boolean(result));
   }

   if (Payload.op IS AstBinaryOperator::HasFlag) {
      if (lhs->kind != LiteralKind::Number or rhs->kind != LiteralKind::Number) return std::nullopt;
      bool result = (lj_num2bit(lhs->number_value) & lj_num2bit(rhs->number_value)) != 0;
      return CompileTimeValue::from_literal(LiteralValue::boolean(result));
   }

   return std::nullopt;
}

//********************************************************************************************************************

std::optional<CompileTimeValue> ConstantEvaluator::evaluate_ternary(const TernaryExprPayload &Payload) const
{
   if (not Payload.condition or not Payload.if_true or not Payload.if_false) return std::nullopt;

   auto condition = this->evaluate(*Payload.condition);
   if (not condition) return std::nullopt;

   auto if_true = this->evaluate(*Payload.if_true);
   auto if_false = this->evaluate(*Payload.if_false);
   if (not if_true or not if_false) return std::nullopt;

   bool take_true = Payload.condition_mode IS TernaryConditionMode::Extended ?
      not condition->is_extended_falsey() : condition->is_truthy();
   return take_true ? if_true : if_false;
}
