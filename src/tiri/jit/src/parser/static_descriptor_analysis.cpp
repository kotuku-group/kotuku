// Stable-binding discovery and parse-time descriptor propagation.

#include "static_descriptor_analysis.h"

#include <algorithm>
#include <string>
#include <utility>

#include "ast/nodes.h"
#include "field_type_lookup.h"
#include "parser_context.h"
#include "table_ownership.h"
#include "../../../defs.h"
#include "../runtime/lj_proto_registry.h"
#include "../runtime/lj_tab.h"

namespace {

struct BindingScope {
   std::vector<std::pair<GCstr *, StaticBindingID>> entries;
};

static bool is_builtin_interface_namespace(uint32_t Hash)
{
   switch (Hash) {
      case kt::strhash("obj"):
      case kt::strhash("string"):
      case kt::strhash("math"):
      case kt::strhash("table"):
      case kt::strhash("bit"):
      case kt::strhash("jit"):
      case kt::strhash("debug"):
      case kt::strhash("array"):
      case kt::strhash("range"):
      case kt::strhash("struct"):
         return true;
      default:
         return false;
   }
}

class StaticDescriptorAnalyser {
public:
   explicit StaticDescriptorAnalyser(ParserContext &Context, bool NativeCallsOnly = false)
      : context_(Context), catalogue_(Context.descriptors()), native_calls_only_(NativeCallsOnly) {}

   void discover(BlockStmt &Module)
   {
      this->scopes_.clear();
      this->global_names_.clear();
      this->scopes_.push_back(BindingScope{});
      this->discover_block(Module, false);
      this->resolve_all_callables();
      this->annotate_callables_block(Module);
   }

   void propagate(BlockStmt &Module)
   {
      if (not this->native_calls_only_) this->refresh_callables();
      // The module block is the depth-zero designation scope, so a designation written at file level can trace its
      // target's allocation exactly as one inside a function can.
      this->enclosing_body_ = &Module;
      this->propagate_block(Module);
      this->enclosing_body_ = nullptr;
   }

private:
   [[nodiscard]] StaticBindingID resolve(GCstr *Name) const
   {
      for (auto scope = this->scopes_.rbegin(); scope != this->scopes_.rend(); ++scope) {
         for (auto entry = scope->entries.rbegin(); entry != scope->entries.rend(); ++entry) {
            if (entry->first IS Name) return entry->second;
         }
      }
      return {};
   }

   [[nodiscard]] bool is_declared_global(GCstr *Name) const
   {
      return std::find(this->global_names_.begin(), this->global_names_.end(), Name) != this->global_names_.end();
   }

   StaticBindingID declare(Identifier &Name, const ExprNode *Initialiser, uint8_t ResultPosition,
      const FunctionExprPayload *Function = nullptr, bool IsParameter = false)
   {
      StaticBindingDescriptor binding;
      binding.name = Name.symbol;
      binding.initialiser = Initialiser;
      binding.function = Function;
      binding.result_position = ResultPosition;
      binding.function_depth = this->function_depth_;
      binding.is_const = Name.has_const;
      binding.is_parameter = IsParameter;
      binding.is_variant = Name.type IS TiriType::Any;
      StaticBindingID id = this->catalogue_.add_binding(binding);
      Name.binding_id = id;
      if (Name.symbol and not Name.is_blank) this->scopes_.back().entries.emplace_back(Name.symbol, id);
      return id;
   }

   void reference(NameRef &Reference, bool Write = false)
   {
      StaticBindingID id = this->resolve(Reference.identifier.symbol);
      Reference.binding_id = id;
      Reference.identifier.binding_id = id;
      if (not id) return;

      auto &binding = this->catalogue_.binding(id);
      if (binding.function_depth < this->function_depth_) binding.captured = true;
      if (Write) binding.immutable = false;
   }

   void discover_function(FunctionExprPayload &Function)
   {
      this->function_depth_++;
      this->scopes_.push_back(BindingScope{});
      for (auto &parameter : Function.parameters) {
         this->declare(parameter.name, nullptr, 0, nullptr, true);
      }
      if (Function.body) this->discover_block(*Function.body, false);
      this->scopes_.pop_back();
      this->function_depth_--;
   }

   void discover_expression(ExprNode &Expression, bool Write = false)
   {
      switch (Expression.kind) {
         case AstNodeKind::IdentifierExpr:
            this->reference(std::get<NameRef>(Expression.data), Write);
            break;
         case AstNodeKind::UnaryExpr: {
            auto &payload = std::get<UnaryExprPayload>(Expression.data);
            if (payload.operand) this->discover_expression(*payload.operand);
            break;
         }
         case AstNodeKind::UpdateExpr: {
            auto &payload = std::get<UpdateExprPayload>(Expression.data);
            if (payload.target) this->discover_expression(*payload.target, true);
            break;
         }
         case AstNodeKind::TypeTestExpr: {
            auto &payload = std::get<TypeTestExprPayload>(Expression.data);
            if (payload.value) this->discover_expression(*payload.value);
            break;
         }
         case AstNodeKind::BinaryExpr: {
            auto &payload = std::get<BinaryExprPayload>(Expression.data);
            if (payload.left) this->discover_expression(*payload.left);
            if (payload.right) this->discover_expression(*payload.right);
            break;
         }
         case AstNodeKind::ComparisonChainExpr: {
            for (auto &operand : std::get<ComparisonChainExprPayload>(Expression.data).operands) {
               if (operand) this->discover_expression(*operand);
            }
            break;
         }
         case AstNodeKind::TernaryExpr: {
            auto &payload = std::get<TernaryExprPayload>(Expression.data);
            if (payload.condition) this->discover_expression(*payload.condition);
            if (payload.if_true) this->discover_expression(*payload.if_true);
            if (payload.if_false) this->discover_expression(*payload.if_false);
            break;
         }
         case AstNodeKind::PresenceExpr: {
            auto &payload = std::get<PresenceExprPayload>(Expression.data);
            if (payload.value) this->discover_expression(*payload.value);
            break;
         }
         case AstNodeKind::PipeExpr: {
            auto &payload = std::get<PipeExprPayload>(Expression.data);
            if (payload.lhs) this->discover_expression(*payload.lhs);
            if (payload.rhs_call) this->discover_expression(*payload.rhs_call);
            break;
         }
         case AstNodeKind::CallExpr:
         case AstNodeKind::SafeCallExpr: {
            auto &payload = std::get<CallExprPayload>(Expression.data);
            if (auto *direct = std::get_if<DirectCallTarget>(&payload.target)) {
               if (direct->callable) this->discover_expression(*direct->callable);
            }
            for (auto &argument : payload.arguments) {
               if (argument) this->discover_expression(*argument);
            }
            break;
         }
         case AstNodeKind::MemberExpr: {
            auto &payload = std::get<MemberExprPayload>(Expression.data);
            if (payload.table) this->discover_expression(*payload.table);
            break;
         }
         case AstNodeKind::IndexExpr: {
            auto &payload = std::get<IndexExprPayload>(Expression.data);
            if (payload.table) this->discover_expression(*payload.table);
            if (payload.index) this->discover_expression(*payload.index);
            break;
         }
         case AstNodeKind::SafeMemberExpr: {
            auto &payload = std::get<SafeMemberExprPayload>(Expression.data);
            if (payload.table) this->discover_expression(*payload.table);
            break;
         }
         case AstNodeKind::SafeIndexExpr: {
            auto &payload = std::get<SafeIndexExprPayload>(Expression.data);
            if (payload.table) this->discover_expression(*payload.table);
            if (payload.index) this->discover_expression(*payload.index);
            break;
         }
         case AstNodeKind::ResultFilterExpr: {
            auto &payload = std::get<ResultFilterPayload>(Expression.data);
            if (payload.expression) this->discover_expression(*payload.expression);
            break;
         }
         case AstNodeKind::TableExpr: {
            for (auto &field : std::get<TableExprPayload>(Expression.data).fields) {
               if (field.key) this->discover_expression(*field.key);
               if (field.value) this->discover_expression(*field.value);
            }
            break;
         }
         case AstNodeKind::FunctionExpr:
            this->discover_function(std::get<FunctionExprPayload>(Expression.data));
            break;
         case AstNodeKind::DeferredExpr: {
            auto &payload = std::get<DeferredExprPayload>(Expression.data);
            if (payload.inner) this->discover_expression(*payload.inner);
            break;
         }
         case AstNodeKind::RangeExpr: {
            auto &payload = std::get<RangeExprPayload>(Expression.data);
            if (payload.start) this->discover_expression(*payload.start);
            if (payload.stop) this->discover_expression(*payload.stop);
            if (payload.step) this->discover_expression(*payload.step);
            break;
         }
         case AstNodeKind::ChooseExpr: {
            auto &payload = std::get<ChooseExprPayload>(Expression.data);
            if (payload.scrutinee) this->discover_expression(*payload.scrutinee);
            for (auto &item : payload.scrutinee_tuple) if (item) this->discover_expression(*item);
            for (auto &choice : payload.cases) {
               if (choice.pattern) this->discover_expression(*choice.pattern);
               for (auto &item : choice.tuple_patterns) if (item) this->discover_expression(*item);
               if (choice.guard) this->discover_expression(*choice.guard);
               if (choice.result) this->discover_expression(*choice.result);
               if (choice.result_stmt) this->discover_statement(*choice.result_stmt);
            }
            break;
         }
         default:
            break;
      }
   }

   void discover_block(BlockStmt &Block, bool NewScope = true)
   {
      if (NewScope) this->scopes_.push_back(BindingScope{});
      for (auto &statement : Block.statements) if (statement) this->discover_statement(*statement);
      if (NewScope) this->scopes_.pop_back();
   }

   void discover_statement(StmtNode &Statement)
   {
      switch (Statement.kind) {
         case AstNodeKind::LocalDeclStmt: {
            auto &payload = std::get<LocalDeclStmtPayload>(Statement.data);
            for (auto &value : payload.values) if (value) this->discover_expression(*value);
            for (size_t i = 0; i < payload.names.size(); ++i) {
               const ExprNode *initialiser = i < payload.values.size() ? payload.values[i].get() :
                  (payload.values.empty() ? nullptr : payload.values.back().get());
               // A declaration with no values reserves the name rather than drawing from a result position, so it
               // keeps position zero.  Treating it as a later result would misreport the binding as a call result.
               uint8_t result_position = (i < payload.values.size() or payload.values.empty()) ? 0 :
                  uint8_t(i - payload.values.size() + 1);
               this->declare(payload.names[i], initialiser, result_position);
            }
            break;
         }
         case AstNodeKind::AssignmentStmt: {
            auto &payload = std::get<AssignmentStmtPayload>(Statement.data);
            for (auto &value : payload.values) if (value) this->discover_expression(*value);
            for (size_t i = 0; i < payload.targets.size(); ++i) {
               auto &target = payload.targets[i];
               if (not target) continue;

               if (target->kind IS AstNodeKind::IdentifierExpr) {
                  auto &reference = std::get<NameRef>(target->data);
                  GCstr *name = reference.identifier.symbol;
                  bool protected_global = name and ((name->flags & STRFLAG_PROTECTED_GLOBAL) != 0);
                  bool creates_local = payload.op IS AssignmentOperator::Plain or
                     payload.op IS AssignmentOperator::IfEmpty or payload.op IS AssignmentOperator::IfNil;
                  if (creates_local and
                      name and not reference.identifier.is_blank and not protected_global and
                      not this->is_declared_global(name) and not this->resolve(name)) {
                     const ExprNode *initialiser = i < payload.values.size() ? payload.values[i].get() :
                        (payload.values.empty() ? nullptr : payload.values.back().get());
                     uint8_t position = i < payload.values.size() ? 0 :
                        uint8_t(i - payload.values.size() + 1);
                     reference.binding_id = this->declare(
                        reference.identifier, initialiser, position);
                     continue;
                  }
               }

               this->discover_expression(*target, true);
            }
            break;
         }
         case AstNodeKind::GlobalDeclStmt: {
            auto &payload = std::get<GlobalDeclStmtPayload>(Statement.data);
            for (auto &value : payload.values) if (value) this->discover_expression(*value);
            for (auto &name : payload.names) {
               if (name.symbol) this->global_names_.push_back(name.symbol);
            }
            break;
         }
         case AstNodeKind::ExternStmt: {
            auto &payload = std::get<ExternDeclStmtPayload>(Statement.data);
            for (auto &name : payload.names) {
               if (name.symbol) this->global_names_.push_back(name.symbol);
            }
            break;
         }
         case AstNodeKind::LocalFunctionStmt: {
            auto &payload = std::get<LocalFunctionStmtPayload>(Statement.data);
            this->declare(payload.name, nullptr, 0, payload.function.get());
            if (payload.function) this->discover_function(*payload.function);
            break;
         }
         case AstNodeKind::FunctionStmt: {
            auto &payload = std::get<FunctionStmtPayload>(Statement.data);
            bool local = not payload.name.is_explicit_global and payload.name.segments.size() IS 1;
            if (local) this->declare(payload.name.segments.front(), nullptr, 0, payload.function.get());
            else if (payload.name.is_explicit_global and not payload.name.segments.empty() and
                     payload.name.segments.front().symbol) {
               this->global_names_.push_back(payload.name.segments.front().symbol);
            }
            if (payload.function) this->discover_function(*payload.function);
            break;
         }
         case AstNodeKind::IfStmt: {
            for (auto &clause : std::get<IfStmtPayload>(Statement.data).clauses) {
               if (clause.condition) {
                  this->discover_expression(*clause.condition);
                  auto truth = this->constant_truth(*clause.condition);
                  if (truth and not *truth) continue;
                  if (clause.block) this->discover_block(*clause.block);
                  if (truth and *truth) break;
               }
               else {
                  if (clause.block) this->discover_block(*clause.block);
                  break;
               }
            }
            break;
         }
         case AstNodeKind::WhileStmt:
         case AstNodeKind::RepeatStmt: {
            auto &payload = std::get<LoopStmtPayload>(Statement.data);
            if (payload.condition) this->discover_expression(*payload.condition);
            if (payload.body) this->discover_block(*payload.body);
            break;
         }
         case AstNodeKind::NumericForStmt: {
            auto &payload = std::get<NumericForStmtPayload>(Statement.data);
            if (payload.start) this->discover_expression(*payload.start);
            if (payload.stop) this->discover_expression(*payload.stop);
            if (payload.step) this->discover_expression(*payload.step);
            this->scopes_.push_back(BindingScope{});
            this->declare(payload.control, nullptr, 0);
            if (payload.body) this->discover_block(*payload.body, false);
            this->scopes_.pop_back();
            break;
         }
         case AstNodeKind::RangeForStmt: {
            auto &payload = std::get<RangeForStmtPayload>(Statement.data);
            if (payload.start) this->discover_expression(*payload.start);
            if (payload.stop) this->discover_expression(*payload.stop);
            if (payload.step) this->discover_expression(*payload.step);
            this->scopes_.push_back(BindingScope{});
            this->declare(payload.control, nullptr, 0);
            if (payload.body) this->discover_block(*payload.body, false);
            this->scopes_.pop_back();
            break;
         }
         case AstNodeKind::GenericForStmt: {
            auto &payload = std::get<GenericForStmtPayload>(Statement.data);
            for (auto &iterator : payload.iterators) if (iterator) this->discover_expression(*iterator);
            this->scopes_.push_back(BindingScope{});
            for (auto &name : payload.names) this->declare(name, nullptr, 0);
            if (payload.body) this->discover_block(*payload.body, false);
            this->scopes_.pop_back();
            break;
         }
         case AstNodeKind::ReturnStmt: {
            for (auto &value : std::get<ReturnStmtPayload>(Statement.data).values) {
               if (value) this->discover_expression(*value);
            }
            break;
         }
         case AstNodeKind::DeferStmt: {
            auto &payload = std::get<DeferStmtPayload>(Statement.data);
            if (payload.callable) this->discover_function(*payload.callable);
            for (auto &argument : payload.arguments) if (argument) this->discover_expression(*argument);
            break;
         }
         case AstNodeKind::DoStmt: {
            auto &payload = std::get<DoStmtPayload>(Statement.data);
            if (payload.block) this->discover_block(*payload.block);
            break;
         }
         case AstNodeKind::ContextStmt: {
            auto &payload = std::get<ContextStmtPayload>(Statement.data);
            if (payload.reference) this->discover_expression(*payload.reference);
            if (payload.block) this->discover_block(*payload.block);
            break;
         }
         case AstNodeKind::ConditionalShorthandStmt: {
            auto &payload = std::get<ConditionalShorthandStmtPayload>(Statement.data);
            if (payload.condition) this->discover_expression(*payload.condition);
            if (payload.body) this->discover_statement(*payload.body);
            break;
         }
         case AstNodeKind::TryExceptStmt: {
            auto &payload = std::get<TryExceptPayload>(Statement.data);
            if (payload.try_block) this->discover_block(*payload.try_block);
            for (auto &clause : payload.except_clauses) {
               for (auto &code : clause.filter_codes) if (code) this->discover_expression(*code);
               this->scopes_.push_back(BindingScope{});
               if (clause.exception_var) this->declare(*clause.exception_var, nullptr, 0);
               if (clause.block) this->discover_block(*clause.block, false);
               this->scopes_.pop_back();
            }
            if (payload.success_block) this->discover_block(*payload.success_block);
            break;
         }
         case AstNodeKind::CheckallStmt: {
            auto &payload = std::get<CheckallStmtPayload>(Statement.data);
            if (payload.block) this->discover_block(*payload.block);
            break;
         }
         case AstNodeKind::RaiseStmt: {
            auto &payload = std::get<RaiseStmtPayload>(Statement.data);
            if (payload.error_code) this->discover_expression(*payload.error_code);
            if (payload.message) this->discover_expression(*payload.message);
            break;
         }
         case AstNodeKind::CheckStmt: {
            auto &payload = std::get<CheckStmtPayload>(Statement.data);
            if (payload.error_code) this->discover_expression(*payload.error_code);
            break;
         }
         case AstNodeKind::ImportStmt: {
            for (auto &entry : std::get<ImportStmtPayload>(Statement.data).entries) {
               if (entry.inlined_body) this->discover_block(*entry.inlined_body);
            }
            break;
         }
         case AstNodeKind::WithStmt: {
            auto &payload = std::get<WithStmtPayload>(Statement.data);
            for (auto &object : payload.objects) if (object) this->discover_expression(*object);
            if (payload.block) this->discover_block(*payload.block);
            break;
         }
         case AstNodeKind::ExpressionStmt: {
            auto &payload = std::get<ExpressionStmtPayload>(Statement.data);
            if (payload.expression) this->discover_expression(*payload.expression);
            break;
         }
         default:
            break;
      }
   }

   [[nodiscard]] StaticResultSet function_results(const FunctionExprPayload &Function) const
   {
      StaticResultSet results;
      results.declared_count = Function.return_types.count;
      results.stored_count = uint8_t(std::min<size_t>(Function.return_types.count, MAX_RETURN_TYPES));
      results.variadic = Function.return_types.is_variadic;
      results.dynamic = not Function.return_types.is_explicit and not Function.return_types.is_inferred;
      for (size_t i = 0; i < results.stored_count; ++i) {
         StaticValueDescriptor value = this->catalogue_.value(Function.return_types.descriptors[i]);
         value.primary = Function.return_types.types[i];
         value.object_class_id = Function.return_types.object_class_ids[i];
         value.struct_def = Function.return_types.struct_defs[i] ?
            Function.return_types.struct_defs[i] : value.struct_def;
         if (Function.return_types.array_elements[i].known) {
            value.array_element = Function.return_types.array_elements[i];
         }
         value.nullable = not Function.return_types.required[i];
         if (Function.return_types.is_explicit and value.primary != TiriType::Any and
             value.primary != TiriType::Unknown) {
            value.proof = StaticProof::Checked;
         }
         else if (Function.return_types.is_inferred) value.proof = StaticProof::Closed;
         else value.proof = StaticProof::Advisory;
         results.values[i] = value;
      }
      return results;
   }

   StaticCallableHandle function_callable(const FunctionExprPayload &Function, StaticBindingID Binding = {})
   {
      if (Function.callable) return Function.callable;
      auto &mutable_function = const_cast<FunctionExprPayload &>(Function);
      StaticResultSetHandle results = this->catalogue_.add_results(this->function_results(Function));
      StaticCallableDescriptor callable;
      callable.function = &Function;
      callable.results = results;
      callable.binding_id = Binding;
      callable.immutable = true;
      mutable_function.callable = this->catalogue_.add_callable(callable);
      return mutable_function.callable;
   }

   StaticCallableHandle resolve_binding_callable(StaticBindingID ID)
   {
      if (not ID) return {};
      auto &binding = this->catalogue_.binding(ID);
      if (binding.callable) return binding.callable;
      if (not binding.immutable or binding.resolving) return {};

      binding.resolving = true;
      StaticCallableHandle callable{};
      if (binding.function) callable = this->function_callable(*binding.function, ID);
      else if (binding.initialiser) {
         const ExprNode &initialiser = *binding.initialiser;
         if (initialiser.kind IS AstNodeKind::FunctionExpr) {
            callable = this->function_callable(std::get<FunctionExprPayload>(initialiser.data), ID);
         }
         else if (initialiser.kind IS AstNodeKind::IdentifierExpr) {
            const auto &reference = std::get<NameRef>(initialiser.data);
            binding.alias_of = reference.binding_id;
            callable = this->resolve_binding_callable(reference.binding_id);
         }
      }
      binding.resolving = false;
      binding.callable = callable;
      return callable;
   }

   void resolve_all_callables()
   {
      for (size_t id = 1; id < this->catalogue_.binding_count(); ++id) {
         this->resolve_binding_callable(StaticBindingID(id));
      }
   }

   void refresh_callables()
   {
      for (size_t id = 1; id < this->catalogue_.binding_count(); ++id) {
         auto &binding = this->catalogue_.binding(StaticBindingID(id));
         if (binding.callable) {
            auto &callable = this->catalogue_.callable(binding.callable);
            if (callable.function) callable.results = this->catalogue_.add_results(
               this->function_results(*callable.function));
         }
         else if (binding.immutable and binding.initialiser and
             binding.initialiser->kind IS AstNodeKind::CallExpr) {
            const auto &call = std::get<CallExprPayload>(binding.initialiser->data);
            if (call.result_type IS TiriType::Func and call.struct_def) {
               StaticResultSet results;
               results.declared_count = 1;
               results.stored_count = 1;
               results.values[0].primary = TiriType::Struct;
               results.values[0].struct_def = call.struct_def;
               results.values[0].proof = StaticProof::Closed;

               StaticCallableDescriptor callable;
               callable.results = this->catalogue_.add_results(results);
               callable.binding_id = StaticBindingID(id);
               callable.immutable = true;
               binding.callable = this->catalogue_.add_callable(callable);
            }
         }
      }
   }

   void annotate_call(CallExprPayload &Call)
   {
      if (auto *direct = std::get_if<DirectCallTarget>(&Call.target); direct and direct->callable) {
         if (direct->callable->kind IS AstNodeKind::FunctionExpr) {
            Call.callable = this->function_callable(std::get<FunctionExprPayload>(direct->callable->data));
         }
         else if (direct->callable->kind IS AstNodeKind::IdentifierExpr) {
            const auto &reference = std::get<NameRef>(direct->callable->data);
            Call.callable = this->resolve_binding_callable(reference.binding_id);
         }
      }
      if (Call.callable) Call.results = this->catalogue_.callable(Call.callable).results;
      if (not Call.results) Call.results = this->native_prototype_results(Call);
      if (not Call.results) Call.results = this->native_call_results(Call);
   }

   struct NativeInterface {
      GCtab *table = nullptr;
      std::string name;
   };

   [[nodiscard]] cTValue * protected_global_value(GCstr *Name) const
   {
      if (not Name or (Name->flags & STRFLAG_PROTECTED_GLOBAL) IS 0) return nullptr;
      return lj_tab_getstr(tabref(this->context_.lua().env), Name);
   }

   [[nodiscard]] std::optional<NativeInterface> native_interface(const ExprNode &Expression) const
   {
      if (Expression.kind IS AstNodeKind::IdentifierExpr) {
         const auto &reference = std::get<NameRef>(Expression.data);
         if (reference.binding_id or not reference.identifier.symbol) return std::nullopt;
         cTValue *value = this->protected_global_value(reference.identifier.symbol);
         if (not value or not tvistab(value)) return std::nullopt;
         return NativeInterface{
            .table = tabV(value),
            .name = std::string(strdata(reference.identifier.symbol), reference.identifier.symbol->len)
         };
      }

      if (Expression.kind != AstNodeKind::MemberExpr) return std::nullopt;
      const auto &member = std::get<MemberExprPayload>(Expression.data);
      if (not member.table or not member.member.symbol) return std::nullopt;
      auto parent = this->native_interface(*member.table);
      if (not parent) return std::nullopt;
      cTValue *value = lj_tab_getstr(parent->table, member.member.symbol);
      if (not value or not tvistab(value)) return std::nullopt;
      parent->table = tabV(value);
      parent->name.append(".");
      parent->name.append(strdata(member.member.symbol), member.member.symbol->len);
      return parent;
   }

   [[nodiscard]] StaticResultSetHandle native_prototype_results(CallExprPayload &Call)
   {
      const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
      if (not direct or not direct->callable) return {};

      const fprototype *prototype = nullptr;
      if (direct->callable->kind IS AstNodeKind::IdentifierExpr) {
         const auto &reference = std::get<NameRef>(direct->callable->data);
         if (reference.binding_id or not reference.identifier.symbol) return {};
         cTValue *value = this->protected_global_value(reference.identifier.symbol);
         if (not value or not tvisfunc(value) or isluafunc(funcV(value))) return {};
         prototype = get_func_prototype_by_hash(reference.identifier.symbol->hash);
      }
      else if (direct->callable->kind IS AstNodeKind::MemberExpr or
          direct->callable->kind IS AstNodeKind::SafeMemberExpr) {
         ExprNode *receiver = nullptr;
         GCstr *member = nullptr;
         if (direct->callable->kind IS AstNodeKind::MemberExpr) {
            const auto &payload = std::get<MemberExprPayload>(direct->callable->data);
            receiver = payload.table.get();
            member = payload.member.symbol;
         }
         else {
            const auto &payload = std::get<SafeMemberExprPayload>(direct->callable->data);
            receiver = payload.table.get();
            member = payload.member.symbol;
         }
         if (not receiver or not member) return {};
         auto interface = this->native_interface(*receiver);
         if (not interface) return {};
         cTValue *value = lj_tab_getstr(interface->table, member);
         if (not value or not tvisfunc(value) or isluafunc(funcV(value))) return {};
         prototype = get_prototype(interface->name, std::string_view(strdata(member), member->len));
      }

      if (not prototype) return {};
      StaticResultSet results = describe_native_prototype_results(prototype);
      if (results.stored_count > 0) {
         if (results.values[0].primary IS TiriType::Object and Call.object_class_id != CLASSID::NIL) {
            results.values[0].object_class_id = Call.object_class_id;
         }
         if ((results.values[0].primary IS TiriType::Struct or
             results.values[0].primary IS TiriType::Func) and Call.struct_def) {
            results.values[0].struct_def = Call.struct_def;
         }
      }
      return this->catalogue_.add_results(results);
   }

   [[nodiscard]] StaticResultSetHandle native_call_results(CallExprPayload &Call)
   {
      const ExprNode *receiver = nullptr;
      GCstr *member = nullptr;

      if (const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
          direct and direct->callable) {
         // A module function selection carries its own immutable signature, so its results are described without a
         // receiver descriptor.  This is the only path that can describe a compiler-managed namespace call, because
         // the namespace has no script value for descriptor propagation to observe.

         if (direct->callable->kind IS AstNodeKind::ModuleFunctionExpr) {
            const auto &payload = std::get<ModuleFunctionExprPayload>(direct->callable->data);
            if (not payload.module or not payload.function.symbol) return {};
            const FunctionField *fields = static_module_function(payload.module,
               std::string_view(strdata(payload.function.symbol), payload.function.symbol->len));
            if (not fields) return {};
            return this->catalogue_.add_results(
               describe_module_call_results(fields, &this->context_.lua()));
         }

         if (direct->callable->kind IS AstNodeKind::MemberExpr) {
            const auto &payload = std::get<MemberExprPayload>(direct->callable->data);
            receiver = payload.table.get();
            member = payload.member.symbol;
         }
         else if (direct->callable->kind IS AstNodeKind::SafeMemberExpr) {
            const auto &payload = std::get<SafeMemberExprPayload>(direct->callable->data);
            receiver = payload.table.get();
            member = payload.member.symbol;
         }
      }
      if (not receiver or not member) return {};
      StaticValueDescriptor base = this->descriptor_of(*receiver);
      if (base.primary IS TiriType::Object and base.proved()) {
         std::string_view exposed_name(strdata(member), member->len);
         if (member->hash IS kt::strhash("new")) {
            const fprototype *prototype = get_prototype("obj", exposed_name);
            if (not prototype) return {};

            StaticResultSet results = describe_native_prototype_results(prototype);
            if (results.stored_count > 0) {
               StaticValueDescriptor &child = results.values[0];
               child.nullable = false;
            }
            return this->catalogue_.add_results(results);
         }

         ObjectCallMemberKind kind = classify_object_call_member(exposed_name);
         if (kind IS ObjectCallMemberKind::None) return {};
         std::string_view native_name = exposed_name.substr(2);
         const FunctionField *fields = nullptr;

         if (kind IS ObjectCallMemberKind::Action) {
            if (not glActions) return {};
            for (size_t i = 1; glActions[i].Name; ++i) {
               if (std::string_view(glActions[i].Name) IS native_name) {
                  fields = glActions[i].Args;
                  return this->catalogue_.add_results(describe_object_call_results(fields));
               }
            }
            return {};
         }

         if (base.object_class_id IS CLASSID::NIL) return {};
         auto *meta_class = FindClass(base.object_class_id);
         if (not meta_class) return {};

         std::span<MethodEntry> methods;
         if (meta_class->getMethods(methods) != ERR::Okay) return {};
         for (size_t i = 1; i < methods.size(); ++i) {
            if (methods[i].Name and std::string_view(methods[i].Name) IS native_name) {
               fields = methods[i].Args;
               return this->catalogue_.add_results(describe_object_call_results(fields));
            }
         }
         return {};
      }

      if (base.primary IS TiriType::Str and base.proved()) {
         const fprototype *prototype = get_prototype(
            "string", std::string_view(strdata(member), member->len));
         if (prototype) {
            return this->catalogue_.add_results(describe_native_prototype_results(prototype));
         }
         return {};
      }

      if (base.primary IS TiriType::Array and base.proved()) {
         const fprototype *prototype = get_prototype(
            "array", std::string_view(strdata(member), member->len));
         if (not prototype) return {};

         StaticResultSet results = describe_native_prototype_results(prototype);
         if (results.stored_count > 0 and results.values[0].primary != TiriType::Array) {
            return this->catalogue_.add_results(results);
         }
         return {};
      }

      return {};
   }

   void report_method_argument(SourceSpan Span, std::string Message)
   {
      ParserDiagnostic diagnostic;
      diagnostic.severity = ParserDiagnosticSeverity::Error;
      diagnostic.code = ParserErrorCode::TypeMismatchArgument;
      diagnostic.message = std::move(Message);
      diagnostic.token = Token::from_span(Span, TokenKind::Identifier);
      this->context_.diagnostics().report(diagnostic);
   }

   void reject_builtin_method_shadow(ExprNode &Target, const StaticValueDescriptor &Assigned)
   {
      if (Target.kind != AstNodeKind::MemberExpr or Assigned.primary != TiriType::Func) return;
      auto &member = std::get<MemberExprPayload>(Target.data);
      if (member.builtin_shadow_reported or not member.table or not member.member.symbol) return;

      StaticValueDescriptor receiver = this->descriptor_of(*member.table);
      if (not receiver.proved() or receiver.primary IS TiriType::Any or receiver.primary IS TiriType::Unknown or
          not get_method_prototype_by_hash(receiver.primary, member.member.symbol->hash)) return;

      ParserDiagnostic diagnostic;
      diagnostic.severity = ParserDiagnosticSeverity::Error;
      diagnostic.code = ParserErrorCode::InvalidAssignment;
      diagnostic.message = std::format(
         "cannot assign a function to built-in method '{}'; use ['{}'] to create an explicit shadowing field",
         std::string_view(strdata(member.member.symbol), member.member.symbol->len),
         std::string_view(strdata(member.member.symbol), member.member.symbol->len));
      diagnostic.token = Token::from_span(Target.span, TokenKind::Identifier);
      this->context_.diagnostics().report(diagnostic);
      member.builtin_shadow_reported = true;
   }

   void reject_builtin_method_shadow(TableField &Field)
   {
      if (Field.builtin_shadow_reported or not (Field.kind IS TableFieldKind::Record) or not Field.name or
          not Field.name->symbol or not Field.value or
          not (this->descriptor_of(*Field.value).primary IS TiriType::Func) or
          not get_method_prototype_by_hash(TiriType::Table, Field.name->symbol->hash)) return;

      ParserDiagnostic diagnostic;
      diagnostic.severity = ParserDiagnosticSeverity::Error;
      diagnostic.code = ParserErrorCode::InvalidAssignment;
      diagnostic.message = std::format(
         "cannot assign a function to built-in method '{}'; use ['{}'] to create an explicit shadowing field",
         std::string_view(strdata(Field.name->symbol), Field.name->symbol->len),
         std::string_view(strdata(Field.name->symbol), Field.name->symbol->len));
      diagnostic.token = Token::from_span(Field.name->span, TokenKind::Identifier);
      this->context_.diagnostics().report(diagnostic);
      Field.builtin_shadow_reported = true;
   }

   void validate_builtin_method_arguments(CallExprPayload &Call)
   {
      if (not Call.builtin_method or Call.builtin_method->arguments_validated) return;
      const fprototype *prototype = Call.builtin_method->prototype;
      if (not prototype or prototype->param_count IS 0) return;

      const TiriType *parameters = prototype->param_types();
      bool validation_complete = true;
      for (size_t index = 0; index < Call.arguments.size(); ++index) {
         size_t native_index = index + 1;
         if (native_index >= prototype->param_count) break;
         if (Call.arguments[index]->kind IS AstNodeKind::IdentifierExpr) {
            const auto &reference = std::get<NameRef>(Call.arguments[index]->data);
            if (reference.binding_id and this->catalogue_.binding(reference.binding_id).is_variant) continue;
         }
         const StaticValueDescriptor actual = this->descriptor_of(*Call.arguments[index]);
         TiriType expected = parameters[native_index];
         if (actual.primary IS TiriType::Nil) {
            if ((prototype->flags & FProtoFlags::NoNil) != FProtoFlags::None) {
               this->report_method_argument(Call.arguments[index]->span,
                  std::format("required argument {} cannot be nil", index + 1));
            }
         }
         else if (expected != TiriType::Any and not actual.proved()) validation_complete = false;
         else if (expected != TiriType::Any and actual.primary != TiriType::Unknown and
             actual.primary != TiriType::Any and actual.primary != expected and
             not (expected IS TiriType::Func and actual.primary IS TiriType::Table)) {
            this->report_method_argument(Call.arguments[index]->span,
               std::format("argument {} expects {}, got {}", index + 1,
                  type_name(expected), type_name(actual.primary)));
         }
      }

      if ((prototype->flags & FProtoFlags::NoNil) != FProtoFlags::None and
          Call.arguments.size() + 1 < prototype->param_count and not Call.forwards_multret) {
         this->report_method_argument(Call.arguments.empty() ? SourceSpan{} : Call.arguments.back()->span,
            std::format("required argument {} is missing", Call.arguments.size() + 1));
      }
      if (Call.forwards_multret) validation_complete = false;
      Call.builtin_method->arguments_validated = validation_complete;
   }

   void resolve_builtin_method(CallExprPayload &Call)
   {
      Call.runtime_builtin_method.reset();
      if (Call.compiler_callable != BuiltinCallableID::Invalid) {
         Call.builtin_method.reset();
         return;
      }
      const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
      if (not direct or not direct->callable or
          (Call.argument_syntax != CallArgumentSyntax::Parenthesised and
           Call.argument_syntax != CallArgumentSyntax::Synthetic) or direct->callable->is_grouped) {
         Call.builtin_method.reset();
         return;
      }

      ExprNode *receiver = nullptr;
      GCstr *member = nullptr;
      bool safe = false;
      if (direct->callable->kind IS AstNodeKind::MemberExpr) {
         const auto &payload = std::get<MemberExprPayload>(direct->callable->data);
         receiver = payload.table.get();
         member = payload.member.symbol;
      }
      else if (direct->callable->kind IS AstNodeKind::SafeMemberExpr) {
         const auto &payload = std::get<SafeMemberExprPayload>(direct->callable->data);
         receiver = payload.table.get();
         member = payload.member.symbol;
         safe = true;
      }
      else {
         Call.builtin_method.reset();
         return;
      }

      if (not receiver or not member) {
         Call.builtin_method.reset();
         return;
      }
      if (receiver->kind IS AstNodeKind::IdentifierExpr) {
         const auto &reference = std::get<NameRef>(receiver->data);
         if (not reference.binding_id and reference.identifier.symbol and
             is_builtin_interface_namespace(reference.identifier.symbol->hash)) {
            Call.builtin_method.reset();
            return;
         }
         if (reference.binding_id and this->catalogue_.binding(reference.binding_id).is_variant) {
            Call.builtin_method.reset();
            Call.runtime_builtin_method = CallExprPayload::RuntimeBuiltinMethodCall{ member, safe };
            this->report_unresolved_method(Call, *receiver, member);
            return;
         }
      }
      const StaticValueDescriptor descriptor = this->descriptor_of(*receiver);
      TiriType receiver_type = descriptor.primary;
      if (Call.argument_syntax IS CallArgumentSyntax::Synthetic and receiver->kind IS AstNodeKind::CallExpr) {
         TiriType generated_type = std::get<CallExprPayload>(receiver->data).result_type;
         if (generated_type != TiriType::Unknown and generated_type != TiriType::Any) receiver_type = generated_type;
      }
      if (receiver_type IS TiriType::Table and
          not get_method_prototype_by_hash(TiriType::Table, member->hash)) {
         Call.builtin_method.reset();
         return;
      }
      if ((not descriptor.proved() and Call.argument_syntax != CallArgumentSyntax::Synthetic) or
          receiver_type IS TiriType::Any or receiver_type IS TiriType::Unknown or
          (descriptor.nullable and not safe and Call.argument_syntax != CallArgumentSyntax::Synthetic)) {
         Call.builtin_method.reset();
         if (Call.argument_syntax IS CallArgumentSyntax::Parenthesised) {
            Call.runtime_builtin_method = CallExprPayload::RuntimeBuiltinMethodCall{ member, safe };
            this->report_unresolved_method(Call, *receiver, member);
         }
         return;
      }

      const fprototype *prototype = get_method_prototype_by_hash(receiver_type, member->hash);
      if (not prototype) {
         Call.builtin_method.reset();
         if (receiver_type IS TiriType::Userdata and
             Call.argument_syntax IS CallArgumentSyntax::Parenthesised) {
            Call.runtime_builtin_method = CallExprPayload::RuntimeBuiltinMethodCall{ member, safe };
         }
         return;
      }

      bool already_validated = Call.builtin_method and Call.builtin_method->prototype IS prototype and
         Call.builtin_method->arguments_validated;
      Call.builtin_method = CallExprPayload::BuiltinMethodCall{
         .prototype = prototype,
         .callable = prototype->builtin_callable_id,
         .receiver_type = receiver_type,
         .safe = safe,
         .arguments_validated = already_validated
      };
      this->validate_builtin_method_arguments(Call);
   }

   void report_unresolved_method(CallExprPayload &Call, const ExprNode &Receiver, GCstr *Member)
   {
      if (Call.unresolved_method_reported or not this->context_.config().warn_unresolved_methods) return;

      this->context_.emit_warning(ParserErrorCode::UnresolvedMethodReceiver,
         Token::from_span(Receiver.span, TokenKind::Identifier), std::format(
         "cannot resolve the receiver type for dot-method '{}'; built-in method selection will occur at runtime",
         std::string_view(strdata(Member), Member->len)));
      Call.unresolved_method_reported = true;
   }

   void annotate_callables_expression(ExprNode &Expression)
   {
      this->walk_expression_children(Expression, [this](ExprNode &Child) {
         this->annotate_callables_expression(Child);
      });
      if (Expression.kind IS AstNodeKind::CallExpr or Expression.kind IS AstNodeKind::SafeCallExpr) {
         this->annotate_call(std::get<CallExprPayload>(Expression.data));
      }
   }

   void annotate_callables_block(BlockStmt &Block)
   {
      this->walk_block_expressions(Block, [this](ExprNode &Expression) {
         this->annotate_callables_expression(Expression);
      });
   }

   [[nodiscard]] StaticValueHandle add_value(StaticValueDescriptor Value)
   {
      return this->catalogue_.add_value(Value);
   }

   [[nodiscard]] StaticValueDescriptor descriptor_of(const ExprNode &Expression) const
   {
      return this->catalogue_.value(Expression.static_value);
   }

   [[nodiscard]] StaticValueDescriptor global_value(GCstr *Name) const
   {
      for (auto entry = this->global_values_.rbegin(); entry != this->global_values_.rend(); ++entry) {
         if (entry->first IS Name) return this->catalogue_.value(entry->second);
      }
      return {};
   }

   [[nodiscard]] StaticValueDescriptor literal_descriptor(const LiteralValue &Literal) const
   {
      StaticValueDescriptor result;
      result.proof = StaticProof::Closed;
      switch (Literal.kind) {
         case LiteralKind::Nil:
            result.primary = TiriType::Nil;
            result.nullable = true;
            break;
         case LiteralKind::Boolean: result.primary = TiriType::Bool; break;
         case LiteralKind::Number:  result.primary = TiriType::Num; break;
         case LiteralKind::String:  result.primary = TiriType::Str; break;
      }
      return result;
   }

   [[nodiscard]] std::optional<bool> constant_truth(const ExprNode &Expression) const
   {
      if (Expression.kind != AstNodeKind::LiteralExpr) return std::nullopt;
      const auto &literal = std::get<LiteralValue>(Expression.data);
      if (literal.kind IS LiteralKind::Nil) return false;
      if (literal.kind IS LiteralKind::Boolean) return literal.bool_value;
      return true;
   }

   [[nodiscard]] StaticValueDescriptor declared_descriptor(
      TiriType Type, struct_record *StructDef, const ArrayElementDescriptor &ArrayElement, StaticProof Proof) const
   {
      StaticValueDescriptor result;
      result.primary = Type;
      result.struct_def = StructDef;
      result.array_element = ArrayElement;
      result.proof = Proof;
      result.nullable = true;
      return result;
   }

   [[nodiscard]] std::optional<std::pair<std::string_view, bool>> array_constructor_type(
      const CallExprPayload &Call) const
   {
      const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
      if (not direct or not direct->callable or direct->callable->kind != AstNodeKind::MemberExpr) return std::nullopt;
      const auto &member = std::get<MemberExprPayload>(direct->callable->data);
      if (not member.table or member.table->kind != AstNodeKind::IdentifierExpr or not member.member.symbol) {
         return std::nullopt;
      }
      const auto &base = std::get<NameRef>(member.table->data);
      if (base.binding_id or not base.identifier.symbol or base.identifier.symbol->hash != kt::strhash("array")) {
         return std::nullopt;
      }

      bool is_of = member.member.symbol->hash IS kt::strhash("of");
      bool is_new = member.member.symbol->hash IS kt::strhash("new");
      if (not is_of and not is_new) return std::nullopt;
      size_t position = is_of ? 0 : 1;
      if (position >= Call.arguments.size() or Call.arguments[position]->kind != AstNodeKind::LiteralExpr) {
         return std::nullopt;
      }
      const auto &literal = std::get<LiteralValue>(Call.arguments[position]->data);
      if (literal.kind != LiteralKind::String or not literal.string_value) return std::nullopt;
      return std::pair(std::string_view(strdata(literal.string_value), literal.string_value->len), true);
   }

   [[nodiscard]] StaticValueDescriptor call_descriptor(CallExprPayload &Call, bool Safe)
   {
      this->resolve_builtin_method(Call);
      if (Call.builtin_method) {
         Call.results = this->catalogue_.add_results(
            describe_native_prototype_results(Call.builtin_method->prototype));
         Safe = Safe or Call.builtin_method->safe;
      }
      else if (this->native_calls_only_) {
         if (not Call.results) Call.results = this->native_call_results(Call);
      }
      else this->annotate_call(Call);
      StaticValueDescriptor result;

      if (Call.results) {
         result = this->catalogue_.results(Call.results).value_at(0);
      }
      if (Call.authorised_contextual_designation and not Call.arguments.empty()) {
         result = this->descriptor_of(*Call.arguments.front());
         result.primary = TiriType::Table;
         result.proof = StaticProof::Closed;
         result.nullable = false;
         result.contextuality = Call.contextual_designation_result;
      }
      else if (Call.compiler_callable IS builtin_callable_id(FastFunc::object_create) and
          Call.result_type IS TiriType::Object) {
         // `obj<Class> { ... }` is compiler-owned syntax.  Its synthetic `obj.new` callee is retained only for
         // source metadata, so a local `obj` binding must not weaken the known result descriptor.
         result.primary = TiriType::Object;
         result.object_class_id = Call.object_class_id;
         result.proof = StaticProof::Closed;
         result.nullable = false;
      }
      else if (auto array_type = this->array_constructor_type(Call)) {
         auto element = describe_array_element(array_type->first, &this->context_.lua());
         if (element) {
            result.primary = TiriType::Array;
            result.array_element = *element;
            result.proof = StaticProof::Closed;
            result.nullable = false;
         }
      }
      else if (const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
          direct and direct->callable and direct->callable->kind IS AstNodeKind::IdentifierExpr and
          not std::get<NameRef>(direct->callable->data).binding_id and
          std::get<NameRef>(direct->callable->data).identifier.symbol and
          std::get<NameRef>(direct->callable->data).identifier.symbol->hash IS kt::strhash("range")) {
         result.primary = TiriType::Range;
         result.proof = StaticProof::Closed;
         result.nullable = false;
      }
      else if (const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
          direct and direct->callable and direct->callable->kind IS AstNodeKind::MemberExpr) {
         const auto &member = std::get<MemberExprPayload>(direct->callable->data);
         const NameRef *base = member.table and member.table->kind IS AstNodeKind::IdentifierExpr ?
            std::get_if<NameRef>(&member.table->data) : nullptr;
         if (base and not base->binding_id and base->identifier.symbol and member.member.symbol and
             member.member.symbol->hash IS kt::strhash("new")) {
            if (base->identifier.symbol->hash IS kt::strhash("range")) {
               result.primary = TiriType::Range;
               result.proof = StaticProof::Closed;
               result.nullable = false;
            }
            else if (base->identifier.symbol->hash IS kt::strhash("obj") and
                Call.result_type IS TiriType::Object) {
               result.primary = TiriType::Object;
               result.object_class_id = Call.object_class_id;
               result.proof = StaticProof::Closed;
               result.nullable = false;
            }
            else if (base->identifier.symbol->hash IS kt::strhash("struct") and
                Call.result_type IS TiriType::Struct) {
               result.primary = TiriType::Struct;
               result.struct_def = Call.struct_def;
               result.proof = StaticProof::Closed;
               result.nullable = false;
            }
         }
      }

      if (result.primary IS TiriType::Unknown and Call.result_type != TiriType::Unknown) {
         result.primary = Call.result_type;
         result.object_class_id = Call.object_class_id;
         result.struct_def = Call.struct_def;
         result.proof = StaticProof::Advisory;
      }

      if (Call.results and this->catalogue_.results(Call.results).stored_count > 0 and
          this->catalogue_.results(Call.results).value_at(0) != result) {
         StaticResultSet enriched_results = this->catalogue_.results(Call.results);
         enriched_results.values[0] = result;
         Call.results = this->catalogue_.add_results(enriched_results);
      }

      if (Safe) {
         result.nullable = true;
         if (Call.results) {
            StaticResultSet safe_results = this->catalogue_.results(Call.results);
            for (size_t i = 0; i < safe_results.stored_count; ++i) {
               safe_results.values[i].nullable = true;
            }
            Call.results = this->catalogue_.add_results(safe_results);
            result = safe_results.value_at(0);
         }
      }
      return result;
   }

   [[nodiscard]] StaticValueDescriptor member_descriptor(
      const StaticValueDescriptor &Base, GCstr *Member) const
   {
      if (not Base.proved()) return {};
      if ((Base.primary IS TiriType::Struct or Base.primary IS TiriType::Table) and Base.struct_def) {
         return describe_struct_field(Base.struct_def, Member);
      }
      if (Base.primary IS TiriType::Object and Base.object_class_id != CLASSID::NIL and Member) {
         auto field = lookup_field_type(Base.object_class_id, Member->hash);
         if (field and field->type != TiriType::Unknown) {
            StaticValueDescriptor result;
            result.primary = field->type;
            result.object_class_id = field->object_class_id;
            result.proof = StaticProof::Trusted;
            return result;
         }
      }
      return {};
   }

   struct CollectionMethodCall {
      ExprNode *source = nullptr;
      GCstr *operation = nullptr;
      bool receiver_first = false;
   };

   [[nodiscard]] static bool collection_interface_member(
      const DirectCallTarget &Direct, uint32_t InterfaceHash, uint32_t MemberHash)
   {
      if (not Direct.callable or Direct.callable->kind != AstNodeKind::MemberExpr) return false;
      const auto &member = std::get<MemberExprPayload>(Direct.callable->data);
      if (not member.table or member.table->kind != AstNodeKind::IdentifierExpr or
          not member.member.symbol or member.member.symbol->hash != MemberHash) return false;
      const auto &base = std::get<NameRef>(member.table->data);
      return not base.binding_id and base.identifier.symbol and
         base.identifier.symbol->hash IS InterfaceHash;
   }

   [[nodiscard]] static std::optional<CollectionMethodCall> collection_method_call(const CallExprPayload &Call)
   {
      auto *direct = std::get_if<DirectCallTarget>(&Call.target);
      if (not direct or not direct->callable or direct->callable->kind != AstNodeKind::MemberExpr) return std::nullopt;

      auto &member = std::get<MemberExprPayload>(direct->callable->data);
      if (not member.table or not member.member.symbol) return std::nullopt;
      if (member.table->kind IS AstNodeKind::IdentifierExpr) {
         const auto &base = std::get<NameRef>(member.table->data);
         bool collection_interface = base.identifier.symbol and
            (base.identifier.symbol->hash IS kt::strhash("array") or
             base.identifier.symbol->hash IS kt::strhash("range"));
         if (not base.binding_id and collection_interface) {
            if (Call.arguments.empty()) return std::nullopt;
            return CollectionMethodCall{ Call.arguments[0].get(), member.member.symbol, true };
         }
      }
      return CollectionMethodCall{ member.table.get(), member.member.symbol, false };
   }

   struct CollectionCallbackCall {
      ExprNode *source = nullptr;
      GCstr *operation = nullptr;
      size_t callback_index = 0;
      bool reduce = false;
   };

   [[nodiscard]] static std::optional<CollectionCallbackCall> collection_callback_call(const CallExprPayload &Call)
   {
      auto collection = collection_method_call(Call);
      if (not collection or not collection->operation) return std::nullopt;

      bool reduce = collection->operation->hash IS kt::strhash("reduce");
      bool callback_operation = reduce or collection->operation->hash IS kt::strhash("each") or
         collection->operation->hash IS kt::strhash("filter") or collection->operation->hash IS kt::strhash("map") or
         collection->operation->hash IS kt::strhash("mapSame") or collection->operation->hash IS kt::strhash("any") or
         collection->operation->hash IS kt::strhash("all") or collection->operation->hash IS kt::strhash("find") or
         collection->operation->hash IS kt::strhash("findIndex");
      if (not callback_operation) return std::nullopt;

      size_t callback_index = collection->receiver_first ? (reduce ? 2 : 1) : (reduce ? 1 : 0);
      if (callback_index >= Call.arguments.size()) return std::nullopt;
      return CollectionCallbackCall{ collection->source, collection->operation, callback_index, reduce };
   }

   [[nodiscard]] StaticValueDescriptor mapped_array_descriptor(const CallExprPayload &Call) const
   {
      auto collection = collection_method_call(Call);
      if (not collection or not collection->source or not collection->operation) return {};

      StaticValueDescriptor source = this->descriptor_of(*collection->source);
      if (source.primary != TiriType::Array and source.primary != TiriType::Range) return {};

      uint32_t operation = collection->operation->hash;
      if (operation IS kt::strhash("mapSame")) {
         if (source.primary IS TiriType::Array and source.array_element.known) {
            source.proof = StaticProof::Trusted;
            source.nullable = false;
            return source;
         }
         return {};
      }
      if (operation != kt::strhash("map")) return {};

      size_t type_index = collection->receiver_first ? 2 : 1;
      if (type_index < Call.arguments.size()) {
         const ExprNode &type_expression = *Call.arguments[type_index];
         if (type_expression.kind IS AstNodeKind::LiteralExpr) {
            const auto &literal = std::get<LiteralValue>(type_expression.data);
            if (literal.kind IS LiteralKind::String and literal.string_value) {
               std::string_view type_name(strdata(literal.string_value), literal.string_value->len);
               if (auto element = describe_array_element(type_name, &this->context_.lua())) {
                  StaticValueDescriptor result;
                  result.primary = TiriType::Array;
                  result.array_element = *element;
                  result.proof = StaticProof::Trusted;
                  result.nullable = false;
                  return result;
               }
            }
            if (literal.kind IS LiteralKind::Nil) {
               StaticValueDescriptor result;
               result.primary = TiriType::Array;
               if (auto element = describe_array_element("any", &this->context_.lua())) {
                  result.array_element = *element;
               }
               result.proof = StaticProof::Trusted;
               result.nullable = false;
               return result;
            }
         }

         StaticValueDescriptor result;
         result.primary = TiriType::Array;
         result.proof = StaticProof::Trusted;
         result.nullable = false;
         return result;
      }

      StaticValueDescriptor result;
      result.primary = TiriType::Array;
      if (auto element = describe_array_element("any", &this->context_.lua())) result.array_element = *element;
      result.proof = StaticProof::Trusted;
      result.nullable = false;
      return result;
   }

   void update_call_result_descriptor(CallExprPayload &Call, const StaticValueDescriptor &Result)
   {
      StaticResultSet results;
      results.stored_count = 1;
      results.values[0] = Result;
      if (Call.results) {
         results = this->catalogue_.results(Call.results);
         if (results.stored_count IS 0) results.stored_count = 1;
         results.values[0] = Result;
      }
      Call.results = this->catalogue_.add_results(results);
   }

   [[nodiscard]] StaticValueDescriptor array_preserving_call_descriptor(const CallExprPayload &Call) const
   {
      auto collection = collection_method_call(Call);
      if (not collection or not collection->source or not collection->operation) return {};
      StaticValueDescriptor receiver = this->descriptor_of(*collection->source);
      if (receiver.primary != TiriType::Array or not receiver.array_element.known) return {};

      switch (collection->operation->hash) {
         case kt::strhash("each"):
         case kt::strhash("filter"):
         case kt::strhash("slice"):
         case kt::strhash("clone"):
         case kt::strhash("mapSame"):
            receiver.proof = StaticProof::Trusted;
            return receiver;
         default:
            return {};
      }
   }

   [[nodiscard]] bool is_for_each_call(const CallExprPayload &Call) const
   {
      const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
      if (not direct or not direct->callable or direct->callable->kind != AstNodeKind::IdentifierExpr) return false;
      const auto &reference = std::get<NameRef>(direct->callable->data);
      if (reference.binding_id or not reference.identifier.symbol or
          reference.identifier.symbol->hash != kt::strhash("forEach")) return false;
      cTValue *builtin = this->protected_global_value(reference.identifier.symbol);
      return builtin and tvisfunc(builtin) and not isluafunc(funcV(builtin));
   }

   void preserve_for_each_result(CallExprPayload &Call, const ExprNode &Target)
   {
      if (not this->is_for_each_call(Call)) return;
      StaticResultSet results;
      results.stored_count = 1;
      results.values[0] = this->descriptor_of(Target);
      Call.results = this->catalogue_.add_results(results);
   }

   [[nodiscard]] StaticValueDescriptor table_derivation_call_descriptor(const CallExprPayload &Call) const
   {
      const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
      if (not direct or not direct->callable or direct->callable->kind != AstNodeKind::MemberExpr or
          Call.arguments.empty()) return {};

      const auto &member = std::get<MemberExprPayload>(direct->callable->data);
      const auto *base = member.table and member.table->kind IS AstNodeKind::IdentifierExpr ?
         std::get_if<NameRef>(&member.table->data) : nullptr;
      if (not base or base->binding_id or not base->identifier.symbol or not member.member.symbol or
          member.member.symbol->hash != kt::strhash("slice")) return {};

      uint32_t interface_hash = base->identifier.symbol->hash;
      if (interface_hash != kt::strhash("range") and interface_hash != kt::strhash("table")) return {};

      StaticValueDescriptor source = this->descriptor_of(*Call.arguments.front());
      if (source.primary IS TiriType::Table or source.contextuality != StaticContextuality::Unknown or
          interface_hash IS kt::strhash("table")) {
         source.primary = TiriType::Table;
         source.proof = source.proved() ? source.proof : StaticProof::Trusted;
         source.nullable = false;
         return source;
      }
      if (source.primary IS TiriType::Str) {
         source.proof = source.proved() ? source.proof : StaticProof::Trusted;
         source.nullable = false;
         return source;
      }
      return {};
   }

   [[nodiscard]] static StaticValueDescriptor array_element_value(
      const ArrayElementDescriptor &Element)
   {
      StaticValueDescriptor result;
      result.primary = Element.logical_type;
      result.object_class_id = Element.object_class_id;
      result.struct_def = Element.struct_def;
      result.proof = StaticProof::Trusted;
      result.nullable = Element.storage IS AET::STR_GC or Element.storage IS AET::OBJECT or
         Element.storage IS AET::TABLE or Element.storage IS AET::ARRAY;
      return result;
   }

   [[nodiscard]] StaticValueDescriptor collection_search_result_descriptor(const CallExprPayload &Call) const
   {
      auto collection = collection_method_call(Call);
      if (not collection or not collection->source or not collection->operation) return {};

      StaticValueDescriptor source = this->descriptor_of(*collection->source);
      if (source.primary != TiriType::Array and source.primary != TiriType::Range) return {};

      const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
      bool builtin = (source.primary IS TiriType::Array and Call.builtin_method and
         (Call.builtin_method->callable IS builtin_callable_id(FastFunc::array_find) or
          Call.builtin_method->callable IS builtin_callable_id(FastFunc::array_findIndex) or
          Call.builtin_method->callable IS builtin_callable_id(FastFunc::array_indexOf))) or
         (source.primary IS TiriType::Range and Call.builtin_method and
         (Call.builtin_method->callable IS builtin_callable_id(FastFunc::range_find) or
          Call.builtin_method->callable IS builtin_callable_id(FastFunc::range_findIndex) or
          Call.builtin_method->callable IS builtin_callable_id(FastFunc::range_indexOf)));
      if (not builtin and collection->receiver_first and direct) {
         builtin = (source.primary IS TiriType::Array and collection_interface_member(
            *direct, kt::strhash("array"), collection->operation->hash)) or
            (source.primary IS TiriType::Range and collection_interface_member(
            *direct, kt::strhash("range"), collection->operation->hash));
      }
      if (not builtin) return {};

      uint32_t operation = collection->operation->hash;
      if (operation IS kt::strhash("find")) {
         if (source.primary IS TiriType::Array and source.array_element.known) {
            StaticValueDescriptor result = array_element_value(source.array_element);
            result.nullable = true;
            return result;
         }
         if (source.primary IS TiriType::Range) {
            return StaticValueDescriptor{ .primary=TiriType::Num, .proof=StaticProof::Trusted, .nullable=true };
         }
      }
      if (operation IS kt::strhash("findIndex") or operation IS kt::strhash("indexOf")) {
         return StaticValueDescriptor{ .primary=TiriType::Num, .proof=StaticProof::Trusted, .nullable=true };
      }
      return {};
   }

   void propagate_function_expression(
      ExprNode &Expression, std::span<const StaticValueDescriptor> ContextParameters)
   {
      auto &function = std::get<FunctionExprPayload>(Expression.data);
      StaticValueDescriptor value;
      value.primary = TiriType::Func;
      value.proof = StaticProof::Closed;
      this->function_callable(function);
      this->propagate_function(function, ContextParameters);
      Expression.static_value = this->add_value(value);
      Expression.static_results = {};
   }

   // Resolve an expression to the table constructor that produced it, following only aliases whose allocation is
   // visible in the analysed function.  Used to inspect a metatable literal for its `__context` marker.

   [[nodiscard]] const TableExprPayload * resolved_table_literal(const ExprNode &Expression, unsigned Depth = 0) const
   {
      if (Depth > 8) return nullptr;

      if (Expression.kind IS AstNodeKind::TableExpr) return &std::get<TableExprPayload>(Expression.data);

      if (Expression.kind IS AstNodeKind::IdentifierExpr) {
         const auto &reference = std::get<NameRef>(Expression.data);
         if (not reference.binding_id) return nullptr;
         const auto &binding = this->catalogue_.binding(reference.binding_id);
         // A rewritten binding no longer describes one literal.  Depth is deliberately not checked: a metatable
         // declared in an enclosing function is still a visible literal, and Section 5.2 keeps a proven marker valid
         // across aliases and storage.  Ownership of the designation *target* is what depth constrains.
         if (not binding.immutable or binding.is_parameter or not binding.initialiser) return nullptr;
         return this->resolved_table_literal(*binding.initialiser, Depth + 1);
      }

      return nullptr;
   }

   // A metatable declares contextual designation when its raw `__context` field is exactly the literal `true`.  The
   // lookup is deliberately raw and shallow: `__index` and any metatable of the metatable are not consulted.

   [[nodiscard]] static bool declares_contextual_marker(const TableExprPayload &Metatable)
   {
      for (const auto &field : Metatable.fields) {
         if (field.kind != TableFieldKind::Record or not field.name or not field.name->symbol) continue;
         std::string_view name(strdata(field.name->symbol), field.name->symbol->len);
         if (name != "__context") continue;
         if (not field.value or field.value->kind != AstNodeKind::LiteralExpr) return false;
         const auto &literal = std::get<LiteralValue>(field.value->data);
         return literal.kind IS LiteralKind::Boolean and literal.bool_value;
      }
      return false;
   }

   // Classify a direct `setmetatable(Target, Metatable)` call for contextual designation.
   //
   // An owned target is authorised: a statically known marker designates it, and an unresolved metatable takes the
   // authorised runtime path where an observed raw `true` designates and anything else is an ordinary metatable
   // change.  An unowned target with a statically known marker is a compile-time error; if the marker can only be
   // discovered dynamically the unauthorised call raises at runtime instead.

   void analyse_contextual_designation(CallExprPayload &Call)
   {
      const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
      if (not direct or not direct->callable) return;
      if (direct->callable->kind != AstNodeKind::IdentifierExpr) return;

      const auto &reference = std::get<NameRef>(direct->callable->data);
      // A local binding of that name shadows the built-in and carries no compiler authorisation.
      if (reference.binding_id or not reference.identifier.symbol) return;
      std::string_view name(strdata(reference.identifier.symbol), reference.identifier.symbol->len);
      if (name != "setmetatable") return;

      cTValue *builtin = this->protected_global_value(reference.identifier.symbol);
      if (not builtin or not tvisfunc(builtin) or isluafunc(funcV(builtin))) return;
      if (Call.arguments.size() < 2 or not Call.arguments[0] or not Call.arguments[1]) return;

      const ExprNode &target = *Call.arguments[0];
      const TableExprPayload *metatable = this->resolved_table_literal(*Call.arguments[1]);
      const bool marker_known = metatable and declares_contextual_marker(*metatable);

      TableOwnershipProof proof = classify_table_ownership(
         this->context_, target, this->function_depth_, this->enclosing_body_);

      if (proof.owned()) {
         Call.authorised_contextual_designation = true;
         StaticContextuality target_contextuality = this->descriptor_of(target).contextuality;
         if (marker_known) Call.contextual_designation_result = StaticContextuality::Contextual;
         else if (metatable) Call.contextual_designation_result = target_contextuality;
         else if (target_contextuality IS StaticContextuality::Contextual) {
            Call.contextual_designation_result = StaticContextuality::Contextual;
         }

         // A known true or unresolved marker can change an owned allocation after earlier ordinary calls.  Invalidate
         // other ordinary bindings conservatively because aliases share the same identity, then publish the precise
         // post-call state on a direct target binding.  Literals allocated after this point are classified normally.
         if (marker_known or not metatable) {
            for (size_t id = 1; id < this->catalogue_.binding_count(); ++id) {
               auto &binding = this->catalogue_.binding(StaticBindingID(id));
               StaticValueDescriptor value = this->catalogue_.value(binding.value);
               if (value.contextuality != StaticContextuality::Ordinary) continue;
               value.contextuality = StaticContextuality::Unknown;
               binding.value = this->add_value(value);
            }

            if (target.kind IS AstNodeKind::IdentifierExpr) {
               auto &mutable_target = const_cast<ExprNode &>(target);
               auto &reference = std::get<NameRef>(mutable_target.data);
               if (reference.binding_id) {
                  StaticValueDescriptor value = this->descriptor_of(target);
                  value.primary = TiriType::Table;
                  value.proof = StaticProof::Closed;
                  value.nullable = false;
                  value.contextuality = Call.contextual_designation_result;
                  auto &binding = this->catalogue_.binding(reference.binding_id);
                  binding.value = this->add_value(value);
                  reference.identifier.static_value = binding.value;
                  mutable_target.static_value = binding.value;
               }
            }
         }
         return;
      }

      // A table already designated contextual is always eligible for later metatable changes, so nothing is rejected
      // on the strength of the target's own contextuality.
      if (this->descriptor_of(target).contextuality IS StaticContextuality::Contextual) return;

      if (not marker_known) return;

      ParserDiagnostic diagnostic;
      diagnostic.severity = ParserDiagnosticSeverity::Error;
      diagnostic.code = ParserErrorCode::InvalidAssignment;
      diagnostic.message = std::format(
         "cannot designate a contextual table through '__context' because {}",
         foreign_table_source_reason(proof.foreign_source));
      diagnostic.token = Token::from_span(target.span, TokenKind::Identifier);
      this->context_.diagnostics().report(diagnostic);
   }

   void propagate_call_children(CallExprPayload &Call)
   {
      if (auto *direct = std::get_if<DirectCallTarget>(&Call.target)) {
         if (direct->callable) this->propagate_expression(*direct->callable);
      }

      auto collection_callback = collection_callback_call(Call);
      for (size_t i = 0; i < Call.arguments.size(); ++i) {
         auto &argument = Call.arguments[i];
         if (not argument) continue;

         bool contextual_callback = collection_callback and i IS collection_callback->callback_index and
            collection_callback->source and
            argument->kind IS AstNodeKind::FunctionExpr;
         StaticValueDescriptor source = contextual_callback ? this->descriptor_of(*collection_callback->source) :
            StaticValueDescriptor{};
         if (contextual_callback and
             not (source.primary IS TiriType::Array and
                  collection_callback->operation->hash IS kt::strhash("find")) and
             ((source.primary IS TiriType::Array and source.array_element.known) or
              source.primary IS TiriType::Range)) {
            std::array<StaticValueDescriptor, 3> parameters;
            size_t parameter_count = 2;
            parameters[0] = source.primary IS TiriType::Array ? array_element_value(source.array_element) :
               StaticValueDescriptor{ .primary=TiriType::Num, .proof=StaticProof::Trusted, .nullable=false };
            parameters[1].primary = TiriType::Num;
            parameters[1].proof = StaticProof::Trusted;
            parameters[1].nullable = false;
            if (collection_callback->reduce) {
               parameters[2] = parameters[1];
               parameters[1] = this->descriptor_of(*Call.arguments[collection_callback->callback_index - 1]);
               parameter_count = 3;
            }
            this->propagate_function_expression(*argument, std::span(parameters.data(), parameter_count));
         }
         else this->propagate_expression(*argument);
      }
   }

   void propagate_expression(ExprNode &Expression)
   {
      if (Expression.kind IS AstNodeKind::CallExpr or Expression.kind IS AstNodeKind::SafeCallExpr) {
         this->propagate_call_children(std::get<CallExprPayload>(Expression.data));
      }
      else {
         this->walk_expression_children(Expression, [this](ExprNode &Child) {
            this->propagate_expression(Child);
         });
      }

      StaticValueDescriptor value;
      StaticResultSetHandle results{};
      switch (Expression.kind) {
         case AstNodeKind::CurrentContextExpr:
            value.primary = TiriType::Table;
            value.nullable = false;
            value.proof = StaticProof::Closed;
            break;
         case AstNodeKind::LiteralExpr:
            value = this->literal_descriptor(std::get<LiteralValue>(Expression.data));
            break;
         case AstNodeKind::IdentifierExpr: {
            const auto &reference = std::get<NameRef>(Expression.data);
            if (reference.binding_id) {
               const auto &binding = this->catalogue_.binding(reference.binding_id);
               value = this->catalogue_.value(binding.value);
               if (binding.function_depth != this->function_depth_) {
                  value.contextuality = StaticContextuality::Unknown;
               }
            }
            else value = this->global_value(reference.identifier.symbol);
            break;
         }
         case AstNodeKind::TableExpr: {
            auto &table = std::get<TableExprPayload>(Expression.data);
            for (auto &field : table.fields) this->reject_builtin_method_shadow(field);
            value.primary = TiriType::Table;
            value.proof = StaticProof::Closed;
            // The allocation is visible here, so an undesignated literal is provably ordinary rather than unknown.
            value.contextuality = table.contextual ? StaticContextuality::Contextual : StaticContextuality::Ordinary;
            break;
         }
         case AstNodeKind::RangeExpr:
            value.primary = TiriType::Range;
            value.proof = StaticProof::Closed;
            break;
         case AstNodeKind::FunctionExpr: {
            auto &function = std::get<FunctionExprPayload>(Expression.data);
            value.primary = TiriType::Func;
            value.proof = StaticProof::Closed;
            this->function_callable(function);
            this->propagate_function(function);
            break;
         }
         case AstNodeKind::TypeTestExpr:
            value.primary = TiriType::Bool;
            value.proof = StaticProof::Closed;
            value.nullable = false;
            break;
         case AstNodeKind::CallExpr:
         case AstNodeKind::SafeCallExpr: {
            auto &call = std::get<CallExprPayload>(Expression.data);
            if (Expression.kind IS AstNodeKind::CallExpr) this->analyse_contextual_designation(call);
            value = this->call_descriptor(call, Expression.kind IS AstNodeKind::SafeCallExpr);
            StaticValueDescriptor mapped = this->mapped_array_descriptor(call);
            if (mapped.primary != TiriType::Unknown) {
               this->update_call_result_descriptor(call, mapped);
               value = mapped;
            }
            StaticValueDescriptor search_result = this->collection_search_result_descriptor(call);
            if (search_result.primary != TiriType::Unknown) {
               this->update_call_result_descriptor(call, search_result);
               value = search_result;
            }
            StaticValueDescriptor transferred = this->array_preserving_call_descriptor(call);
            if (transferred.primary != TiriType::Unknown) {
               transferred.nullable = value.nullable;
               value = transferred;
            }
            StaticValueDescriptor derived_table = this->table_derivation_call_descriptor(call);
            if (derived_table.primary != TiriType::Unknown) {
               derived_table.nullable = value.nullable;
               value = derived_table;
            }
            if (not call.arguments.empty()) {
               this->preserve_for_each_result(call, *call.arguments.front());
               if (this->is_for_each_call(call)) value = this->descriptor_of(*call.arguments.front());
            }
            results = call.results;
            break;
         }
         case AstNodeKind::ResultFilterExpr: {
            auto &filter = std::get<ResultFilterPayload>(Expression.data);
            if (filter.expression and filter.expression->static_results) {
               StaticResultSet mapped = map_static_result_filter(
                  this->catalogue_.results(filter.expression->static_results),
                  filter.keep_mask, filter.explicit_count, filter.trailing_keep);
               results = this->catalogue_.add_results(mapped);
               value = mapped.value_at(0);
            }
            break;
         }
         case AstNodeKind::PipeExpr: {
            auto &pipe = std::get<PipeExprPayload>(Expression.data);
            if (pipe.rhs_call) {
               if (pipe.lhs and (pipe.rhs_call->kind IS AstNodeKind::CallExpr or
                                pipe.rhs_call->kind IS AstNodeKind::SafeCallExpr)) {
                  auto &call = std::get<CallExprPayload>(pipe.rhs_call->data);
                  if (this->is_for_each_call(call)) {
                     if (pipe.limit IS 0 and pipe.lhs->static_results) {
                        const StaticResultSet &input = this->catalogue_.results(pipe.lhs->static_results);
                        if (input.stored_count > 1) {
                           ParserDiagnostic diagnostic;
                           diagnostic.severity = ParserDiagnosticSeverity::Error;
                           diagnostic.code = ParserErrorCode::TypeMismatchArgument;
                           diagnostic.message = std::format("forEach expects exactly 2 arguments, got {}",
                              input.stored_count + call.arguments.size());
                           diagnostic.token = Token::from_span(pipe.rhs_call->span, TokenKind::Identifier);
                           this->context_.diagnostics().report(diagnostic);
                        }
                     }
                     StaticValueDescriptor target = this->descriptor_of(*pipe.lhs);
                     this->preserve_for_each_result(call, *pipe.lhs);
                     pipe.rhs_call->static_value = this->add_value(target);
                     pipe.rhs_call->static_results = call.results;
                  }
               }
               value = this->descriptor_of(*pipe.rhs_call);
               results = pipe.rhs_call->static_results;
            }
            break;
         }
         case AstNodeKind::IndexExpr:
         case AstNodeKind::SafeIndexExpr: {
            ExprNode *base = nullptr;
            ExprNode *index = nullptr;
            bool safe = Expression.kind IS AstNodeKind::SafeIndexExpr;
            if (safe) {
               auto &payload = std::get<SafeIndexExprPayload>(Expression.data);
               base = payload.table.get();
               index = payload.index.get();
            }
            else {
               auto &payload = std::get<IndexExprPayload>(Expression.data);
               base = payload.table.get();
               index = payload.index.get();
            }
            if (base) {
               StaticValueDescriptor base_value = this->descriptor_of(*base);
               if (index and index->kind IS AstNodeKind::RangeExpr and
                   (base_value.primary IS TiriType::Table or
                    base_value.contextuality != StaticContextuality::Unknown)) {
                  value = base_value;
                  value.primary = TiriType::Table;
                  value.nullable = safe;
               }
               else if (index and index->kind IS AstNodeKind::RangeExpr and
                        base_value.primary IS TiriType::Str) {
                  value = base_value;
                  value.nullable = safe;
               }
               else if (base_value.primary IS TiriType::Array and base_value.array_element.known and
                   base_value.proved()) {
                  value = this->array_element_value(base_value.array_element);
                  value.nullable = value.nullable or safe;
               }
            }
            break;
         }
         case AstNodeKind::MemberExpr:
         case AstNodeKind::SafeMemberExpr: {
            ExprNode *base = nullptr;
            GCstr *member = nullptr;
            bool safe = Expression.kind IS AstNodeKind::SafeMemberExpr;
            if (safe) {
               auto &payload = std::get<SafeMemberExprPayload>(Expression.data);
               base = payload.table.get();
               member = payload.member.symbol;
            }
            else {
               auto &payload = std::get<MemberExprPayload>(Expression.data);
               base = payload.table.get();
               member = payload.member.symbol;
            }
            if (base) value = this->member_descriptor(this->descriptor_of(*base), member);
            if (safe) value.nullable = true;
            break;
         }
         case AstNodeKind::BinaryExpr: {
            auto &payload = std::get<BinaryExprPayload>(Expression.data);
            if ((payload.op IS AstBinaryOperator::LogicalAnd or payload.op IS AstBinaryOperator::LogicalOr or
                 payload.op IS AstBinaryOperator::IfEmpty) and payload.left and payload.right) {
               value = join_static_descriptors(
                  this->descriptor_of(*payload.left), this->descriptor_of(*payload.right));
            }
            else if ((payload.op IS AstBinaryOperator::Add or payload.op IS AstBinaryOperator::Subtract or
                      payload.op IS AstBinaryOperator::Multiply or payload.op IS AstBinaryOperator::Divide or
                      payload.op IS AstBinaryOperator::Modulo or payload.op IS AstBinaryOperator::Power) and
                     payload.left and payload.right) {
               value = describe_arithmetic_result(
                  this->descriptor_of(*payload.left), this->descriptor_of(*payload.right));
            }
            else {
               TiriType inferred = infer_expression_type(Expression);
               if (inferred != TiriType::Unknown) {
                  value.primary = inferred;
                  value.proof = StaticProof::Advisory;
               }
            }
            break;
         }
         case AstNodeKind::UnaryExpr: {
            auto &payload = std::get<UnaryExprPayload>(Expression.data);
            if (payload.op IS AstUnaryOperator::Length and payload.operand) {
               value = describe_length_result(this->descriptor_of(*payload.operand));
            }
            else if (payload.op IS AstUnaryOperator::Negate and payload.operand) {
               value = describe_unary_numeric_result(this->descriptor_of(*payload.operand));
            }
            else {
               TiriType inferred = infer_expression_type(Expression);
               if (inferred != TiriType::Unknown) {
                  value.primary = inferred;
                  value.proof = payload.op IS AstUnaryOperator::Not ?
                     StaticProof::Closed : StaticProof::Advisory;
               }
            }
            break;
         }
         case AstNodeKind::TernaryExpr: {
            auto &payload = std::get<TernaryExprPayload>(Expression.data);
            if (payload.if_true and payload.if_false) {
               value = join_static_descriptors(
                  this->descriptor_of(*payload.if_true), this->descriptor_of(*payload.if_false));
            }
            break;
         }
         case AstNodeKind::ChooseExpr: {
            bool have_value = false;
            for (auto &choice : std::get<ChooseExprPayload>(Expression.data).cases) {
               if (not choice.result) continue;
               if (not have_value) {
                  value = this->descriptor_of(*choice.result);
                  have_value = true;
               }
               else value = join_static_descriptors(value, this->descriptor_of(*choice.result));
            }
            break;
         }
         case AstNodeKind::PresenceExpr:
         case AstNodeKind::ComparisonChainExpr:
            value.primary = TiriType::Bool;
            value.proof = StaticProof::Closed;
            break;
         default: {
            TiriType inferred = infer_expression_type(Expression);
            if (inferred != TiriType::Unknown) {
               value.primary = inferred;
               value.proof = StaticProof::Advisory;
            }
            break;
         }
      }

      if (value.primary != TiriType::Unknown or value.array_element.known) {
         Expression.static_value = this->add_value(value);
      }
      Expression.static_results = results;
   }

   void set_binding_value(Identifier &Name, const ExprNode *Initialiser, uint8_t ResultPosition)
   {
      if (not Name.binding_id) return;
      auto &binding = this->catalogue_.binding(Name.binding_id);
      StaticValueDescriptor value;

      if (Name.type != TiriType::Unknown and Name.type != TiriType::Any) {
         value = this->declared_descriptor(
            Name.type, Name.struct_def, Name.array_element, StaticProof::Checked);
         if (Initialiser) value.contextuality = this->descriptor_of(*Initialiser).contextuality;
      }
      else if (binding.function) {
         value.primary = TiriType::Func;
         value.proof = StaticProof::Closed;
      }
      else if (Initialiser) {
         if (ResultPosition > 0 and Initialiser->static_results) {
            value = this->catalogue_.results(Initialiser->static_results).value_at(ResultPosition);
         }
         else value = this->descriptor_of(*Initialiser);
      }

      // An explicit `any` binding may later receive a different table identity, including one designated elsewhere.
      // Preserve useful value-shape information but never bake the initial table's contextuality into member calls.
      if (Name.type IS TiriType::Any) value.contextuality = StaticContextuality::Unknown;

      if (value.module and not binding.immutable) value.module = nullptr;
      if (not binding.immutable) binding.callable = {};
      binding.value = this->add_value(value);
      Name.static_value = binding.value;
   }

   void update_assigned_binding(ExprNode &Target, const StaticValueDescriptor &Assigned)
   {
      if (Target.kind != AstNodeKind::IdentifierExpr) return;
      auto &reference = std::get<NameRef>(Target.data);
      if (not reference.binding_id) return;

      auto &binding = this->catalogue_.binding(reference.binding_id);
      StaticValueDescriptor previous = this->catalogue_.value(binding.value);
      StaticValueDescriptor joined = previous.primary IS TiriType::Unknown and not previous.array_element.known ?
         Assigned : join_static_descriptors(previous, Assigned);
      binding.value = this->add_value(joined);
      binding.callable = {};
      reference.identifier.static_value = binding.value;
      Target.static_value = binding.value;

      lj_assertX(Assigned.proved() or not this->catalogue_.value(binding.value).proved(),
         "an advisory assignment retained a proved binding descriptor");
   }

   [[nodiscard]] StaticValueDescriptor compound_assignment_descriptor(
      AssignmentOperator Operator, const ExprNode &Target, const ExprNode &Value) const
   {
      StaticValueDescriptor current = this->descriptor_of(Target);
      StaticValueDescriptor operand = this->descriptor_of(Value);
      switch (Operator) {
         case AssignmentOperator::Add:
         case AssignmentOperator::Subtract:
         case AssignmentOperator::Multiply:
         case AssignmentOperator::Divide:
         case AssignmentOperator::Modulo:
            return describe_arithmetic_result(current, operand);

         case AssignmentOperator::Concat:
            if (current.primary IS TiriType::Array and current.proved() and not current.nullable) return current;
            return StaticValueDescriptor{
               .primary = current.primary,
               .proof = StaticProof::Advisory,
               .nullable = current.nullable or operand.nullable
            };

         default:
            return {};
      }
   }

   void propagate_function(
      FunctionExprPayload &Function, std::span<const StaticValueDescriptor> ContextParameters = {})
   {
      for (size_t i = 0; i < Function.parameters.size(); ++i) {
         auto &parameter = Function.parameters[i];
         if (not parameter.name.binding_id) continue;
         StaticValueDescriptor value;
         if (not parameter.type_is_explicit and i < ContextParameters.size()) {
            value = ContextParameters[i];
         }
         else {
            value = this->declared_descriptor(parameter.type, parameter.struct_def, parameter.array_element,
               parameter.type IS TiriType::Any ? StaticProof::Advisory : StaticProof::Checked);
            value.nullable = not parameter.required;
         }
         auto &binding = this->catalogue_.binding(parameter.name.binding_id);
         binding.value = this->add_value(value);
         parameter.name.static_value = binding.value;
      }
      if (Function.body) {
         // Ownership is a property of the function performing the designation, so the proof must be evaluated against
         // this body and this depth rather than the module's.
         const BlockStmt *outer_body = this->enclosing_body_;
         this->enclosing_body_ = Function.body.get();
         this->function_depth_++;
         this->propagate_block(*Function.body);
         this->function_depth_--;
         this->enclosing_body_ = outer_body;
      }

      StaticResultSet inferred;
      bool have_return = false;
      if (Function.body) this->collect_return_descriptors(*Function.body, inferred, have_return);
      if (have_return) {
         if (Function.return_types.is_explicit or Function.return_types.is_inferred) {
            inferred.declared_count = Function.return_types.count;
            inferred.stored_count = uint8_t(std::min<size_t>(
               Function.return_types.count, MAX_RETURN_TYPES));
            inferred.variadic = Function.return_types.is_variadic;
            inferred.dynamic = false;
            for (size_t i = 0; i < inferred.stored_count; ++i) {
               auto &value = inferred.values[i];
               value.primary = Function.return_types.types[i];
               value.object_class_id = Function.return_types.object_class_ids[i];
               value.struct_def = Function.return_types.struct_defs[i] ?
                  Function.return_types.struct_defs[i] : value.struct_def;
               if (Function.return_types.array_elements[i].known) {
                  value.array_element = Function.return_types.array_elements[i];
               }
               value.nullable = not Function.return_types.required[i];
               value.proof = Function.return_types.is_explicit ?
                  (value.primary IS TiriType::Any ? StaticProof::Advisory : StaticProof::Checked) :
                  StaticProof::Closed;
               Function.return_types.descriptors[i] = this->add_value(value);
            }
         }
         if (Function.callable) {
            this->catalogue_.callable(Function.callable).results = this->catalogue_.add_results(inferred);
         }
      }
   }

   void collect_return_descriptors(
      BlockStmt &Block, StaticResultSet &Results, bool &HaveReturn)
   {
      for (auto &statement : Block.statements) {
         if (not statement) continue;
         if (statement->kind IS AstNodeKind::ReturnStmt) {
            auto &payload = std::get<ReturnStmtPayload>(statement->data);
            StaticResultSet current;
            current.declared_count = uint16_t(payload.values.size());
            current.stored_count = uint8_t(std::min<size_t>(payload.values.size(), MAX_RETURN_TYPES));
            for (size_t i = 0; i < current.stored_count; ++i) {
               current.values[i] = this->descriptor_of(*payload.values[i]);
            }
            if (not payload.values.empty()) {
               ExprNode &last = *payload.values.back();
               if (last.static_results) {
                  const auto &tail = this->catalogue_.results(last.static_results);
                  size_t fixed = payload.values.size() - 1;
                  current.declared_count = uint16_t(fixed + tail.declared_count);
                  current.dynamic = tail.dynamic;
                  current.variadic = tail.variadic;
                  for (size_t i = fixed; i < MAX_RETURN_TYPES; ++i) {
                     current.values[i] = tail.value_at(i - fixed);
                  }
                  current.stored_count = uint8_t(std::min<size_t>(
                     MAX_RETURN_TYPES, fixed + tail.stored_count));
               }
            }

            if (not HaveReturn) {
               Results = current;
               HaveReturn = true;
            }
            else {
               size_t count = std::max<size_t>(Results.stored_count, current.stored_count);
               for (size_t i = 0; i < count; ++i) {
                  Results.values[i] = join_static_descriptors(
                     Results.value_at(i), current.value_at(i));
               }
               Results.stored_count = uint8_t(count);
               Results.declared_count = std::max(Results.declared_count, current.declared_count);
               Results.dynamic = Results.dynamic or current.dynamic;
               Results.variadic = Results.variadic and current.variadic;
            }
            continue;
         }

         this->walk_statement_blocks(*statement, [this, &Results, &HaveReturn](BlockStmt &Nested) {
            this->collect_return_descriptors(Nested, Results, HaveReturn);
         });
      }
   }

   void propagate_block(BlockStmt &Block)
   {
      for (auto &statement : Block.statements) {
         if (statement) this->propagate_statement(*statement);
      }
   }

   void propagate_statement(StmtNode &Statement)
   {
      switch (Statement.kind) {
         case AstNodeKind::LocalDeclStmt: {
            auto &payload = std::get<LocalDeclStmtPayload>(Statement.data);
            for (auto &value : payload.values) if (value) this->propagate_expression(*value);
            for (size_t i = 0; i < payload.names.size(); ++i) {
               const ExprNode *initialiser = i < payload.values.size() ? payload.values[i].get() :
                  (payload.values.empty() ? nullptr : payload.values.back().get());
               // See the matching note in discover_statement(): a value-less declaration has no result position.
               uint8_t position = (i < payload.values.size() or payload.values.empty()) ? 0 :
                  uint8_t(i - payload.values.size() + 1);
               this->set_binding_value(payload.names[i], initialiser, position);
            }
            break;
         }
         case AstNodeKind::LocalFunctionStmt: {
            auto &payload = std::get<LocalFunctionStmtPayload>(Statement.data);
            this->set_binding_value(payload.name, nullptr, 0);
            if (payload.function) this->propagate_function(*payload.function);
            break;
         }
         case AstNodeKind::AssignmentStmt: {
            auto &payload = std::get<AssignmentStmtPayload>(Statement.data);
            for (auto &value : payload.values) if (value) this->propagate_expression(*value);
            for (auto &target : payload.targets) if (target) this->propagate_expression(*target);
            for (size_t i = 0; i < payload.targets.size() and not payload.values.empty(); ++i) {
               if (not payload.targets[i]) continue;
               size_t source = std::min(i, payload.values.size() - 1);
               this->reject_builtin_method_shadow(
                  *payload.targets[i], this->descriptor_of(*payload.values[source]));
            }
            if (payload.op IS AssignmentOperator::Plain or payload.op IS AssignmentOperator::IfEmpty or
                payload.op IS AssignmentOperator::IfNil) {
               for (size_t i = 0; i < payload.targets.size(); ++i) {
                  if (not payload.targets[i] or payload.values.empty()) continue;
                  size_t source = std::min(i, payload.values.size() - 1);
                  bool initialises_binding = false;
                  if (payload.targets[i]->kind IS AstNodeKind::IdentifierExpr) {
                     const auto &reference = std::get<NameRef>(payload.targets[i]->data);
                     if (reference.binding_id) {
                        const auto &binding = this->catalogue_.binding(reference.binding_id);
                        initialises_binding = binding.initialiser IS payload.values[source].get();
                     }
                  }
                  StaticValueDescriptor assigned;
                  if (source IS payload.values.size() - 1 and i >= payload.values.size() and
                      payload.values[source]->static_results) {
                     assigned = this->catalogue_.results(
                        payload.values[source]->static_results).value_at(i - source);
                  }
                  else assigned = this->descriptor_of(*payload.values[source]);
                  if (initialises_binding) {
                     auto &reference = std::get<NameRef>(payload.targets[i]->data);
                     auto &binding = this->catalogue_.binding(reference.binding_id);
                     if (assigned.module and not binding.immutable) assigned.module = nullptr;
                     binding.value = this->add_value(assigned);
                     binding.callable = {};
                     reference.identifier.static_value = binding.value;
                     payload.targets[i]->static_value = binding.value;
                  }
                  else this->update_assigned_binding(*payload.targets[i], assigned);
               }
            }
            else if (payload.op IS AssignmentOperator::Add or payload.op IS AssignmentOperator::Subtract or
                     payload.op IS AssignmentOperator::Multiply or payload.op IS AssignmentOperator::Divide or
                     payload.op IS AssignmentOperator::Modulo or payload.op IS AssignmentOperator::Concat) {
               if (payload.targets.size() IS 1 and payload.values.size() IS 1 and
                   payload.targets.front() and payload.values.front()) {
                  StaticValueDescriptor assigned = this->compound_assignment_descriptor(
                     payload.op, *payload.targets.front(), *payload.values.front());
                  this->update_assigned_binding(*payload.targets.front(), assigned);
               }
            }
            break;
         }
         case AstNodeKind::FunctionStmt: {
            auto &payload = std::get<FunctionStmtPayload>(Statement.data);
            if (payload.name.segments.size() IS 1) {
               this->set_binding_value(payload.name.segments.front(), nullptr, 0);
            }
            if (payload.function) this->propagate_function(*payload.function);
            break;
         }
         case AstNodeKind::IfStmt: {
            auto &payload = std::get<IfStmtPayload>(Statement.data);
            for (auto &clause : payload.clauses) {
               if (clause.condition) {
                  this->propagate_expression(*clause.condition);
                  auto truth = this->constant_truth(*clause.condition);
                  if (truth and not *truth) continue;
                  if (clause.block) this->propagate_block(*clause.block);
                  if (truth and *truth) break;
               }
               else {
                  if (clause.block) this->propagate_block(*clause.block);
                  break;
               }
            }
            break;
         }
         case AstNodeKind::GlobalDeclStmt: {
            auto &payload = std::get<GlobalDeclStmtPayload>(Statement.data);
            for (auto &value : payload.values) if (value) this->propagate_expression(*value);
            for (size_t i = 0; i < payload.names.size(); ++i) {
               auto &name = payload.names[i];
               if (name.type IS TiriType::Any or not name.symbol) continue;
               StaticValueDescriptor value;
               if (name.type != TiriType::Unknown) {
                  value = this->declared_descriptor(
                     name.type, name.struct_def, name.array_element, StaticProof::Checked);
               }
               else if (not payload.values.empty()) {
                  size_t source = std::min(i, payload.values.size() - 1);
                  value = this->descriptor_of(*payload.values[source]);
                  if (source IS payload.values.size() - 1 and i >= payload.values.size() and
                      payload.values[source]->static_results) {
                     value = this->catalogue_.results(
                        payload.values[source]->static_results).value_at(i - source);
                  }
               }
               if (value.module and not name.has_const) value.module = nullptr;
               if (value.primary IS TiriType::Unknown or value.primary IS TiriType::Any) continue;
               name.static_value = this->add_value(value);
               this->global_values_.emplace_back(name.symbol, name.static_value);
            }
            break;
         }
         case AstNodeKind::GenericForStmt: {
            auto &payload = std::get<GenericForStmtPayload>(Statement.data);
            for (auto &iterator : payload.iterators) if (iterator) this->propagate_expression(*iterator);
            if (payload.iterators.size() IS 1 and not payload.names.empty()) {
               StaticValueDescriptor iterator = this->descriptor_of(*payload.iterators.front());
               if (iterator.proved() and iterator.primary IS TiriType::Func and
                   (not iterator.nullable or payload.iterators.front()->kind IS AstNodeKind::CallExpr)) {
                  payload.target = GenericForTarget::IteratorProtocol;
               }
               else if (iterator.proved() and not iterator.nullable and iterator.primary IS TiriType::Table) {
                  payload.target = GenericForTarget::BareTable;
               }
               else payload.target = GenericForTarget::BareTarget;
            }
            else payload.target = GenericForTarget::IteratorProtocol;
            if (payload.body) this->propagate_block(*payload.body);
            break;
         }
         case AstNodeKind::DeferStmt: {
            auto &payload = std::get<DeferStmtPayload>(Statement.data);
            for (auto &argument : payload.arguments) {
               if (argument) this->propagate_expression(*argument);
            }
            if (payload.callable) this->propagate_function(*payload.callable);
            break;
         }
         default:
            this->walk_statement_expressions(Statement, [this](ExprNode &Expression) {
               this->propagate_expression(Expression);
            });
            this->walk_statement_blocks(Statement, [this](BlockStmt &Nested) {
               this->propagate_block(Nested);
            });
            break;
      }
   }

   template<typename Visitor>
   void walk_expression_children(ExprNode &Expression, Visitor Visit)
   {
      auto visit = [&](ExprNodePtr &Node) { if (Node) Visit(*Node); };
      switch (Expression.kind) {
         case AstNodeKind::UnaryExpr: visit(std::get<UnaryExprPayload>(Expression.data).operand); break;
         case AstNodeKind::UpdateExpr: visit(std::get<UpdateExprPayload>(Expression.data).target); break;
         case AstNodeKind::TypeTestExpr: visit(std::get<TypeTestExprPayload>(Expression.data).value); break;
         case AstNodeKind::BinaryExpr: {
            auto &p = std::get<BinaryExprPayload>(Expression.data); visit(p.left); visit(p.right); break;
         }
         case AstNodeKind::ComparisonChainExpr:
            for (auto &v : std::get<ComparisonChainExprPayload>(Expression.data).operands) visit(v);
            break;
         case AstNodeKind::TernaryExpr: {
            auto &p = std::get<TernaryExprPayload>(Expression.data);
            visit(p.condition); visit(p.if_true); visit(p.if_false); break;
         }
         case AstNodeKind::PresenceExpr: visit(std::get<PresenceExprPayload>(Expression.data).value); break;
         case AstNodeKind::PipeExpr: {
            auto &p = std::get<PipeExprPayload>(Expression.data); visit(p.lhs); visit(p.rhs_call); break;
         }
         case AstNodeKind::CallExpr:
         case AstNodeKind::SafeCallExpr: {
            auto &p = std::get<CallExprPayload>(Expression.data);
            if (auto *d = std::get_if<DirectCallTarget>(&p.target)) visit(d->callable);
            for (auto &v : p.arguments) visit(v);
            break;
         }
         case AstNodeKind::MemberExpr: visit(std::get<MemberExprPayload>(Expression.data).table); break;
         case AstNodeKind::IndexExpr: {
            auto &p = std::get<IndexExprPayload>(Expression.data); visit(p.table); visit(p.index); break;
         }
         case AstNodeKind::SafeMemberExpr: visit(std::get<SafeMemberExprPayload>(Expression.data).table); break;
         case AstNodeKind::SafeIndexExpr: {
            auto &p = std::get<SafeIndexExprPayload>(Expression.data); visit(p.table); visit(p.index); break;
         }
         case AstNodeKind::ResultFilterExpr: visit(std::get<ResultFilterPayload>(Expression.data).expression); break;
         case AstNodeKind::TableExpr:
            for (auto &f : std::get<TableExprPayload>(Expression.data).fields) { visit(f.key); visit(f.value); }
            break;
         case AstNodeKind::DeferredExpr: visit(std::get<DeferredExprPayload>(Expression.data).inner); break;
         case AstNodeKind::RangeExpr: {
            auto &p = std::get<RangeExprPayload>(Expression.data); visit(p.start); visit(p.stop); visit(p.step); break;
         }
         case AstNodeKind::ChooseExpr: {
            auto &p = std::get<ChooseExprPayload>(Expression.data);
            visit(p.scrutinee);
            for (auto &v : p.scrutinee_tuple) visit(v);
            for (auto &c : p.cases) {
               visit(c.pattern);
               for (auto &v : c.tuple_patterns) visit(v);
               visit(c.guard); visit(c.result);
            }
            break;
         }
         default: break;
      }
   }

   template<typename Visitor>
   void walk_statement_expressions(StmtNode &Statement, Visitor Visit)
   {
      auto visit = [&](ExprNodePtr &Node) { if (Node) Visit(*Node); };
      switch (Statement.kind) {
         case AstNodeKind::AssignmentStmt: {
            auto &p = std::get<AssignmentStmtPayload>(Statement.data);
            for (auto &v : p.values) visit(v);
            for (auto &v : p.targets) visit(v);
            break;
         }
         case AstNodeKind::GlobalDeclStmt:
            for (auto &v : std::get<GlobalDeclStmtPayload>(Statement.data).values) visit(v);
            break;
         case AstNodeKind::IfStmt:
            for (auto &c : std::get<IfStmtPayload>(Statement.data).clauses) visit(c.condition);
            break;
         case AstNodeKind::WhileStmt:
         case AstNodeKind::RepeatStmt:
            visit(std::get<LoopStmtPayload>(Statement.data).condition);
            break;
         case AstNodeKind::NumericForStmt: {
            auto &p = std::get<NumericForStmtPayload>(Statement.data);
            visit(p.start);
            visit(p.stop);
            visit(p.step);
            break;
         }
         case AstNodeKind::RangeForStmt: {
            auto &p = std::get<RangeForStmtPayload>(Statement.data);
            visit(p.start);
            visit(p.stop);
            visit(p.step);
            break;
         }
         case AstNodeKind::GenericForStmt:
            for (auto &v : std::get<GenericForStmtPayload>(Statement.data).iterators) visit(v);
            break;
         case AstNodeKind::ReturnStmt:
            for (auto &v : std::get<ReturnStmtPayload>(Statement.data).values) visit(v);
            break;
         case AstNodeKind::DeferStmt:
            for (auto &v : std::get<DeferStmtPayload>(Statement.data).arguments) visit(v);
            break;
         case AstNodeKind::ConditionalShorthandStmt:
            visit(std::get<ConditionalShorthandStmtPayload>(Statement.data).condition);
            break;
         case AstNodeKind::TryExceptStmt:
            for (auto &c : std::get<TryExceptPayload>(Statement.data).except_clauses) {
               for (auto &v : c.filter_codes) visit(v);
            }
            break;
         case AstNodeKind::CheckallStmt:
            break;
         case AstNodeKind::RaiseStmt: {
            auto &p = std::get<RaiseStmtPayload>(Statement.data); visit(p.error_code); visit(p.message); break;
         }
         case AstNodeKind::CheckStmt: visit(std::get<CheckStmtPayload>(Statement.data).error_code); break;
         case AstNodeKind::WithStmt:
            for (auto &v : std::get<WithStmtPayload>(Statement.data).objects) visit(v);
            break;
         case AstNodeKind::ContextStmt:
            visit(std::get<ContextStmtPayload>(Statement.data).reference);
            break;
         case AstNodeKind::ExpressionStmt: visit(std::get<ExpressionStmtPayload>(Statement.data).expression); break;
         default: break;
      }
   }

   template<typename Visitor>
   void walk_statement_blocks(StmtNode &Statement, Visitor Visit)
   {
      auto visit = [&](std::unique_ptr<BlockStmt> &Block) { if (Block) Visit(*Block); };
      switch (Statement.kind) {
         case AstNodeKind::IfStmt:
            for (auto &c : std::get<IfStmtPayload>(Statement.data).clauses) visit(c.block);
            break;
         case AstNodeKind::WhileStmt:
         case AstNodeKind::RepeatStmt: visit(std::get<LoopStmtPayload>(Statement.data).body); break;
         case AstNodeKind::NumericForStmt: visit(std::get<NumericForStmtPayload>(Statement.data).body); break;
         case AstNodeKind::RangeForStmt: visit(std::get<RangeForStmtPayload>(Statement.data).body); break;
         case AstNodeKind::GenericForStmt: visit(std::get<GenericForStmtPayload>(Statement.data).body); break;
         case AstNodeKind::DoStmt: visit(std::get<DoStmtPayload>(Statement.data).block); break;
         case AstNodeKind::ContextStmt: visit(std::get<ContextStmtPayload>(Statement.data).block); break;
         case AstNodeKind::TryExceptStmt: {
            auto &p = std::get<TryExceptPayload>(Statement.data);
            visit(p.try_block);
            for (auto &c : p.except_clauses) visit(c.block);
            visit(p.success_block);
            break;
         }
         case AstNodeKind::CheckallStmt:
            visit(std::get<CheckallStmtPayload>(Statement.data).block);
            break;
         case AstNodeKind::ImportStmt:
            for (auto &e : std::get<ImportStmtPayload>(Statement.data).entries) visit(e.inlined_body);
            break;
         case AstNodeKind::WithStmt: visit(std::get<WithStmtPayload>(Statement.data).block); break;
         default: break;
      }
   }

   template<typename Visitor>
   void walk_block_expressions(BlockStmt &Block, Visitor Visit)
   {
      for (auto &statement : Block.statements) {
         if (not statement) continue;
         if (statement->kind IS AstNodeKind::LocalDeclStmt) {
            for (auto &v : std::get<LocalDeclStmtPayload>(statement->data).values) if (v) Visit(*v);
         }
         else if (statement->kind IS AstNodeKind::LocalFunctionStmt) {
            auto &p = std::get<LocalFunctionStmtPayload>(statement->data);
            if (p.function and p.function->body) this->walk_block_expressions(*p.function->body, Visit);
         }
         else if (statement->kind IS AstNodeKind::FunctionStmt) {
            auto &p = std::get<FunctionStmtPayload>(statement->data);
            if (p.function and p.function->body) this->walk_block_expressions(*p.function->body, Visit);
         }
         else if (statement->kind IS AstNodeKind::DeferStmt) {
            auto &p = std::get<DeferStmtPayload>(statement->data);
            for (auto &argument : p.arguments) if (argument) Visit(*argument);
            if (p.callable and p.callable->body) this->walk_block_expressions(*p.callable->body, Visit);
         }
         else {
            this->walk_statement_expressions(*statement, Visit);
            this->walk_statement_blocks(*statement, [this, &Visit](BlockStmt &Nested) {
               this->walk_block_expressions(Nested, Visit);
            });
         }
      }
   }

   ParserContext &context_;
   StaticDescriptorCatalogue &catalogue_;
   std::vector<BindingScope> scopes_;
   std::vector<std::pair<GCstr *, StaticValueHandle>> global_values_;
   std::vector<GCstr *> global_names_;
   const BlockStmt *enclosing_body_ = nullptr;
   uint16_t function_depth_ = 0;
   bool native_calls_only_ = false;
};

} // namespace

void discover_static_bindings(ParserContext &Context, BlockStmt &Module)
{
   StaticDescriptorAnalyser analyser(Context);
   analyser.discover(Module);
}

void propagate_static_descriptors(ParserContext &Context, BlockStmt &Module)
{
   StaticDescriptorAnalyser analyser(Context);
   analyser.propagate(Module);
}

void propagate_native_object_descriptors(ParserContext &Context, BlockStmt &Module)
{
   StaticDescriptorAnalyser analyser(Context, true);
   analyser.propagate(Module);
}
