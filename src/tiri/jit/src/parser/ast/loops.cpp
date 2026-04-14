// AST Builder - Loop Parsers
// Copyright © 2025-2026 Paul Manias
//
// This file contains parsers for loop constructs:
// - Numeric for loops (for i = start, stop, step)
// - Generic for loops (for k, v in iterator)
// - Anonymous for loops (for {range} do)
// - Range literal optimisation for JIT compilation

//********************************************************************************************************************
// Attempts to parse a range expression enclosed in braces: {expr..expr} or {expr...expr}
// Used by for-in loops to always interpret {Y..Z} as a range, even when Y and Z are complex expressions.
//
// Uses a lookahead scan to confirm that '..' or '...' exists at brace/paren/bracket depth 0 inside the braces.
// If confirmed, parses start and stop as full expressions and returns a RangeExpr node.
// If the scan does not find a range operator, returns failure so the caller can fall through to normal parsing.
//
// The start expression is parsed with precedence 5 so that it stops before consuming '..' (Cat token,
// left precedence 5).  The stop expression uses default precedence and naturally stops at '}'.

ParserResult<ExprNodePtr> AstBuilder::parse_range_in_braces()
{
   // Lookahead scan: starting from the token after '{', verify a strict
   // {start_expr .. stop_expr} or {start_expr ... stop_expr} shape at depth 0.
   // This avoids misclassifying table literals that merely contain '..'/'...'.

   bool found_range_operator = false;
   bool is_inclusive = false;
   bool has_start_expr_tokens = false;
   bool has_stop_expr_tokens = false;
   bool ended_on_right_brace = false;
   bool invalid_top_level_shape = false;
   int depth = 0;

   for (size_t i = 1; ; i++) {
      Token tok = this->ctx.tokens().peek(i);
      auto kind = tok.kind();

      if (kind IS TokenKind::EndOfFile) {
         break;
      }

      if (depth IS 0 and kind IS TokenKind::RightBrace) {
         ended_on_right_brace = true;
         break;
      }

      if (kind IS TokenKind::LeftParen or kind IS TokenKind::LeftBracket or kind IS TokenKind::LeftBrace) {
         if (depth IS 0) {
            if (found_range_operator) has_stop_expr_tokens = true;
            else has_start_expr_tokens = true;
         }
         depth++;
      }
      else if (kind IS TokenKind::RightParen or kind IS TokenKind::RightBracket) {
         depth--;
         if (depth < 0) {
            invalid_top_level_shape = true;
            break;
         }
      }
      else if (kind IS TokenKind::RightBrace) {
         depth--;
         if (depth < 0) {
            invalid_top_level_shape = true;
            break;
         }
      }
      else if (depth IS 0) {
         if (kind IS TokenKind::Comma) {
            invalid_top_level_shape = true;
            break;
         }

         if (kind IS TokenKind::Cat or kind IS TokenKind::Dots) {
            // Exactly one top-level range operator is allowed, and it must
            // appear after at least one token belonging to the start expression.
            if (found_range_operator or not has_start_expr_tokens) {
               invalid_top_level_shape = true;
               break;
            }

            found_range_operator = true;
            is_inclusive = (kind IS TokenKind::Dots);
         }
         else {
            if (found_range_operator) has_stop_expr_tokens = true;
            else has_start_expr_tokens = true;
         }
      }
   }

   if (invalid_top_level_shape or not ended_on_right_brace or not found_range_operator
      or not has_start_expr_tokens or not has_stop_expr_tokens) {
      return ParserResult<ExprNodePtr>::failure(
         ParserError(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(), "not a range expression"));
   }

   // Confirmed range pattern.  Consume '{' and parse the range.

   Token brace_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // Consume '{'

   // Parse start expression with precedence 5, which stops before '..' (Cat, left precedence 5)
   auto start_expr = this->parse_expression(5);
   if (not start_expr.ok()) return ParserResult<ExprNodePtr>::failure(start_expr.error_ref());

   // Consume the range operator
   Token range_tok = this->ctx.tokens().current();
   if (range_tok.kind() IS TokenKind::Cat) {
      is_inclusive = false;
   }
   else if (range_tok.kind() IS TokenKind::Dots) {
      is_inclusive = true;
   }
   else {
      return this->fail<ExprNodePtr>(ParserErrorCode::ExpectedToken, range_tok, "expected '..' or '...' range operator");
   }
   this->ctx.tokens().advance();  // Consume '..' or '...'

   // Parse stop expression (stops naturally at '}')
   auto stop_expr = this->parse_expression();
   if (not stop_expr.ok()) return ParserResult<ExprNodePtr>::failure(stop_expr.error_ref());

   this->ctx.consume(TokenKind::RightBrace, ParserErrorCode::ExpectedToken);

   ExprNodePtr node = make_range_expr(brace_token.span(), std::move(start_expr.value_ref()),
      std::move(stop_expr.value_ref()), is_inclusive);
   return ParserResult<ExprNodePtr>::success(std::move(node));
}

//********************************************************************************************************************
// Parses for loops, handling both numeric (for i=start,stop,step) and generic (for k,v in iterator) forms.

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

   if (this->ctx.match(TokenKind::Equals).ok()) {
      auto start = this->parse_expression();
      if (not start.ok()) return ParserResult<StmtNodePtr>::failure(start.error_ref());
      this->ctx.consume(TokenKind::Comma, ParserErrorCode::ExpectedToken);
      auto stop = this->parse_expression();
      if (not stop.ok()) return ParserResult<StmtNodePtr>::failure(stop.error_ref());
      ExprNodePtr step_expr;
      if (this->ctx.match(TokenKind::Comma).ok()) {
         auto step = this->parse_expression();
         if (not step.ok()) return ParserResult<StmtNodePtr>::failure(step.error_ref());
         step_expr = std::move(step.value_ref());
      }
      this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);
      auto body = this->parse_scoped_block({ TokenKind::EndToken });
      if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
      this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

      auto stmt = std::make_unique<StmtNode>(AstNodeKind::NumericForStmt, token.span());
      NumericForStmtPayload payload(make_identifier(name_token.value_ref()),
         std::move(start.value_ref()), std::move(stop.value_ref()), std::move(step_expr), std::move(body.value_ref()));
      stmt->data = std::move(payload);
      return ParserResult<StmtNodePtr>::success(std::move(stmt));
   }

   std::vector<Identifier> names;
   names.push_back(make_identifier(name_token.value_ref()));
   while (this->ctx.match(TokenKind::Comma).ok()) {
      auto extra = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not extra.ok()) return ParserResult<StmtNodePtr>::failure(extra.error_ref());
      names.push_back(make_identifier(extra.value_ref()));
   }

   this->ctx.consume(TokenKind::InToken, ParserErrorCode::ExpectedToken);

   // In for-in loops, always interpret {expr..expr} as a range expression, even when the operands
   // are complex expressions like {0..total-1}.  This bypasses the restrictive lookahead in
   // parse_table_literal() which only handles simple operands.

   ExprNodeList iterator_nodes;
   if (this->ctx.check(TokenKind::LeftBrace)) {
      auto range_result = this->parse_range_in_braces();
      if (range_result.ok()) {
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

   // JIT Optimisation: Convert range literals with a single loop variable to numeric for loops.
   // This allows the JIT to compile `for i in {1..10} do` into optimised BC_FORI/BC_FORL bytecode
   // instead of the slower generic iterator path (BC_ITERC/BC_ITERL).
   //
   // Conversion: for i in {start..stop} do  =>  for i = start, stop-1, step do  (exclusive)
   //             for i in {start...stop} do =>  for i = start, stop, step do    (inclusive)
   //
   // Step is computed at runtime based on start/stop direction.

   if (names.size() IS 1 and iterator_nodes.size() IS 1) {
      ExprNodePtr &first = iterator_nodes[0];
      if (first and first->kind IS AstNodeKind::RangeExpr) {
         auto* range_payload = std::get_if<RangeExprPayload>(&first->data);
         if (range_payload and range_payload->start and range_payload->stop) {
            SourceSpan span = first->span;

            // Move the start and stop expressions from the range
            ExprNodePtr start_expr = std::move(range_payload->start);
            ExprNodePtr stop_expr = std::move(range_payload->stop);
            bool inclusive = range_payload->inclusive;

            // Check if both start and stop are numeric literals for compile-time optimisation.
            // When both are constants, we can compute the step direction and exclusive adjustment
            // at compile time, producing optimal BC_FORI/BC_FORL bytecode.

            bool start_is_num = start_expr->kind IS AstNodeKind::LiteralExpr and
               std::get_if<LiteralValue>(&start_expr->data) and
               std::get<LiteralValue>(start_expr->data).kind IS LiteralKind::Number;

            bool stop_is_num = stop_expr->kind IS AstNodeKind::LiteralExpr and
               std::get_if<LiteralValue>(&stop_expr->data) and
               std::get<LiteralValue>(stop_expr->data).kind IS LiteralKind::Number;

            if (start_is_num and stop_is_num) {
               lua_Number start_val = std::get<LiteralValue>(start_expr->data).number_value;
               lua_Number stop_val = std::get<LiteralValue>(stop_expr->data).number_value;

               // Determine step direction based on start/stop values
               lua_Number step_val = (start_val <= stop_val) ? 1.0 : -1.0;

               // For exclusive ranges, adjust stop
               lua_Number final_stop = stop_val;
               if (not inclusive) {
                  final_stop = (step_val > 0) ? (stop_val - 1) : (stop_val + 1);
               }

               // Create literals for stop and step
               ExprNodePtr final_stop_expr = make_literal_expr(stop_expr->span, LiteralValue::number(final_stop));
               ExprNodePtr step_expr = make_literal_expr(span, LiteralValue::number(step_val));

               this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);
               auto body = this->parse_scoped_block({ TokenKind::EndToken });
               if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
               this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

               auto stmt = std::make_unique<StmtNode>(AstNodeKind::NumericForStmt, token.span());
               NumericForStmtPayload payload(std::move(names[0]), std::move(start_expr),
                  std::move(final_stop_expr), std::move(step_expr), std::move(body.value_ref()));
               stmt->data = std::move(payload);
               return ParserResult<StmtNodePtr>::success(std::move(stmt));
            }

            // Non-constant range: fall back to generic for loop with iterator
            // Restore the range expression since we moved from it
            range_payload->start = std::move(start_expr);
            range_payload->stop = std::move(stop_expr);
         }
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
//    for {0..10} do print("hello") end     -- prints "hello" 10 times
//    for {1...5} do total += 1 end         -- increments total 5 times
//
// The implementation creates a blank identifier internally and leverages the existing for-loop
// machinery, including JIT optimisation for constant ranges.

ParserResult<StmtNodePtr> AstBuilder::parse_anonymous_for(const Token& ForToken)
{
   // Parse the iterator expression (expected to be a range like {0..10}).
   // Try parse_range_in_braces() first to support complex expressions like {0..total-1}.

   ExprNodePtr iter_expr;
   if (this->ctx.check(TokenKind::LeftBrace)) {
      auto range_result = this->parse_range_in_braces();
      if (range_result.ok()) {
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

   // JIT Optimisation: Convert constant range literals to numeric for loops.
   // This allows the JIT to compile `for {1..10} do` into optimised BC_FORI/BC_FORL bytecode.
   if (iter_expr and iter_expr->kind IS AstNodeKind::RangeExpr) {
      auto* range_payload = std::get_if<RangeExprPayload>(&iter_expr->data);
      if (range_payload and range_payload->start and range_payload->stop) {
         SourceSpan span = iter_expr->span;

         // Check if both start and stop are numeric literals for compile-time optimisation.

         bool start_is_num = range_payload->start->kind IS AstNodeKind::LiteralExpr and
            std::get_if<LiteralValue>(&range_payload->start->data) and
            std::get<LiteralValue>(range_payload->start->data).kind IS LiteralKind::Number;

         bool stop_is_num = range_payload->stop->kind IS AstNodeKind::LiteralExpr and
            std::get_if<LiteralValue>(&range_payload->stop->data) and
            std::get<LiteralValue>(range_payload->stop->data).kind IS LiteralKind::Number;

         if (start_is_num and stop_is_num) {
            lua_Number start_val = std::get<LiteralValue>(range_payload->start->data).number_value;
            lua_Number stop_val = std::get<LiteralValue>(range_payload->stop->data).number_value;
            bool inclusive = range_payload->inclusive;

            // Determine step direction based on start/stop values
            lua_Number step_val = (start_val <= stop_val) ? 1.0 : -1.0;

            // For exclusive ranges, adjust stop
            lua_Number final_stop = stop_val;
            if (not inclusive) {
               final_stop = (step_val > 0) ? (stop_val - 1) : (stop_val + 1);
            }

            // Move the start expression from the range
            ExprNodePtr start_expr = std::move(range_payload->start);

            // Create literals for stop and step
            ExprNodePtr final_stop_expr = make_literal_expr(span, LiteralValue::number(final_stop));
            ExprNodePtr step_expr = make_literal_expr(span, LiteralValue::number(step_val));

            this->ctx.consume(TokenKind::DoToken, ParserErrorCode::ExpectedToken);
            auto body = this->parse_scoped_block({ TokenKind::EndToken });
            if (not body.ok()) return ParserResult<StmtNodePtr>::failure(body.error_ref());
            this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

            StmtNodePtr stmt = std::make_unique<StmtNode>(AstNodeKind::NumericForStmt, ForToken.span());
            stmt->data.emplace<NumericForStmtPayload>(std::move(blank_id), std::move(start_expr), std::move(final_stop_expr),
               std::move(step_expr), std::move(body.value_ref()));
            return ParserResult<StmtNodePtr>::success(std::move(stmt));
         }
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
