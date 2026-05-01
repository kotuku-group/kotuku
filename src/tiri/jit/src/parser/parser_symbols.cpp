// Parser symbol metadata collection for validation and LSP tooling.
// Copyright © 2025-2026 Paul Manias

#include "parser_symbols.h"

#include <cctype>
#include <string_view>

#include "ast/nodes.h"
#include "runtime/lj_obj.h"
#include "../../../defs.h"

enum class DocSection : uint8_t {
   Description,
   Input,
   Results,
   Errors,
   Example
};

struct ParsedDocText {
   ParserDocBlockMetadata block;
   std::vector<std::pair<std::string, std::string>> inputs;
   std::vector<ParserDocReturnMetadata> results;
   std::vector<ParserDocErrorMetadata> errors;
};

static std::string gcstr_to_string(GCstr *Value)
{
   if (Value IS nullptr) return {};
   return std::string(strdata(Value), Value->len);
}

static std::string type_to_string(TiriType Type)
{
   if (Type IS TiriType::Unknown) return {};
   return std::string(type_name(Type));
}

static std::string trim_left(std::string_view Text)
{
   size_t pos = 0;
   while (pos < Text.size() and std::isspace((unsigned char)Text[pos])) pos++;
   return std::string(Text.substr(pos));
}

static std::string trim_right(std::string_view Text)
{
   size_t end = Text.size();
   while (end > 0 and std::isspace((unsigned char)Text[end - 1])) end--;
   return std::string(Text.substr(0, end));
}

static std::string trim(std::string_view Text)
{
   return trim_right(trim_left(Text));
}

static std::vector<std::string> normalise_doc_lines(std::string_view Text)
{
   std::vector<std::string> lines;
   size_t start = 0;

   while (start <= Text.size()) {
      size_t end = Text.find('\n', start);
      if (end IS std::string_view::npos) end = Text.size();

      std::string_view line = Text.substr(start, end - start);
      if (not line.empty() and line.back() IS '\r') line.remove_suffix(1);
      lines.push_back(trim_left(line));

      if (end >= Text.size()) break;
      start = end + 1;
   }

   return lines;
}

static std::string join_lines(const std::vector<std::string> &Lines)
{
   size_t first = 0;
   size_t last = Lines.size();

   while (first < last and trim(Lines[first]).empty()) first++;
   while (last > first and trim(Lines[last - 1]).empty()) last--;

   std::string result;
   for (size_t i = first; i < last; ++i) {
      if (i > first) result += "\n";
      result += Lines[i];
   }
   return result;
}

static void parse_name_doc_line(std::string_view Line, std::string &Name, std::string &Doc)
{
   size_t colon = Line.find(':');
   if (colon IS std::string_view::npos) {
      Name = trim(Line);
      Doc.clear();
      return;
   }

   Name = trim(Line.substr(0, colon));
   Doc = trim(Line.substr(colon + 1));
}

static bool doc_uses_compact_form(const std::vector<std::string> &Lines)
{
   for (const std::string &line : Lines) {
      std::string trimmed = trim(line);
      if (trimmed.empty()) continue;
      return trimmed.front() IS '@';
   }

   return false;
}

static std::string compact_doc_payload(std::string_view Line, std::string_view Tag)
{
   if (Line.size() < Tag.size()) return {};
   if (Line.substr(0, Tag.size()) != Tag) return {};
   if (Line.size() > Tag.size() and not std::isspace((unsigned char)Line[Tag.size()])) return {};
   return trim(Line.substr(Tag.size()));
}

static ParsedDocText parse_compact_doc_text(std::string_view Text, const std::vector<std::string> &Lines)
{
   ParsedDocText parsed;
   parsed.block.raw = std::string(Text);

   for (const std::string &line : Lines) {
      std::string trimmed = trim(line);
      if (trimmed.empty()) continue;

      std::string payload = compact_doc_payload(trimmed, "@desc");
      if (not payload.empty()) {
         if (parsed.block.summary.empty()) parsed.block.summary = payload;
         if (parsed.block.body.empty()) parsed.block.body = payload;
         else parsed.block.body += "\n" + payload;
         continue;
      }

      payload = compact_doc_payload(trimmed, "@input");
      if (not payload.empty()) {
         std::string name;
         std::string doc;
         parse_name_doc_line(payload, name, doc);
         if (not name.empty()) parsed.inputs.emplace_back(std::move(name), std::move(doc));
         continue;
      }

      payload = compact_doc_payload(trimmed, "@result");
      if (not payload.empty()) {
         ParserDocReturnMetadata ret;
         parse_name_doc_line(payload, ret.type, ret.doc);
         parsed.results.push_back(std::move(ret));
         continue;
      }

      payload = compact_doc_payload(trimmed, "@error");
      if (not payload.empty()) {
         ParserDocErrorMetadata err;
         parse_name_doc_line(payload, err.code, err.doc);
         if (not err.code.empty()) parsed.errors.push_back(std::move(err));
      }
   }

   return parsed;
}

static ParsedDocText parse_marker_doc_text(std::string_view Text, const std::vector<std::string> &Lines)
{
   ParsedDocText parsed;
   parsed.block.raw = std::string(Text);

   std::vector<std::string> description_lines;
   std::vector<std::string> example_lines;
   DocSection section = DocSection::Description;

   auto flush_example = [&]() {
      if (not example_lines.empty()) {
         parsed.block.examples.push_back(join_lines(example_lines));
         example_lines.clear();
      }
   };

   for (const std::string &line : Lines) {
      std::string marker = trim(line);

      if (marker IS "-INPUT-" or marker IS "-RESULTS-" or marker IS "-ERRORS-" or marker IS "-EXAMPLE-") {
         if (section IS DocSection::Example) flush_example();

         if (marker IS "-INPUT-") section = DocSection::Input;
         else if (marker IS "-RESULTS-") section = DocSection::Results;
         else if (marker IS "-ERRORS-") section = DocSection::Errors;
         else section = DocSection::Example;

         continue;
      }

      switch (section) {
         case DocSection::Description:
            description_lines.push_back(line);
            break;
         case DocSection::Input: {
            if (marker.empty()) break;
            std::string name;
            std::string doc;
            parse_name_doc_line(line, name, doc);
            if (not name.empty()) parsed.inputs.emplace_back(std::move(name), std::move(doc));
            break;
         }
         case DocSection::Results: {
            if (marker.empty()) break;
            ParserDocReturnMetadata ret;
            parse_name_doc_line(line, ret.type, ret.doc);
            parsed.results.push_back(std::move(ret));
            break;
         }
         case DocSection::Errors: {
            if (marker.empty()) break;
            ParserDocErrorMetadata err;
            parse_name_doc_line(line, err.code, err.doc);
            if (not err.code.empty()) parsed.errors.push_back(std::move(err));
            break;
         }
         case DocSection::Example:
            example_lines.push_back(line);
            break;
      }
   }

   if (section IS DocSection::Example) flush_example();

   parsed.block.body = join_lines(description_lines);
   for (const std::string &line : description_lines) {
      std::string summary = trim(line);
      if (not summary.empty()) {
         parsed.block.summary = std::move(summary);
         break;
      }
   }

   return parsed;
}

static ParsedDocText parse_doc_text(std::string_view Text)
{
   std::vector<std::string> lines = normalise_doc_lines(Text);
   if (doc_uses_compact_form(lines)) return parse_compact_doc_text(Text, lines);
   return parse_marker_doc_text(Text, lines);
}

static bool annotation_name_is(const AnnotationEntry &Annotation, std::string_view Name)
{
   if (Annotation.name IS nullptr) return false;
   return std::string_view(strdata(Annotation.name), Annotation.name->len) IS Name;
}

static std::string annotation_value_to_string(const AnnotationArgValue &Value)
{
   switch (Value.type) {
      case AnnotationArgValue::Type::Bool:
         return Value.bool_value ? "true" : "false";
      case AnnotationArgValue::Type::Number:
         return std::to_string(Value.number_value);
      case AnnotationArgValue::Type::String:
         return gcstr_to_string(Value.string_value);
      case AnnotationArgValue::Type::Array: {
         std::string result;
         for (size_t i = 0; i < Value.array_value.size(); ++i) {
            if (i > 0) result += ", ";
            result += annotation_value_to_string(Value.array_value[i]);
         }
         return result;
      }
      default:
         return {};
   }
}

static const AnnotationArgValue * find_annotation_arg(const AnnotationEntry &Annotation, std::string_view Name)
{
   for (const auto &[key, value] : Annotation.args) {
      if (key and std::string_view(strdata(key), key->len) IS Name) return &value;
   }

   return nullptr;
}

static std::string find_doc_text(const std::vector<AnnotationEntry> &Annotations)
{
   for (const AnnotationEntry &annotation : Annotations) {
      if (not annotation_name_is(annotation, "Doc")) continue;

      const AnnotationArgValue *text = find_annotation_arg(annotation, "text");
      if (text and text->type IS AnnotationArgValue::Type::String) return gcstr_to_string(text->string_value);
   }

   return {};
}

static std::vector<ParserAnnotationMetadata> collect_annotations(const std::vector<AnnotationEntry> &Annotations)
{
   std::vector<ParserAnnotationMetadata> result;

   for (const AnnotationEntry &annotation : Annotations) {
      ParserAnnotationMetadata meta;
      meta.name = gcstr_to_string(annotation.name);

      for (const auto &[key, value] : annotation.args) {
         if (key) meta.args.emplace_back(gcstr_to_string(key), annotation_value_to_string(value));
      }

      result.push_back(std::move(meta));
   }

   return result;
}

static std::string identifier_to_string(const Identifier &Identifier)
{
   return gcstr_to_string(Identifier.symbol);
}

static std::string function_name_to_string(const FunctionNamePath &Path)
{
   std::string name;

   for (size_t i = 0; i < Path.segments.size(); ++i) {
      if (i > 0) name += ".";
      name += identifier_to_string(Path.segments[i]);
   }

   if (Path.method.has_value()) {
      name += ":";
      name += identifier_to_string(Path.method.value());
   }

   return name;
}

static std::string expression_name_to_string(const ExprNode &Expression)
{
   if (Expression.kind IS AstNodeKind::IdentifierExpr) {
      if (const auto *name = std::get_if<NameRef>(&Expression.data)) return identifier_to_string(name->identifier);
   }

   if (Expression.kind IS AstNodeKind::MemberExpr) {
      if (const auto *member = std::get_if<MemberExprPayload>(&Expression.data)) {
         if (member->table) {
            std::string table_name = expression_name_to_string(*member->table);
            if (not table_name.empty()) return table_name + "." + identifier_to_string(member->member);
         }
      }
   }

   return {};
}

static std::string build_signature(const std::string &Name, const FunctionExprPayload &Function)
{
   std::string signature = Name + "(";
   bool first = true;

   for (const FunctionParameter &param : Function.parameters) {
      if (param.is_self) continue;

      if (not first) signature += ", ";
      first = false;

      signature += identifier_to_string(param.name);
      if (param.type != TiriType::Any and param.type != TiriType::Unknown) {
         signature += ": ";
         signature += type_to_string(param.type);
      }
   }

   if (Function.is_vararg) {
      if (not first) signature += ", ";
      signature += "...";
   }

   signature += ")";

   if (Function.return_types.count IS 1) {
      signature += ":";
      signature += type_to_string(Function.return_types.types[0]);
   }
   else if (Function.return_types.count > 1) {
      signature += ":<";
      for (uint8_t i = 0; i < Function.return_types.count; ++i) {
         if (i > 0) signature += ", ";
         signature += type_to_string(Function.return_types.types[i]);
      }
      if (Function.return_types.is_variadic) signature += ", ...";
      signature += ">";
   }

   return signature;
}

static std::string find_param_doc(const ParsedDocText &Doc, const std::string &Name, bool &Found)
{
   for (const auto &[doc_name, doc_text] : Doc.inputs) {
      if (doc_name IS Name) {
         Found = true;
         return doc_text;
      }
   }

   Found = false;
   return {};
}

static void add_params(ParserSymbolMetadata &Symbol, const FunctionExprPayload &Function, const ParsedDocText *Doc)
{
   for (const FunctionParameter &param : Function.parameters) {
      if (param.is_self) continue;

      ParserDocParamMetadata meta;
      meta.name = identifier_to_string(param.name);
      meta.type = type_to_string(param.type);

      bool found = false;
      if (Doc) meta.doc = find_param_doc(*Doc, meta.name, found);
      meta.inferred = not found;

      Symbol.params.push_back(std::move(meta));
   }
}

static void add_results(ParserSymbolMetadata &Symbol, const FunctionExprPayload &Function, const ParsedDocText *Doc)
{
   size_t declared_count = Function.return_types.count;
   size_t doc_count = Doc ? Doc->results.size() : 0;
   size_t count = declared_count > doc_count ? declared_count : doc_count;

   for (size_t i = 0; i < count; ++i) {
      ParserDocReturnMetadata meta;

      if (i < declared_count) meta.type = type_to_string(Function.return_types.types[i]);
      if (Doc and i < Doc->results.size()) {
         meta.doc = Doc->results[i].doc;
         if (meta.type.empty()) meta.type = Doc->results[i].type;
      }

      meta.inferred = i >= declared_count;
      Symbol.results.push_back(std::move(meta));
   }
}

static SourceSpan end_span_for(const FunctionExprPayload &Function)
{
   if (Function.body) return Function.body->span;
   return {};
}

static ParserSymbolMetadata make_function_symbol(const std::string &Name, const FunctionExprPayload &Function,
   SourceSpan Span)
{
   ParserSymbolMetadata symbol;
   symbol.name = Name;
   symbol.kind = Function.is_thunk ? "thunk" : "function";
   symbol.span = Span;
   symbol.end_span = end_span_for(Function);
   symbol.signature = build_signature(Name, Function);
   symbol.annotations = collect_annotations(Function.annotations);

   std::string doc_text = find_doc_text(Function.annotations);
   if (not doc_text.empty()) {
      ParsedDocText parsed = parse_doc_text(doc_text);
      symbol.doc = parsed.block;
      symbol.errors = std::move(parsed.errors);
      add_params(symbol, Function, &parsed);
      add_results(symbol, Function, &parsed);
   }
   else {
      add_params(symbol, Function, nullptr);
      add_results(symbol, Function, nullptr);
   }

   return symbol;
}

static void collect_from_block(ParserSymbolCollection &Collection, const BlockStmt &Block);

static void collect_nested_function_body(ParserSymbolCollection &Collection, const FunctionExprPayload &Function)
{
   if (Function.body) collect_from_block(Collection, *Function.body);
}

static void collect_assignment_functions(ParserSymbolCollection &Collection, const ExprNodeList &Targets,
   const ExprNodeList &Values, SourceSpan Span)
{
   size_t count = Targets.size() < Values.size() ? Targets.size() : Values.size();

   for (size_t i = 0; i < count; ++i) {
      const ExprNodePtr &target = Targets[i];
      const ExprNodePtr &value = Values[i];
      if (not target or not value or value->kind != AstNodeKind::FunctionExpr) continue;

      const auto *function = std::get_if<FunctionExprPayload>(&value->data);
      if (not function) continue;

      std::string name = expression_name_to_string(*target);
      if (not name.empty()) Collection.symbols.push_back(make_function_symbol(name, *function, Span));
      collect_nested_function_body(Collection, *function);
   }
}

template <typename DeclPayload>
static void collect_decl_functions(ParserSymbolCollection &Collection, const DeclPayload &Payload,
   SourceSpan Span)
{
   size_t count = Payload.names.size() < Payload.values.size() ? Payload.names.size() : Payload.values.size();

   for (size_t i = 0; i < count; ++i) {
      const ExprNodePtr &value = Payload.values[i];
      if (not value or value->kind != AstNodeKind::FunctionExpr) continue;

      const auto *function = std::get_if<FunctionExprPayload>(&value->data);
      if (not function) continue;

      Collection.symbols.push_back(make_function_symbol(identifier_to_string(Payload.names[i]), *function, Span));
      collect_nested_function_body(Collection, *function);
   }
}

static void collect_from_statement(ParserSymbolCollection &Collection, const StmtNode &Statement)
{
   switch (Statement.kind) {
      case AstNodeKind::LocalFunctionStmt: {
         const auto &payload = std::get<LocalFunctionStmtPayload>(Statement.data);
         if (payload.function) {
            Collection.symbols.push_back(make_function_symbol(identifier_to_string(payload.name),
               *payload.function, Statement.span));
            collect_nested_function_body(Collection, *payload.function);
         }
         break;
      }
      case AstNodeKind::FunctionStmt: {
         const auto &payload = std::get<FunctionStmtPayload>(Statement.data);
         if (payload.function) {
            Collection.symbols.push_back(make_function_symbol(function_name_to_string(payload.name),
               *payload.function, Statement.span));
            collect_nested_function_body(Collection, *payload.function);
         }
         break;
      }
      case AstNodeKind::LocalDeclStmt: {
         const auto &payload = std::get<LocalDeclStmtPayload>(Statement.data);
         collect_decl_functions(Collection, payload, Statement.span);
         break;
      }
      case AstNodeKind::GlobalDeclStmt: {
         const auto &payload = std::get<GlobalDeclStmtPayload>(Statement.data);
         collect_decl_functions(Collection, payload, Statement.span);
         break;
      }
      case AstNodeKind::AssignmentStmt: {
         const auto &payload = std::get<AssignmentStmtPayload>(Statement.data);
         collect_assignment_functions(Collection, payload.targets, payload.values, Statement.span);
         break;
      }
      case AstNodeKind::IfStmt: {
         const auto &payload = std::get<IfStmtPayload>(Statement.data);
         for (const IfClause &clause : payload.clauses) {
            if (clause.block) collect_from_block(Collection, *clause.block);
         }
         break;
      }
      case AstNodeKind::WhileStmt:
      case AstNodeKind::RepeatStmt: {
         const auto &payload = std::get<LoopStmtPayload>(Statement.data);
         if (payload.body) collect_from_block(Collection, *payload.body);
         break;
      }
      case AstNodeKind::NumericForStmt: {
         const auto &payload = std::get<NumericForStmtPayload>(Statement.data);
         if (payload.body) collect_from_block(Collection, *payload.body);
         break;
      }
      case AstNodeKind::GenericForStmt: {
         const auto &payload = std::get<GenericForStmtPayload>(Statement.data);
         if (payload.body) collect_from_block(Collection, *payload.body);
         break;
      }
      case AstNodeKind::DoStmt: {
         const auto &payload = std::get<DoStmtPayload>(Statement.data);
         if (payload.block) collect_from_block(Collection, *payload.block);
         break;
      }
      case AstNodeKind::ConditionalShorthandStmt: {
         const auto &payload = std::get<ConditionalShorthandStmtPayload>(Statement.data);
         if (payload.body) collect_from_statement(Collection, *payload.body);
         break;
      }
      case AstNodeKind::DeferStmt: {
         const auto &payload = std::get<DeferStmtPayload>(Statement.data);
         if (payload.callable) collect_nested_function_body(Collection, *payload.callable);
         break;
      }
      case AstNodeKind::TryExceptStmt: {
         const auto &payload = std::get<TryExceptPayload>(Statement.data);
         if (payload.try_block) collect_from_block(Collection, *payload.try_block);
         for (const ExceptClause &clause : payload.except_clauses) {
            if (clause.block) collect_from_block(Collection, *clause.block);
         }
         if (payload.success_block) collect_from_block(Collection, *payload.success_block);
         break;
      }
      case AstNodeKind::WithStmt: {
         const auto &payload = std::get<WithStmtPayload>(Statement.data);
         if (payload.block) collect_from_block(Collection, *payload.block);
         break;
      }
      default:
         break;
   }
}

static void collect_from_block(ParserSymbolCollection &Collection, const BlockStmt &Block)
{
   for (const StmtNode &statement : Block.view()) collect_from_statement(Collection, statement);
}

void collect_parser_symbols(lua_State &Lua, const BlockStmt &Chunk)
{
   if (Lua.parser_symbols) {
      delete Lua.parser_symbols;
      Lua.parser_symbols = nullptr;
   }

   if (Lua.script IS nullptr or (Lua.script->Flags & SCF::PROCESS_DOC) IS SCF::NIL) return;

   auto *collection = new ParserSymbolCollection();
   collect_from_block(*collection, Chunk);
   Lua.parser_symbols = collection;
}
