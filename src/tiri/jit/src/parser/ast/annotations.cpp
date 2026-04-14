// AST Builder - Annotation Parsers
// Copyright © 2025-2026 Paul Manias
//
// This file contains parsers for annotations:
// - Annotation values (strings, numbers, booleans, arrays)
// - Annotation entries with arguments
// - Annotated statements (functions with annotations)

//********************************************************************************************************************
// Parses annotation value types: strings, numbers, booleans, arrays, and bare identifiers.
// @Test(name="foo", count=5, enabled=true, labels=["a","b"], fast)

ParserResult<AnnotationArgValue> AstBuilder::parse_annotation_value()
{
   Token current = this->ctx.tokens().current();
   AnnotationArgValue value;

   // String literal
   if (current.kind() IS TokenKind::String) {
      value.type = AnnotationArgValue::Type::String;
      value.string_value = current.payload().as_string();
      this->ctx.tokens().advance();
      return ParserResult<AnnotationArgValue>::success(std::move(value));
   }

   // Number literal
   if (current.kind() IS TokenKind::Number) {
      value.type = AnnotationArgValue::Type::Number;
      value.number_value = current.payload().as_number();
      this->ctx.tokens().advance();
      return ParserResult<AnnotationArgValue>::success(std::move(value));
   }

   // Boolean literals (true/false)
   if (current.kind() IS TokenKind::TrueToken) {
      value.type = AnnotationArgValue::Type::Bool;
      value.bool_value = true;
      this->ctx.tokens().advance();
      return ParserResult<AnnotationArgValue>::success(std::move(value));
   }

   if (current.kind() IS TokenKind::FalseToken) {
      value.type = AnnotationArgValue::Type::Bool;
      value.bool_value = false;
      this->ctx.tokens().advance();
      return ParserResult<AnnotationArgValue>::success(std::move(value));
   }

   // Array literal: [item, item, ...] or {item, item, ...}
   if (current.kind() IS TokenKind::LeftBracket or current.kind() IS TokenKind::LeftBrace) {
      TokenKind close_kind = (current.kind() IS TokenKind::LeftBracket) ? TokenKind::RightBracket : TokenKind::RightBrace;
      this->ctx.tokens().advance();  // Consume [ or {
      value.type = AnnotationArgValue::Type::Array;

      while (not this->ctx.check(close_kind) and not this->ctx.check(TokenKind::EndOfFile)) {
         auto element = this->parse_annotation_value();
         if (not element.ok()) return ParserResult<AnnotationArgValue>::failure(element.error_ref());
         value.array_value.push_back(std::move(element.value_ref()));

         if (not this->ctx.match(TokenKind::Comma).ok()) break;
      }

      if (not this->ctx.check(close_kind)) {
         return this->fail<AnnotationArgValue>(ParserErrorCode::ExpectedToken, this->ctx.tokens().current(),
            (close_kind IS TokenKind::RightBracket) ? "expected ']' to close array" : "expected '}' to close array");
      }
      this->ctx.tokens().advance();  // Consume ] or }
      return ParserResult<AnnotationArgValue>::success(std::move(value));
   }

   // Bare identifier (treated as string value) or error
   if (current.kind() IS TokenKind::Identifier) {
      value.type = AnnotationArgValue::Type::String;
      value.string_value = current.identifier();
      this->ctx.tokens().advance();
      return ParserResult<AnnotationArgValue>::success(std::move(value));
   }

   return this->fail<AnnotationArgValue>(ParserErrorCode::UnexpectedToken, current,
      "expected annotation value (string, number, boolean, array, or identifier)");
}

//********************************************************************************************************************
// Parses one or more annotations in sequence: @Name(args); @Name2; @Name3(args)
// Returns when a non-@ token is encountered.

ParserResult<std::vector<AnnotationEntry>> AstBuilder::parse_annotations()
{
   std::vector<AnnotationEntry> annotations;

   while (this->ctx.check(TokenKind::Annotate)) {
      Token at_token = this->ctx.tokens().current();
      this->ctx.tokens().advance();  // Consume @

      // Expect annotation name (identifier)
      auto name_result = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not name_result.ok()) return ParserResult<std::vector<AnnotationEntry>>::failure(name_result.error_ref());

      AnnotationEntry entry;
      entry.name = name_result.value_ref().identifier();
      entry.span = at_token.span();

      // Optional arguments in parentheses
      if (this->ctx.check(TokenKind::LeftParen)) {
         this->ctx.tokens().advance();  // Consume (

         while (not this->ctx.check(TokenKind::RightParen) and not this->ctx.check(TokenKind::EndOfFile)) {
            // Parse key (identifier)
            auto key_result = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
            if (not key_result.ok()) return ParserResult<std::vector<AnnotationEntry>>::failure(key_result.error_ref());
            GCstr* key = key_result.value_ref().identifier();

            // Check for = (key=value) or bare identifier (key=true)
            if (this->ctx.match(TokenKind::Equals).ok()) {
               auto value_result = this->parse_annotation_value();
               if (not value_result.ok()) return ParserResult<std::vector<AnnotationEntry>>::failure(value_result.error_ref());
               entry.args.emplace_back(key, std::move(value_result.value_ref()));
            }
            else {
               // Bare identifier = true
               AnnotationArgValue true_value;
               true_value.type = AnnotationArgValue::Type::Bool;
               true_value.bool_value = true;
               entry.args.emplace_back(key, std::move(true_value));
            }

            // Skip comma separator
            if (not this->ctx.match(TokenKind::Comma).ok()) break;
         }

         // Expect closing parenthesis
         if (not this->ctx.check(TokenKind::RightParen)) {
            return this->fail<std::vector<AnnotationEntry>>(ParserErrorCode::ExpectedToken,
               this->ctx.tokens().current(), "expected ')' to close annotation arguments");
         }
         this->ctx.tokens().advance();  // Consume )
      }

      annotations.push_back(std::move(entry));

      // Optional semicolon separator between annotations
      this->ctx.match(TokenKind::Semicolon);
   }

   return ParserResult<std::vector<AnnotationEntry>>::success(std::move(annotations));
}

//********************************************************************************************************************
// Parses a statement preceded by one or more annotations.
// Annotations can only precede function declarations (function, local function, global function, thunk).

ParserResult<StmtNodePtr> AstBuilder::parse_annotated_statement()
{
   // Parse the annotation sequence
   auto annotations_result = this->parse_annotations();
   if (not annotations_result.ok()) return ParserResult<StmtNodePtr>::failure(annotations_result.error_ref());
   std::vector<AnnotationEntry> annotations = std::move(annotations_result.value_ref());

   if (annotations.empty()) {
      // No annotations were parsed, return null statement
      return ParserResult<StmtNodePtr>::success(nullptr);
   }

   Token current = this->ctx.tokens().current();

   // Parse the following statement - must be a function declaration
   StmtNodePtr stmt;

   if (current.kind() IS TokenKind::Function or current.kind() IS TokenKind::ThunkToken) {
      auto result = this->parse_function_stmt();
      if (not result.ok()) return result;
      stmt = std::move(result.value_ref());
   }
   else if (current.kind() IS TokenKind::Local) {
      auto result = this->parse_local();
      if (not result.ok()) return result;
      stmt = std::move(result.value_ref());
      // Verify it's a local function, not a variable declaration
      if (stmt->kind != AstNodeKind::LocalFunctionStmt) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, current,
            "annotations can only precede function declarations");
      }
   }
   else if (current.kind() IS TokenKind::Global) {
      auto result = this->parse_global();
      if (not result.ok()) return result;
      stmt = std::move(result.value_ref());
      // Verify it's a global function, not a variable declaration
      if (stmt->kind != AstNodeKind::FunctionStmt) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, current,
            "annotations can only precede function declarations");
      }
   }
   else {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, current,
         "annotations must precede a function declaration");
   }

   // Attach annotations to the function payload
   if (stmt->kind IS AstNodeKind::FunctionStmt) {
      auto* payload = std::get_if<FunctionStmtPayload>(&stmt->data);
      if (payload and payload->function) {
         payload->function->annotations = std::move(annotations);
      }
   }
   else if (stmt->kind IS AstNodeKind::LocalFunctionStmt) {
      auto* payload = std::get_if<LocalFunctionStmtPayload>(&stmt->data);
      if (payload and payload->function) {
         payload->function->annotations = std::move(annotations);
      }
   }

   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}
