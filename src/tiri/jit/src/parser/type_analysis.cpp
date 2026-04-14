// Type Analysis for Tiri Parser
// Copyright © 2025-2026 Paul Manias
//
// This module performs semantic type analysis on the Tiri AST after parsing.  It implements:
//
// - Type inference for local variables and function returns
// - Type checking for assignments and function arguments
// - Return type validation within functions
// - Detection of recursive functions requiring explicit type declarations
// - Scope-based variable tracking for unused variable detection
// - Shadowing detection for variables in nested scopes
//
// The analysis is non-blocking by default - type mismatches generate warnings unless the parser is configured with
// type_errors_are_fatal = true.

#include "parser/type_checker.h"

#include <format>

#include <ankerl/unordered_dense.h>
#include <kotuku/main.h>
#include "parser/parser_context.h"

#ifdef INCLUDE_TIPS
#include "parser/parser_tips.h"
#endif

// Infer the type of a literal value (nil, boolean, number, string).  Literal types are always marked as
// constant since their values cannot change.

[[nodiscard]] static InferredType infer_literal_type(const LiteralValue &Literal)
{
   InferredType result;
   result.is_constant = true;

   switch (Literal.kind) {
      case LiteralKind::Nil:
         result.primary = TiriType::Nil;
         result.is_nullable = true;
         break;
      case LiteralKind::Boolean: result.primary = TiriType::Bool; break;
      case LiteralKind::Number:  result.primary = TiriType::Num; break;
      case LiteralKind::String:  result.primary = TiriType::Str; break;
   }
   return result;
}

// Helper to check if type tracing is enabled

[[nodiscard]] inline bool should_trace_types(lua_State *L)
{
   auto prv = (prvTiri *)L->script->ChildPrivate;
   return (prv->JitOptions & JOF::TRACE_TYPES) != JOF::NIL;
}

//********************************************************************************************************************
// TypeAnalyser - Main class for performing semantic type analysis on Tiri AST.
//
// The analyser walks the AST and performs:
// 1. Type inference - Determines types for variables without explicit annotations
// 2. Type checking - Validates type compatibility for assignments and function calls
// 3. Return validation - Ensures consistent return types within functions
// 4. Usage tracking - Detects unused variables and parameters (for tip)
// 5. Shadowing detection - Warns when inner scope variables shadow outer ones
//
// The analyser maintains a scope stack to track variable declarations and types as it traverses nested blocks,
// functions, and control structures.

class TypeAnalyser {
public:
   explicit TypeAnalyser(ParserContext &Context) : ctx_(Context) {}

   // Entry point: analyse an entire module (top-level block)
   void analyse_module(const BlockStmt &);

   // Access collected type diagnostics after analysis
   [[nodiscard]] const std::vector<TypeDiagnostic> & diagnostics() const { return this->diagnostics_; }

private:
   // Scope management - maintains a stack of TypeCheckScope for tracking variable types
   void push_scope();
   void pop_scope();
   [[nodiscard]] TypeCheckScope & current_scope();
   [[nodiscard]] const TypeCheckScope & current_scope() const;

   // Function context management - tracks return type expectations for nested functions
   void enter_function(const FunctionExprPayload &, GCstr *Name = nullptr);
   void leave_function();
   [[nodiscard]] FunctionContext* current_function();
   [[nodiscard]] const FunctionContext* current_function() const;

   // AST traversal methods - recursively analyse each node type
   void analyse_block(const BlockStmt &);
   void analyse_statement(const StmtNode &);
   void analyse_assignment(const AssignmentStmtPayload &);
   void analyse_local_decl(const LocalDeclStmtPayload &);
   void analyse_global_decl(const GlobalDeclStmtPayload &);
   void analyse_local_function(const LocalFunctionStmtPayload &);
   void analyse_function_stmt(const FunctionStmtPayload &);
   void analyse_function_payload(const FunctionExprPayload &, GCstr *Name = nullptr);
   void analyse_expression(const ExprNode &);
   void analyse_call_expr(const CallExprPayload &);

   // Argument type checking for function calls
   void check_arguments(const FunctionExprPayload &, const CallExprPayload &);
   void check_argument_type(const ExprNode &, TiriType, size_t);

   // Type inference - determines types from expressions and context
   [[nodiscard]] InferredType infer_expression_type(const ExprNode &);
   [[nodiscard]] InferredType infer_call_return_type(const ExprNode &, size_t Position) const;

   // Symbol resolution - looks up variables and functions in scope stack
   [[nodiscard]] std::optional<InferredType> resolve_identifier(GCstr *) const;
   [[nodiscard]] const FunctionExprPayload * resolve_call_target(const CallTarget &) const;
   [[nodiscard]] const FunctionExprPayload * resolve_function(GCstr *) const;

   // Const checking - checks if a local variable has <const> attribute
   [[nodiscard]] bool is_local_const(GCstr *) const;

   // Type fixation - locks variable type after first concrete assignment
   void fix_local_type(GCstr *, TiriType, CLASSID ObjectClassId = CLASSID::NIL);

   // Usage tracking - marks variables as used for unused variable detection
   void mark_identifier_used(GCstr *);

   // Return type validation - ensures consistency within functions
   void validate_return_types(const ReturnStmtPayload &, SourceSpan);

   // Recursive function detection - identifies functions that call themselves
   [[nodiscard]] bool is_recursive_function(const FunctionExprPayload &, GCstr *) const;
   [[nodiscard]] bool function_has_return_values(const FunctionExprPayload &) const;
   [[nodiscard]] bool body_has_return_values(const BlockStmt &) const;
   [[nodiscard]] bool body_contains_call_to(const BlockStmt &, GCstr *) const;
   [[nodiscard]] bool statement_contains_call_to(const StmtNode &, GCstr *) const;
   [[nodiscard]] bool expression_contains_call_to(const ExprNode &, GCstr *) const;

   // Debug tracing - outputs type inference steps when TRACE_TYPES is enabled
   [[nodiscard]] bool trace_enabled() const { return should_trace_types(&this->ctx_.lua()); }
   void trace_infer(BCLine Line, std::string_view Context, TiriType Type) const;
   void trace_fix(BCLine Line, GCstr* Name, TiriType Type) const;
   void trace_decl(BCLine Line, GCstr* Name, TiriType Type, bool IsFixed) const;

   // Shadowing detection - warns when inner variable shadows outer scope variable
   #ifdef INCLUDE_TIPS
   void check_shadowing(GCstr *Name, SourceSpan Location);

   // Global access in loop detection - warns when globals are accessed in loops without caching
   void check_global_in_loop(GCstr *Name, SourceSpan Location);

   // Function definition in loop detection - warns when closures are created inside loops
   void check_function_in_loop(SourceSpan Location);

   // String concatenation in loop detection - warns when .. is used in loops
   void check_concat_in_loop(SourceSpan Location);

   // Track a global variable declaration
   void track_global(GCstr *Name);
   #endif

   // Global variable type tracking - stores type information for variables declared with 'global' keyword
   struct GlobalTypeInfo {
      InferredType type{};
      SourceSpan location{};
      const FunctionExprPayload* function = nullptr;  // Non-null if declared as global function
      bool is_const = false;  // True if declared with <const> attribute
   };

   void declare_global(GCstr *Name, const InferredType &Type, SourceSpan Location, bool IsConst = false);
   void declare_global_function(GCstr *Name, const FunctionExprPayload *Function, SourceSpan Location);
   [[nodiscard]] std::optional<InferredType> lookup_global_type(GCstr *Name) const;
   [[nodiscard]] bool is_global_const(GCstr *Name) const;
   void fix_global_type(GCstr *Name, TiriType Type, CLASSID ObjectClassId = CLASSID::NIL);

   ParserContext &ctx_;                             // Parser context for diagnostics and lexer access
   std::vector<TypeCheckScope> scope_stack_{};      // Stack of scopes for variable tracking
   std::vector<FunctionContext> function_stack_{};  // Stack of function contexts for return type tracking
   std::vector<TypeDiagnostic> diagnostics_{};      // Collected type errors and warnings
   uint32_t loop_depth_{0};                         // Current loop nesting depth for performance tip
   ankerl::unordered_dense::map<GCstr*, GlobalTypeInfo> global_types_{};  // Type info for global variables
   #ifdef INCLUDE_TIPS
   std::vector<GCstr*> declared_globals_{};         // Globals explicitly declared with 'global' keyword
   #endif
};

//********************************************************************************************************************
// Debug Tracing
//
// These methods output type inference steps to the log when --jit-options trace-types is enabled.
// Useful for debugging type inference logic and understanding how types are determined.

void TypeAnalyser::trace_infer(BCLine Line, std::string_view Context, TiriType Type) const
{
   if (not this->trace_enabled()) return;
   auto type_str = type_name(Type);
   pf::Log("TypeCheck").msg("[%d] infer %.*s -> %.*s", Line.lineNumber(), int(Context.size()), Context.data(),
      int(type_str.size()), type_str.data());
}

void TypeAnalyser::trace_fix(BCLine Line, GCstr* Name, TiriType Type) const
{
   if (not this->trace_enabled()) return;
   std::string_view name_view = Name ? std::string_view(strdata(Name), Name->len) : std::string_view("<unknown>");
   auto type_str = type_name(Type);
   pf::Log("TypeCheck").msg("[%d] fix '%.*s' -> %.*s", Line.lineNumber(), int(name_view.size()), name_view.data(),
      int(type_str.size()), type_str.data());
}

void TypeAnalyser::trace_decl(BCLine Line, GCstr* Name, TiriType Type, bool IsFixed) const
{
   if (not this->trace_enabled()) return;
   std::string_view name_view = Name ? std::string_view(strdata(Name), Name->len) : std::string_view("<unknown>");
   auto type_str = type_name(Type);
   pf::Log("TypeCheck").msg("[%d] decl '%.*s': %.*s%s", Line.lineNumber(), int(name_view.size()), name_view.data(),
      int(type_str.size()), type_str.data(), IsFixed ? " (fixed)" : "");
}

//********************************************************************************************************************
// Shadowing Detection
//
// Checks if declaring a variable would shadow a variable in an outer scope.  Only checks outer scopes (not the
// current scope) since redeclaration in the same scope is handled differently. Shadowing is a common source of bugs
// where the programmer accidentally uses a new variable instead of the intended outer one.
//
// The check skips blank identifiers (single underscore '_') which are intentionally used to discard values.

#ifdef INCLUDE_TIPS
void TypeAnalyser::check_shadowing(GCstr *Name, SourceSpan Location)
{
   if (not this->ctx_.should_emit_tip(2)) return;
   if (not Name) return;
   if (Name->len IS 1 and strdata(Name)[0] IS '_') return;

   // Check all scopes except the current one (outermost to second-to-last)
   // We iterate in reverse (innermost first) but skip the current scope
   if (this->scope_stack_.size() < 2) return;  // Need at least 2 scopes for shadowing

   for (size_t i = 0; i < this->scope_stack_.size() - 1; ++i) {
      const auto& scope = this->scope_stack_[i];
      auto existing = scope.lookup_local_type(Name);
      if (existing) {
         std::string_view name_view(strdata(Name), Name->len);
         this->ctx_.emit_tip(2, TipCategory::CodeQuality,
            std::format("Variable '{}' shadows a variable in an outer scope", name_view),
            Token::from_span(Location, TokenKind::Identifier));
         return;  // Only report once per variable
      }

      // Also check parameters in this scope
      auto param = scope.lookup_parameter_type(Name);
      if (param) {
         std::string_view name_view(strdata(Name), Name->len);
         this->ctx_.emit_tip(2, TipCategory::CodeQuality,
            std::format("Variable '{}' shadows a parameter in an outer scope", name_view),
            Token::from_span(Location, TokenKind::Identifier));
         return;
      }
   }
}

//********************************************************************************************************************
// Global Variable Access in Loop Detection:  Warns when global variables declared with the 'global' keyword are
// accessed within loops.  Accessing globals in tight loops incurs a performance penalty because each access requires
// a hash table lookup in the global environment table. For optimal JIT performance, globals should be cached in
// local variables before entering the loop.

void TypeAnalyser::track_global(GCstr *Name)
{
   if (not Name) return;
   // Avoid duplicates
   for (const auto* existing : this->declared_globals_) {
      if (existing IS Name) return;
   }
   this->declared_globals_.push_back(Name);
}

void TypeAnalyser::check_global_in_loop(GCstr *Name, SourceSpan Location)
{
   if (not this->ctx_.should_emit_tip(2)) return;
   if (this->loop_depth_ IS 0) return;
   if (not Name) return;
   if (Name->len IS 1 and strdata(Name)[0] IS '_') return;

   // Check if this identifier is a local or parameter in any scope
   for (const auto& scope : this->scope_stack_) {
      if (scope.lookup_local_type(Name)) return;  // Found as local
      if (scope.lookup_parameter_type(Name)) return;  // Found as parameter
   }

   // Only warn about globals that were explicitly declared in this script with 'global'
   bool is_declared_global = false;
   for (const auto* declared : this->declared_globals_) {
      if (declared IS Name) {
         is_declared_global = true;
         break;
      }
   }
   if (not is_declared_global) return;

   // It's a declared global variable being accessed inside a loop
   std::string_view name_view(strdata(Name), Name->len);
   this->ctx_.emit_tip(2, TipCategory::Performance,
      std::format("Global '{}' accessed in loop; consider caching in a local variable for better JIT performance",
         name_view),
      Token::from_span(Location, TokenKind::Identifier));
}

//********************************************************************************************************************
// Function Definition in Loop Detection: Warns when function expressions (closures) are defined inside loops.

void TypeAnalyser::check_function_in_loop(SourceSpan Location)
{
   if (not this->ctx_.should_emit_tip(2)) return;
   if (this->loop_depth_ IS 0) return;

   this->ctx_.emit_tip(2, TipCategory::Performance,
      "Function defined inside loop; consider moving it outside the loop for better performance",
      Token::from_span(Location, TokenKind::Function));
}

//********************************************************************************************************************
// String Concatenation in Loop Detection:  Warns when string concatenation (..) is used inside loops. Each
// concatenation creates a new intermediate string object, which is inefficient when building strings iteratively.
// For building strings in loops, array.join() is more efficient as it allocates only once.

void TypeAnalyser::check_concat_in_loop(SourceSpan Location)
{
   if (not this->ctx_.should_emit_tip(2)) return;
   if (this->loop_depth_ IS 0) return;

   this->ctx_.emit_tip(2, TipCategory::Performance,
      "String concatenation in loop; consider using array.join() for better performance",
      Token::from_span(Location, TokenKind::Cat));
}
#endif

//********************************************************************************************************************
// Scope Management
//
// Scopes are pushed when entering blocks (functions, loops, if statements, do blocks) and popped when leaving them.
// Each scope tracks its own local variables and their types.
//
// When a scope is popped, unused variable detection runs to identify variables that were declared but never
// referenced. This helps catch typos and dead code.

void TypeAnalyser::push_scope() {
   this->scope_stack_.emplace_back();
}

// Pop the current scope and report any unused variables.
// This is called when leaving a block, function, or control structure.

void TypeAnalyser::pop_scope() {
   if (this->scope_stack_.empty()) return;

   #ifdef INCLUDE_TIPS
   // Report unused variables before popping the scope (skip if tip wouldn't be emitted)
   if (this->ctx_.should_emit_tip(2)) {
      auto unused = this->scope_stack_.back().get_unused_variables();
      for (const auto& var : unused) {
         std::string_view name_view(strdata(var.name), var.name->len);
         if (var.is_parameter) {
            this->ctx_.emit_tip(2, TipCategory::CodeQuality,
               std::format("Unused function parameter '{}'", name_view),
               Token::from_span(var.location, TokenKind::Identifier));
         }
         else if (var.is_function) {
            this->ctx_.emit_tip(2, TipCategory::CodeQuality,
               std::format("Unused local function '{}'", name_view),
               Token::from_span(var.location, TokenKind::Identifier));
         }
         else {
            this->ctx_.emit_tip(2, TipCategory::CodeQuality,
               std::format("Unused local variable '{}'", name_view),
               Token::from_span(var.location, TokenKind::Identifier));
         }
      }
   }
   #endif

   this->scope_stack_.pop_back();
}

//********************************************************************************************************************

TypeCheckScope & TypeAnalyser::current_scope() {
   if (this->scope_stack_.empty()) this->push_scope();
   return this->scope_stack_.back();
}

//********************************************************************************************************************

const TypeCheckScope & TypeAnalyser::current_scope() const {
   lj_assertX(not this->scope_stack_.empty(), "type analysis scope stack is empty");
   return this->scope_stack_.back();
}

//********************************************************************************************************************
// Function Context Management
//
// When entering a function, we push a FunctionContext to track expected return types.  This enables validation of
// return statements against declared or inferred types.
//
// If the function has explicit return type annotations, those are used immediately.  Otherwise, the first return
// statement with non-nil values establishes the expected types (first-wins inference rule).

void TypeAnalyser::enter_function(const FunctionExprPayload &Function, GCstr *Name)
{
   FunctionContext ctx;
   ctx.function = &Function;
   ctx.function_name = Name;

   // If function has explicit return types, use them
   if (Function.return_types.is_explicit) {
      ctx.expected_returns = Function.return_types;
      ctx.return_type_inferred = true;  // Explicit types are considered "inferred" for validation purposes
   }

   this->function_stack_.push_back(ctx);
}

//********************************************************************************************************************

void TypeAnalyser::leave_function()
{
   if (not this->function_stack_.empty()) {
      this->function_stack_.pop_back();
   }
}

FunctionContext * TypeAnalyser::current_function()
{
   if (this->function_stack_.empty()) return nullptr;
   return &this->function_stack_.back();
}

const FunctionContext* TypeAnalyser::current_function() const
{
   if (this->function_stack_.empty()) return nullptr;
   return &this->function_stack_.back();
}

void TypeAnalyser::analyse_module(const BlockStmt &Module)
{
   this->push_scope();
   this->analyse_block(Module);
   this->pop_scope();
}

void TypeAnalyser::analyse_block(const BlockStmt &Block)
{
   for (const auto &statement : Block.view()) {
      this->analyse_statement(statement);
   }
}

//********************************************************************************************************************
// Statement Analysis
//
// Dispatches to the appropriate handler based on statement type.
// Each handler may push/pop scopes, declare variables, or analyse nested expressions.

void TypeAnalyser::analyse_statement(const StmtNode &Statement)
{
   switch (Statement.kind) {
      case AstNodeKind::AssignmentStmt: {
         auto *payload = std::get_if<AssignmentStmtPayload>(&Statement.data);
         if (payload) this->analyse_assignment(*payload);
         break;
      }
      case AstNodeKind::LocalDeclStmt: {
         auto *payload = std::get_if<LocalDeclStmtPayload>(&Statement.data);
         if (payload) this->analyse_local_decl(*payload);
         break;
      }
      case AstNodeKind::GlobalDeclStmt: {
         auto *payload = std::get_if<GlobalDeclStmtPayload>(&Statement.data);
         if (payload) this->analyse_global_decl(*payload);
         break;
      }
      case AstNodeKind::LocalFunctionStmt: {
         auto *payload = std::get_if<LocalFunctionStmtPayload>(&Statement.data);
         if (payload) this->analyse_local_function(*payload);
         break;
      }
      case AstNodeKind::FunctionStmt: {
         auto *payload = std::get_if<FunctionStmtPayload>(&Statement.data);
         if (payload) this->analyse_function_stmt(*payload);
         break;
      }
      case AstNodeKind::IfStmt: {
         auto *payload = std::get_if<IfStmtPayload>(&Statement.data);
         if (payload) {
            for (const auto &clause : payload->clauses) {
               if (clause.condition) this->analyse_expression(*clause.condition);
               if (clause.block) {
                  this->push_scope();
                  this->analyse_block(*clause.block);
                  this->pop_scope();
               }
            }
         }
         break;
      }
      case AstNodeKind::WhileStmt:
      case AstNodeKind::RepeatStmt: {
         auto *payload = std::get_if<LoopStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->condition) this->analyse_expression(*payload->condition);
            if (payload->body) {
               this->push_scope();
               this->loop_depth_++;
               this->analyse_block(*payload->body);
               this->loop_depth_--;
               this->pop_scope();
            }
         }
         break;
      }
      case AstNodeKind::NumericForStmt: {
         auto *payload = std::get_if<NumericForStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->start) this->analyse_expression(*payload->start);
            if (payload->stop) this->analyse_expression(*payload->stop);
            if (payload->step) this->analyse_expression(*payload->step);
            if (payload->body) {
               this->push_scope();
               // For loop control variable is implicitly typed as num
               if (payload->control.symbol) {
                  InferredType loop_var;
                  loop_var.primary = TiriType::Num;
                  this->current_scope().declare_local(payload->control.symbol, loop_var, payload->control.span);
               }
               this->loop_depth_++;
               this->analyse_block(*payload->body);
               this->loop_depth_--;
               this->pop_scope();
            }
         }
         break;
      }
      case AstNodeKind::GenericForStmt: {
         auto *payload = std::get_if<GenericForStmtPayload>(&Statement.data);
         if (payload) {
            for (const auto &iterator : payload->iterators) {
               this->analyse_expression(*iterator);
            }
            if (payload->body) {
               this->push_scope();
               // Declare loop variables in the for loop's scope
               for (const auto &name : payload->names) {
                  if (name.symbol) {
                     InferredType loop_var;
                     loop_var.primary = TiriType::Any;  // Type depends on iterator
                     this->current_scope().declare_local(name.symbol, loop_var, name.span);
                  }
               }
               this->loop_depth_++;
               this->analyse_block(*payload->body);
               this->loop_depth_--;
               this->pop_scope();
            }
         }
         break;
      }
      case AstNodeKind::ReturnStmt: {
         auto *payload = std::get_if<ReturnStmtPayload>(&Statement.data);
         if (payload) {
            for (const auto &value : payload->values) {
               this->analyse_expression(*value);
            }
            // Validate return types against function declaration
            this->validate_return_types(*payload, Statement.span);
         }
         break;
      }
      case AstNodeKind::DeferStmt: {
         auto *payload = std::get_if<DeferStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->callable) this->analyse_function_payload(*payload->callable);
            for (const auto &argument : payload->arguments) {
               this->analyse_expression(*argument);
            }
         }
         break;
      }
      case AstNodeKind::DoStmt: {
         auto *payload = std::get_if<DoStmtPayload>(&Statement.data);
         if (payload and payload->block) {
            this->push_scope();
            this->analyse_block(*payload->block);
            this->pop_scope();
         }
         break;
      }
      case AstNodeKind::ExpressionStmt: {
         auto *payload = std::get_if<ExpressionStmtPayload>(&Statement.data);
         if (payload and payload->expression) this->analyse_expression(*payload->expression);
         break;
      }
      default:
         break;
   }
}

//********************************************************************************************************************
// Assignment Analysis
//
// Handles assignment statements (x = value, a, b = c, d).
// For typed variables, validates that the assigned value matches the expected type.
// For untyped variables, the first non-nil assignment fixes the variable's type.
//
// Type fixation rules:
// - Variables declared with explicit type annotations are fixed immediately
// - Variables without annotations become fixed after first non-nil, non-any assignment
// - Nil assignments never fix or change type (nil is compatible with all types)
// - 'any' type variables accept all assignments without fixation

void TypeAnalyser::analyse_assignment(const AssignmentStmtPayload &Payload)
{
   // Check for compound concatenation assignment (..=) in loops
   #ifdef INCLUDE_TIPS
   if (Payload.op IS AssignmentOperator::Concat and not Payload.targets.empty()) {
      this->check_concat_in_loop(Payload.targets[0]->span);
   }
   #endif

   for (size_t i = 0; i < Payload.targets.size(); ++i) {
      const auto &target_ptr = Payload.targets[i];
      if (not target_ptr) continue;
      const auto &target = *target_ptr;

      // Check local and global variable assignments
      if (target.kind IS AstNodeKind::IdentifierExpr) {
         auto *name_ref = std::get_if<NameRef>(&target.data);
         if (not name_ref) continue;

         GCstr *name = name_ref->identifier.symbol;

         // First check local variables

         auto existing = this->resolve_identifier(name);
         bool is_global = false;
         bool is_const = false;

         // If not found as local, check global variables

         if (not existing) {
            existing = this->lookup_global_type(name);
            is_global = existing.has_value();
            if (is_global) is_const = this->is_global_const(name);
         }
         else is_const = this->is_local_const(name);

         if (not existing) continue;

         // Check for assignment to const variable

         if (is_const) {
            std::string_view name_view(strdata(name), name->len);
            TypeDiagnostic diag;
            diag.location = target.span;
            diag.code = ParserErrorCode::AssignToConstant;
            diag.message = std::format("cannot assign to const {} '{}'", is_global ? "global" : "local", name_view);
            this->diagnostics_.push_back(std::move(diag));
            continue;  // Skip further checking for this target
         }

         if (i >= Payload.values.size()) continue;
         InferredType value_type = this->infer_expression_type(*Payload.values[i]);

         if (existing->is_fixed) {
            // Fixed type: check compatibility
            if (existing->primary IS TiriType::Any) continue; // 'any' accepts everything including nil
            if (value_type.primary IS TiriType::Nil) continue;  // Nil is always allowed as a "clear" operation

            if (value_type.primary != TiriType::Any and value_type.primary != existing->primary) {
               // Type mismatch
               TypeDiagnostic diag;
               diag.location = target.span;
               diag.expected = existing->primary;
               diag.actual = value_type.primary;
               diag.code = ParserErrorCode::TypeMismatchAssignment;
               diag.message = std::format("cannot assign '{}' to {} of type '{}'",
                  type_name(value_type.primary), is_global ? "global" : "variable", type_name(existing->primary));
               this->diagnostics_.push_back(std::move(diag));
            }
            // Check for object class ID mismatch (both types are Object but different classes)
            else if (existing->primary IS TiriType::Object and value_type.primary IS TiriType::Object and
               existing->object_class_id != CLASSID::NIL) {
               if (existing->object_class_id != value_type.object_class_id) {
                  TypeDiagnostic diag;
                  diag.location = target.span;
                  diag.expected = TiriType::Object;
                  diag.actual = TiriType::Object;
                  diag.code = ParserErrorCode::ObjectClassMismatch;
                  diag.message = std::format("object class mismatch: cannot assign object of different class to {} ({} vs {})",
                     is_global ? "global" : "variable", ResolveClassID(value_type.object_class_id), ResolveClassID(existing->object_class_id));
                  this->diagnostics_.push_back(std::move(diag));
               }
            }
         }
         else {
            // Unfixed variable: first non-nil assignment fixes the type
            // But don't fix if the variable was explicitly declared as 'any'
            if ((existing->primary != TiriType::Any) and (value_type.primary != TiriType::Nil) and
                (value_type.primary != TiriType::Any)) {
               if (is_global) {
                  this->fix_global_type(name, value_type.primary, value_type.object_class_id);
               }
               else {
                  this->fix_local_type(name, value_type.primary, value_type.object_class_id);
               }
            }
         }
      }
   }

   // Continue with existing analysis

   for (const auto &value : Payload.values) this->analyse_expression(*value);
   for (const auto &target : Payload.targets) this->analyse_expression(*target);
}

//********************************************************************************************************************
// Local Declaration Analysis
//
// Handles 'local' variable declarations with optional type annotations and initialisers.  Supports multi-value
// assignments from function calls (local a, b, c = func()).
//
// Type determination priority:
// 1. Explicit type annotation (local x:num = 5) - type is fixed
// 2. Inferred from initialiser (local x = 5) - type becomes fixed
// 3. No initialiser (local x) - starts as nil, fixes on first assignment

void TypeAnalyser::analyse_local_decl(const LocalDeclStmtPayload &Payload)
{
   // Track which position we're at for multi-value returns from function calls
   // When a function call is the last (or only) value, it may provide multiple return values
   size_t value_index = 0;
   size_t call_return_index = 0;  // Position within a multi-return call
   const ExprNode* multi_return_call = nullptr;  // The function call providing multi-returns

   for (size_t name_index = 0; name_index < Payload.names.size(); ++name_index) {
      const auto& name = Payload.names[name_index];
      InferredType inferred;
      InferredType value_type;
      bool have_value_type = false;

      // Determine the value type for this variable
      if (value_index < Payload.values.size()) {
         // We have an explicit value at this position
         const ExprNode& value_expr = *Payload.values[value_index];
         value_type = this->infer_expression_type(value_expr);
         have_value_type = true;

         // If this is the last value and it's a call expression, it may provide multiple returns
         if (value_index == Payload.values.size() - 1 and value_expr.kind IS AstNodeKind::CallExpr) {
            multi_return_call = &value_expr;
            call_return_index = 0;
         }

         value_index += 1;
      }
      else if (multi_return_call) {
         // No more explicit values, but we have a trailing function call
         // Use the next return value position from the multi-return call
         call_return_index += 1;
         value_type = this->infer_call_return_type(*multi_return_call, call_return_index);
         have_value_type = (value_type.primary != TiriType::Any);
      }

      // Explicit type annotation takes precedence (Unknown = no annotation)
      if (name.type != TiriType::Unknown) {
         inferred.primary = name.type;
         // 'any' type is not fixed - it accepts any value
         inferred.is_fixed = (name.type != TiriType::Any);

         // Check that initial value matches declared type (if present and not 'any')
         if (name.type != TiriType::Any and have_value_type) {
            // Nil is always allowed as initial value for typed variables
            if (value_type.primary != TiriType::Nil and
                value_type.primary != TiriType::Any and
                value_type.primary != name.type) {
               TypeDiagnostic diag;
               if (value_index > 0 and value_index - 1 < Payload.values.size()) {
                  diag.location = Payload.values[value_index - 1]->span;
               }
               diag.expected = name.type;
               diag.actual = value_type.primary;
               diag.code = ParserErrorCode::TypeMismatchAssignment;
               diag.message = std::format("cannot assign '{}' to variable of type '{}'",
                  type_name(value_type.primary), type_name(name.type));
               this->diagnostics_.push_back(std::move(diag));
            }
         }
      }
      else if (have_value_type) {
         // No annotation: infer type from initial value
         inferred = value_type;

         // Non-nil, non-any initial values fix the type
         if (inferred.primary != TiriType::Nil and inferred.primary != TiriType::Any) {
            inferred.is_fixed = true;
         }
      }
      else {
         // No annotation and no initialiser: starts as nil, type not yet determined
         // Use Nil (not Any) so the first non-nil assignment will fix the type
         inferred.primary = TiriType::Nil;
         inferred.is_fixed = false;
      }

      #ifdef INCLUDE_TIPS
      this->check_shadowing(name.symbol, name.span);
      #endif

      this->current_scope().declare_local(name.symbol, inferred, name.span, name.has_const);
      this->trace_decl(this->ctx_.lex().linenumber, name.symbol, inferred.primary, inferred.is_fixed);
   }

   for (const auto &value : Payload.values) {
      this->analyse_expression(*value);
   }
}

//********************************************************************************************************************
// Global Naming Convention Validation
//
// Checks if a global variable name follows Tiri naming conventions:
// - glX... - Starts with 'gl' followed by uppercase letter (e.g., glMyGlobal, glConfig)
// - ALL_CAPS - Full uppercase with underscores for constants (e.g., MY_FLAG, ERR_OKAY)
// - mX... - Starts with 'm' for modules from mod.load() (e.g., mSys, mDisplay)
//
// These conventions help distinguish globals from locals and make code more readable.

[[nodiscard]] static bool is_valid_global_name(std::string_view Name)
{
   if (Name.empty()) return false;

   // Check for 'gl' prefix: glX... where X is uppercase
   if (Name.size() >= 3 and Name[0] IS 'g' and Name[1] IS 'l') {
      return std::isupper(static_cast<unsigned char>(Name[2]));
   }

   // Check for 'm' prefix (module naming): mX... where X is uppercase
   if (Name.size() >= 2 and Name[0] IS 'm' and std::isupper(static_cast<unsigned char>(Name[1]))) {
      return true;
   }

   // Check for ALL_CAPS_WITH_UNDERSCORES pattern
   // Must have at least one character, all uppercase or underscore, must start with uppercase
   if (not std::isupper(static_cast<unsigned char>(Name[0]))) return false;

   for (char c : Name) {
      if (not std::isupper(static_cast<unsigned char>(c)) and c != '_' and not std::isdigit(static_cast<unsigned char>(c))) {
         return false;
      }
   }
   return true;
}

//********************************************************************************************************************
// Global Declaration Analysis
//
// Handles 'global' variable declarations. Unlike locals, globals are stored in the global table and persist across
// function calls. This method checks naming conventions to encourage good practices (globals should be visually
// distinct from locals).

void TypeAnalyser::analyse_global_decl(const GlobalDeclStmtPayload &Payload)
{
   // Analyse the values first
   for (const auto &value : Payload.values) {
      this->analyse_expression(*value);
   }

   // Track global variable types for type checking on subsequent assignments
   for (size_t i = 0; i < Payload.names.size(); ++i) {
      const auto &name = Payload.names[i];
      if (not name.symbol) continue;

      InferredType inferred;

      // Explicit type annotation takes precedence
      if (name.type != TiriType::Unknown) {
         inferred.primary = name.type;
         inferred.is_fixed = (name.type != TiriType::Any);
      }
      else if (i < Payload.values.size()) {
         // Infer type from initial value
         inferred = this->infer_expression_type(*Payload.values[i]);
         // Non-nil, non-any initial values fix the type
         if (inferred.primary != TiriType::Nil and inferred.primary != TiriType::Any) {
            inferred.is_fixed = true;
         }
      }
      else { // No annotation and no initialiser: starts as nil, type not yet fixed
         inferred.primary = TiriType::Nil;
         inferred.is_fixed = false;
      }

      this->declare_global(name.symbol, inferred, name.span, name.has_const);
   }

   #ifdef INCLUDE_TIPS
   // Track globals for loop access detection
   for (const auto &name : Payload.names) {
      if (name.symbol) this->track_global(name.symbol);
   }

   // Check global naming conventions
   if (this->ctx_.should_emit_tip(3)) {
      for (const auto &name : Payload.names) {
         if (not name.symbol) continue;
         std::string_view name_view(strdata(name.symbol), name.symbol->len);
         if (not is_valid_global_name(name_view)) {
            this->ctx_.emit_tip(3, TipCategory::Style,
               std::format("Global variable '{}' should follow naming convention: 'gl[A-Z]...' or 'ALL_CAPS'",
                  name_view),
               Token::from_span(name.span, TokenKind::Identifier));
         }
      }
   }
   #endif
}

//********************************************************************************************************************
// Local Function Analysis: Handles 'local function name()' declarations. The function is registered in the current
// scope for unused variable detection and then its body is analysed.

void TypeAnalyser::analyse_local_function(const LocalFunctionStmtPayload &Payload)
{
   #ifdef INCLUDE_TIPS
   this->check_shadowing(Payload.name.symbol, Payload.name.span);
   #endif

   const FunctionExprPayload* function = Payload.function.get();
   this->current_scope().declare_function(Payload.name.symbol, function, Payload.name.span);

   if (function) this->analyse_function_payload(*function, Payload.name.symbol);
}

//********************************************************************************************************************
// Function Statement Analysis: Handles top-level function declarations (function name(), function table.method()).
// Distinguishes between local functions (tracked for usage) and global functions (exempt from unused detection since
// they're accessible externally).

void TypeAnalyser::analyse_function_stmt(const FunctionStmtPayload &Payload)
{
   const FunctionExprPayload* function = Payload.function.get();
   GCstr *function_name = nullptr;
   SourceSpan function_location{};

   // Only track non-global function declarations for unused variable detection
   // Global functions (declared with `global function`) are not local to any scope
   if (not Payload.name.is_explicit_global) {
      if (not Payload.name.segments.empty()) {
         const Identifier& terminal = Payload.name.segments.back();
         this->current_scope().declare_function(terminal.symbol, function, terminal.span);
         function_name = terminal.symbol;
         function_location = terminal.span;
      }

      if (Payload.name.method) {
         this->current_scope().declare_function(Payload.name.method->symbol, function, Payload.name.method->span);
         function_name = Payload.name.method->symbol;
         function_location = Payload.name.method->span;
      }
   }
   else {
      // Track global function type for type checking on reassignment
      // Note: Global functions are exempt from naming convention checks
      if (not Payload.name.segments.empty()) {
         function_name = Payload.name.segments.back().symbol;
         function_location = Payload.name.segments.back().span;
         this->declare_global_function(function_name, function, function_location);
      }
      else if (Payload.name.method) {
         function_name = Payload.name.method->symbol;
         function_location = Payload.name.method->span;
         this->declare_global_function(function_name, function, function_location);
      }
   }

   if (function) this->analyse_function_payload(*function, function_name);
}

//********************************************************************************************************************
// Function Payload Analysis: Analyses a function's body, including parameter registration, return type validation,
// and recursive function detection. Creates a new scope for the function body.

void TypeAnalyser::analyse_function_payload(const FunctionExprPayload &Function, GCstr *Name)
{
   this->push_scope();
   this->enter_function(Function, Name);

   for (const auto &param : Function.parameters) {
      this->current_scope().declare_parameter(param.name.symbol, param.type, param.name.span);
   }

   // Check for recursive functions without explicit return types
   // Recursive functions must have explicit return type declarations because their
   // return type cannot be inferred without executing the recursion.
   // Exception: void functions (no return values) are exempt since there's nothing to infer.

   if ((not Function.return_types.is_explicit) and (Name) and (this->is_recursive_function(Function, Name)) and
       this->function_has_return_values(Function)) {
      TypeDiagnostic diag;
      diag.location = Function.body ? Function.body->span : SourceSpan{};
      diag.code = ParserErrorCode::RecursiveFunctionNeedsType;
      diag.message = std::format("recursive function '{}' must have explicit return type declaration",
         Name ? std::string_view(strdata(Name), Name->len) : "<anonymous>");
      this->diagnostics_.push_back(std::move(diag));
   }

   // Advise on missing return type annotation for functions that return values

   #ifdef INCLUDE_TIPS
   if (this->ctx_.should_emit_tip(1) and
       (not Function.return_types.is_explicit) and this->function_has_return_values(Function)) {
      SourceSpan span = Function.body ? Function.body->span : SourceSpan{};
      this->ctx_.emit_tip(1, TipCategory::TypeSafety,
         "Function lacks return type annotation; consider adding ': type' after the parameter list",
         Token::from_span(span, TokenKind::Function));
   }
   #endif

   if (Function.body) this->analyse_block(*Function.body);

   this->leave_function();
   this->pop_scope();
}

//********************************************************************************************************************
// Expression Analysis: Recursively analyses expressions to track variable usage and collect type information.
// Marks identifiers as used when they appear in expressions (for unused variable detection).

void TypeAnalyser::analyse_expression(const ExprNode &Expression)
{
   switch (Expression.kind) {
      case AstNodeKind::UnaryExpr: {
         auto *payload = std::get_if<UnaryExprPayload>(&Expression.data);
         if (payload and payload->operand) this->analyse_expression(*payload->operand);
         break;
      }
      case AstNodeKind::UpdateExpr: {
         auto *payload = std::get_if<UpdateExprPayload>(&Expression.data);
         if (payload and payload->target) this->analyse_expression(*payload->target);
         break;
      }
      case AstNodeKind::BinaryExpr: {
         auto *payload = std::get_if<BinaryExprPayload>(&Expression.data);
         if (payload) {
            #ifdef INCLUDE_TIPS
            if (payload->op IS AstBinaryOperator::Concat) {
               this->check_concat_in_loop(Expression.span);
            }
            #endif
            if (payload->left) this->analyse_expression(*payload->left);
            if (payload->right) this->analyse_expression(*payload->right);

            // Warn if `has` operator operands are not numeric
            if (payload->op IS AstBinaryOperator::HasFlag) {
               auto check_operand = [&](const std::unique_ptr<ExprNode> &operand, const char *Side) {
                  if (not operand) return;
                  auto inferred = this->infer_expression_type(*operand);
                  if (inferred.primary != TiriType::Any and inferred.primary != TiriType::Unknown
                      and inferred.primary != TiriType::Num and inferred.primary != TiriType::Nil) {
                     TypeDiagnostic diag;
                     diag.location = operand->span;
                     diag.expected = TiriType::Num;
                     diag.actual = inferred.primary;
                     diag.code = ParserErrorCode::TypeMismatchArgument;
                     diag.message = std::format("'has' operator expects numeric {}, got {}", Side,
                        type_name(inferred.primary));
                     this->diagnostics_.push_back(std::move(diag));
                  }
               };
               check_operand(payload->left, "operand");
               check_operand(payload->right, "operand");
            }
         }
         break;
      }
      case AstNodeKind::TernaryExpr: {
         auto *payload = std::get_if<TernaryExprPayload>(&Expression.data);
         if (payload) {
            if (payload->condition) this->analyse_expression(*payload->condition);
            if (payload->if_true) this->analyse_expression(*payload->if_true);
            if (payload->if_false) this->analyse_expression(*payload->if_false);
         }
         break;
      }
      case AstNodeKind::PresenceExpr: {
         auto *payload = std::get_if<PresenceExprPayload>(&Expression.data);
         if (payload and payload->value) this->analyse_expression(*payload->value);
         break;
      }
      case AstNodeKind::CallExpr: {
         auto *payload = std::get_if<CallExprPayload>(&Expression.data);
         if (payload) this->analyse_call_expr(*payload);
         break;
      }
      case AstNodeKind::MemberExpr: {
         auto *payload = std::get_if<MemberExprPayload>(&Expression.data);
         if (payload and payload->table) this->analyse_expression(*payload->table);
         break;
      }
      case AstNodeKind::IndexExpr: {
         auto *payload = std::get_if<IndexExprPayload>(&Expression.data);
         if (payload) {
            if (payload->table) this->analyse_expression(*payload->table);
            if (payload->index) this->analyse_expression(*payload->index);
         }
         break;
      }
      case AstNodeKind::SafeMemberExpr: {
         auto *payload = std::get_if<SafeMemberExprPayload>(&Expression.data);
         if (payload and payload->table) this->analyse_expression(*payload->table);
         break;
      }
      case AstNodeKind::SafeIndexExpr: {
         auto *payload = std::get_if<SafeIndexExprPayload>(&Expression.data);
         if (payload) {
            if (payload->table) this->analyse_expression(*payload->table);
            if (payload->index) this->analyse_expression(*payload->index);
         }
         break;
      }
      case AstNodeKind::TableExpr: {
         auto *payload = std::get_if<TableExprPayload>(&Expression.data);
         if (payload) {
            for (const auto &field : payload->fields) {
               if (field.key) this->analyse_expression(*field.key);
               if (field.value) this->analyse_expression(*field.value);
            }
         }
         break;
      }
      case AstNodeKind::FunctionExpr: {
         auto *payload = std::get_if<FunctionExprPayload>(&Expression.data);
         if (payload) {
            #ifdef INCLUDE_TIPS
            this->check_function_in_loop(Expression.span);
            #endif
            this->analyse_function_payload(*payload);
         }
         break;
      }
      case AstNodeKind::IdentifierExpr: {
         // Mark variable as used when it appears in an expression
         auto *payload = std::get_if<NameRef>(&Expression.data);
         if (payload) {
            this->mark_identifier_used(payload->identifier.symbol);
            #ifdef INCLUDE_TIPS
            this->check_global_in_loop(payload->identifier.symbol, payload->identifier.span);
            #endif
         }
         break;
      }
      case AstNodeKind::ChooseExpr: {
         auto *payload = std::get_if<ChooseExprPayload>(&Expression.data);
         if (payload) {
            // Analyse scrutinee (the value being matched)
            if (payload->scrutinee) this->analyse_expression(*payload->scrutinee);
            for (const auto &tuple_elem : payload->scrutinee_tuple) {
               if (tuple_elem) this->analyse_expression(*tuple_elem);
            }
            // Analyse each case
            for (const auto &case_item : payload->cases) {
               if (case_item.pattern) this->analyse_expression(*case_item.pattern);
               for (const auto &tuple_pattern : case_item.tuple_patterns) {
                  if (tuple_pattern) this->analyse_expression(*tuple_pattern);
               }
               if (case_item.guard) this->analyse_expression(*case_item.guard);
               if (case_item.result) this->analyse_expression(*case_item.result);
               if (case_item.result_stmt) this->analyse_statement(*case_item.result_stmt);
            }
         }
         break;
      }
      default:
         break;
   }
}

//********************************************************************************************************************
// Call Expression Analysis: Analyses function calls including direct calls, method calls, and safe method calls.
// Validates argument types against the function's parameter declarations if available.

void TypeAnalyser::analyse_call_expr(const CallExprPayload &Call)
{
   // Analyse the callable to mark function names as used
   if (std::holds_alternative<DirectCallTarget>(Call.target)) {
      const auto &direct = std::get<DirectCallTarget>(Call.target);
      if (direct.callable) this->analyse_expression(*direct.callable);
   }
   else if (std::holds_alternative<MethodCallTarget>(Call.target)) {
      const auto &method = std::get<MethodCallTarget>(Call.target);
      if (method.receiver) this->analyse_expression(*method.receiver);
   }
   else if (std::holds_alternative<SafeMethodCallTarget>(Call.target)) {
      const auto &safe_method = std::get<SafeMethodCallTarget>(Call.target);
      if (safe_method.receiver) this->analyse_expression(*safe_method.receiver);
   }

   // Analyse arguments
   for (const auto &argument : Call.arguments) {
      this->analyse_expression(*argument);
   }

   const FunctionExprPayload* target = this->resolve_call_target(Call.target);
   if (target) this->check_arguments(*target, Call);
}

//********************************************************************************************************************
// Validate each argument against the corresponding parameter type declaration.

void TypeAnalyser::check_arguments(const FunctionExprPayload &Function, const CallExprPayload &Call)
{
   size_t param_index = 0;
   for (const auto &param : Function.parameters) {
      if (param_index >= Call.arguments.size()) break;
      this->check_argument_type(*Call.arguments[param_index], param.type, param_index);
      param_index += 1;
   }
}

//********************************************************************************************************************
// Check a single argument against its expected type, reporting diagnostics for mismatches.

void TypeAnalyser::check_argument_type(const ExprNode& Argument, TiriType Expected, size_t Index)
{
   if (Expected IS TiriType::Any) return;

   InferredType actual = this->infer_expression_type(Argument);

   if (not actual.matches(Expected)) {
      TypeDiagnostic diag;
      diag.location = Argument.span;
      diag.expected = Expected;
      diag.actual = actual.primary;
      diag.code = ParserErrorCode::TypeMismatchArgument;
      diag.message = std::format("type mismatch: argument {} expects '{}', got '{}'",
         Index + 1, type_name(Expected), type_name(actual.primary));
      this->diagnostics_.push_back(std::move(diag));
   }
}

//********************************************************************************************************************
// Type Inference: Infers the type of an expression based on its AST structure. Returns InferredType containing the
// primary type and metadata (constant, nullable, fixed).
//
// Inference rules by expression type:
// - Literals: Type determined by literal kind (nil, bool, num, str)
// - Identifiers: Looked up in scope stack, returns declared or inferred type
// - Tables: Always TiriType::Table
// - Functions: Always TiriType::Func
// - Calls: Uses function's declared return type if available, otherwise Any
// - Binary ops: Depends on operator (comparisons -> bool, arithmetic -> num, etc.)
// - Unary ops: Depends on operator (not -> bool, negate -> num, length -> num)

InferredType TypeAnalyser::infer_expression_type(const ExprNode& Expr)
{
   InferredType result;

   switch (Expr.kind) {
      case AstNodeKind::LiteralExpr: {
         auto *payload = std::get_if<LiteralValue>(&Expr.data);
         if (payload) return infer_literal_type(*payload);
         break;
      }
      case AstNodeKind::IdentifierExpr: {
         auto *payload = std::get_if<NameRef>(&Expr.data);
         if (payload) {
            // Mark the variable as used
            this->mark_identifier_used(payload->identifier.symbol);
            auto resolved = this->resolve_identifier(payload->identifier.symbol);
            if (resolved) return *resolved;
         }
         break;
      }
      case AstNodeKind::TableExpr:
         result.primary = TiriType::Table;
         break;
      case AstNodeKind::FunctionExpr:
         result.primary = TiriType::Func;
         break;
      case AstNodeKind::CallExpr: {
         // For call expressions, try to infer from the function's declared return type
         auto *payload = std::get_if<CallExprPayload>(&Expr.data);
         if (payload) {
            // First check if the call has a known result type (e.g., obj.new() returns Object)
            if (payload->result_type != TiriType::Unknown) {
               result.primary = payload->result_type;
               // Propagate object class ID for Object types
               if (payload->result_type IS TiriType::Object) {
                  result.object_class_id = payload->object_class_id;
               }
               return result;
            }
            // Otherwise try to infer from the function's declared return type
            const FunctionExprPayload* target = this->resolve_call_target(payload->target);
            if (target and target->return_types.is_explicit and target->return_types.count > 0) {
               result.primary = target->return_types.types[0];
               return result;
            }
         }
         result.primary = TiriType::Any;
         break;
      }
      case AstNodeKind::BinaryExpr: {
         // Infer type from binary expression operands and operator
         auto *payload = std::get_if<BinaryExprPayload>(&Expr.data);
         if (payload) {
            switch (payload->op) {
               // Comparison operators always return boolean
               case AstBinaryOperator::Equal:
               case AstBinaryOperator::NotEqual:
               case AstBinaryOperator::LessThan:
               case AstBinaryOperator::LessEqual:
               case AstBinaryOperator::GreaterThan:
               case AstBinaryOperator::GreaterEqual:
               case AstBinaryOperator::HasFlag:
                  result.primary = TiriType::Bool;
                  return result;
               // Logical operators in Lua/Tiri return one of their operands.
               // Try to infer from operands, if both have the same type, use that.
               case AstBinaryOperator::LogicalAnd:
               case AstBinaryOperator::LogicalOr: {
                  InferredType left_type, right_type;
                  if (payload->left) left_type = this->infer_expression_type(*payload->left);
                  if (payload->right) right_type = this->infer_expression_type(*payload->right);

                  // If both operands have the same concrete type, return that

                  if ((left_type.primary IS right_type.primary) and (left_type.primary != TiriType::Any) and
                      (left_type.primary != TiriType::Unknown)) {
                     return left_type;
                  }

                  // For `or`, the right operand is the fallback, so prefer its type if known

                  if (payload->op IS AstBinaryOperator::LogicalOr) {
                     if ((right_type.primary != TiriType::Any) and (right_type.primary != TiriType::Unknown)) {
                        return right_type;
                     }
                     if ((left_type.primary != TiriType::Any) and (left_type.primary != TiriType::Unknown)) {
                        return left_type;
                     }
                  }
                  else { // For `and`, the left operand short-circuits, so prefer left type if known
                     if ((left_type.primary != TiriType::Any) and (left_type.primary != TiriType::Unknown)) {
                        return left_type;
                     }
                     if ((right_type.primary != TiriType::Any) and (right_type.primary != TiriType::Unknown)) {
                        return right_type;
                     }
                  }

                  result.primary = TiriType::Any;
                  return result;
               }
               // Concatenation returns string
               case AstBinaryOperator::Concat:
                  result.primary = TiriType::Str;
                  return result;
               // Arithmetic operators return number
               case AstBinaryOperator::Add:
               case AstBinaryOperator::Subtract:
               case AstBinaryOperator::Multiply:
               case AstBinaryOperator::Divide:
               case AstBinaryOperator::Modulo:
               case AstBinaryOperator::Power:
               case AstBinaryOperator::BitAnd:
               case AstBinaryOperator::BitOr:
               case AstBinaryOperator::BitXor:
               case AstBinaryOperator::ShiftLeft:
               case AstBinaryOperator::ShiftRight:
                  result.primary = TiriType::Num;
                  return result;
               // IfEmpty returns type of the operands
               case AstBinaryOperator::IfEmpty:
                  if (payload->left) {
                     result = this->infer_expression_type(*payload->left);
                     if (result.primary != TiriType::Any and result.primary != TiriType::Unknown) {
                        return result;
                     }
                  }
                  if (payload->right) {
                     return this->infer_expression_type(*payload->right);
                  }
                  break;
            }
         }
         result.primary = TiriType::Any;
         break;
      }
      case AstNodeKind::UnaryExpr: {
         auto *payload = std::get_if<UnaryExprPayload>(&Expr.data);
         if (payload) {
            switch (payload->op) {
               case AstUnaryOperator::Not:
                  result.primary = TiriType::Bool;
                  return result;
               case AstUnaryOperator::Negate:
               case AstUnaryOperator::BitNot:
                  result.primary = TiriType::Num;
                  return result;
               case AstUnaryOperator::Length:
                  result.primary = TiriType::Num;
                  return result;
            }
         }
         result.primary = TiriType::Any;
         break;
      }
      case AstNodeKind::TernaryExpr: {
         // Ternary returns type of true branch (or false branch if true is unknown)
         auto *payload = std::get_if<TernaryExprPayload>(&Expr.data);
         if (payload) {
            if (payload->if_true) {
               result = this->infer_expression_type(*payload->if_true);
               if (result.primary != TiriType::Any and result.primary != TiriType::Unknown) return result;
            }

            if (payload->if_false) return this->infer_expression_type(*payload->if_false);
         }
         result.primary = TiriType::Any;
         break;
      }
      default:
         result.primary = TiriType::Any;
         break;
   }

   return result;
}

//********************************************************************************************************************
// Multi-Value Return Type Inference: Infers the return type at a specific position from a function call expression.
// Used for multi-value assignments like: local a, b, c = func()
// where func() returns multiple values and we need to know the type of each.

[[nodiscard]] InferredType TypeAnalyser::infer_call_return_type(const ExprNode& Expr, size_t Position) const
{
   InferredType result;
   result.primary = TiriType::Any;

   if (Expr.kind != AstNodeKind::CallExpr) return result;

   auto *payload = std::get_if<CallExprPayload>(&Expr.data);
   if (not payload) return result;

   const FunctionExprPayload* target = this->resolve_call_target(payload->target);
   if (not target) return result;

   if (not target->return_types.is_explicit) return result;

   // Get the type at the requested position
   TiriType type = target->return_types.type_at(Position);
   if (type != TiriType::Unknown) result.primary = type;

   return result;
}

//********************************************************************************************************************
// Symbol Resolution: These methods look up identifiers in the scope stack to find their types and resolve function
// references for call target analysis.
//
// Look up a variable's type by searching from innermost to outermost scope.

std::optional<InferredType> TypeAnalyser::resolve_identifier(GCstr *Name) const
{
   for (auto it = this->scope_stack_.rbegin(); it != this->scope_stack_.rend(); ++it) {
      auto type = it->lookup_local_type(Name);
      if (type) return type;

      auto param = it->lookup_parameter_type(Name);
      if (param) {
         InferredType inferred;
         inferred.primary = *param;
         return inferred;
      }
   }
   return std::nullopt;
}

// Mark a variable as used when it appears in an expression.  Searches from innermost to outermost scope to find
// where it's defined.

void TypeAnalyser::mark_identifier_used(GCstr *Name)
{
   if (not Name) return;

   // Mark the variable as used in the scope where it's defined
   for (auto it = this->scope_stack_.rbegin(); it != this->scope_stack_.rend(); ++it) {
      // Check if this scope has the variable and mark it
      auto type = it->lookup_local_type(Name);
      if (type) {
         it->mark_used(Name);
         return;
      }

      auto param = it->lookup_parameter_type(Name);
      if (param) {
         it->mark_used(Name);
         return;
      }
   }
}

//********************************************************************************************************************
// Resolve the target of a function call to get its FunctionExprPayload.  Handles direct calls (func()) and
// identifier references (myFunc()).

const FunctionExprPayload * TypeAnalyser::resolve_call_target(const CallTarget &Target) const
{
   if (std::holds_alternative<DirectCallTarget>(Target)) {
      const auto &direct = std::get<DirectCallTarget>(Target);
      if (direct.callable) {
         if (direct.callable->kind IS AstNodeKind::FunctionExpr) {
            auto *payload = std::get_if<FunctionExprPayload>(&direct.callable->data);
            if (payload) return payload;
         }
         if (direct.callable->kind IS AstNodeKind::IdentifierExpr) {
            auto *payload = std::get_if<NameRef>(&direct.callable->data);
            if (payload) return this->resolve_function(payload->identifier.symbol);
         }
      }
   }
   return nullptr;
}

// Look up a function by name in the scope stack.

const FunctionExprPayload * TypeAnalyser::resolve_function(GCstr *Name) const
{
   for (auto it = this->scope_stack_.rbegin(); it != this->scope_stack_.rend(); ++it) {
      const FunctionExprPayload* fn = it->lookup_function(Name);
      if (fn) return fn;
   }
   return nullptr;
}

//********************************************************************************************************************
// Fix (lock) a variable's type after the first concrete assignment.  Once fixed, the variable cannot be assigned
// values of different types.

bool TypeAnalyser::is_local_const(GCstr *Name) const
{
   for (auto it = this->scope_stack_.rbegin(); it != this->scope_stack_.rend(); ++it) {
      if (it->lookup_local_type(Name)) return it->is_local_const(Name);
   }
   return false;
}

void TypeAnalyser::fix_local_type(GCstr *Name, TiriType Type, CLASSID ObjectClassId)
{
   for (auto it = this->scope_stack_.rbegin(); it != this->scope_stack_.rend(); ++it) {
      auto existing = it->lookup_local_type(Name);
      if (existing) {
         it->fix_local_type(Name, Type, ObjectClassId);
         this->trace_fix(this->ctx_.lex().linenumber, Name, Type);
         return;
      }
   }
}

//********************************************************************************************************************
// Global Variable Type Tracking
//
// These methods manage type information for global variables declared with the 'global' keyword.
// Unlike locals which use scope-based tracking, globals use a flat map since they persist for
// the entire script lifetime.

void TypeAnalyser::declare_global(GCstr *Name, const InferredType &Type, SourceSpan Location, bool IsConst)
{
   if (not Name) return;
   GlobalTypeInfo info;
   info.type = Type;
   info.location = Location;
   info.is_const = IsConst;
   this->global_types_[Name] = info;
   this->trace_decl(this->ctx_.lex().linenumber, Name, Type.primary, Type.is_fixed);
}

void TypeAnalyser::declare_global_function(GCstr *Name, const FunctionExprPayload *Function, SourceSpan Location)
{
   if (not Name) return;
   GlobalTypeInfo info;
   info.type.primary = TiriType::Func;
   info.type.is_fixed = true;  // Functions have fixed type
   info.location = Location;
   info.function = Function;
   this->global_types_[Name] = info;
   this->trace_decl(this->ctx_.lex().linenumber, Name, TiriType::Func, true);
}

std::optional<InferredType> TypeAnalyser::lookup_global_type(GCstr *Name) const
{
   if (not Name) return std::nullopt;
   if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) return it->second.type;
   return std::nullopt;
}

void TypeAnalyser::fix_global_type(GCstr *Name, TiriType Type, CLASSID ObjectClassId)
{
   if (not Name) return;
   if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) {
      it->second.type.primary = Type;
      it->second.type.is_fixed = true;
      it->second.type.object_class_id = ObjectClassId;
      this->trace_fix(this->ctx_.lex().linenumber, Name, Type);
   }
}

bool TypeAnalyser::is_global_const(GCstr *Name) const
{
   if (not Name) return false;
   if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) return it->second.is_const;
   return false;
}

//********************************************************************************************************************
// Return type validation: This method validates return statements against the function's declared or inferred return
// types.
//
// It implements:
// - Type mismatch detection between returned values and declared types
// - Return count validation (too many values returned)
// - First-wins inference rule for functions without explicit return type declarations
// - Nil is always allowed as a valid return value for any type slot

void TypeAnalyser::validate_return_types(const ReturnStmtPayload &Return, SourceSpan Location)
{
   FunctionContext* ctx = this->current_function();
   if (not ctx) return;  // Not inside a function (shouldn't happen in valid code)

   size_t return_count = Return.values.size();

   if (ctx->expected_returns.is_explicit) {
      // Explicit declaration: validate against declared types

      // Check for too many return values (unless variadic)
      if (not ctx->expected_returns.is_variadic and return_count > ctx->expected_returns.count) {
         TypeDiagnostic diag;
         diag.location = Location;
         diag.code     = ParserErrorCode::ReturnCountMismatch;
         diag.message  = std::format("too many return values: function declares {} but {} returned",
            ctx->expected_returns.count, return_count);
         this->diagnostics_.push_back(std::move(diag));
      }

      // Validate type of each returned value
      for (size_t i = 0; i < return_count and i < MAX_RETURN_TYPES; ++i) {
         TiriType expected = ctx->expected_returns.type_at(i);
         if (expected IS TiriType::Any or expected IS TiriType::Unknown) continue;

         InferredType actual = this->infer_expression_type(*Return.values[i]);

         // Nil is always allowed as a "clear" or "no value" return
         if (actual.primary IS TiriType::Nil) continue;
         // Any can be assigned to any type
         if (actual.primary IS TiriType::Any) continue;

         if (actual.primary != expected) {
            TypeDiagnostic diag;
            diag.location = Return.values[i]->span;
            diag.expected = expected;
            diag.actual = actual.primary;
            diag.code = ParserErrorCode::ReturnTypeMismatch;
            diag.message = std::format("return type mismatch at position {}: expected '{}', got '{}'",
               i + 1, type_name(expected), type_name(actual.primary));
            this->diagnostics_.push_back(std::move(diag));
         }
      }
   }
   else {
      // Inference mode: first non-nil return statement fixes types (first-wins rule)
      // Nil returns don't establish a type - they're compatible with any future type
      if (not ctx->return_type_inferred and return_count > 0) {
         // First return: infer types from returned values
         bool has_non_nil = false;
         for (size_t i = 0; i < std::min(return_count, MAX_RETURN_TYPES); ++i) {
            InferredType inferred = this->infer_expression_type(*Return.values[i]);
            ctx->expected_returns.types[i] = inferred.primary;
            if (inferred.primary != TiriType::Nil and inferred.primary != TiriType::Any) {
               has_non_nil = true;
            }
         }
         ctx->expected_returns.count = uint8_t(std::min(return_count, MAX_RETURN_TYPES));
         // Only mark as inferred if we have at least one concrete (non-nil) type
         // This allows a later return with concrete types to establish the actual types
         ctx->return_type_inferred = has_non_nil;
      }
      else if (return_count > 0) {
         // Subsequent return: check consistency with inferred types
         size_t check_count = std::min(return_count, size_t(ctx->expected_returns.count));

         for (size_t i = 0; i < check_count; ++i) {
            TiriType expected = ctx->expected_returns.types[i];
            InferredType actual = this->infer_expression_type(*Return.values[i]);

            // If expected is nil/any/unknown, and actual is concrete, upgrade the expected type
            if ((expected IS TiriType::Nil or expected IS TiriType::Any or expected IS TiriType::Unknown) and
                actual.primary != TiriType::Nil and actual.primary != TiriType::Any and
                actual.primary != TiriType::Unknown) {
               ctx->expected_returns.types[i] = actual.primary;
               ctx->return_type_inferred = true;
               continue;
            }

            if (expected IS TiriType::Any or expected IS TiriType::Unknown) continue;

            // Nil is always allowed as a "clear" or "no value" return
            if (actual.primary IS TiriType::Nil) continue;
            // Any can match any type
            if (actual.primary IS TiriType::Any) continue;

            if (actual.primary != expected) {
               TypeDiagnostic diag;
               diag.location = Return.values[i]->span;
               diag.expected = expected;
               diag.actual = actual.primary;
               diag.code = ParserErrorCode::ReturnTypeMismatch;
               diag.message = std::format("inconsistent return type at position {}: first return established '{}', but this returns '{}'",
                  i + 1, type_name(expected), type_name(actual.primary));
               this->diagnostics_.push_back(std::move(diag));
            }
         }
      }
   }
}

//********************************************************************************************************************
// Recursive function detection:  Recursive functions must have explicit return type declarations because their
// return type cannot be inferred without executing the recursion.  This detects direct recursion (function calls
// itself) and flags an error if no explicit return type is declared.

bool TypeAnalyser::is_recursive_function(const FunctionExprPayload &Function, GCstr *Name) const
{
   if (not Name) return false;
   if (not Function.body) return false;
   return this->body_contains_call_to(*Function.body, Name);
}

// Check if a function has any return statements with values (non-void returns)

bool TypeAnalyser::function_has_return_values(const FunctionExprPayload &Function) const
{
   if (not Function.body) return false;
   return this->body_has_return_values(*Function.body);
}

// Recursively check if a block contains any return statements with values

bool TypeAnalyser::body_has_return_values(const BlockStmt& Block) const
{
   for (const auto &stmt : Block.statements) {
      if (not stmt) continue;

      switch (stmt->kind) {
         case AstNodeKind::ReturnStmt: {
            auto *payload = std::get_if<ReturnStmtPayload>(&stmt->data);
            if (payload and not payload->values.empty()) return true;  // Found a return with values
            break;
         }
         case AstNodeKind::IfStmt: {
            auto *payload = std::get_if<IfStmtPayload>(&stmt->data);
            if (payload) {
               for (const auto &clause : payload->clauses) {
                  if (clause.block and this->body_has_return_values(*clause.block)) return true;
               }
            }
            break;
         }
         case AstNodeKind::WhileStmt:
         case AstNodeKind::RepeatStmt: {
            auto *payload = std::get_if<LoopStmtPayload>(&stmt->data);
            if (payload and payload->body and this->body_has_return_values(*payload->body)) return true;
            break;
         }
         case AstNodeKind::NumericForStmt: {
            auto *payload = std::get_if<NumericForStmtPayload>(&stmt->data);
            if (payload and payload->body and this->body_has_return_values(*payload->body)) return true;
            break;
         }
         case AstNodeKind::GenericForStmt: {
            auto *payload = std::get_if<GenericForStmtPayload>(&stmt->data);
            if (payload and payload->body and this->body_has_return_values(*payload->body)) return true;
            break;
         }
         case AstNodeKind::DoStmt: {
            auto *payload = std::get_if<DoStmtPayload>(&stmt->data);
            if (payload and payload->block and this->body_has_return_values(*payload->block)) return true;
            break;
         }
         default:
            break;
      }
   }
   return false;
}

//********************************************************************************************************************
// Recursive Call Detection Helpers: These methods search the AST for calls to a specific function name, used to
// detect direct recursion. They traverse all statement and expression types that might contain function calls.

bool TypeAnalyser::body_contains_call_to(const BlockStmt& Block, GCstr *Name) const
{
   for (const auto &stmt : Block.statements) {
      if (stmt and this->statement_contains_call_to(*stmt, Name)) return true;
   }
   return false;
}

bool TypeAnalyser::statement_contains_call_to(const StmtNode& Stmt, GCstr *Name) const
{
   switch (Stmt.kind) {
      case AstNodeKind::ExpressionStmt: {
         auto *payload = std::get_if<ExpressionStmtPayload>(&Stmt.data);
         if (payload and payload->expression) {
            return this->expression_contains_call_to(*payload->expression, Name);
         }
         break;
      }
      case AstNodeKind::AssignmentStmt: {
         auto *payload = std::get_if<AssignmentStmtPayload>(&Stmt.data);
         if (payload) {
            for (const auto &value : payload->values) {
               if (value and this->expression_contains_call_to(*value, Name)) return true;
            }
         }
         break;
      }
      case AstNodeKind::LocalDeclStmt: {
         auto *payload = std::get_if<LocalDeclStmtPayload>(&Stmt.data);
         if (payload) {
            for (const auto &value : payload->values) {
               if (value and this->expression_contains_call_to(*value, Name)) return true;
            }
         }
         break;
      }
      case AstNodeKind::ReturnStmt: {
         auto *payload = std::get_if<ReturnStmtPayload>(&Stmt.data);
         if (payload) {
            for (const auto &value : payload->values) {
               if (value and this->expression_contains_call_to(*value, Name)) return true;
            }
         }
         break;
      }
      case AstNodeKind::IfStmt: {
         auto *payload = std::get_if<IfStmtPayload>(&Stmt.data);
         if (payload) {
            for (const auto &clause : payload->clauses) {
               if (clause.condition and this->expression_contains_call_to(*clause.condition, Name)) return true;
               if (clause.block and this->body_contains_call_to(*clause.block, Name)) return true;
            }
         }
         break;
      }
      case AstNodeKind::WhileStmt:
      case AstNodeKind::RepeatStmt: {
         auto *payload = std::get_if<LoopStmtPayload>(&Stmt.data);
         if (payload) {
            if (payload->condition and this->expression_contains_call_to(*payload->condition, Name)) return true;
            if (payload->body and this->body_contains_call_to(*payload->body, Name)) return true;
         }
         break;
      }
      case AstNodeKind::NumericForStmt: {
         auto *payload = std::get_if<NumericForStmtPayload>(&Stmt.data);
         if (payload) {
            if (payload->start and this->expression_contains_call_to(*payload->start, Name)) return true;
            if (payload->stop and this->expression_contains_call_to(*payload->stop, Name)) return true;
            if (payload->step and this->expression_contains_call_to(*payload->step, Name)) return true;
            if (payload->body and this->body_contains_call_to(*payload->body, Name)) return true;
         }
         break;
      }
      case AstNodeKind::GenericForStmt: {
         auto *payload = std::get_if<GenericForStmtPayload>(&Stmt.data);
         if (payload) {
            for (const auto &iter : payload->iterators) {
               if (iter and this->expression_contains_call_to(*iter, Name)) return true;
            }
            if (payload->body and this->body_contains_call_to(*payload->body, Name)) return true;
         }
         break;
      }
      case AstNodeKind::DoStmt: {
         auto *payload = std::get_if<DoStmtPayload>(&Stmt.data);
         if (payload and payload->block) return this->body_contains_call_to(*payload->block, Name);
         break;
      }
      default:
         break;
   }
   return false;
}

bool TypeAnalyser::expression_contains_call_to(const ExprNode& Expr, GCstr *Name) const
{
   switch (Expr.kind) {
      case AstNodeKind::CallExpr: {
         auto *payload = std::get_if<CallExprPayload>(&Expr.data);
         if (payload) {
            // Check if this is a direct call to the function name
            if (std::holds_alternative<DirectCallTarget>(payload->target)) {
               const auto &direct = std::get<DirectCallTarget>(payload->target);
               if (direct.callable and direct.callable->kind IS AstNodeKind::IdentifierExpr) {
                  auto *name_ref = std::get_if<NameRef>(&direct.callable->data);
                  if (name_ref and name_ref->identifier.symbol IS Name) {
                     return true;  // Direct recursive call found
                  }
               }
               // Also check inside the callable expression
               if (direct.callable and this->expression_contains_call_to(*direct.callable, Name)) {
                  return true;
               }
            }
            // Check arguments for recursive calls
            for (const auto &arg : payload->arguments) {
               if (arg and this->expression_contains_call_to(*arg, Name)) return true;
            }
         }
         break;
      }
      case AstNodeKind::BinaryExpr: {
         auto *payload = std::get_if<BinaryExprPayload>(&Expr.data);
         if (payload) {
            if (payload->left and this->expression_contains_call_to(*payload->left, Name)) return true;
            if (payload->right and this->expression_contains_call_to(*payload->right, Name)) return true;
         }
         break;
      }
      case AstNodeKind::UnaryExpr: {
         auto *payload = std::get_if<UnaryExprPayload>(&Expr.data);
         if (payload and payload->operand) {
            return this->expression_contains_call_to(*payload->operand, Name);
         }
         break;
      }
      case AstNodeKind::TernaryExpr: {
         auto *payload = std::get_if<TernaryExprPayload>(&Expr.data);
         if (payload) {
            if (payload->condition and this->expression_contains_call_to(*payload->condition, Name)) return true;
            if (payload->if_true and this->expression_contains_call_to(*payload->if_true, Name)) return true;
            if (payload->if_false and this->expression_contains_call_to(*payload->if_false, Name)) return true;
         }
         break;
      }
      case AstNodeKind::MemberExpr: {
         auto *payload = std::get_if<MemberExprPayload>(&Expr.data);
         if (payload and payload->table) return this->expression_contains_call_to(*payload->table, Name);
         break;
      }
      case AstNodeKind::IndexExpr: {
         auto *payload = std::get_if<IndexExprPayload>(&Expr.data);
         if (payload) {
            if (payload->table and this->expression_contains_call_to(*payload->table, Name)) return true;
            if (payload->index and this->expression_contains_call_to(*payload->index, Name)) return true;
         }
         break;
      }
      case AstNodeKind::TableExpr: {
         auto *payload = std::get_if<TableExprPayload>(&Expr.data);
         if (payload) {
            for (const auto &field : payload->fields) {
               if (field.key and this->expression_contains_call_to(*field.key, Name)) return true;
               if (field.value and this->expression_contains_call_to(*field.value, Name)) return true;
            }
         }
         break;
      }
      default:
         break;
   }
   return false;
}

//********************************************************************************************************************
// Diagnostic Publishing: Converts internal TypeDiagnostic records to ParserDiagnostic format for output.  The
// severity depends on the parser configuration - type errors can be warnings or fatal errors depending on
// type_errors_are_fatal setting.

static void publish_type_diagnostics(ParserContext& Context, const std::vector<TypeDiagnostic>& Diagnostics)
{
   for (const auto &diag : Diagnostics) {
      ParserDiagnostic diagnostic;
      // Object class mismatches are always errors (user preference for strict type safety)
      if (diag.code IS ParserErrorCode::ObjectClassMismatch) {
         diagnostic.severity = ParserDiagnosticSeverity::Error;
      }
      else {
         diagnostic.severity = Context.config().type_errors_are_fatal ? ParserDiagnosticSeverity::Error
            : ParserDiagnosticSeverity::Warning;
      }
      diagnostic.code = diag.code;
      diagnostic.message = diag.message;
      diagnostic.token = Token::from_span(diag.location);
      Context.diagnostics().report(diagnostic);
   }
}

//********************************************************************************************************************
// Entry Point: Called from the parser after AST construction to run semantic type analysis.  Creates a TypeAnalyser
// instance, runs analysis on the module, and publishes any collected diagnostics.

void run_type_analysis(ParserContext& Context, const BlockStmt& Module)
{
   TypeAnalyser analyser(Context);
   analyser.analyse_module(Module);
   publish_type_diagnostics(Context, analyser.diagnostics());
}
