// Allocation-site ownership proof for contextual table designation.

#include "table_ownership.h"

#include <variant>
#include <vector>

#include "ast/nodes.h"
#include "parser_context.h"

namespace {

// Merge two branch classifications.  Two owned allocations remain owned, because both branches allocated a table the
// author owns.  Any other combination degrades: a merge with unknown provenance can never be designated.
[[nodiscard]] TableOwnershipProof merge_ownership(const TableOwnershipProof &Left, const TableOwnershipProof &Right)
{
   if (Left.owned() and Right.owned()) return Left;

   // A foreign branch is a definite rejection and carries the more specific reason.
   if (Left.ownership IS TableOwnership::Foreign) return Left;
   if (Right.ownership IS TableOwnership::Foreign) return Right;

   return { TableOwnership::Unknown, ForeignTableSource::MergedUnknown };
}

// Collect the assignment sources that can reach UseOffset.  A definite plain assignment replaces earlier sources;
// assignments on conditional paths merge with them.  Nested function bodies are not visited because their writes are
// handled conservatively through StaticBindingDescriptor::captured.
void collect_assignments(
   const BlockStmt &Body, StaticBindingID Binding, size_t UseOffset, bool Conditional,
   std::vector<const ExprNode *> &Sources, bool &InitialReaches, bool &Unresolved);

[[nodiscard]] bool binding_written_in_closure(
   const BlockStmt &Body, StaticBindingID Binding, bool InspectAssignments = false);

[[nodiscard]] bool statement_writes_binding_in_closure(
   const StmtNode &Statement, StaticBindingID Binding, bool InspectAssignments)
{
   auto block_writes = [&](const std::unique_ptr<BlockStmt> &Block, bool Inspect) {
      return Block and binding_written_in_closure(*Block, Binding, Inspect);
   };

   if (InspectAssignments and Statement.kind IS AstNodeKind::AssignmentStmt) {
      const auto &payload = std::get<AssignmentStmtPayload>(Statement.data);
      for (const auto &target : payload.targets) {
         if (not target or target->kind != AstNodeKind::IdentifierExpr) continue;
         if (std::get<NameRef>(target->data).binding_id IS Binding) return true;
      }
   }

   switch (Statement.kind) {
      case AstNodeKind::LocalFunctionStmt: {
         const auto &payload = std::get<LocalFunctionStmtPayload>(Statement.data);
         return payload.function and block_writes(payload.function->body, true);
      }
      case AstNodeKind::FunctionStmt: {
         const auto &payload = std::get<FunctionStmtPayload>(Statement.data);
         return payload.function and block_writes(payload.function->body, true);
      }
      case AstNodeKind::DeferStmt: {
         const auto &payload = std::get<DeferStmtPayload>(Statement.data);
         return payload.callable and block_writes(payload.callable->body, true);
      }
      case AstNodeKind::IfStmt:
         for (const auto &clause : std::get<IfStmtPayload>(Statement.data).clauses) {
            if (block_writes(clause.block, InspectAssignments)) return true;
         }
         return false;
      case AstNodeKind::WhileStmt:
      case AstNodeKind::RepeatStmt:
         return block_writes(std::get<LoopStmtPayload>(Statement.data).body, InspectAssignments);
      case AstNodeKind::NumericForStmt:
         return block_writes(std::get<NumericForStmtPayload>(Statement.data).body, InspectAssignments);
      case AstNodeKind::RangeForStmt:
         return block_writes(std::get<RangeForStmtPayload>(Statement.data).body, InspectAssignments);
      case AstNodeKind::GenericForStmt:
         return block_writes(std::get<GenericForStmtPayload>(Statement.data).body, InspectAssignments);
      case AstNodeKind::DoStmt:
         return block_writes(std::get<DoStmtPayload>(Statement.data).block, InspectAssignments);
      case AstNodeKind::ContextStmt:
         return block_writes(std::get<ContextStmtPayload>(Statement.data).block, InspectAssignments);
      case AstNodeKind::TryExceptStmt: {
         const auto &payload = std::get<TryExceptPayload>(Statement.data);
         if (block_writes(payload.try_block, InspectAssignments)) return true;
         for (const auto &clause : payload.except_clauses) {
            if (block_writes(clause.block, InspectAssignments)) return true;
         }
         return block_writes(payload.success_block, InspectAssignments);
      }
      case AstNodeKind::WithStmt:
         return block_writes(std::get<WithStmtPayload>(Statement.data).block, InspectAssignments);
      case AstNodeKind::ConditionalShorthandStmt: {
         const auto &payload = std::get<ConditionalShorthandStmtPayload>(Statement.data);
         return payload.body and statement_writes_binding_in_closure(*payload.body, Binding, InspectAssignments);
      }
      case AstNodeKind::ImportStmt:
         for (const auto &entry : std::get<ImportStmtPayload>(Statement.data).entries) {
            if (block_writes(entry.inlined_body, InspectAssignments)) return true;
         }
         return false;
      default:
         return false;
   }
}

[[nodiscard]] bool binding_written_in_closure(
   const BlockStmt &Body, StaticBindingID Binding, bool InspectAssignments)
{
   for (const auto &statement : Body.statements) {
      if (statement and statement_writes_binding_in_closure(*statement, Binding, InspectAssignments)) return true;
   }
   return false;
}

void collect_statement_assignments(
   const StmtNode &Statement, StaticBindingID Binding, size_t UseOffset, bool Conditional,
   std::vector<const ExprNode *> &Sources, bool &InitialReaches, bool &Unresolved)
{
   auto visit_block = [&](const std::unique_ptr<BlockStmt> &Block, bool IsConditional) {
      if (Block) {
         collect_assignments(
            *Block, Binding, UseOffset, Conditional or IsConditional, Sources, InitialReaches, Unresolved);
      }
   };

   switch (Statement.kind) {
      case AstNodeKind::AssignmentStmt: {
         const auto &payload = std::get<AssignmentStmtPayload>(Statement.data);
         for (size_t i = 0; i < payload.targets.size(); ++i) {
            const auto &target = payload.targets[i];
            if (not target or target->kind != AstNodeKind::IdentifierExpr) continue;
            if (std::get<NameRef>(target->data).binding_id != Binding) continue;

            // A compound assignment produces an arithmetic or concatenation result rather than a table allocation.
            if (payload.op != AssignmentOperator::Plain and payload.op != AssignmentOperator::IfEmpty and
                payload.op != AssignmentOperator::IfNil) {
               Unresolved = true;
               continue;
            }

            // A plain assignment on a path that must execute supersedes every earlier value.  Conditional assignments
            // and assignments in branches retain the incoming sources because either value may reach the use.
            if (payload.op IS AssignmentOperator::Plain and not Conditional) {
               Sources.clear();
               InitialReaches = false;
               Unresolved = false;
            }

            // A target drawing from a later result position has no single allocation expression.
            if (payload.values.empty() or i >= payload.values.size()) {
               Unresolved = true;
               continue;
            }
            if (payload.values[i]) Sources.push_back(payload.values[i].get());
            else Unresolved = true;
         }
         break;
      }

      // A loop variable is bound by the iteration protocol rather than an owned allocation.
      case AstNodeKind::GenericForStmt: {
         const auto &payload = std::get<GenericForStmtPayload>(Statement.data);
         for (const auto &name : payload.names) {
            if (name.binding_id IS Binding) Unresolved = true;
         }
         visit_block(payload.body, true);
         return;
      }

      case AstNodeKind::IfStmt:
         for (const auto &clause : std::get<IfStmtPayload>(Statement.data).clauses) visit_block(clause.block, true);
         return;
      case AstNodeKind::WhileStmt:
      case AstNodeKind::RepeatStmt:
         visit_block(std::get<LoopStmtPayload>(Statement.data).body, true);
         return;
      case AstNodeKind::NumericForStmt:
         visit_block(std::get<NumericForStmtPayload>(Statement.data).body, true);
         return;
      case AstNodeKind::RangeForStmt:
         visit_block(std::get<RangeForStmtPayload>(Statement.data).body, true);
         return;
      case AstNodeKind::DoStmt:
         visit_block(std::get<DoStmtPayload>(Statement.data).block, false);
         return;
      case AstNodeKind::ContextStmt:
         visit_block(std::get<ContextStmtPayload>(Statement.data).block, false);
         return;
      case AstNodeKind::TryExceptStmt: {
         const auto &payload = std::get<TryExceptPayload>(Statement.data);
         visit_block(payload.try_block, true);
         for (const auto &clause : payload.except_clauses) visit_block(clause.block, true);
         visit_block(payload.success_block, true);
         return;
      }
      case AstNodeKind::WithStmt:
         visit_block(std::get<WithStmtPayload>(Statement.data).block, false);
         return;
      case AstNodeKind::ConditionalShorthandStmt: {
         const auto &payload = std::get<ConditionalShorthandStmtPayload>(Statement.data);
         if (payload.body) {
            collect_statement_assignments(
               *payload.body, Binding, UseOffset, true, Sources, InitialReaches, Unresolved);
         }
         return;
      }
      case AstNodeKind::ImportStmt:
         for (const auto &entry : std::get<ImportStmtPayload>(Statement.data).entries) {
            visit_block(entry.inlined_body, false);
         }
         return;

      default:
         return;
   }
}

void collect_assignments(
   const BlockStmt &Body, StaticBindingID Binding, size_t UseOffset, bool Conditional,
   std::vector<const ExprNode *> &Sources, bool &InitialReaches, bool &Unresolved)
{
   for (const auto &statement : Body.statements) {
      if (not statement) continue;
      if (statement->span.offset >= UseOffset) break;
      collect_statement_assignments(
         *statement, Binding, UseOffset, Conditional, Sources, InitialReaches, Unresolved);
   }
}

class TableOwnershipAnalyser {
public:
   TableOwnershipAnalyser(
      const StaticDescriptorCatalogue &Catalogue, uint16_t FunctionDepth, const BlockStmt *EnclosingBody)
      : catalogue_(Catalogue), enclosing_body_(EnclosingBody), function_depth_(FunctionDepth) {}

   [[nodiscard]] TableOwnershipProof classify(const ExprNode &Expression)
   {
      // Alias chains are finite, but a malformed or self-referential binding graph must not spin.
      if (this->depth_ >= MAX_TRACE_DEPTH) return { TableOwnership::Unknown, ForeignTableSource::None };
      this->depth_++;
      TableOwnershipProof proof = this->classify_expression(Expression);
      this->depth_--;
      return proof;
   }

private:
   static constexpr unsigned MAX_TRACE_DEPTH = 32;

   [[nodiscard]] TableOwnershipProof classify_expression(const ExprNode &Expression)
   {
      switch (Expression.kind) {
         // A table constructor is the allocation site itself and is owned by construction.
         case AstNodeKind::TableExpr:
            return { TableOwnership::Owned, ForeignTableSource::None };

         case AstNodeKind::IdentifierExpr:
            return this->classify_reference(std::get<NameRef>(Expression.data), Expression.span.offset);

         // A control-flow merge is owned only when every branch is an owned allocation.
         case AstNodeKind::TernaryExpr: {
            const auto &payload = std::get<TernaryExprPayload>(Expression.data);
            if (not payload.if_true or not payload.if_false) {
               return { TableOwnership::Unknown, ForeignTableSource::None };
            }
            return merge_ownership(this->classify(*payload.if_true), this->classify(*payload.if_false));
         }

         // `a or b` and `a and b` select one operand and therefore merge in the same way.
         case AstNodeKind::BinaryExpr: {
            const auto &payload = std::get<BinaryExprPayload>(Expression.data);
            if (payload.op != AstBinaryOperator::LogicalOr and payload.op != AstBinaryOperator::LogicalAnd) {
               return { TableOwnership::Unknown, ForeignTableSource::NotATable };
            }
            if (not payload.left or not payload.right) {
               return { TableOwnership::Unknown, ForeignTableSource::None };
            }
            return merge_ownership(this->classify(*payload.left), this->classify(*payload.right));
         }

         // A call result is allocated by the callee, so the caller does not own it.
         case AstNodeKind::CallExpr:
         case AstNodeKind::SafeCallExpr:
            return { TableOwnership::Foreign, ForeignTableSource::CallResult };

         // A table reached through a field or index belongs to the containing table's author.  Nested tables are
         // independently designated, matching semantic contract item 9.
         case AstNodeKind::MemberExpr:
         case AstNodeKind::SafeMemberExpr:
         case AstNodeKind::IndexExpr:
         case AstNodeKind::SafeIndexExpr:
            return { TableOwnership::Foreign, ForeignTableSource::Global };

         // The current context is never an owned allocation of the designating function.
         case AstNodeKind::CurrentContextExpr:
            return { TableOwnership::Foreign, ForeignTableSource::Global };

         // A literal, range or function expression can never be a table allocation.
         case AstNodeKind::LiteralExpr:
         case AstNodeKind::RangeExpr:
         case AstNodeKind::FunctionExpr:
            return { TableOwnership::Foreign, ForeignTableSource::NotATable };

         default:
            return { TableOwnership::Unknown, ForeignTableSource::None };
      }
   }

   [[nodiscard]] TableOwnershipProof classify_reference(const NameRef &Reference, size_t UseOffset)
   {
      // An unbound name is a global; the table belongs to the whole program rather than this author.
      if (not Reference.binding_id) return { TableOwnership::Foreign, ForeignTableSource::Global };

      const auto &binding = this->catalogue_.binding(Reference.binding_id);

      // The caller owns a parameter's table, so designating it would permanently alter a foreign table.
      if (binding.is_parameter) return { TableOwnership::Foreign, ForeignTableSource::Parameter };

      // A binding declared in an enclosing function is an upvalue whose allocation is not visible here.
      if (binding.function_depth != this->function_depth_) {
         return { TableOwnership::Foreign, ForeignTableSource::Upvalue };
      }

      return this->classify_binding(Reference.binding_id, UseOffset);
   }

   // Resolve a binding to the allocation it holds.  A reassigned binding is owned only when the initialiser and every
   // assignment visible in this function are themselves owned allocations, which is the merge rule of Section 5.2.1.
   [[nodiscard]] TableOwnershipProof classify_binding_sources(
      const StaticBindingDescriptor &Binding, StaticBindingID ID, size_t UseOffset)
   {
      // A binding taking a value from a second or later result position has no single allocation expression.
      if (Binding.result_position > 0) return { TableOwnership::Foreign, ForeignTableSource::CallResult };

      bool unresolved = false;
      bool initial_reaches = true;
      std::vector<const ExprNode *> sources;

      // `immutable` is cleared only by a direct reassignment of the name; field and index writes leave it set, so an
      // immutable binding is fully described by its initialiser.
      if (not Binding.immutable) {
         if (not this->enclosing_body_) return { TableOwnership::Unknown, ForeignTableSource::MergedUnknown };
         // A captured mutable binding can be rewritten by a nested function.  Call order is outside this bounded
         // allocation-site proof, so authorising its current value would be unsound.
         if (Binding.captured and binding_written_in_closure(*this->enclosing_body_, ID)) {
            return { TableOwnership::Unknown, ForeignTableSource::MergedUnknown };
         }
         collect_assignments(
            *this->enclosing_body_, ID, UseOffset, false, sources, initial_reaches, unresolved);
         if (unresolved) return { TableOwnership::Unknown, ForeignTableSource::MergedUnknown };
      }

      TableOwnershipProof proof;
      bool have_source = false;

      // The initialiser participates unless the declaration merely reserved the name.
      if (initial_reaches and Binding.initialiser) {
         proof = this->classify(*Binding.initialiser);
         have_source = true;
      }

      for (const ExprNode *source : sources) {
         // An assignment that is also the recorded initialiser has already been counted.
         if (source IS Binding.initialiser) continue;
         TableOwnershipProof branch = this->classify(*source);
         proof = have_source ? merge_ownership(proof, branch) : branch;
         have_source = true;
      }

      if (not have_source) return { TableOwnership::Unknown, ForeignTableSource::None };
      return proof;
   }

   [[nodiscard]] TableOwnershipProof classify_binding(StaticBindingID ID, size_t UseOffset)
   {
      if (this->depth_ >= MAX_TRACE_DEPTH) return { TableOwnership::Unknown, ForeignTableSource::None };

      // An alias cycle would otherwise re-enter the same binding indefinitely.
      for (StaticBindingID active : this->active_) {
         if (active IS ID) return { TableOwnership::Unknown, ForeignTableSource::None };
      }

      const auto &binding = this->catalogue_.binding(ID);

      // The caller owns a parameter's table, so designating it would permanently alter a foreign table.
      if (binding.is_parameter) return { TableOwnership::Foreign, ForeignTableSource::Parameter };

      // A binding declared in an enclosing function is an upvalue whose allocation is not visible here.
      if (binding.function_depth != this->function_depth_) {
         return { TableOwnership::Foreign, ForeignTableSource::Upvalue };
      }

      this->depth_++;
      this->active_.push_back(ID);
      TableOwnershipProof proof = this->classify_binding_sources(binding, ID, UseOffset);
      this->active_.pop_back();
      this->depth_--;
      return proof;
   }

   const StaticDescriptorCatalogue &catalogue_;
   const BlockStmt *enclosing_body_ = nullptr;
   std::vector<StaticBindingID> active_;
   uint16_t function_depth_ = 0;
   unsigned depth_ = 0;
};

}  // namespace

//********************************************************************************************************************

TableOwnershipProof classify_table_ownership(
   const ParserContext &Context, const ExprNode &Target, uint16_t CurrentFunctionDepth,
   const BlockStmt *EnclosingBody)
{
   TableOwnershipAnalyser analyser(Context.descriptors(), CurrentFunctionDepth, EnclosingBody);
   return analyser.classify(Target);
}

//********************************************************************************************************************

const char * foreign_table_source_reason(ForeignTableSource Source) noexcept
{
   switch (Source) {
      case ForeignTableSource::Parameter:
         return "a parameter is owned by the caller";
      case ForeignTableSource::Global:
         return "a global table is not owned by this function";
      case ForeignTableSource::Upvalue:
         return "the allocation belongs to an enclosing function";
      case ForeignTableSource::Import:
         return "an imported value is not owned by this function";
      case ForeignTableSource::CallResult:
         return "a call result is allocated by the callee";
      case ForeignTableSource::MergedUnknown:
         return "the table has unknown provenance";
      case ForeignTableSource::NotATable:
         return "the target is not a table allocation";
      case ForeignTableSource::None:
      default:
         return "the allocation site is not visible in this function";
   }
}
