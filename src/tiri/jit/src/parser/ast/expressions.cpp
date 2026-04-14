// AST Builder - Expression Parsers
// Copyright © 2025-2026 Paul Manias
//
// This file contains parsers for expression constructs:
// - Expression statements (assignments, compound assignments, conditional shorthands)
// - Binary/unary operators with precedence climbing
// - Primary expressions (literals, identifiers, parenthesised)
// - Suffix operations (field access, indexing, method calls)
// - Arrow functions
// - Operator matching

//********************************************************************************************************************
// Parses expression statements, handling assignments, compound assignments, conditional shorthands, and standalone expressions.

ParserResult<StmtNodePtr> AstBuilder::parse_expression_stmt()
{
   auto first = this->parse_expression();
   if (not first.ok()) return ParserResult<StmtNodePtr>::failure(first.error_ref());

   ExprNodeList targets;
   targets.push_back(std::move(first.value_ref()));
   while (this->ctx.match(TokenKind::Comma).ok()) {
      auto extra = this->parse_expression();
      if (not extra.ok()) return ParserResult<StmtNodePtr>::failure(extra.error_ref());
      targets.push_back(std::move(extra.value_ref()));
   }

   Token op = this->ctx.tokens().current();
   auto assignment_result = token_to_assignment_op(op.kind());

   if (assignment_result.has_value()) {
      AssignmentOperator assignment = assignment_result.value();
      this->ctx.tokens().advance();
      auto values = this->parse_expression_list();
      if (not values.ok()) return ParserResult<StmtNodePtr>::failure(values.error_ref());
      auto stmt = std::make_unique<StmtNode>(AstNodeKind::AssignmentStmt, op.span());
      AssignmentStmtPayload payload(assignment, std::move(targets), std::move(values.value_ref()));
      stmt->data = std::move(payload);
      return ParserResult<StmtNodePtr>::success(std::move(stmt));
   }

   // Conditional shorthand pattern: value ?? return/break/continue/raise/check

   if (targets.size() IS 1 and is_presence_expr(targets[0])) {
      Token next = this->ctx.tokens().current();
      if (is_shorthand_statement_keyword(next.kind())) {
         auto* presence_payload = std::get_if<PresenceExprPayload>(&targets[0]->data);
         if (presence_payload and presence_payload->value) {
            ExprNodePtr condition = std::move(presence_payload->value);
            StmtNodePtr body;

            if (next.kind() IS TokenKind::ReturnToken) {
               Token return_token = next;
               this->ctx.tokens().advance();

               auto payload = this->parse_return_payload(return_token, true);
               if (not payload.ok()) return ParserResult<StmtNodePtr>::failure(payload.error_ref());

               auto node = std::make_unique<StmtNode>(AstNodeKind::ReturnStmt, return_token.span());
               node->data = std::move(payload.value_ref());
               body = std::move(node);
            }
            else if (next.kind() IS TokenKind::BreakToken) {
               auto control = make_control_stmt(this->ctx, AstNodeKind::BreakStmt, next);
               if (not control.ok()) return ParserResult<StmtNodePtr>::failure(control.error_ref());
               body = std::move(control.value_ref());
            }
            else if (next.kind() IS TokenKind::ContinueToken) {
               auto control = make_control_stmt(this->ctx, AstNodeKind::ContinueStmt, next);
               if (not control.ok()) return ParserResult<StmtNodePtr>::failure(control.error_ref());
               body = std::move(control.value_ref());
            }
            else if (next.kind() IS TokenKind::RaiseToken) {
               auto raise_stmt = this->parse_raise();
               if (not raise_stmt.ok()) return raise_stmt;
               body = std::move(raise_stmt.value_ref());
            }
            else if (next.kind() IS TokenKind::CheckToken) {
               auto check_stmt = this->parse_check();
               if (not check_stmt.ok()) return check_stmt;
               body = std::move(check_stmt.value_ref());
            }

            if (body) {
               SourceSpan span = combine_spans(condition->span, body->span);
               auto stmt = std::make_unique<StmtNode>(AstNodeKind::ConditionalShorthandStmt, span);
               ConditionalShorthandStmtPayload payload(std::move(condition), std::move(body));
               stmt->data = std::move(payload);
               return ParserResult<StmtNodePtr>::success(std::move(stmt));
            }
         }
      }
   }

   if (targets.size() > 1) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "unexpected expression list without assignment");
   }

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::ExpressionStmt, targets[0]->span);
   ExpressionStmtPayload payload(std::move(targets[0]));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses expressions using precedence climbing for binary operators, ternary conditionals, and pipe operators.

ParserResult<ExprNodePtr> AstBuilder::parse_expression(uint8_t precedence)
{
   auto left = this->parse_unary();
   if (not left.ok()) return left;

   while (true) {
      Token next = this->ctx.tokens().current();

      // Pipe operator: precedence 2, right-associative (left=2, right=1)
      // Higher than logical operators, lower than comparison

      if (next.kind() IS TokenKind::Pipe) {
         constexpr uint8_t pipe_left = 2;
         if (pipe_left <= precedence) break;

         // Extract limit from token payload (0 = unlimited)
         uint32_t limit = 0;
         if (next.payload().has_value()) {
            double limit_val = next.payload().as_number();
            if (limit_val >= 1) limit = uint32_t(limit_val);
         }

         this->ctx.tokens().advance();

         // Parse RHS as a primary expression with suffixes (to allow call expressions)

         auto rhs = this->parse_unary();
         if (not rhs.ok()) return rhs;

         // Apply suffixes to get the complete RHS expression

         rhs = this->parse_suffixed(std::move(rhs.value_ref()));
         if (not rhs.ok()) return rhs;

         // Check for pipe iteration pattern: range/array |> function
         // When LHS is a range or array literal and RHS is a function (not a call), rewrite to LHS:each(func)
         // Also support chaining: range:each(f1) |> f2 → range:each(f1):each(f2)

         bool lhs_is_range = left.value_ref()->kind IS AstNodeKind::RangeExpr;

         // Check if LHS is an array literal or a method call to :each() (for chaining support)
         bool lhs_is_array = false;
         bool lhs_is_each_call = false;
         if (left.value_ref()->kind IS AstNodeKind::CallExpr) {
            const CallExprPayload& call_data = std::get<CallExprPayload>(left.value_ref()->data);
            if (call_data.result_type IS TiriType::Array) {
               lhs_is_array = true;
            }
            else if (const auto* method = std::get_if<MethodCallTarget>(&call_data.target)) {
               if (method->method.symbol and strcmp(strdata(method->method.symbol), "each") IS 0) {
                  lhs_is_each_call = true;
               }
            }
         }

         bool rhs_is_function = rhs.value_ref()->kind IS AstNodeKind::FunctionExpr or
                                rhs.value_ref()->kind IS AstNodeKind::IdentifierExpr or
                                rhs.value_ref()->kind IS AstNodeKind::MemberExpr or
                                rhs.value_ref()->kind IS AstNodeKind::IndexExpr;
         bool rhs_is_call = rhs.value_ref()->kind IS AstNodeKind::CallExpr or
                            rhs.value_ref()->kind IS AstNodeKind::SafeCallExpr;

         if ((lhs_is_range or lhs_is_each_call or lhs_is_array) and rhs_is_function) {
            // Pipe iteration: transform range/array |> func into LHS:each(func)
            // For chaining: LHS:each(f1) |> f2 → LHS:each(f1):each(f2)
            SourceSpan span = combine_spans(left.value_ref()->span, rhs.value_ref()->span);

            Identifier method(&this->ctx.lua(), "each", next.span());

            ExprNodeList args;
            args.push_back(std::move(rhs.value_ref()));

            ExprNodePtr call = make_method_call_expr(span, std::move(left.value_ref()), method, std::move(args), false);
            left = ParserResult<ExprNodePtr>::success(std::move(call));
            continue;
         }

         // When RHS is a function reference (not a call) and LHS type is unknown at parse time,
         // create a deferred pipe iteration node.  The IR emitter will resolve the LHS type and
         // either emit :each() for arrays or raise an error.

         if (rhs_is_function) {
            SourceSpan span = combine_spans(left.value_ref()->span, rhs.value_ref()->span);
            auto pipe = make_pipe_expr(span, std::move(left.value_ref()), std::move(rhs.value_ref()), limit);
            std::get<PipeExprPayload>(pipe->data).deferred_iteration = true;
            left = ParserResult<ExprNodePtr>::success(std::move(pipe));
            continue;
         }

         // Validate that RHS is a call expression for normal pipes

         if (not rhs_is_call) {
            return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, next,
               "pipe operator requires function call on right-hand side");
         }

         SourceSpan span = combine_spans(left.value_ref()->span, rhs.value_ref()->span);
         left = ParserResult<ExprNodePtr>::success(make_pipe_expr(span, std::move(left.value_ref()), std::move(rhs.value_ref()), limit));
         continue;
      }

      if (next.kind() IS TokenKind::Question) {
         // Ternary operator has priority 1 (lowest). Only process if current
         // precedence level allows it, otherwise let higher-priority operators
         // complete first (e.g., x > 0 ? ... should parse as (x > 0) ? ...)

         if (1 <= precedence) break;
         this->ctx.tokens().advance();
         auto true_branch = this->parse_expression();
         if (not true_branch.ok()) return true_branch;
         this->ctx.consume(TokenKind::TernarySep, ParserErrorCode::ExpectedToken);
         auto false_branch = this->parse_expression();
         if (not false_branch.ok()) return false_branch;
         SourceSpan span = combine_spans(left.value_ref()->span, false_branch.value_ref()->span);
         ExprNodePtr ternary = make_ternary_expr(span,
            std::move(left.value_ref()), std::move(true_branch.value_ref()),
            std::move(false_branch.value_ref()));
         left = ParserResult<ExprNodePtr>::success(std::move(ternary));
         continue;
      }

      // Membership operator: expr in range
      // Transform `lhs in rhs` into a method call `rhs:contains(lhs)` so that
      // ranges can implement membership via their :contains method.

      if (next.kind() IS TokenKind::InToken) {
         constexpr uint8_t in_left = 3;
         constexpr uint8_t in_right = 3;

         if (in_left <= precedence) break;

         this->ctx.tokens().advance();
         auto right = this->parse_expression(in_right);
         if (not right.ok()) return right;

         SourceSpan left_span = left.value_ref()->span;
         SourceSpan right_span = right.value_ref()->span;

         ExprNodePtr rhs_expr = std::move(right.value_ref());
         ExprNodePtr lhs_expr = std::move(left.value_ref());

         Identifier method(&this->ctx.lua(), "contains", next.span());

         ExprNodeList args;
         args.push_back(std::move(lhs_expr));

         SourceSpan span = combine_spans(left_span, right_span);
         ExprNodePtr call = make_method_call_expr(span, std::move(rhs_expr), method, std::move(args), false);
         left = ParserResult<ExprNodePtr>::success(std::move(call));
         continue;
      }

      // 'is not' compound operator: treat as inequality (equivalent to !=)
      // Handled here rather than in match_binary_operator() to avoid lookahead
      // side-effects that corrupt the token stream for f-strings with expressions.

      if (next.kind() IS TokenKind::IsToken) {
         constexpr uint8_t is_left = 3;
         constexpr uint8_t is_right = 3;
         if (is_left <= precedence) break;
         this->ctx.tokens().advance(); // Consume 'is'
         AstBinaryOperator is_op = AstBinaryOperator::Equal;
         if (this->ctx.tokens().current().kind() IS TokenKind::NotToken) {
            this->ctx.tokens().advance(); // Consume 'not'
            is_op = AstBinaryOperator::NotEqual;
         }
         auto right = this->parse_expression(is_right);
         if (not right.ok()) return right;
         SourceSpan span = combine_spans(left.value_ref()->span, right.value_ref()->span);
         left = ParserResult<ExprNodePtr>::success(make_binary_expr(span, is_op, std::move(left.value_ref()), std::move(right.value_ref())));
         continue;
      }

      auto op_info = this->match_binary_operator(next);
      if (not op_info.has_value()) break;
      if (op_info->left <= precedence) break;
      this->ctx.tokens().advance();
      auto right = this->parse_expression(op_info->right);
      if (not right.ok()) return right;
      SourceSpan span = combine_spans(left.value_ref()->span, right.value_ref()->span);
      left = ParserResult<ExprNodePtr>::success(make_binary_expr(span, op_info->op, std::move(left.value_ref()), std::move(right.value_ref())));
   }

   return left;
}

//********************************************************************************************************************
// Parses unary expressions (not, negation, length, bit not, prefix increment).

ParserResult<ExprNodePtr> AstBuilder::parse_unary()
{
   Token current = this->ctx.tokens().current();
   if (current.kind() IS TokenKind::NotToken) {
      this->ctx.tokens().advance();
      auto operand = this->parse_unary();
      if (not operand.ok()) return operand;

      return ParserResult<ExprNodePtr>::success(make_unary_expr(current.span(), AstUnaryOperator::Not, std::move(operand.value_ref())));
   }

   if (current.kind() IS TokenKind::Minus) {
      this->ctx.tokens().advance();
      auto operand = this->parse_unary();
      if (not operand.ok()) return operand;

      return ParserResult<ExprNodePtr>::success(make_unary_expr(current.span(), AstUnaryOperator::Negate, std::move(operand.value_ref())));
   }

   if (current.raw() IS '#') {
      this->ctx.tokens().advance();
      auto operand = this->parse_unary();
      if (not operand.ok()) return operand;
      return ParserResult<ExprNodePtr>::success(make_unary_expr(current.span(), AstUnaryOperator::Length, std::move(operand.value_ref())));
   }

   if (current.raw() IS '~') {
      this->ctx.tokens().advance();
      auto operand = this->parse_unary();
      if (not operand.ok()) return operand;

      return ParserResult<ExprNodePtr>::success(make_unary_expr(current.span(), AstUnaryOperator::BitNot, std::move(operand.value_ref())));
   }
   if (current.kind() IS TokenKind::PlusPlus) {
      this->ctx.tokens().advance();
      auto operand = this->parse_unary();
      if (not operand.ok()) return operand;

      return ParserResult<ExprNodePtr>::success(make_update_expr(current.span(), AstUpdateOperator::Increment, false, std::move(operand.value_ref())));
   }
   return this->parse_primary();
}

//********************************************************************************************************************
// Parses primary expressions (literals, identifiers, varargs, functions, tables, parenthesised expressions) and their suffixes.

ParserResult<ExprNodePtr> AstBuilder::parse_primary()
{
   Token current = this->ctx.tokens().current();
   ExprNodePtr node;
   switch (current.kind()) {
      case TokenKind::Number:
      case TokenKind::String:
      case TokenKind::Nil:
      case TokenKind::TrueToken:
      case TokenKind::FalseToken:
         node = make_literal_expr(current.span(), make_literal(current));
         this->ctx.tokens().advance();
         break;

      case TokenKind::Identifier: {
         Identifier id = make_identifier(current);
         NameRef name;
         name.identifier = id;
         this->ctx.tokens().advance();
         ExprNodePtr identifier_expr = make_identifier_expr(current.span(), name);
         if (this->ctx.check(TokenKind::Arrow)) {
            ExprNodeList parameters;
            parameters.push_back(std::move(identifier_expr));
            return this->parse_arrow_function(std::move(parameters));
         }

         node = std::move(identifier_expr);
         break;
      }

      case TokenKind::Dots:
         node = make_vararg_expr(current.span());
         this->ctx.tokens().advance();
         break;

      case TokenKind::Function: {
         Token function_token = this->ctx.tokens().current();
         this->ctx.tokens().advance();
         auto fn = this->parse_function_literal(function_token, false);
         if (not fn.ok()) return fn;

         node = std::move(fn.value_ref());
         break;
      }

      case TokenKind::ThunkToken: {
         // Anonymous thunk expression: thunk():type ... end
         Token thunk_token = this->ctx.tokens().current();
         this->ctx.tokens().advance();
         auto fn = this->parse_function_literal(thunk_token, true);
         if (not fn.ok()) return fn;

         // Only auto-invoke parameterless thunks to return thunk userdata
         // Thunks with parameters remain callable functions
         auto *payload = std::get_if<FunctionExprPayload>(&fn.value_ref()->data);
         if (payload and payload->parameters.empty()) {
            SourceSpan span = fn.value_ref()->span;
            ExprNodeList call_args;
            node = make_call_expr(span, std::move(fn.value_ref()), std::move(call_args), false);
         }
         else node = std::move(fn.value_ref());
         break;
      }

      case TokenKind::Choose: {
         auto choose_result = this->parse_choose_expr();
         if (not choose_result.ok()) return choose_result;
         node = std::move(choose_result.value_ref());
         break;
      }

      case TokenKind::LeftBrace: {
         auto table = this->parse_table_literal();
         if (not table.ok()) return table;

         node = std::move(table.value_ref());
         break;
      }

      case TokenKind::LeftParen: {
         Token open_paren = this->ctx.tokens().current();
         this->ctx.tokens().advance();
         ExprNodeList expressions;
         bool parsed_empty = false;

         if (this->ctx.check(TokenKind::RightParen)) {
            parsed_empty = true;
            this->ctx.tokens().advance();
         }
         else {
            auto expr = this->parse_expression();
            if (not expr.ok()) return expr;

            expressions.push_back(std::move(expr.value_ref()));
            while (this->ctx.match(TokenKind::Comma).ok()) {
               auto next_expr = this->parse_expression();
               if (not next_expr.ok()) return next_expr;
               expressions.push_back(std::move(next_expr.value_ref()));
            }

            this->ctx.consume(TokenKind::RightParen, ParserErrorCode::ExpectedToken);
         }

         if (this->ctx.check(TokenKind::Arrow)) {
            return this->parse_arrow_function(std::move(expressions));
         }

         if (parsed_empty) {
            return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken,
               Token::from_span(open_paren.span(), TokenKind::LeftParen),
               "empty parentheses are not an expression");
         }

         if (expressions.size() > 1) {
            return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken,
               Token::from_span(open_paren.span(), TokenKind::LeftParen),
               "multiple expressions in parentheses are not supported");
         }

         node = std::move(expressions.front());
         break;
      }

      case TokenKind::LeftBracket: {
         // Result filter prefix syntax: [_*]func()
         return this->parse_result_filter_expr(current);
      }

      case TokenKind::DeferredOpen: {
         // Deferred expression: <{ expr }>
         // Desugar to: (thunk():inferred_type return expr end)()
         Token start = this->ctx.tokens().current();
         this->ctx.tokens().advance();
         auto inner = this->parse_expression();
         if (not inner.ok()) return inner;
         Token close_token = this->ctx.tokens().current();
         if (not this->ctx.match(TokenKind::DeferredClose).ok()) {
            return this->fail<ExprNodePtr>(ParserErrorCode::ExpectedToken, close_token,
               "Expected '}>' to close deferred expression");
         }

         // Infer type from inner expression
         TiriType inferred_type = infer_expression_type(*inner.value_ref());
         SourceSpan span = span_from(start, close_token);

         // Build return statement with inner expression
         ExprNodeList return_values;
         return_values.push_back(std::move(inner.value_ref()));
         StmtNodePtr return_stmt = make_return_stmt(span, std::move(return_values), false);

         // Build thunk body containing just the return statement
         StmtNodeList body_stmts;
         body_stmts.push_back(std::move(return_stmt));
         auto body = make_block(span, std::move(body_stmts));

         // Build anonymous thunk function (no parameters, is_thunk=true)
         ExprNodePtr thunk_func = make_function_expr(span, {}, false, std::move(body), true, inferred_type);

         // Build immediate call to thunk (no arguments)
         ExprNodeList call_args;
         node = make_call_expr(span, std::move(thunk_func), std::move(call_args), false);
         break;
      }

      case TokenKind::ArrayTyped: {
         // Typed array expression: array<type> or array<type, size> or array<type, expr> { values }
         // Desugar to:
         //   array<type>             -> array.new(0, 'type')
         //   array<type, size>       -> array.new(size, 'type')
         //   array<type, expr>       -> array.new(expr, 'type')
         //   array<type> { v1, v2 }  -> array.of('type', v1, v2, ...)
         //   array<type, size> { v1, v2 } -> array.new(max(size, #values), 'type') then populate

         Token start = this->ctx.tokens().current();
         GCstr *type_str = start.payload().as_string();
         int64_t specified_size = this->ctx.lex().array_typed_size;
         this->ctx.tokens().advance();

         // If size is -2, the lexer found a comma followed by a non-literal expression
         // Parse a unary expression (stops before binary operators like '>') and expect '>'
         ExprNodePtr size_expr = nullptr;
         if (specified_size IS -2) {
            auto expr_result = this->parse_unary();
            if (not expr_result.ok()) return expr_result;
            size_expr = std::move(expr_result.value_ref());

            if (not this->ctx.check(TokenKind::Greater)) {
               return this->fail<ExprNodePtr>(ParserErrorCode::ExpectedToken, this->ctx.tokens().current(),
                  "Expected '>' to close array<type, expr>");
            }
            this->ctx.tokens().advance();  // Consume '>'
         }

         // Check for optional initialiser { values }
         ExprNodeList init_values;
         bool has_initialiser = false;
         if (this->ctx.check(TokenKind::LeftBrace)) {
            has_initialiser = true;
            // Parse the table literal to extract values
            auto table_result = this->parse_table_literal();
            if (not table_result.ok()) return table_result;

            // Extract array-style values from table literal
            // The table should contain only sequential integer-keyed entries
            if (table_result.value_ref()->kind IS AstNodeKind::TableExpr) {
               auto *table_payload = std::get_if<TableExprPayload>(&table_result.value_ref()->data);
               if (table_payload) {
                  for (auto &field : table_payload->fields) {
                     if (field.kind IS TableFieldKind::Array and field.value) {
                        init_values.push_back(std::move(field.value));
                     }
                     else {
                        // Non-array field in array initialiser - emit error
                        return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, start,
                           "Array initialiser can only contain sequential values, not key-value pairs");
                     }
                  }
               }
            }
         }

         SourceSpan span = start.span();

         // Build identifier for 'array' global
         Identifier array_id = Identifier::from_keepstr(this->ctx.lex().keepstr("array"), span);
         NameRef array_ref;
         array_ref.identifier = array_id;
         ExprNodePtr array_base = make_identifier_expr(span, array_ref);

         if (has_initialiser and not init_values.empty()) {
            // array<type> { values } -> array.of('type', v1, v2, ...)
            // Build: array.of('type', values...)

            // Create member access for .of
            Identifier of_id = Identifier::from_keepstr(this->ctx.lex().keepstr("of"), span);
            ExprNodePtr array_of = make_member_expr(span, std::move(array_base), of_id, false);

            // Build argument list: ('type', v1, v2, ...)
            ExprNodeList args;

            // First argument: type name as string literal
            args.push_back(make_literal_expr(span, LiteralValue::string(type_str)));

            // Add all initialiser values
            for (auto &val : init_values) {
               args.push_back(std::move(val));
            }

            ExprNodePtr array_of_call = make_call_expr(span, std::move(array_of), std::move(args), false);

            // If size was specified (literal or expression) and may be larger than values count, wrap in IIFE to resize
            // For literal sizes, we only wrap if size > values count
            // For dynamic expressions, we always wrap since we can't know at parse time

            bool needs_resize = size_expr != nullptr or (specified_size > 0 and size_t(specified_size) > init_values.size());

            if (needs_resize) {
               // Generate: (function() local _arr = array.of(...); array.resize(_arr, size); return _arr end)()

               // Create local variable name "_arr"
               Identifier arr_id = Identifier::from_keepstr(this->ctx.lex().keepstr("_arr"), span);

               // Statement 1: local _arr = array.of('type', v1, v2, ...)

               std::vector<Identifier> local_names;
               local_names.push_back(arr_id);
               ExprNodeList local_values;
               local_values.push_back(std::move(array_of_call));
               StmtNodePtr local_stmt = make_local_decl_stmt(span, std::move(local_names), std::move(local_values));

               // Build array.resize(_arr, size_expr_or_literal)

               Identifier array_id2 = Identifier::from_keepstr(this->ctx.lex().keepstr("array"), span);
               NameRef array_ref2;
               array_ref2.identifier = array_id2;
               ExprNodePtr array_base2 = make_identifier_expr(span, array_ref2);

               Identifier resize_id = Identifier::from_keepstr(this->ctx.lex().keepstr("resize"), span);
               ExprNodePtr array_resize = make_member_expr(span, std::move(array_base2), resize_id, false);

               // Arguments for resize: (_arr, size)

               ExprNodeList resize_args;
               NameRef arr_ref;
               arr_ref.identifier = arr_id;
               resize_args.push_back(make_identifier_expr(span, arr_ref));

               // Use size_expr if available, otherwise use literal

               if (size_expr) resize_args.push_back(std::move(size_expr));
               else resize_args.push_back(make_literal_expr(span, LiteralValue::number(double(specified_size))));

               ExprNodePtr resize_call = make_call_expr(span, std::move(array_resize), std::move(resize_args), false);

               // Statement 2: array.resize(_arr, size)
               StmtNodePtr resize_stmt = make_expression_stmt(span, std::move(resize_call));

               // Statement 3: return _arr
               ExprNodeList return_values;
               NameRef arr_ref2;
               arr_ref2.identifier = arr_id;
               return_values.push_back(make_identifier_expr(span, arr_ref2));
               StmtNodePtr return_stmt = make_return_stmt(span, std::move(return_values), false);

               // Build function body block
               StmtNodeList body_stmts;
               body_stmts.push_back(std::move(local_stmt));
               body_stmts.push_back(std::move(resize_stmt));
               body_stmts.push_back(std::move(return_stmt));
               auto body = make_block(span, std::move(body_stmts));

               // Build anonymous function (no parameters)
               ExprNodePtr anon_func = make_function_expr(span, {}, false, std::move(body), false, TiriType::Any);

               // Build immediate call to function (no arguments)
               ExprNodeList call_args;
               node = make_call_expr(span, std::move(anon_func), std::move(call_args), false);
            }
            else node = std::move(array_of_call);
         }
         else {
            // Empty braces {} or no initialiser: use array.new()
            // array<type> or array<type, size> -> array.new(size, 'type')

            // Create member access for .new
            Identifier new_id = Identifier::from_keepstr(this->ctx.lex().keepstr("new"), span);
            ExprNodePtr array_new = make_member_expr(span, std::move(array_base), new_id, false);

            // Build argument list: (size, 'type')

            ExprNodeList args;
            if (size_expr) args.push_back(std::move(size_expr));
            else args.push_back(make_literal_expr(span, LiteralValue::number((specified_size >= 0) ? double(specified_size) : 0.0)));

            args.push_back(make_literal_expr(span, LiteralValue::string(type_str)));

            node = make_call_expr(span, std::move(array_new), std::move(args), false);
         }

         // Mark the result as TiriType::Array so downstream index expressions emit AGET/ASET opcodes
         if (node->kind IS AstNodeKind::CallExpr) {
            auto *call_payload = std::get_if<CallExprPayload>(&node->data);
            if (call_payload) call_payload->result_type = TiriType::Array;
         }
         break;
      }

      case TokenKind::DeferredTyped: {
         // Typed deferred expression: <type{ expr }>
         // Desugar to: (thunk():explicit_type return expr end)()

         Token start = this->ctx.tokens().current();

         // Get the type name from the token payload

         GCstr *type_str = start.payload().as_string();
         TiriType explicit_type = TiriType::Unknown;
         if (type_str) {
            std::string_view type_name(strdata(type_str), type_str->len);
            explicit_type = parse_type_name(type_name);
            if (explicit_type IS TiriType::Unknown) {
               return this->fail<ExprNodePtr>(ParserErrorCode::UnknownTypeName, start,
                  std::format("Unknown type name '{}' in typed deferred expression", type_name));
            }
         }
         this->ctx.tokens().advance();
         auto inner = this->parse_expression();
         if (not inner.ok()) return inner;
         Token close_token = this->ctx.tokens().current();
         if (not this->ctx.match(TokenKind::DeferredClose).ok()) {
            return this->fail<ExprNodePtr>(ParserErrorCode::ExpectedToken, close_token,
               "Expected '}>' to close typed deferred expression");
         }

         SourceSpan span = span_from(start, close_token);

         // Build return statement with inner expression
         ExprNodeList return_values;
         return_values.push_back(std::move(inner.value_ref()));
         StmtNodePtr return_stmt = make_return_stmt(span, std::move(return_values), false);

         // Build thunk body containing just the return statement
         StmtNodeList body_stmts;
         body_stmts.push_back(std::move(return_stmt));
         auto body = make_block(span, std::move(body_stmts));

         // Build anonymous thunk function (no parameters, is_thunk=true)
         ExprNodePtr thunk_func = make_function_expr(span, {}, false, std::move(body), true, explicit_type);

         // Build immediate call to thunk (no arguments)
         ExprNodeList call_args;
         node = make_call_expr(span, std::move(thunk_func), std::move(call_args), false);
         break;
      }

      default: {
         std::string msg;
         if (is_compound_assignment(current.kind())) {
            msg = std::format("'{}' is a statement, not an expression; use 'do ... end' for statements in arrow functions",
               this->ctx.lex().token2str(current.raw()));
         }
         else msg = std::format("Expected expression, got '{}'", this->ctx.lex().token2str(current.raw()));

         return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, current, std::move(msg));
      }
   }
   return this->parse_suffixed(std::move(node));
}

//********************************************************************************************************************
// Parses arrow function expressions: params => expr | params => do ... end.

ParserResult<ExprNodePtr> AstBuilder::parse_arrow_function(ExprNodeList parameters)
{
   Token arrow_token = this->ctx.tokens().current();
   this->ctx.consume(TokenKind::Arrow, ParserErrorCode::ExpectedToken);

   std::vector<FunctionParameter> parsed_params;
   parsed_params.reserve(parameters.size());
   const ExprNodePtr *invalid_param = nullptr;

   if (not build_arrow_parameters(parameters, parsed_params, &invalid_param)) {
      SourceSpan span = arrow_token.span();
      if (invalid_param and *invalid_param) span = (*invalid_param)->span;
      return this->fail<ExprNodePtr>(ParserErrorCode::ExpectedIdentifier,
         Token::from_span(span, TokenKind::Identifier),
         "arrow function parameters must be identifiers");
   }

   std::unique_ptr<BlockStmt> body;
   FunctionReturnTypes return_types;

   if (this->ctx.check(TokenKind::DoToken)) {
      this->ctx.tokens().advance();
      auto block = this->parse_scoped_block({ TokenKind::EndToken });
      if (not block.ok()) return ParserResult<ExprNodePtr>::failure(block.error_ref());
      this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);
      body = std::move(block.value_ref());
   }
   else {
      // Expression body - check for optional type annotation: => type: expr
      // The syntax is: => type: expr (where type is a known type name like num, str, bool, etc.)
      // We must distinguish this from method calls like: => value:method()
      // Only consume as type annotation if the identifier is a KNOWN type name.
      Token current = this->ctx.tokens().current();
      if (current.kind() IS TokenKind::Identifier) {
         // Check if this identifier is a known type name
         GCstr *type_name_str = current.identifier();
         std::string_view type_str(strdata(type_name_str), type_name_str->len);
         TiriType parsed = parse_type_name(type_str);

         // Only treat as type annotation if:
         // 1. The identifier is a known type name (not Unknown)
         // 2. It's followed by a colon
         if (not (parsed IS TiriType::Unknown)) {
            Token next = this->ctx.tokens().peek(1);
            if (next.kind() IS TokenKind::Colon) {
               // This is a type annotation: "=> type: expr"
               this->ctx.tokens().advance();  // consume type identifier
               this->ctx.tokens().advance();  // consume ':'

               return_types.types[0] = parsed;
               return_types.count = 1;
               return_types.is_explicit = true;
            }
         }
      }

      auto expr = this->parse_expression();
      if (not expr.ok()) return ParserResult<ExprNodePtr>::failure(expr.error_ref());

      // Check if a compound assignment follows - this indicates the user tried to use a statement
      // in an expression-body arrow function. Provide a helpful error message.
      Token next = this->ctx.tokens().current();
      if (is_compound_assignment(next.kind())) {
         return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, next,
            std::format("'{}' is a statement, not an expression; use 'do ... end' for statement bodies in arrow functions",
               this->ctx.lex().token2str(next.raw())));
      }

      ExprNodeList return_values;
      return_values.push_back(std::move(expr.value_ref()));
      SourceSpan return_span = return_values.front()->span;
      StmtNodePtr return_stmt = make_return_stmt(return_span, std::move(return_values), false);

      StmtNodeList statements;
      statements.push_back(std::move(return_stmt));
      body = make_block(return_span, std::move(statements));
   }

   SourceSpan function_span = arrow_token.span();
   if (not parsed_params.empty()) function_span = combine_spans(parsed_params.front().name.span, body->span);
   else function_span = combine_spans(arrow_token.span(), body->span);

   ExprNodePtr node = make_function_expr(function_span, std::move(parsed_params), false, std::move(body),
      false, TiriType::Any, return_types);
   return ParserResult<ExprNodePtr>::success(std::move(node));
}

//********************************************************************************************************************
// Parses suffix operations on expressions (field access, indexing, method calls, function calls, postfix increment, presence checks).

ParserResult<ExprNodePtr> AstBuilder::parse_suffixed(ExprNodePtr base)
{
   while (true) {
      Token token = this->ctx.tokens().current();
      if (token.kind() IS TokenKind::Dot) {
         this->ctx.tokens().advance();
         auto name_token = this->ctx.expect_name(ParserErrorCode::ExpectedIdentifier);
         if (not name_token.ok()) return ParserResult<ExprNodePtr>::failure(name_token.error_ref());

         base = make_member_expr(span_from(token, name_token.value_ref()), std::move(base),
            make_identifier(name_token.value_ref()), false);
         continue;
      }

      if (token.kind() IS TokenKind::SafeField) {
         this->ctx.tokens().advance();
         auto name_token = this->ctx.expect_name(ParserErrorCode::ExpectedIdentifier);
         if (not name_token.ok()) return ParserResult<ExprNodePtr>::failure(name_token.error_ref());

         base = make_safe_member_expr(span_from(token, name_token.value_ref()), std::move(base),
            make_identifier(name_token.value_ref()));
         continue;
      }

      if (token.kind() IS TokenKind::LeftBracket) {
         this->ctx.tokens().advance();
         auto index = this->parse_expression();
         if (not index.ok()) return index;

         this->ctx.consume(TokenKind::RightBracket, ParserErrorCode::ExpectedToken);
         SourceSpan span = combine_spans(base->span, index.value_ref()->span);
         base = make_index_expr(span, std::move(base), std::move(index.value_ref()));
         continue;
      }

      if (token.kind() IS TokenKind::SafeIndex) {
         this->ctx.tokens().advance();
         auto index = this->parse_expression();
         if (not index.ok()) return index;

         this->ctx.consume(TokenKind::RightBracket, ParserErrorCode::ExpectedToken);
         SourceSpan span = combine_spans(base->span, index.value_ref()->span);
         base = make_safe_index_expr(span, std::move(base), std::move(index.value_ref()));
         continue;
      }

      if (token.kind() IS TokenKind::Colon) {
         this->ctx.tokens().advance();
         auto name_token = this->ctx.expect_name(ParserErrorCode::ExpectedIdentifier);
         if (not name_token.ok()) return ParserResult<ExprNodePtr>::failure(name_token.error_ref());

         bool forwards = false;
         auto args = this->parse_call_arguments(&forwards);
         if (not args.ok()) return ParserResult<ExprNodePtr>::failure(args.error_ref());

         SourceSpan span = combine_spans(base->span, name_token.value_ref().span());
         base = make_method_call_expr(span, std::move(base),
            make_identifier(name_token.value_ref()), std::move(args.value_ref()), forwards);
         continue;
      }

      if (token.kind() IS TokenKind::SafeMethod) {
         this->ctx.tokens().advance();
         auto name_token = this->ctx.expect_name(ParserErrorCode::ExpectedIdentifier);
         if (not name_token.ok()) return ParserResult<ExprNodePtr>::failure(name_token.error_ref());

         bool forwards = false;
         auto args = this->parse_call_arguments(&forwards);
         if (not args.ok()) return ParserResult<ExprNodePtr>::failure(args.error_ref());

         SourceSpan span = combine_spans(base->span, name_token.value_ref().span());
         base = make_safe_method_call_expr(span, std::move(base),
            make_identifier(name_token.value_ref()), std::move(args.value_ref()), forwards);
         continue;
      }

      if (token.kind() IS TokenKind::LeftParen or token.kind() IS TokenKind::String) {
         // For string tokens, check if this is actually the start of a choose case pattern
         // (string followed by ->). If so, don't treat it as a call argument.
         if (token.kind() IS TokenKind::String) {
            Token next = this->ctx.tokens().peek(1);
            if (next.kind() IS TokenKind::CaseArrow) break;
         }

         // For parentheses in a choose expression context, check if this starts a tuple pattern.
         // We scan ahead to find ) and check if -> or 'when' follows.
         // BUT: if the base expression is callable (identifier, member, index), treat as function call.
         if (token.kind() IS TokenKind::LeftParen and this->in_choose_expression) {
            // If base is a callable expression, this is a function call, not a tuple pattern
            bool is_callable = base->kind IS AstNodeKind::IdentifierExpr or
                               base->kind IS AstNodeKind::MemberExpr or
                               base->kind IS AstNodeKind::IndexExpr or
                               base->kind IS AstNodeKind::CallExpr;

            if (not is_callable) {
               // Look ahead to find matching ) and check if followed by ->
               int paren_depth = 1;
               size_t pos = 1;  // Start after (
               while (paren_depth > 0 and pos < 100) {  // Limit scan to avoid performance issues
                  Token ahead = this->ctx.tokens().peek(pos);
                  if (ahead.kind() IS TokenKind::LeftParen) paren_depth++;
                  else if (ahead.kind() IS TokenKind::RightParen) paren_depth--;
                  else if (ahead.kind() IS TokenKind::EndOfFile) break;
                  pos++;
               }
               // Check if ) is followed by -> (tuple pattern) or 'when' (tuple pattern with guard)
               if (paren_depth IS 0) {
                  Token after_paren = this->ctx.tokens().peek(pos);
                  if (after_paren.kind() IS TokenKind::CaseArrow or after_paren.kind() IS TokenKind::When) {
                     break;  // This is a tuple pattern, not a function call
                  }
               }
            }
         }

         bool forwards = false;
         auto args = this->parse_call_arguments(&forwards);
         if (not args.ok()) return ParserResult<ExprNodePtr>::failure(args.error_ref());
         SourceSpan span = combine_spans(base->span, token.span());
         base = make_call_expr(span, std::move(base), std::move(args.value_ref()), forwards);
         continue;
      }

      if (token.kind() IS TokenKind::PlusPlus) {
         this->ctx.tokens().advance();
         base = make_update_expr(token.span(), AstUpdateOperator::Increment, true, std::move(base));
         continue;
      }

      if (token.kind() IS TokenKind::Presence and this->ctx.lex().should_emit_presence()) {
         this->ctx.tokens().advance();
         base = make_presence_expr(token.span(), std::move(base));
         continue;
      }
      break;
   }
   return ParserResult<ExprNodePtr>::success(std::move(base));
}

//********************************************************************************************************************
// Matches a token to a binary operator and returns its precedence information, or returns empty if not a binary operator.

std::optional<AstBuilder::BinaryOpInfo> AstBuilder::match_binary_operator(const Token &token) const
{
   BinaryOpInfo info;
   switch (token.kind()) {
      case TokenKind::Plus:
         info.op = AstBinaryOperator::Add;
         info.left = 7;
         info.right = 7;
         return info;
      case TokenKind::Minus:
         // Check if this is actually the start of a choose case negative literal pattern
         // (-number -> result). If so, don't treat it as a binary operator.
         // Only check when inside a choose expression, and skip when parsing guard expressions.
         if (this->in_choose_expression and not this->in_guard_expression) {
            if (this->is_choose_relational_pattern(1)) return std::nullopt;
         }
         info.op = AstBinaryOperator::Subtract;
         info.left = 7;
         info.right = 7;
         return info;
      case TokenKind::Multiply:
         info.op = AstBinaryOperator::Multiply;
         info.left = 8;
         info.right = 8;
         return info;
      case TokenKind::Divide:
         info.op = AstBinaryOperator::Divide;
         info.left = 8;
         info.right = 8;
         return info;
      case TokenKind::Modulo:
         info.op = AstBinaryOperator::Modulo;
         info.left = 8;
         info.right = 8;
         return info;
      case TokenKind::Cat:
         info.op = AstBinaryOperator::Concat;
         info.left = 5;
         info.right = 4;
         return info;
      case TokenKind::Equal:
         info.op = AstBinaryOperator::Equal;
         info.left = 3;
         info.right = 3;
         return info;
      case TokenKind::NotEqual:
         info.op = AstBinaryOperator::NotEqual;
         info.left = 3;
         info.right = 3;
         return info;
      case TokenKind::LessEqual:
         // Check if this is actually the start of a choose case relational pattern
         // (<= followed by expression then ->). If so, don't treat it as a binary operator.
         // Only check when inside a choose expression, and skip when parsing guard expressions.
         if (this->in_choose_expression and not this->in_guard_expression) {
            if (this->is_choose_relational_pattern(1)) return std::nullopt;
         }
         info.op = AstBinaryOperator::LessEqual;
         info.left = 3;
         info.right = 3;
         return info;
      case TokenKind::GreaterEqual:
         // Check if this is actually the start of a choose case relational pattern
         // (>= followed by expression then ->). If so, don't treat it as a binary operator.
         // Only check when inside a choose expression, and skip when parsing guard expressions.
         if (this->in_choose_expression and not this->in_guard_expression) {
            if (this->is_choose_relational_pattern(1)) return std::nullopt;
         }
         info.op = AstBinaryOperator::GreaterEqual;
         info.left = 3;
         info.right = 3;
         return info;
      case TokenKind::AndToken:
         info.op = AstBinaryOperator::LogicalAnd;
         info.left = 2;
         info.right = 2;
         return info;
      case TokenKind::OrToken:
         info.op = AstBinaryOperator::LogicalOr;
         info.left = 1;
         info.right = 1;
         return info;
      case TokenKind::HasToken:
         info.op = AstBinaryOperator::HasFlag;
         info.left = 3;
         info.right = 2;  // Allow bitwise ops (|, ^, &) to be parsed as RHS without parentheses
         return info;
      case TokenKind::Presence:
         // Only treat ?? as binary if-empty when lookahead indicates binary usage
         if (not this->ctx.lex().should_emit_presence()) {
            info.op = AstBinaryOperator::IfEmpty;
            info.left = 1;
            info.right = 1;
            return info;
         }
         break;  // Not a binary operator, will be handled as postfix
      case TokenKind::ShiftLeft:
         info.op = AstBinaryOperator::ShiftLeft;
         info.left = 6;   // C precedence: shifts bind tighter than AND (5)
         info.right = 6;  // Left-associative: 1 << 2 << 3 = (1 << 2) << 3
         return info;
      case TokenKind::ShiftRight:
         info.op = AstBinaryOperator::ShiftRight;
         info.left = 6;   // C precedence: shifts bind tighter than AND (5)
         info.right = 6;  // Left-associative
         return info;
      case TokenKind::Power:
         info.op = AstBinaryOperator::Power;
         info.left = 11;
         info.right = 10;  // Right-associative
         return info;
      default:
         break;
   }

   if (token.raw() IS '<') {
      // Check if this is actually the start of a choose case relational pattern
      // (< followed by expression then ->). If so, don't treat it as a binary operator.
      // Only check when inside a choose expression, and skip when parsing guard expressions.

      if (this->in_choose_expression and not this->in_guard_expression) {
         Token peek1 = this->ctx.tokens().peek(1);

         // Check for <= pattern: < = expr ->
         if (peek1.kind() IS TokenKind::Equals) {
            if (this->is_choose_relational_pattern(2)) return std::nullopt;
         }
         else {
            if (this->is_choose_relational_pattern(1)) return std::nullopt;
         }
      }
      info.op = AstBinaryOperator::LessThan;
      info.left = 3;
      info.right = 3;
      return info;
   }

   if (token.raw() IS '>') {
      // Check if this is actually the start of a choose case relational pattern
      // (> followed by expression then ->). If so, don't treat it as a binary operator.
      // Only check when inside a choose expression, and skip when parsing guard expressions.

      if (this->in_choose_expression and not this->in_guard_expression) {
         Token peek1 = this->ctx.tokens().peek(1);

         // Check for >= pattern: > = expr ->
         if (peek1.kind() IS TokenKind::Equals) {
            if (this->is_choose_relational_pattern(2)) return std::nullopt;
         }
         else {
            if (this->is_choose_relational_pattern(1)) return std::nullopt;
         }
      }
      info.op = AstBinaryOperator::GreaterThan;
      info.left = 3;
      info.right = 3;
      return info;
   }

   if (token.raw() IS '&') {
      info.op = AstBinaryOperator::BitAnd;
      info.left = 5;  // AND > XOR > OR per C precedence; above shifts (6)
      info.right = 5;  // Left-associative: a & b & c = (a & b) & c
      return info;
   }

   if (token.raw() IS '|') {
      info.op = AstBinaryOperator::BitOr;
      info.left = 3;  // Above logical-and (2); allows `x has A|B` to parse as `x has (A|B)`
      info.right = 3;  // Left-associative: a | b | c = (a | b) | c
      return info;
   }

   if (token.raw() IS '^') {
      info.op = AstBinaryOperator::BitXor;
      info.left = 4;  // XOR binds tighter than OR (3), looser than AND (5)
      info.right = 4;  // Left-associative: a ^ b ^ c = (a ^ b) ^ c
      return info;
   }
   return std::nullopt;
}

//********************************************************************************************************************
// Checks if looking at a choose expression relational pattern by scanning ahead through the expression.
// Start position is the offset from current token (e.g., 1 to start after '<').
// Returns true if the pattern ends with '->' (CaseArrow), indicating this is a case pattern not a binary operator.

bool AstBuilder::is_choose_relational_pattern(size_t StartPos) const
{
   size_t pos = StartPos;
   int paren_depth = 0;
   int brace_depth = 0;
   int bracket_depth = 0;

   // Scan through the expression, tracking bracket depths
   while (pos < 100) {  // Limit scan to avoid performance issues
      Token ahead = this->ctx.tokens().peek(pos);
      TokenKind kind = ahead.kind();

      // Track nesting depths
      if (kind IS TokenKind::LeftParen) paren_depth++;
      else if (kind IS TokenKind::RightParen) {
         if (paren_depth IS 0) break;  // Unmatched close, end of expression
         paren_depth--;
      }
      else if (kind IS TokenKind::LeftBrace) brace_depth++;
      else if (kind IS TokenKind::RightBrace) {
         if (brace_depth IS 0) break;
         brace_depth--;
      }
      else if (kind IS TokenKind::LeftBracket) bracket_depth++;
      else if (kind IS TokenKind::RightBracket) {
         if (bracket_depth IS 0) break;
         bracket_depth--;
      }
      // At depth 0, check for CaseArrow which indicates this is a choose pattern
      else if (paren_depth IS 0 and brace_depth IS 0 and bracket_depth IS 0) {
         if (kind IS TokenKind::CaseArrow) return true;

         // These tokens indicate end of expression without finding CaseArrow
         if (kind IS TokenKind::EndToken or
             kind IS TokenKind::EndOfFile or
             kind IS TokenKind::Else or
             kind IS TokenKind::When or
             kind IS TokenKind::Comma or
             kind IS TokenKind::Semicolon or
             kind IS TokenKind::ThenToken or
             kind IS TokenKind::DoToken) {
            return false;
         }
      }
      pos++;
   }
   return false;
}
