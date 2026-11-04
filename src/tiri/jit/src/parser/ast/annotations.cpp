// AST Builder - Annotation Parsers
// Copyright © 2025-2026 Paul Manias
//
// This file contains parsers for annotations:
// - Annotation values (strings, numbers, booleans, arrays)
// - Annotation entries with arguments
// - Annotated statements (functions with annotations)

//********************************************************************************************************************
// Parses the metadata preamble that belongs to this compilation unit.

ParserResult<bool> AstBuilder::parse_compilation_unit_preamble()
{
   while (this->ctx.check(TokenKind::Annotate)) {
      Token name = this->ctx.tokens().peek(1);
      if (not name.is_identifier() or not name.identifier()) break;
      const std::string_view annotation_name(strdata(name.identifier()), name.identifier()->len);
      if (annotation_name != "Package" and annotation_name != "Dependencies") break;

      Token declaration = this->ctx.tokens().current();
      auto parsed = this->parse_annotations(true);
      if (not parsed.ok()) return ParserResult<bool>::failure(parsed.error_ref());
      auto &annotations = parsed.value_ref();
      if (annotation_name IS "Dependencies") {
         if (this->dependency_requirements_) {
            return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
               "@Dependencies may be declared only once per compilation unit");
         }
         if (this->parent_builder and not this->module_initialiser) {
            return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
               "@Dependencies is not permitted in a local source inclusion");
         }

         tiri::DependencyRequirements requirements;
         bool have_tiri = false;
         bool have_kotuku = false;
         for (const auto &[key, value] : annotations.front().args) {
            const std::string_view key_text(strdata(key), key->len);
            if (key_text != "tiri" and key_text != "kotuku") {
               return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
                  "@Dependencies accepts only the named arguments 'tiri' and 'kotuku'");
            }

            if (value.type != AnnotationArgValue::Type::String or not value.string_literal) {
               return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
                  "@Dependencies arguments must be string literals");
            }

            const std::string text(strdata(value.string_value), value.string_value->len);
            tiri::VersionConstraint constraint;
            if (auto error = tiri::parse_version_constraint(text, constraint); error) {
               return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
                  std::format("invalid {} version constraint at byte {}: {}", key_text, error.Offset,
                     tiri::version_error_text(error.Error)));
            }

            if (key_text IS "tiri") {
               if (have_tiri) return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
                  "@Dependencies contains a duplicate 'tiri' argument");
               requirements.Tiri = std::move(constraint);
               have_tiri = true;
            }
            else {
               if (have_kotuku) return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
                  "@Dependencies contains a duplicate 'kotuku' argument");
               requirements.Kotuku = std::move(constraint);
               have_kotuku = true;
            }
         }

         tiri::CompatibilityFailure failure;
         if (not tiri::check_requirements(requirements, tiri::runtime_versions(), &failure)) {
            return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
               std::format("unsatisfied {} dependency '{}': running version {} fails comparison {}",
                  failure.Domain, failure.Constraint->Original, failure.Running->Text,
                  tiri::comparison_text(*failure.Comparison)));
         }

         this->dependency_requirements_ = std::move(requirements);
         this->ctx.lex().dependency_requirements = this->dependency_requirements_;
         if (this->ctx.check(TokenKind::Annotate) and
             this->ctx.tokens().current().span().line IS declaration.span().line) {
            return this->fail<bool>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
               "@Dependencies may not be combined with other annotations");
         }

         continue;
      }

      if (this->dependency_requirements_) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
            "@Package must precede @Dependencies in the compilation-unit preamble");
      }

      if (this->package_identity_) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
            "@Package may be declared only once per compilation unit");
      }

      if (this->parent_builder and not this->module_initialiser) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
            "@Package is not permitted in a local source inclusion");
      }

      tiri::PackageIdentity identity;
      bool have_name = false;
      bool have_version = false;
      for (const auto &[key, value] : annotations.front().args) {
         const std::string_view key_text(strdata(key), key->len);
         if (key_text != "name" and key_text != "version") {
            return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
               "@Package accepts only the named arguments 'name' and 'version'");
         }

         if (value.type != AnnotationArgValue::Type::String or not value.string_literal) {
            return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
               "@Package arguments must be string literals");
         }

         std::string text(strdata(value.string_value), value.string_value->len);
         if (key_text IS "name") {
            if (have_name) return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
               "@Package contains a duplicate 'name' argument");
            identity.Name = std::move(text);
            have_name = true;
         }
         else {
            if (have_version) return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
               "@Package contains a duplicate 'version' argument");
            identity.Version = std::move(text);
            have_version = true;
         }
      }

      if (not have_name or not have_version) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
            "@Package requires exactly one 'name' and one 'version' argument");
      }

      if (auto error = tiri::validate_package_name(identity.Name); error != tiri::PackageValidationError::OKAY) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
            std::string("invalid package name: ") + std::string(tiri::package_validation_error_text(error)));
      }

      if (auto error = tiri::parse_package_version(identity.Version); error != tiri::PackageValidationError::OKAY) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
            std::string("invalid package version: ") + std::string(tiri::package_validation_error_text(error)));
      }

      if (this->expected_package_ and identity != *this->expected_package_) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, declaration,
            std::format("package identity mismatch: expected '{}@{}', declared '{}@{}'",
               this->expected_package_->Name, this->expected_package_->Version, identity.Name, identity.Version));
      }

      this->package_identity_ = std::move(identity);
      this->package_declaration_span_ = declaration.span();
      this->ctx.lex().package_identity = this->package_identity_;
      if (this->ctx.check(TokenKind::Annotate) and
          this->ctx.tokens().current().span().line IS declaration.span().line) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "@Package may not be combined with other annotations");
      }
   }

   if (this->expected_package_ and not this->package_identity_) {
      return this->fail<bool>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         std::format("package entry point must declare @Package(name=\"{}\", version=\"{}\")",
            this->expected_package_->Name, this->expected_package_->Version));
   }

   if (this->expected_resolution_) {
      std::string diagnostic;
      if (not tiri::check_resolved_package(*this->expected_resolution_, this->package_identity_, diagnostic)) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(), diagnostic);
      }
   }

   if (this->import_requirement_) {
      tiri::PackageImportFailure failure;
      if (not tiri::check_package_import(*this->import_requirement_, this->package_identity_, &failure)) {
         return this->fail<bool>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            tiri::package_import_error(*this->import_requirement_, failure));
      }
   }
   return ParserResult<bool>::success(true);
}

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
      value.string_literal = true;
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

ParserResult<std::vector<AnnotationEntry>> AstBuilder::parse_annotations(bool Single)
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
      if (Single) break;
   }

   return ParserResult<std::vector<AnnotationEntry>>::success(std::move(annotations));
}

//********************************************************************************************************************
// Attaches parsed annotations to a function expression value.

static bool attach_annotations_to_function_value(ExprNodePtr &Value, std::vector<AnnotationEntry> &Annotations)
{
   if (not Value or Value->kind != AstNodeKind::FunctionExpr) return false;

   if (auto *function = std::get_if<FunctionExprPayload>(&Value->data)) {
      function->annotations = std::move(Annotations);
      return true;
   }

   return false;
}

//********************************************************************************************************************
// Attaches parsed annotations to a statement containing a single function expression.

static bool attach_annotations_to_statement(StmtNode &Statement, std::vector<AnnotationEntry> &Annotations)
{
   if (Statement.kind IS AstNodeKind::FunctionStmt) {
      auto *payload = std::get_if<FunctionStmtPayload>(&Statement.data);
      if (payload and payload->function) {
         payload->function->annotations = std::move(Annotations);
         return true;
      }
   }
   else if (Statement.kind IS AstNodeKind::LocalFunctionStmt) {
      auto *payload = std::get_if<LocalFunctionStmtPayload>(&Statement.data);
      if (payload and payload->function) {
         payload->function->annotations = std::move(Annotations);
         return true;
      }
   }
   else if (Statement.kind IS AstNodeKind::AssignmentStmt) {
      auto *payload = std::get_if<AssignmentStmtPayload>(&Statement.data);
      if (payload and payload->values.size() IS 1) {
         return attach_annotations_to_function_value(payload->values[0], Annotations);
      }
   }
   else if (Statement.kind IS AstNodeKind::LocalDeclStmt) {
      auto *payload = std::get_if<LocalDeclStmtPayload>(&Statement.data);
      if (payload and payload->values.size() IS 1) {
         return attach_annotations_to_function_value(payload->values[0], Annotations);
      }
   }
   else if (Statement.kind IS AstNodeKind::GlobalDeclStmt) {
      auto *payload = std::get_if<GlobalDeclStmtPayload>(&Statement.data);
      if (payload and payload->values.size() IS 1) {
         return attach_annotations_to_function_value(payload->values[0], Annotations);
      }
   }

   return false;
}

//********************************************************************************************************************
// Parses a statement preceded by one or more annotations.
// Annotations can precede function declarations or single function-valued assignments.

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

   for (const auto &annotation : annotations) {
      if (annotation.name and std::string_view(strdata(annotation.name), annotation.name->len) IS "Package") {
         Token current = this->ctx.tokens().current();
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, current,
            "@Package must appear in the compilation-unit preamble");
      }
      if (annotation.name and std::string_view(strdata(annotation.name), annotation.name->len) IS "Dependencies") {
         Token current = this->ctx.tokens().current();
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, current,
            "@Dependencies must appear in the compilation-unit preamble");
      }
   }

   Token current = this->ctx.tokens().current();

   // Parse the following statement - must contain a single function value.
   StmtNodePtr stmt;

   if (current.kind() IS TokenKind::Function or current.kind() IS TokenKind::ThunkToken) {
      auto result = this->parse_function_stmt();
      if (not result.ok()) return result;
      stmt = std::move(result.value_ref());
   }
   else if (current.kind() IS TokenKind::Local) {
      auto result = this->parse_explicit_local_declaration();
      if (not result.ok()) return result;
      stmt = std::move(result.value_ref());
   }
   else if (current.kind() IS TokenKind::Global) {
      auto result = this->parse_global();
      if (not result.ok()) return result;
      stmt = std::move(result.value_ref());
   }
   else if (current.kind() IS TokenKind::Identifier) {
      auto result = this->parse_expression_stmt();
      if (not result.ok()) return result;
      stmt = std::move(result.value_ref());
   }
   else {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, current,
         "annotations must precede a function declaration or function assignment");
   }

   if (not stmt or not attach_annotations_to_statement(*stmt, annotations)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, current,
         "annotations must precede a function declaration or function assignment");
   }

   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}
