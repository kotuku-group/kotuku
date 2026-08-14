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

#include <unordered_map>

//********************************************************************************************************************
// Parses function literals (anonymous functions) with parameters and body.
// Parses optional return type annotation after parameters for all functions.
// If is_thunk is true, validates thunk-specific constraints.

ParserResult<ExprNodePtr> AstBuilder::parse_function_literal(
   const Token &FunctionToken, bool IsThunk, GCstr *FunctionName, FunctionCallableKind CallableKind)
{
   auto params = this->parse_parameter_list(false);
   if (not params.ok()) return ParserResult<ExprNodePtr>::failure(params.error_ref());

   if (IsThunk and params.value_ref().is_vararg) {
      return this->fail<ExprNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "thunk functions do not support varargs");
   }

   // Parse optional return type annotation for all functions (not just thunks)
   auto type_result = this->parse_return_type_annotation();
   if (not type_result.ok()) return ParserResult<ExprNodePtr>::failure(type_result.error_ref());
   FunctionReturnTypes return_types = type_result.value_ref();

   const TokenKind terms[] = { TokenKind::EndToken };
   FunctionNameScope function_name_scope(*this, FunctionName);
   ++this->function_depth;
   auto body = this->parse_block(terms);
   --this->function_depth;
   if (not body.ok()) return ParserResult<ExprNodePtr>::failure(body.error_ref());

   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);
   ExprNodePtr node = make_function_expr(FunctionToken.span(), std::move(params.value_ref().parameters),
      params.value_ref().is_vararg, std::move(body.value_ref()), IsThunk, return_types, CallableKind);
   return ParserResult<ExprNodePtr>::success(std::move(node));
}

// Parses table constructor expressions with array and record fields.
// Also handles range literals: {start to stop} (exclusive), {start into stop} (inclusive) and optional `by step`.

ParserResult<ExprNodePtr> AstBuilder::parse_table_literal(bool AllowRange)
{
   Token token = this->ctx.tokens().current();

   if (AllowRange) {
      RangeLiteralScan scan;
      if (scan_range_literal(this->ctx, scan)) {
         auto range = this->parse_scanned_range_in_braces(scan.has_step, scan.has_bare_string_operand);
         if (not range.ok()) return range;
         return range;
      }
   }

   // Standard table parsing path
   this->ctx.tokens().advance();
   bool has_array = false;
   auto fields = this->parse_table_fields(&has_array);
   if (not fields.ok()) return ParserResult<ExprNodePtr>::failure(fields.error_ref());

   this->ctx.consume(TokenKind::RightBrace, ParserErrorCode::ExpectedToken);
   ExprNodePtr node = make_table_expr(token.span(), std::move(fields.value_ref()), has_array);
   return ParserResult<ExprNodePtr>::success(std::move(node));
}

//********************************************************************************************************************
// Parses the braced initialiser of a typed array: array<Type> { Value, Value, ... }
//
// Typed arrays are positional by construction, so this parser returns the values directly instead of building a
// throw-away table literal.  Avoiding parse_table_fields() also avoids allocating a TableField per value and hashing
// a canonical key that is sequential by definition.
//
// Range scanning is deliberately absent: `to` and `into` are ordinary identifiers inside these braces, matching the
// previous parse_table_literal(false) behaviour.  Separators are consumed through the normal token stream, because
// peeking across one can pre-expand an f-string value into the lexer's buffered tokens and corrupt later parsing.

ParserResult<ExprNodeList> AstBuilder::parse_array_initialiser()
{
   ExprNodeList values;

   if (not this->ctx.match(TokenKind::LeftBrace).ok()) {
      return this->fail<ExprNodeList>(ParserErrorCode::ExpectedToken, this->ctx.tokens().current(),
         "Expected '{' to open array initialiser");
   }

   while (not this->ctx.check(TokenKind::RightBrace)) {
      Token current = this->ctx.tokens().current();

      // Reject keyed syntax at the offending field rather than at the array type token.  Lookahead is limited to the
      // start of the field so that buffered interpolation tokens are never disturbed.

      const bool record_key = (current.is_identifier_or_future_reserved() or current.kind() IS TokenKind::Method) and
         this->ctx.tokens().peek(1).kind() IS TokenKind::Equals;

      if (record_key or current.kind() IS TokenKind::LeftBracket) {
         return this->fail<ExprNodeList>(ParserErrorCode::UnexpectedToken, current,
            "Array initialiser can only contain sequential values, not key-value pairs");
      }

      auto value = this->parse_expression();
      if (not value.ok()) return ParserResult<ExprNodeList>::failure(value.error_ref());

      values.push_back(std::move(value.value_ref()));

      // Separators are optional; adjacent values are accepted for source compatibility.  In diagnose mode a failed
      // expression parse can leave the stream stationary, so guarantee forward progress rather than looping forever.

      if (this->ctx.match(TokenKind::Comma).ok() or this->ctx.match(TokenKind::Semicolon).ok()) continue;

      if (this->ctx.tokens().current().kind() IS current.kind() and
          this->ctx.tokens().current().span().offset IS current.span().offset) {
         return this->fail<ExprNodeList>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "Expected a value or '}' in array initialiser");
      }
   }

   this->ctx.consume(TokenKind::RightBrace, ParserErrorCode::ExpectedToken);
   return ParserResult<ExprNodeList>::success(std::move(values));
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

ParserResult<Token> AstBuilder::parse_type_annotation(
   TiriType &Type, struct_record *&StructDef, ArrayElementDescriptor &ArrayElement, bool &Required)
{
   Token type_token = this->ctx.tokens().current();
   auto kind = type_token.kind();
   std::string_view type_view;
   Required = false;

   auto finish_annotation = [&]() -> ParserResult<Token> {
      if (this->ctx.tokens().current().raw() IS '!') {
         Token required_token = this->ctx.tokens().current();
         this->ctx.tokens().advance();
         if (Type IS TiriType::Nil) {
            return this->fail<Token>(ParserErrorCode::UnexpectedToken, required_token,
               "'nil!' is contradictory because a required value cannot be nil");
         }
         Required = true;
      }
      return ParserResult<Token>::success(type_token);
   };

   if (kind IS TokenKind::ArrayTyped) {
      this->ctx.tokens().advance();
      if (this->ctx.lex().array_typed_size != -1) {
         return this->fail<Token>(ParserErrorCode::UnexpectedToken, type_token,
            "Array type annotations cannot declare a size");
      }
      GCstr *element_symbol = type_token.payload().as_string();
      std::string_view element_name(strdata(element_symbol), element_symbol->len);
      if (element_name.starts_with("array<")) {
         return this->fail<Token>(ParserErrorCode::UnexpectedToken, type_token,
            "Nested array specialisation is not supported; use array<array>");
      }
      if (element_name IS "object") {
         return this->fail<Token>(ParserErrorCode::UnknownTypeName, type_token,
            "Unknown array element type 'object'; use 'obj'");
      }
      auto element = describe_array_element(element_name IS "obj" ? "object" : element_name, &this->ctx.lua());
      if (not element or element->storage IS AET::PTR or
          (element->storage IS AET::STRUCT and not element->struct_def)) {
         return this->fail<Token>(ParserErrorCode::UnknownTypeName, type_token,
            std::format("Unknown array element type '{}'", element_name));
      }
      Type = TiriType::Array;
      ArrayElement = *element;
      return finish_annotation();
   }

   if (kind IS TokenKind::StructTyped) {
      // struct<Name> lexes as a single token carrying the referenced struct name
      this->ctx.tokens().advance();
      GCstr *name_symbol = type_token.payload().as_string();
      std::string_view name(strdata(name_symbol), name_symbol->len);
      auto found = find_struct(&this->ctx.lua(), name);
      if (not found) {
         return this->fail<Token>(ParserErrorCode::UnknownTypeName, type_token,
            std::format("Unknown struct name '{}'; declarations must precede use", name));
      }
      Type = TiriType::Struct;
      StructDef = found;
      return finish_annotation();
   }

   if (kind IS TokenKind::Identifier) {
      this->ctx.tokens().advance();
      GCstr *type_symbol = type_token.identifier();
      if (type_symbol) type_view = std::string_view(strdata(type_symbol), type_symbol->len);
   }
   else if (kind IS TokenKind::Function or kind IS TokenKind::Nil) {
      this->ctx.tokens().advance();
      type_view = token_kind_name_constexpr(kind);
   }
   else return this->fail<Token>(ParserErrorCode::ExpectedTypeName, type_token, "Expected type name after ':'");

   Type = parse_type_name(type_view);
   if (Type IS TiriType::Unknown) {
      return this->fail<Token>(ParserErrorCode::UnknownTypeName, type_token,
         std::format("Unknown type name '{}'; expected a valid type name", type_view));
   }

   if (Type IS TiriType::Array) {
      if (this->ctx.check(TokenKind::Less)) {
         this->ctx.tokens().advance();
         Token element_token = this->ctx.tokens().current();
         std::string element_storage;
         if (element_token.kind() IS TokenKind::StructTyped) {
            GCstr *name = element_token.payload().as_string();
            element_storage = std::format("struct<{}>", std::string_view(strdata(name), name->len));
            this->ctx.tokens().advance();
         }
         else if (element_token.kind() IS TokenKind::ArrayTyped) {
            return this->fail<Token>(ParserErrorCode::UnexpectedToken, element_token,
               "Nested array specialisation is not supported; use array<array>");
         }
         else if (element_token.kind() IS TokenKind::Identifier) {
            GCstr *name = element_token.identifier();
            element_storage.assign(strdata(name), name->len);
            this->ctx.tokens().advance();
         }
         else {
            return this->fail<Token>(ParserErrorCode::ExpectedTypeName, element_token,
               "Expected an array element type");
         }

         if (this->ctx.check(TokenKind::Comma)) {
            return this->fail<Token>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
               "Array type annotations cannot declare a size");
         }
         auto close = this->ctx.consume(TokenKind::Greater, ParserErrorCode::ExpectedToken);
         if (not close.ok()) return ParserResult<Token>::failure(close.error_ref());

         if (element_storage IS "object") {
            return this->fail<Token>(ParserErrorCode::UnknownTypeName, element_token,
               "Unknown array element type 'object'; use 'obj'");
         }
         auto element = describe_array_element(element_storage IS "obj" ? "object" : element_storage, &this->ctx.lua());
         if (not element or element->storage IS AET::PTR or
             (element->storage IS AET::STRUCT and not element->struct_def)) {
            return this->fail<Token>(ParserErrorCode::UnknownTypeName, element_token,
               std::format("Unknown array element type '{}'", element_storage));
         }
         ArrayElement = *element;
         return finish_annotation();
      }
      return this->fail<Token>(ParserErrorCode::ExpectedTypeName, type_token,
         "Array annotations require an element type; use array<any> for a wildcard array contract");
   }

   if (Type IS TiriType::Struct and this->ctx.check(TokenKind::Less)) {
      this->ctx.tokens().advance();
      auto name_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not name_token.ok()) return ParserResult<Token>::failure(name_token.error_ref());
      GCstr *name_symbol = name_token.value_ref().identifier();
      std::string_view name(strdata(name_symbol), name_symbol->len);
      auto found = find_struct(&this->ctx.lua(), name);
      if (not found) {
         return this->fail<Token>(ParserErrorCode::UnknownTypeName, name_token.value_ref(),
            std::format("Unknown struct name '{}'; declarations must precede use", name));
      }
      StructDef = found;
      auto close = this->ctx.consume(TokenKind::Greater, ParserErrorCode::ExpectedToken);
      if (not close.ok()) return ParserResult<Token>::failure(close.error_ref());
   }
   return finish_annotation();
}

ParserResult<std::vector<Identifier>> AstBuilder::parse_name_list()
{
   std::vector<Identifier> names;

   auto parse_named_identifier = [&]() -> ParserResult<Identifier> {
      auto token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not token.ok()) return ParserResult<Identifier>::failure(token.error_ref());

      Identifier identifier = make_identifier(token.value_ref());
      if (this->is_module_namespace_name(identifier.symbol)) {
         return this->fail<Identifier>(ParserErrorCode::UnexpectedToken, token.value_ref(),
            std::format("Module namespace '{}' cannot be declared as a variable",
               std::string_view(strdata(identifier.symbol), identifier.symbol->len)));
      }

      // Parse optional type annotation (:type)
      if (this->ctx.check(TokenKind::Colon)) {
         this->ctx.tokens().advance();
         bool required = false;
         auto parsed = this->parse_type_annotation(
            identifier.type, identifier.struct_def, identifier.array_element, required);
         if (not parsed.ok()) return ParserResult<Identifier>::failure(parsed.error_ref());
         if (required) {
            return this->fail<Identifier>(ParserErrorCode::UnexpectedToken, parsed.value_ref(),
               "Required annotations are only permitted on function parameters and return values");
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
               this->ctx.make_error(ParserErrorCode::ExpectedToken, current, "expected '>' after attribute"));
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
         bool required = false;
         auto parsed = this->parse_type_annotation(
            identifier.type, identifier.struct_def, identifier.array_element, required);
         if (not parsed.ok()) return ParserResult<Identifier>::failure(parsed.error_ref());
         if (required) {
            return this->fail<Identifier>(ParserErrorCode::UnexpectedToken, parsed.value_ref(),
               "Required annotations are only permitted on function parameters and return values");
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
         if (this->is_module_namespace_name(param.name.symbol)) {
            return this->fail<ParameterListResult>(ParserErrorCode::UnexpectedToken, name.value_ref(),
               std::format("Module namespace '{}' cannot be declared as a function parameter",
                  std::string_view(strdata(param.name.symbol), param.name.symbol->len)));
         }

         if (this->ctx.check(TokenKind::Colon)) {
            this->ctx.tokens().advance();
            auto parsed = this->parse_type_annotation(
               param.type, param.struct_def, param.array_element, param.required);
            if (not parsed.ok()) return ParserResult<ParameterListResult>::failure(parsed.error_ref());
            param.type_is_explicit = true;
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

// Canonical form of a statically known table-constructor key, used to detect provable duplicates.
//
// Numerical keys are canonicalised so that equivalent integer and floating representations collide, and named
// fields collide with equivalent constant string keys.  Keys whose value cannot be proven without evaluating user
// code are not represented here and are never diagnosed.

namespace {

struct ConstantKey {
   enum class Kind : uint8_t { Number, String, Boolean } kind;
   lua_Number number{};
   std::string text;
   bool boolean{};

   [[nodiscard]] bool operator==(const ConstantKey &Other) const
   {
      if (kind != Other.kind) return false;
      switch (kind) {
         case Kind::Number:  return number IS Other.number;
         case Kind::String:  return text IS Other.text;
         case Kind::Boolean: return boolean IS Other.boolean;
      }
      return false;
   }

   [[nodiscard]] std::string describe() const
   {
      switch (kind) {
         case Kind::Number:  return std::format("{}", number);
         case Kind::String:  return std::format("'{}'", text);
         case Kind::Boolean: return boolean ? "true" : "false";
      }
      return "?";
   }
};

struct ConstantKeyHash {
   [[nodiscard]] size_t operator()(const ConstantKey &Key) const noexcept
   {
      const size_t kind_hash = size_t(Key.kind) << 1;
      switch (Key.kind) {
         case ConstantKey::Kind::Number:  return std::hash<lua_Number>{}(Key.number) ^ kind_hash;
         case ConstantKey::Kind::String:  return std::hash<std::string>{}(Key.text) ^ kind_hash;
         case ConstantKey::Kind::Boolean: return std::hash<bool>{}(Key.boolean) ^ kind_hash;
      }
      return kind_hash;
   }
};

// These expressions retain a variable result count when they are the final positional field.  Their nominal array
// index is not guaranteed to be written, so it cannot participate in a provable duplicate-key diagnostic.

[[nodiscard]] bool can_expand_table_tail(const ExprNode &Expression)
{
   return Expression.kind IS AstNodeKind::CallExpr or Expression.kind IS AstNodeKind::SafeCallExpr or
      Expression.kind IS AstNodeKind::VarArgExpr or Expression.kind IS AstNodeKind::PipeExpr or
      Expression.kind IS AstNodeKind::ResultFilterExpr;
}

// Extract the canonical key of a field, if the parser can prove it without evaluating user code.

[[nodiscard]] std::optional<ConstantKey> constant_key_of(const TableField &Field, int32_t &NextArrayIndex)
{
   if (Field.kind IS TableFieldKind::Record) {
      if (not Field.name or not Field.name->symbol) return std::nullopt;
      ConstantKey key;
      key.kind = ConstantKey::Kind::String;
      key.text.assign(strdata(Field.name->symbol), Field.name->symbol->len);
      return key;
   }

   if (Field.kind IS TableFieldKind::Array) {
      // Positional entries occupy consecutive indices from zero, so they collide with equivalent explicit keys.
      ConstantKey key;
      key.kind = ConstantKey::Kind::Number;
      key.number = (lua_Number)NextArrayIndex++;
      return key;
   }

   // Computed: only a literal key is statically provable.
   if (not Field.key or Field.key->kind != AstNodeKind::LiteralExpr) return std::nullopt;
   const auto *literal = std::get_if<LiteralValue>(&Field.key->data);
   if (not literal) return std::nullopt;

   ConstantKey key;
   switch (literal->kind) {
      case LiteralKind::Number:
         key.kind = ConstantKey::Kind::Number;
         key.number = literal->number_value;
         return key;
      case LiteralKind::String:
         if (not literal->string_value) return std::nullopt;
         key.kind = ConstantKey::Kind::String;
         key.text.assign(strdata(literal->string_value), literal->string_value->len);
         return key;
      case LiteralKind::Boolean:
         key.kind = ConstantKey::Kind::Boolean;
         key.boolean = literal->bool_value;
         return key;
      default:
         return std::nullopt;  //  A nil key is rejected at runtime, not here.
   }
}

} // namespace

ParserResult<std::vector<TableField>> AstBuilder::parse_table_fields(bool *has_array_part)
{
   std::vector<TableField> fields;
   bool array = false;

   // Provable duplicate keys within one literal are rejected: the overwritten intermediate value cannot be observed
   // and the later entry unambiguously wins today, so accepting the literal can only hide a mistake.
   std::unordered_map<ConstantKey, Token, ConstantKeyHash> seen_keys;
   int32_t next_array_index = 0;

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
      else if ((current.is_identifier_or_future_reserved() or current.kind() IS TokenKind::Method) and
         this->ctx.tokens().peek(1).kind() IS TokenKind::Equals) {
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

      // Consume the separator through the normal token stream before checking for a trailing separator.  Peeking
      // across it can pre-expand an f-string field into the lexer's buffered tokens and corrupt subsequent parsing.
      const bool has_separator = this->ctx.match(TokenKind::Comma).ok() or
         this->ctx.match(TokenKind::Semicolon).ok();
      const bool final_field = this->ctx.check(TokenKind::RightBrace);
      const bool variable_tail = final_field and field.kind IS TableFieldKind::Array and field.value and
         can_expand_table_tail(*field.value);

      if (auto key = constant_key_of(field, next_array_index)) {
         if (not variable_tail) {
            auto [entry, inserted] = seen_keys.emplace(*key, current);
            if (not inserted) {
               return this->fail<std::vector<TableField>>(ParserErrorCode::UnexpectedToken, current,
                  std::format("Duplicate key {} in table constructor; first defined at line {}",
                     key->describe(), entry->second.span().line + 1));
            }
         }
      }

      fields.push_back(std::move(field));
      if (has_separator) continue;
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
// Parses result filter expressions: [_*]func(), [*_]obj.method(), etc.
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
   uint64_t all_kept_mask;
   if (f.explicit_count IS 0) all_kept_mask = 0;
   else if (f.explicit_count >= 64) all_kept_mask = ~uint64_t(0);
   else all_kept_mask = (uint64_t(1) << f.explicit_count) - 1;
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

      // An empty list is an explicit void declaration.
      if (this->ctx.tokens().current().raw() IS '>') {
         this->ctx.tokens().advance();
         return ParserResult<FunctionReturnTypes>::success(result);
      }

      // Parse comma-separated type list
      do {
         current = this->ctx.tokens().current();

         // Check for variadic marker ...
         if (current.kind() IS TokenKind::Dots) {
            if (result.count IS 0) {
               return this->fail<FunctionReturnTypes>(ParserErrorCode::ExpectedToken, current,
                  "variadic result marker requires a preceding result type");
            }
            this->ctx.tokens().advance();
            result.is_variadic = true;
            break;  // ... must be last
         }

         // Handle overflow: 9th+ types force 8th to 'any'
         if (result.count >= MAX_RETURN_TYPES) {
            if (result.count IS MAX_RETURN_TYPES) {
               result.types[MAX_RETURN_TYPES - 1] = TiriType::Any;
               result.required[MAX_RETURN_TYPES - 1] = false;
            }
            TiriType overflow_type = TiriType::Unknown;
            struct_record *overflow_struct = nullptr;
            ArrayElementDescriptor overflow_array;
            bool overflow_required = false;
            auto overflow_token = this->parse_type_annotation(
               overflow_type, overflow_struct, overflow_array, overflow_required);
            if (not overflow_token.ok()) {
               return ParserResult<FunctionReturnTypes>::failure(overflow_token.error_ref());
            }
            result.count++;
            continue;
         }

         TiriType parsed = TiriType::Unknown;
         struct_record *struct_def = nullptr;
         ArrayElementDescriptor array_element;
         bool required = false;
         auto type_token = this->parse_type_annotation(parsed, struct_def, array_element, required);
         if (not type_token.ok()) return ParserResult<FunctionReturnTypes>::failure(type_token.error_ref());
         result.types[result.count] = parsed;
         result.struct_defs[result.count] = struct_def;
         result.array_elements[result.count] = array_element;
         result.required[result.count++] = required;

      } while (this->ctx.match(TokenKind::Comma).ok());

      // Expect closing '>'
      current = this->ctx.tokens().current();
      if (current.raw() IS '>') this->ctx.tokens().advance();
      else return this->fail<FunctionReturnTypes>(ParserErrorCode::ExpectedToken, current, "expected '>' to close return type list");
   }
   else {
      auto type_token = this->parse_type_annotation(
         result.types[0], result.struct_defs[0], result.array_elements[0], result.required[0]);
      if (not type_token.ok()) return ParserResult<FunctionReturnTypes>::failure(type_token.error_ref());
      result.count = 1;
   }

   return ParserResult<FunctionReturnTypes>::success(result);
}
