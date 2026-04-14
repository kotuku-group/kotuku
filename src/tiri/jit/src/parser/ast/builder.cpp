// AST Builder - Main Entry Point and Core Infrastructure
// Copyright © 2025-2026 Paul Manias
//
// This file contains the core infrastructure for the AST builder:
// - Constructor and main entry point (parse_chunk)
// - Block parsing (parse_block, parse_scoped_block)
// - Statement dispatch (parse_statement)
// - Utility functions (make_identifier, make_literal, at_end_of_block, is_statement_start)
// - Token-to-operator mapping (token_to_assignment_op)

#include "ast/builder.h"

#include <cstring>
#include <format>
#include <utility>

#include "../token_types.h"
#include "../parse_types.h"
#include "runtime/lj_str.h"
#include "runtime/lj_tab.h"
#include "runtime/lj_gc.h"

#include <kotuku/main.h>    // For objFile, ERR
#include <kotuku/modules/core.h>  // For File access

#ifdef INCLUDE_TIPS
#include "../parser_tips.h"
#endif

// Extracts the function payload from an expression node if it's a function expression, otherwise returns null.

static FunctionExprPayload * function_payload_from(ExprNode &Node)
{
   if (Node.kind != AstNodeKind::FunctionExpr) return nullptr;
   return std::get_if<FunctionExprPayload>(&Node.data);
}

// Moves the function payload data out of an expression node, transferring ownership of parameters and body.

static std::unique_ptr<FunctionExprPayload> move_function_payload(ExprNodePtr &Node)
{
   auto *payload = function_payload_from(*Node);
   if (payload IS nullptr) return std::make_unique<FunctionExprPayload>();

   std::unique_ptr<FunctionExprPayload> result = std::make_unique<FunctionExprPayload>();
   result->parameters        = std::move(payload->parameters);
   result->is_vararg         = payload->is_vararg;
   result->is_thunk          = payload->is_thunk;
   result->thunk_return_type = payload->thunk_return_type;
   result->return_types      = payload->return_types;  // Copy return type information
   result->body              = std::move(payload->body);
   result->annotations = std::move(payload->annotations);
   return result;
}

// Checks if a token kind is a statement keyword that can be used in conditional shorthand syntax (e.g., value ?? return).

static bool is_shorthand_statement_keyword(TokenKind Kind)
{
   switch (Kind) {
      case TokenKind::ReturnToken:
      case TokenKind::BreakToken:
      case TokenKind::ContinueToken:
      case TokenKind::RaiseToken:
      case TokenKind::CheckToken:
         return true;
      default:
         return false;
   }
}

// Checks if a statement unconditionally terminates control flow (return, break, continue).

static bool is_terminating_statement(const StmtNode *Stmt)
{
   if (not Stmt) return false;
   switch (Stmt->kind) {
      case AstNodeKind::ReturnStmt:
      case AstNodeKind::BreakStmt:
      case AstNodeKind::ContinueStmt:
         return true;
      default:
         return false;
   }
}

// Checks if a token kind is a compound assignment operator (+=, -=, etc.).
// These are statements, not expressions, which helps provide better error messages.

static bool is_compound_assignment(TokenKind Kind)
{
   switch (Kind) {
      case TokenKind::CompoundAdd:
      case TokenKind::CompoundSub:
      case TokenKind::CompoundMul:
      case TokenKind::CompoundDiv:
      case TokenKind::CompoundMod:
      case TokenKind::CompoundConcat:
      case TokenKind::CompoundIfEmpty:
      case TokenKind::CompoundIfNil:
         return true;
      default:
         return false;
   }
}

// Checks if an expression node is a presence check expression (the ?? operator).

static bool is_presence_expr(const ExprNodePtr &Expr)
{
   return Expr and Expr->kind IS AstNodeKind::PresenceExpr;
}

// Validates that an expression can be used as an arrow function parameter (identifier only).

static bool extract_arrow_parameter(const ExprNodePtr &Expr, FunctionParameter &Parameter)
{
   if (not Expr) return false;
   if (not (Expr->kind IS AstNodeKind::IdentifierExpr)) return false;

   auto *name_ref = std::get_if<NameRef>(&Expr->data);
   if (name_ref IS nullptr) return false;

   Parameter.name = name_ref->identifier;
   return true;
}

// Populates a parameter list from expressions parsed before the arrow token.

static bool build_arrow_parameters(const ExprNodeList &Expressions, std::vector<FunctionParameter> &Parameters,
   const ExprNodePtr **Invalid)
{
   for (const auto &expr : Expressions) {
      FunctionParameter param;
      if (not extract_arrow_parameter(expr, param)) {
         if (Invalid) *Invalid = &expr;
         return false;
      }
      Parameters.push_back(param);
   }
   return true;
}

static ParserResult<StmtNodePtr> make_control_stmt(ParserContext& Context, AstNodeKind Kind, const Token& Token)
{
   auto node = std::make_unique<StmtNode>(Kind, Token.span());
   if (Kind IS AstNodeKind::BreakStmt) node->data.emplace<BreakStmtPayload>();
   else node->data.emplace<ContinueStmtPayload>();
   Context.tokens().advance();
   return ParserResult<StmtNodePtr>::success(std::move(node));
}

AstBuilder::AstBuilder(ParserContext &Context) : ctx(Context)
{
}

//********************************************************************************************************************
// Main entry point for parsing a chunk (entire source file).

ParserResult<std::unique_ptr<BlockStmt>> AstBuilder::parse_chunk()
{
   const TokenKind terminators[] = { TokenKind::EndOfFile };
   return this->parse_block(terminators);
}

//********************************************************************************************************************
// Parses a block of statements until a terminator token is encountered.  When abort_on_error is false (DIAGNOSE
// mode), uses panic-mode recovery to continue parsing after errors, collecting multiple diagnostics and returning a
// partial AST.

ParserResult<std::unique_ptr<BlockStmt>> AstBuilder::parse_block(std::span<const TokenKind> terminators)
{
   StmtNodeList statements;
   bool recovery_mode = not this->ctx.config().abort_on_error;

   #ifdef INCLUDE_TIPS
   const StmtNode *terminating_stmt = nullptr;  // Track the first terminating statement in this block
   #endif

   while (not this->at_end_of_block(terminators)) {
      Token stmt_start = this->ctx.tokens().current();
      auto stmt = this->parse_statement();

      if (not stmt.ok()) {
         if (not recovery_mode) { // Standard mode: return failure immediately
            return ParserResult<std::unique_ptr<BlockStmt>>::failure(stmt.error_ref());
         }

         // DIAGNOSE mode: skip to next synchronisation point and continue
         Token error_token = this->ctx.tokens().current();
         [[maybe_unused]] size_t skipped = this->skip_to_synchronisation_point(terminators);

         #if 0 // Quite noisy, needs a control mechanism
         if (skipped > 0) {
            // Emit informational diagnostic about skipped tokens
            ParserDiagnostic info;
            info.severity = ParserDiagnosticSeverity::Info;
            info.code = ParserErrorCode::RecoverySkippedTokens;
            info.message = std::format("skipped {} token{} during error recovery", skipped, skipped IS 1 ? "" : "s");
            info.token = error_token;
            this->ctx.diagnostics().report(info);
         }
         #endif

         // If we've hit end of block or EOF, stop trying
         if (this->at_end_of_block(terminators)) break;

         // Continue parsing from synchronisation point
         continue;
      }

      if (stmt.value_ref()) {
         #ifdef INCLUDE_TIPS
         // Check for unreachable code: if we've already seen a terminating statement, this code is unreachable
         if (terminating_stmt) {
            const char *terminator_name = nullptr;
            switch (terminating_stmt->kind) {
               case AstNodeKind::ReturnStmt:   terminator_name = "return"; break;
               case AstNodeKind::BreakStmt:    terminator_name = "break"; break;
               case AstNodeKind::ContinueStmt: terminator_name = "continue"; break;
               default: break;
            }
            if (terminator_name) {
               this->ctx.emit_tip(1, TipCategory::TypeSafety,
                  std::format("Unreachable code after '{}' statement", terminator_name),
                  stmt_start);
            }
         }
         // Track if this statement terminates control flow
         else if (is_terminating_statement(stmt.value_ref().get())) {
            terminating_stmt = stmt.value_ref().get();
         }
         #endif

         statements.push_back(std::move(stmt.value_ref()));
      }
   }

   Token last = this->ctx.tokens().current();
   return ParserResult<std::unique_ptr<BlockStmt>>::success(make_block(last.span(), std::move(statements)));
}

//********************************************************************************************************************
// Check if an identifier is followed by a <const> or <close> attribute.  Due to lexer lookahead buffer complexities,
// we access the lexer's buffered_tokens directly when the special '<identifier' handling has been triggered.
//
// Patterns: `name <attr>`, `name:type <attr>`
// Returns true if this looks like an implicit local declaration with an attribute.

static bool is_implicit_local_with_attribute(TokenStreamAdapter& Tokens)
{
   // Current token must be an identifier (the variable name)
   if (Tokens.current().kind() != TokenKind::Identifier) return false;

   // The lexer has special handling for '<identifier': when it sees '<' followed immediately
   // by an identifier, it buffers the identifier via push_front and returns '<'.
   // This means when we peek, the buffered identifier appears BEFORE '<' in the peek order.
   //
   // For "b <const> = 10":
   // - Current: b
   // - peek(1): const (buffered via push_front by '<identifier' handling)
   // - peek(2): <
   // - peek(3): >
   //
   // We need to detect: identifier (current) followed by 'const'/'close' then '<' then '>'

   size_t pos = 1;
   Token next = Tokens.peek(pos);

   // Handle optional type annotation before the attribute (:type <const>)
   if (next.kind() IS TokenKind::Colon) {
      pos++;
      next = Tokens.peek(pos);
      // Type name must be an identifier or reserved type keyword
      if (next.kind() != TokenKind::Identifier and next.kind() != TokenKind::Function and next.kind() != TokenKind::Nil) {
         return false;
      }
      pos++;
      next = Tokens.peek(pos);
   }

   // Next should be 'const' or 'close' (the buffered identifier from '<identifier' handling)
   if (next.kind() != TokenKind::Identifier) return false;

   GCstr* attr_name = next.identifier();
   if (!attr_name) return false;

   std::string_view attr_str(strdata(attr_name), attr_name->len);
   if (attr_str != "const" and attr_str != "close") return false;

   // After the attribute name, we should see '<' (which was returned by the lexer)
   Token angle_open = Tokens.peek(pos + 1);
   if (angle_open.raw() != '<') return false;

   // After '<', we should see '>'
   Token angle_close = Tokens.peek(pos + 2);
   return angle_close.raw() IS '>';
}

//********************************************************************************************************************
// Statement dispatch - routes to the appropriate parser based on token type.

ParserResult<StmtNodePtr> AstBuilder::parse_statement()
{
   Token current = this->ctx.tokens().current();

   switch (current.kind()) {
      case TokenKind::Annotate:      return this->parse_annotated_statement();
      case TokenKind::Local:         return this->parse_local();
      case TokenKind::Global:        return this->parse_global();
      case TokenKind::Function:      return this->parse_function_stmt();
      case TokenKind::ThunkToken:    return this->parse_function_stmt();
      case TokenKind::If:            return this->parse_if();
      case TokenKind::WhileToken:    return this->parse_while();
      case TokenKind::Repeat:        return this->parse_repeat();
      case TokenKind::For:           return this->parse_for();
      case TokenKind::DoToken:       return this->parse_do();
      case TokenKind::WithToken:     return this->parse_with();
      case TokenKind::DeferToken:    return this->parse_defer();
      case TokenKind::ReturnToken:   return this->parse_return();
      case TokenKind::TryToken:      return this->parse_try();
      case TokenKind::RaiseToken:    return this->parse_raise();
      case TokenKind::CheckToken:    return this->parse_check();
      case TokenKind::ImportToken:   return this->parse_import();
      case TokenKind::NamespaceToken: return this->parse_namespace();
      case TokenKind::CompileIf:     return this->parse_compile_if();
      case TokenKind::Choose: {
         auto expr = this->parse_choose_expr();
         if (not expr.ok()) return ParserResult<StmtNodePtr>::failure(expr.error_ref());
         return ParserResult<StmtNodePtr>::success(make_expression_stmt(current.span(), std::move(expr.value_ref())));
      }
      case TokenKind::BreakToken:    return make_control_stmt(this->ctx, AstNodeKind::BreakStmt, current);
      case TokenKind::ContinueToken: return make_control_stmt(this->ctx, AstNodeKind::ContinueStmt, current);
      case TokenKind::Semicolon:
         this->ctx.tokens().advance();
         return ParserResult<StmtNodePtr>::success(nullptr);

      case TokenKind::Identifier: {
         // Check for implicit local declaration with <const> or <close> attribute
         if (is_implicit_local_with_attribute(this->ctx.tokens())) {
            return this->parse_local();
         }
         return this->parse_expression_stmt();
      }

      default:
         return this->parse_expression_stmt();
   }
}

//********************************************************************************************************************
// Parses a scoped block with a specified set of terminator tokens, automatically adding end-of-file as a terminator.

ParserResult<std::unique_ptr<BlockStmt>> AstBuilder::parse_scoped_block(std::initializer_list<TokenKind> terminators)
{
   std::vector<TokenKind> merged(terminators);
   merged.push_back(TokenKind::EndOfFile);
   return this->parse_block(merged);
}

// Checks if the current token indicates the end of a block by matching against terminator tokens.

bool AstBuilder::at_end_of_block(std::span<const TokenKind> terminators) const
{
   TokenKind kind = this->ctx.tokens().current().kind();
   if (kind IS TokenKind::EndOfFile) return true;
   for (TokenKind term : terminators) {
      if (kind IS term) return true;
   }
   return false;
}

// Checks if a token kind can begin a statement.

bool AstBuilder::is_statement_start(TokenKind kind) const
{
   switch (kind) {
      case TokenKind::Local:
      case TokenKind::Global:
      case TokenKind::Function:
      case TokenKind::ThunkToken:
      case TokenKind::Annotate:
      case TokenKind::If:
      case TokenKind::WhileToken:
      case TokenKind::Repeat:
      case TokenKind::For:
      case TokenKind::DoToken:
      case TokenKind::WithToken:
      case TokenKind::DeferToken:
      case TokenKind::ReturnToken:
      case TokenKind::BreakToken:
      case TokenKind::ContinueToken:
      case TokenKind::Choose:
      case TokenKind::TryToken:
      case TokenKind::RaiseToken:
      case TokenKind::CheckToken:
      case TokenKind::ImportToken:
      case TokenKind::NamespaceToken:
      case TokenKind::CompileIf:
         return true;
      default:
         return false;
   }
}

//********************************************************************************************************************
// Checks if the current token is a valid synchronisation point for error recovery.
// A synchronisation point is either a token that can start a new statement, a block terminator, or end of file.

bool AstBuilder::is_synchronisation_point(std::span<const TokenKind> terminators) const
{
   TokenKind kind = this->ctx.tokens().current().kind();

   if (kind IS TokenKind::EndOfFile) return true;   // End of file is always a synchronisation point
   if (this->is_statement_start(kind)) return true; // Check if this is a statement start
   if (kind IS TokenKind::Identifier) return true;  // Identifier can start an expression statement

   for (TokenKind term : terminators) { // Check block terminators
      if (kind IS term) return true;
   }

   return false;
}

//********************************************************************************************************************
// Skips tokens until reaching a synchronisation point (statement start or block terminator).
// Returns the number of tokens skipped. This implements "panic mode" error recovery.

size_t AstBuilder::skip_to_synchronisation_point(std::span<const TokenKind> terminators)
{
   size_t skipped = 0;

   while (not this->is_synchronisation_point(terminators)) {
      this->ctx.tokens().advance();
      if (++skipped > 1000) break;  // Safety limit to prevent infinite loops
   }

   return skipped;
}

// Creates an identifier structure from a token, extracting its symbol and source span.

Identifier AstBuilder::make_identifier(const Token &Token)
{
   Identifier id;
   id.symbol   = Token.identifier();
   id.span     = Token.span();
   // Check if the identifier is a blank placeholder (single underscore)
   id.is_blank = id.symbol and id.symbol->len IS 1 and strdata(id.symbol)[0] IS '_';
   return id;
}

// Creates a literal value structure from a token, extracting the appropriate value based on token type.

LiteralValue AstBuilder::make_literal(const Token &token)
{
   switch (token.kind()) {
      case TokenKind::Number:     return LiteralValue::number(token.payload().as_number());
      case TokenKind::String:     return LiteralValue::string(token.payload().as_string());
      case TokenKind::Nil:        return LiteralValue::nil();
      case TokenKind::TrueToken:  return LiteralValue::boolean(true);
      case TokenKind::FalseToken: return LiteralValue::boolean(false);
      default: return LiteralValue::nil();
   }
}

//********************************************************************************************************************
// Maps a TokenKind to its corresponding AssignmentOperator.

std::optional<AssignmentOperator> AstBuilder::token_to_assignment_op(TokenKind Kind)
{
   switch (Kind) {
      case TokenKind::Equals:           return AssignmentOperator::Plain;
      case TokenKind::CompoundAdd:      return AssignmentOperator::Add;
      case TokenKind::CompoundSub:      return AssignmentOperator::Subtract;
      case TokenKind::CompoundMul:      return AssignmentOperator::Multiply;
      case TokenKind::CompoundDiv:      return AssignmentOperator::Divide;
      case TokenKind::CompoundMod:      return AssignmentOperator::Modulo;
      case TokenKind::CompoundConcat:   return AssignmentOperator::Concat;
      case TokenKind::CompoundIfEmpty:  return AssignmentOperator::IfEmpty;
      case TokenKind::CompoundIfNil:    return AssignmentOperator::IfNil;
      default:                          return std::nullopt;
   }
}

//********************************************************************************************************************

#include "statements.cpp"
#include "loops.cpp"
#include "expressions.cpp"
#include "literals.cpp"
#include "choose.cpp"
#include "annotations.cpp"
