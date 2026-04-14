// AST Builder - Choose Expression Parser
// Copyright © 2025-2026 Paul Manias
//
// This file contains the parser for choose expressions (pattern matching):
// - Single value scrutinee: choose expr from pattern -> result ... end
// - Tuple scrutinee: choose (expr1, expr2) from (pattern1, pattern2) -> result ... end
// - Relational patterns: < <= > >=
// - Wildcard patterns: _
// - Table patterns: { key = value }
// - Guard clauses: when condition

//********************************************************************************************************************
// Parses a choose expression: choose scrutinee from pattern -> result ... end
// Also supports tuple scrutinee: choose (expr1, expr2, ...) from (pattern1, pattern2, ...) -> result ... end

ParserResult<ExprNodePtr> AstBuilder::parse_choose_expr()
{
   Token choose_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // consume 'choose'

   // Parse scrutinee - check for tuple scrutinee: (expr, expr, ...)
   ExprNodeList scrutinee_tuple;
   ExprNodePtr single_scrutinee;
   size_t tuple_arity = 0;

   if (this->ctx.check(TokenKind::LeftParen)) {
      Token paren_token = this->ctx.tokens().current();
      this->ctx.tokens().advance();  // consume '('

      // Parse first expression
      auto first_expr = this->parse_expression();
      if (not first_expr.ok()) return ParserResult<ExprNodePtr>::failure(first_expr.error_ref());

      if (this->ctx.check(TokenKind::Comma)) {
         // This is a tuple scrutinee
         scrutinee_tuple.push_back(std::move(first_expr.value_ref()));

         while (this->ctx.match(TokenKind::Comma).ok()) {
            auto next_expr = this->parse_expression();
            if (not next_expr.ok()) return ParserResult<ExprNodePtr>::failure(next_expr.error_ref());
            scrutinee_tuple.push_back(std::move(next_expr.value_ref()));
         }

         auto close_paren = this->ctx.consume(TokenKind::RightParen, ParserErrorCode::ExpectedToken);
         if (not close_paren.ok()) return ParserResult<ExprNodePtr>::failure(close_paren.error_ref());

         tuple_arity = scrutinee_tuple.size();
      }
      else {
         // Single parenthesised expression: choose (expr) from
         auto close_paren = this->ctx.consume(TokenKind::RightParen, ParserErrorCode::ExpectedToken);
         if (not close_paren.ok()) return ParserResult<ExprNodePtr>::failure(close_paren.error_ref());
         single_scrutinee = std::move(first_expr.value_ref());
      }
   }
   else {
      // Non-parenthesised single expression
      auto scrutinee = this->parse_expression();
      if (not scrutinee.ok()) return ParserResult<ExprNodePtr>::failure(scrutinee.error_ref());
      single_scrutinee = std::move(scrutinee.value_ref());
   }

   // Expect 'from' keyword
   auto from_match = this->ctx.consume(TokenKind::From, ParserErrorCode::ExpectedToken);
   if (not from_match.ok()) return ParserResult<ExprNodePtr>::failure(from_match.error_ref());

   std::vector<ChooseCase> cases;

   // Set flag to indicate we're parsing choose expression cases (for tuple pattern lookahead)
   this->in_choose_expression = true;

   // Lookahead to detect tuple patterns when scrutinee is single expression
   // This enables `choose func() from (0, 0) -> ...` where func() returns 2 values
   size_t inferred_tuple_arity = 0;
   if (tuple_arity IS 0 and this->ctx.check(TokenKind::LeftParen)) {
      size_t pos = 1;
      int paren_depth = 1;
      size_t comma_count = 0;

      // Scan for commas at paren depth 1
      while (paren_depth > 0 and pos < 100) {
         Token ahead = this->ctx.tokens().peek(pos);
         if (ahead.kind() IS TokenKind::LeftParen) paren_depth++;
         else if (ahead.kind() IS TokenKind::RightParen) paren_depth--;
         else if (ahead.kind() IS TokenKind::Comma and paren_depth IS 1) comma_count++;
         pos++;
      }

      // Check if followed by -> or 'when' (indicating this is a pattern, not a call)
      if (paren_depth IS 0) {
         Token after_paren = this->ctx.tokens().peek(pos);
         if ((after_paren.kind() IS TokenKind::CaseArrow or after_paren.kind() IS TokenKind::When)
             and comma_count > 0) {
            // This is a tuple pattern! Infer arity
            inferred_tuple_arity = comma_count + 1;
            tuple_arity = inferred_tuple_arity;  // Enable tuple pattern parsing
         }
      }
   }

   bool seen_else = false;  // Track if else branch has been parsed

   // Parse cases until 'end'
   while (not this->ctx.check(TokenKind::EndToken) and not this->ctx.check(TokenKind::EndOfFile)) {
      // Validate else is last - no cases allowed after else
      if (seen_else) {
         this->in_choose_expression = false;  // Clean up flag before returning
         return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "'else' must be the last case in choose expression");
      }

      ChooseCase case_arm;
      case_arm.span = this->ctx.tokens().current().span();

      if (this->ctx.check(TokenKind::Else)) {
         this->ctx.tokens().advance();  // consume 'else'
         case_arm.is_else = true;
         case_arm.pattern = nullptr;
         seen_else = true;  // Mark that else has been seen
      }
      else {
         // Check for tuple pattern (p1, p2, ...) - only valid when scrutinee is a tuple
         Token current = this->ctx.tokens().current();
         if (tuple_arity > 0 and this->ctx.check(TokenKind::LeftParen)) {
            case_arm.is_tuple_pattern = true;
            this->ctx.tokens().advance();  // consume '('

            // Parse tuple pattern elements
            while (true) {
               Token elem_token = this->ctx.tokens().current();

               // Check for wildcard in tuple position: _ followed by , or )
               if (elem_token.is_identifier()) {
                  GCstr* name = elem_token.identifier();
                  if (name->len IS 1 and strdata(name)[0] IS '_') {
                     Token next = this->ctx.tokens().peek(1);
                     if (next.kind() IS TokenKind::Comma or next.kind() IS TokenKind::RightParen) {
                        this->ctx.tokens().advance();  // consume '_'
                        case_arm.tuple_wildcards.push_back(true);
                        case_arm.tuple_patterns.push_back(nullptr);  // Placeholder for wildcard
                     }
                     else {
                        // Parse as expression
                        auto elem = this->parse_expression();
                        if (not elem.ok()) return ParserResult<ExprNodePtr>::failure(elem.error_ref());
                        case_arm.tuple_wildcards.push_back(false);
                        case_arm.tuple_patterns.push_back(std::move(elem.value_ref()));
                     }
                  }
                  else {
                     auto elem = this->parse_expression();
                     if (not elem.ok()) return ParserResult<ExprNodePtr>::failure(elem.error_ref());
                     case_arm.tuple_wildcards.push_back(false);
                     case_arm.tuple_patterns.push_back(std::move(elem.value_ref()));
                  }
               }
               else {
                  auto elem = this->parse_expression();
                  if (not elem.ok()) return ParserResult<ExprNodePtr>::failure(elem.error_ref());
                  case_arm.tuple_wildcards.push_back(false);
                  case_arm.tuple_patterns.push_back(std::move(elem.value_ref()));
               }

               if (not this->ctx.match(TokenKind::Comma).ok()) break;
            }

            auto close_paren = this->ctx.consume(TokenKind::RightParen, ParserErrorCode::ExpectedToken);
            if (not close_paren.ok()) {
               this->in_choose_expression = false;
               return ParserResult<ExprNodePtr>::failure(close_paren.error_ref());
            }

            // Arity validation - compile error on mismatch
            if (case_arm.tuple_patterns.size() != tuple_arity) {
               this->in_choose_expression = false;
               return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, current,
                  "tuple pattern has " + std::to_string(case_arm.tuple_patterns.size()) +
                  " elements but scrutinee has " + std::to_string(tuple_arity));
            }

            // Check if all wildcards (equivalent to bare _ wildcard)
            bool all_wildcards = true;
            for (bool wc : case_arm.tuple_wildcards) {
               if (not wc) { all_wildcards = false; break; }
            }

            if (all_wildcards) case_arm.is_wildcard = true;
         }
         // Check for relational pattern operators (< <= > >=)
         else if (current.raw() IS '<') {
            this->ctx.tokens().advance();  // consume '<'
            if (this->ctx.check(TokenKind::Equals)) {
               this->ctx.tokens().advance();  // consume '=' (for <=)
               case_arm.relational_op = ChooseRelationalOp::LessEqual;
            }
            else case_arm.relational_op = ChooseRelationalOp::LessThan;

            // Parse the comparison value expression

            auto pattern = this->parse_expression();
            if (not pattern.ok()) {
               this->in_choose_expression = false;
               return ParserResult<ExprNodePtr>::failure(pattern.error_ref());
            }
            case_arm.pattern = std::move(pattern.value_ref());
         }
         else if (current.raw() IS '>') {
            this->ctx.tokens().advance();  // consume '>'
            if (this->ctx.check(TokenKind::Equals)) {
               this->ctx.tokens().advance();  // consume '=' (for >=)
               case_arm.relational_op = ChooseRelationalOp::GreaterEqual;
            }
            else case_arm.relational_op = ChooseRelationalOp::GreaterThan;

            // Parse the comparison value expression

            auto pattern = this->parse_expression();
            if (not pattern.ok()) {
               this->in_choose_expression = false;
               return ParserResult<ExprNodePtr>::failure(pattern.error_ref());
            }
            case_arm.pattern = std::move(pattern.value_ref());
         }
         else if (this->ctx.check(TokenKind::LessEqual)) {
            this->ctx.tokens().advance();  // consume '<='
            case_arm.relational_op = ChooseRelationalOp::LessEqual;
            auto pattern = this->parse_expression();
            if (not pattern.ok()) {
               this->in_choose_expression = false;
               return ParserResult<ExprNodePtr>::failure(pattern.error_ref());
            }
            case_arm.pattern = std::move(pattern.value_ref());
         }
         else if (this->ctx.check(TokenKind::GreaterEqual)) {
            this->ctx.tokens().advance();  // consume '>='
            case_arm.relational_op = ChooseRelationalOp::GreaterEqual;
            auto pattern = this->parse_expression();
            if (not pattern.ok()) {
               this->in_choose_expression = false;
               return ParserResult<ExprNodePtr>::failure(pattern.error_ref());
            }
            case_arm.pattern = std::move(pattern.value_ref());
         }
         // Check for table pattern { key = value, ... }
         else if (this->ctx.check(TokenKind::LeftBrace)) {
            case_arm.is_table_pattern = true;
            auto pattern = this->parse_expression();  // Reuse existing table parsing
            if (not pattern.ok()) {
               this->in_choose_expression = false;
               return ParserResult<ExprNodePtr>::failure(pattern.error_ref());
            }
            case_arm.pattern = std::move(pattern.value_ref());
         }
         // Check for wildcard pattern '_'
         else if (current.is_identifier()) {
            GCstr* name = current.identifier();
            if (name->len IS 1 and strdata(name)[0] IS '_') {
               // Peek ahead to check if next token is '->' or 'when' (to confirm this is pattern position)
               Token next = this->ctx.tokens().peek(1);
               if (next.kind() IS TokenKind::CaseArrow or next.kind() IS TokenKind::When) {
                  this->ctx.tokens().advance();  // consume '_'
                  case_arm.is_wildcard = true;
                  case_arm.pattern = nullptr;
               }
               else { // Not a wildcard pattern, parse as normal expression
                  auto pattern = this->parse_expression();
                  if (not pattern.ok()) {
                     this->in_choose_expression = false;
                     return ParserResult<ExprNodePtr>::failure(pattern.error_ref());
                  }
                  case_arm.pattern = std::move(pattern.value_ref());
               }
            }
            else { // Parse pattern (only literal expressions)
               auto pattern = this->parse_expression();
               if (not pattern.ok()) {
                  this->in_choose_expression = false;
                  return ParserResult<ExprNodePtr>::failure(pattern.error_ref());
               }
               case_arm.pattern = std::move(pattern.value_ref());
            }
         }
         else { // Parse pattern (only literal expressions)
            auto pattern = this->parse_expression();
            if (not pattern.ok()) {
               this->in_choose_expression = false;
               return ParserResult<ExprNodePtr>::failure(pattern.error_ref());
            }
            case_arm.pattern = std::move(pattern.value_ref());
         }
      }

      // Check for optional 'when <condition>' guard clause
      if (this->ctx.check(TokenKind::When)) {
         this->ctx.tokens().advance();  // consume 'when'

         // Set flags to disable lookaheads during guard parsing
         this->in_guard_expression = true;
         this->in_choose_expression = false;  // Disable tuple pattern lookahead
         auto guard = this->parse_expression();
         this->in_guard_expression = false;
         this->in_choose_expression = true;   // Re-enable for next case

         if (not guard.ok()) return ParserResult<ExprNodePtr>::failure(guard.error_ref());
         case_arm.guard = std::move(guard.value_ref());
      }

      // Expect '->'
      auto arrow_match = this->ctx.consume(TokenKind::CaseArrow, ParserErrorCode::ExpectedToken);
      if (not arrow_match.ok()) {
         this->in_choose_expression = false;
         return ParserResult<ExprNodePtr>::failure(arrow_match.error_ref());
      }

      // Parse result - could be expression OR statement (assignment)
      // Detect assignment by parsing first expression and checking for assignment operator
      auto first_expr = this->parse_expression();
      if (not first_expr.ok()) {
         this->in_choose_expression = false;
         return ParserResult<ExprNodePtr>::failure(first_expr.error_ref());
      }

      // Check if this is an assignment statement
      Token maybe_assign = this->ctx.tokens().current();
      bool is_assignment = false;
      switch (maybe_assign.kind()) {
         case TokenKind::Equals:
         case TokenKind::CompoundAdd:
         case TokenKind::CompoundSub:
         case TokenKind::CompoundMul:
         case TokenKind::CompoundDiv:
         case TokenKind::CompoundMod:
         case TokenKind::CompoundConcat:
         case TokenKind::CompoundIfEmpty:
         case TokenKind::CompoundIfNil:
            is_assignment = true;
            break;
         case TokenKind::Comma:
            // Multi-target assignment: a, b = ...
            is_assignment = true;
            break;
         default:
            break;
      }

      if (is_assignment) {
         // Parse as statement - build assignment AST
         ExprNodeList targets;
         targets.push_back(std::move(first_expr.value_ref()));

         // Handle multi-target assignment: a, b, c = ...
         while (this->ctx.match(TokenKind::Comma).ok()) {
            auto extra = this->parse_expression();
            if (not extra.ok()) {
               this->in_choose_expression = false;
               return ParserResult<ExprNodePtr>::failure(extra.error_ref());
            }
            targets.push_back(std::move(extra.value_ref()));
         }

         Token op = this->ctx.tokens().current();
         auto assignment_op = token_to_assignment_op(op.kind()).value_or(AssignmentOperator::Plain);
         this->ctx.tokens().advance();  // consume assignment operator

         auto values = this->parse_expression_list();
         if (not values.ok()) {
            this->in_choose_expression = false;
            return ParserResult<ExprNodePtr>::failure(values.error_ref());
         }

         auto stmt = std::make_unique<StmtNode>(AstNodeKind::AssignmentStmt, op.span());
         AssignmentStmtPayload payload(assignment_op, std::move(targets), std::move(values.value_ref()));
         stmt->data = std::move(payload);

         case_arm.result_stmt = std::move(stmt);
         case_arm.has_statement_result = true;
      }
      else { // Parse as expression (original behaviour)
         case_arm.result = std::move(first_expr.value_ref());
      }

      cases.push_back(std::move(case_arm));
   }

   // Consume 'end'
   auto end_match = this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);
   if (not end_match.ok()) {
      this->in_choose_expression = false;
      return ParserResult<ExprNodePtr>::failure(end_match.error_ref());
   }

   // Reset flag - we're done parsing choose expression
   this->in_choose_expression = false;

   // Build choose expression - use tuple version if scrutinee is an explicit tuple
   if (tuple_arity > 0 and not scrutinee_tuple.empty()) {
      // Explicit tuple scrutinee: (a, b)
      return ParserResult<ExprNodePtr>::success(
         make_choose_expr_tuple(choose_token.span(), std::move(scrutinee_tuple), std::move(cases))
      );
   }
   else {
      // Single scrutinee (possibly with inferred tuple arity for function returns)
      return ParserResult<ExprNodePtr>::success(
         make_choose_expr(choose_token.span(), std::move(single_scrutinee), std::move(cases), inferred_tuple_arity)
      );
   }
}
