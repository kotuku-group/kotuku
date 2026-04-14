// AST Builder - Statement Parsers
// Copyright © 2025-2026 Paul Manias
//
// This file contains parsers for statement constructs:
// - Local/global variable declarations
// - Function declarations
// - Control flow statements (if, while, repeat, do)
// - Defer statements
// - Return statements
// - Expression statements

//********************************************************************************************************************
// Parses local variable declarations, local function statements and local thunk function statements.
// Supports both explicit 'local' keyword and implicit local declarations with <const>/<close> attributes.

ParserResult<StmtNodePtr> AstBuilder::parse_local()
{
   Token local_token = this->ctx.tokens().current();
   bool implicit_local = (local_token.kind() IS TokenKind::Identifier);

   if (not implicit_local) {
      this->ctx.tokens().advance();  // Consume the 'local' keyword

      bool is_thunk = false;
      if (this->ctx.check(TokenKind::ThunkToken)) {
         is_thunk = true;
         this->ctx.tokens().advance();
      }

      if (this->ctx.check(TokenKind::Function) or is_thunk) {
         if (not is_thunk) {
            this->ctx.tokens().advance();
         }
         Token function_token = local_token;  // Use local_token as span start
         auto name_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
         if (not name_token.ok()) return ParserResult<StmtNodePtr>::failure(name_token.error_ref());
         auto fn = this->parse_function_literal(function_token, is_thunk);
         if (not fn.ok()) return ParserResult<StmtNodePtr>::failure(fn.error_ref());
         ExprNodePtr function_expr = std::move(fn.value_ref());
         auto stmt = std::make_unique<StmtNode>(AstNodeKind::LocalFunctionStmt, this->span_from(local_token, name_token.value_ref()));
         LocalFunctionStmtPayload payload(make_identifier(name_token.value_ref()), move_function_payload(function_expr));
         stmt->data = std::move(payload);
         return ParserResult<StmtNodePtr>::success(std::move(stmt));
      }
   }

   auto names = this->parse_name_list();
   if (not names.ok()) return ParserResult<StmtNodePtr>::failure(names.error_ref());

   ExprNodeList values;
   AssignmentOperator assign_op = AssignmentOperator::Plain;

   // Check for plain = or conditional ?=/??= assignment
   if (this->ctx.match(TokenKind::Equals).ok()) {
      auto rhs = this->parse_expression_list();
      if (not rhs.ok()) return ParserResult<StmtNodePtr>::failure(rhs.error_ref());
      values = std::move(rhs.value_ref());
   }
   else if (this->ctx.match(TokenKind::CompoundIfEmpty).ok()) {
      assign_op = AssignmentOperator::IfEmpty;
      auto rhs = this->parse_expression_list();
      if (not rhs.ok()) return ParserResult<StmtNodePtr>::failure(rhs.error_ref());
      values = std::move(rhs.value_ref());
   }
   else if (this->ctx.match(TokenKind::CompoundIfNil).ok()) {
      assign_op = AssignmentOperator::IfNil;
      auto rhs = this->parse_expression_list();
      if (not rhs.ok()) return ParserResult<StmtNodePtr>::failure(rhs.error_ref());
      values = std::move(rhs.value_ref());
   }

   // Check if any trailing expressions beyond the name count are bare identifiers,
   // and if so, convert them to additional variable names.

   auto& name_list = names.value_ref();
   size_t name_count = name_list.size();

   if (values.size() > name_count) {
      for (size_t i = name_count; i < values.size(); ++i) {
         ExprNodePtr& expr = values[i];
         if (expr and expr->kind IS AstNodeKind::IdentifierExpr) {
            auto* name_ref = std::get_if<NameRef>(&expr->data);
            if (name_ref) { // Convert this identifier expression to a variable name
               name_list.push_back(name_ref->identifier);
            }
         }
         else {
            // Non-identifier expression in trailing position - this is an error
            return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedIdentifier, this->ctx.tokens().current(),
               "expected identifier after values in local declaration");
         }
      }
      // Remove the converted identifiers from the values list
      values.resize(name_count);
   }

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::LocalDeclStmt, local_token.span());
   stmt->data.emplace<LocalDeclStmtPayload>(assign_op, std::move(name_list), std::move(values));
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses global variable declarations, forcing variables to be stored in the global table.

ParserResult<StmtNodePtr> AstBuilder::parse_global()
{
   Token global_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();

   // Handle `global function name()` and `global thunk name()` syntax

   bool is_thunk = false;
   if (this->ctx.check(TokenKind::ThunkToken)) {
      is_thunk = true;
      this->ctx.tokens().advance();
   }

   if (this->ctx.check(TokenKind::Function) or is_thunk) {
      if (not is_thunk) this->ctx.tokens().advance();

      Token function_token = global_token;
      auto name_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not name_token.ok()) return ParserResult<StmtNodePtr>::failure(name_token.error_ref());
      auto fn = this->parse_function_literal(function_token, is_thunk);
      if (not fn.ok()) return ParserResult<StmtNodePtr>::failure(fn.error_ref());
      ExprNodePtr function_expr = std::move(fn.value_ref());

      // Build a FunctionStmt with a simple name path (will store to global)

      auto stmt = std::make_unique<StmtNode>(AstNodeKind::FunctionStmt, this->span_from(global_token, name_token.value_ref()));
      FunctionNamePath name;
      name.segments.push_back(make_identifier(name_token.value_ref()));
      name.is_explicit_global = true;  // Mark as explicitly global
      FunctionStmtPayload payload(std::move(name), move_function_payload(function_expr));
      stmt->data = std::move(payload);
      return ParserResult<StmtNodePtr>::success(std::move(stmt));
   }

   auto names = this->parse_name_list();
   if (not names.ok()) return ParserResult<StmtNodePtr>::failure(names.error_ref());

   ExprNodeList values;
   AssignmentOperator assign_op = AssignmentOperator::Plain;

   // Check for plain = or conditional ?=/??= assignment
   if (this->ctx.match(TokenKind::Equals).ok()) {
      auto rhs = this->parse_expression_list();
      if (not rhs.ok()) return ParserResult<StmtNodePtr>::failure(rhs.error_ref());
      values = std::move(rhs.value_ref());
   }
   else if (this->ctx.match(TokenKind::CompoundIfEmpty).ok()) {
      assign_op = AssignmentOperator::IfEmpty;
      auto rhs = this->parse_expression_list();
      if (not rhs.ok()) return ParserResult<StmtNodePtr>::failure(rhs.error_ref());
      values = std::move(rhs.value_ref());
   }
   else if (this->ctx.match(TokenKind::CompoundIfNil).ok()) {
      assign_op = AssignmentOperator::IfNil;
      auto rhs = this->parse_expression_list();
      if (not rhs.ok()) return ParserResult<StmtNodePtr>::failure(rhs.error_ref());
      values = std::move(rhs.value_ref());
   }

   // Check if any trailing expressions beyond the name count are bare identifiers,
   // and if so, convert them to additional variable names.

   auto &name_list = names.value_ref();
   size_t name_count = name_list.size();

   if (values.size() > name_count) {
      for (size_t i = name_count; i < values.size(); ++i) {
         ExprNodePtr& expr = values[i];
         if (expr and expr->kind IS AstNodeKind::IdentifierExpr) {
            if (auto *name_ref = std::get_if<NameRef>(&expr->data)) {
               // Convert this identifier expression to a variable name
               name_list.push_back(name_ref->identifier);
            }
         }
         else {
            // Non-identifier expression in trailing position - this is an error
            return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedIdentifier, this->ctx.tokens().current(),
               "expected identifier after values in global declaration");
         }
      }
      // Remove the converted identifiers from the values list
      values.resize(name_count);
   }

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::GlobalDeclStmt, global_token.span());
   GlobalDeclStmtPayload payload(assign_op, std::move(name_list), std::move(values));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses function declarations, including method definitions with colon syntax and thunk functions.

ParserResult<StmtNodePtr> AstBuilder::parse_function_stmt()
{
   Token func_token = this->ctx.tokens().current();
   bool is_thunk = (func_token.kind() IS TokenKind::ThunkToken);
   this->ctx.tokens().advance();
   FunctionNamePath path;
   auto name_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
   if (not name_token.ok()) return ParserResult<StmtNodePtr>::failure(name_token.error_ref());
   path.segments.push_back(make_identifier(name_token.value_ref()));

   bool method = false;
   while (this->ctx.match(TokenKind::Dot).ok()) {
      auto seg = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not seg.ok()) return ParserResult<StmtNodePtr>::failure(seg.error_ref());
      path.segments.push_back(make_identifier(seg.value_ref()));
   }

   if (this->ctx.match(TokenKind::Colon).ok()) {
      if (is_thunk) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "thunk functions do not support method syntax");
      }
      method = true;
      auto seg = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not seg.ok()) {
         return ParserResult<StmtNodePtr>::failure(seg.error_ref());
      }
      path.method = make_identifier(seg.value_ref());
   }

   auto fn = this->parse_function_literal(func_token, is_thunk);
   if (not fn.ok()) return ParserResult<StmtNodePtr>::failure(fn.error_ref());
   ExprNodePtr function_expr = std::move(fn.value_ref());

   if (method and path.method.has_value()) {
      auto* payload = function_payload_from(*function_expr);
      FunctionParameter self_param;
      self_param.name = Identifier(&this->ctx.lua(), "self", path.method.value().span);
      self_param.is_self = true;
      if (payload) payload->parameters.insert(payload->parameters.begin(), self_param);
   }

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::FunctionStmt, this->span_from(func_token, name_token.value_ref()));
   FunctionStmtPayload payload(std::move(path), move_function_payload(function_expr));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses if-then-else conditional statements with support for elseif chains.

ParserResult<StmtNodePtr> AstBuilder::parse_if()
{
   Token if_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();
   std::vector<IfClause> clauses;
   auto condition = this->parse_expression();
   if (not condition.ok()) return ParserResult<StmtNodePtr>::failure(condition.error_ref());

   this->ctx.consume(TokenKind::ThenToken, ParserErrorCode::ExpectedToken);
   auto then_block = this->parse_scoped_block({ TokenKind::ElseIf, TokenKind::Else, TokenKind::EndToken });
   if (not then_block.ok()) return ParserResult<StmtNodePtr>::failure(then_block.error_ref());

   IfClause clause;
   clause.condition = std::move(condition.value_ref());
   clause.block = std::move(then_block.value_ref());
   clauses.push_back(std::move(clause));

   while (this->ctx.check(TokenKind::ElseIf)) {
      this->ctx.tokens().advance();
      auto cond = this->parse_expression();
      if (not cond.ok()) return ParserResult<StmtNodePtr>::failure(cond.error_ref());
      this->ctx.consume(TokenKind::ThenToken, ParserErrorCode::ExpectedToken);
      auto block = this->parse_scoped_block({ TokenKind::ElseIf, TokenKind::Else, TokenKind::EndToken });
      if (not block.ok()) return ParserResult<StmtNodePtr>::failure(block.error_ref());
      IfClause elseif_clause;
      elseif_clause.condition = std::move(cond.value_ref());
      elseif_clause.block = std::move(block.value_ref());
      clauses.push_back(std::move(elseif_clause));
   }

   if (this->ctx.match(TokenKind::Else).ok()) {
      auto else_block = this->parse_scoped_block({ TokenKind::EndToken });
      if (not else_block.ok()) {
         return ParserResult<StmtNodePtr>::failure(else_block.error_ref());
      }
      IfClause else_clause;
      else_clause.condition = nullptr;
      else_clause.block = std::move(else_block.value_ref());
      clauses.push_back(std::move(else_clause));
   }

   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::IfStmt, if_token.span());
   IfStmtPayload payload(std::move(clauses));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses while-do loop statements.

ParserResult<StmtNodePtr> AstBuilder::parse_while()
{
   Token token = this->ctx.tokens().current();
   this->ctx.tokens().advance();
   auto condition = this->parse_expression();
   if (not condition.ok()) return ParserResult<StmtNodePtr>::failure(condition.error_ref());
   this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);
   auto body = this->parse_scoped_block({ TokenKind::EndToken });
   if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::WhileStmt, token.span());
   LoopStmtPayload payload(LoopStyle::WhileLoop, std::move(condition.value_ref()), std::move(body.value_ref()));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses repeat-until loop statements.

ParserResult<StmtNodePtr> AstBuilder::parse_repeat()
{
   Token token = this->ctx.tokens().current();
   this->ctx.tokens().advance();
   const TokenKind terms[] = { TokenKind::Until };
   auto body = this->parse_block(terms);
   if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
   this->ctx.consume(TokenKind::Until, ParserErrorCode::ExpectedToken);
   auto condition = this->parse_expression();
   if (not condition.ok()) return ParserResult<StmtNodePtr>::failure(condition.error_ref());

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::RepeatStmt, token.span());
   LoopStmtPayload payload(LoopStyle::RepeatUntil, std::move(condition.value_ref()), std::move(body.value_ref()));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses do-end block statements that create a new scope.

ParserResult<StmtNodePtr> AstBuilder::parse_do()
{
   Token token = this->ctx.tokens().current();
   this->ctx.tokens().advance();
   const TokenKind terms[] = { TokenKind::EndToken };
   auto block = this->parse_block(terms);
   if (not block.ok()) return ParserResult<StmtNodePtr>::failure(block.error_ref());

   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::DoStmt, token.span());
   DoStmtPayload payload(std::move(block.value_ref()));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses with statements: with expr1, expr2 do ... end
// Auto-locks Kotuku objects for the duration of the block, unlocking when scope exits.

ParserResult<StmtNodePtr> AstBuilder::parse_with()
{
   Token token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // consume 'with'

   auto exprs = this->parse_expression_list();
   if (not exprs.ok()) return ParserResult<StmtNodePtr>::failure(exprs.error_ref());

   this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);

   const TokenKind terms[] = { TokenKind::EndToken };
   auto body = this->parse_block(terms);
   if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());

   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::WithStmt, token.span());
   WithStmtPayload payload(std::move(exprs.value_ref()), std::move(body.value_ref()));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses defer statements that execute code when the current scope exits.

ParserResult<StmtNodePtr> AstBuilder::parse_defer()
{
   Token token = this->ctx.tokens().current();
   this->ctx.tokens().advance();
   bool has_params = this->ctx.check(TokenKind::LeftParen);
   ParameterListResult param_info;

   if (has_params) {
      auto parsed = this->parse_parameter_list(true);
      if (not parsed.ok()) return ParserResult<StmtNodePtr>::failure(parsed.error_ref());
      param_info = std::move(parsed.value_ref());
   }

   const TokenKind body_terms[] = { TokenKind::EndToken };
   auto body = this->parse_block(body_terms);
   if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   ExprNodeList args;
   if (this->ctx.match(TokenKind::LeftParen).ok()) {
      if (not this->ctx.check(TokenKind::RightParen)) {
         auto parsed_args = this->parse_expression_list();
         if (not parsed_args.ok()) return ParserResult<StmtNodePtr>::failure(parsed_args.error_ref());
         args = std::move(parsed_args.value_ref());
      }
      this->ctx.consume(TokenKind::RightParen, ParserErrorCode::ExpectedToken);
   }

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::DeferStmt, token.span());
   DeferStmtPayload payload(make_function_payload(std::move(param_info.parameters), param_info.is_vararg,
      std::move(body.value_ref())), std::move(args));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parse return payload shared by explicit returns and conditional shorthand returns.

ParserResult<ReturnStmtPayload> AstBuilder::parse_return_payload(const Token& return_token, bool same_line_only)
{
   ExprNodeList values;
   bool forwards_call = false;
   Token current = this->ctx.tokens().current();

   bool is_terminator = this->ctx.check(TokenKind::EndToken) or this->ctx.check(TokenKind::Else) or
      this->ctx.check(TokenKind::ElseIf) or this->ctx.check(TokenKind::Until) or
      this->ctx.check(TokenKind::EndOfFile) or this->ctx.check(TokenKind::Semicolon);

   bool same_line = same_line_only
      ? ((current.kind() IS TokenKind::EndOfFile) ? false : (current.span().line IS return_token.span().line))
      : true;

   bool parse_values = not is_terminator and (not same_line_only or same_line);

   if (parse_values) {
      auto exprs = this->parse_expression_list();
      if (not exprs.ok()) return ParserResult<ReturnStmtPayload>::failure(exprs.error_ref());
      if (exprs.value_ref().size() IS 1 and exprs.value_ref()[0]->kind IS AstNodeKind::CallExpr) {
         forwards_call = true;
      }
      values = std::move(exprs.value_ref());
   }

   if (this->ctx.match(TokenKind::Semicolon).ok()) {
      // Optional separator consumed.
   }

   ReturnStmtPayload payload(std::move(values), forwards_call);
   return ParserResult<ReturnStmtPayload>::success(std::move(payload));
}

//********************************************************************************************************************
// Parses return statements with optional return values.

ParserResult<StmtNodePtr> AstBuilder::parse_return()
{
   Token token = this->ctx.tokens().current();

   // Warn if this is a top-level return in an imported file, as it will affect
   // control flow in the importing script (since imports are inlined at parse time).
   if (this->ctx.is_being_imported() and this->at_top_level()) {
      this->ctx.emit_warning(ParserErrorCode::UnexpectedToken, token,
         "Top-level 'return' in imported file will return from the importing script's scope");
   }

   this->ctx.tokens().advance();
   auto payload = this->parse_return_payload(token, false);
   if (not payload.ok()) return ParserResult<StmtNodePtr>::failure(payload.error_ref());

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::ReturnStmt, token.span());
   stmt->data = std::move(payload.value_ref());
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses try...except...end exception handling blocks.
//
// Syntax:
//   try
//      <body>
//   except [e] [when { ERR_A, ERR_B } | when ERR_C]
//      <handler>
//   [except ...]
//   end

ParserResult<StmtNodePtr> AstBuilder::parse_try()
{
   Token try_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // consume 'try'

   // Parse optional <trace> attribute
   bool enable_trace = false;
   if (this->ctx.tokens().current().raw() IS '<') {
      this->ctx.tokens().advance();  // consume '<'
      auto attribute = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not attribute.ok()) return ParserResult<StmtNodePtr>::failure(attribute.error_ref());

      if (GCstr *attr_name = attribute.value_ref().identifier()) {
         std::string_view view(strdata(attr_name), attr_name->len);
         if (view IS std::string_view("trace")) {
            enable_trace = true;
         }
         else {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken,
               attribute.value_ref(), "unknown try attribute, expected 'trace'");
         }
      }

      if (not this->ctx.lex_opt('>')) {
         return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken,
            this->ctx.tokens().current(), "expected '>' after try attribute");
      }
   }

   // Parse try block body - terminates on 'except', 'success', or 'end'
   const TokenKind try_terms[] = { TokenKind::ExceptToken, TokenKind::SuccessToken, TokenKind::EndToken };
   auto try_body = this->parse_block(try_terms);
   if (not try_body.ok()) return ParserResult<StmtNodePtr>::failure(try_body.error_ref());

   std::vector<ExceptClause> clauses;
   bool has_catch_all = false;

   // Parse except clauses
   while (this->ctx.check(TokenKind::ExceptToken)) {
      if (has_catch_all) {
         // Error: catch-all must be last
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "catch-all 'except' must be the last clause");
      }

      Token except_token = this->ctx.tokens().current();
      this->ctx.tokens().advance();  // consume 'except'

      ExceptClause clause;
      clause.span = this->ctx.tokens().current().span();

      // Check for optional exception variable
      // Patterns: `except e when ...`, `except e`, `except when ...`, `except`
      // The exception variable must be on the same line as 'except'
      if (this->ctx.check(TokenKind::Identifier)) {
         Token name_token = this->ctx.tokens().current();
         // Only treat as exception variable if on same line as 'except'
         if (name_token.span().line IS except_token.span().line) {
            this->ctx.tokens().advance();
            clause.exception_var = make_identifier(name_token);
         }
      }

      // Optional when clause for filtering
      // Check for unexpected tokens on the same line after 'except [var]' (e.g., 'where' instead of 'when')
      if (this->ctx.check(TokenKind::Identifier)) {
         Token unexpected = this->ctx.tokens().current();
         if (unexpected.span().line IS except_token.span().line) {
            GCstr *ident = unexpected.identifier();
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, unexpected,
               "expected 'when' or newline after 'except', not '" + std::string(strdata(ident), ident->len) + "'");
         }
      }

      if (this->ctx.check(TokenKind::When)) {
         Token when_token = this->ctx.tokens().current();
         this->ctx.tokens().advance();  // consume 'when'

         // Filter code(s) must be on the same line as 'when'
         Token next_token = this->ctx.tokens().current();
         if (next_token.span().line != when_token.span().line) {
            return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, when_token,
               "expected error code(s) after 'when' on the same line");
         }

         // Parse error code filter(s): when ERR_A or when ERR_A, ERR_B
         auto first_code = this->parse_expression();
         if (not first_code.ok()) return ParserResult<StmtNodePtr>::failure(first_code.error_ref());
         clause.filter_codes.push_back(std::move(first_code.value_ref()));

         // Continue parsing comma-separated codes on the same line as 'when'
         while (this->ctx.check(TokenKind::Comma)) {
            Token comma_token = this->ctx.tokens().current();
            if (comma_token.span().line != when_token.span().line) break;
            this->ctx.tokens().advance();  // consume ','

            Token code_token = this->ctx.tokens().current();
            if (code_token.span().line != when_token.span().line) {
               return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, comma_token,
                  "expected error code after ',' on the same line as 'when'");
            }

            auto next_code = this->parse_expression();
            if (not next_code.ok()) return ParserResult<StmtNodePtr>::failure(next_code.error_ref());
            clause.filter_codes.push_back(std::move(next_code.value_ref()));
         }
      }
      else {
         has_catch_all = true;  // No 'when' = catch-all
      }

      // Parse except block body - terminates on next 'except', 'success', or 'end'
      const TokenKind except_terms[] = { TokenKind::ExceptToken, TokenKind::SuccessToken, TokenKind::EndToken };
      auto except_body = this->parse_block(except_terms);
      if (not except_body.ok()) return ParserResult<StmtNodePtr>::failure(except_body.error_ref());
      clause.block = std::move(except_body.value_ref());

      clauses.push_back(std::move(clause));
   }

   // Parse optional success clause
   std::unique_ptr<BlockStmt> success_block;
   if (this->ctx.check(TokenKind::SuccessToken)) {
      this->ctx.tokens().advance();  // consume 'success'

      // Parse success block body - terminates on 'end'
      const TokenKind success_terms[] = { TokenKind::EndToken };
      auto success_body = this->parse_block(success_terms);
      if (not success_body.ok()) return ParserResult<StmtNodePtr>::failure(success_body.error_ref());
      success_block = std::move(success_body.value_ref());
   }

   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   // Build the statement
   auto stmt = std::make_unique<StmtNode>(AstNodeKind::TryExceptStmt, try_token.span());
   TryExceptPayload payload;
   payload.try_block = std::move(try_body.value_ref());
   payload.except_clauses = std::move(clauses);
   payload.success_block = std::move(success_block);
   payload.enable_trace = enable_trace;
   stmt->data = std::move(payload);

   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses raise statements: raise expression [, message]
//
// The raise keyword always triggers an exception with the given error code.

ParserResult<StmtNodePtr> AstBuilder::parse_raise()
{
   Token raise_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // consume 'raise'

   // Parse error code expression (required)
   auto error_code = this->parse_expression();
   if (not error_code.ok()) return ParserResult<StmtNodePtr>::failure(error_code.error_ref());

   RaiseStmtPayload payload;
   payload.error_code = std::move(error_code.value_ref());

   // Check for optional message
   if (this->ctx.check(TokenKind::Comma)) {
      this->ctx.tokens().advance();  // consume ','
      auto message = this->parse_expression();
      if (not message.ok()) return ParserResult<StmtNodePtr>::failure(message.error_ref());
      payload.message = std::move(message.value_ref());
   }

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::RaiseStmt, raise_token.span());
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses check statements: check expression
//
// The check keyword raises an exception only if the error code >= ERR::ExceptionThreshold.

ParserResult<StmtNodePtr> AstBuilder::parse_check()
{
   Token check_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // consume 'check'

   // Parse error code expression (required)
   auto error_code = this->parse_expression();
   if (not error_code.ok()) return ParserResult<StmtNodePtr>::failure(error_code.error_ref());

   CheckStmtPayload payload;
   payload.error_code = std::move(error_code.value_ref());

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::CheckStmt, check_token.span());
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses import statements: import 'library' [as alias]
//
// The import statement is a compile-time feature that reads and parses the referenced file, inlining its content as
// statements executed within the current scope.
//
// When using 'as alias' syntax, the imported library must declare a namespace. The alias creates a local const variable
// that references _LIB['namespace'] for convenient access to the library exports.

ParserResult<StmtNodePtr> AstBuilder::parse_import()
{
   pf::Log log(__FUNCTION__);

   Token import_token = this->ctx.tokens().current();

   // Import statements must be at the top level of the script

   if (not this->at_top_level()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, import_token, "Use of 'import' is not permitted inside function blocks");
   }

   this->ctx.tokens().advance();  // consume 'import'

   // Require a string literal for the library path

   Token path_token = this->ctx.tokens().current();
   if (not path_token.is(TokenKind::String)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, path_token, "Import path must be a string literal");
   }

   GCstr *path_str = path_token.payload().as_string();
   if (not path_str) return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, path_token, "Invalid import path");

   std::string_view mod_name(strdata(path_str), path_str->len);
   this->ctx.tokens().advance();  // consume string

   log.traceBranch("Library: %.*s", int(mod_name.size()), mod_name.data());

   // Check for 'as' alias syntax

   std::optional<Identifier> alias;
   Token as_token;
   if (this->ctx.check(TokenKind::AsToken)) {
      as_token = this->ctx.tokens().current();
      this->ctx.tokens().advance();  // consume 'as'

      auto alias_result = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not alias_result.ok()) return ParserResult<StmtNodePtr>::failure(alias_result.error_ref());

      alias = make_identifier(alias_result.value_ref());
      alias->has_const = true;  // Namespace alias is const
   }

   std::string path = this->ctx.resolve_lib_to_path(mod_name);

   // Check for circular import

   if (this->ctx.is_importing(path)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, import_token, "Circular import detected: " + path);
   }

   // Parse the imported file

   auto imported_body = this->parse_imported_file(path, mod_name, import_token);
   if (not imported_body.ok()) return ParserResult<StmtNodePtr>::failure(imported_body.error_ref());

   // Look up the FileSource index and namespace for this import (registered during parse_imported_file)

   lua_State *L = &this->ctx.lua();
   auto file_idx = find_file_source(L, pf::strihash(path));
   std::string default_ns;

   if (file_idx.has_value()) {
      const FileSource *source = get_file_source(L, file_idx.value());
      if (source) default_ns = source->declared_namespace;
   }

   // If using 'as' alias, the library must declare a namespace

   if (alias and default_ns.empty()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, as_token,
         std::string("cannot use 'as' alias: library '") + std::string(mod_name) + "' does not declare a namespace");
   }

   // Determine final namespace name (alias takes precedence)

   std::string final_ns;
   if (alias) final_ns = std::string(strdata(alias->symbol), alias->symbol->len);
   else if (not default_ns.empty()) final_ns = default_ns;

   // Create ImportStmtPayload

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::ImportStmt, import_token.span());
   ImportStmtPayload payload;
   payload.lib_path = path;
   payload.inlined_body = std::move(imported_body.value_ref());

   if (file_idx.has_value()) {
      payload.file_source_idx = file_idx.value();
   }

   // If we have a namespace (either from alias or default), set up the namespace binding
   if (not final_ns.empty()) {
      Identifier ns_id;
      ns_id.symbol = lj_str_new(L, final_ns.c_str(), final_ns.size());
      ns_id.span = import_token.span();
      ns_id.has_const = true;
      payload.namespace_name = std::move(ns_id);
      payload.default_namespace = default_ns;  // Store original for _LIB lookup
   }

   stmt->data = std::move(payload);

   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses namespace statements: namespace 'name'
//
// The namespace statement declares a default namespace for a library. When this library is imported, the
// importing file can reference the library exports via `_LIB['name']`. This statement generates:
//   local _NS <const> = 'name'
//
// The namespace is stored in the current file's FileSource entry for lookup by the importing statement.

ParserResult<StmtNodePtr> AstBuilder::parse_namespace()
{
   pf::Log log(__FUNCTION__);

   Token ns_token = this->ctx.tokens().current();

   // Namespace must be at top level
   if (not this->at_top_level()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, ns_token, "'namespace' must be at library level");
   }

   this->ctx.tokens().advance();  // consume 'namespace'

   // Require string literal for namespace name
   Token name_token = this->ctx.tokens().current();
   if (not name_token.is(TokenKind::String)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, name_token,
         "namespace name must be a string literal");
   }

   GCstr *name_str = name_token.payload().as_string();
   std::string_view ns_name(strdata(name_str), name_str->len);
   this->ctx.tokens().advance();  // consume string

   log.detail("Namespace: %.*s", int(ns_name.size()), ns_name.data());

   lua_State *L = &this->ctx.lua();
   uint8_t current_file_index = this->ctx.lex().current_file_index;

   // Namespace conflicts are permitted - common namespaces like gui have many interlinking parts.
   auto existing_file = find_file_source_by_namespace(L, std::string(ns_name));
   if (existing_file.has_value() and existing_file.value() != current_file_index) {
      log.detail("Note: namespace '%.*s' already defined by another library", int(ns_name.size()), ns_name.data());
   }

   // Record the namespace in the current file's FileSource entry
   set_file_source_namespace(L, current_file_index, std::string(ns_name));

   // Transform to: local _NS <const> = 'name'
   Identifier id;
   id.symbol = lj_str_new(L, "_NS", 3);
   id.span = ns_token.span();
   id.has_const = true;

   std::vector<Identifier> names;
   names.push_back(std::move(id));

   ExprNodeList values;
   auto str_expr = std::make_unique<ExprNode>(AstNodeKind::LiteralExpr, name_token.span());
   LiteralValue lit;
   lit.kind = LiteralKind::String;
   lit.string_value = name_str;
   str_expr->data = lit;
   values.push_back(std::move(str_expr));

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::LocalDeclStmt, ns_token.span());
   stmt->data.emplace<LocalDeclStmtPayload>(AssignmentOperator::Plain,
      std::move(names), std::move(values));

   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Reads a file and parses its contents, returning the parsed block.  This is used by parse_import() to inline
// imported libraries at compile time.
//
// Each imported file is registered with a unique FileSource index for accurate error reporting.
// The file index is encoded in the upper 8 bits of BCLine values.

ParserResult<std::unique_ptr<BlockStmt>> AstBuilder::parse_imported_file(std::string &Path, std::string_view Library, const Token &ImportToken)
{
   pf::Log log(__FUNCTION__);

   lua_State *L = &this->ctx.lua();

   std::string resolved_path;
   if (ResolvePath(Path, RSF::NO_FILE_CHECK, &resolved_path) IS ERR::Okay) {
      Path = resolved_path;
   }

   auto libhash = pf::strihash(Path);

   // Check if this file is already registered in FileSource
   auto existing_index = find_file_source(L, libhash);
   if (existing_index.has_value()) {
      log.detail("Library %.*s already imported (file index %d)", int(Library.size()), Library.data(), existing_index.value());
      return ParserResult<std::unique_ptr<BlockStmt>>::success(make_block(ImportToken.span(), {}));
   }
   else log.branch("Importing '%.*s' from %s", int(Library.size()), Library.data(), Path.c_str());

   // Push this file onto the import stack to detect circular imports
   this->ctx.push_import(Path);

   // Read the file contents using Kotuku File API
   objFile::create file = { fl::Path(Path.c_str()), fl::Flags(FL::READ) };
   if (not file.ok()) {
      this->ctx.pop_import();
      return this->fail<std::unique_ptr<BlockStmt>>(ParserErrorCode::UnexpectedToken, ImportToken,
         "Cannot open imported file: " + Path);
   }

   // Get file size and read contents
   int file_size = 0;
   if (file->get(FID_Size, file_size) != ERR::Okay or file_size <= 0) {
      this->ctx.pop_import();
      // Empty file - return an empty block
      return ParserResult<std::unique_ptr<BlockStmt>>::success(make_block(ImportToken.span(), {}));
   }

   std::string source;
   source.resize(size_t(file_size));
   int bytes_read = 0;
   ERR err = file->read(source.data(), file_size, &bytes_read);

   if (err != ERR::Okay or bytes_read <= 0) {
      this->ctx.pop_import();
      return this->fail<std::unique_ptr<BlockStmt>>(ParserErrorCode::UnexpectedToken, ImportToken, "Cannot read imported file: " + Path);
   }
   source.resize(size_t(bytes_read));

   // Count source lines for FileSource metadata
   BCLine source_lines = 1;
   for (char c : source) {
      if (c IS '\n') source_lines++;
   }

   // Extract filename from path for display

   std::string filename = Path;
   auto pos = Path.find_last_of("/\\:");
   if (pos != std::string::npos) filename = Path.substr(pos + 1);

   // Get the parent file's index and the line where import occurred

   uint8_t parent_index = this->ctx.lex().current_file_index;
   BCLine import_line = ImportToken.span().line.lineNumber();  // Decode line from parent's encoded BCLine

   // Register this imported file with FileSource tracking

   uint8_t new_file_index = register_file_source(L, Path, filename, 1, source_lines, parent_index, import_line);

   // RAII guard handles cleanup on normal path; lua_load handles SEH error path
   ImportLexerGuard import_guard(L, source, std::string("@") + Path);
   LexState *import_lex = import_guard.get();

   import_lex->current_file_index = new_file_index; // Set the file index for this imported file
   import_lex->diagnose_mode = this->ctx.lex().diagnose_mode;  // Propagate diagnose mode from parent

   // Set chunk_name for error reporting (normally done in lj_parse for the main file)
   import_lex->chunk_name = lj_str_newz(L, import_lex->chunk_arg);

   // Point the FuncState to the new lexer temporarily
   FuncState &fs = this->ctx.func();
   LexState *saved_ls = fs.ls;
   fs.ls = import_lex;

   // Initialize the import lexer
   import_lex->fs = &fs;
   import_lex->L = L;

   import_lex->next(); // Prime the lexer

   // Create a temporary parser context for the imported file
   ParserContext import_ctx(*import_lex, fs, *L, ParserAllocator::from(L), this->ctx.config());

   // Copy the import stack to the child context for circular detection
   for (const auto& imported_path : this->ctx.import_stack()) {
      import_ctx.push_import(imported_path);
   }

   // Parse up to EOF
   AstBuilder import_builder(import_ctx);
   const TokenKind terms[] = { TokenKind::EndOfFile };
   auto result = import_builder.parse_block(terms);

   fs.ls = saved_ls; // Restore the parent FuncState's lexer reference
   // import_guard destructor handles cleanup

   this->ctx.pop_import();

   if (not result.ok()) {
      // Prepend import context to error message
      ParserError error = result.error_ref();
      error.message = "in imported file '" + Path + "': " + error.message;
      return ParserResult<std::unique_ptr<BlockStmt>>::failure(error);
   }

   return result;
}

//********************************************************************************************************************
// Skips tokens until a matching @end is found, handling nested @if/@end blocks.  Called when the @if condition
// evaluates to false.

void AstBuilder::skip_to_compile_end()
{
   pf::Log log(__FUNCTION__);

   int depth = 1;  // Already consumed one @if

   while (depth > 0) {
      Token current = this->ctx.tokens().current();

      if (current.is(TokenKind::EndOfFile)) {
         // Unclosed @if - emit error and return
         this->ctx.emit_error(ParserErrorCode::UnexpectedToken, current, "Unclosed @if - expected @end");
         return;
      }

      if (current.is(TokenKind::CompileIf)) {
         depth++;
         log.detail("Found nested @if, depth now %d", depth);
      }
      else if (current.is(TokenKind::CompileEnd)) {
         depth--;
         log.detail("Found @end, depth now %d", depth);
      }

      this->ctx.tokens().advance();
   }
}

//********************************************************************************************************************
// Parses compile-time conditional: @if(condition) ... @end
//
// Supported conditions:
//   @if(imported=true)     - Include block only when file is being imported
//   @if(imported=false)    - Include block only when file is the main script
//   @if(debug=true)        - Include block only when log level > 0 (debugging enabled)
//   @if(debug=false)       - Include block only when log level == 0 (no debugging)
//   @if(platform="name")   - Include block only when platform matches (windows, linux, osx, native)
//   @if(exists="path")     - Include block only if file/folder exists (relative to script)
//
// When condition is true, parses the block normally.
// When condition is false, skips tokens until @end without parsing.

ParserResult<StmtNodePtr> AstBuilder::parse_compile_if()
{
   pf::Log log(__FUNCTION__);

   Token compif_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // consume @if

   // Expect '('
   if (not this->ctx.tokens().current().is(TokenKind::LeftParen)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, this->ctx.tokens().current(),
         "Expected '(' after @if");
   }
   this->ctx.tokens().advance();  // consume '('

   // Parse condition: identifier '=' value
   Token ident_token = this->ctx.tokens().current();
   if (not ident_token.is(TokenKind::Identifier)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, ident_token, "Expected identifier in @if condition");
   }

   GCstr *ident_str = ident_token.payload().as_string();
   std::string_view condition_name(strdata(ident_str), ident_str->len);
   this->ctx.tokens().advance();  // consume identifier

   // Expect '='
   if (not this->ctx.tokens().current().is(TokenKind::Equals)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, this->ctx.tokens().current(), "Expected '=' in @if condition");
   }
   this->ctx.tokens().advance();  // consume '='

   // Parse the condition value - can be true, false, or a string
   Token value_token = this->ctx.tokens().current();
   bool is_bool_value = false;
   bool bool_value = false;
   std::string_view string_value;

   if (value_token.is(TokenKind::TrueToken)) {
      is_bool_value = true;
      bool_value = true;
   }
   else if (value_token.is(TokenKind::FalseToken)) {
      is_bool_value = true;
      bool_value = false;
   }
   else if (value_token.is(TokenKind::String)) {
      is_bool_value = false;
      GCstr *str = value_token.payload().as_string();
      if (str) string_value = std::string_view(strdata(str), str->len);
   }
   else return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, value_token, "Expected 'true', 'false', or a string literal in @if condition");

   this->ctx.tokens().advance();  // consume value

   // Expect ')'

   if (not this->ctx.tokens().current().is(TokenKind::RightParen)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, this->ctx.tokens().current(), "Expected ')' after @if condition");
   }
   this->ctx.tokens().advance();  // consume ')'

   // Evaluate condition

   bool condition_result = false;

   if (condition_name IS "imported") {
      if (not is_bool_value) return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, value_token, "Condition 'imported' requires a boolean value");
      bool is_imported = this->ctx.is_being_imported();
      condition_result = is_imported IS bool_value;
   }
   else if (condition_name IS "debug") {
      if (not is_bool_value) return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, value_token, "Condition 'debug' requires a boolean value");
      condition_result = (GetResource(RES::LOG_LEVEL) > 2) IS bool_value;
   }
   else if (condition_name IS "platform") {
      if (is_bool_value) return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, value_token, "Condition 'platform' requires a string value");
      const SystemState *state = GetSystemState();
      std::string_view current_platform = state->Platform ? state->Platform : "";
      condition_result = pf::iequals(current_platform, string_value);
   }
   else if (condition_name IS "exists") {
      if (is_bool_value) return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, value_token, "Condition 'exists' requires a string path value");

      // Resolve the path relative to the current script
      std::string check_path;
      if (this->ctx.lex().chunk_arg) {
         std::string current_file(this->ctx.lex().chunk_arg);
         if (not current_file.empty() and (current_file[0] IS '@' or current_file[0] IS '=')) {
            current_file = current_file.substr(1);
         }
         size_t last_sep = current_file.find_last_of("/\\");
         if (last_sep != std::string::npos) {
            check_path = current_file.substr(0, last_sep + 1) + std::string(string_value);
         }
         else {
            auto working_path = this->ctx.lua().script->get<CSTRING>(FID_WorkingPath);
            if (working_path) check_path = std::string(working_path) + std::string(string_value);
            else check_path = std::string(string_value);
         }
      }
      else {
         auto working_path = this->ctx.lua().script->get<CSTRING>(FID_WorkingPath);
         if (working_path) check_path = std::string(working_path) + std::string(string_value);
         else check_path = std::string(string_value);
      }

      condition_result = (AnalysePath(check_path.c_str(), nullptr) IS ERR::Okay);
   }
   else return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, ident_token, "Unknown @if condition: " + std::string(condition_name));

   if (condition_result) { // Condition is true - parse statements until @end
      log.detail("@if condition true, parsing block");

      std::vector<StmtNodePtr> statements;

      while (not this->ctx.tokens().current().is(TokenKind::CompileEnd) and
             not this->ctx.tokens().current().is(TokenKind::EndOfFile)) {
         auto stmt = this->parse_statement();
         if (not stmt.ok()) return ParserResult<StmtNodePtr>::failure(stmt.error_ref());
         if (stmt.value_ref()) statements.push_back(std::move(stmt.value_ref()));
      }

      // Expect @end

      if (not this->ctx.tokens().current().is(TokenKind::CompileEnd)) {
         return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, this->ctx.tokens().current(),
            "Expected @end to close @if block");
      }
      this->ctx.tokens().advance();  // consume @end

      // Return a do block containing the statements (transparent wrapper)

      auto block = make_block(compif_token.span(), std::move(statements));
      auto stmt = std::make_unique<StmtNode>(AstNodeKind::DoStmt, compif_token.span());
      stmt->data = DoStmtPayload(std::move(block));
      return ParserResult<StmtNodePtr>::success(std::move(stmt));
   }
   else {
      log.detail("@if condition false, skipping to @end");
      this->skip_to_compile_end();
      return ParserResult<StmtNodePtr>::success(nullptr);
   }
}
