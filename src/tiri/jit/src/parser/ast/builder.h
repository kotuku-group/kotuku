// AST builder that threads typed tokens through the parser and produces the schema declared in ast_nodes.h without
// touching FuncState/bytecode state.
//
// The top-level parse contract is `parse_chunk()` which returns ownership of the root BlockStmt describing the
// current chunk; `IrEmitter::emit_chunk()` consumes that BlockStmt to generate bytecode, so the builder and emitter
// can evolve independently while sharing a single AST boundary.

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "nodes.h"
#include "../parser_context.h"

class AstBuilder {
public:
   explicit AstBuilder(ParserContext& context);

   ParserResult<std::unique_ptr<BlockStmt>> parse_chunk();
   ParserResult<ExprNodePtr> parse_expression(uint8_t precedence = 0);
   ParserResult<ExprNodeList> parse_expression_list();

   [[nodiscard]] bool at_top_level() const { return function_depth_ == 0; }

private:
   ParserContext& ctx;
   bool in_guard_expression = false;  // True when parsing 'when' clause guard expression
   bool in_choose_expression = false; // True when parsing choose expression cases (for tuple pattern detection)
   int function_depth_ = 0;           // Tracks nesting depth inside function bodies

   struct BinaryOpInfo {
      AstBinaryOperator op = AstBinaryOperator::Add;
      uint8_t left = 0;
      uint8_t right = 0;
   };

   [[nodiscard]] ParserResult<std::unique_ptr<BlockStmt>> parse_block(std::span<const TokenKind> terminators);
   ParserResult<StmtNodePtr> parse_statement();
   ParserResult<StmtNodePtr> parse_local();
   ParserResult<StmtNodePtr> parse_global();
   ParserResult<StmtNodePtr> parse_function_stmt();
   ParserResult<StmtNodePtr> parse_annotated_statement();
   ParserResult<std::vector<AnnotationEntry>> parse_annotations();
   ParserResult<AnnotationArgValue> parse_annotation_value();
   ParserResult<StmtNodePtr> parse_if();
   ParserResult<StmtNodePtr> parse_while();
   ParserResult<StmtNodePtr> parse_repeat();
   ParserResult<StmtNodePtr> parse_for();
   ParserResult<StmtNodePtr> parse_anonymous_for(const Token &);
   ParserResult<ExprNodePtr> parse_range_in_braces();
   ParserResult<StmtNodePtr> parse_do();
   ParserResult<StmtNodePtr> parse_with();
   ParserResult<StmtNodePtr> parse_defer();
   ParserResult<StmtNodePtr> parse_return();
   ParserResult<StmtNodePtr> parse_try();
   ParserResult<StmtNodePtr> parse_raise();
   ParserResult<StmtNodePtr> parse_check();
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
   ParserResult<ExprNodePtr> parse_arrow_function(ExprNodeList parameters);
   ParserResult<ExprNodePtr> parse_function_literal(const Token &, bool is_thunk = false);
   ParserResult<ExprNodePtr> parse_table_literal();
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
   [[nodiscard]] bool is_statement_start(TokenKind kind) const;
   [[nodiscard]] bool is_synchronisation_point(std::span<const TokenKind> terminators) const;
   [[nodiscard]] size_t skip_to_synchronisation_point(std::span<const TokenKind> terminators);
   [[nodiscard]] static Identifier make_identifier(const Token &);
   [[nodiscard]] static LiteralValue make_literal(const Token &);
   [[nodiscard]] inline SourceSpan span_from(const Token &Token) { return Token.span(); }
   [[nodiscard]] inline SourceSpan span_from(const Token &Start, const Token &End) const { return combine_spans(Start.span(), End.span()); }
   [[nodiscard]] std::optional<BinaryOpInfo> match_binary_operator(const Token &) const;
   [[nodiscard]] bool is_choose_relational_pattern(size_t) const;

   // Helper to emit an error and return a failure result in one step.
   // Reduces boilerplate for the common pattern of emit_error + return failure.
   template<typename T>
   [[nodiscard]] ParserResult<T> fail(ParserErrorCode Code, const Token& ErrorToken, std::string Message) {
      this->ctx.emit_error(Code, ErrorToken, Message);
      return ParserResult<T>::failure(ParserError(Code, ErrorToken, std::move(Message)));
   }

   // Helper to map TokenKind to AssignmentOperator.
   // Returns std::nullopt if the token is not an assignment operator.
   [[nodiscard]] static std::optional<AssignmentOperator> token_to_assignment_op(TokenKind Kind);
};
