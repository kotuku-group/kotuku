// Validate the destinations of fresh @NoDiscard results before bytecode emission.
// Copyright © 2026 Paul Manias

#include "no_discard_analysis.h"

#include <array>
#include <format>
#include <unordered_set>

#include "ast/nodes.h"
#include "constant_evaluator.h"
#include "parser_context.h"

namespace {

enum class FreshResultUse : uint8_t {
   Storage,
   Discard,
   Blank,
   Truncated,
   Filtered,
   Argument,
   Projection,
   Conversion,
   Control,
   Return
};

using FreshResultUses = std::array<FreshResultUse, MAX_RETURN_TYPES>;

[[nodiscard]] FreshResultUses result_uses(FreshResultUse Use)
{
   FreshResultUses uses;
   uses.fill(Use);
   return uses;
}

[[nodiscard]] FreshResultUses scalar_result_use(FreshResultUse Use)
{
   auto uses = result_uses(FreshResultUse::Truncated);
   uses[0] = Use;
   return uses;
}

[[nodiscard]] bool retains_assignment_value(AssignmentOperator Operator)
{
   return Operator IS AssignmentOperator::Plain or Operator IS AssignmentOperator::IfEmpty or
      Operator IS AssignmentOperator::IfNil;
}

class NoDiscardValidator {
public:
   explicit NoDiscardValidator(ParserContext &Context)
      : context_(Context), constants_(Context.lex()), file_index_(Context.lex().current_file_index) {}

   void validate(const BlockStmt &Module) { this->block(Module); }

private:
   void report(const ExprNode &Expression, const CallExprPayload &Call, size_t Position, FreshResultUse Use)
   {
      if (Call.no_discard_reported) return;
      Call.no_discard_reported = true;
      std::string name = "?";
      if (const auto *direct = std::get_if<DirectCallTarget>(&Call.target); direct and direct->callable) {
         name = context_binding_text(*direct->callable);
      }

      std::string_view reason;
      switch (Use) {
         case FreshResultUse::Blank: reason = " A blank assignment target discards this result."; break;
         case FreshResultUse::Truncated: reason = " Result adjustment truncates this result."; break;
         case FreshResultUse::Filtered: reason = " A result filter removes this result."; break;
         case FreshResultUse::Projection:
            reason = " Immediate member or index access does not store this result."; break;
         case FreshResultUse::Conversion: reason = " Converting this result does not establish storage."; break;
         case FreshResultUse::Control: reason = " A control-flow path consumes this result without storing it."; break;
         case FreshResultUse::Return: reason = " Forward it at a protected result position of an @NoDiscard function.";
            break;
         default: break;
      }

      ParserDiagnostic diagnostic;
      diagnostic.code = ParserErrorCode::NoDiscard;
      diagnostic.file_index = this->file_index_;
      diagnostic.token = Token::from_span(Expression.span, TokenKind::Identifier);
      diagnostic.message = std::format(
         "Result {} of '{}' is marked @NoDiscard; store it in a non-blank variable or container, "
         "or pass it to a function.{}",
         Position + 1, name, reason);
      this->context_.diagnostics().report(diagnostic);
   }

   // The final expression can supply multiple results; every preceding expression supplies exactly one, even when
   // its first result is excess.  Keep positions separate so an unrelated stored result cannot satisfy a contract.
   template<typename Destination>
   void expression_list(const ExprNodeList &Values, Destination UseAt, bool ExpandLast = true)
   {
      for (size_t i = 0; i < Values.size(); ++i) {
         if (not Values[i]) continue;
         auto uses = result_uses(FreshResultUse::Truncated);
         size_t count = ExpandLast and i + 1 IS Values.size() ? MAX_RETURN_TYPES : 1;
         for (size_t position = 0; position < count; ++position) uses[position] = UseAt(i + position);
         this->expression(*Values[i], uses);
      }
   }

   void consume(const ExprNodePtr &Value, FreshResultUse Use)
   {
      if (Value) this->expression(*Value, result_uses(Use));
   }

   // Branch folding uses only side-effect-free constants and syntax that is unconditionally truthy.  In particular,
   // an optional type annotation is not proof of truthiness: nil results are permitted by @NoDiscard.
   [[nodiscard]] std::optional<bool> truth(const ExprNode &Value, bool Extended)
   {
      if (auto constant = this->constants_.evaluate(Value)) {
         return Extended ? not constant->is_extended_falsey() : constant->is_truthy();
      }
      if (Value.kind IS AstNodeKind::FunctionExpr or Value.kind IS AstNodeKind::RangeExpr) return true;
      if (Value.kind IS AstNodeKind::TableExpr) {
         if (not Extended) return true;
         const auto &fields = std::get<TableExprPayload>(Value.data).fields;
         if (fields.empty()) return false;
         // A final non-nil field cannot be overwritten by a later entry, so the completed table is non-empty.
         if (fields.back().value) {
            const auto &last = *fields.back().value;
            if (auto constant = this->constants_.evaluate(last); constant and constant->kind != LiteralKind::Nil) {
               return true;
            }
            if (last.kind IS AstNodeKind::TableExpr or last.kind IS AstNodeKind::FunctionExpr or
                last.kind IS AstNodeKind::RangeExpr) return true;
         }
      }
      return std::nullopt;
   }

   void function(const FunctionExprPayload &Function)
   {
      this->consume(Function.context.reference, FreshResultUse::Control);
      const auto *saved = this->function_;
      this->function_ = &Function;
      if (Function.body) this->block(*Function.body);
      this->function_ = saved;
   }

   void expression(const ExprNode &Expression, FreshResultUses Uses)
   {
      if (Expression.is_grouped) {
         for (size_t i = 1; i < Uses.size(); ++i) Uses[i] = FreshResultUse::Truncated;
      }

      switch (Expression.kind) {
         case AstNodeKind::CallExpr:
         case AstNodeKind::SafeCallExpr: {
            const auto &call = std::get<CallExprPayload>(Expression.data);
            StaticResultObligations obligations = call.callable ?
               this->context_.descriptors().callable(call.callable).no_discard : 0;
            if (Expression.static_results) {
               obligations |= this->context_.descriptors().results(Expression.static_results).no_discard;
            }
            for (size_t i = 0; i < Uses.size(); ++i) {
               if ((obligations & (1u << i)) and
                   Uses[i] != FreshResultUse::Storage and Uses[i] != FreshResultUse::Argument) {
                  this->report(Expression, call, i, Uses[i]);
               }
            }
            if (const auto *direct = std::get_if<DirectCallTarget>(&call.target)) {
               this->consume(direct->callable, FreshResultUse::Projection);
            }
            this->expression_list(call.arguments, [&](size_t Position) {
               // Array literals lower to calls, but their elements must still reach the literal's destination.
               // Ordinary function arguments satisfy the contract independently of the enclosing call's result.
               if (call.result_retention IS CallResultRetention::ArrayElements and Position > 0) return Uses[0];
               if (call.result_retention IS CallResultRetention::FirstArgument and Position IS 0) return Uses[0];
               return FreshResultUse::Argument;
            }, call.forwards_multret);
            break;
         }
         case AstNodeKind::ResultFilterExpr: {
            const auto &filter = std::get<ResultFilterPayload>(Expression.data);
            auto source_uses = result_uses(FreshResultUse::Filtered);
            size_t output = 0;
            for (size_t source = 0; source < source_uses.size(); ++source) {
               bool keep = source < filter.explicit_count ?
                  (filter.keep_mask & (uint64_t(1) << source)) != 0 : filter.trailing_keep;
               if (keep) source_uses[source] = Uses[output++];
            }
            if (filter.expression) this->expression(*filter.expression, source_uses);
            break;
         }
         case AstNodeKind::TableExpr: {
            const auto &table = std::get<TableExprPayload>(Expression.data);
            for (size_t i = 0; i < table.fields.size(); ++i) {
               const auto &field = table.fields[i];
               // A retained table owns both its computed keys and its values.
               if (field.key) this->expression(*field.key, scalar_result_use(Uses[0]));
               if (not field.value) continue;
               // Only a final positional field expands all results into the table.
               bool expands = field.kind IS TableFieldKind::Array and i + 1 IS table.fields.size();
               this->expression(*field.value, expands ? result_uses(Uses[0]) : scalar_result_use(Uses[0]));
            }
            break;
         }
         case AstNodeKind::PipeExpr: {
            const auto &pipe = std::get<PipeExprPayload>(Expression.data);
            this->consume(pipe.lhs, FreshResultUse::Argument);
            if (pipe.rhs_call) this->expression(*pipe.rhs_call, Uses);
            break;
         }
         case AstNodeKind::BinaryExpr: {
            const auto &binary = std::get<BinaryExprPayload>(Expression.data);
            if (binary.op IS AstBinaryOperator::LogicalAnd or binary.op IS AstBinaryOperator::LogicalOr or
                binary.op IS AstBinaryOperator::IfEmpty) {
               auto left_truth = binary.left ?
                  this->truth(*binary.left, binary.op IS AstBinaryOperator::IfEmpty) : std::nullopt;
               bool and_op = binary.op IS AstBinaryOperator::LogicalAnd;
               bool may_evaluate_right = not left_truth or *left_truth IS and_op;
               if (binary.left) this->expression(*binary.left, scalar_result_use(
                  may_evaluate_right ? FreshResultUse::Control : Uses[0]));
               if (may_evaluate_right and binary.right) {
                  this->expression(*binary.right, scalar_result_use(Uses[0]));
               }
            }
            else {
               this->consume(binary.left, FreshResultUse::Conversion);
               this->consume(binary.right, FreshResultUse::Conversion);
            }
            break;
         }
         case AstNodeKind::TernaryExpr: {
            const auto &ternary = std::get<TernaryExprPayload>(Expression.data);
            this->consume(ternary.condition, FreshResultUse::Control);
            auto condition = ternary.condition ? this->truth(*ternary.condition,
               ternary.condition_mode IS TernaryConditionMode::Extended) : std::nullopt;
            if ((not condition or *condition) and ternary.if_true) {
               this->expression(*ternary.if_true, scalar_result_use(Uses[0]));
            }
            if ((not condition or not *condition) and ternary.if_false) {
               this->expression(*ternary.if_false, scalar_result_use(Uses[0]));
            }
            break;
         }
         case AstNodeKind::ChooseExpr: {
            const auto &choose = std::get<ChooseExprPayload>(Expression.data);
            this->consume(choose.scrutinee, FreshResultUse::Control);
            for (const auto &value : choose.scrutinee_tuple) this->consume(value, FreshResultUse::Control);
            for (const auto &arm : choose.cases) {
               this->consume(arm.pattern, FreshResultUse::Control);
               for (const auto &value : arm.tuple_patterns) this->consume(value, FreshResultUse::Control);
               this->consume(arm.guard, FreshResultUse::Control);
               if (arm.result) this->expression(*arm.result, scalar_result_use(Uses[0]));
               if (arm.result_stmt) this->statement(*arm.result_stmt);
            }
            break;
         }
         case AstNodeKind::FunctionExpr:
            this->function(std::get<FunctionExprPayload>(Expression.data));
            break;
         case AstNodeKind::DeferredExpr: {
            // A lazy expression runs as a synthetic thunk, whose caller has no published forwarding contract.
            const auto *saved = this->function_;
            this->function_ = nullptr;
            this->consume(std::get<DeferredExprPayload>(Expression.data).inner, FreshResultUse::Return);
            this->function_ = saved;
            break;
         }
         case AstNodeKind::MemberExpr:
            this->consume(std::get<MemberExprPayload>(Expression.data).table, FreshResultUse::Projection);
            break;
         case AstNodeKind::SafeMemberExpr:
            this->consume(std::get<SafeMemberExprPayload>(Expression.data).table, FreshResultUse::Projection);
            break;
         case AstNodeKind::IndexExpr: {
            const auto &index = std::get<IndexExprPayload>(Expression.data);
            this->consume(index.table, FreshResultUse::Projection);
            this->consume(index.index, FreshResultUse::Conversion);
            break;
         }
         case AstNodeKind::SafeIndexExpr: {
            const auto &index = std::get<SafeIndexExprPayload>(Expression.data);
            this->consume(index.table, FreshResultUse::Projection);
            this->consume(index.index, FreshResultUse::Conversion);
            break;
         }
         case AstNodeKind::UnaryExpr:
            this->consume(std::get<UnaryExprPayload>(Expression.data).operand, FreshResultUse::Conversion);
            break;
         case AstNodeKind::UpdateExpr:
            this->consume(std::get<UpdateExprPayload>(Expression.data).target, FreshResultUse::Conversion);
            break;
         case AstNodeKind::TypeTestExpr:
            this->consume(std::get<TypeTestExprPayload>(Expression.data).value, FreshResultUse::Conversion);
            break;
         case AstNodeKind::PresenceExpr:
            this->consume(std::get<PresenceExprPayload>(Expression.data).value, FreshResultUse::Conversion);
            break;
         case AstNodeKind::ComparisonChainExpr:
            for (const auto &value : std::get<ComparisonChainExprPayload>(Expression.data).operands) {
               this->consume(value, FreshResultUse::Conversion);
            }
            break;
         case AstNodeKind::RangeExpr: {
            const auto &range = std::get<RangeExprPayload>(Expression.data);
            this->consume(range.start, FreshResultUse::Conversion);
            this->consume(range.stop, FreshResultUse::Conversion);
            this->consume(range.step, FreshResultUse::Conversion);
            break;
         }
         case AstNodeKind::RaiseExpr: {
            const auto &raise = std::get<RaisePayload>(Expression.data);
            this->consume(raise.error_code, FreshResultUse::Conversion);
            this->consume(raise.message, FreshResultUse::Conversion);
            break;
         }
         default: break; // Literals and reads of already stored bindings carry no fresh obligation.
      }
   }

   [[nodiscard]] FreshResultUse assignment_use(const ExprNode &Target)
   {
      if (Target.kind IS AstNodeKind::IdentifierExpr) {
         const auto &reference = std::get<NameRef>(Target.data);
         if (reference.assignment_resolution IS AssignmentTargetResolution::Blank) return FreshResultUse::Blank;
         if (assignment_target_is_existing_storage(reference.assignment_resolution) or
             assignment_target_creates_local(reference.assignment_resolution)) return FreshResultUse::Storage;
         return FreshResultUse::Discard;
      }
      if (Target.kind IS AstNodeKind::MemberExpr or Target.kind IS AstNodeKind::IndexExpr or
          Target.kind IS AstNodeKind::SafeMemberExpr or Target.kind IS AstNodeKind::SafeIndexExpr) {
         return FreshResultUse::Storage;
      }
      return FreshResultUse::Discard;
   }

   template<typename Declaration>
   void declaration(const Declaration &Payload)
   {
      this->expression_list(Payload.values, [&](size_t Position) {
         if (not retains_assignment_value(Payload.op)) return FreshResultUse::Conversion;
         if (Position >= Payload.names.size()) return FreshResultUse::Truncated;
         const auto &name = Payload.names[Position];
         // Field assignment blocks hold their receiver in a compiler temporary, not in client-owned storage.
         if (name.symbol and std::string_view(strdata(name.symbol), name.symbol->len) IS FIELD_BLOCK_BINDING_NAME) {
            return FreshResultUse::Projection;
         }
         return name.is_blank ? FreshResultUse::Blank : FreshResultUse::Storage;
      });
   }

   void nested_block(const std::unique_ptr<BlockStmt> &Block)
   {
      if (Block) this->block(*Block);
   }

   void statement(const StmtNode &Statement)
   {
      switch (Statement.kind) {
         case AstNodeKind::LocalDeclStmt: this->declaration(std::get<LocalDeclStmtPayload>(Statement.data)); break;
         case AstNodeKind::GlobalDeclStmt: this->declaration(std::get<GlobalDeclStmtPayload>(Statement.data)); break;
         case AstNodeKind::AssignmentStmt: {
            const auto &assignment = std::get<AssignmentStmtPayload>(Statement.data);
            for (const auto &target : assignment.targets) this->consume(target, FreshResultUse::Projection);
            this->expression_list(assignment.values, [&](size_t Position) {
               if (not retains_assignment_value(assignment.op)) return FreshResultUse::Conversion;
               if (Position >= assignment.targets.size()) return FreshResultUse::Truncated;
               return this->assignment_use(*assignment.targets[Position]);
            });
            break;
         }
         case AstNodeKind::LocalFunctionStmt: {
            const auto &payload = std::get<LocalFunctionStmtPayload>(Statement.data);
            if (payload.function) this->function(*payload.function);
            break;
         }
         case AstNodeKind::FunctionStmt: {
            const auto &payload = std::get<FunctionStmtPayload>(Statement.data);
            if (payload.function) this->function(*payload.function);
            break;
         }
         case AstNodeKind::ReturnStmt:
            this->expression_list(std::get<ReturnStmtPayload>(Statement.data).values, [&](size_t Position) {
               return this->function_ and this->function_->no_discard_at(Position) ?
                  FreshResultUse::Storage : FreshResultUse::Return;
            });
            break;
         case AstNodeKind::IfStmt:
            for (const auto &clause : std::get<IfStmtPayload>(Statement.data).clauses) {
               this->consume(clause.condition, FreshResultUse::Control);
               auto condition = clause.condition ? this->truth(*clause.condition, false) : std::optional<bool>(true);
               if (not condition or *condition) this->nested_block(clause.block);
               if (condition and *condition) break;
            }
            break;
         case AstNodeKind::WhileStmt:
         case AstNodeKind::RepeatStmt: {
            const auto &loop = std::get<LoopStmtPayload>(Statement.data);
            this->consume(loop.condition, FreshResultUse::Control);
            this->nested_block(loop.body);
            break;
         }
         case AstNodeKind::NumericForStmt: {
            const auto &loop = std::get<NumericForStmtPayload>(Statement.data);
            this->consume(loop.start, FreshResultUse::Conversion);
            this->consume(loop.stop, FreshResultUse::Conversion);
            this->consume(loop.step, FreshResultUse::Conversion);
            this->nested_block(loop.body);
            break;
         }
         case AstNodeKind::RangeForStmt: {
            const auto &loop = std::get<RangeForStmtPayload>(Statement.data);
            this->consume(loop.start, FreshResultUse::Conversion);
            this->consume(loop.stop, FreshResultUse::Conversion);
            this->consume(loop.step, FreshResultUse::Conversion);
            this->nested_block(loop.body);
            break;
         }
         case AstNodeKind::GenericForStmt: {
            const auto &loop = std::get<GenericForStmtPayload>(Statement.data);
            for (const auto &value : loop.iterators) this->consume(value, FreshResultUse::Control);
            this->nested_block(loop.body);
            break;
         }
         case AstNodeKind::DeferStmt: {
            const auto &defer = std::get<DeferStmtPayload>(Statement.data);
            for (const auto &value : defer.arguments) this->consume(value, FreshResultUse::Argument);
            if (defer.callable) this->function(*defer.callable);
            break;
         }
         case AstNodeKind::DoStmt: this->nested_block(std::get<DoStmtPayload>(Statement.data).block); break;
         case AstNodeKind::ContextStmt: {
            const auto &context = std::get<ContextStmtPayload>(Statement.data);
            this->consume(context.reference, FreshResultUse::Control);
            this->nested_block(context.block);
            break;
         }
         case AstNodeKind::ConditionalShorthandStmt: {
            const auto &conditional = std::get<ConditionalShorthandStmtPayload>(Statement.data);
            this->consume(conditional.condition, FreshResultUse::Control);
            if (conditional.body) this->statement(*conditional.body);
            break;
         }
         case AstNodeKind::TryExceptStmt: {
            const auto &attempt = std::get<TryExceptPayload>(Statement.data);
            this->nested_block(attempt.try_block);
            for (const auto &clause : attempt.except_clauses) {
               for (const auto &value : clause.filter_codes) this->consume(value, FreshResultUse::Conversion);
               this->nested_block(clause.block);
            }
            this->nested_block(attempt.success_block);
            break;
         }
         case AstNodeKind::CheckallStmt: this->nested_block(std::get<CheckallStmtPayload>(Statement.data).block); break;
         case AstNodeKind::RaiseStmt: {
            const auto &raise = std::get<RaiseStmtPayload>(Statement.data);
            this->consume(raise.error_code, FreshResultUse::Conversion);
            this->consume(raise.message, FreshResultUse::Conversion);
            break;
         }
         case AstNodeKind::CheckStmt:
            this->consume(std::get<CheckStmtPayload>(Statement.data).error_code, FreshResultUse::Conversion);
            break;
         case AstNodeKind::ImportStmt:
            for (const auto &entry : std::get<ImportStmtPayload>(Statement.data).entries) {
               const BlockStmt *body = entry.inlined_body.get();
               uint8_t file = entry.file_source_idx;
               if (entry.module_unit) {
                  if (not this->modules_.insert(entry.module_unit.get()).second) continue;
                  body = entry.module_unit->body.get();
                  file = entry.module_unit->file_source_idx;
               }
               uint8_t saved_file = this->file_index_;
               const auto *saved_function = this->function_;
               this->file_index_ = file;
               this->function_ = nullptr;
               if (body) this->block(*body);
               this->file_index_ = saved_file;
               this->function_ = saved_function;
            }
            break;
         case AstNodeKind::NamespaceStmt:
            this->consume(std::get<NamespaceStmtPayload>(Statement.data).initialiser, FreshResultUse::Storage);
            break;
         case AstNodeKind::WithStmt: {
            const auto &with = std::get<WithStmtPayload>(Statement.data);
            for (const auto &value : with.objects) this->consume(value, FreshResultUse::Control);
            this->nested_block(with.block);
            break;
         }
         case AstNodeKind::ExpressionStmt:
            this->consume(std::get<ExpressionStmtPayload>(Statement.data).expression, FreshResultUse::Discard);
            break;
         default: break;
      }
   }

   void block(const BlockStmt &Block)
   {
      for (const auto &statement : Block.statements) {
         if (statement) this->statement(*statement);
      }
   }

   ParserContext &context_;
   ConstantEvaluator constants_;
   const FunctionExprPayload *function_ = nullptr;
   uint8_t file_index_;
   std::unordered_set<const ImportedModuleUnit *> modules_;
};

} // namespace

void validate_no_discard_results(ParserContext &Context, const BlockStmt &Module)
{
   NoDiscardValidator(Context).validate(Module);
}
