// Authoritative semantic resolution for identifier assignment targets.

#include "assignment_target_resolution.h"

#include <algorithm>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "ast/nodes.h"
#include "parser_context.h"
#include "../../../defs.h"
#include "../runtime/lj_tab.h"

namespace {

struct AssignmentBinding {
   GCstr *name = nullptr;
   StaticBindingID binding_id{};
   uint16_t function_depth = 0;
};

struct AssignmentScope {
   std::vector<AssignmentBinding> bindings;
};

class AssignmentTargetResolver {
public:
   explicit AssignmentTargetResolver(ParserContext &Context) : context_(Context) {}

   void resolve(BlockStmt &Module)
   {
      this->scopes_.clear();
      this->global_names_.clear();
      this->function_depth_ = 0;
      this->next_binding_id_ = StaticBindingID(1);
      this->scopes_.push_back(AssignmentScope{});
      this->resolve_block(Module, false);
   }

private:
   [[nodiscard]] const AssignmentBinding *find_binding(GCstr *Name) const
   {
      if (not Name) return nullptr;
      for (auto scope = this->scopes_.rbegin(); scope != this->scopes_.rend(); ++scope) {
         for (auto binding = scope->bindings.rbegin(); binding != scope->bindings.rend(); ++binding) {
            if (binding->name IS Name) return &*binding;
         }
      }
      return nullptr;
   }

   [[nodiscard]] bool is_declared_global(GCstr *Name) const
   {
      return std::find(this->global_names_.begin(), this->global_names_.end(), Name) != this->global_names_.end();
   }

   [[nodiscard]] bool is_registered_constant(GCstr *Name) const
   {
      if (not Name) return false;
      std::shared_lock lock(glConstantMutex);
      return glConstantRegistry.contains(Name->hash);
   }

   [[nodiscard]] bool is_environment_global(GCstr *Name) const
   {
      if (not Name) return false;
      GCtab *environment = tabref(this->context_.lua().env);
      if (not environment) return false;
      cTValue *value = lj_tab_getstr(environment, Name);
      return (value and not tvisnil(value)) or lj_tab_get_global_contract(environment, Name) != nullptr;
   }

   [[nodiscard]] bool is_existing_global(GCstr *Name) const
   {
      if (not Name) return false;
      // The glX/mX naming convention is a read-only fallback, not an assignment external policy.  Named source
      // externs are recorded in global_names_, so a convention-shaped unknown assignment still creates a local.
      if (this->is_declared_global(Name) or this->is_registered_constant(Name) or
          (Name->flags & STRFLAG_PROTECTED_GLOBAL) != 0) return true;
      return this->is_environment_global(Name);
   }

   [[nodiscard]] StaticBindingID allocate_binding_id()
   {
      StaticBindingID result = this->next_binding_id_;
      ++this->next_binding_id_;
      return result;
   }

   AssignmentBinding prepare_declaration(Identifier &Name)
   {
      AssignmentBinding binding;
      binding.name = Name.symbol;
      binding.function_depth = this->function_depth_;
      if (Name.symbol and not Name.is_blank) {
         binding.binding_id = this->allocate_binding_id();
         Name.binding_id = binding.binding_id;
      }
      else Name.binding_id = {};
      return binding;
   }

   void publish_declaration(AssignmentBinding Binding)
   {
      if (Binding.name and Binding.binding_id) this->scopes_.back().bindings.push_back(std::move(Binding));
   }

   void declare(Identifier &Name)
   {
      this->publish_declaration(this->prepare_declaration(Name));
   }

   void resolve_reference(NameRef &Reference)
   {
      const AssignmentBinding *binding = this->find_binding(Reference.identifier.symbol);
      Reference.binding_id = binding ? binding->binding_id : StaticBindingID{};
      Reference.identifier.binding_id = Reference.binding_id;
   }

   void resolve_identifier_target(NameRef &Reference, bool AllowsCreation,
      std::vector<AssignmentBinding> *PendingDeclarations)
   {
      Reference.binding_id = {};
      Reference.identifier.binding_id = {};

      if (Reference.identifier.is_blank) {
         Reference.assignment_resolution = AssignmentTargetResolution::Blank;
         return;
      }

      if (const AssignmentBinding *binding = this->find_binding(Reference.identifier.symbol)) {
         Reference.binding_id = binding->binding_id;
         Reference.identifier.binding_id = binding->binding_id;
         Reference.assignment_resolution = binding->function_depth IS this->function_depth_ ?
            AssignmentTargetResolution::ExistingLocal : AssignmentTargetResolution::ExistingUpvalue;
         return;
      }

      if (this->is_existing_global(Reference.identifier.symbol)) {
         Reference.assignment_resolution = AssignmentTargetResolution::ExistingGlobal;
         return;
      }

      if (AllowsCreation and PendingDeclarations) {
         AssignmentBinding binding = this->prepare_declaration(Reference.identifier);
         Reference.binding_id = binding.binding_id;
         Reference.assignment_resolution = AssignmentTargetResolution::NewLocal;
         PendingDeclarations->push_back(std::move(binding));
         return;
      }

      Reference.assignment_resolution = AssignmentTargetResolution::Invalid;
   }

   void resolve_assignment_target(ExprNode &Target, bool AllowsCreation,
      std::vector<AssignmentBinding> *PendingDeclarations = nullptr)
   {
      if (Target.kind IS AstNodeKind::IdentifierExpr) {
         this->resolve_identifier_target(
            std::get<NameRef>(Target.data), AllowsCreation, PendingDeclarations);
         return;
      }

      // A member or index lvalue does not give its base identifier assignment-target semantics.  Its operands are
      // ordinary reads and retain NameResolution for the later read/callable pipeline.
      this->resolve_expression(Target);
   }

   void resolve_function(FunctionExprPayload &Function)
   {
      ++this->function_depth_;
      this->scopes_.push_back(AssignmentScope{});
      for (FunctionParameter &parameter : Function.parameters) this->declare(parameter.name);
      if (Function.body) this->resolve_block(*Function.body, false);
      this->scopes_.pop_back();
      --this->function_depth_;
   }

   void resolve_expression(ExprNode &Expression)
   {
      switch (Expression.kind) {
         case AstNodeKind::IdentifierExpr:
            this->resolve_reference(std::get<NameRef>(Expression.data));
            break;
         case AstNodeKind::UnaryExpr: {
            auto &payload = std::get<UnaryExprPayload>(Expression.data);
            if (payload.operand) this->resolve_expression(*payload.operand);
            break;
         }
         case AstNodeKind::UpdateExpr: {
            auto &payload = std::get<UpdateExprPayload>(Expression.data);
            if (payload.target) this->resolve_assignment_target(*payload.target, false);
            break;
         }
         case AstNodeKind::TypeTestExpr: {
            auto &payload = std::get<TypeTestExprPayload>(Expression.data);
            if (payload.value) this->resolve_expression(*payload.value);
            break;
         }
         case AstNodeKind::BinaryExpr: {
            auto &payload = std::get<BinaryExprPayload>(Expression.data);
            if (payload.left) this->resolve_expression(*payload.left);
            if (payload.right) this->resolve_expression(*payload.right);
            break;
         }
         case AstNodeKind::ComparisonChainExpr:
            for (auto &operand : std::get<ComparisonChainExprPayload>(Expression.data).operands) {
               if (operand) this->resolve_expression(*operand);
            }
            break;
         case AstNodeKind::TernaryExpr: {
            auto &payload = std::get<TernaryExprPayload>(Expression.data);
            if (payload.condition) this->resolve_expression(*payload.condition);
            if (payload.if_true) this->resolve_expression(*payload.if_true);
            if (payload.if_false) this->resolve_expression(*payload.if_false);
            break;
         }
         case AstNodeKind::PresenceExpr: {
            auto &payload = std::get<PresenceExprPayload>(Expression.data);
            if (payload.value) this->resolve_expression(*payload.value);
            break;
         }
         case AstNodeKind::PipeExpr: {
            auto &payload = std::get<PipeExprPayload>(Expression.data);
            if (payload.lhs) this->resolve_expression(*payload.lhs);
            if (payload.rhs_call) this->resolve_expression(*payload.rhs_call);
            break;
         }
         case AstNodeKind::CallExpr:
         case AstNodeKind::SafeCallExpr: {
            auto &payload = std::get<CallExprPayload>(Expression.data);
            if (auto *direct = std::get_if<DirectCallTarget>(&payload.target)) {
               if (direct->callable) this->resolve_expression(*direct->callable);
            }
            for (auto &argument : payload.arguments) if (argument) this->resolve_expression(*argument);
            break;
         }
         case AstNodeKind::MemberExpr: {
            auto &payload = std::get<MemberExprPayload>(Expression.data);
            if (payload.table) this->resolve_expression(*payload.table);
            break;
         }
         case AstNodeKind::IndexExpr: {
            auto &payload = std::get<IndexExprPayload>(Expression.data);
            if (payload.table) this->resolve_expression(*payload.table);
            if (payload.index) this->resolve_expression(*payload.index);
            break;
         }
         case AstNodeKind::SafeMemberExpr: {
            auto &payload = std::get<SafeMemberExprPayload>(Expression.data);
            if (payload.table) this->resolve_expression(*payload.table);
            break;
         }
         case AstNodeKind::SafeIndexExpr: {
            auto &payload = std::get<SafeIndexExprPayload>(Expression.data);
            if (payload.table) this->resolve_expression(*payload.table);
            if (payload.index) this->resolve_expression(*payload.index);
            break;
         }
         case AstNodeKind::ResultFilterExpr: {
            auto &payload = std::get<ResultFilterPayload>(Expression.data);
            if (payload.expression) this->resolve_expression(*payload.expression);
            break;
         }
         case AstNodeKind::TableExpr:
            for (auto &field : std::get<TableExprPayload>(Expression.data).fields) {
               if (field.key) this->resolve_expression(*field.key);
               if (field.value) this->resolve_expression(*field.value);
            }
            break;
         case AstNodeKind::FunctionExpr:
            this->resolve_function(std::get<FunctionExprPayload>(Expression.data));
            break;
         case AstNodeKind::DeferredExpr: {
            auto &payload = std::get<DeferredExprPayload>(Expression.data);
            if (payload.inner) this->resolve_expression(*payload.inner);
            break;
         }
         case AstNodeKind::RangeExpr: {
            auto &payload = std::get<RangeExprPayload>(Expression.data);
            if (payload.start) this->resolve_expression(*payload.start);
            if (payload.stop) this->resolve_expression(*payload.stop);
            if (payload.step) this->resolve_expression(*payload.step);
            break;
         }
         case AstNodeKind::ChooseExpr: {
            auto &payload = std::get<ChooseExprPayload>(Expression.data);
            if (payload.scrutinee) this->resolve_expression(*payload.scrutinee);
            for (auto &item : payload.scrutinee_tuple) if (item) this->resolve_expression(*item);
            for (auto &choice : payload.cases) {
               if (choice.pattern) this->resolve_expression(*choice.pattern);
               for (auto &item : choice.tuple_patterns) if (item) this->resolve_expression(*item);
               if (choice.guard) this->resolve_expression(*choice.guard);
               if (choice.result) this->resolve_expression(*choice.result);
               if (choice.result_stmt) this->resolve_statement(*choice.result_stmt);
            }
            break;
         }
         default:
            break;
      }
   }

   void resolve_assignment(AssignmentStmtPayload &Payload)
   {
      // Assignment expressions execute against the pre-assignment scope.  Resolve every RHS and lvalue operand before
      // publishing any new local so sibling targets cannot affect one another.
      for (auto &value : Payload.values) if (value) this->resolve_expression(*value);

      bool allows_creation = Payload.op IS AssignmentOperator::Plain or
         Payload.op IS AssignmentOperator::IfEmpty or Payload.op IS AssignmentOperator::IfNil;
      std::vector<AssignmentBinding> pending;
      pending.reserve(Payload.targets.size());
      for (auto &target : Payload.targets) {
         if (target) this->resolve_assignment_target(*target, allows_creation, &pending);
      }
      for (AssignmentBinding &binding : pending) this->publish_declaration(std::move(binding));
   }

   void resolve_block(BlockStmt &Block, bool NewScope = true)
   {
      if (NewScope) this->scopes_.push_back(AssignmentScope{});
      for (auto &statement : Block.statements) if (statement) this->resolve_statement(*statement);
      if (NewScope) this->scopes_.pop_back();
   }

   void resolve_statement(StmtNode &Statement)
   {
      switch (Statement.kind) {
         case AstNodeKind::LocalDeclStmt: {
            auto &payload = std::get<LocalDeclStmtPayload>(Statement.data);
            for (auto &value : payload.values) if (value) this->resolve_expression(*value);
            std::vector<AssignmentBinding> pending;
            pending.reserve(payload.names.size());
            for (Identifier &name : payload.names) pending.push_back(this->prepare_declaration(name));
            for (AssignmentBinding &binding : pending) this->publish_declaration(std::move(binding));
            break;
         }
         case AstNodeKind::AssignmentStmt:
            this->resolve_assignment(std::get<AssignmentStmtPayload>(Statement.data));
            break;
         case AstNodeKind::GlobalDeclStmt: {
            auto &payload = std::get<GlobalDeclStmtPayload>(Statement.data);
            for (auto &value : payload.values) if (value) this->resolve_expression(*value);
            for (Identifier &name : payload.names) if (name.symbol) this->global_names_.push_back(name.symbol);
            break;
         }
         case AstNodeKind::ExternStmt:
            for (Identifier &name : std::get<ExternDeclStmtPayload>(Statement.data).names) {
               if (name.symbol) this->global_names_.push_back(name.symbol);
            }
            break;
         case AstNodeKind::LocalFunctionStmt: {
            auto &payload = std::get<LocalFunctionStmtPayload>(Statement.data);
            this->declare(payload.name);
            if (payload.function) this->resolve_function(*payload.function);
            break;
         }
         case AstNodeKind::FunctionStmt: {
            auto &payload = std::get<FunctionStmtPayload>(Statement.data);
            bool local = not payload.name.is_explicit_global and payload.name.segments.size() IS 1;
            if (local) this->declare(payload.name.segments.front());
            else if (payload.name.is_explicit_global and not payload.name.segments.empty() and
                     payload.name.segments.front().symbol) {
               this->global_names_.push_back(payload.name.segments.front().symbol);
            }
            else if (not payload.name.segments.empty()) {
               const AssignmentBinding *binding = this->find_binding(payload.name.segments.front().symbol);
               payload.name.segments.front().binding_id = binding ? binding->binding_id : StaticBindingID{};
            }
            if (payload.function) this->resolve_function(*payload.function);
            break;
         }
         case AstNodeKind::IfStmt:
            for (auto &clause : std::get<IfStmtPayload>(Statement.data).clauses) {
               if (clause.condition) this->resolve_expression(*clause.condition);
               if (clause.block) this->resolve_block(*clause.block);
            }
            break;
         case AstNodeKind::WhileStmt: {
            auto &payload = std::get<LoopStmtPayload>(Statement.data);
            if (payload.condition) this->resolve_expression(*payload.condition);
            if (payload.body) this->resolve_block(*payload.body);
            break;
         }
         case AstNodeKind::RepeatStmt: {
            auto &payload = std::get<LoopStmtPayload>(Statement.data);
            this->scopes_.push_back(AssignmentScope{});
            if (payload.body) this->resolve_block(*payload.body, false);
            if (payload.condition) this->resolve_expression(*payload.condition);
            this->scopes_.pop_back();
            break;
         }
         case AstNodeKind::NumericForStmt: {
            auto &payload = std::get<NumericForStmtPayload>(Statement.data);
            if (payload.start) this->resolve_expression(*payload.start);
            if (payload.stop) this->resolve_expression(*payload.stop);
            if (payload.step) this->resolve_expression(*payload.step);
            this->scopes_.push_back(AssignmentScope{});
            this->declare(payload.control);
            if (payload.body) this->resolve_block(*payload.body, false);
            this->scopes_.pop_back();
            break;
         }
         case AstNodeKind::RangeForStmt: {
            auto &payload = std::get<RangeForStmtPayload>(Statement.data);
            if (payload.start) this->resolve_expression(*payload.start);
            if (payload.stop) this->resolve_expression(*payload.stop);
            if (payload.step) this->resolve_expression(*payload.step);
            this->scopes_.push_back(AssignmentScope{});
            this->declare(payload.control);
            if (payload.body) this->resolve_block(*payload.body, false);
            this->scopes_.pop_back();
            break;
         }
         case AstNodeKind::GenericForStmt: {
            auto &payload = std::get<GenericForStmtPayload>(Statement.data);
            for (auto &iterator : payload.iterators) if (iterator) this->resolve_expression(*iterator);
            this->scopes_.push_back(AssignmentScope{});
            for (Identifier &name : payload.names) this->declare(name);
            if (payload.body) this->resolve_block(*payload.body, false);
            this->scopes_.pop_back();
            break;
         }
         case AstNodeKind::ReturnStmt:
            for (auto &value : std::get<ReturnStmtPayload>(Statement.data).values) {
               if (value) this->resolve_expression(*value);
            }
            break;
         case AstNodeKind::DeferStmt: {
            auto &payload = std::get<DeferStmtPayload>(Statement.data);
            if (payload.callable) this->resolve_function(*payload.callable);
            for (auto &argument : payload.arguments) if (argument) this->resolve_expression(*argument);
            break;
         }
         case AstNodeKind::DoStmt: {
            auto &payload = std::get<DoStmtPayload>(Statement.data);
            if (payload.block) this->resolve_block(*payload.block);
            break;
         }
         case AstNodeKind::ContextStmt: {
            auto &payload = std::get<ContextStmtPayload>(Statement.data);
            if (payload.reference) this->resolve_expression(*payload.reference);
            if (payload.block) this->resolve_block(*payload.block);
            break;
         }
         case AstNodeKind::ConditionalShorthandStmt: {
            auto &payload = std::get<ConditionalShorthandStmtPayload>(Statement.data);
            if (payload.condition) this->resolve_expression(*payload.condition);
            if (payload.body) this->resolve_statement(*payload.body);
            break;
         }
         case AstNodeKind::TryExceptStmt: {
            auto &payload = std::get<TryExceptPayload>(Statement.data);
            if (payload.try_block) this->resolve_block(*payload.try_block);
            for (ExceptClause &clause : payload.except_clauses) {
               for (auto &code : clause.filter_codes) if (code) this->resolve_expression(*code);
               this->scopes_.push_back(AssignmentScope{});
               if (clause.exception_var) this->declare(*clause.exception_var);
               if (clause.block) this->resolve_block(*clause.block, false);
               this->scopes_.pop_back();
            }
            if (payload.success_block) this->resolve_block(*payload.success_block);
            break;
         }
         case AstNodeKind::CheckallStmt: {
            auto &payload = std::get<CheckallStmtPayload>(Statement.data);
            if (payload.block) this->resolve_block(*payload.block);
            break;
         }
         case AstNodeKind::RaiseStmt: {
            auto &payload = std::get<RaiseStmtPayload>(Statement.data);
            if (payload.error_code) this->resolve_expression(*payload.error_code);
            if (payload.message) this->resolve_expression(*payload.message);
            break;
         }
         case AstNodeKind::CheckStmt: {
            auto &payload = std::get<CheckStmtPayload>(Statement.data);
            if (payload.error_code) this->resolve_expression(*payload.error_code);
            break;
         }
         case AstNodeKind::ImportStmt:
            for (ImportEntryPayload &entry : std::get<ImportStmtPayload>(Statement.data).entries) {
               if (entry.inlined_body) this->resolve_block(*entry.inlined_body);
               if (entry.namespace_name) this->declare(*entry.namespace_name);
            }
            break;
         case AstNodeKind::WithStmt: {
            auto &payload = std::get<WithStmtPayload>(Statement.data);
            for (auto &object : payload.objects) if (object) this->resolve_expression(*object);
            if (payload.block) this->resolve_block(*payload.block);
            break;
         }
         case AstNodeKind::ExpressionStmt: {
            auto &payload = std::get<ExpressionStmtPayload>(Statement.data);
            if (payload.expression) this->resolve_expression(*payload.expression);
            break;
         }
         default:
            break;
      }
   }

   ParserContext &context_;
   std::vector<AssignmentScope> scopes_;
   std::vector<GCstr *> global_names_;
   StaticBindingID next_binding_id_{ 1 };
   uint16_t function_depth_ = 0;
};

} // namespace

void resolve_assignment_targets(ParserContext &Context, BlockStmt &Module)
{
   AssignmentTargetResolver(Context).resolve(Module);
}
