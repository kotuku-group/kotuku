// Side-effect-free evaluation of primitive constant AST expressions.
// Copyright © 2026 Paul Manias

#pragma once

#include <functional>
#include <optional>
#include <utility>

#include "ast/nodes.h"

//********************************************************************************************************************
// Shared by compile-time folding and runtime emission of the approximate equality operator; the two paths must
// always agree on this tolerance.

inline constexpr lua_Number TIRI_APPROX_TOLERANCE = 1e-5;

//********************************************************************************************************************

struct CompileTimeValue {
   LiteralKind kind = LiteralKind::Nil;
   bool bool_value = false;
   lua_Number number_value = 0.0;
   GCstr *string_value = nullptr;

   [[nodiscard]] static CompileTimeValue from_literal(const LiteralValue &Literal);
   [[nodiscard]] LiteralValue to_literal() const;
   [[nodiscard]] bool is_truthy() const;
   [[nodiscard]] bool is_extended_falsey() const;
};

using CompileTimeResolver = std::function<std::optional<CompileTimeValue>(const NameRef &)>;

//********************************************************************************************************************

class ConstantEvaluator {
public:
   explicit ConstantEvaluator(LexState &State, CompileTimeResolver Resolver = {});

   [[nodiscard]] std::optional<CompileTimeValue> evaluate(const ExprNode &Expression) const;

private:
   LexState &lex_state;
   CompileTimeResolver resolver;

   [[nodiscard]] std::optional<CompileTimeValue> evaluate_unary(const UnaryExprPayload &Payload) const;
   [[nodiscard]] std::optional<CompileTimeValue> evaluate_binary(const BinaryExprPayload &Payload) const;
   [[nodiscard]] std::optional<CompileTimeValue> evaluate_ternary(const TernaryExprPayload &Payload) const;
};
