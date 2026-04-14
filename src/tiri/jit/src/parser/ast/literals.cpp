// AST Builder - Literal and Composite Parsers
// Copyright © 2025-2026 Paul Manias
//
// This file contains parsers for literals and composite constructs:
// - Function literals
// - Table literals and fields
// - Range literals
// - Expression lists and name lists
// - Parameter lists
// - Call arguments
// - Result filter expressions
// - Return type annotations

//********************************************************************************************************************
// Parses function literals (anonymous functions) with parameters and body.
// Parses optional return type annotation after parameters for all functions.
// If is_thunk is true, validates thunk-specific constraints.

ParserResult<ExprNodePtr> AstBuilder::parse_function_literal(const Token &function_token, bool is_thunk)
{
   auto params = this->parse_parameter_list(false);
   if (not params.ok()) return ParserResult<ExprNodePtr>::failure(params.error_ref());

   if (is_thunk and params.value_ref().is_vararg) {
      return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "thunk functions do not support varargs");
   }

   // Parse optional return type annotation for all functions (not just thunks)
   auto type_result = this->parse_return_type_annotation();
   if (not type_result.ok()) return ParserResult<ExprNodePtr>::failure(type_result.error_ref());
   FunctionReturnTypes return_types = type_result.value_ref();

   // For thunk compatibility: extract single return type for thunk_return_type field
   TiriType thunk_return_type = TiriType::Any;
   if (is_thunk and return_types.count > 0) {
      thunk_return_type = return_types.types[0];
   }

   const TokenKind terms[] = { TokenKind::EndToken };
   ++this->function_depth_;
   auto body = this->parse_block(terms);
   --this->function_depth_;
   if (not body.ok()) return ParserResult<ExprNodePtr>::failure(body.error_ref());

   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);
   ExprNodePtr node = make_function_expr(function_token.span(), std::move(params.value_ref().parameters),
      params.value_ref().is_vararg, std::move(body.value_ref()), is_thunk, thunk_return_type, return_types);
   return ParserResult<ExprNodePtr>::success(std::move(node));
}

//********************************************************************************************************************
// Checks if the token stream matches a range literal pattern using lookahead.
// Valid patterns: {num..num}, {ident..ident}, {-num..num}, {ident..-num}, etc.
// Returns true if the pattern matches, and sets is_inclusive for ... (three dots).

static bool check_range_pattern(ParserContext& ctx, bool& is_inclusive)
{
   is_inclusive = false;

   // Helper to get the token count for a simple range operand (number, identifier, or -number)
   // Returns 0 if not a valid range operand
   auto operand_length = [&ctx](int start_offset) -> int {
      Token tok = ctx.tokens().peek(start_offset);
      if (tok.kind() IS TokenKind::Number or tok.kind() IS TokenKind::Identifier) return 1;

      if (tok.kind() IS TokenKind::Minus) {
         Token next = ctx.tokens().peek(start_offset + 1);
         if (next.kind() IS TokenKind::Number) return 2;  // -num
      }
      return 0;
   };

   // Check first operand

   int first_len = operand_length(0);
   if (first_len IS 0) return false;

   // Check for range operator at expected position

   Token range_op = ctx.tokens().peek(first_len);
   if (range_op.kind() IS TokenKind::Cat) is_inclusive = false;
   else if (range_op.kind() IS TokenKind::Dots) is_inclusive = true;
   else return false;

   // Check second operand

   int second_len = operand_length(first_len + 1);
   if (second_len IS 0) return false;

   // Verify the range is followed by closing brace (strict pattern match)

   Token closing = ctx.tokens().peek(first_len + 1 + second_len);
   return closing.kind() IS TokenKind::RightBrace;
}

//********************************************************************************************************************
// Parses table constructor expressions with array and record fields.
// Also handles range literals: {start..stop} (exclusive) and {start...stop} (inclusive)

ParserResult<ExprNodePtr> AstBuilder::parse_table_literal()
{
   Token token = this->ctx.tokens().current();
   this->ctx.tokens().advance();

   // Check for range literal pattern using lookahead: {expr..expr} or {expr...expr}
   // This avoids ambiguity with string concatenation like {'str' .. func(), ...}

   if (not this->ctx.check(TokenKind::RightBrace)) {
      bool is_inclusive = false;

      if (check_range_pattern(this->ctx, is_inclusive)) {
         // Confirmed range pattern - parse start expression
         auto first_expr = this->parse_unary();
         if (not first_expr.ok()) return ParserResult<ExprNodePtr>::failure(first_expr.error_ref());

         // Consume the range operator (already verified by lookahead)
         this->ctx.tokens().advance();

         // Parse stop expression
         auto stop_expr = this->parse_unary();
         if (not stop_expr.ok()) return ParserResult<ExprNodePtr>::failure(stop_expr.error_ref());

         this->ctx.consume(TokenKind::RightBrace, ParserErrorCode::ExpectedToken);
         ExprNodePtr node = make_range_expr(token.span(), std::move(first_expr.value_ref()),
            std::move(stop_expr.value_ref()), is_inclusive);
         return ParserResult<ExprNodePtr>::success(std::move(node));
      }
   }

   // Standard table parsing path
   bool has_array = false;
   auto fields = this->parse_table_fields(&has_array);
   if (not fields.ok()) return ParserResult<ExprNodePtr>::failure(fields.error_ref());

   this->ctx.consume(TokenKind::RightBrace, ParserErrorCode::ExpectedToken);
   ExprNodePtr node = make_table_expr(token.span(), std::move(fields.value_ref()), has_array);
   return ParserResult<ExprNodePtr>::success(std::move(node));
}

//********************************************************************************************************************
// Parses comma-separated lists of expressions.

ParserResult<ExprNodeList> AstBuilder::parse_expression_list()
{
   ExprNodeList nodes;
   auto first = this->parse_expression();
   if (not first.ok()) return ParserResult<ExprNodeList>::failure(first.error_ref());

   nodes.push_back(std::move(first.value_ref()));
   while (this->ctx.match(TokenKind::Comma).ok()) {
      auto next = this->parse_expression();
      if (not next.ok()) return ParserResult<ExprNodeList>::failure(next.error_ref());
      nodes.push_back(std::move(next.value_ref()));
   }
   return ParserResult<ExprNodeList>::success(std::move(nodes));
}

//********************************************************************************************************************
// Parses comma-separated lists of identifiers with optional attributes (e.g., <close>).

ParserResult<std::vector<Identifier>> AstBuilder::parse_name_list()
{
   std::vector<Identifier> names;

   auto parse_named_identifier = [&]() -> ParserResult<Identifier> {
      auto token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not token.ok()) return ParserResult<Identifier>::failure(token.error_ref());

      Identifier identifier = make_identifier(token.value_ref());

      // Parse optional type annotation (:type)
      if (this->ctx.check(TokenKind::Colon)) {
         this->ctx.tokens().advance();

         Token type_token = this->ctx.tokens().current();
         std::string_view type_view;

         auto kind = type_token.kind();
         if (kind IS TokenKind::Identifier) {
            this->ctx.tokens().advance();
            GCstr* type_symbol = type_token.identifier();
            if (type_symbol) type_view = std::string_view(strdata(type_symbol), type_symbol->len);
         }
         else if (kind IS TokenKind::Function or kind IS TokenKind::Nil) {
            this->ctx.tokens().advance();
            type_view = token_kind_name_constexpr(kind);
         }
         else {
            this->ctx.emit_error(ParserErrorCode::ExpectedTypeName, type_token, "expected type name after ':'");
            return ParserResult<Identifier>::failure(
               ParserError(ParserErrorCode::ExpectedTypeName, type_token, "expected type name after ':'"));
         }

         identifier.type = parse_type_name(type_view);
         if (identifier.type IS TiriType::Unknown) {
            std::string message("Invalid type.  Common types are: any, bool, num, str, table, array");
            this->ctx.emit_error(ParserErrorCode::UnknownTypeName, type_token, message);
            return ParserResult<Identifier>::failure(
               ParserError(ParserErrorCode::UnknownTypeName, type_token, std::move(message)));
         }
      }

      // Check for attribute: either '<' (normal case) or 'const'/'close' followed by '<' (buffered case)
      // The buffered case occurs when the lexer's '<identifier' handling put the identifier in the buffer
      // before the '<', and then when we advanced past the variable name, we got the buffered identifier.
      bool is_buffered_attribute = false;
      if (this->ctx.tokens().current().kind() IS TokenKind::Identifier) {
         GCstr *maybe_attr = this->ctx.tokens().current().identifier();
         if (maybe_attr) {
            std::string_view attr_view(strdata(maybe_attr), maybe_attr->len);
            if ((attr_view IS "const" or attr_view IS "close") and this->ctx.tokens().peek(1).raw() IS '<') {
               is_buffered_attribute = true;
            }
         }
      }

      if (this->ctx.tokens().current().raw() IS '<' or is_buffered_attribute) {
         bool is_close_attribute = false;
         bool is_const_attribute = false;

         if (is_buffered_attribute) {
            // Current token is already the attribute name ('const' or 'close')
            GCstr *attr_name = this->ctx.tokens().current().identifier();
            std::string_view view(strdata(attr_name), attr_name->len);
            if (view IS "close") is_close_attribute = true;
            else if (view IS "const") is_const_attribute = true;

            this->ctx.tokens().advance();  // Advance past 'const'/'close'
            this->ctx.tokens().advance();  // Advance past '<'
         }
         else {
            // Normal case: current is '<'
            this->ctx.tokens().advance();  // Advance past '<'

            auto attribute = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
            if (not attribute.ok()) return ParserResult<Identifier>::failure(attribute.error_ref());

            if (GCstr *attr_name = attribute.value_ref().identifier()) {
               std::string_view view(strdata(attr_name), attr_name->len);
               if (view IS std::string_view("close")) is_close_attribute = true;
               else if (view IS std::string_view("const")) is_const_attribute = true;
            }
         }

         if (not this->ctx.lex_opt('>')) {
            Token current = this->ctx.tokens().current();
            this->ctx.emit_error(ParserErrorCode::ExpectedToken, current, "expected '>' after attribute");
            return ParserResult<Identifier>::failure(
               ParserError(ParserErrorCode::ExpectedToken, current, "expected '>' after attribute"));
         }

         if (is_close_attribute) identifier.has_close = true;
         else if (is_const_attribute) identifier.has_const = true;
         else {
            this->ctx.emit_error(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(), "unknown attribute");
         }
      }

      // Parse optional type annotation (:type) after attribute (supports `name <const>:type` syntax)
      if (identifier.type IS TiriType::Unknown and this->ctx.check(TokenKind::Colon)) {
         this->ctx.tokens().advance();

         Token type_token = this->ctx.tokens().current();
         std::string_view type_view;

         auto kind = type_token.kind();
         if (kind IS TokenKind::Identifier) {
            this->ctx.tokens().advance();
            GCstr* type_symbol = type_token.identifier();
            if (type_symbol) type_view = std::string_view(strdata(type_symbol), type_symbol->len);
         }
         else if (kind IS TokenKind::Function or kind IS TokenKind::Nil) {
            this->ctx.tokens().advance();
            type_view = token_kind_name_constexpr(kind);
         }
         else {
            this->ctx.emit_error(ParserErrorCode::ExpectedTypeName, type_token, "expected type name after ':'");
            return ParserResult<Identifier>::failure(
               ParserError(ParserErrorCode::ExpectedTypeName, type_token, "expected type name after ':'"));
         }

         identifier.type = parse_type_name(type_view);
         if (identifier.type IS TiriType::Unknown) {
            auto message = std::format("Invalid type.  Common types are: any, bool, num, str, table, array", type_view);
            this->ctx.emit_error(ParserErrorCode::UnknownTypeName, type_token, message);
            return ParserResult<Identifier>::failure(
               ParserError(ParserErrorCode::UnknownTypeName, type_token, std::move(message)));
         }
      }

      return ParserResult<Identifier>::success(std::move(identifier));
   };

   auto first = parse_named_identifier();
   if (not first.ok()) return ParserResult<std::vector<Identifier>>::failure(first.error_ref());

   names.push_back(std::move(first.value_ref()));
   while (this->ctx.match(TokenKind::Comma).ok()) {
      auto name = parse_named_identifier();
      if (not name.ok()) return ParserResult<std::vector<Identifier>>::failure(name.error_ref());
      names.push_back(std::move(name.value_ref()));
   }
   return ParserResult<std::vector<Identifier>>::success(std::move(names));
}

//********************************************************************************************************************
// Parses function parameter lists with optional type annotations and varargs.

ParserResult<AstBuilder::ParameterListResult> AstBuilder::parse_parameter_list(bool allow_optional)
{
   ParameterListResult result;
   if (allow_optional and not this->ctx.check(TokenKind::LeftParen)) {
      return ParserResult<ParameterListResult>::success(result);
   }

   this->ctx.consume(TokenKind::LeftParen, ParserErrorCode::ExpectedToken);
   if (not this->ctx.check(TokenKind::RightParen)) {
      do {
         if (this->ctx.check(TokenKind::Dots)) {
            this->ctx.tokens().advance();
            result.is_vararg = true;
            break;
         }
         auto name = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
         if (not name.ok()) return ParserResult<ParameterListResult>::failure(name.error_ref());

         FunctionParameter param;
         param.name = make_identifier(name.value_ref());

         if (this->ctx.check(TokenKind::Colon)) {
            this->ctx.tokens().advance();

            Token type_token = this->ctx.tokens().current();
            std::string_view type_view;

            auto kind = type_token.kind();
            if (kind IS TokenKind::Identifier) {
               this->ctx.tokens().advance();
               GCstr *type_symbol = type_token.identifier();
               if (type_symbol) type_view = std::string_view(strdata(type_symbol), type_symbol->len);
            }
            else if (kind IS TokenKind::Function or kind IS TokenKind::Nil) {
               this->ctx.tokens().advance();
               type_view = token_kind_name_constexpr(kind);
            }
            else {
               return this->fail<ParameterListResult>(ParserErrorCode::ExpectedTypeName, type_token,
                  "Expected type name after ':'");
            }

            param.type = parse_type_name(type_view);
            // If parse_type_name returns an invalid type, emit error
            if (param.type IS TiriType::Unknown) {
               return this->fail<ParameterListResult>(ParserErrorCode::UnknownTypeName, type_token,
                  std::format("Unknown type name '{}'; expected a valid type name", type_view));
            }
         }
         else { // No type annotation provided - emit tips for untyped parameter
            if (param.name.symbol) {
               #ifdef INCLUDE_TIPS
               auto param_name = std::string_view(strdata(param.name.symbol), param.name.symbol->len);
               auto message = std::format("Function parameter '{}' lacks type annotation", param_name);
               this->ctx.emit_tip(1, TipCategory::TypeSafety, std::move(message), name.value_ref());
               #endif
            }
         }
         result.parameters.push_back(param);
      } while (this->ctx.match(TokenKind::Comma).ok());
   }
   this->ctx.consume(TokenKind::RightParen, ParserErrorCode::ExpectedToken);
   return ParserResult<ParameterListResult>::success(std::move(result));
}

//********************************************************************************************************************
// Parses the fields inside table constructors, distinguishing between array, record, and computed key forms.

ParserResult<std::vector<TableField>> AstBuilder::parse_table_fields(bool *has_array_part)
{
   std::vector<TableField> fields;
   bool array = false;

   while (not this->ctx.check(TokenKind::RightBrace)) {
      TableField field;
      Token current = this->ctx.tokens().current();
      if (current.kind() IS TokenKind::LeftBracket) {
         this->ctx.tokens().advance();
         auto key = this->parse_expression();
         if (not key.ok()) return ParserResult<std::vector<TableField>>::failure(key.error_ref());
         this->ctx.consume(TokenKind::RightBracket, ParserErrorCode::ExpectedToken);
         this->ctx.consume(TokenKind::Equals, ParserErrorCode::ExpectedToken);
         auto value = this->parse_expression();
         if (not value.ok()) return ParserResult<std::vector<TableField>>::failure(value.error_ref());

         field.kind = TableFieldKind::Computed;
         field.key = std::move(key.value_ref());
         field.value = std::move(value.value_ref());
      }
      else if (current.kind() IS TokenKind::Identifier and this->ctx.tokens().peek(1).kind() IS TokenKind::Equals) {
         this->ctx.tokens().advance();
         this->ctx.tokens().advance();
         auto value = this->parse_expression();
         if (not value.ok()) return ParserResult<std::vector<TableField>>::failure(value.error_ref());

         field.kind = TableFieldKind::Record;
         field.name = make_identifier(current);
         field.value = std::move(value.value_ref());
      }
      else {
         auto value = this->parse_expression();
         if (not value.ok()) return ParserResult<std::vector<TableField>>::failure(value.error_ref());

         field.kind = TableFieldKind::Array;
         field.value = std::move(value.value_ref());
         array = true;
      }
      field.span = current.span();
      fields.push_back(std::move(field));
      if (this->ctx.match(TokenKind::Comma).ok()) continue;
      if (this->ctx.match(TokenKind::Semicolon).ok()) continue;
   }
   if (has_array_part) *has_array_part = array;
   return ParserResult<std::vector<TableField>>::success(std::move(fields));
}

//********************************************************************************************************************
// Parses function call arguments, handling parenthesised expressions, table constructors, and string literals.

ParserResult<ExprNodeList> AstBuilder::parse_call_arguments(bool *ForwardsMultret)
{
   ExprNodeList args;
   *ForwardsMultret = false;

   if (this->ctx.check(TokenKind::LeftParen)) {
      this->ctx.tokens().advance();
      if (not this->ctx.check(TokenKind::RightParen)) {
         auto parsed = this->parse_expression_list();
         if (not parsed.ok()) return ParserResult<ExprNodeList>::failure(parsed.error_ref());

         args = std::move(parsed.value_ref());
         if (not args.empty()) {
            const ExprNodePtr& tail = args.back();
            if (tail and (tail->kind IS AstNodeKind::CallExpr or tail->kind IS AstNodeKind::VarArgExpr)) {
               *ForwardsMultret = true;
            }
         }
      }

      this->ctx.consume(TokenKind::RightParen, ParserErrorCode::ExpectedToken);
      return ParserResult<ExprNodeList>::success(std::move(args));
   }

   if (this->ctx.check(TokenKind::LeftBrace)) {
      auto table = this->parse_table_literal();
      if (not table.ok()) return ParserResult<ExprNodeList>::failure(table.error_ref());

      args.push_back(std::move(table.value_ref()));
      return ParserResult<ExprNodeList>::success(std::move(args));
   }

   if (this->ctx.check(TokenKind::String)) {
      Token literal = this->ctx.tokens().current();
      args.push_back(make_literal_expr(literal.span(), make_literal(literal)));
      this->ctx.tokens().advance();
      return ParserResult<ExprNodeList>::success(std::move(args));
   }

   return this->fail<ExprNodeList>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
      "invalid call arguments");
}

//********************************************************************************************************************
// Parses the result filter pattern inside brackets: [_*], [*_], [_**_], etc.
// The pattern consists of '_' (drop) and '*' (keep) characters.
// The last character determines the trailing behaviour for excess values.

ParserResult<AstBuilder::ResultFilterInfo> AstBuilder::parse_result_filter_pattern()
{
   ResultFilterInfo info;
   info.keep_mask = 0;
   info.explicit_count = 0;
   info.trailing_keep = false;

   uint8_t position = 0;
   Token current = this->ctx.tokens().current();

   while (current.kind() != TokenKind::RightBracket) {
      if (position >= 64) {
         return this->fail<ResultFilterInfo>(ParserErrorCode::UnexpectedToken, current,
            "result filter pattern too long (max 64 positions)");
      }

      if (current.kind() IS TokenKind::Multiply) {  // *
         info.keep_mask |= (1ULL << position);
         info.trailing_keep = true;
         position++;
      }
      else if (current.kind() IS TokenKind::Power) {  // ** treated as two keep positions
         info.keep_mask |= (1ULL << position);
         info.trailing_keep = true;
         position++;
         if (position >= 64) {
            return this->fail<ResultFilterInfo>(ParserErrorCode::UnexpectedToken, current,
               "result filter pattern too long (max 64 positions)");
         }
         info.keep_mask |= (1ULL << position);
         position++;
      }
      else if (current.kind() IS TokenKind::Identifier) {
         // Check for underscore identifier - may contain multiple underscores (e.g. "__")
         GCstr *id = current.identifier();
         if (id) {
            const char* data = strdata(id);
            MSize len = id->len;
            bool all_underscores = true;
            for (MSize i = 0; i < len; i++) {
               if (data[i] != '_') {
                  all_underscores = false;
                  break;
               }
            }
            if (all_underscores and len > 0) {
               // Each underscore counts as one "drop" position
               for (MSize i = 0; i < len; i++) {
                  if (position >= 64) {
                     return this->fail<ResultFilterInfo>(ParserErrorCode::UnexpectedToken, current,
                        "result filter pattern too long (max 64 positions)");
                  }
                  info.trailing_keep = false;
                  position++;
                  if (position IS 64) {
                     return this->fail<ResultFilterInfo>(ParserErrorCode::UnexpectedToken, current,
                        "result filter pattern too long (max 64 positions)");
                  }
               }
            }
            else {
               return this->fail<ResultFilterInfo>(ParserErrorCode::UnexpectedToken, current,
                  "result filter pattern expects '_' or '*'");
            }
         }
         else {
            return this->fail<ResultFilterInfo>(ParserErrorCode::UnexpectedToken, current,
               "result filter pattern expects '_' or '*'");
         }
      }
      else {
         return this->fail<ResultFilterInfo>(ParserErrorCode::UnexpectedToken, current,
            "result filter pattern expects '_' or '*'");
      }

      this->ctx.tokens().advance();
      current = this->ctx.tokens().current();
   }

   info.explicit_count = position;
   return ParserResult<ResultFilterInfo>::success(info);
}

//********************************************************************************************************************
// Parses result filter expressions: [_*]func(), [*_]obj:method(), etc.
// This syntax allows selective extraction of return values from multi-value function calls.

ParserResult<ExprNodePtr> AstBuilder::parse_result_filter_expr(const Token &StartToken)
{
   this->ctx.tokens().advance();  // Consume '['

   auto filter = this->parse_result_filter_pattern();
   if (not filter.ok()) return ParserResult<ExprNodePtr>::failure(filter.error_ref());

   this->ctx.consume(TokenKind::RightBracket, ParserErrorCode::ExpectedToken);

   // Parse the expression to filter (must be followed by a callable)

   auto expr = this->parse_unary();
   if (not expr.ok()) return expr;

   expr = this->parse_suffixed(std::move(expr.value_ref()));
   if (not expr.ok()) return expr;

   // Validate that result is a call expression

   AstNodeKind kind = expr.value_ref()->kind;
   if (kind != AstNodeKind::CallExpr and kind != AstNodeKind::SafeCallExpr) {
      return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, StartToken,
         "result filter requires a function call");
   }

   // Optimisation: If the filter keeps all values (trailing_keep=true and all explicit
   // positions are kept), skip the filter wrapper entirely. This handles [*], [**], [***], etc.
   // A mask of all 1s up to explicit_count means (1 << count) - 1

   auto &f = filter.value_ref();
   uint64_t all_kept_mask = (f.explicit_count > 0) ? ((1ULL << f.explicit_count) - 1) : 0;
   if (f.trailing_keep and f.keep_mask IS all_kept_mask) return expr;  // No filtering needed, just return the call expression

   SourceSpan span = combine_spans(StartToken.span(), expr.value_ref()->span);
   return ParserResult<ExprNodePtr>::success(
      make_result_filter_expr(span, std::move(expr.value_ref()), f.keep_mask, f.explicit_count, f.trailing_keep));
}

//********************************************************************************************************************
// Parses optional return type annotation after function parameters.
// Supports single type `:type` and multiple types `:<type1, type2, ...>` syntax.
// Returns empty FunctionReturnTypes if no annotation is present.

ParserResult<FunctionReturnTypes> AstBuilder::parse_return_type_annotation()
{
   FunctionReturnTypes result;

   if (not this->ctx.match(TokenKind::Colon).ok()) {
      return ParserResult<FunctionReturnTypes>::success(result);
   }

   result.is_explicit = true;
   Token current = this->ctx.tokens().current();

   // Check for multi-type syntax: :<type1, type2, ...>
   if (current.raw() IS '<') {
      this->ctx.tokens().advance();  // consume '<'

      // Parse comma-separated type list
      do {
         current = this->ctx.tokens().current();

         // Check for variadic marker ...
         if (current.kind() IS TokenKind::Dots) {
            this->ctx.tokens().advance();
            result.is_variadic = true;
            break;  // ... must be last
         }

         // Handle overflow: 9th+ types force 8th to 'any'
         if (result.count >= MAX_RETURN_TYPES) {
            if (result.count IS MAX_RETURN_TYPES) {
               result.types[MAX_RETURN_TYPES - 1] = TiriType::Any;
            }
            // Skip remaining types until '>' or '...'
            if (current.kind() IS TokenKind::Identifier) this->ctx.tokens().advance();
            result.count++;
            continue;
         }

         // Parse type name
         auto type_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
         if (not type_token.ok()) return ParserResult<FunctionReturnTypes>::failure(type_token.error_ref());

         GCstr *type_name_str = type_token.value_ref().identifier();
         if (type_name_str IS nullptr) {
            return this->fail<FunctionReturnTypes>(ParserErrorCode::ExpectedIdentifier, type_token.value_ref(),
               "expected type name in return type list");
         }

         std::string_view type_str(strdata(type_name_str), type_name_str->len);
         TiriType parsed = parse_type_name(type_str);

         if (parsed IS TiriType::Unknown) {
            return this->fail<FunctionReturnTypes>(ParserErrorCode::UnexpectedToken, type_token.value_ref(),
               std::format("unknown type name '{}'", type_str));
         }

         result.types[result.count++] = parsed;

      } while (this->ctx.match(TokenKind::Comma).ok());

      // Expect closing '>'
      current = this->ctx.tokens().current();
      if (current.raw() IS '>') this->ctx.tokens().advance();
      else return this->fail<FunctionReturnTypes>(ParserErrorCode::ExpectedToken, current, "expected '>' to close return type list");
   }
   else {
      // Single type: :typename
      auto type_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not type_token.ok()) return ParserResult<FunctionReturnTypes>::failure(type_token.error_ref());

      GCstr *type_name_str = type_token.value_ref().identifier();
      if (type_name_str IS nullptr) {
         return this->fail<FunctionReturnTypes>(ParserErrorCode::ExpectedIdentifier, type_token.value_ref(),
            "expected type name after ':'");
      }

      std::string_view type_str(strdata(type_name_str), type_name_str->len);
      TiriType parsed = parse_type_name(type_str);

      if (parsed IS TiriType::Unknown) {
         return this->fail<FunctionReturnTypes>(ParserErrorCode::UnexpectedToken, type_token.value_ref(),
            std::format("unknown type name '{}'", type_str));
      }

      result.types[0] = parsed;
      result.count = 1;
   }

   return ParserResult<FunctionReturnTypes>::success(result);
}
