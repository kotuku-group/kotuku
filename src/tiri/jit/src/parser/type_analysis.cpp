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
#include <shared_mutex>

#include <ankerl/unordered_dense.h>
#include <kotuku/main.h>
#include "parser/parser_context.h"
#include "../runtime/lj_array.h"
#include "../runtime/lj_tab.h"
#include "../../../defs.h"

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

[[nodiscard]] static bool same_concrete_type(const InferredType &Left, const InferredType &Right)
{
   if (Left.primary != Right.primary or Left.primary IS TiriType::Any or Left.primary IS TiriType::Unknown) {
      return false;
   }
   if (Left.primary IS TiriType::Object) return Left.object_class_id IS Right.object_class_id;
   if (Left.primary IS TiriType::Struct) return Left.struct_def IS Right.struct_def;
   if (Left.primary IS TiriType::Array) return Left.array_element IS Right.array_element;
   return true;
}

[[nodiscard]] static bool type_identifier_name_is(GCstr *Name, std::string_view Text)
{
   return Name and std::string_view(strdata(Name), Name->len) IS Text;
}

[[nodiscard]] static RuntimeContractEntry global_contract_entry(const InferredType &Type, bool IsConst)
{
   RuntimeContractEntry result{
      .type = Type.primary,
      .array_element_type = Type.primary IS TiriType::Array ? Type.array_element.storage : AET::MAX,
      .flags = uint8_t(contract_flag(ContractEntryFlag::Nullable) |
         (IsConst ? contract_flag(ContractEntryFlag::Const) : 0)),
      .position = 1,
      .object_class_id = Type.primary IS TiriType::Object ? Type.object_class_id : CLASSID::NIL
   };
   if (Type.primary IS TiriType::Struct and Type.struct_def) result.constraint_name = Type.struct_def->Name;
   else if (Type.primary IS TiriType::Array) {
      if (Type.array_element.storage IS AET::STRUCT and Type.array_element.struct_def) {
         result.constraint_name = Type.array_element.struct_def->Name;
      }
      else if (Type.array_element.storage IS AET::ARRAY and Type.array_element.nested_array_identity) {
         GCstr *identity = Type.array_element.nested_array_identity;
         result.constraint_name = std::string_view(strdata(identity), identity->len);
      }
   }
   return result;
}

[[nodiscard]] static std::string global_contract_type_name(const InferredType &Type)
{
   if (Type.primary IS TiriType::Array) return std::format("array<{}>", array_element_name(Type.array_element));
   if (Type.primary IS TiriType::Struct and Type.struct_def) return std::format("struct<{}>", Type.struct_def->Name);
   if (Type.primary IS TiriType::Object and Type.object_class_id != CLASSID::NIL) {
      return std::format("obj<{}>", ResolveClassID(Type.object_class_id));
   }
   return std::string(type_name(Type.primary));
}

[[nodiscard]] static bool type_is_registered_constant(GCstr *Name)
{
   if (not Name) return false;

   std::shared_lock lock(glConstantMutex);
   return glConstantRegistry.contains(Name->hash);
}

[[nodiscard]] static GCstr * type_literal_string_key(const ExprNode &Expr)
{
   if (Expr.kind != AstNodeKind::LiteralExpr) return nullptr;
   const auto &literal = std::get<LiteralValue>(Expr.data);
   if (literal.kind != LiteralKind::String) return nullptr;
   return literal.string_value;
}

[[nodiscard]] static bool is_function_forward_declaration(const FunctionExprPayload &Function)
{
   return Function.body and Function.body->statements.empty();
}

[[nodiscard]] static std::optional<std::string> function_signature_mismatch(
   const FunctionExprPayload &Expected, const FunctionExprPayload &Actual)
{
   if (Expected.is_thunk != Actual.is_thunk) {
      return std::format("callable kind differs (expected {}, got {})",
         Expected.is_thunk ? "thunk" : "function", Actual.is_thunk ? "thunk" : "function");
   }

   if (Expected.parameters.size() != Actual.parameters.size()) {
      return std::format("parameter count differs (expected {}, got {})",
         Expected.parameters.size(), Actual.parameters.size());
   }

   if (Expected.is_vararg != Actual.is_vararg) {
      return std::format("variadic parameter marker differs (expected {}, got {})",
         Expected.is_vararg ? "variadic" : "fixed", Actual.is_vararg ? "variadic" : "fixed");
   }

   for (size_t i = 0; i < Expected.parameters.size(); ++i) {
      const FunctionParameter &expected = Expected.parameters[i];
      const FunctionParameter &actual = Actual.parameters[i];

      if (expected.type_is_explicit != actual.type_is_explicit) {
         return std::format("parameter {} annotation differs (expected {}, got {})", i + 1,
            expected.type_is_explicit ? type_name(expected.type) : "untyped",
            actual.type_is_explicit ? type_name(actual.type) : "untyped");
      }

      if (expected.type != actual.type) {
         return std::format("parameter {} has type '{}', expected '{}'",
            i + 1, type_name(actual.type), type_name(expected.type));
      }

      if (expected.required != actual.required) {
         return std::format("parameter {} required annotation differs (expected {}, got {})", i + 1,
            expected.required ? "required" : "nullable", actual.required ? "required" : "nullable");
      }

      if (expected.struct_def != actual.struct_def) {
         std::string_view expected_name = expected.struct_def ? expected.struct_def->Name : "struct";
         std::string_view actual_name = actual.struct_def ? actual.struct_def->Name : "struct";
         return std::format("parameter {} has struct type '{}', expected '{}'",
            i + 1, actual_name, expected_name);
      }
      if (not (expected.array_element IS actual.array_element)) {
         return std::format("parameter {} has array type 'array<{}>', expected 'array<{}>'", i + 1,
            array_element_name(actual.array_element), array_element_name(expected.array_element));
      }
   }

   const FunctionReturnTypes &expected_results = Expected.return_types;
   const FunctionReturnTypes &actual_results = Actual.return_types;
   if (expected_results.is_explicit != actual_results.is_explicit) {
      return std::format("return annotation differs (expected {}, got {})",
         expected_results.is_explicit ? "explicit" : "inferred",
         actual_results.is_explicit ? "explicit" : "inferred");
   }

   if (expected_results.count != actual_results.count) {
      return std::format("return count differs (expected {}, got {})",
         expected_results.count, actual_results.count);
   }

   if (expected_results.is_variadic != actual_results.is_variadic) {
      return std::format("variadic return marker differs (expected {}, got {})",
         expected_results.is_variadic ? "variadic" : "fixed",
         actual_results.is_variadic ? "variadic" : "fixed");
   }

   size_t result_count = std::min<size_t>(expected_results.count, MAX_RETURN_TYPES);
   for (size_t i = 0; i < result_count; ++i) {
      if (expected_results.types[i] != actual_results.types[i]) {
         return std::format("return {} has type '{}', expected '{}'",
            i + 1, type_name(actual_results.types[i]), type_name(expected_results.types[i]));
      }
      if (expected_results.required[i] != actual_results.required[i]) {
         return std::format("return {} required annotation differs (expected {}, got {})", i + 1,
            expected_results.required[i] ? "required" : "nullable",
            actual_results.required[i] ? "required" : "nullable");
      }

      if (expected_results.struct_defs[i] != actual_results.struct_defs[i]) {
         std::string_view expected_name = expected_results.struct_defs[i] ?
            expected_results.struct_defs[i]->Name : "struct";
         std::string_view actual_name = actual_results.struct_defs[i] ?
            actual_results.struct_defs[i]->Name : "struct";
         return std::format("return {} has struct type '{}', expected '{}'",
            i + 1, actual_name, expected_name);
      }
      if (not (expected_results.array_elements[i] IS actual_results.array_elements[i])) {
         return std::format("return {} has array type 'array<{}>', expected 'array<{}>'", i + 1,
            array_element_name(actual_results.array_elements[i]),
            array_element_name(expected_results.array_elements[i]));
      }
   }

   return std::nullopt;
}

// Helper to check if type tracing is enabled

[[nodiscard]] inline bool should_trace_types(lua_State *L)
{
   return (L->script->JitOptions & JOF::TRACE_TYPES) != JOF::NIL;
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
   void analyse_module(BlockStmt &);

   // Access collected type diagnostics after analysis
   [[nodiscard]] const std::vector<TypeDiagnostic> & diagnostics() const { return this->diagnostics_; }

   // Publish fixed global types to the lexer state so the IR emitter can specialise global member access
   void publish_global_type_hints(LexState &) const;

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
   void analyse_block(BlockStmt &);
   void analyse_statement(StmtNode &);
   bool lower_array_length_range(StmtNode &);
   void lower_unanalysed_block(BlockStmt &);
   void lower_unanalysed_except_clause(ExceptClause &);
   void lower_unanalysed_expression(ExprNode &);
   void lower_unanalysed_function(const FunctionExprPayload &);
   void lower_unanalysed_statement(StmtNode &);
   void analyse_assignment(const AssignmentStmtPayload &);
   void analyse_local_decl(const LocalDeclStmtPayload &);
   void analyse_global_decl(const GlobalDeclStmtPayload &);
   void discover_global_decl_policy(const GlobalDeclStmtPayload &, bool PublishStaticPolicy);
   void analyse_local_function(const LocalFunctionStmtPayload &);
   void analyse_function_stmt(const FunctionStmtPayload &);
   void declare_local_function(GCstr *, const FunctionExprPayload *, SourceSpan);
   void analyse_function_payload(const FunctionExprPayload &, GCstr *Name = nullptr);
   void analyse_expression(const ExprNode &);
   void analyse_call_expr(const CallExprPayload &, SourceSpan);
   [[nodiscard]] bool is_global_environment_reference(const ExprNode &) const;
   [[nodiscard]] GCstr * protected_global_store_key(const ExprNode &) const;
   void report_protected_global_override(GCstr *, SourceSpan);
   void report_dynamic_destination_required(GCstr *, SourceSpan, bool IsGlobal);

   // Argument type checking for function calls
   void check_arguments(const FunctionExprPayload &, const CallExprPayload &, SourceSpan);
   void check_argument_type(const ExprNode &, const FunctionParameter &, size_t);

   // Type inference - determines types from expressions and context
   [[nodiscard]] InferredType infer_expression_type(const ExprNode &);
   [[nodiscard]] InferredType refine_static_expression_type(const ExprNode &, InferredType) const;
   [[nodiscard]] bool is_variant_expression(const ExprNode &, size_t Position = 0) const;
   [[nodiscard]] bool is_table_read_expression(const ExprNode &);
   [[nodiscard]] InferredType infer_call_return_type(const ExprNode &, size_t Position) const;
   struct LocalInitialiser {
      InferredType type;
      const ExprNode *expression = nullptr;
   };
   [[nodiscard]] std::optional<LocalInitialiser> infer_local_initialiser(const ExprNodeList &, size_t Position);
   [[nodiscard]] InferredType analyse_annotated_local_binding(
      const Identifier &, const ExprNodeList &, size_t Position);

   // Symbol resolution - looks up variables and functions in scope stack
   [[nodiscard]] std::optional<InferredType> resolve_identifier(GCstr *) const;
   [[nodiscard]] const FunctionExprPayload * resolve_call_target(const CallExprPayload &) const;
   [[nodiscard]] const FunctionExprPayload * resolve_function(GCstr *) const;

   // Const checking - checks if a local variable has <const> attribute
   [[nodiscard]] bool is_local_const(GCstr *) const;

   // Type fixation - locks variable type after first concrete assignment
   void fix_local_type(GCstr *, StaticBindingID, TiriType, CLASSID ObjectClassId = CLASSID::NIL,
      struct_record *StructDef = nullptr, ArrayElementDescriptor ArrayElement = {});
   void publish_binding_type(StaticBindingID, const InferredType &);

   // Usage tracking - marks variables as used for unused variable detection
   void mark_identifier_used(GCstr *);

   // Diagnostic recording - stamps the active file index so diagnostics raised inside imported
   // statements are attributed to the imported file rather than the importing one
   void record_diagnostic(TypeDiagnostic Diag) {
      Diag.file_index = this->current_file_index_;
      this->diagnostics_.push_back(std::move(Diag));
   }

   #ifdef INCLUDE_TIPS
   // Tip gating - tips carry no file attribution, so they are suppressed entirely while analysing
   // imported statements to avoid misattributing library internals to the importing file.  They are also
   // suppressed during reduced traversal, which declares variables without observing their uses and would
   // otherwise report false unused-variable tips.
   [[nodiscard]] bool should_emit_tip(uint8_t Priority) const {
      return this->import_depth_ IS 0 and this->unanalysed_depth_ IS 0 and this->ctx_.should_emit_tip(Priority);
   }

   void emit_tip(uint8_t Priority, TipCategory Category, std::string Message, const Token &Location) {
      if (this->import_depth_ != 0 or this->unanalysed_depth_ != 0) return;
      this->ctx_.emit_tip(Priority, Category, std::move(Message), Location);
   }
   #endif

   // RAII guard for descending into an imported file's inlined body: swaps the active file index
   // and tracks import depth so early returns in analysis helpers cannot leave stale state
   struct ImportGuard {
      TypeAnalyser &analyser;
      uint8_t saved_file_index;

      ImportGuard(TypeAnalyser &Analyser, uint8_t FileIndex)
         : analyser(Analyser), saved_file_index(Analyser.current_file_index_) {
         Analyser.current_file_index_ = FileIndex;
         Analyser.import_depth_++;
      }

      ~ImportGuard() {
         this->analyser.current_file_index_ = this->saved_file_index;
         this->analyser.import_depth_--;
      }

      ImportGuard(const ImportGuard &) = delete;
      ImportGuard & operator=(const ImportGuard &) = delete;
   };

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

   // Choose expression fallback detection - warns when no else or unguarded wildcard can handle no-match cases
   void check_choose_missing_fallback(const ChooseExprPayload &, SourceSpan);

   // Track a global variable declaration
   void track_global(GCstr *Name);
   #endif

   // Global variable type tracking - stores type information for variables declared with 'global' keyword
   struct GlobalTypeInfo {
      InferredType type{};
      SourceSpan location{};
      const FunctionExprPayload* function = nullptr;  // Non-null if declared as global function
      const FunctionExprPayload* forward_declaration = nullptr; // Original empty declaration used as the contract
      bool is_const = false;  // True if declared with <const> attribute
      bool implicit = false;  // True if inferred from a plain assignment rather than a 'global' declaration
      bool environment_contract = false; // Persisted policy already enforced by the environment store boundary.
      GlobalContractPolicy contract_policy = GlobalContractPolicy::Advisory;
   };

   struct EnvironmentGlobalFacts {
      bool exists = false;
      bool is_const = false;
      std::optional<InferredType> exact_type;
      GlobalContractPolicy contract_policy = GlobalContractPolicy::Advisory;
   };

   void declare_global(GCstr *Name, const InferredType &Type, SourceSpan Location, bool IsConst = false,
      GlobalContractPolicy ContractPolicy = GlobalContractPolicy::Advisory);
   void declare_global_function(GCstr *Name, const FunctionExprPayload *Function, SourceSpan Location);
   void record_implicit_global(GCstr *Name, InferredType Type, SourceSpan Location);
   [[nodiscard]] EnvironmentGlobalFacts environment_global_facts(GCstr *Name) const;
   void seed_environment_global(GCstr *Name, SourceSpan Location);
   [[nodiscard]] std::optional<InferredType> lookup_global_type(GCstr *Name) const;
   [[nodiscard]] bool is_global_const(GCstr *Name) const;
   [[nodiscard]] bool is_implicit_global(GCstr *Name) const;
   [[nodiscard]] bool has_environment_contract(GCstr *Name) const;
   void fix_global_type(GCstr *Name, TiriType Type, CLASSID ObjectClassId = CLASSID::NIL,
      struct_record *StructDef = nullptr, ArrayElementDescriptor ArrayElement = {});
   void mark_dynamic_ingress(GCstr *Name, bool IsGlobal, bool RequiresDestination = true);
   void degrade_global_type(GCstr *Name);
   void invalidate_global_flow_policy(GCstr *Name, SourceSpan Location);

   ParserContext &ctx_;                             // Parser context for diagnostics and lexer access
   std::vector<TypeCheckScope> scope_stack_{};      // Stack of scopes for variable tracking
   std::vector<FunctionContext> function_stack_{};  // Stack of function contexts for return type tracking
   std::vector<TypeDiagnostic> diagnostics_{};      // Collected type errors and warnings
   uint32_t loop_depth_{0};                         // Current loop nesting depth for performance tip
   uint32_t import_depth_{0};                       // Import nesting depth; non-zero suppresses tips
   uint32_t unanalysed_depth_{0};                   // Reduced-traversal nesting depth; non-zero suppresses tips
   uint8_t current_file_index_{0};                  // FileSource index of the file being analysed
   ankerl::unordered_dense::map<GCstr*, GlobalTypeInfo> global_types_{};  // Type info for global variables
   ankerl::unordered_dense::set<StaticBindingID> explicit_variant_bindings_{};
   ankerl::unordered_dense::set<StaticBindingID> implicit_local_bindings_{};
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
   kt::Log("TypeCheck").msg("[%d] infer %.*s -> %.*s", Line.lineNumber(), int(Context.size()), Context.data(),
      int(type_str.size()), type_str.data());
}

void TypeAnalyser::trace_fix(BCLine Line, GCstr* Name, TiriType Type) const
{
   if (not this->trace_enabled()) return;
   std::string_view name_view = Name ? std::string_view(strdata(Name), Name->len) : std::string_view("<unknown>");
   auto type_str = type_name(Type);
   kt::Log("TypeCheck").msg("[%d] fix '%.*s' -> %.*s", Line.lineNumber(), int(name_view.size()), name_view.data(),
      int(type_str.size()), type_str.data());
}

void TypeAnalyser::trace_decl(BCLine Line, GCstr* Name, TiriType Type, bool IsFixed) const
{
   if (not this->trace_enabled()) return;
   std::string_view name_view = Name ? std::string_view(strdata(Name), Name->len) : std::string_view("<unknown>");
   auto type_str = type_name(Type);
   kt::Log("TypeCheck").msg("[%d] decl '%.*s': %.*s%s", Line.lineNumber(), int(name_view.size()), name_view.data(),
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
   if (not this->should_emit_tip(2)) return;
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
         this->emit_tip(2, TipCategory::CodeQuality,
            std::format("Variable '{}' shadows a variable in an outer scope", name_view),
            Token::from_span(Location, TokenKind::Identifier));
         return;  // Only report once per variable
      }

      // Also check parameters in this scope
      auto param = scope.lookup_parameter_type(Name);
      if (param) {
         std::string_view name_view(strdata(Name), Name->len);
         this->emit_tip(2, TipCategory::CodeQuality,
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
   if (not this->should_emit_tip(2)) return;
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
   this->emit_tip(2, TipCategory::Performance,
      std::format("Global '{}' accessed in loop; consider caching in a local variable for better JIT performance",
         name_view),
      Token::from_span(Location, TokenKind::Identifier));
}

//********************************************************************************************************************
// Function Definition in Loop Detection: Warns when function expressions (closures) are defined inside loops.

void TypeAnalyser::check_function_in_loop(SourceSpan Location)
{
   if (not this->should_emit_tip(2)) return;
   if (this->loop_depth_ IS 0) return;

   this->emit_tip(2, TipCategory::Performance,
      "Function defined inside loop; consider moving it outside the loop for better performance",
      Token::from_span(Location, TokenKind::Function));
}

//********************************************************************************************************************
// String Concatenation in Loop Detection:  Warns when string concatenation (..) is used inside loops. Each
// concatenation creates a new intermediate string object, which is inefficient when building strings iteratively.
// For building strings in loops, array.concat() is more efficient as it allocates only once.

void TypeAnalyser::check_concat_in_loop(SourceSpan Location)
{
   if (not this->should_emit_tip(2)) return;
   if (this->loop_depth_ IS 0) return;

   this->emit_tip(2, TipCategory::Performance,
      "String concatenation in loop; consider using array.concat() for better performance",
      Token::from_span(Location, TokenKind::Cat));
}

//********************************************************************************************************************
// Choose Missing Fallback Detection: Warns when a choose expression has no fallback branch.  Without an else branch
// or unguarded wildcard, unmatched values evaluate to nil, which is easy to miss in assignment expressions.

void TypeAnalyser::check_choose_missing_fallback(const ChooseExprPayload &Payload, SourceSpan Location)
{
   if (not this->should_emit_tip(1)) return;

   for (const ChooseCase &case_item : Payload.cases) {
      if (case_item.is_else or (case_item.is_wildcard and not case_item.guard)) return;
   }

   this->emit_tip(1, TipCategory::TypeSafety,
      "Choose expression has no else branch; unmatched values evaluate to nil",
      Token::from_span(Location, TokenKind::Choose));
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
   if (this->should_emit_tip(2)) {
      auto unused = this->scope_stack_.back().get_unused_variables();
      for (const auto& var : unused) {
         std::string_view name_view(strdata(var.name), var.name->len);
         // Skip compiler-generated hidden bindings (e.g. \x1fmodfn:... module-function locals). Their names carry an
         // internal encoding that must never surface in user-facing diagnostics.
         if (not name_view.empty() and (name_view.front() IS '\x1f')) continue;
         if (var.is_parameter) {
            this->emit_tip(2, TipCategory::CodeQuality,
               std::format("Unused function parameter '{}'", name_view),
               Token::from_span(var.location, TokenKind::Identifier));
         }
         else if (var.is_function) {
            this->emit_tip(2, TipCategory::CodeQuality,
               std::format("Unused local function '{}'", name_view),
               Token::from_span(var.location, TokenKind::Identifier));
         }
         else {
            this->emit_tip(2, TipCategory::CodeQuality,
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
   }

   this->function_stack_.push_back(ctx);
}

//********************************************************************************************************************

void TypeAnalyser::leave_function()
{
   if (this->function_stack_.empty()) return;

   FunctionContext &ctx = this->function_stack_.back();
   if (ctx.function and not ctx.expected_returns.is_explicit) {
      size_t inferred_return_count = ctx.observed_return_count;
      while (inferred_return_count > 0 and
          ctx.inferred_returns[inferred_return_count - 1].state IS ReturnInferenceState::NilOnly) {
         inferred_return_count--;
      }

      for (size_t i = 0; i < inferred_return_count; ++i) {
         const InferredReturnPosition &position = ctx.inferred_returns[i];
         if (position.state != ReturnInferenceState::Dynamic) continue;

         TypeDiagnostic diag;
         diag.location = position.dynamic_location;
         diag.actual = position.dynamic_type;
         diag.code = ParserErrorCode::ReturnTypeRequired;
         if (position.concrete.primary != TiriType::Any and position.concrete.primary != TiriType::Unknown) {
            diag.expected = position.concrete.primary;
            diag.message = std::format(
               "cannot infer result {} as '{}' from a dynamic value; declare ':{}' to check it at runtime or "
               "':any' to allow a variant result",
               i + 1, type_name(position.concrete.primary), type_name(position.concrete.primary));
         }
         else {
            diag.message = std::format(
               "cannot infer result {} from a dynamic value; declare a concrete result type to check it at "
               "runtime or ':any' to allow a variant result", i + 1);
         }
         this->record_diagnostic(std::move(diag));
         ctx.inference_failed = true;
      }

      if (inferred_return_count > 0 and not ctx.inference_failed) {
         #ifdef INCLUDE_TIPS
         if (this->should_emit_tip(1)) {
            for (size_t i = 0; i < inferred_return_count; ++i) {
               const InferredReturnPosition &position = ctx.inferred_returns[i];
               if (position.state != ReturnInferenceState::ExplicitAny) continue;

               this->emit_tip(1, TipCategory::TypeSafety,
                  "Function infers an 'any' result type; declare ':any' to make the variant result explicit",
                  Token::from_span(position.location, TokenKind::ReturnToken));
               break;
            }
         }
         #endif

         FunctionReturnTypes &published = ctx.function->return_types;
         published.count = uint8_t(inferred_return_count);
         published.is_inferred = true;
         published.is_variadic = false;
         for (size_t i = 0; i < inferred_return_count; ++i) {
            const InferredReturnPosition &position = ctx.inferred_returns[i];
            if (position.state IS ReturnInferenceState::NilOnly) {
               published.types[i] = TiriType::Nil;
               published.object_class_ids[i] = CLASSID::NIL;
               published.struct_defs[i] = nullptr;
            }
            else {
               published.types[i] = position.concrete.primary;
               published.object_class_ids[i] = position.concrete.object_class_id;
               published.struct_defs[i] = position.concrete.struct_def;
               published.array_elements[i] = position.concrete.array_element;
            }
         }
      }
   }

   this->function_stack_.pop_back();
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

void TypeAnalyser::analyse_module(BlockStmt &Module)
{
   // For loadFile sources the whole chunk carries a single non-zero FileSource index
   this->current_file_index_ = this->ctx_.lex().current_file_index;
   this->push_scope();
   this->analyse_block(Module);
   this->pop_scope();
}

void TypeAnalyser::analyse_block(BlockStmt &Block)
{
   for (auto &statement : Block.statements) {
      if (statement) this->analyse_statement(*statement);
   }
}

//********************************************************************************************************************
// Lowers `{0 to #array}` only after semantic analysis has proved that the length operand is a native array.  Keeping
// unknown and non-array operands on the generic range path preserves custom __len direction and validation semantics.

bool TypeAnalyser::lower_array_length_range(StmtNode &Statement)
{
   if (Statement.kind != AstNodeKind::GenericForStmt) return false;

   auto *generic = std::get_if<GenericForStmtPayload>(&Statement.data);
   if (not generic or generic->names.size() != 1 or generic->iterators.size() != 1 or not generic->body) return false;

   ExprNode *iterator = generic->iterators[0].get();
   if (not iterator or iterator->kind != AstNodeKind::CallExpr) return false;

   auto *call = std::get_if<CallExprPayload>(&iterator->data);
   if (not call or not call->arguments.empty()) return false;

   auto *direct = std::get_if<DirectCallTarget>(&call->target);
   if (not direct or not direct->callable or direct->callable->kind != AstNodeKind::RangeExpr) return false;

   auto *range = std::get_if<RangeExprPayload>(&direct->callable->data);
   if (not range or range->inclusive or range->step or not range->start or not range->stop) return false;

   const auto *start = std::get_if<LiteralValue>(&range->start->data);
   if (range->start->kind != AstNodeKind::LiteralExpr or not start or start->kind != LiteralKind::Number or
       start->number_value != 0) {
      return false;
   }

   auto *length = std::get_if<UnaryExprPayload>(&range->stop->data);
   if (range->stop->kind != AstNodeKind::UnaryExpr or not length or
       length->op != AstUnaryOperator::Length or not length->operand) {
      return false;
   }

   InferredType operand_type = this->infer_expression_type(*length->operand);
   if (operand_type.primary != TiriType::Array) return false;

   SourceSpan span = direct->callable->span;
   Identifier control = std::move(generic->names[0]);
   ExprNodePtr start_expr = std::move(range->start);
   ExprNodePtr stop_expr = std::move(range->stop);
   ExprNodePtr one_expr = make_literal_expr(span, LiteralValue::number(1));
   ExprNodePtr final_stop_expr = make_binary_expr(
      span, AstBinaryOperator::Subtract, std::move(stop_expr), std::move(one_expr));
   ExprNodePtr step_expr = make_literal_expr(span, LiteralValue::number(1));
   std::unique_ptr<BlockStmt> body = std::move(generic->body);

   Statement.kind = AstNodeKind::NumericForStmt;
   Statement.data.emplace<NumericForStmtPayload>(std::move(control), std::move(start_expr),
      std::move(final_stop_expr), std::move(step_expr), std::move(body));
   return true;
}

//********************************************************************************************************************
// Some statement forms deliberately bypass ordinary type diagnostics so runtime type failures inside them remain
// catchable.  This reduced traversal tracks only the lexical types needed for safe array-range lowering.  Each
// entry point raises unanalysed_depth_ so tip emission stays suppressed: the traversal declares variables but
// never observes their uses, so unused-variable reporting on scope exit would be a false positive.

void TypeAnalyser::lower_unanalysed_block(BlockStmt &Block)
{
   this->unanalysed_depth_++;
   this->push_scope();
   for (auto &statement : Block.statements) {
      if (statement) this->lower_unanalysed_statement(*statement);
   }
   this->pop_scope();
   this->unanalysed_depth_--;
}

//********************************************************************************************************************

void TypeAnalyser::lower_unanalysed_except_clause(ExceptClause &Clause)
{
   if (not Clause.block) return;

   this->unanalysed_depth_++;
   this->push_scope();
   if (Clause.exception_var and Clause.exception_var->symbol) {
      this->current_scope().declare_local(
         Clause.exception_var->symbol, InferredType(TiriType::Any), Clause.exception_var->span);
   }
   for (auto &statement : Clause.block->statements) {
      if (statement) this->lower_unanalysed_statement(*statement);
   }
   this->pop_scope();
   this->unanalysed_depth_--;
}

//********************************************************************************************************************

void TypeAnalyser::lower_unanalysed_function(const FunctionExprPayload &Function)
{
   this->unanalysed_depth_++;
   this->push_scope();
   for (const FunctionParameter &param : Function.parameters) {
      this->current_scope().declare_parameter(
         param.name.symbol, param.type, param.struct_def, param.required, param.name.span);
   }
   if (Function.body) {
      for (auto &statement : Function.body->statements) {
         if (statement) this->lower_unanalysed_statement(*statement);
      }
   }
   this->pop_scope();
   this->unanalysed_depth_--;
}

//********************************************************************************************************************

void TypeAnalyser::lower_unanalysed_expression(ExprNode &Expression)
{
   auto lower = [this](ExprNodePtr &Child) {
      if (Child) this->lower_unanalysed_expression(*Child);
   };

   switch (Expression.kind) {
      case AstNodeKind::UnaryExpr:
         lower(std::get<UnaryExprPayload>(Expression.data).operand);
         break;
      case AstNodeKind::UpdateExpr:
         lower(std::get<UpdateExprPayload>(Expression.data).target);
         break;
      case AstNodeKind::TypeTestExpr:
         lower(std::get<TypeTestExprPayload>(Expression.data).value);
         break;
      case AstNodeKind::BinaryExpr: {
         auto &payload = std::get<BinaryExprPayload>(Expression.data);
         lower(payload.left);
         lower(payload.right);
         break;
      }
      case AstNodeKind::ComparisonChainExpr:
         for (auto &operand : std::get<ComparisonChainExprPayload>(Expression.data).operands) lower(operand);
         break;
      case AstNodeKind::TernaryExpr: {
         auto &payload = std::get<TernaryExprPayload>(Expression.data);
         lower(payload.condition);
         lower(payload.if_true);
         lower(payload.if_false);
         break;
      }
      case AstNodeKind::PresenceExpr:
         lower(std::get<PresenceExprPayload>(Expression.data).value);
         break;
      case AstNodeKind::PipeExpr: {
         auto &payload = std::get<PipeExprPayload>(Expression.data);
         lower(payload.lhs);
         lower(payload.rhs_call);
         break;
      }
      case AstNodeKind::CallExpr:
      case AstNodeKind::SafeCallExpr: {
         auto &payload = std::get<CallExprPayload>(Expression.data);
         if (auto *direct = std::get_if<DirectCallTarget>(&payload.target)) lower(direct->callable);
         for (auto &argument : payload.arguments) lower(argument);
         break;
      }
      case AstNodeKind::MemberExpr:
         lower(std::get<MemberExprPayload>(Expression.data).table);
         break;
      case AstNodeKind::IndexExpr: {
         auto &payload = std::get<IndexExprPayload>(Expression.data);
         lower(payload.table);
         lower(payload.index);
         break;
      }
      case AstNodeKind::SafeMemberExpr:
         lower(std::get<SafeMemberExprPayload>(Expression.data).table);
         break;
      case AstNodeKind::SafeIndexExpr: {
         auto &payload = std::get<SafeIndexExprPayload>(Expression.data);
         lower(payload.table);
         lower(payload.index);
         break;
      }
      case AstNodeKind::ResultFilterExpr:
         lower(std::get<ResultFilterPayload>(Expression.data).expression);
         break;
      case AstNodeKind::TableExpr:
         for (auto &field : std::get<TableExprPayload>(Expression.data).fields) {
            lower(field.key);
            lower(field.value);
         }
         break;
      case AstNodeKind::FunctionExpr:
         this->lower_unanalysed_function(std::get<FunctionExprPayload>(Expression.data));
         break;
      case AstNodeKind::DeferredExpr:
         lower(std::get<DeferredExprPayload>(Expression.data).inner);
         break;
      case AstNodeKind::RangeExpr: {
         auto &payload = std::get<RangeExprPayload>(Expression.data);
         lower(payload.start);
         lower(payload.stop);
         lower(payload.step);
         break;
      }
      case AstNodeKind::ChooseExpr: {
         auto &payload = std::get<ChooseExprPayload>(Expression.data);
         lower(payload.scrutinee);
         for (auto &item : payload.scrutinee_tuple) lower(item);
         for (auto &choice : payload.cases) {
            lower(choice.pattern);
            for (auto &item : choice.tuple_patterns) lower(item);
            lower(choice.guard);
            lower(choice.result);
            if (choice.result_stmt) this->lower_unanalysed_statement(*choice.result_stmt);
         }
         break;
      }
      default:
         break;
   }
}

//********************************************************************************************************************

void TypeAnalyser::lower_unanalysed_statement(StmtNode &Statement)
{
   this->unanalysed_depth_++;
   this->lower_array_length_range(Statement);

   switch (Statement.kind) {
      case AstNodeKind::AssignmentStmt: {
         auto *payload = std::get_if<AssignmentStmtPayload>(&Statement.data);
         if (payload) {
            for (auto &target : payload->targets) this->lower_unanalysed_expression(*target);
            for (auto &value : payload->values) this->lower_unanalysed_expression(*value);
         }
         break;
      }
      case AstNodeKind::LocalDeclStmt: {
         auto *payload = std::get_if<LocalDeclStmtPayload>(&Statement.data);
         if (not payload) break;

         std::vector<InferredType> inferred_types;
         inferred_types.reserve(payload->names.size());
         for (size_t i = 0; i < payload->names.size(); ++i) {
            InferredType inferred;
            if (payload->names[i].type != TiriType::Unknown) {
               inferred.primary = payload->names[i].type;
               inferred.struct_def = payload->names[i].struct_def;
               inferred.is_fixed = inferred.primary != TiriType::Any;
            }
            else if (i < payload->values.size() and payload->values[i]) {
               // Unlike analyse_local_decl this marks Unknown inferences as fixed, which is harmless here:
               // these scopes only feed the Array check in lower_array_length_range, never diagnostics.
               inferred = this->infer_expression_type(*payload->values[i]);
               inferred.is_fixed = inferred.primary != TiriType::Nil and inferred.primary != TiriType::Any;
            }
            inferred_types.push_back(inferred);
         }

         for (size_t i = 0; i < payload->names.size(); ++i) {
            const Identifier &name = payload->names[i];
            this->current_scope().declare_local(name.symbol, inferred_types[i], name.span, name.has_const);
         }
         for (auto &value : payload->values) this->lower_unanalysed_expression(*value);
         break;
      }
      case AstNodeKind::GlobalDeclStmt: {
         auto *payload = std::get_if<GlobalDeclStmtPayload>(&Statement.data);
         if (payload) {
            this->discover_global_decl_policy(*payload, false);
            for (auto &value : payload->values) this->lower_unanalysed_expression(*value);
         }
         break;
      }
      case AstNodeKind::LocalFunctionStmt: {
         auto *payload = std::get_if<LocalFunctionStmtPayload>(&Statement.data);
         if (payload and payload->function) {
            this->current_scope().declare_function(
               payload->name.symbol, payload->function.get(), payload->name.span);
            this->lower_unanalysed_function(*payload->function);
         }
         break;
      }
      case AstNodeKind::FunctionStmt: {
         auto *payload = std::get_if<FunctionStmtPayload>(&Statement.data);
         if (payload and payload->function) this->lower_unanalysed_function(*payload->function);
         break;
      }
      case AstNodeKind::IfStmt: {
         auto *payload = std::get_if<IfStmtPayload>(&Statement.data);
         if (payload) {
            for (auto &clause : payload->clauses) {
               if (clause.condition) this->lower_unanalysed_expression(*clause.condition);
               if (clause.block) this->lower_unanalysed_block(*clause.block);
            }
         }
         break;
      }
      case AstNodeKind::WhileStmt:
      case AstNodeKind::RepeatStmt: {
         auto *payload = std::get_if<LoopStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->condition) this->lower_unanalysed_expression(*payload->condition);
            if (payload->body) this->lower_unanalysed_block(*payload->body);
         }
         break;
      }
      case AstNodeKind::NumericForStmt: {
         auto *payload = std::get_if<NumericForStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->start) this->lower_unanalysed_expression(*payload->start);
            if (payload->stop) this->lower_unanalysed_expression(*payload->stop);
            if (payload->step) this->lower_unanalysed_expression(*payload->step);
         }
         if (payload and payload->body) {
            this->push_scope();
            InferredType control_type(TiriType::Num);
            this->current_scope().declare_local(
               payload->control.symbol, control_type, payload->control.span);
            for (auto &statement : payload->body->statements) {
               if (statement) this->lower_unanalysed_statement(*statement);
            }
            this->pop_scope();
         }
         break;
      }
      case AstNodeKind::RangeForStmt: {
         auto *payload = std::get_if<RangeForStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->start) this->lower_unanalysed_expression(*payload->start);
            if (payload->stop) this->lower_unanalysed_expression(*payload->stop);
            if (payload->step) this->lower_unanalysed_expression(*payload->step);
         }
         if (payload and payload->body) {
            this->push_scope();
            InferredType control_type(TiriType::Num);
            this->current_scope().declare_local(
               payload->control.symbol, control_type, payload->control.span);
            for (auto &statement : payload->body->statements) {
               if (statement) this->lower_unanalysed_statement(*statement);
            }
            this->pop_scope();
         }
         break;
      }
      case AstNodeKind::GenericForStmt: {
         auto *payload = std::get_if<GenericForStmtPayload>(&Statement.data);
         if (payload) {
            for (auto &iterator : payload->iterators) this->lower_unanalysed_expression(*iterator);
         }
         if (payload and payload->body) {
            this->push_scope();
            for (const Identifier &name : payload->names) {
               this->current_scope().declare_local(name.symbol, InferredType(TiriType::Any), name.span);
            }
            for (auto &statement : payload->body->statements) {
               if (statement) this->lower_unanalysed_statement(*statement);
            }
            this->pop_scope();
         }
         break;
      }
      case AstNodeKind::ReturnStmt: {
         auto *payload = std::get_if<ReturnStmtPayload>(&Statement.data);
         if (payload) {
            for (auto &value : payload->values) this->lower_unanalysed_expression(*value);
         }
         break;
      }
      case AstNodeKind::DeferStmt: {
         auto *payload = std::get_if<DeferStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->callable) this->lower_unanalysed_function(*payload->callable);
            for (auto &argument : payload->arguments) this->lower_unanalysed_expression(*argument);
         }
         break;
      }
      case AstNodeKind::DoStmt: {
         auto *payload = std::get_if<DoStmtPayload>(&Statement.data);
         if (payload and payload->block) this->lower_unanalysed_block(*payload->block);
         break;
      }
      case AstNodeKind::ContextStmt: {
         auto *payload = std::get_if<ContextStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->reference) this->lower_unanalysed_expression(*payload->reference);
            if (payload->block) this->lower_unanalysed_block(*payload->block);
         }
         break;
      }
      case AstNodeKind::ConditionalShorthandStmt: {
         auto *payload = std::get_if<ConditionalShorthandStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->condition) this->lower_unanalysed_expression(*payload->condition);
            if (payload->body) this->lower_unanalysed_statement(*payload->body);
         }
         break;
      }
      case AstNodeKind::TryExceptStmt: {
         auto *payload = std::get_if<TryExceptPayload>(&Statement.data);
         if (payload) {
            if (payload->try_block) this->lower_unanalysed_block(*payload->try_block);
            for (auto &clause : payload->except_clauses) {
               for (auto &filter : clause.filter_codes) this->lower_unanalysed_expression(*filter);
               this->lower_unanalysed_except_clause(clause);
            }
            if (payload->success_block) this->lower_unanalysed_block(*payload->success_block);
         }
         break;
      }
      case AstNodeKind::CheckallStmt: {
         auto *payload = std::get_if<CheckallStmtPayload>(&Statement.data);
         if (payload and payload->block) this->lower_unanalysed_block(*payload->block);
         break;
      }
      case AstNodeKind::RaiseStmt: {
         auto *payload = std::get_if<RaiseStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->error_code) this->lower_unanalysed_expression(*payload->error_code);
            if (payload->message) this->lower_unanalysed_expression(*payload->message);
         }
         break;
      }
      case AstNodeKind::CheckStmt: {
         auto *payload = std::get_if<CheckStmtPayload>(&Statement.data);
         if (payload and payload->error_code) this->lower_unanalysed_expression(*payload->error_code);
         break;
      }
      case AstNodeKind::ImportStmt: {
         auto *payload = std::get_if<ImportStmtPayload>(&Statement.data);
         if (payload) {
            for (auto &entry : payload->entries) {
               if (not entry.inlined_body) continue;
               ImportGuard guard(*this, entry.file_source_idx);
               this->lower_unanalysed_block(*entry.inlined_body);
            }
         }
         break;
      }
      case AstNodeKind::NamespaceStmt: {
         auto *payload = std::get_if<NamespaceStmtPayload>(&Statement.data);
         if (payload and payload->initialiser) this->lower_unanalysed_expression(*payload->initialiser);
         break;
      }
      case AstNodeKind::WithStmt: {
         auto *payload = std::get_if<WithStmtPayload>(&Statement.data);
         if (payload) {
            for (auto &object : payload->objects) this->lower_unanalysed_expression(*object);
            if (payload->block) this->lower_unanalysed_block(*payload->block);
         }
         break;
      }
      case AstNodeKind::ExpressionStmt: {
         auto *payload = std::get_if<ExpressionStmtPayload>(&Statement.data);
         if (payload and payload->expression) this->lower_unanalysed_expression(*payload->expression);
         break;
      }
      default:
         break;
   }

   this->unanalysed_depth_--;
}

//********************************************************************************************************************
// Statement Analysis
//
// Dispatches to the appropriate handler based on statement type.
// Each handler may push/pop scopes, declare variables, or analyse nested expressions.

void TypeAnalyser::analyse_statement(StmtNode &Statement)
{
   this->lower_array_length_range(Statement);

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
      case AstNodeKind::RangeForStmt: {
         auto *payload = std::get_if<RangeForStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->start) this->analyse_expression(*payload->start);
            if (payload->stop) this->analyse_expression(*payload->stop);
            if (payload->step) this->analyse_expression(*payload->step);
            if (payload->body) {
               this->push_scope();
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
      case AstNodeKind::ContextStmt: {
         auto *payload = std::get_if<ContextStmtPayload>(&Statement.data);
         if (payload) {
            if (payload->reference) this->analyse_expression(*payload->reference);
            if (payload->block) {
               this->push_scope();
               this->analyse_block(*payload->block);
               this->pop_scope();
            }
         }
         break;
      }
      case AstNodeKind::ConditionalShorthandStmt: {
         auto *payload = std::get_if<ConditionalShorthandStmtPayload>(&Statement.data);
         if (payload and payload->body) this->lower_unanalysed_statement(*payload->body);
         break;
      }
      case AstNodeKind::TryExceptStmt: {
         auto *payload = std::get_if<TryExceptPayload>(&Statement.data);
         if (payload) {
            if (payload->try_block) this->lower_unanalysed_block(*payload->try_block);
            for (auto &clause : payload->except_clauses) {
               this->lower_unanalysed_except_clause(clause);
            }
            if (payload->success_block) this->lower_unanalysed_block(*payload->success_block);
         }
         break;
      }
      case AstNodeKind::CheckallStmt: {
         auto *payload = std::get_if<CheckallStmtPayload>(&Statement.data);
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
      case AstNodeKind::ImportStmt: {
         // Imports are inlined at compile time and never independently compiled, so their bodies
         // must be analysed here or their type errors can never surface.  Repeated imports of the
         // same library are deduplicated at parse time (subsequent entries get an empty block), so
         // descent cannot double-analyse a library.  The scope pair mirrors emit_import_entry(),
         // which wraps the inlined body in its own lexical scope - imported top-level locals must
         // not leak into the importer's scope tables.  Globals intentionally remain shared.
         auto *payload = std::get_if<ImportStmtPayload>(&Statement.data);
         if (payload) {
            for (const auto &entry : payload->entries) {
               std::optional<InferredType> namespace_type;
               if (entry.inlined_body) {
                  ImportGuard guard(*this, entry.file_source_idx);
                  this->push_scope();
                  this->analyse_block(*entry.inlined_body);
                  if (not entry.default_namespace.empty()) {
                     GCstr *namespace_symbol = lj_str_new(
                        &this->ctx_.lua(), entry.default_namespace.data(), entry.default_namespace.size());
                     namespace_type = this->resolve_identifier(namespace_symbol);
                  }
                  this->pop_scope();
               }

               // Import emission publishes the namespace as a const local after the isolated library body.  Type
               // analysis must mirror that declaration so later assignments receive a user-facing const diagnostic
               // instead of appearing to reference an unknown lexical binding.
               if (entry.namespace_name) {
                  const Identifier &name = *entry.namespace_name;
                  this->current_scope().declare_local(
                     name.symbol, namespace_type.value_or(InferredType(TiriType::Any)), name.span, name.has_const);
               }
            }
         }
         break;
      }
      case AstNodeKind::NamespaceStmt: {
         auto *payload = std::get_if<NamespaceStmtPayload>(&Statement.data);
         if (not payload) break;

         InferredType inferred(TiriType::Any);
         if (payload->initialiser) {
            this->analyse_expression(*payload->initialiser);
            inferred = this->infer_expression_type(*payload->initialiser);
            inferred = this->refine_static_expression_type(*payload->initialiser, inferred);
            if (inferred.primary != TiriType::Any and inferred.primary != TiriType::Unknown) {
               inferred.is_fixed = true;
            }
         }

         if (not payload->reuses_import_binding) {
            this->current_scope().declare_local(
               payload->name.symbol, inferred, payload->name.span, true);
            this->publish_binding_type(payload->name.binding_id, inferred);
         }
         break;
      }
      case AstNodeKind::WithStmt: {
         auto *payload = std::get_if<WithStmtPayload>(&Statement.data);
         if (payload and payload->block) this->lower_unanalysed_block(*payload->block);
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

      // Computed '_G[key]' assignment is permitted: the runtime environment mutation boundary enforces sticky
      // contracts and protected built-ins for every environment store.

      if (GCstr *protected_name = this->protected_global_store_key(target)) {
         this->report_protected_global_override(protected_name, target.span);
         continue;
      }

      // Check local and global variable assignments
      if (target.kind IS AstNodeKind::IdentifierExpr) {
         auto *name_ref = std::get_if<NameRef>(&target.data);
         if (not name_ref) continue;

         GCstr *name = name_ref->identifier.symbol;
         AssignmentTargetResolution category = name_ref->assignment_resolution;
         if (category IS AssignmentTargetResolution::Unresolved) {
            TypeDiagnostic diag;
            diag.location = target.span;
            diag.code = ParserErrorCode::InternalInvariant;
            diag.message = "assignment target reached type analysis without semantic resolution";
            this->record_diagnostic(std::move(diag));
            continue;
         }
         if (category IS AssignmentTargetResolution::Blank or
             category IS AssignmentTargetResolution::Invalid) continue;

         bool is_global = category IS AssignmentTargetResolution::ExistingGlobal;
         bool is_lexical = assignment_target_is_existing_lexical(category);

         // A plain identifier target naming a host pre-registered global is an override unless an
         // explicit local shadow or parameter is in scope.

         if (is_global and name and ((name->flags & STRFLAG_PROTECTED_GLOBAL) != 0)) {
            this->report_protected_global_override(name, target.span);
            continue;
         }

         if (is_global and type_is_registered_constant(name)) {
            TypeDiagnostic diag;
            diag.location = target.span;
            diag.code = ParserErrorCode::AssignToConstant;
            diag.message = std::format("cannot assign to constant '{}'", std::string_view(strdata(name), name->len));
            this->record_diagnostic(std::move(diag));
            continue;
         }

         const Identifier &identifier = name_ref->identifier;
         std::optional<InferredType> existing;
         bool is_const = false;
         if (is_lexical) {
            existing = this->resolve_identifier(name);
            is_const = this->is_local_const(name);
            bool valid_binding = name_ref->binding_id and identifier.binding_id IS name_ref->binding_id and
               name_ref->binding_id.raw() < this->ctx_.descriptors().binding_count();
            if (valid_binding) {
               valid_binding = this->ctx_.descriptors().binding(name_ref->binding_id).name IS name;
            }
            if (not existing or not valid_binding) {
               TypeDiagnostic diag;
               diag.location = target.span;
               diag.code = ParserErrorCode::InternalInvariant;
               diag.message = "lexical assignment target does not match an analysed binding";
               this->record_diagnostic(std::move(diag));
               continue;
            }
         }

         if (assignment_target_is_existing_storage(category) and identifier.type != TiriType::Unknown) {
            std::string_view name_view(strdata(name), name->len);
            TypeDiagnostic diag;
            diag.location = identifier.span;
            diag.code = ParserErrorCode::InvalidAssignment;
            diag.message = std::format(
               "cannot redeclare existing {} '{}' with a type annotation",
               is_global ? "global" : "local", name_view);
            this->record_diagnostic(std::move(diag));
            continue;
         }

         if (is_global) {
            this->seed_environment_global(name, target.span);
            existing = this->lookup_global_type(name);
            if (existing) is_const = this->is_global_const(name);
            else {
               InferredType inferred(TiriType::Any, false, true, true);
               if (Payload.op IS AssignmentOperator::Plain and not Payload.values.empty()) {
                  size_t source = std::min(i, Payload.values.size() - 1);
                  size_t result_position = i - source;
                  if (result_position IS 0) inferred = this->infer_expression_type(*Payload.values[source]);
                  else inferred = this->infer_call_return_type(*Payload.values[source], result_position);
               }
               this->record_implicit_global(name, inferred, target.span);
               existing = this->lookup_global_type(name);
            }
         }

         if (not existing) {
            if (category != AssignmentTargetResolution::NewLocal) {
               TypeDiagnostic diag;
               diag.location = target.span;
               diag.code = ParserErrorCode::InternalInvariant;
               diag.message = "existing assignment target has no type-analysis record";
               this->record_diagnostic(std::move(diag));
               continue;
            }
            if (name_ref->binding_id and identifier.type != TiriType::Unknown) {
               InferredType inferred = this->analyse_annotated_local_binding(identifier, Payload.values, i);
               this->current_scope().declare_local(name, inferred, target.span);
               continue;
            }

            if (name_ref->binding_id and not Payload.values.empty()) {
               size_t source = std::min(i, Payload.values.size() - 1);
               size_t result_position = i - source;
               InferredType inferred;
               if (result_position IS 0) {
                  inferred = this->infer_expression_type(*Payload.values[source]);
                  inferred = this->refine_static_expression_type(*Payload.values[source], inferred);
               }
               else {
                  const ExprNode &value_expr = *Payload.values[source];
                  bool provides_multiple_results = value_expr.kind IS AstNodeKind::CallExpr or
                     value_expr.kind IS AstNodeKind::SafeCallExpr or
                     (value_expr.static_results and
                      (this->ctx_.descriptors().results(value_expr.static_results).declared_count > 1 or
                       this->ctx_.descriptors().results(value_expr.static_results).variadic));
                  if (provides_multiple_results) {
                     inferred = this->infer_call_return_type(value_expr, result_position);
                  }
                  else {
                     inferred.primary = TiriType::Nil;
                     inferred.is_nullable = true;
                  }
               }

               inferred.requires_destination_type =
                  (inferred.primary IS TiriType::Any or inferred.primary IS TiriType::Unknown) and
                  not (result_position IS 0 and is_table_read_expression(*Payload.values[source]));
               inferred.is_fixed = inferred.primary != TiriType::Nil and inferred.primary != TiriType::Any and
                  inferred.primary != TiriType::Unknown;
               if (result_position IS 0 and is_table_read_expression(*Payload.values[source])) {
                  this->explicit_variant_bindings_.insert(name_ref->binding_id);
               }
               if (inferred.requires_destination_type) {
                  inferred = InferredType(TiriType::Any);
                  this->explicit_variant_bindings_.insert(name_ref->binding_id);
               }
               this->current_scope().declare_local(name, inferred, target.span);
               this->implicit_local_bindings_.insert(name_ref->binding_id);
               this->publish_binding_type(name_ref->binding_id, inferred);
               continue;
            }

            InferredType inferred(TiriType::Nil, false, true, false);
            this->current_scope().declare_local(name, inferred, target.span);
            this->implicit_local_bindings_.insert(name_ref->binding_id);
            this->publish_binding_type(name_ref->binding_id, inferred);
            continue;
         }

         // Check for assignment to const variable

         if (is_const) {
            std::string_view name_view(strdata(name), name->len);
            TypeDiagnostic diag;
            diag.location = target.span;
            diag.code = ParserErrorCode::AssignToConstant;
            diag.message = std::format("cannot assign to const {} '{}'", is_global ? "global" : "local", name_view);
            this->record_diagnostic(std::move(diag));
            continue;  // Skip further checking for this target
         }

         if (Payload.values.empty()) continue;
         size_t source = std::min(i, Payload.values.size() - 1);
         size_t result_position = i - source;
         const bool is_table_read = result_position IS 0 and is_table_read_expression(*Payload.values[source]);
         InferredType value_type;
         if (result_position IS 0) value_type = this->infer_expression_type(*Payload.values[source]);
         else {
            const ExprNode &value_expr = *Payload.values[source];
            bool provides_multiple_results = value_expr.kind IS AstNodeKind::CallExpr or
               value_expr.kind IS AstNodeKind::SafeCallExpr or
               (value_expr.static_results and
                (this->ctx_.descriptors().results(value_expr.static_results).declared_count > 1 or
                 this->ctx_.descriptors().results(value_expr.static_results).variadic));
            if (provides_multiple_results) {
               value_type = this->infer_call_return_type(value_expr, result_position);
            }
            else {
               value_type.primary = TiriType::Nil;
               value_type.is_nullable = true;
            }
         }

         if (existing->requires_destination_type) {
            if (value_type.primary != TiriType::Nil) {
               this->report_dynamic_destination_required(name, target.span, is_global);
            }
            continue;
         }

         // Persisted contracts remain authoritative at the environment boundary.  Static metadata supports reads
         // and redeclaration checks, but ordinary stores must still compile so runtime validation remains catchable.
         if (is_global and this->has_environment_contract(name)) continue;

         if (existing->is_fixed) {
            // Fixed type: check compatibility
            if (existing->primary IS TiriType::Any) continue; // 'any' accepts everything including nil
            if (value_type.primary IS TiriType::Nil) continue;  // Nil is always allowed as a "clear" operation

            if (Payload.op IS AssignmentOperator::Concat) {
               // Compound concatenation (..=) on an array target appends in place via array.append(),
               // preserving the array type; the RHS is validated by push() at runtime.
               if (existing->primary IS TiriType::Array) continue;
               // Concatenating a number onto a string target still yields a string.
               if (existing->primary IS TiriType::Str and value_type.primary IS TiriType::Num) continue;
            }

            if (value_type.primary != TiriType::Any and value_type.primary != existing->primary) {
               // Implicit globals were never declared by the user, so a conflicting assignment is legal;
               // degrade the tracked type to 'any' rather than raising a diagnostic.
               if (is_global and this->is_implicit_global(name)) {
                  this->degrade_global_type(name);
                  continue;
               }

               // Type mismatch
               TypeDiagnostic diag;
               diag.location = target.span;
               diag.expected = existing->primary;
               diag.actual = value_type.primary;
               diag.code = ParserErrorCode::TypeMismatchAssignment;
               diag.message = std::format("cannot assign '{}' to {} of type '{}'",
                  type_name(value_type.primary), is_global ? "global" : "variable", type_name(existing->primary));
               this->record_diagnostic(std::move(diag));
            }
            // Check for object class ID mismatch (both types are Object but different classes)
            else if (existing->primary IS TiriType::Object and value_type.primary IS TiriType::Object and
               existing->object_class_id != CLASSID::NIL) {
               if (existing->object_class_id != value_type.object_class_id) {
                  // Object class identity remains sticky after inference, including for implicit globals.  Advisory
                  // global typing permits unrelated value types to degrade to 'any'; it does not permit an Object
                  // variable to silently change class.
                  TypeDiagnostic diag;
                  diag.location = target.span;
                  diag.expected = TiriType::Object;
                  diag.actual = TiriType::Object;
                  diag.code = ParserErrorCode::ObjectClassMismatch;
                  diag.message = std::format("object class mismatch: cannot assign object of different class to {} ({} vs {})",
                     is_global ? "global" : "variable", ResolveClassID(value_type.object_class_id), ResolveClassID(existing->object_class_id));
                  this->record_diagnostic(std::move(diag));
               }
            }
            else if (existing->primary IS TiriType::Struct and value_type.primary IS TiriType::Struct and
               existing->struct_def and value_type.struct_def and existing->struct_def != value_type.struct_def) {
               if (is_global and this->is_implicit_global(name)) {
                  this->degrade_global_type(name);
                  continue;
               }
               TypeDiagnostic diag;
               diag.location = target.span;
               diag.expected = TiriType::Struct;
               diag.actual = TiriType::Struct;
               diag.code = ParserErrorCode::TypeMismatchAssignment;
               diag.message = std::format("struct layout mismatch: cannot assign '{}' to '{}'",
                  value_type.struct_def->Name, existing->struct_def->Name);
               this->record_diagnostic(std::move(diag));
            }
            else if (existing->primary IS TiriType::Array and value_type.primary IS TiriType::Array and
                value_type.array_element.known and
                not array_element_matches(existing->array_element, value_type.array_element)) {
               TypeDiagnostic diag;
               diag.location = target.span;
               diag.expected = TiriType::Array;
               diag.actual = TiriType::Array;
               diag.code = ParserErrorCode::TypeMismatchAssignment;
               diag.message = std::format("array member mismatch: expected array<{}>, got array<{}>",
                  array_element_name(existing->array_element), array_element_name(value_type.array_element));
               this->record_diagnostic(std::move(diag));
            }
         }
         else {
            // Unfixed variable: first non-nil assignment fixes the type
            // But don't fix if the variable was explicitly declared as 'any'
            if (existing->primary IS TiriType::Nil and
                (value_type.primary IS TiriType::Any or value_type.primary IS TiriType::Unknown)) {
               if (is_table_read and not is_global) {
                  this->fix_local_type(name, name_ref->binding_id, TiriType::Any);
                  this->explicit_variant_bindings_.insert(name_ref->binding_id);
               }
               else {
                  bool requires_destination = is_global ? not this->is_implicit_global(name) :
                     not this->implicit_local_bindings_.contains(name_ref->binding_id);
                  this->mark_dynamic_ingress(name, is_global, requires_destination);
                  if (not is_global and not requires_destination) {
                     InferredType variant(TiriType::Any);
                     this->explicit_variant_bindings_.insert(name_ref->binding_id);
                     this->publish_binding_type(name_ref->binding_id, variant);
                  }
               }
            }
            else if ((existing->primary != TiriType::Any) and (value_type.primary != TiriType::Nil)) {
               if (is_global) {
                  this->fix_global_type(name, value_type.primary, value_type.object_class_id, value_type.struct_def,
                     value_type.array_element);
               }
               else {
                  this->fix_local_type(name, name_ref->binding_id, value_type.primary,
                     value_type.object_class_id, value_type.struct_def, value_type.array_element);
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
   for (size_t name_index = 0; name_index < Payload.names.size(); ++name_index) {
      const auto& name = Payload.names[name_index];
      InferredType inferred;
      std::optional<LocalInitialiser> initialiser;

      // Explicit type annotation takes precedence (Unknown = no annotation)
      if (name.type != TiriType::Unknown) {
         inferred = this->analyse_annotated_local_binding(name, Payload.values, name_index);
      }
      else {
         initialiser = this->infer_local_initialiser(Payload.values, name_index);
         if (initialiser) {
            // No annotation: infer type from initial value
            inferred = initialiser->type;

            inferred.requires_destination_type = not name.is_blank and
               (inferred.primary IS TiriType::Any or inferred.primary IS TiriType::Unknown) and
               not is_table_read_expression(*initialiser->expression);

            // Non-nil, non-any initial values fix the type
            if (inferred.primary != TiriType::Nil and inferred.primary != TiriType::Any and
                inferred.primary != TiriType::Unknown) {
               inferred.is_fixed = true;
            }
         }
         else {
            // No annotation and no initialiser: starts as nil, type not yet determined
            // Use Nil (not Any) so the first non-nil assignment will fix the type
            inferred.primary = TiriType::Nil;
            inferred.is_fixed = false;
         }
      }

      #ifdef INCLUDE_TIPS
      this->check_shadowing(name.symbol, name.span);
      #endif

      this->current_scope().declare_local(name.symbol, inferred, name.span, name.has_const);
      if (name.type IS TiriType::Unknown and initialiser and
          is_table_read_expression(*initialiser->expression) and name.binding_id) {
         this->explicit_variant_bindings_.insert(name.binding_id);
      }
      if (name.type IS TiriType::Unknown) this->publish_binding_type(name.binding_id, inferred);
      this->trace_decl(this->ctx_.lex().linenumber, name.symbol, inferred.primary, inferred.is_fixed);
   }

   for (const auto &value : Payload.values) {
      this->analyse_expression(*value);
   }
}

std::optional<TypeAnalyser::LocalInitialiser> TypeAnalyser::infer_local_initialiser(
   const ExprNodeList &Values, size_t Position)
{
   if (Values.empty()) return std::nullopt;

   size_t source = std::min(Position, Values.size() - 1);
   size_t result_position = Position - source;
   const ExprNode &expression = *Values[source];
   InferredType type;
   if (result_position IS 0) {
      type = this->infer_expression_type(expression);
      type = this->refine_static_expression_type(expression, type);
   }
   else {
      bool provides_multiple_results = expression.kind IS AstNodeKind::CallExpr or
         expression.kind IS AstNodeKind::SafeCallExpr or
         (expression.static_results and
          (this->ctx_.descriptors().results(expression.static_results).declared_count > 1 or
           this->ctx_.descriptors().results(expression.static_results).variadic));
      if (not provides_multiple_results) return std::nullopt;
      type = this->infer_call_return_type(expression, result_position);
   }
   return LocalInitialiser { type, &expression };
}

InferredType TypeAnalyser::analyse_annotated_local_binding(
   const Identifier &Name, const ExprNodeList &Values, size_t Position)
{
   InferredType inferred;
   inferred.primary = Name.type;
   inferred.struct_def = Name.struct_def;
   inferred.array_element = Name.array_element;
   inferred.requires_destination_type = false;
   inferred.is_fixed = Name.type != TiriType::Any;

   auto initialiser = this->infer_local_initialiser(Values, Position);
   if (Name.type != TiriType::Any and initialiser) {
      const InferredType &value_type = initialiser->type;
      TypeDiagnostic diag;
      diag.location = initialiser->expression->span;
      diag.expected = Name.type;
      diag.actual = value_type.primary;
      diag.code = ParserErrorCode::TypeMismatchAssignment;

      if (value_type.primary != TiriType::Nil and value_type.primary != TiriType::Any and
          value_type.primary != TiriType::Unknown and value_type.primary != Name.type) {
         diag.message = std::format("cannot assign '{}' to variable of type '{}'",
            type_name(value_type.primary), type_name(Name.type));
         this->record_diagnostic(std::move(diag));
      }
      else if (Name.type IS TiriType::Struct and value_type.primary IS TiriType::Struct and
          Name.struct_def and value_type.struct_def and Name.struct_def != value_type.struct_def) {
         diag.message = std::format("struct layout mismatch: cannot assign '{}' to '{}'",
            value_type.struct_def->Name, Name.struct_def->Name);
         this->record_diagnostic(std::move(diag));
      }
      else if (Name.type IS TiriType::Array and value_type.primary IS TiriType::Array and
          value_type.array_element.known and
          not array_element_matches(Name.array_element, value_type.array_element)) {
         diag.message = std::format("array member mismatch: expected array<{}>, got array<{}>",
            array_element_name(Name.array_element), array_element_name(value_type.array_element));
         this->record_diagnostic(std::move(diag));
      }
   }

   if (Name.type IS TiriType::Any and Name.binding_id) {
      this->explicit_variant_bindings_.insert(Name.binding_id);
   }
   this->publish_binding_type(Name.binding_id, inferred);
   return inferred;
}

//********************************************************************************************************************
// Global Naming Convention Validation
//
// Checks if a global variable name follows Tiri naming conventions:
// - glX... - Starts with 'gl' followed by uppercase letter (e.g., glMyGlobal, glConfig)
// - ALL_CAPS - Full uppercase with underscores for constants (e.g., MY_FLAG, ERR_OKAY)
// - mX... - Starts with 'm' for module namespaces (e.g., mSys, mDisplay)
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
   for (const auto &value : Payload.values) this->analyse_expression(*value);

   for (const auto &name : Payload.names) {
      if (not name.symbol) continue;

      if ((name.symbol->flags & STRFLAG_PROTECTED_GLOBAL) != 0) {
         this->report_protected_global_override(name.symbol, name.span);
      }
      else if (type_is_registered_constant(name.symbol)) {
         std::string_view name_view(strdata(name.symbol), name.symbol->len);
         TypeDiagnostic diag;
         diag.location = name.span;
         diag.code = ParserErrorCode::AssignToConstant;
         diag.message = std::format("cannot assign to constant '{}'", name_view);
         this->record_diagnostic(std::move(diag));
      }
   }

   this->discover_global_decl_policy(Payload, true);

   #ifdef INCLUDE_TIPS
   // Track globals for loop access detection
   for (const auto &name : Payload.names) {
      if (name.symbol) this->track_global(name.symbol);
   }

   // Check global naming conventions
   if (this->should_emit_tip(3)) {
      for (const auto &name : Payload.names) {
         if (not name.symbol) continue;
         std::string_view name_view(strdata(name.symbol), name.symbol->len);
         if (not is_valid_global_name(name_view)) {
            this->emit_tip(3, TipCategory::Style,
               std::format("Global variable '{}' should follow naming convention: 'gl[A-Z]...' or 'ALL_CAPS'",
                  name_view),
               Token::from_span(name.span, TokenKind::Identifier));
         }
      }
   }
   #endif
}

//********************************************************************************************************************
// Discover the binding policy needed by IR emission without performing ordinary expression analysis or diagnostics.
// Reduced-analysis blocks use this path so their runtime failures remain catchable while global declarations still
// publish the same sticky environment contracts as declarations in ordinarily analysed blocks.

void TypeAnalyser::discover_global_decl_policy(const GlobalDeclStmtPayload &Payload, bool PublishStaticPolicy)
{
   size_t value_index = 0;
   size_t call_return_index = 0;
   const ExprNode *multi_return_call = nullptr;

   // Track global variable types for type checking on subsequent assignments
   for (size_t i = 0; i < Payload.names.size(); ++i) {
      const auto &name = Payload.names[i];
      if (not name.symbol) continue;

      if ((name.symbol->flags & STRFLAG_PROTECTED_GLOBAL) != 0 or type_is_registered_constant(name.symbol)) continue;

      InferredType inferred;
      InferredType value_type;
      bool have_value_type = false;

      if (value_index < Payload.values.size()) {
         const ExprNode &value_expr = *Payload.values[value_index];
         value_type = this->infer_expression_type(value_expr);
         value_type = this->refine_static_expression_type(value_expr, value_type);
         have_value_type = true;

         if (value_index IS Payload.values.size() - 1 and
             (value_expr.kind IS AstNodeKind::CallExpr or value_expr.kind IS AstNodeKind::SafeCallExpr or
              (value_expr.static_results and
               (this->ctx_.descriptors().results(value_expr.static_results).declared_count > 1 or
                this->ctx_.descriptors().results(value_expr.static_results).variadic)))) {
            multi_return_call = &value_expr;
            call_return_index = 0;
         }

         value_index += 1;
      }
      else if (multi_return_call) {
         call_return_index += 1;
         value_type = this->infer_call_return_type(*multi_return_call, call_return_index);
         have_value_type = true;
      }

      // Explicit type annotation takes precedence
      if (name.type != TiriType::Unknown) {
         inferred.primary = name.type;
         inferred.struct_def = name.struct_def;
         inferred.array_element = name.array_element;
         inferred.is_fixed = (name.type != TiriType::Any);
      }
      else if (have_value_type) {
         // Infer type from initial value
         inferred = value_type;
         inferred.requires_destination_type = not name.is_blank and
            (inferred.primary IS TiriType::Any or inferred.primary IS TiriType::Unknown);
         // Non-nil, non-any initial values fix the type
         if (inferred.primary != TiriType::Nil and inferred.primary != TiriType::Any and
             inferred.primary != TiriType::Unknown) {
            inferred.is_fixed = true;
         }
      }
      else { // No annotation and no initialiser: starts as nil, type not yet fixed
         inferred.primary = TiriType::Nil;
         inferred.is_fixed = false;
      }

      GlobalContractPolicy contract_policy = GlobalContractPolicy::Advisory;
      if (name.type IS TiriType::Any) contract_policy = GlobalContractPolicy::Variant;
      else if (inferred.is_fixed) contract_policy = GlobalContractPolicy::Enforced;

      name.global_contract_type = inferred.primary;
      name.global_contract_object_class_id = inferred.primary IS TiriType::Object ?
         inferred.object_class_id : CLASSID::NIL;
      name.global_contract_struct_def = inferred.struct_def;
      name.global_contract_array_element = inferred.array_element;
      name.global_contract_policy = contract_policy;
      if (name.has_const and not inferred.is_fixed) {
         name.global_contract_type = TiriType::Any;
         name.global_contract_object_class_id = CLASSID::NIL;
         name.global_contract_struct_def = nullptr;
         name.global_contract_array_element = {};
         name.global_contract_policy = GlobalContractPolicy::Enforced;
      }
      if (PublishStaticPolicy) this->declare_global(name.symbol, inferred, name.span, name.has_const, contract_policy);
      else this->invalidate_global_flow_policy(name.symbol, name.span);
   }
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
   this->declare_local_function(Payload.name.symbol, function, Payload.name.span);

   if (function) this->analyse_function_payload(*function, Payload.name.symbol);
}

//********************************************************************************************************************
// Local Function Declaration: Retains an empty declaration as a lexical-scope-local signature contract for the
// next definition of the same simple name.

void TypeAnalyser::declare_local_function(GCstr *Name, const FunctionExprPayload *Function, SourceSpan Location)
{
   const FunctionExprPayload *forward_declaration = this->current_scope().lookup_function(Name);
   if (forward_declaration and Function and is_function_forward_declaration(*forward_declaration)) {
      if (auto mismatch = function_signature_mismatch(*forward_declaration, *Function)) {
         TypeDiagnostic diag;
         diag.location = Location;
         diag.code = ParserErrorCode::FunctionSignatureMismatch;
         diag.message = std::format("signature for local function '{}' does not match its forward declaration: {}",
            std::string_view(strdata(Name), Name->len), *mismatch);
         this->record_diagnostic(std::move(diag));
      }
   }

   this->current_scope().declare_function(Name, Function, Location);
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
         if (Payload.name.segments.size() IS 1) this->declare_local_function(terminal.symbol, function, terminal.span);
         else this->current_scope().declare_function(terminal.symbol, function, terminal.span);
         function_name = terminal.symbol;
         function_location = terminal.span;
      }

   }
   else {
      // Track global function type for type checking on reassignment
      // Note: Global functions are exempt from naming convention checks
      if (not Payload.name.segments.empty()) {
         function_name = Payload.name.segments.back().symbol;
         function_location = Payload.name.segments.back().span;
         bool is_direct_global_store = Payload.name.segments.size() IS 1;
         if (is_direct_global_store and function_name and ((function_name->flags & STRFLAG_PROTECTED_GLOBAL) != 0)) {
            this->report_protected_global_override(function_name, function_location);
         }
         else if (is_direct_global_store and type_is_registered_constant(function_name)) {
            std::string_view name_view(strdata(function_name), function_name->len);
            TypeDiagnostic diag;
            diag.location = function_location;
            diag.code = ParserErrorCode::AssignToConstant;
            diag.message = std::format("cannot assign to constant '{}'", name_view);
            this->record_diagnostic(std::move(diag));
         }
         else this->declare_global_function(function_name, function, function_location);
      }
   }

   if (function) this->analyse_function_payload(*function, function_name);
}

bool TypeAnalyser::is_global_environment_reference(const ExprNode &Expr) const
{
   if (Expr.kind != AstNodeKind::IdentifierExpr) return false;

   const auto *name_ref = std::get_if<NameRef>(&Expr.data);
   if (not name_ref or not type_identifier_name_is(name_ref->identifier.symbol, "_G")) return false;

   return not this->resolve_identifier(name_ref->identifier.symbol).has_value();
}

GCstr * TypeAnalyser::protected_global_store_key(const ExprNode &Expr) const
{
   GCstr *key = nullptr;

   if (Expr.kind IS AstNodeKind::MemberExpr) {
      const auto &payload = std::get<MemberExprPayload>(Expr.data);
      if (payload.table and this->is_global_environment_reference(*payload.table)) key = payload.member.symbol;
   }
   else if (Expr.kind IS AstNodeKind::IndexExpr) {
      const auto &payload = std::get<IndexExprPayload>(Expr.data);
      if (payload.table and payload.index and this->is_global_environment_reference(*payload.table)) {
         key = type_literal_string_key(*payload.index);
      }
   }

   if (key and ((key->flags & STRFLAG_PROTECTED_GLOBAL) != 0)) return key;
   return nullptr;
}

void TypeAnalyser::report_protected_global_override(GCstr *Name, SourceSpan Location)
{
   if (not Name) return;

   TypeDiagnostic diag;
   diag.location = Location;
   diag.code = ParserErrorCode::OverrideProtectedGlobal;
   diag.message = std::format("cannot override built-in '{}'", std::string_view(strdata(Name), Name->len));
   this->record_diagnostic(std::move(diag));
}

void TypeAnalyser::report_dynamic_destination_required(GCstr *Name, SourceSpan Location, bool IsGlobal)
{
   if (not Name) return;

   TypeDiagnostic diag;
   diag.location = Location;
   diag.code = ParserErrorCode::DeferredTypeRequired;
   diag.message = std::format(
      "cannot infer type of {} '{}' from a dynamic value; add a concrete annotation or ':any'",
      IsGlobal ? "global" : "local", std::string_view(strdata(Name), Name->len));
   this->record_diagnostic(std::move(diag));
}

//********************************************************************************************************************
// Function Payload Analysis: Analyses a function's body, including parameter registration, return type validation,
// and recursive function detection. Creates a new scope for the function body.

void TypeAnalyser::analyse_function_payload(const FunctionExprPayload &Function, GCstr *Name)
{
   this->push_scope();
   this->enter_function(Function, Name);

   for (const auto &param : Function.parameters) {
      this->current_scope().declare_parameter(
         param.name.symbol, param.type, param.struct_def, param.required, param.name.span);
      if (param.type_is_explicit and param.type IS TiriType::Any and param.name.binding_id) {
         this->explicit_variant_bindings_.insert(param.name.binding_id);
      }
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
      this->record_diagnostic(std::move(diag));
   }

   // Advise on missing return type annotation for functions that return values

   #ifdef INCLUDE_TIPS
   if (this->should_emit_tip(1) and
       (not Function.return_types.is_explicit) and this->function_has_return_values(Function)) {
      SourceSpan span = Function.body ? Function.body->span : SourceSpan{};
      this->emit_tip(1, TipCategory::TypeSafety,
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
      case AstNodeKind::TypeTestExpr: {
         auto *payload = std::get_if<TypeTestExprPayload>(&Expression.data);
         if (payload and payload->value) this->analyse_expression(*payload->value);
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

            // Warn if numeric-only operators receive statically known non-numeric operands.
            if (payload->op IS AstBinaryOperator::HasFlag or payload->op IS AstBinaryOperator::Approx) {
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
                     const char *op_name = payload->op IS AstBinaryOperator::HasFlag ? "has" : "≈";
                     diag.message = std::format("'{}' operator expects numeric {}, got {}", op_name, Side,
                        type_name(inferred.primary));
                     this->record_diagnostic(std::move(diag));
                  }
               };
               check_operand(payload->left, "operand");
               check_operand(payload->right, "operand");
            }
         }
         break;
      }
      case AstNodeKind::ComparisonChainExpr: {
         auto *payload = std::get_if<ComparisonChainExprPayload>(&Expression.data);
         if (payload) {
            for (const auto &operand : payload->operands) {
               if (operand) this->analyse_expression(*operand);
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
      case AstNodeKind::CallExpr:
      case AstNodeKind::SafeCallExpr: {
         auto *payload = std::get_if<CallExprPayload>(&Expression.data);
         if (payload) this->analyse_call_expr(*payload, Expression.span);
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
            #ifdef INCLUDE_TIPS
            this->check_choose_missing_fallback(*payload, Expression.span);
            #endif

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

void TypeAnalyser::analyse_call_expr(const CallExprPayload &Call, SourceSpan Location)
{
   if ((Call.result_type IS TiriType::Struct or Call.result_type IS TiriType::Func) and
       not Call.struct_def and not Call.arguments.empty()) {
      const auto *literal = std::get_if<LiteralValue>(&Call.arguments[0]->data);
      if (literal and literal->kind IS LiteralKind::String and literal->string_value) {
         std::string_view name(strdata(literal->string_value), literal->string_value->len);
         Call.struct_def = find_struct(&this->ctx_.lua(), name);
      }
   }
   if (not Call.struct_def) {
      if (const auto *direct = std::get_if<DirectCallTarget>(&Call.target);
          direct and direct->callable and direct->callable->kind IS AstNodeKind::IdentifierExpr) {
         const auto &name_ref = std::get<NameRef>(direct->callable->data);
         auto callable = this->resolve_identifier(name_ref.identifier.symbol);
         if (callable and callable->struct_def) {
            Call.result_type = TiriType::Struct;
            Call.struct_def = callable->struct_def;
         }
         else if (not callable and not this->lookup_global_type(name_ref.identifier.symbol)) {
            // Declared structs no longer bind constructor variables.  A call on an unresolved name that matches a
            // registered struct is almost certainly the retired 'Name { ... }' / 'Name()' construction syntax.
            std::string_view name(strdata(name_ref.identifier.symbol), name_ref.identifier.symbol->len);
            if (find_struct(&this->ctx_.lua(), name)) {
               TypeDiagnostic diag;
               diag.location = direct->callable->span;
               diag.expected = TiriType::Struct;
               diag.actual = TiriType::Nil;
               diag.code = ParserErrorCode::UnexpectedToken;
               diag.message = std::format("'{}' is a struct declaration; construct instances with struct<{}> {{ ... }}",
                  name, name);
               this->record_diagnostic(std::move(diag));
            }
         }
      }
   }

   // Analyse the callable to mark function names as used
   const auto &direct = std::get<DirectCallTarget>(Call.target);
   if (direct.callable) this->analyse_expression(*direct.callable);

   // Analyse arguments
   for (const auto &argument : Call.arguments) {
      this->analyse_expression(*argument);
   }

   const FunctionExprPayload* target = this->resolve_call_target(Call);
   if (target) {
      if (Call.result_type IS TiriType::Unknown and
          (target->return_types.is_explicit or target->return_types.is_inferred) and
          target->return_types.count > 0) {
         Call.result_type = target->return_types.types[0];
         Call.object_class_id = target->return_types.object_class_ids[0];
         Call.struct_def = target->return_types.struct_defs[0];
      }
      this->check_arguments(*target, Call, Location);
   }
}

//********************************************************************************************************************
// Validate each argument against the corresponding parameter type declaration.

void TypeAnalyser::check_arguments(
   const FunctionExprPayload &Function, const CallExprPayload &Call, SourceSpan Location)
{
   size_t effective_argument_count = Call.arguments.size();
   bool dynamic_tail = false;
   const ExprNode *tail_argument = Call.arguments.empty() ? nullptr : Call.arguments.back().get();
   if (tail_argument and (tail_argument->kind IS AstNodeKind::CallExpr or
       tail_argument->kind IS AstNodeKind::SafeCallExpr or tail_argument->kind IS AstNodeKind::VarArgExpr)) {
      if (tail_argument->static_results) {
         const StaticResultSet &results = this->ctx_.descriptors().results(tail_argument->static_results);
         if (results.dynamic or results.variadic) dynamic_tail = true;
         else effective_argument_count = Call.arguments.size() - 1 + results.declared_count;
      }
      else dynamic_tail = true;
   }

   size_t param_index = 0;
   for (const auto &param : Function.parameters) {
      if (param_index >= Call.arguments.size()) {
         if (dynamic_tail) {
            param_index += 1;
            continue;
         }
         if (param_index < effective_argument_count and tail_argument) {
            InferredType actual = this->infer_call_return_type(
               *tail_argument, param_index - (Call.arguments.size() - 1));
            if (param.required and actual.primary IS TiriType::Nil) {
               TypeDiagnostic diag;
               diag.location = tail_argument->span;
               diag.expected = param.type;
               diag.actual = actual.primary;
               diag.code = ParserErrorCode::TypeMismatchArgument;
               diag.message = std::format("required argument {} cannot be nil", param_index + 1);
               this->record_diagnostic(std::move(diag));
            }
         }
         else if (param.required) {
            TypeDiagnostic diag;
            diag.location = Location;
            diag.expected = param.type;
            diag.actual = TiriType::Nil;
            diag.code = ParserErrorCode::TypeMismatchArgument;
            diag.message = std::format("required argument {} is missing", param_index + 1);
            this->record_diagnostic(std::move(diag));
         }
         param_index += 1;
         continue;
      }
      this->check_argument_type(*Call.arguments[param_index], param, param_index);
      param_index += 1;
   }
}

//********************************************************************************************************************
// Check a single argument against its expected type, reporting diagnostics for mismatches.

void TypeAnalyser::check_argument_type(
   const ExprNode& Argument, const FunctionParameter &Parameter, size_t Index)
{
   TiriType Expected = Parameter.type;
   InferredType actual = this->infer_expression_type(Argument);

   if (Parameter.required and actual.primary IS TiriType::Nil) {
      TypeDiagnostic diag;
      diag.location = Argument.span;
      diag.expected = Expected;
      diag.actual = actual.primary;
      diag.code = ParserErrorCode::TypeMismatchArgument;
      diag.message = std::format("required argument {} cannot be nil", Index + 1);
      this->record_diagnostic(std::move(diag));
      return;
   }
   if (Expected IS TiriType::Any) return;

   bool member_matches = Expected != TiriType::Array or actual.primary != TiriType::Array or
      not actual.array_element.known or array_element_matches(Parameter.array_element, actual.array_element);
   if (not actual.matches(Expected) or not member_matches) {
      TypeDiagnostic diag;
      diag.location = Argument.span;
      diag.expected = Expected;
      diag.actual = actual.primary;
      diag.code = ParserErrorCode::TypeMismatchArgument;
      if (Expected IS TiriType::Array and actual.primary IS TiriType::Array) {
         diag.message = std::format("type mismatch: argument {} expects array<{}>, got array<{}>", Index + 1,
            array_element_name(Parameter.array_element), array_element_name(actual.array_element));
      }
      else diag.message = std::format("type mismatch: argument {} expects '{}', got '{}'",
         Index + 1, type_name(Expected), type_name(actual.primary));
      this->record_diagnostic(std::move(diag));
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

// Reads from an untyped table, including an unannotated parameter, yield 'any' because element types are not tracked.
// Such a value is dynamic by nature rather than by an unresolved ingress, so it must not lock the destination into
// requiring an annotation.  Struct fields and typed array elements infer concrete types and never reach this path.

bool TypeAnalyser::is_table_read_expression(const ExprNode &Expr)
{
   const ExprNode *base = nullptr;

   switch (Expr.kind) {
      case AstNodeKind::MemberExpr:
         base = std::get<MemberExprPayload>(Expr.data).table.get();
         break;
      case AstNodeKind::IndexExpr:
         base = std::get<IndexExprPayload>(Expr.data).table.get();
         break;
      case AstNodeKind::SafeMemberExpr:
         base = std::get<SafeMemberExprPayload>(Expr.data).table.get();
         break;
      case AstNodeKind::SafeIndexExpr:
         base = std::get<SafeIndexExprPayload>(Expr.data).table.get();
         break;

      // Binary expressions yield 'any' when a table read can invoke an overloaded operator.  This includes defaulted
      // reads such as t[k] ?? fallback and arithmetic such as 0.5 + t.value.  Recurse so the exemption follows the
      // value's origin.

      case AstNodeKind::BinaryExpr: {
         const auto *payload = std::get_if<BinaryExprPayload>(&Expr.data);
         return payload and ((payload->left and this->is_table_read_expression(*payload->left)) or
            (payload->right and this->is_table_read_expression(*payload->right)));
      }

      // Unary negation can remain dynamic when its operand is a table read because the value may overload '-'.

      case AstNodeKind::UnaryExpr: {
         const auto *payload = std::get_if<UnaryExprPayload>(&Expr.data);
         return payload and payload->operand and this->is_table_read_expression(*payload->operand);
      }

      default:
         return false;
   }

   if (not base) return false;

   const TiriType base_type = this->infer_expression_type(*base).primary;
   return base_type IS TiriType::Table or base_type IS TiriType::Any;
}

InferredType TypeAnalyser::infer_expression_type(const ExprNode& Expr)
{
   InferredType result;

   switch (Expr.kind) {
      case AstNodeKind::TypeTestExpr:
         result.primary = TiriType::Bool;
         result.is_nullable = false;
         return result;
      case AstNodeKind::CurrentContextExpr:
         result.primary = TiriType::Table;
         result.is_nullable = false;
         break;
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
            if (type_is_registered_constant(payload->identifier.symbol)) {
               result.primary = TiriType::Num;
               result.is_constant = true;
               result.is_fixed = true;
               return result;
            }
            resolved = this->lookup_global_type(payload->identifier.symbol);
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
      case AstNodeKind::CallExpr:
      case AstNodeKind::SafeCallExpr: {
         // For call expressions, try to infer from the function's declared return type
         auto *payload = std::get_if<CallExprPayload>(&Expr.data);
         if (payload) {
            if (const auto *direct = std::get_if<DirectCallTarget>(&payload->target);
                direct and direct->callable and direct->callable->kind IS AstNodeKind::IdentifierExpr) {
               const auto &name_ref = std::get<NameRef>(direct->callable->data);
               auto callable = this->resolve_identifier(name_ref.identifier.symbol);
               if (callable and callable->struct_def) {
                  result.primary = TiriType::Struct;
                  result.struct_def = callable->struct_def;
                  payload->result_type = TiriType::Struct;
                  payload->struct_def = callable->struct_def;
                  return result;
               }
            }
            const FunctionExprPayload* target = this->resolve_call_target(*payload);
            if (target and (target->return_types.is_explicit or target->return_types.is_inferred) and
                target->return_types.count > 0) {
               result.primary = target->return_types.types[0];
               result.object_class_id = target->return_types.object_class_ids[0];
               result.struct_def = target->return_types.struct_defs[0];
               result.array_element = target->return_types.array_elements[0];
               payload->struct_def = result.struct_def;
               return result;
            }
            // First check if the call has a known result type (e.g., obj.new() returns Object)
            if (payload->result_type != TiriType::Unknown) {
               result.primary = payload->result_type;
               // Propagate object class ID for Object types
               if (payload->result_type IS TiriType::Object) {
                  result.object_class_id = payload->object_class_id;
               }
               result.struct_def = payload->struct_def;
               if (result.primary IS TiriType::Array and Expr.static_value) {
                  result.array_element = this->ctx_.descriptors().value(Expr.static_value).array_element;
               }
               if ((result.primary IS TiriType::Struct or result.primary IS TiriType::Func) and
                   not result.struct_def and not payload->arguments.empty()) {
                  const auto *literal = std::get_if<LiteralValue>(&payload->arguments[0]->data);
                  if (literal and literal->kind IS LiteralKind::String and literal->string_value) {
                     std::string_view name(strdata(literal->string_value), literal->string_value->len);
                     result.struct_def = find_struct(&this->ctx_.lua(), name);
                     payload->struct_def = result.struct_def;
                  }
               }
               return result;
            }
            if (Expr.static_value) {
               const StaticValueDescriptor &descriptor = this->ctx_.descriptors().value(Expr.static_value);
               if (descriptor.primary != TiriType::Unknown and descriptor.primary != TiriType::Any) {
                  result.primary = descriptor.primary;
                  result.is_nullable = descriptor.nullable;
                  result.object_class_id = descriptor.object_class_id;
                  result.struct_def = descriptor.struct_def;
                  result.array_element = descriptor.array_element;
                  return result;
               }
            }
         }
         result.primary = TiriType::Any;
         break;
      }
      case AstNodeKind::ResultFilterExpr: {
         if (Expr.static_value) {
            const StaticValueDescriptor &descriptor = this->ctx_.descriptors().value(Expr.static_value);
            if (descriptor.primary != TiriType::Unknown) {
               result.primary = descriptor.primary;
               result.is_nullable = descriptor.nullable;
               result.object_class_id = descriptor.object_class_id;
               result.struct_def = descriptor.struct_def;
               result.array_element = descriptor.array_element;
               return result;
            }
         }
         result.primary = TiriType::Any;
         break;
      }
      case AstNodeKind::MemberExpr:
      case AstNodeKind::SafeMemberExpr: {
         const ExprNode *base = nullptr;
         GCstr *member = nullptr;
         bool safe = Expr.kind IS AstNodeKind::SafeMemberExpr;
         if (safe) {
            const auto &payload = std::get<SafeMemberExprPayload>(Expr.data);
            base = payload.table.get();
            member = payload.member.symbol;
         }
         else {
            const auto &payload = std::get<MemberExprPayload>(Expr.data);
            base = payload.table.get();
            member = payload.member.symbol;
         }

         if (Expr.static_value) {
            const StaticValueDescriptor &descriptor = this->ctx_.descriptors().value(Expr.static_value);
            if (descriptor.primary != TiriType::Unknown and descriptor.primary != TiriType::Any) {
               result.primary = descriptor.primary;
               result.is_nullable = descriptor.nullable;
               result.object_class_id = descriptor.object_class_id;
               result.struct_def = descriptor.struct_def;
               return result;
            }
         }

         struct_record *struct_def = nullptr;
         if (base and base->static_value) {
            const StaticValueDescriptor &base_descriptor =
               this->ctx_.descriptors().value(base->static_value);
            if (base_descriptor.proved() and
                (base_descriptor.primary IS TiriType::Struct or base_descriptor.primary IS TiriType::Table)) {
               struct_def = base_descriptor.struct_def;
            }
         }
         if (base and not struct_def) {
            InferredType base_type = this->infer_expression_type(*base);
            if (base_type.primary IS TiriType::Struct or base_type.primary IS TiriType::Table) {
               struct_def = base_type.struct_def;
            }
         }
         if (struct_def and member) {
            StaticValueDescriptor descriptor = describe_struct_field(struct_def, member);
            if (descriptor.primary != TiriType::Unknown and descriptor.primary != TiriType::Any) {
               result.primary = descriptor.primary;
               result.is_nullable = descriptor.nullable or safe;
               result.object_class_id = descriptor.object_class_id;
               result.struct_def = descriptor.struct_def;
               return result;
            }
         }

         result.primary = TiriType::Any;
         break;
      }
      case AstNodeKind::RangeExpr:
         result.primary = TiriType::Range;
         break;
      case AstNodeKind::ComparisonChainExpr:
         result.primary = TiriType::Bool;
         return result;

      case AstNodeKind::BinaryExpr: {
         // Infer type from binary expression operands and operator
         auto *payload = std::get_if<BinaryExprPayload>(&Expr.data);
         if (payload) {
            switch (payload->op) {
               // Comparison operators always return boolean
               case AstBinaryOperator::Equal:
               case AstBinaryOperator::NotEqual:
               case AstBinaryOperator::Approx:
               case AstBinaryOperator::LessThan:
               case AstBinaryOperator::LessEqual:
               case AstBinaryOperator::GreaterThan:
               case AstBinaryOperator::GreaterEqual:
               case AstBinaryOperator::HasFlag:
               case AstBinaryOperator::Contains:
                  result.primary = TiriType::Bool;
                  return result;
               // Logical operators in Lua/Tiri return one of their operands.
               // Try to infer from operands, if both have the same type, use that.
               case AstBinaryOperator::LogicalAnd:
               case AstBinaryOperator::LogicalOr: {
                  InferredType left_type, right_type;
                  if (payload->left) left_type = this->infer_expression_type(*payload->left);
                  if (payload->right) right_type = this->infer_expression_type(*payload->right);

                  auto known_truth = [this](const ExprNode *Operand) -> std::optional<bool> {
                     if (not Operand) return std::nullopt;

                     const ExprNode *candidate = Operand;
                     if (Operand->kind IS AstNodeKind::IdentifierExpr) {
                        const auto &reference = std::get<NameRef>(Operand->data);
                        if (reference.binding_id) {
                           const auto &binding = this->ctx_.descriptors().binding(reference.binding_id);
                           if (binding.is_const and binding.initialiser) candidate = binding.initialiser;
                        }
                     }

                     if (candidate->kind != AstNodeKind::LiteralExpr) return std::nullopt;
                     const auto &literal = std::get<LiteralValue>(candidate->data);
                     if (literal.kind IS LiteralKind::Nil) return false;
                     if (literal.kind IS LiteralKind::Boolean) return literal.bool_value;
                     return true;
                  };

                  if (auto truth = known_truth(payload->left.get())) {
                     if (payload->op IS AstBinaryOperator::LogicalAnd) return *truth ? right_type : left_type;
                     return *truth ? left_type : right_type;
                  }

                  // In the common `condition and truthy_value or fallback` form, the falsey result of the inner
                  // `and` is discarded by the outer `or`.  When both surviving branches share a concrete type, retain
                  // that type instead of treating the intermediate `and` result as an unrepresentable union.

                  if (payload->op IS AstBinaryOperator::LogicalOr and payload->left and
                      payload->left->kind IS AstNodeKind::BinaryExpr) {
                     const auto &conditional = std::get<BinaryExprPayload>(payload->left->data);
                     if (conditional.op IS AstBinaryOperator::LogicalAnd and conditional.right) {
                        InferredType truthy_type = this->infer_expression_type(*conditional.right);
                        if (same_concrete_type(truthy_type, right_type)) {
                           return right_type;
                        }
                     }
                  }

                  // If both operands have the same concrete type, return that

                  if (same_concrete_type(left_type, right_type)) {
                     return left_type;
                  }

                  // A nil left operand has a statically known short-circuit outcome.  Other differing operand types
                  // produce a union that cannot be represented by a concrete TiriType and must remain variant.

                  if (left_type.primary IS TiriType::Nil) {
                     if (payload->op IS AstBinaryOperator::LogicalOr) return right_type;
                     return left_type;
                  }

                  result.primary = TiriType::Any;
                  return result;
               }
               // Concatenation returns string
               case AstBinaryOperator::Concat:
                  result.primary = TiriType::Str;
                  return result;
               // Arithmetic operators return number, unless an operand may be a table or object
               // whose metamethods (__add, __sub, ...) can overload the operation to return any
               // type.  Operands of type Any or Unknown may hold such values, so the result is
               // only known to be numeric when neither operand can carry a metatable.
               case AstBinaryOperator::Add:
               case AstBinaryOperator::Subtract:
               case AstBinaryOperator::Multiply:
               case AstBinaryOperator::Divide:
               case AstBinaryOperator::Modulo:
               case AstBinaryOperator::Power: {
                  auto may_overload = [](TiriType Type) {
                     return Type IS TiriType::Table or Type IS TiriType::Object or
                            Type IS TiriType::Any or Type IS TiriType::Unknown;
                  };
                  InferredType left_type, right_type;
                  if (payload->left) left_type = this->infer_expression_type(*payload->left);
                  if (payload->right) right_type = this->infer_expression_type(*payload->right);
                  if (may_overload(left_type.primary) or may_overload(right_type.primary)) {
                     result.primary = TiriType::Any;
                     return result;
                  }
                  result.primary = TiriType::Num;
                  return result;
               }
               // Bitwise operators are implemented via the bit library and always return number
               case AstBinaryOperator::BitAnd:
               case AstBinaryOperator::BitOr:
               case AstBinaryOperator::BitXor:
               case AstBinaryOperator::ShiftLeft:
               case AstBinaryOperator::ShiftRight:
                  result.primary = TiriType::Num;
                  return result;
               // IfEmpty returns type of the operands
               case AstBinaryOperator::IfEmpty: {
                  InferredType left_type, right_type;
                  if (payload->left) left_type = this->infer_expression_type(*payload->left);
                  if (payload->right) right_type = this->infer_expression_type(*payload->right);

                  if (payload->left and payload->left->kind IS AstNodeKind::LiteralExpr) {
                     const auto &literal = std::get<LiteralValue>(payload->left->data);
                     bool is_empty = literal.kind IS LiteralKind::Nil or
                        (literal.kind IS LiteralKind::Boolean and not literal.bool_value) or
                        (literal.kind IS LiteralKind::Number and literal.number_value IS 0) or
                        (literal.kind IS LiteralKind::String and literal.string_value and
                           literal.string_value->len IS 0);
                     return is_empty ? right_type : left_type;
                  }

                  if (left_type.primary IS TiriType::Nil) return right_type;
                  if (left_type.primary IS TiriType::Any or left_type.primary IS TiriType::Unknown or
                      right_type.primary IS TiriType::Any or right_type.primary IS TiriType::Unknown or
                      left_type.primary != right_type.primary or
                      (left_type.primary IS TiriType::Object and
                         left_type.object_class_id != right_type.object_class_id) or
                      (left_type.primary IS TiriType::Struct and left_type.struct_def != right_type.struct_def)) {
                     result.primary = TiriType::Any;
                     return result;
                  }

                  left_type.is_nullable = right_type.is_nullable;
                  return left_type;
               }
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
               case AstUnaryOperator::Negate: {
                  // Negation may be overloaded via the __unm metamethod on tables and objects;
                  // Any/Unknown operands may hold such values
                  InferredType operand_type;
                  if (payload->operand) operand_type = this->infer_expression_type(*payload->operand);
                  if (operand_type.primary IS TiriType::Table or operand_type.primary IS TiriType::Object or
                      operand_type.primary IS TiriType::Any or operand_type.primary IS TiriType::Unknown) {
                     result.primary = TiriType::Any;
                     return result;
                  }
                  result.primary = TiriType::Num;
                  return result;
               }
               case AstUnaryOperator::BitNot:
                  result.primary = TiriType::Num;
                  return result;
               case AstUnaryOperator::Length:
                  result.primary = TiriType::Num;
                  if (payload->operand) {
                     InferredType operand_type = this->infer_expression_type(*payload->operand);
                     result.is_nullable = operand_type.primary IS TiriType::Table or
                        operand_type.primary IS TiriType::Any or operand_type.primary IS TiriType::Unknown;
                  }
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

InferredType TypeAnalyser::refine_static_expression_type(const ExprNode &Expr, InferredType Type) const
{
   if (not Expr.static_value) return Type;

   const StaticValueDescriptor &descriptor = this->ctx_.descriptors().value(Expr.static_value);
   if (Type.primary IS TiriType::Array and descriptor.primary IS TiriType::Array) {
      Type.array_element = descriptor.array_element;
      return Type;
   }
   if (Type.primary != TiriType::Any and Type.primary != TiriType::Unknown) return Type;

   if (Expr.kind IS AstNodeKind::IdentifierExpr) {
      const auto &reference = std::get<NameRef>(Expr.data);
      if (reference.binding_id and this->explicit_variant_bindings_.contains(reference.binding_id)) return Type;
   }

   if (descriptor.primary IS TiriType::Unknown or descriptor.primary IS TiriType::Any) return Type;

   Type.primary = descriptor.primary;
   Type.is_nullable = descriptor.nullable;
   Type.object_class_id = descriptor.object_class_id;
   Type.struct_def = descriptor.struct_def;
   Type.array_element = descriptor.array_element;
   Type.is_fixed = true;
   return Type;
}

bool TypeAnalyser::is_variant_expression(const ExprNode &Expr, size_t Position) const
{
   if (Expr.kind IS AstNodeKind::IdentifierExpr) {
      const auto &reference = std::get<NameRef>(Expr.data);
      if (reference.binding_id) return this->explicit_variant_bindings_.contains(reference.binding_id);
      if (reference.identifier.symbol) {
         auto global = this->global_types_.find(reference.identifier.symbol);
         return global != this->global_types_.end() and
            global->second.contract_policy IS GlobalContractPolicy::Variant;
      }
   }
   else if (Expr.kind IS AstNodeKind::CallExpr or Expr.kind IS AstNodeKind::SafeCallExpr) {
      const auto &call = std::get<CallExprPayload>(Expr.data);
      if (Position IS 0 and call.result_type IS TiriType::Any) return true;

      const FunctionExprPayload *target = this->resolve_call_target(call);
      return target and (target->return_types.is_explicit or target->return_types.is_inferred) and
         Position < target->return_types.count and
         target->return_types.types[Position] IS TiriType::Any;
   }
   return false;
}

//********************************************************************************************************************
// Multi-Value Return Type Inference: Infers the return type at a specific position from a function call expression.
// Used for multi-value assignments like: local a, b, c = func()
// where func() returns multiple values and we need to know the type of each.

[[nodiscard]] InferredType TypeAnalyser::infer_call_return_type(const ExprNode& Expr, size_t Position) const
{
   InferredType result;
   result.primary = TiriType::Any;

   if (Expr.static_results) {
      const StaticValueDescriptor descriptor = this->ctx_.descriptors().results(Expr.static_results).value_at(Position);
      result.primary = descriptor.primary;
      result.is_nullable = descriptor.nullable;
      result.object_class_id = descriptor.object_class_id;
      result.struct_def = descriptor.struct_def;
      result.array_element = descriptor.array_element;
      return result;
   }

   if (Expr.kind != AstNodeKind::CallExpr and Expr.kind != AstNodeKind::SafeCallExpr) return result;

   auto *payload = std::get_if<CallExprPayload>(&Expr.data);
   if (not payload) return result;

   const FunctionExprPayload* target = this->resolve_call_target(*payload);
   if (not target) return result;

   if (not target->return_types.is_explicit and not target->return_types.is_inferred) return result;

   if (Position >= target->return_types.count and not target->return_types.is_variadic) {
      result.primary = TiriType::Nil;
      result.is_nullable = true;
      return result;
   }

   // Get the type at the requested position
   TiriType type = target->return_types.type_at(Position);
   if (type != TiriType::Unknown) {
      const size_t type_position = Position < target->return_types.count ?
         Position : size_t(target->return_types.count - 1);
      result.primary = type;
      result.is_nullable = not target->return_types.required_at(Position);
      result.object_class_id = target->return_types.object_class_ids[type_position];
      result.struct_def = target->return_types.struct_defs[type_position];
      result.array_element = target->return_types.array_elements[type_position];
   }

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
      if (param) return param;
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

const FunctionExprPayload * TypeAnalyser::resolve_call_target(const CallExprPayload &Call) const
{
   if (Call.callable) {
      const auto &callable = this->ctx_.descriptors().callable(Call.callable);
      if (callable.immutable and callable.function) return callable.function;
   }

   const CallTarget &Target = Call.target;
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
   if (auto global = this->global_types_.find(Name); global != this->global_types_.end()) {
      return global->second.function;
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

void TypeAnalyser::publish_binding_type(StaticBindingID Binding, const InferredType &Type)
{
   if (not Binding or not Type.is_fixed or Type.primary IS TiriType::Unknown or
       Type.primary IS TiriType::Any or Type.primary IS TiriType::Nil) return;

   StaticValueDescriptor value;
   value.primary = Type.primary;
   value.object_class_id = Type.object_class_id;
   value.struct_def = Type.struct_def;
   value.array_element = Type.array_element;
   value.nullable = Type.is_nullable;
   value.proof = StaticProof::Advisory;
   this->ctx_.descriptors().binding(Binding).analysed_value = this->ctx_.descriptors().add_value(value);
}

void TypeAnalyser::fix_local_type(GCstr *Name, StaticBindingID Binding, TiriType Type,
   CLASSID ObjectClassId, struct_record *StructDef, ArrayElementDescriptor ArrayElement)
{
   for (auto it = this->scope_stack_.rbegin(); it != this->scope_stack_.rend(); ++it) {
      auto existing = it->lookup_local_type(Name);
      if (existing) {
         it->fix_local_type(Name, Type, ObjectClassId, StructDef, ArrayElement);
         InferredType fixed(Type, false, false, true, ObjectClassId);
         fixed.struct_def = StructDef;
         fixed.array_element = ArrayElement;
         this->publish_binding_type(Binding, fixed);
         this->trace_fix(this->ctx_.lex().linenumber, Name, Type);
         return;
      }
   }
}

void TypeAnalyser::mark_dynamic_ingress(GCstr *Name, bool IsGlobal, bool RequiresDestination)
{
   if (not Name) return;

   if (IsGlobal) {
      if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) {
         it->second.type.primary = TiriType::Any;
         it->second.type.is_fixed = false;
         it->second.type.requires_destination_type = RequiresDestination;
      }
      return;
   }

   for (auto it = this->scope_stack_.rbegin(); it != this->scope_stack_.rend(); ++it) {
      if (it->lookup_local_type(Name)) {
         it->mark_dynamic_ingress(Name, RequiresDestination);
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

void TypeAnalyser::declare_global(GCstr *Name, const InferredType &Type, SourceSpan Location, bool IsConst,
   GlobalContractPolicy ContractPolicy)
{
   if (not Name) return;

   if (auto existing = this->global_types_.find(Name); existing != this->global_types_.end() and
       existing->second.contract_policy != GlobalContractPolicy::Advisory) {
      if (existing->second.is_const) {
         TypeDiagnostic diag;
         diag.location = Location;
         diag.code = ParserErrorCode::AssignToConstant;
         diag.message = std::format("cannot redeclare const global '{}'", std::string_view(strdata(Name), Name->len));
         this->record_diagnostic(std::move(diag));
         return;
      }

      if (ContractPolicy IS GlobalContractPolicy::Advisory) return;

      RuntimeContractEntry established = global_contract_entry(existing->second.type, existing->second.is_const);
      RuntimeContractEntry incoming = global_contract_entry(Type, IsConst);
      if (not global_contract_entries_equivalent(established, incoming)) {
         TypeDiagnostic diag;
         diag.location = Location;
         diag.expected = existing->second.type.primary;
         diag.actual = Type.primary;
         diag.code = ParserErrorCode::TypeMismatchAssignment;
         diag.message = std::format("cannot redeclare global '{}' from '{}' to '{}'",
            std::string_view(strdata(Name), Name->len), global_contract_type_name(existing->second.type),
            global_contract_type_name(Type));
         this->record_diagnostic(std::move(diag));
      }
      return;
   }

   GlobalTypeInfo info;
   info.type = Type;
   info.location = Location;
   info.is_const = IsConst;
   info.contract_policy = ContractPolicy;
   this->global_types_[Name] = info;
   this->trace_decl(this->ctx_.lex().linenumber, Name, Type.primary, Type.is_fixed);
}

void TypeAnalyser::declare_global_function(GCstr *Name, const FunctionExprPayload *Function, SourceSpan Location)
{
   if (not Name) return;

   const FunctionExprPayload *forward_declaration = nullptr;
   if (auto existing = this->global_types_.find(Name); existing != this->global_types_.end()) {
      if (existing->second.is_const or
          (existing->second.contract_policy != GlobalContractPolicy::Advisory and
           existing->second.type.primary != TiriType::Func)) {
         TypeDiagnostic diag;
         diag.location = Location;
         diag.expected = existing->second.type.primary;
         diag.actual = TiriType::Func;
         diag.code = existing->second.is_const ?
            ParserErrorCode::AssignToConstant : ParserErrorCode::TypeMismatchAssignment;
         diag.message = existing->second.is_const ?
            std::format("cannot redeclare const global '{}'", std::string_view(strdata(Name), Name->len)) :
            std::format("cannot redeclare global '{}' from '{}' to 'func'",
               std::string_view(strdata(Name), Name->len), global_contract_type_name(existing->second.type));
         this->record_diagnostic(std::move(diag));
         return;
      }
      forward_declaration = existing->second.forward_declaration;
      if (not forward_declaration and existing->second.function and
          is_function_forward_declaration(*existing->second.function)) {
         forward_declaration = existing->second.function;
      }
   }

   if (forward_declaration and Function) {
      if (auto mismatch = function_signature_mismatch(*forward_declaration, *Function)) {
         TypeDiagnostic diag;
         diag.location = Location;
         diag.code = ParserErrorCode::FunctionSignatureMismatch;
         diag.message = std::format("signature for global function '{}' does not match its forward declaration: {}",
            std::string_view(strdata(Name), Name->len), *mismatch);
         this->record_diagnostic(std::move(diag));
         return;
      }
   }

   GlobalTypeInfo info;
   info.type.primary = TiriType::Func;
   info.type.is_fixed = true;  // Functions have fixed type
   info.location = Location;
   info.function = Function;
   info.forward_declaration = forward_declaration;
   info.contract_policy = GlobalContractPolicy::Enforced;
   if (not info.forward_declaration and Function and is_function_forward_declaration(*Function)) {
      info.forward_declaration = Function;
   }
   this->global_types_[Name] = info;
   this->trace_decl(this->ctx_.lex().linenumber, Name, TiriType::Func, true);
}

// Records an ExistingGlobal assignment that has no exact persisted contract.  Storage identity is supplied by the
// assignment resolver; this advisory type never decides whether the target is local or global.

void TypeAnalyser::record_implicit_global(GCstr *Name, InferredType Type, SourceSpan Location)
{
   if (not Name or (Name->flags & STRFLAG_PROTECTED_GLOBAL) != 0) return;

   Type.requires_destination_type = false;
   if (Type.primary != TiriType::Nil and Type.primary != TiriType::Any and Type.primary != TiriType::Unknown) {
      Type.is_fixed = true;
   }
   else Type.is_fixed = false;

   GlobalTypeInfo info;
   info.type = Type;
   info.location = Location;
   info.implicit = true;
   this->global_types_[Name] = info;
   this->trace_decl(this->ctx_.lex().linenumber, Name, Type.primary, Type.is_fixed);
}

TypeAnalyser::EnvironmentGlobalFacts TypeAnalyser::environment_global_facts(GCstr *Name) const
{
   EnvironmentGlobalFacts facts;
   if (not Name) return facts;

   GCtab *environment = tabref(this->ctx_.lua().env);
   if (not environment) return facts;

   cTValue *value = lj_tab_getstr(environment, Name);
   facts.exists = value and not tvisnil(value);

   GCstr *persisted = lj_tab_get_global_contract(environment, Name);
   if (not persisted) return facts;
   facts.exists = true;

   RuntimeContractDescriptor descriptor;
   if (not decode_runtime_contract(persisted, descriptor) or
       descriptor.boundary != ContractBoundary::Global or descriptor.contract_count IS 0) return facts;

   const RuntimeContractEntry &entry = descriptor.entries[0];
   InferredType type;
   type.primary = entry.type;
   type.object_class_id = entry.type IS TiriType::Object ? entry.object_class_id : CLASSID::NIL;
   type.is_nullable = (entry.flags & contract_flag(ContractEntryFlag::Nullable)) != 0;
   type.is_fixed = entry.type != TiriType::Any and entry.type != TiriType::Unknown;

   if (entry.type IS TiriType::Struct and not entry.constraint_name.empty()) {
      type.struct_def = find_struct(&this->ctx_.lua(), entry.constraint_name);
   }
   else if (entry.type IS TiriType::Array and entry.array_element_type != AET::MAX) {
      std::string element_name;
      if (entry.array_element_type IS AET::STRUCT and not entry.constraint_name.empty()) {
         element_name = std::format("struct<{}>", entry.constraint_name);
      }
      else if (entry.array_element_type IS AET::ARRAY and not entry.constraint_name.empty()) {
         element_name.assign(entry.constraint_name);
      }
      else element_name = lj_array_elemtype_name(entry.array_element_type);

      if (auto element = parse_array_element_type(element_name, &this->ctx_.lua())) {
         type.array_element = *element;
      }
   }

   facts.is_const = contract_entry_is_const(entry);
   facts.exact_type = type;
   facts.contract_policy = entry.type IS TiriType::Any ?
      GlobalContractPolicy::Variant : GlobalContractPolicy::Enforced;
   return facts;
}

void TypeAnalyser::seed_environment_global(GCstr *Name, SourceSpan Location)
{
   if (not Name or this->global_types_.contains(Name)) return;
   EnvironmentGlobalFacts facts = this->environment_global_facts(Name);
   if (not facts.exists or not facts.exact_type) return;

   GlobalTypeInfo info;
   info.type = *facts.exact_type;
   info.location = Location;
   info.is_const = facts.is_const;
   info.environment_contract = true;
   info.contract_policy = facts.contract_policy;
   this->global_types_[Name] = info;
   this->trace_decl(this->ctx_.lex().linenumber, Name, info.type.primary, info.type.is_fixed);
}

std::optional<InferredType> TypeAnalyser::lookup_global_type(GCstr *Name) const
{
   if (not Name) return std::nullopt;
   if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) return it->second.type;
   return std::nullopt;
}

bool TypeAnalyser::is_implicit_global(GCstr *Name) const
{
   if (not Name) return false;
   if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) return it->second.implicit;
   return false;
}

bool TypeAnalyser::has_environment_contract(GCstr *Name) const
{
   if (not Name) return false;
   if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) {
      return it->second.environment_contract;
   }
   return false;
}

// Degrades a global's tracked type to 'any', disabling specialised access without raising diagnostics.  Applied
// when an implicit global receives assignments of conflicting types.

void TypeAnalyser::degrade_global_type(GCstr *Name)
{
   if (not Name) return;
   if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) {
      it->second.type.primary = TiriType::Any;
      it->second.type.is_fixed = true;  // 'any' accepts every subsequent assignment
      it->second.type.object_class_id = CLASSID::NIL;
      it->second.type.struct_def = nullptr;
      this->trace_fix(this->ctx_.lex().linenumber, Name, TiriType::Any);
   }
}

// A reduced declaration may fail before its environment policy is installed.  Clear state that depends on whether
// the declaration executes successfully, while retaining any forward signature that constrains later definitions.

void TypeAnalyser::invalidate_global_flow_policy(GCstr *Name, SourceSpan Location)
{
   if (not Name) return;

   const FunctionExprPayload *forward_declaration = nullptr;
   if (auto existing = this->global_types_.find(Name); existing != this->global_types_.end()) {
      if (existing->second.contract_policy != GlobalContractPolicy::Advisory) return;
      forward_declaration = existing->second.forward_declaration;
      if (not forward_declaration and existing->second.function and
          is_function_forward_declaration(*existing->second.function)) {
         forward_declaration = existing->second.function;
      }
   }

   GlobalTypeInfo info;
   info.type.primary = TiriType::Any;
   info.type.is_fixed = true;
   info.location = Location;
   info.forward_declaration = forward_declaration;
   this->global_types_[Name] = info;
   this->trace_decl(this->ctx_.lex().linenumber, Name, TiriType::Any, true);
}

void TypeAnalyser::fix_global_type(GCstr *Name, TiriType Type, CLASSID ObjectClassId, struct_record *StructDef,
   ArrayElementDescriptor ArrayElement)
{
   if (not Name) return;
   if (auto it = this->global_types_.find(Name); it != this->global_types_.end()) {
      it->second.type.primary = Type;
      it->second.type.is_fixed = true;
      it->second.type.object_class_id = ObjectClassId;
      it->second.type.struct_def = StructDef;
      it->second.type.array_element = ArrayElement;
      if (not it->second.implicit) it->second.contract_policy = GlobalContractPolicy::Enforced;
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
// - Declared-position validation without constraining result count
// - First-wins inference rule for functions without explicit return type declarations
// - Nil remains valid for nullable slots and is rejected for required slots

void TypeAnalyser::validate_return_types(const ReturnStmtPayload &Return, SourceSpan Location)
{
   FunctionContext* ctx = this->current_function();
   if (not ctx) return;  // Not inside a function (shouldn't happen in valid code)

   size_t return_count = Return.values.size();
   size_t fixed_count = return_count;
   const ExprNode *tail_expression = nullptr;
   const StaticResultSet *tail_results = nullptr;
   if (Return.forwards_call and not Return.values.empty()) {
      tail_expression = Return.values.back().get();
      if (tail_expression->static_results) {
         const StaticResultSet &results = this->ctx_.descriptors().results(tail_expression->static_results);
         if (not results.dynamic and not results.variadic) {
            fixed_count--;
            tail_results = &results;
            return_count = fixed_count + results.declared_count;
         }
      }
   }

   auto expression_at = [&](size_t Position) -> const ExprNode & {
      return Position < fixed_count ? *Return.values[Position] : *tail_expression;
   };

   auto infer_position = [&](size_t Position) {
      const ExprNode &expression = expression_at(Position);
      if (tail_results and Position >= fixed_count) {
         return this->infer_call_return_type(expression, Position - fixed_count);
      }
      InferredType actual = this->infer_expression_type(expression);
      return this->refine_static_expression_type(expression, actual);
   };

   if (ctx->expected_returns.is_explicit) {
      // Explicit declaration: validate against declared types

      for (size_t i = return_count; i < ctx->expected_returns.count; ++i) {
         if (not ctx->expected_returns.required_at(i)) continue;
         TypeDiagnostic diag;
         diag.location = Location;
         diag.expected = ctx->expected_returns.type_at(i);
         diag.actual = TiriType::Nil;
         diag.code = ParserErrorCode::ReturnTypeMismatch;
         diag.message = std::format("required return value {} is missing", i + 1);
         this->record_diagnostic(std::move(diag));
      }

      // Validate type of each returned value
      for (size_t i = 0; i < return_count and i < MAX_RETURN_TYPES; ++i) {
         TiriType expected = ctx->expected_returns.type_at(i);
         const ExprNode &expression = expression_at(i);
         InferredType actual = tail_results and i > fixed_count ?
            infer_position(i) : this->infer_expression_type(expression);

         if (actual.primary IS TiriType::Nil) {
            if (ctx->expected_returns.required_at(i)) {
               TypeDiagnostic diag;
               diag.location = expression.span;
               diag.expected = expected;
               diag.actual = actual.primary;
               diag.code = ParserErrorCode::ReturnTypeMismatch;
               diag.message = std::format("required return value {} cannot be nil", i + 1);
               this->record_diagnostic(std::move(diag));
            }
            continue;
         }
         if (expected IS TiriType::Any or expected IS TiriType::Unknown) continue;
         // A concrete declaration supplies the destination type for runtime validation of dynamic values.
         if (actual.primary IS TiriType::Any or actual.primary IS TiriType::Unknown) continue;

         const bool struct_matches = expected != TiriType::Struct or not ctx->expected_returns.struct_defs[i] or
            actual.struct_def IS ctx->expected_returns.struct_defs[i];
         const bool array_matches = expected != TiriType::Array or actual.primary != TiriType::Array or
            not actual.array_element.known or
            array_element_matches(ctx->expected_returns.array_elements[i], actual.array_element);
         if (actual.primary != expected or not struct_matches or not array_matches) {
            TypeDiagnostic diag;
            diag.location = expression.span;
            diag.expected = expected;
            diag.actual = actual.primary;
            diag.code = ParserErrorCode::ReturnTypeMismatch;
            diag.message = std::format("return type mismatch at position {}: expected '{}', got '{}'",
               i + 1, type_name(expected), type_name(actual.primary));
            this->record_diagnostic(std::move(diag));
         }
      }
   }
   else {
      const size_t check_count = std::min(return_count, size_t(MAX_RETURN_TYPES));
      ctx->observed_return_count = uint8_t(std::max<size_t>(ctx->observed_return_count, check_count));

      for (size_t i = 0; i < check_count; ++i) {
         InferredReturnPosition &position = ctx->inferred_returns[i];
         const ExprNode &expression = expression_at(i);
         const size_t expression_position = tail_results and i >= fixed_count ? i - fixed_count : 0;
         InferredType actual = infer_position(i);
         position.location = expression.span;

         if (actual.primary IS TiriType::Nil) {
            if (position.state IS ReturnInferenceState::Unobserved) {
               position.state = ReturnInferenceState::NilOnly;
            }
            continue;
         }

         if (((actual.primary IS TiriType::Any or actual.primary IS TiriType::Unknown) and
              this->is_variant_expression(expression, expression_position)) or
             (actual.primary IS TiriType::Any and is_table_read_expression(expression))) {
            position.concrete = actual;
            position.concrete.primary = TiriType::Any;
            position.state = ReturnInferenceState::ExplicitAny;
            continue;
         }

         if (position.state IS ReturnInferenceState::ExplicitAny) continue;

         if (actual.primary IS TiriType::Any or actual.primary IS TiriType::Unknown) {
            position.state = ReturnInferenceState::Dynamic;
            position.dynamic_location = expression.span;
            position.dynamic_type = actual.primary;
            continue;
         }

         if (position.concrete.primary IS TiriType::Any or position.concrete.primary IS TiriType::Unknown) {
            position.concrete = actual;
            if (position.state != ReturnInferenceState::Dynamic) position.state = ReturnInferenceState::Concrete;
            continue;
         }

         const bool type_matches = actual.primary IS position.concrete.primary;
         const bool class_matches = position.concrete.primary != TiriType::Object or
            position.concrete.object_class_id IS CLASSID::NIL or
            actual.object_class_id IS position.concrete.object_class_id;
         const bool struct_matches = position.concrete.primary != TiriType::Struct or
            not position.concrete.struct_def or actual.struct_def IS position.concrete.struct_def;
         const bool array_matches = position.concrete.primary != TiriType::Array or
            array_element_matches(position.concrete.array_element, actual.array_element);
         if (type_matches and class_matches and struct_matches and array_matches) continue;

         TypeDiagnostic diag;
         diag.location = expression.span;
         diag.expected = position.concrete.primary;
         diag.actual = actual.primary;
         diag.code = ParserErrorCode::ReturnTypeMismatch;
         diag.message = std::format(
            "inconsistent return type at position {}: first return established '{}', but this returns '{}'",
            i + 1, type_name(position.concrete.primary), type_name(actual.primary));
         this->record_diagnostic(std::move(diag));
         ctx->inference_failed = true;
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
         case AstNodeKind::RangeForStmt: {
            auto *payload = std::get_if<RangeForStmtPayload>(&stmt->data);
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
         case AstNodeKind::ContextStmt: {
            auto *payload = std::get_if<ContextStmtPayload>(&stmt->data);
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
      case AstNodeKind::RangeForStmt: {
         auto *payload = std::get_if<RangeForStmtPayload>(&Stmt.data);
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
      case AstNodeKind::ContextStmt: {
         auto *payload = std::get_if<ContextStmtPayload>(&Stmt.data);
         if (payload) {
            if (payload->reference and this->expression_contains_call_to(*payload->reference, Name)) return true;
            if (payload->block and this->body_contains_call_to(*payload->block, Name)) return true;
         }
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
      case AstNodeKind::ComparisonChainExpr: {
         auto *payload = std::get_if<ComparisonChainExprPayload>(&Expr.data);
         if (payload) {
            for (const auto &operand : payload->operands) {
               if (operand and this->expression_contains_call_to(*operand, Name)) return true;
            }
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
      case AstNodeKind::TypeTestExpr: {
         auto *payload = std::get_if<TypeTestExprPayload>(&Expr.data);
         return payload and payload->value and this->expression_contains_call_to(*payload->value, Name);
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
      diagnostic.file_index = diag.file_index;
      diagnostic.message = diag.message;
      diagnostic.token = Token::from_span(diag.location);
      Context.diagnostics().report(diagnostic);
   }
}

//********************************************************************************************************************
// Entry Point: Called from the parser after AST construction to run semantic type analysis.  Creates a TypeAnalyser
// instance, runs analysis on the module, and publishes any collected diagnostics.

// Publish global type policy to the lexer state so that IR emission can attach metadata to global reads, enforce
// sticky contracts for declared globals, and preserve explicit 'any' as the variant opt-out.

void TypeAnalyser::publish_global_type_hints(LexState &Lex) const
{
   for (const auto &[name, info] : this->global_types_) {
      if (info.environment_contract) {
         if (info.type.is_fixed and
             (info.type.primary IS TiriType::Struct or info.type.primary IS TiriType::Object or
              info.type.primary IS TiriType::Array)) {
            Lex.global_type_hints[name] = {
               info.type.primary, info.type.object_class_id, info.type.struct_def, info.type.array_element,
               GlobalContractPolicy::Advisory
            };
         }
         continue;
      }
      if (info.contract_policy IS GlobalContractPolicy::Variant) {
         Lex.global_type_hints[name] = {
            TiriType::Any, CLASSID::NIL, nullptr, {}, GlobalContractPolicy::Variant
         };
         continue;
      }
      if (info.is_const and not info.type.is_fixed) {
         Lex.global_type_hints[name] = {
            TiriType::Any, CLASSID::NIL, nullptr, {}, GlobalContractPolicy::Enforced
         };
         continue;
      }
      if (not info.type.is_fixed) continue;
      if (info.contract_policy IS GlobalContractPolicy::Enforced) {
         Lex.global_type_hints[name] = {
            info.type.primary, info.type.object_class_id, info.type.struct_def, info.type.array_element,
            GlobalContractPolicy::Enforced
         };
         continue;
      }
      switch (info.type.primary) {
         case TiriType::Struct:
         case TiriType::Object:
         case TiriType::Array:
            Lex.global_type_hints[name] = {
               info.type.primary, info.type.object_class_id, info.type.struct_def, info.type.array_element,
               GlobalContractPolicy::Advisory
            };
            break;
         default:
            break;
      }
   }
}

//********************************************************************************************************************

void run_type_analysis(ParserContext& Context, BlockStmt& Module)
{
   TypeAnalyser analyser(Context);
   analyser.analyse_module(Module);
   analyser.publish_global_type_hints(Context.lex());
   publish_type_diagnostics(Context, analyser.diagnostics());
}
