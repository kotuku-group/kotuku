// AST builder that threads typed tokens through the parser and produces the schema declared in ast_nodes.h without
// touching FuncState/bytecode state.
//
// The top-level parse contract is `parse_chunk()` which returns ownership of the root BlockStmt describing the
// current chunk; `IrEmitter::emit_chunk()` consumes that BlockStmt to generate bytecode, so the builder and emitter
// can evolve independently while sharing a single AST boundary.

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nodes.h"
#include "../parser_context.h"

class AstBuilder {
public:
   explicit AstBuilder(ParserContext& context, AstBuilder *Parent = nullptr);
   ~AstBuilder();

   AstBuilder(const AstBuilder &) = delete;
   AstBuilder& operator=(const AstBuilder &) = delete;

   ParserResult<std::unique_ptr<BlockStmt>> parse_chunk();
   ParserResult<ExprNodePtr> parse_expression(uint8_t precedence = 0);
   ParserResult<ExprNodeList> parse_expression_list();
   void commit_registered_enum_constants();
   void rollback_registered_enum_constants();
   void rollback_registered_enum_hierarchy();
   void commit_registered_structs();
   void rollback_registered_structs();

   [[nodiscard]] bool at_top_level() const { return function_depth IS 0 and block_depth IS 0; }

private:
   ParserContext& ctx;
   bool in_guard_expression = false;  // True when parsing 'when' clause guard expression
   bool in_choose_expression = false; // True when parsing choose expression cases (for tuple pattern detection)
   int function_depth = 0;           // Tracks nesting depth inside function bodies
   int block_depth = 0;              // Tracks nested statement blocks below chunk scope
   bool enum_constants_committed = false;
   bool source_namespace_declared = false;
   AstBuilder *parent_builder = nullptr;
   std::vector<GCstr *> function_name_stack;
   std::vector<uint32_t> registered_enum_constants;
   std::vector<uint32_t> registered_structs;
   std::vector<uint32_t> chunk_import_hashes;  // Path hashes inlined during this compilation (root builder only)
   StmtNodeList pending_statements;

   // One record per canonical module declared by this compilation unit.  Aliases of the same module share a
   // dependency, so a function referenced through two namespaces materialises exactly one hidden callable.
   //
   // The activation statement is emitted when the declaration is parsed, then its hidden bindings are finalised once
   // the whole unit has been seen and the complete ordered function list is known.

   struct ModuleFunctionBinding {
      GCstr *function = nullptr;
      GCstr *binding = nullptr;
   };

   struct ModuleDependency {
      std::string canonical_module;
      std::vector<ModuleFunctionBinding> functions;   // Canonical names and hidden locals, in descriptor order
      StmtNode *activation = nullptr;                 // Generated activation declaration, finalised after parsing
      uint32_t descriptor = 0;                        // Index in the prototype dependency table
      SourceSpan declaration_span{};
      bool implicit = false;                          // Created on demand by an implicit namespace such as mSys
   };

   struct ModuleNamespaceSymbol {
      GCstr *source_name = nullptr;
      StaticModuleHandle module = nullptr;
      std::string canonical_module;
      size_t dependency = 0;                          // Index into module_dependencies
   };

   std::unordered_map<GCstr *, ModuleNamespaceSymbol> module_namespaces;
   std::vector<std::unique_ptr<ModuleDependency>> module_dependencies;
   std::unordered_map<std::string, bool> module_availability;

   // FileSource entries persist across compilations, so diagnose-mode re-parsing of cached imports needs a
   // dedup scope limited to the current chunk.  Records Hash on the root builder; returns true if already seen.
   [[nodiscard]] bool import_seen_this_chunk(uint32_t Hash) {
      AstBuilder *root = this;
      while (root->parent_builder) root = root->parent_builder;
      if (std::find(root->chunk_import_hashes.begin(), root->chunk_import_hashes.end(), Hash) != root->chunk_import_hashes.end()) return true;
      root->chunk_import_hashes.push_back(Hash);
      return false;
   }

   class FunctionNameScope {
   public:
      FunctionNameScope(AstBuilder &Builder, GCstr *FunctionName);
      ~FunctionNameScope();

      FunctionNameScope(const FunctionNameScope &) = delete;
      FunctionNameScope& operator=(const FunctionNameScope &) = delete;

   private:
      AstBuilder &builder;
   };

   class BlockDepthScope {
   public:
      explicit BlockDepthScope(AstBuilder &Builder);
      ~BlockDepthScope();

      BlockDepthScope(const BlockDepthScope &) = delete;
      BlockDepthScope& operator=(const BlockDepthScope &) = delete;

   private:
      AstBuilder &builder;
   };

   struct BinaryOpInfo {
      AstBinaryOperator op = AstBinaryOperator::Add;
      uint8_t left = 0;
      uint8_t right = 0;
   };

   [[nodiscard]] ParserResult<std::unique_ptr<BlockStmt>> parse_block(std::span<const TokenKind> terminators);
   ParserResult<StmtNodePtr> parse_statement();
   ParserResult<StmtNodePtr> parse_explicit_local_declaration();
   ParserResult<StmtNodePtr> parse_bare_annotated_assignment();
   ParserResult<StmtNodePtr> parse_local_name_list(const Token &StartToken, bool BareAnnotatedSyntax);
   ParserResult<StmtNodePtr> parse_global();
   ParserResult<StmtNodePtr> parse_extern();
   ParserResult<StmtNodePtr> parse_enum(const Token &StartToken);
   ParserResult<StmtNodePtr> parse_struct_declaration();
   ParserResult<int64_t> parse_enum_integer_literal();
   void track_registered_enum_constant(uint32_t Hash);
   void adopt_registered_enum_constants(AstBuilder &Child);
   void adopt_registered_structs(AstBuilder &Child);
   void track_registered_struct(uint32_t Key);
   ParserResult<StmtNodePtr> parse_function_stmt();
   ParserResult<StmtNodePtr> parse_annotated_statement();
   ParserResult<std::vector<AnnotationEntry>> parse_annotations();
   ParserResult<AnnotationArgValue> parse_annotation_value();
   ParserResult<StmtNodePtr> parse_if();
   ParserResult<StmtNodePtr> parse_while();
   ParserResult<StmtNodePtr> parse_repeat();
   ParserResult<StmtNodePtr> parse_for();
   ParserResult<StmtNodePtr> parse_anonymous_for(const Token &);
   ParserResult<ExprNodePtr> parse_scanned_range_in_braces(
      bool HasStep, bool HasBareStringOperand);
   ParserResult<StmtNodePtr> parse_do();
   ParserResult<StmtNodePtr> parse_using();
   ParserResult<StmtNodePtr> parse_with();
   ParserResult<StmtNodePtr> parse_defer();
   ParserResult<StmtNodePtr> parse_return();
   ParserResult<StmtNodePtr> parse_try();
   ParserResult<StmtNodePtr> parse_checkall();
   ParserResult<StmtNodePtr> parse_raise();
   ParserResult<StmtNodePtr> parse_check();
   ParserResult<StmtNodePtr> parse_include_stmt();
   ParserResult<StmtNodePtr> parse_module_decl();
   ParserResult<ImportEntryPayload> parse_import_entry(const Token&, bool, bool * = nullptr);
   ParserResult<StmtNodePtr> parse_import();
   ParserResult<StmtNodePtr> parse_namespace();
   ParserResult<std::unique_ptr<BlockStmt>> parse_imported_file(std::string &, std::string_view, const Token& import_token);
   ParserResult<StmtNodePtr> parse_compile_if();
   void skip_to_compile_end();
   ParserResult<StmtNodePtr> parse_expression_stmt();
   ParserResult<ExprNodePtr> parse_choose_expr();
   ParserResult<ExprNodePtr> parse_unary();
   ParserResult<ExprNodePtr> parse_primary();
   ParserResult<ExprNodePtr> parse_suffixed(ExprNodePtr);
   ParserResult<Token> consume_ternary_separator();
   ParserResult<ExprNodePtr> parse_arrow_function(ExprNodeList parameters);
   ParserResult<ExprNodePtr> parse_function_literal(
      const Token &, bool IsThunk = false, GCstr *FunctionName = nullptr);
   ParserResult<ExprNodePtr> parse_table_literal(bool AllowRange = true);
   ParserResult<ExprNodeList> parse_array_initialiser();
   ParserResult<ReturnStmtPayload> parse_return_payload(const Token &, bool same_line_only);
   ParserResult<FunctionReturnTypes> parse_return_type_annotation();

   ParserResult<std::vector<Identifier>> parse_name_list();
   struct ParameterListResult {
      std::vector<FunctionParameter> parameters;
      bool is_vararg = false;
   };

   static inline SourceSpan combine_spans(const SourceSpan &Start, const SourceSpan &End) {
      SourceSpan span = Start;
      span.offset = End.offset;
      span.line   = End.line;
      span.column = End.column;
      return span;
   }

   ParserResult<ParameterListResult> parse_parameter_list(bool);
   ParserResult<Token> parse_type_annotation(
      TiriType &Type, struct_record *&StructDef, ArrayElementDescriptor &ArrayElement, bool &Required);
   ParserResult<TypeTestDescriptor> parse_type_test_descriptor();
   [[nodiscard]] bool choose_case_has_type_test_descriptor() const;
   ParserResult<std::vector<TableField>> parse_table_fields(bool *);
   ParserResult<ExprNodeList> parse_call_arguments(bool *);

   struct ResultFilterInfo {
      uint64_t keep_mask = 0;
      uint8_t explicit_count = 0;
      bool trailing_keep = false;
   };

   ParserResult<ResultFilterInfo> parse_result_filter_pattern();
   ParserResult<ExprNodePtr> parse_result_filter_expr(const Token &);
   ParserResult<std::unique_ptr<BlockStmt>> parse_scoped_block(std::initializer_list<TokenKind>);

   [[nodiscard]] bool at_end_of_block(std::span<const TokenKind>) const;
   [[nodiscard]] bool is_statement_start(TokenKind Kind) const;
   [[nodiscard]] bool is_synchronisation_point(std::span<const TokenKind> terminators) const;
   [[nodiscard]] size_t skip_to_synchronisation_point(std::span<const TokenKind> terminators);
   [[nodiscard]] static Identifier make_identifier(const Token &);
   [[nodiscard]] static LiteralValue make_literal(const Token &);
   [[nodiscard]] GCstr *anonymous_function_name();
   [[nodiscard]] GCstr *current_function_name();
   [[nodiscard]] GCstr *current_source_file();
   [[nodiscard]] inline SourceSpan span_from(const Token &Token) { return Token.span(); }
   [[nodiscard]] inline SourceSpan span_from(const Token &Start, const Token &End) const { return combine_spans(Start.span(), End.span()); }
   [[nodiscard]] std::optional<BinaryOpInfo> match_binary_operator(const Token &) const;
   [[nodiscard]] bool is_choose_relational_pattern(size_t) const;
   [[nodiscard]] bool is_extended_ternary_ahead() const;
   [[nodiscard]] const ModuleNamespaceSymbol *find_module_namespace(GCstr *) const;
   [[nodiscard]] const ModuleNamespaceSymbol *resolve_module_namespace(GCstr *);
   [[nodiscard]] bool is_module_namespace_name(GCstr *) const;
   [[nodiscard]] bool module_is_available(std::string_view);
   [[nodiscard]] std::pair<size_t, bool> find_or_create_module_dependency(
      std::string_view, const SourceSpan &, bool Implicit);
   [[nodiscard]] GCstr *module_function_binding(ModuleDependency &, GCstr *CanonicalFunction);
   [[nodiscard]] StmtNodePtr make_dependency_activation(ModuleDependency &, const SourceSpan &);
   [[nodiscard]] ParserResult<StmtNodePtr> make_assignment_statement(const Token &, AssignmentOperator,
      ExprNodeList, ExprNodeList);
   void finalise_module_dependencies();
   void publish_dependency_descriptors();
   void prepend_implicit_dependencies(BlockStmt &);
   void append_pending_statements(StmtNodeList &);

   // Helper to emit an error and return a failure result in one step.
   // Reduces boilerplate for the common pattern of emit_error + return failure.
   template<typename T>
   [[nodiscard]] ParserResult<T> fail(ParserErrorCode Code, const Token& ErrorToken, std::string Message) {
      this->ctx.emit_error(Code, ErrorToken, Message);
      return ParserResult<T>::failure(ParserError(Code, ErrorToken, std::move(Message),
         this->ctx.lex().current_file_index));
   }

   // Helper to map TokenKind to AssignmentOperator.
   // Returns std::nullopt if the token is not an assignment operator.
   [[nodiscard]] static std::optional<AssignmentOperator> token_to_assignment_op(TokenKind Kind);
};
