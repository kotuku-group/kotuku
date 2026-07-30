// AST Builder - Loop Parsers
// Copyright © 2025-2026 Paul Manias
//
// This file contains parsers for loop constructs:
// - Optimised numeric loop nodes lowered from range syntax
// - Generic for loops (for k, v in iterator)
// - Anonymous for loops (for {range} do)
// - Range literal optimisation for JIT compilation

//********************************************************************************************************************
// Parses a range expression already confirmed by scan_range_literal(): {expr to expr} or {expr into expr}, with an
// optional `by` step expression.

ParserResult<ExprNodePtr> AstBuilder::parse_scanned_range_in_braces(bool HasStep, bool HasBareStringOperand)
{
   if (HasBareStringOperand) {
      return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "range literals do not support bare string operands");
   }

   // Confirmed range pattern.  Consume '{' and parse the range.

   Token brace_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // Consume '{'

   // Parse start expression.  It naturally stops before contextual `to` or `into`, which are not binary operators.
   auto start_expr = this->parse_expression();
   if (not start_expr.ok()) return ParserResult<ExprNodePtr>::failure(start_expr.error_ref());

   // Consume the range separator
   Token separator = this->ctx.tokens().current();
   bool is_inclusive = false;
   if (not token_is_range_separator(separator, is_inclusive)) {
      return this->fail<ExprNodePtr>(ParserErrorCode::ExpectedToken, separator,
         "expected 'to' or 'into' range separator");
   }
   this->ctx.tokens().advance();  // Consume `to` or `into`

   // Parse stop expression (stops naturally at `by` or `}`)
   auto stop_expr = this->parse_expression();
   if (not stop_expr.ok()) return ParserResult<ExprNodePtr>::failure(stop_expr.error_ref());

   ExprNodePtr step_expr;
   if (HasStep) {
      Token by_token = this->ctx.tokens().current();
      if (not token_is_range_step_separator(by_token)) {
         return this->fail<ExprNodePtr>(ParserErrorCode::ExpectedToken, by_token, "expected 'by' range step separator");
      }
      this->ctx.tokens().advance();

      auto step = this->parse_expression();
      if (not step.ok()) return ParserResult<ExprNodePtr>::failure(step.error_ref());
      step_expr = std::move(step.value_ref());
   }

   auto close = this->ctx.consume(TokenKind::RightBrace, ParserErrorCode::ExpectedToken);
   if (not close.ok()) return ParserResult<ExprNodePtr>::failure(close.error_ref());

   SourceSpan span = combine_spans(brace_token.span(), close.value_ref().span());
   ExprNodePtr node = make_range_expr(span, std::move(start_expr.value_ref()),
      std::move(stop_expr.value_ref()), is_inclusive, std::move(step_expr));
   return ParserResult<ExprNodePtr>::success(std::move(node));
}

// Parses generic, range and anonymous for loops.  Lua-style numeric headers are rejected.

ParserResult<StmtNodePtr> AstBuilder::parse_for()
{
   Token token = this->ctx.tokens().current();
   this->ctx.tokens().advance();

   // Check for anonymous for loop: for {range} do
   // This allows iterating over a range without declaring a loop variable.
   if (this->ctx.check(TokenKind::LeftBrace)) {
      return this->parse_anonymous_for(token);
   }

   auto name_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
   if (not name_token.ok()) return ParserResult<StmtNodePtr>::failure(name_token.error_ref());

   if (this->ctx.check(TokenKind::Equals)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::DeprecatedSyntax, this->ctx.tokens().current(),
         "Lua-style numeric 'for' syntax is no longer supported; use an inclusive range such as "
         "'for i in {0 into 8} do'");
   }

   std::vector<Identifier> names;
   names.push_back(make_identifier(name_token.value_ref()));
   while (this->ctx.match(TokenKind::Comma).ok()) {
      auto extra = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not extra.ok()) return ParserResult<StmtNodePtr>::failure(extra.error_ref());
      names.push_back(make_identifier(extra.value_ref()));
   }

   this->ctx.consume(TokenKind::InToken, ParserErrorCode::ExpectedToken);

   // In for-in loops, always interpret {expr to expr} as a range expression, even when the operands
   // are complex expressions like {0 to total-1}.  This bypasses the restrictive lookahead in
   // parse_table_literal() which only handles simple operands.

   ExprNodeList iterator_nodes;
   if (this->ctx.check(TokenKind::LeftBrace)) {
      RangeLiteralScan scan;
      if (scan_range_literal(this->ctx, scan)) {
         auto range_result = this->parse_scanned_range_in_braces(scan.has_step, scan.has_bare_string_operand);
         if (not range_result.ok()) return ParserResult<StmtNodePtr>::failure(range_result.error_ref());
         iterator_nodes.push_back(std::move(range_result.value_ref()));
      }
      else {
         auto iterators = this->parse_expression_list();
         if (not iterators.ok()) return ParserResult<StmtNodePtr>::failure(iterators.error_ref());
         iterator_nodes = std::move(iterators.value_ref());
      }
   }
   else {
      auto iterators = this->parse_expression_list();
      if (not iterators.ok()) return ParserResult<StmtNodePtr>::failure(iterators.error_ref());
      iterator_nodes = std::move(iterators.value_ref());
   }

   // Direct range literals use ordinal-based range-loop bytecode.  Keeping constant and runtime ranges on the same
   // path avoids the legacy numeric loop's frame-state aliasing when a traced loop contains protected regions.
   if (names.size() IS 1 and iterator_nodes.size() IS 1 and
       iterator_nodes[0] and iterator_nodes[0]->kind IS AstNodeKind::RangeExpr) {
      auto *range_payload = std::get_if<RangeExprPayload>(&iterator_nodes[0]->data);
      if (range_payload and range_payload->start and range_payload->stop) {
         bool inclusive = range_payload->inclusive;
         ExprNodePtr start = std::move(range_payload->start);
         ExprNodePtr stop = std::move(range_payload->stop);
         ExprNodePtr step = std::move(range_payload->step);

         this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);
         auto body = this->parse_scoped_block({ TokenKind::EndToken });
         if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
         this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

         auto stmt = std::make_unique<StmtNode>(AstNodeKind::RangeForStmt, token.span());
         stmt->data.emplace<RangeForStmtPayload>(std::move(names[0]), std::move(start), std::move(stop),
            std::move(step), inclusive, std::move(body.value_ref()));
         return ParserResult<StmtNodePtr>::success(std::move(stmt));
      }
   }

   // Generic for loop path: wrap range literals in a call to get the iterator
   if (iterator_nodes.size() IS 1) {
      ExprNodePtr &first = iterator_nodes[0];
      if (first and first->kind IS AstNodeKind::RangeExpr) {
         SourceSpan span = first->span;
         ExprNodePtr callee = std::move(first);
         ExprNodeList args;
         bool forwards_multret = false;
         first = make_call_expr(span, std::move(callee), std::move(args), forwards_multret);
      }
   }

   this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);
   auto body = this->parse_scoped_block({ TokenKind::EndToken });
   if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::GenericForStmt, token.span());
   GenericForStmtPayload payload(std::move(names), std::move(iterator_nodes), std::move(body.value_ref()));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses anonymous for loops: for {range} do ... end
// This allows iterating over a range without declaring a loop variable, useful when the iteration
// count matters but the index value is not needed.
//
// Examples:
//    for {0 to 10} do print("hello") end     -- prints "hello" 10 times
//    for {1 into 5} do total += 1 end         -- increments total 5 times
//
// The implementation creates a blank identifier internally and leverages the existing for-loop
// machinery, including direct range-loop JIT optimisation.

ParserResult<StmtNodePtr> AstBuilder::parse_anonymous_for(const Token& ForToken)
{
   // Parse the iterator expression (expected to be a range like {0 to 10}).
   // Try the range scan first to support complex expressions like {0 to total-1}.

   ExprNodePtr iter_expr;
   if (this->ctx.check(TokenKind::LeftBrace)) {
      RangeLiteralScan scan;
      if (scan_range_literal(this->ctx, scan)) {
         auto range_result = this->parse_scanned_range_in_braces(scan.has_step, scan.has_bare_string_operand);
         if (not range_result.ok()) return ParserResult<StmtNodePtr>::failure(range_result.error_ref());
         iter_expr = std::move(range_result.value_ref());
      }
      else {
         auto iterator = this->parse_expression();
         if (not iterator.ok()) return ParserResult<StmtNodePtr>::failure(iterator.error_ref());
         iter_expr = std::move(iterator.value_ref());
      }
   }
   else {
      auto iterator = this->parse_expression();
      if (not iterator.ok()) return ParserResult<StmtNodePtr>::failure(iterator.error_ref());
      iter_expr = std::move(iterator.value_ref());
   }

   // Create a blank identifier for the anonymous loop variable
   Identifier blank_id;
   blank_id.symbol = nullptr;
   blank_id.is_blank = true;
   blank_id.span = ForToken.span();

   if (iter_expr and iter_expr->kind IS AstNodeKind::RangeExpr) {
      auto *range_payload = std::get_if<RangeExprPayload>(&iter_expr->data);
      if (range_payload and range_payload->start and range_payload->stop) {
         bool inclusive = range_payload->inclusive;
         ExprNodePtr start = std::move(range_payload->start);
         ExprNodePtr stop = std::move(range_payload->stop);
         ExprNodePtr step = std::move(range_payload->step);

         this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);
         auto body = this->parse_scoped_block({ TokenKind::EndToken });
         if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
         this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

         auto stmt = std::make_unique<StmtNode>(AstNodeKind::RangeForStmt, ForToken.span());
         stmt->data.emplace<RangeForStmtPayload>(std::move(blank_id), std::move(start), std::move(stop),
            std::move(step), inclusive, std::move(body.value_ref()));
         return ParserResult<StmtNodePtr>::success(std::move(stmt));
      }
   }

   // Generic for loop fallback: wrap range in a call to get the iterator
   if (iter_expr and iter_expr->kind IS AstNodeKind::RangeExpr) {
      SourceSpan span = iter_expr->span;
      ExprNodePtr callee = std::move(iter_expr);
      ExprNodeList args;
      iter_expr = make_call_expr(span, std::move(callee), std::move(args), false);
   }

   this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);
   auto body = this->parse_scoped_block({ TokenKind::EndToken });
   if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::GenericForStmt, ForToken.span());
   std::vector<Identifier> names;
   names.push_back(std::move(blank_id));

   ExprNodeList iterators;
   iterators.push_back(std::move(iter_expr));

   GenericForStmtPayload payload(std::move(names), std::move(iterators), std::move(body.value_ref()));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}
