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

      if (this->ctx.check(TokenKind::Enum)) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "Local enum declarations are not supported");
      }

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
         GCstr *funcname = name_token.value_ref().identifier();
         auto fn = this->parse_function_literal(function_token, is_thunk, funcname);
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

   auto &name_list = names.value_ref();
   size_t name_count = name_list.size();

   if (values.size() > name_count) {
      for (size_t i = name_count; i < values.size(); ++i) {
         ExprNodePtr& expr = values[i];
         if (expr and expr->kind IS AstNodeKind::IdentifierExpr) {
            auto* name_ref = std::get_if<NameRef>(&expr->data);
            if (name_ref) { // Convert this identifier expression to a variable name
               if (name_ref->identifier.is_future_reserved) {
                  return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedIdentifier,
                     Token::from_span(name_ref->identifier.span, TokenKind::Identifier),
                     future_reserved_variable_message(name_ref->identifier));
               }
               name_list.push_back(name_ref->identifier);
            }
         }
         else {
            // Non-identifier expression in trailing position - this is an error
            return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedIdentifier, this->ctx.tokens().current(),
               "Expected identifier after values in local declaration");
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

   if (this->ctx.check(TokenKind::Enum)) {
      return this->parse_enum(global_token);
   }

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
      GCstr *funcname = name_token.value_ref().identifier();
      auto fn = this->parse_function_literal(function_token, is_thunk, funcname);
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

   auto& name_list = names.value_ref();
   size_t name_count = name_list.size();

   if (values.size() > name_count) {
      for (size_t i = name_count; i < values.size(); ++i) {
         ExprNodePtr& expr = values[i];
         if (expr and expr->kind IS AstNodeKind::IdentifierExpr) {
            if (auto *name_ref = std::get_if<NameRef>(&expr->data)) {
               // Convert this identifier expression to a variable name
               if (name_ref->identifier.is_future_reserved) {
                  return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedIdentifier,
                     Token::from_span(name_ref->identifier.span, TokenKind::Identifier),
                     future_reserved_variable_message(name_ref->identifier));
               }
               name_list.push_back(name_ref->identifier);
            }
         }
         else {
            // Non-identifier expression in trailing position - this is an error
            return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedIdentifier, this->ctx.tokens().current(),
               "Expected identifier after values in global declaration");
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
// Parses extern symbol declarations.  Extern names are not emitted as variables; they only suppress strict
// undefined-identifier checks for symbols supplied by another parsed file or runtime host.

ParserResult<StmtNodePtr> AstBuilder::parse_extern()
{
   Token extern_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();

   if (this->ctx.check(TokenKind::Multiply)) {
      this->ctx.tokens().advance();

      if (this->ctx.check(TokenKind::Comma)) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "Extern wildcard cannot be combined with named symbols");
      }

      if (this->ctx.check(TokenKind::Equals) or
            token_to_assignment_op(this->ctx.tokens().current().kind()).has_value()) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "Extern declarations cannot have an initialiser");
      }

      auto stmt = std::make_unique<StmtNode>(AstNodeKind::ExternStmt, extern_token.span());
      ExternDeclStmtPayload payload;
      payload.all_symbols = true;
      stmt->data = std::move(payload);
      return ParserResult<StmtNodePtr>::success(std::move(stmt));
   }

   auto names = this->parse_name_list();
   if (not names.ok()) return ParserResult<StmtNodePtr>::failure(names.error_ref());

   if (this->ctx.check(TokenKind::Equals) or token_to_assignment_op(this->ctx.tokens().current().kind()).has_value()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "Extern declarations cannot have an initialiser");
   }

   for (const Identifier &identifier : names.value_ref()) {
      if (identifier.is_blank) {
         return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedIdentifier,
            Token::from_span(identifier.span, TokenKind::Identifier), "extern declarations require named symbols");
      }

      if (identifier.type != TiriType::Unknown or identifier.has_const or identifier.has_close) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken,
            Token::from_span(identifier.span, TokenKind::Identifier),
            "Extern declarations cannot have type annotations or attributes");
      }
   }

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::ExternStmt, extern_token.span());
   ExternDeclStmtPayload payload(std::move(names.value_ref()));
   stmt->data = std::move(payload);
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses top-level enum declarations and registers their generated constants for compile-time substitution.

struct EnumConstantDecl {
   std::string name;
   int64_t value = 0;
   SourceSpan span{};
};

static bool is_uppercase_enum_name(GCstr *Symbol)
{
   if (not Symbol or Symbol->len IS 0) return false;

   CSTRING text = strdata(Symbol);
   unsigned char first = (unsigned char)text[0];
   if (not std::isupper(int(first))) return false;

   for (size_t i = 1; i < Symbol->len; ++i) {
      unsigned char c = (unsigned char)text[i];
      if (std::isupper(int(c)) or std::isdigit(int(c)) or c IS '_') continue;
      return false;
   }
   return true;
}

// Enables lexer comment capture for the lifetime of a struct declaration.  Comments are only of interest inside a
// declaration body, so capture is switched off (and the buffer released) as soon as parsing leaves it.

struct CommentCaptureGuard {
   LexState &lex;

   explicit CommentCaptureGuard(LexState &Lex) : lex(Lex) {
      lex.comments.clear();
      lex.capture_comments = true;
   }

   ~CommentCaptureGuard() {
      lex.capture_comments = false;
      lex.comments.clear();
      lex.comments.shrink_to_fit();
   }

   CommentCaptureGuard(const CommentCaptureGuard &) = delete;
   CommentCaptureGuard & operator=(const CommentCaptureGuard &) = delete;
};

ParserResult<StmtNodePtr> AstBuilder::parse_struct_declaration()
{
   Token struct_token = this->ctx.tokens().current();
   if (not this->at_top_level()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, struct_token,
         "Struct declarations are only allowed at top-level scope");
   }
   this->ctx.tokens().advance();

   auto name_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
   if (not name_token.ok()) return ParserResult<StmtNodePtr>::failure(name_token.error_ref());
   GCstr *name_symbol = name_token.value_ref().identifier();
   std::string struct_name(strdata(name_symbol), name_symbol->len);
   if (this->ctx.check(TokenKind::LeftBrace)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "Struct declarations use 'end' termination; braces are reserved for struct<Name> { ... } construction");
   }

   // Capture comments for the body only.  The statement dispatcher peeks through the declaration name, so nothing
   // before this point could have been captured anyway.

   CommentCaptureGuard comment_capture(this->ctx.lex());

   // Tooling metadata (field display types, doc comments, spans) is only captured for documentation-processing
   // parses such as debug.validate(source, { symbols = true }); normal compiles skip the collection entirely.
   lua_State &lua_ref = this->ctx.lua();
   bool collect_meta = lua_ref.script and ((lua_ref.script->Flags & SCF::PROCESS_DOC) != SCF::NIL);
   LexState::StructDeclarationMetadata meta;
   if (collect_meta) {
      meta.name = struct_name;
      meta.keyword_span = struct_token.span();
      meta.name_span = name_token.value_ref().span();
   }

   struct_record record(struct_name);
   record.DeclarationLine = struct_token.span().line.lineNumber();
   if (auto source_symbol = this->current_source_file()) {
      record.DeclarationSource.assign(strdata(source_symbol), source_symbol->len);
   }

   while (not this->ctx.check(TokenKind::EndToken)) {
      if (this->ctx.check(TokenKind::EndOfFile)) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedEndOfFile, struct_token,
            "Unterminated struct declaration");
      }

      auto field_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not field_token.ok()) return ParserResult<StmtNodePtr>::failure(field_token.error_ref());
      GCstr *field_symbol = field_token.value_ref().identifier();
      std::string field_name(strdata(field_symbol), field_symbol->len);
      uint32_t field_hash = kt::strihash(field_name);
      for (const auto &existing : record.Fields) {
         if (existing.nameHash() IS field_hash) {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, field_token.value_ref(),
               std::format("Struct fields '{}' and '{}' collide case-insensitively", existing.Name, field_name));
         }
      }

      auto colon = this->ctx.consume(TokenKind::Colon, ParserErrorCode::ExpectedToken);
      if (not colon.ok()) return ParserResult<StmtNodePtr>::failure(colon.error_ref());
      Token type_token = this->ctx.tokens().current();
      bool array_type = type_token.kind() IS TokenKind::ArrayTyped;
      bool struct_typed = type_token.kind() IS TokenKind::StructTyped;
      int64_t array_dimension = array_type ? this->ctx.lex().array_typed_size : -1;
      if (array_type or struct_typed) this->ctx.tokens().advance();
      else {
         auto type_result = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
         if (not type_result.ok()) return ParserResult<StmtNodePtr>::failure(type_result.error_ref());
         type_token = type_result.value_ref();
      }
      GCstr *type_symbol = (array_type or struct_typed) ? type_token.payload().as_string() : type_token.identifier();
      std::string_view type_name = array_type ? std::string_view("array") :
         struct_typed ? std::string_view("struct") :
         std::string_view(strdata(type_symbol), type_symbol->len);

      struct_field field;
      field.Name = field_name;
      field.ArraySize = 0;
      bool dynamic_array = false;
      std::string type_display;  // Display spelling of the type for tooling metadata, e.g. 'obj<NetSocket>'
      if (type_name IS "array") {
         dynamic_array = true;
         if (array_dimension != -1) {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, type_token,
               "Dynamically sized struct array fields cannot declare a dimension");
         }
         std::string_view element_name(strdata(type_symbol), type_symbol->len);

         if (element_name IS "bool") { field.Type = FD_BYTE; field.NativeType = NativeStructType::Bool; }
         else if (element_name IS "char") { field.Type = FD_BYTE|FD_CUSTOM; field.NativeType = NativeStructType::Char; }
         else if (element_name IS "byte" or element_name IS "uint8") {
            field.Type = FD_BYTE; field.NativeType = NativeStructType::UInt8;
         }
         else if (element_name IS "int8") { field.Type = FD_BYTE; field.NativeType = NativeStructType::Int8; }
         else if (element_name IS "int16") { field.Type = FD_WORD; field.NativeType = NativeStructType::Int16; }
         else if (element_name IS "uint16") {
            field.Type = FD_WORD|FD_UNSIGNED; field.NativeType = NativeStructType::UInt16;
         }
         else if (element_name IS "int" or element_name IS "int32") {
            field.Type = FD_INT; field.NativeType = NativeStructType::Int32;
         }
         else if (element_name IS "uint" or element_name IS "uint32") {
            field.Type = FD_INT|FD_UNSIGNED; field.NativeType = NativeStructType::UInt32;
         }
         else if (element_name IS "int64") { field.Type = FD_INT64; field.NativeType = NativeStructType::Int64; }
         else if (element_name IS "uint64") {
            field.Type = FD_INT64|FD_UNSIGNED; field.NativeType = NativeStructType::UInt64;
         }
         else if (element_name IS "float") { field.Type = FD_FLOAT; field.NativeType = NativeStructType::Float; }
         else if (element_name IS "double") { field.Type = FD_DOUBLE; field.NativeType = NativeStructType::Double; }
         else if (element_name IS "string") {
            field.Type = FD_STRING; field.NativeType = NativeStructType::String;
         }
         else if (element_name.starts_with("struct<") and element_name.ends_with(">")) {
            std::string_view reference_name = element_name.substr(7, element_name.size() - 8);
            auto referenced = find_struct(&this->ctx.lua(), reference_name);
            if (not referenced) {
               return this->fail<StmtNodePtr>(ParserErrorCode::UnknownTypeName, type_token,
                  std::format("Unknown struct name '{}'; declarations must precede use", reference_name));
            }
            field.Type = FD_STRUCT;
            field.NativeType = NativeStructType::Struct;
            field.StructRef = struct_key(reference_name);
            field.StructDefinition = referenced;
         }
         else if (element_name IS "array") {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, type_token,
               "Nested array fields are not supported");
         }
         else if (element_name IS "cstr" or element_name IS "ptr" or element_name IS "obj" or
               element_name IS "func") {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, type_token,
               std::format("array<{}> fields cannot own this element type", element_name));
         }
         else {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnknownTypeName, type_token,
               std::format("Unknown array element type '{}'", element_name));
         }

         field.Type |= FD_CPP|FD_ARRAY;
         field.ArraySize = 1;
         type_display = std::format("array<{}>", element_name);
      }
      else if (struct_typed) {
         // Embedded struct reference in the single-token struct<Name> form
         std::string_view reference_name(strdata(type_symbol), type_symbol->len);
         auto referenced = find_struct(&this->ctx.lua(), reference_name);
         if (not referenced) {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnknownTypeName, type_token,
               std::format("Unknown struct name '{}'; declarations must precede use", reference_name));
         }
         field.Type = FD_STRUCT;
         field.NativeType = NativeStructType::Struct;
         field.StructRef = struct_key(reference_name);
         field.StructDefinition = referenced;
         type_display = std::format("struct<{}>", reference_name);
      }
      else if (type_name IS "bool") { field.Type = FD_BYTE; field.NativeType = NativeStructType::Bool; }
      else if (type_name IS "char") { field.Type = FD_BYTE|FD_CUSTOM; field.NativeType = NativeStructType::Char; }
      else if (type_name IS "byte" or type_name IS "uint8") {
         field.Type = FD_BYTE; field.NativeType = NativeStructType::UInt8;
      }
      else if (type_name IS "int8") { field.Type = FD_BYTE; field.NativeType = NativeStructType::Int8; }
      else if (type_name IS "int16") { field.Type = FD_WORD; field.NativeType = NativeStructType::Int16; }
      else if (type_name IS "uint16") {
         field.Type = FD_WORD|FD_UNSIGNED; field.NativeType = NativeStructType::UInt16;
      }
      else if (type_name IS "int" or type_name IS "int32") {
         field.Type = FD_INT; field.NativeType = NativeStructType::Int32;
      }
      else if (type_name IS "uint" or type_name IS "uint32") {
         field.Type = FD_INT|FD_UNSIGNED; field.NativeType = NativeStructType::UInt32;
      }
      else if (type_name IS "int64") { field.Type = FD_INT64; field.NativeType = NativeStructType::Int64; }
      else if (type_name IS "uint64") {
         field.Type = FD_INT64|FD_UNSIGNED; field.NativeType = NativeStructType::UInt64;
      }
      else if (type_name IS "float") { field.Type = FD_FLOAT; field.NativeType = NativeStructType::Float; }
      else if (type_name IS "double") { field.Type = FD_DOUBLE; field.NativeType = NativeStructType::Double; }
      else if (type_name IS "string") {
         field.Type = FD_STRING|FD_CPP; field.NativeType = NativeStructType::String;
      }
      else if (type_name IS "cstr") { field.Type = FD_STRING; field.NativeType = NativeStructType::CStr; }
      else if (type_name IS "func") { field.Type = FD_FUNCTION; field.NativeType = NativeStructType::Function; }
      else if (type_name IS "obj") {
         field.Type = FD_OBJECT; field.NativeType = NativeStructType::Object;
         if (this->ctx.match(TokenKind::Less).ok()) {
            auto class_name = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
            if (not class_name.ok()) return ParserResult<StmtNodePtr>::failure(class_name.error_ref());
            GCstr *class_symbol = class_name.value_ref().identifier();
            std::string_view class_view(strdata(class_symbol), class_symbol->len);
            field.ObjectClassID = CLASSID(kt::strihash(class_view));
            type_display = std::format("obj<{}>", class_view);
            auto close = this->ctx.consume(TokenKind::Greater, ParserErrorCode::ExpectedToken);
            if (not close.ok()) return ParserResult<StmtNodePtr>::failure(close.error_ref());
         }
      }
      else if (type_name IS "ptr" or type_name IS "struct") {
         bool is_pointer = type_name IS "ptr";
         field.Type = is_pointer ? FD_POINTER : FD_STRUCT;
         field.NativeType = is_pointer ? NativeStructType::Pointer : NativeStructType::Struct;
         if (this->ctx.match(TokenKind::Less).ok()) {
            auto reference = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
            if (not reference.ok()) return ParserResult<StmtNodePtr>::failure(reference.error_ref());
            GCstr *reference_symbol = reference.value_ref().identifier();
            std::string_view reference_name(strdata(reference_symbol), reference_symbol->len);
            auto referenced = find_struct(&this->ctx.lua(), reference_name);
            if (referenced) {
               field.StructRef = struct_key(reference_name);
               field.StructDefinition = referenced;
               field.Type |= FD_STRUCT;
            }
            else if (is_pointer) {
               if (reference_name IS "bool" or reference_name IS "char" or reference_name IS "byte" or
                     reference_name IS "int8" or reference_name IS "uint8") field.Type |= FD_BYTE;
               else if (reference_name IS "int16" or reference_name IS "uint16") field.Type |= FD_WORD;
               else if (reference_name IS "int" or reference_name IS "uint" or reference_name IS "int32" or
                     reference_name IS "uint32") field.Type |= FD_INT;
               else if (reference_name IS "int64" or reference_name IS "uint64") field.Type |= FD_INT64;
               else if (reference_name IS "float") field.Type |= FD_FLOAT;
               else if (reference_name IS "double") field.Type |= FD_DOUBLE;
               else {
                  return this->fail<StmtNodePtr>(ParserErrorCode::UnknownTypeName, reference.value_ref(),
                     std::format("Unknown struct or scalar name '{}'", reference_name));
               }
               if (reference_name.starts_with("uint") or reference_name IS "byte") field.Type |= FD_UNSIGNED;
            }
            else {
               return this->fail<StmtNodePtr>(ParserErrorCode::UnknownTypeName, reference.value_ref(),
                  std::format("Unknown struct name '{}'; declarations must precede use", reference_name));
            }
            type_display = std::format("{}<{}", type_name, reference_name);
            // Handling for ptr<Struct[]>
            if (is_pointer and this->ctx.match(TokenKind::LeftBracket).ok()) {
               auto empty = this->ctx.consume(TokenKind::RightBracket, ParserErrorCode::ExpectedToken);
               if (not empty.ok()) return ParserResult<StmtNodePtr>::failure(empty.error_ref());
               field.Type |= FD_ARRAY;
               field.ArraySize = -1;
               type_display += "[]";
            }
            auto close = this->ctx.consume(TokenKind::Greater, ParserErrorCode::ExpectedToken);
            if (not close.ok()) return ParserResult<StmtNodePtr>::failure(close.error_ref());
            type_display += ">";
         }
         else if (not is_pointer) {
            return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, type_token,
               "struct fields require a referenced name, for example struct<User>");
         }
      }
      else {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnknownTypeName, type_token,
            std::format("Unknown struct field type '{}'", type_name));
      }

      if (type_display.empty()) type_display.assign(type_name);

      if (this->ctx.match(TokenKind::LeftBracket).ok()) {
         Token dimension = this->ctx.tokens().current();
         if (dynamic_array) {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, dimension,
               "Dynamically sized array fields cannot have a fixed dimension");
         }
         lua_Number dimension_value = dimension.payload().as_number();
         if (dimension.kind() != TokenKind::Number or dimension_value < 1 or
               dimension_value > lua_Number(std::numeric_limits<int>::max()) or
               dimension_value != lua_Number(int64_t(dimension_value))) {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, dimension,
               "Struct array dimensions must be positive integers within the supported range");
         }
         if ((field.Type & FD_STRING) and (field.Type & FD_CPP)) {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, dimension,
               "Fixed arrays of owned strings are not supported");
         }
         field.Type |= FD_ARRAY;
         field.ArraySize = int(dimension_value);
         type_display += std::format("[{}]", int(dimension_value));
         this->ctx.tokens().advance();
         auto close = this->ctx.consume(TokenKind::RightBracket, ParserErrorCode::ExpectedToken);
         if (not close.ok()) return ParserResult<StmtNodePtr>::failure(close.error_ref());
      }

      field.precomputeNameHash();
      record.Fields.push_back(std::move(field));

      auto documentation = this->ctx.lex().documentation_for_line(
         field_token.value_ref().span().line.lineNumber());
      if (collect_meta) {
         meta.fields.push_back({ field_name, std::move(type_display), documentation,
            field_token.value_ref().span() });
      }
      if (not documentation.empty()) {
         this->ctx.lex().struct_field_documentation.push_back({ struct_name, field_name,
            std::move(documentation), field_token.value_ref().span() });
      }

      Token last = this->ctx.tokens().current();
      if (this->ctx.match(TokenKind::Comma).ok()) continue;
      if (this->ctx.check(TokenKind::EndToken)) continue;
      if (last.span().line.lineNumber() <= type_token.span().line.lineNumber()) {
         return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, last,
            "Expected a newline, ',' or 'end' after struct field");
      }
   }

   auto close = this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);
   if (not close.ok()) return ParserResult<StmtNodePtr>::failure(close.error_ref());
   if (record.Fields.empty()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, struct_token,
         "Struct declarations must contain at least one field");
   }

   bool inserted = false;
   const struct_record *existing = nullptr;
   std::string registration_detail;
   ERR registration = register_declared_struct(&this->ctx.lua(), std::move(record), &inserted, &existing,
      &registration_detail);
   if (registration != ERR::Okay) {
      if (registration != ERR::Exists) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, name_token.value_ref(),
            registration_detail.empty() ? std::format("Failed to lay out struct '{}': {}", struct_name,
               GetErrorMsg(registration)) : registration_detail);
      }
      std::string location = (existing and not existing->DeclarationSource.empty()) ?
         std::format(" at {}:{}", existing->DeclarationSource, existing->DeclarationLine) : " from native code";
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, name_token.value_ref(),
         std::format("Struct '{}' conflicts with its previous declaration{}", struct_name, location));
   }
   if (inserted) this->track_registered_struct(struct_key(struct_name));

   if (collect_meta) {
      meta.end_span = close.value_ref().span();
      this->ctx.lex().struct_declaration_metadata.push_back(std::move(meta));
   }

   // Declarations only register the layout.  Construction is explicit via struct<Name> { ... }, so no
   // constructor variable is bound and the declaration itself emits no bytecode.
   auto stmt = std::make_unique<StmtNode>(AstNodeKind::DoStmt, struct_token.span());
   stmt->data = DoStmtPayload(make_block(struct_token.span(), {}));
   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

static bool is_prefixed_attribute_token(TokenStreamAdapter &Tokens)
{
   Token current = Tokens.current();
   if (current.raw() IS '<') return true;

   if (current.kind() != TokenKind::Identifier) return false;
   GCstr *symbol = current.identifier();
   if (not symbol) return false;

   std::string_view name(strdata(symbol), symbol->len);
   if (name != "const" and name != "close") return false;
   return Tokens.peek(1).raw() IS '<';
}

ParserResult<int64_t> AstBuilder::parse_enum_integer_literal()
{
   int sign = 1;
   Token token = this->ctx.tokens().current();
   if (token.kind() IS TokenKind::Plus or token.kind() IS TokenKind::Minus) {
      if (token.kind() IS TokenKind::Minus) sign = -1;
      this->ctx.tokens().advance();
      token = this->ctx.tokens().current();
   }

   if (not token.is(TokenKind::Number)) {
      return this->fail<int64_t>(ParserErrorCode::UnexpectedToken, token,
         "Enum values must be integer literals");
   }

   double number_value = token.payload().as_number();
   if (not std::isfinite(number_value) or std::trunc(number_value) != number_value) {
      return this->fail<int64_t>(ParserErrorCode::UnexpectedToken, token,
         "Enum values must be integer literals");
   }

   double signed_value = number_value * double(sign);
   constexpr double min_value = double(std::numeric_limits<int64_t>::min());
   constexpr double max_value = double(std::numeric_limits<int64_t>::max());
   if (signed_value < min_value or signed_value > max_value) {
      return this->fail<int64_t>(ParserErrorCode::UnexpectedToken, token,
         "Enum integer literal is out of range");
   }

   this->ctx.tokens().advance();
   return ParserResult<int64_t>::success(int64_t(signed_value));
}

ParserResult<StmtNodePtr> AstBuilder::parse_enum(const Token &StartToken)
{
   if (not this->at_top_level()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "Enum declarations are only allowed at top-level scope");
   }

   Token enum_token = this->ctx.tokens().current();
   auto enum_result = this->ctx.consume(TokenKind::Enum, ParserErrorCode::ExpectedToken);
   if (not enum_result.ok()) return ParserResult<StmtNodePtr>::failure(enum_result.error_ref());

   auto prefix_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
   if (not prefix_token.ok()) return ParserResult<StmtNodePtr>::failure(prefix_token.error_ref());

   GCstr *prefix_symbol = prefix_token.value_ref().identifier();
   if (not is_uppercase_enum_name(prefix_symbol)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, prefix_token.value_ref(),
         "Declare enum prefixes in uppercase only");
   }

   if (this->ctx.check(TokenKind::Colon)) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "Enum prefixes cannot use a type annotation");
   }

   if (is_prefixed_attribute_token(this->ctx.tokens())) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "Enum prefixes cannot use <const> or <close> attributes");
   }

   auto open_brace = this->ctx.consume(TokenKind::LeftBrace, ParserErrorCode::ExpectedToken);
   if (not open_brace.ok()) return ParserResult<StmtNodePtr>::failure(open_brace.error_ref());

   std::vector<EnumConstantDecl> constants;
   std::vector<std::string> members;
   int64_t next_value = 0;
   std::string prefix(strdata(prefix_symbol), prefix_symbol->len);

   while (not this->ctx.check(TokenKind::RightBrace)) {
      if (this->ctx.check(TokenKind::EndOfFile)) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedEndOfFile, enum_token,
            "Unterminated enum declaration");
      }

      auto member_token = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not member_token.ok()) return ParserResult<StmtNodePtr>::failure(member_token.error_ref());

      GCstr *member_symbol = member_token.value_ref().identifier();
      if (not is_uppercase_enum_name(member_symbol)) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, member_token.value_ref(),
            "Declare enum members in uppercase only");
      }

      std::string member(strdata(member_symbol), member_symbol->len);
      for (const std::string &existing : members) {
         if (existing IS member) {
            return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, member_token.value_ref(),
               "Duplicate enum member '" + member + "'");
         }
      }
      members.push_back(member);

      int64_t value = next_value;
      if (this->ctx.match(TokenKind::Equals).ok()) {
         auto explicit_value = this->parse_enum_integer_literal();
         if (not explicit_value.ok()) return ParserResult<StmtNodePtr>::failure(explicit_value.error_ref());
         value = explicit_value.value_ref();
      }

      constants.push_back(EnumConstantDecl{ prefix + "_" + member, value, member_token.value_ref().span() });
      if (value IS std::numeric_limits<int64_t>::max()) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, member_token.value_ref(),
            "Enum value overflow");
      }
      next_value = value + 1;

      if (this->ctx.match(TokenKind::Comma).ok()) {
         continue;
      }
      if (not this->ctx.check(TokenKind::RightBrace)) {
         return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, this->ctx.tokens().current(),
            "Expected ',' or '}' in enum declaration");
      }
   }

   auto close_brace = this->ctx.consume(TokenKind::RightBrace, ParserErrorCode::ExpectedToken);
   if (not close_brace.ok()) return ParserResult<StmtNodePtr>::failure(close_brace.error_ref());

   if (constants.empty()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, StartToken,
         "Enum declarations must contain at least one member");
   }

   std::string duplicate_name;
   SourceSpan duplicate_span{};

   {
      std::unique_lock lock(glConstantMutex);
      for (const EnumConstantDecl &constant : constants) {
         uint32_t hash = kt::strhash(constant.name);
         if (glConstantRegistry.contains(hash)) {
            duplicate_name = constant.name;
            duplicate_span = constant.span;
            break;
         }
      }

      if (duplicate_name.empty()) {
         for (const EnumConstantDecl &constant : constants) {
            uint32_t hash = kt::strhash(constant.name);
            glConstantRegistry.emplace(hash, TiriConstant(constant.value));
            this->track_registered_enum_constant(hash);
         }
      }
   }

   if (not duplicate_name.empty()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken,
         Token::from_span(duplicate_span, TokenKind::Identifier),
         "Duplicate enum constant '" + duplicate_name + "'");
   }

   return ParserResult<StmtNodePtr>::success(nullptr);
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
            "Thunk functions do not support method syntax");
      }
      method = true;
      auto seg = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not seg.ok()) {
         return ParserResult<StmtNodePtr>::failure(seg.error_ref());
      }
      path.method = make_identifier(seg.value_ref());
   }

   GCstr *funcname = nullptr;
   if (path.method.has_value() and path.method->symbol) {
      funcname = path.method->symbol;
   }
   else if (not path.segments.empty()) {
      funcname = path.segments.back().symbol;
   }

   auto fn = this->parse_function_literal(func_token, is_thunk, funcname);
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
   BlockDepthScope block_scope(*this);
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
   BlockDepthScope block_scope(*this);
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
   BlockDepthScope block_scope(*this);
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
   BlockDepthScope block_scope(*this);
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
               attribute.value_ref(), "Unknown try attribute, expected 'trace'");
         }
      }

      if (not this->ctx.lex_opt('>')) {
         return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken,
            this->ctx.tokens().current(), "Expected '>' after try attribute");
      }
   }

   // Parse try block body - terminates on 'except', 'success', or 'end'
   const TokenKind try_terms[] = { TokenKind::ExceptToken, TokenKind::SuccessToken, TokenKind::EndToken };
   std::unique_ptr<BlockStmt> try_block;
   {
      BlockDepthScope block_scope(*this);
      auto try_body = this->parse_block(try_terms);
      if (not try_body.ok()) return ParserResult<StmtNodePtr>::failure(try_body.error_ref());
      try_block = std::move(try_body.value_ref());
   }

   std::vector<ExceptClause> clauses;
   bool has_catch_all = false;

   // Parse except clauses
   while (this->ctx.check(TokenKind::ExceptToken)) {
      if (has_catch_all) {
         // Error: catch-all must be last
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
            "Catch-all 'except' must be the last clause");
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
               "Expected 'when' or newline after 'except', not '" + std::string(strdata(ident), ident->len) + "'");
         }
      }

      if (this->ctx.check(TokenKind::When)) {
         Token when_token = this->ctx.tokens().current();
         this->ctx.tokens().advance();  // consume 'when'

         // Filter code(s) must be on the same line as 'when'
         Token next_token = this->ctx.tokens().current();
         if (next_token.span().line != when_token.span().line) {
            return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, when_token,
               "Expected error code(s) after 'when' on the same line");
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
                  "Expected error code after ',' on the same line as 'when'");
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
      {
         BlockDepthScope block_scope(*this);
         auto except_body = this->parse_block(except_terms);
         if (not except_body.ok()) return ParserResult<StmtNodePtr>::failure(except_body.error_ref());
         clause.block = std::move(except_body.value_ref());
      }

      clauses.push_back(std::move(clause));
   }

   // Parse optional success clause
   std::unique_ptr<BlockStmt> success_block;
   if (this->ctx.check(TokenKind::SuccessToken)) {
      this->ctx.tokens().advance();  // consume 'success'

      // Parse success block body - terminates on 'end'
      const TokenKind success_terms[] = { TokenKind::EndToken };
      {
         BlockDepthScope block_scope(*this);
         auto success_body = this->parse_block(success_terms);
         if (not success_body.ok()) return ParserResult<StmtNodePtr>::failure(success_body.error_ref());
         success_block = std::move(success_body.value_ref());
      }
   }

   this->ctx.consume(TokenKind::EndToken, ParserErrorCode::ExpectedToken);

   // Build the statement
   auto stmt = std::make_unique<StmtNode>(AstNodeKind::TryExceptStmt, try_token.span());
   TryExceptPayload payload;
   payload.try_block = std::move(try_block);
   payload.except_clauses = std::move(clauses);
   payload.success_block = std::move(success_block);
   payload.enable_trace = enable_trace;
   stmt->data = std::move(payload);

   return ParserResult<StmtNodePtr>::success(std::move(stmt));
}

//********************************************************************************************************************
// Parses raise statements: raise expression [, message]
//
// The first expression is normally the error code.  If it evaluates to a string and no comma message is supplied, the
// emitter treats it as the custom message and lets the runtime default the error code to ERR::Exception.

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
// Validates an include module name before load_include() touches the module loader.

static bool include_module_name_is_valid(std::string_view Module)
{
   if (Module.empty() or Module.size() >= 32) return false;

   for (char c : Module) {
      if ((c >= 'a') and (c <= 'z')) continue;
      if ((c >= 'A') and (c <= 'Z')) continue;
      if ((c >= '0') and (c <= '9')) continue;
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Parses include statements: include 'module', 'module2'
//
// Include is a compile-time statement.  It loads module constants and struct definitions while parsing and emits no
// runtime bytecode.

ParserResult<StmtNodePtr> AstBuilder::parse_include_stmt()
{
   Token include_token = this->ctx.tokens().current();
   this->ctx.tokens().advance();  // consume 'include'

   bool first_item = true;

   while (true) {
      Token name_token = this->ctx.tokens().current();
      if (not name_token.is(TokenKind::String)) {
         const char *message = first_item ? "Include module name must be a string literal" :
            "'include' list items must be string literals";
         return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, name_token, message);
      }

      GCstr *name_str = name_token.payload().as_string();
      if (not name_str) {
         return this->fail<StmtNodePtr>(ParserErrorCode::ExpectedToken, name_token, "Invalid include module name");
      }

      std::string module_name(strdata(name_str), name_str->len);
      if (not include_module_name_is_valid(module_name)) {
         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, name_token,
            "Invalid module name; only alpha-numeric names shorter than 32 characters are permitted with include");
      }

      if (auto error = load_include(this->ctx.lua().script, module_name); error != ERR::Okay) {
         std::string message;
         if (error IS ERR::FileNotFound) message = std::format("Requested include file '{}' does not exist", module_name);
         else message = std::format("Failed to process include file '{}': {}", module_name, GetErrorMsg(error));

         return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, name_token, std::move(message));
      }

      this->ctx.tokens().advance();  // consume module string
      first_item = false;

      if (not this->ctx.match(TokenKind::Comma).ok()) break;
   }

   (void)include_token;
   return ParserResult<StmtNodePtr>::success(nullptr);
}

//********************************************************************************************************************
// Parses one import list entry.
//
// The import statement is a compile-time feature that reads and parses the referenced file, inlining its content as
// statements executed within the current scope.
//
// When using 'as alias' syntax, the imported library must declare a namespace. The alias creates a local const variable
// that references _LIB['namespace'] for convenient access to the library exports.

ParserResult<ImportEntryPayload> AstBuilder::parse_import_entry(const Token &ImportToken, bool AllowAlias,
   bool *UsedAlias)
{
   kt::Log log(__FUNCTION__);

   if (UsedAlias) *UsedAlias = false;

   // Require a string literal for the library path

   Token path_token = this->ctx.tokens().current();
   if (not path_token.is(TokenKind::String)) {
      const char *message = AllowAlias ? "Import path must be a string literal" :
         "Import list items must be string literals";
      return this->fail<ImportEntryPayload>(ParserErrorCode::ExpectedToken, path_token, message);
   }

   GCstr *path_str = path_token.payload().as_string();
   if (not path_str) {
      return this->fail<ImportEntryPayload>(ParserErrorCode::ExpectedToken, path_token, "Invalid import path");
   }

   std::string_view mod_name(strdata(path_str), path_str->len);
   this->ctx.tokens().advance();  // consume string

   log.traceBranch("Library: %.*s", int(mod_name.size()), mod_name.data());

   // Check for 'as' alias syntax

   std::optional<Identifier> alias;
   Token as_token;
   if (this->ctx.check(TokenKind::AsToken)) {
      as_token = this->ctx.tokens().current();

      if (not AllowAlias) {
         return this->fail<ImportEntryPayload>(ParserErrorCode::UnexpectedToken, as_token,
            "Import lists do not support 'as' aliases");
      }

      this->ctx.tokens().advance();  // consume 'as'

      auto alias_result = this->ctx.expect_identifier(ParserErrorCode::ExpectedIdentifier);
      if (not alias_result.ok()) return ParserResult<ImportEntryPayload>::failure(alias_result.error_ref());

      alias = make_identifier(alias_result.value_ref());
      alias->has_const = true;  // Namespace alias is const
      if (UsedAlias) *UsedAlias = true;
   }

   std::string path = this->ctx.resolve_lib_to_path(mod_name);

   // Check for circular import

   if (this->ctx.is_importing(path)) {
      return this->fail<ImportEntryPayload>(ParserErrorCode::UnexpectedToken, ImportToken,
         "Circular import detected: " + path);
   }

   // Parse the imported file

   auto imported_body = this->parse_imported_file(path, mod_name, ImportToken);
   if (not imported_body.ok()) return ParserResult<ImportEntryPayload>::failure(imported_body.error_ref());

   // Look up the FileSource index and namespace for this import (registered during parse_imported_file)

   lua_State *L = &this->ctx.lua();
   auto file_idx = find_file_source(L, kt::strihash(path));
   std::string default_ns;

   if (file_idx.has_value()) {
      const FileSource *source = get_file_source(L, file_idx.value());
      if (source) default_ns = source->declared_namespace;
   }

   // If using 'as' alias, the library must declare a namespace

   if (alias and default_ns.empty()) {
      return this->fail<ImportEntryPayload>(ParserErrorCode::UnexpectedToken, as_token,
         std::string("Cannot use 'as' alias: library '") + std::string(mod_name) + "' does not declare a namespace");
   }

   // Determine final namespace name (alias takes precedence)

   std::string final_ns;
   if (alias) final_ns = std::string(strdata(alias->symbol), alias->symbol->len);
   else if (not default_ns.empty()) final_ns = default_ns;

   ImportEntryPayload entry;
   entry.lib_path = path;
   entry.inlined_body = std::move(imported_body.value_ref());

   if (file_idx.has_value()) {
      entry.file_source_idx = file_idx.value();
   }

   // If we have a namespace (either from alias or default), set up the namespace binding
   if (not final_ns.empty()) {
      Identifier ns_id;
      ns_id.symbol = lj_str_new(L, final_ns.c_str(), final_ns.size());
      ns_id.span = ImportToken.span();
      ns_id.has_const = true;
      entry.namespace_name = std::move(ns_id);
      entry.default_namespace = default_ns;  // Store original for _LIB lookup
   }

   return ParserResult<ImportEntryPayload>::success(std::move(entry));
}

//********************************************************************************************************************
// Parses import statements: import 'library' [, 'library'...] [as alias for single imports only]

ParserResult<StmtNodePtr> AstBuilder::parse_import()
{
   Token import_token = this->ctx.tokens().current();

   // Import statements must be at the top level of the script

   if (not this->at_top_level()) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, import_token,
         "Use of 'import' is not permitted inside function blocks");
   }

   this->ctx.tokens().advance();  // consume 'import'

   ImportStmtPayload payload;

   bool first_used_alias = false;
   auto first_entry = this->parse_import_entry(import_token, true, &first_used_alias);
   if (not first_entry.ok()) return ParserResult<StmtNodePtr>::failure(first_entry.error_ref());
   payload.entries.push_back(std::move(first_entry.value_ref()));

   if (this->ctx.check(TokenKind::Comma) and first_used_alias) {
      return this->fail<StmtNodePtr>(ParserErrorCode::UnexpectedToken, this->ctx.tokens().current(),
         "Import lists do not support 'as' aliases");
   }

   while (this->ctx.match(TokenKind::Comma).ok()) {
      auto entry = this->parse_import_entry(import_token, false);
      if (not entry.ok()) return ParserResult<StmtNodePtr>::failure(entry.error_ref());
      payload.entries.push_back(std::move(entry.value_ref()));
   }

   auto stmt = std::make_unique<StmtNode>(AstNodeKind::ImportStmt, import_token.span());
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
   kt::Log log(__FUNCTION__);

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
         "Namespace name must be a string literal");
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
   kt::Log log(__FUNCTION__);

   lua_State *L = &this->ctx.lua();

   std::string resolved_path;
   if (!ResolvePath(Path, RSF::NO_FILE_CHECK, &resolved_path)) {
      Path = resolved_path;
   }

   auto libhash = kt::strihash(Path);

   // Check if this file is already registered in FileSource.  FileSource entries persist for the lifetime of the
   // lua_State, so this hit can come from an earlier, unrelated compilation.  In diagnose mode the type analyser
   // needs the real body regardless - validation never emits or executes code, so re-parsing cannot double-execute
   // library code, and skipping here would silently drop imported-file diagnostics on every validation after the
   // first.  The existing FileSource index is reused below instead of registering a duplicate.
   auto existing_index = find_file_source(L, libhash);
   bool seen_this_chunk = this->import_seen_this_chunk(libhash);
   if (existing_index.has_value()) {
      if (seen_this_chunk or not this->ctx.lex().diagnose_mode) {
         log.detail("Library %.*s already imported (file index %d)", int(Library.size()), Library.data(), existing_index.value());
         return ParserResult<std::unique_ptr<BlockStmt>>::success(make_block(ImportToken.span(), {}));
      }
      log.branch("Re-parsing '%.*s' for diagnostics (file index %d)", int(Library.size()), Library.data(), existing_index.value());
   }
   else log.branch("Importing '%.*s' from %s", int(Library.size()), Library.data(), Path.c_str());

   // Push this file onto the import stack to detect circular imports
   this->ctx.push_import(Path);

   // Read the file contents using Kotuku File API
   objFile::create file = { fl::Path(Path), fl::Flags(FL::READ) };
   if (not file.ok()) {
      this->ctx.pop_import();
      return this->fail<std::unique_ptr<BlockStmt>>(ParserErrorCode::UnexpectedToken, ImportToken,
         "Cannot open imported file: " + Path);
   }

   // Get file size and read contents
   int64_t file_size = 0;
   if (file->getSize(file_size) != ERR::Okay or file_size <= 0) {
      this->ctx.pop_import();
      // Empty file - return an empty block
      return ParserResult<std::unique_ptr<BlockStmt>>::success(make_block(ImportToken.span(), {}));
   }

   std::string source;
   source.resize(size_t(file_size));
   int bytes_read = 0;
   ERR err = file->read(std::span<int8_t>((int8_t *)source.data(), size_t(file_size)), &bytes_read);

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

   // Register this imported file with FileSource tracking, or reuse the existing entry when re-parsing a cached
   // import in diagnose mode.

   uint8_t new_file_index = existing_index.has_value() ? existing_index.value()
      : register_file_source(L, Path, filename, 1, source_lines, parent_index, import_line);

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

   // Create a temporary parser context for the imported file
   ParserContext import_ctx(*import_lex, fs, *L, ParserAllocator::from(L), this->ctx.config());
   import_ctx.set_error_rollback_callback(rollback_ast_builder_constants, this);

   // Copy the import stack to the child context for circular detection
   for (const auto& imported_path : this->ctx.import_stack()) {
      import_ctx.push_import(imported_path);
   }

   import_lex->next(); // Prime the lexer

   // Parse up to EOF
   AstBuilder import_builder(import_ctx, this);
   const TokenKind terms[] = { TokenKind::EndOfFile };
   auto result = import_builder.parse_block(terms);

   fs.ls = saved_ls; // Restore the parent FuncState's lexer reference
   // import_guard destructor handles cleanup

   this->ctx.pop_import();

   // Merge the child context's diagnostics into the parent so that diagnose-mode errors and warnings collected
   // while parsing the imported file survive with their own file_index (the child collector is discarded below).

   for (const auto &entry : import_ctx.diagnostics().entries()) {
      this->ctx.diagnostics().report(entry);
   }

   if (not result.ok()) {
      // Prepend import context to error message
      ParserError error = result.error_ref();
      error.message = "in imported file '" + Path + "': " + error.message;
      return ParserResult<std::unique_ptr<BlockStmt>>::failure(error);
   }

   this->adopt_registered_enum_constants(import_builder);
   this->adopt_registered_structs(import_builder);

   return result;
}

//********************************************************************************************************************
// Skips tokens until a matching @end is found, handling nested @if/@end blocks.  Called when the @if condition
// evaluates to false.

void AstBuilder::skip_to_compile_end()
{
   kt::Log log(__FUNCTION__);

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
   kt::Log log(__FUNCTION__);

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
      condition_result = kt::iequals(current_platform, string_value);
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
            std::string_view working_path;
            this->ctx.lua().script->getWorkingPath(working_path);
            if (not working_path.empty()) check_path = std::string(working_path) + std::string(string_value);
            else check_path = std::string(string_value);
         }
      }
      else {
         std::string_view working_path;
         this->ctx.lua().script->getWorkingPath(working_path);
         if (not working_path.empty()) check_path = std::string(working_path) + std::string(string_value);
         else check_path = std::string(string_value);
      }

      condition_result = (!AnalysePath(check_path, nullptr));
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
