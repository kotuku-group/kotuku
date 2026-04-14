
#include "../eval/eval_detail.h"
#include <algorithm>
#include <array>
#include <optional>
#include <kotuku/strings.hpp>
#include <utility>
#include "../../xml/uri_utils.h"

namespace {

struct BinaryOperatorMapping {
   std::string_view token;
   BinaryOperationKind kind;
};

constexpr std::array<BinaryOperatorMapping, 24> BINARY_OPERATOR_MAPPINGS = {{
   {"and", BinaryOperationKind::AND},
   {"or", BinaryOperationKind::OR},
   {"|", BinaryOperationKind::UNION},
   {"intersect", BinaryOperationKind::INTERSECT},
   {"except", BinaryOperationKind::EXCEPT},
   {",", BinaryOperationKind::COMMA},
   {"=", BinaryOperationKind::GENERAL_EQ},
   {"!=", BinaryOperationKind::GENERAL_NE},
   {"<", BinaryOperationKind::GENERAL_LT},
   {"<=", BinaryOperationKind::GENERAL_LE},
   {">", BinaryOperationKind::GENERAL_GT},
   {">=", BinaryOperationKind::GENERAL_GE},
   {"eq", BinaryOperationKind::VALUE_EQ},
   {"ne", BinaryOperationKind::VALUE_NE},
   {"lt", BinaryOperationKind::VALUE_LT},
   {"le", BinaryOperationKind::VALUE_LE},
   {"gt", BinaryOperationKind::VALUE_GT},
   {"ge", BinaryOperationKind::VALUE_GE},
   {"+", BinaryOperationKind::ADD},
   {"-", BinaryOperationKind::SUB},
   {"*", BinaryOperationKind::MUL},
   {"div", BinaryOperationKind::DIV},
   {"mod", BinaryOperationKind::MOD},
   {"to", BinaryOperationKind::RANGE}
}};

[[nodiscard]] constexpr std::optional<BinaryOperationKind> lookup_binary_operator_kind(std::string_view Text)
{
   for (const auto &entry : BINARY_OPERATOR_MAPPINGS) {
      if (entry.token IS Text) return entry.kind;
   }
   return std::nullopt;
}

struct UnaryOperatorMapping {
   std::string_view token;
   UnaryOperationKind kind;
};

constexpr std::array<UnaryOperatorMapping, 2> UNARY_OPERATOR_MAPPINGS = {{
   {"-", UnaryOperationKind::NEGATE},
   {"not", UnaryOperationKind::LOGICAL_NOT}
}};

[[nodiscard]] constexpr std::optional<UnaryOperationKind> lookup_unary_operator_kind(std::string_view Text)
{
   for (const auto &entry : UNARY_OPERATOR_MAPPINGS) {
      if (entry.token IS Text) return entry.kind;
   }
   return std::nullopt;
}

} // namespace

// Helper to compute feature flags from the parsed prolog and AST

XQF CompiledXQuery::feature_flags() const
{
   XQF flags = XQF::NIL;

   if (auto p = prolog.get(); p) { // Prolog-derived flags
      bool has_any_prolog_decl = false;
      if (not p->declared_namespaces.empty()) has_any_prolog_decl = true;
      if (not p->variables.empty()) has_any_prolog_decl = true;
      if (not p->functions.empty()) has_any_prolog_decl = true;
      if (not p->module_imports.empty()) { has_any_prolog_decl = true; flags |= XQF::MODULE_IMPORTS; }
      if (p->module_namespace_uri.has_value() or p->module_namespace_prefix.has_value()) has_any_prolog_decl = true;
      if (p->static_base_uri_declared or (not p->static_base_uri.empty())) { has_any_prolog_decl = true; flags |= XQF::BASE_URI_DECLARED; }
      if (p->default_collation_declared or (not p->default_collation.empty())) { has_any_prolog_decl = true; flags |= XQF::DEFAULT_COLLATION_DECLARED; }
      if (p->default_function_namespace.has_value() or p->default_function_namespace_uri.has_value()) { has_any_prolog_decl = true; flags |= XQF::DEFAULT_FUNCTION_NS; }
      if (p->default_element_namespace.has_value() or p->default_element_namespace_uri.has_value()) { has_any_prolog_decl = true; flags |= XQF::DEFAULT_ELEMENT_NS; }
      if (p->boundary_space_declared and (p->boundary_space IS XQueryProlog::BoundarySpace::Preserve)) flags |= XQF::BOUNDARY_PRESERVE;
      if (p->construction_declared and (p->construction_mode IS XQueryProlog::ConstructionMode::Preserve)) flags |= XQF::CONSTRUCTION_PRESERVE;
      if (p->ordering_declared and (p->ordering_mode IS XQueryProlog::OrderingMode::Unordered)) flags |= XQF::ORDERING_UNORDERED;

      if (has_any_prolog_decl) flags |= XQF::HAS_PROLOG;
      if (p->is_library_module) flags |= XQF::LIBRARY_MODULE;
   }

   if (expression) {
      // Mark simple XPath location paths
      if (expression->type IS XQueryNodeType::LOCATION_PATH) flags |= XQF::XPATH;

      // AST-derived flags
      auto scan_ast = [&](auto &&self, const XPathNode *node) -> void {
         if (not node) return;
         if (node->type IS XQueryNodeType::WILDCARD) flags |= XQF::HAS_WILDCARD_TESTS;
         for (const auto &child : node->children) self(self, child.get());
      };

      scan_ast(scan_ast, expression.get());
   }

   return flags;
};

//********************************************************************************************************************
// Parses a list of XPath tokens and returns an CompiledXQuery object containing:
//   - expression: the root node of the parse tree (AST) if parsing succeeds, or nullptr if parsing fails
//   - prolog: the parsed XQuery prolog
//   - (optional) module cache and other metadata
// Errors can be inspected via the error list.

CompiledXQuery XPathParser::parse(TokenBlock TokenList)
{
   CompiledXQuery result;
   result.prolog = std::make_shared<XQueryProlog>();
   active_prolog = result.prolog.get();

   token_storage = std::move(TokenList.storage);
   if (!token_storage) token_storage = std::make_shared<TokenBuffer>();

   tokens = std::move(TokenList.tokens);
   for (auto &token : tokens) {
      switch (token.type) {
         case XPathTokenType::FUNCTION:
         case XPathTokenType::VARIABLE:
         case XPathTokenType::NAMESPACE:
         case XPathTokenType::EXTERNAL:
         case XPathTokenType::BOUNDARY_SPACE:
         case XPathTokenType::BASE_URI:
            token.type = XPathTokenType::IDENTIFIER;
            break;
         default:
            break;
      }
   }
   current_token = 0;
   errors.clear();

   if (check_literal_keyword("module")) {
      size_t lookahead = current_token + 1U;
      bool is_module_decl = false;

      if (lookahead < tokens.size()) {
         const auto &next = tokens[lookahead];
         std::string_view keyword = keyword_from_token_type(next.type);
         if (keyword IS "namespace") is_module_decl = true;
         else if ((next.type IS XPathTokenType::IDENTIFIER) and (next.text IS "namespace")) is_module_decl = true;
      }

      if (is_module_decl) {
         advance();
         if (not parse_module_decl(*result.prolog)) {
            result.expression.reset();
            return result;
         }
      }
   }

   bool prolog_result = parse_prolog(*result.prolog);
   if (prolog_result and has_errors()) {
      // Prolog parsing detected but encountered errors
      result.expression.reset();
      return result;
   }
   else if (has_errors()) {
      result.expression.reset();
      return result;
   }

   if (result.prolog->is_library_module) {
      if (not result.prolog->validate_library_exports()) {
         report_error("XQST0048: Library module exports must use the declared namespace");
      }

      while (match(XPathTokenType::SEMICOLON)) {}

      if (not is_at_end()) {
         XPathToken token = peek();
         std::string token_text(token.text);
         if (token_text.empty()) token_text = "<unexpected>";
         report_error("XQST0039: Library module must not contain a query body (unexpected token '" + token_text + "')");
      }

      if (has_errors()) {
         result.expression.reset();
         return result;
      }

      result.expression.reset();
      return result;
   }

   auto expression = parse_expr();
   active_prolog = nullptr;

   if (not is_at_end()) {
      XPathToken token = peek();
      std::string token_text(token.text);
      if (token_text.empty()) token_text = "<unexpected>";
      report_error("XPST0003: Unexpected token '" + token_text + "' in XPath expression");
      result.expression.reset();
      return result;
   }
   else if (has_errors() or (not expression)) {
      result.expression.reset();
      return result;
   }
   else if (expression->type IS XQueryNodeType::LOCATION_PATH) {
      result.expression = std::move(expression);
      return result;
   }

   if (expression->type IS XQueryNodeType::PATH) {
      if (expression->child_count() IS 1) {
         auto child = std::move(expression->children[0]);
          if (child and (child->type IS XQueryNodeType::LOCATION_PATH)) {
             result.expression = std::move(child);
             return result;
          }
      }
   }

   auto root = std::make_unique<XPathNode>(XQueryNodeType::EXPRESSION);
   root->add_child(std::move(expression));
   result.expression = std::move(root);
   return result;
}

//********************************************************************************************************************

bool XPathParser::match_literal_keyword(std::string_view Keyword)
{
   if (check_literal_keyword(Keyword)) {
      advance();
      return true;
   }
   return false;
}

//********************************************************************************************************************

bool XPathParser::is_function_call_ahead(size_t Index) const
{
   if (Index >= tokens.size()) return false;

   const auto &first = tokens[Index];
   if (not is_identifier_token(first)) return false;

   size_t lookahead = Index + 1;

   if ((lookahead < tokens.size()) and (tokens[lookahead].type IS XPathTokenType::COLON)) {
      lookahead++;
      if (lookahead >= tokens.size()) return false;

      if (not is_identifier_token(tokens[lookahead])) return false;
      lookahead++;
   }

   if (lookahead >= tokens.size()) return false;
   return tokens[lookahead].type IS XPathTokenType::LPAREN;
}

//********************************************************************************************************************

extern std::string_view keyword_from_token_type(XPathTokenType Type)
{
   switch (Type) {
      case XPathTokenType::AND:               return "and";
      case XPathTokenType::OR:                return "or";
      case XPathTokenType::NOT:               return "not";
      case XPathTokenType::DIVIDE:            return "div";
      case XPathTokenType::MODULO:            return "mod";
      case XPathTokenType::EQ:                return "eq";
      case XPathTokenType::NE:                return "ne";
      case XPathTokenType::LT:                return "lt";
      case XPathTokenType::LE:                return "le";
      case XPathTokenType::GT:                return "gt";
      case XPathTokenType::GE:                return "ge";
      case XPathTokenType::IF:                return "if";
      case XPathTokenType::THEN:              return "then";
      case XPathTokenType::ELSE:              return "else";
      case XPathTokenType::FOR:               return "for";
      case XPathTokenType::LET:               return "let";
      case XPathTokenType::IN:                return "in";
      case XPathTokenType::RETURN:            return "return";
      case XPathTokenType::WHERE:             return "where";
      case XPathTokenType::GROUP:             return "group";
      case XPathTokenType::BY:                return "by";
      case XPathTokenType::ORDER:             return "order";
      case XPathTokenType::STABLE:            return "stable";
      case XPathTokenType::ASCENDING:         return "ascending";
      case XPathTokenType::DESCENDING:        return "descending";
      case XPathTokenType::EMPTY:             return "empty";
      case XPathTokenType::DEFAULT:           return "default";
      case XPathTokenType::TYPESWITCH:        return "typeswitch";
      case XPathTokenType::CASE:              return "case";
      case XPathTokenType::DECLARE:           return "declare";
      case XPathTokenType::FUNCTION:          return "function";
      case XPathTokenType::VARIABLE:          return "variable";
      case XPathTokenType::NAMESPACE:         return "namespace";
      case XPathTokenType::EXTERNAL:          return "external";
      case XPathTokenType::BOUNDARY_SPACE:    return "boundary-space";
      case XPathTokenType::BASE_URI:          return "base-uri";
      case XPathTokenType::GREATEST:          return "greatest";
      case XPathTokenType::LEAST:             return "least";
      case XPathTokenType::COLLATION:         return "collation";
      case XPathTokenType::CONSTRUCTION:      return "construction";
      case XPathTokenType::ORDERING:          return "ordering";
      case XPathTokenType::COPY_NAMESPACES:   return "copy-namespaces";
      case XPathTokenType::DECIMAL_FORMAT:    return "decimal-format";
      case XPathTokenType::OPTION:            return "option";
      case XPathTokenType::IMPORT:            return "import";
      case XPathTokenType::MODULE:            return "module";
      case XPathTokenType::SCHEMA:            return "schema";
      case XPathTokenType::COUNT:             return "count";
      case XPathTokenType::SOME:              return "some";
      case XPathTokenType::EVERY:             return "every";
      case XPathTokenType::SATISFIES:         return "satisfies";
      case XPathTokenType::CAST:              return "cast";
      case XPathTokenType::CASTABLE:          return "castable";
      case XPathTokenType::TREAT:             return "treat";
      case XPathTokenType::AS:                return "as";
      case XPathTokenType::INSTANCE:          return "instance";
      case XPathTokenType::OF:                return "of";
      case XPathTokenType::TO:                return "to";
      case XPathTokenType::UNION:             return "union";
      case XPathTokenType::INTERSECT:         return "intersect";
      case XPathTokenType::EXCEPT:            return "except";
      default:
         break;
   }

   return std::string_view();
}

//********************************************************************************************************************
// Returns true if the given token type represents a keyword that can also function as an identifier in name
// contexts (element names, attribute names, function names, etc.). This provides a centralized source of truth
// that automatically includes all keywords defined in keyword_from_token_type().


//********************************************************************************************************************

bool XPathParser::check_literal_keyword(std::string_view Keyword) const
{
   const auto &token = peek();

   if (token.type IS XPathTokenType::IDENTIFIER) return token.text IS Keyword;

   std::string_view token_keyword = keyword_from_token_type(token.type);
   if (not token_keyword.empty()) return Keyword IS token_keyword;

   return false;
}

//********************************************************************************************************************
// Parses a string literal value and returns it, or std::nullopt on error.

std::optional<std::string> XPathParser::parse_string_literal_value()
{
   if (not check(XPathTokenType::STRING)) {
      report_error("XPST0003: Expected string literal");
      return std::nullopt;
   }

   std::string value(peek().text);
   advance();
   return value;
}

//********************************************************************************************************************

std::optional<std::string> XPathParser::parse_ncname()
{
   if (not is_identifier_token(peek())) {
      report_error("XPST0003: Expected name");
      return std::nullopt;
   }

   std::string name(peek().text);
   advance();
   return name;
}

//********************************************************************************************************************

std::optional<std::string> XPathParser::parse_qname_string()
{
   if (not is_identifier_token(peek())) {
      report_error("XPST0003: Expected QName");
      return std::nullopt;
   }

   std::string name(peek().text);
   advance();

   if (match(XPathTokenType::COLON)) {
      if (not is_identifier_token(peek())) {
         report_error("XPST0003: Expected local-name after ':'");
         return std::nullopt;
      }

      name.append(":");
      name.append(peek().text);
      advance();
   }

   return name;
}

//********************************************************************************************************************

std::optional<std::string> XPathParser::collect_sequence_type(bool StopAtReturnKeyword)
{
   std::string collected;
   int paren_depth = 0;
   XPathTokenType previous_type = XPathTokenType::UNKNOWN;

   while (not is_at_end()) {
      const auto &token = peek();

      if ((token.type IS XPathTokenType::COMMA) and (paren_depth IS 0)) break;
      if ((token.type IS XPathTokenType::RPAREN) and (paren_depth IS 0)) break;
      if ((token.type IS XPathTokenType::LBRACE) and (paren_depth IS 0)) break;
      if ((token.type IS XPathTokenType::ASSIGN) and (paren_depth IS 0)) break;
      if ((token.type IS XPathTokenType::SEMICOLON) and (paren_depth IS 0)) break;
      if (check_literal_keyword("external") and (paren_depth IS 0)) break;
      if (StopAtReturnKeyword and (token.type IS XPathTokenType::RETURN) and (paren_depth IS 0)) {
         // Permit QNames such as xs:return by ensuring the keyword is not part of a prefixed name.
         if (previous_type IS XPathTokenType::COLON) {}
         else break;
      }

      if (token.type IS XPathTokenType::LPAREN) paren_depth++;
      else if (token.type IS XPathTokenType::RPAREN) {
         if (paren_depth IS 0) break;
         paren_depth--;
      }

      bool add_space = !collected.empty();
      if (add_space) {
         if ((previous_type IS XPathTokenType::COLON) or (token.type IS XPathTokenType::COLON) or
             (previous_type IS XPathTokenType::QUESTION_MARK) or (token.type IS XPathTokenType::QUESTION_MARK)) add_space = false;
      }

      if (add_space) collected.push_back(' ');
      collected.append(std::string(token.text));
      advance();
      previous_type = token.type;
   }

   if (collected.empty()) return std::nullopt;
   return collected;
}

//********************************************************************************************************************
// Parses the leading XQuery prolog, returning true when any declarations were consumed.

bool XPathParser::parse_prolog(XQueryProlog &prolog)
{
   bool saw_prolog = false;

   while (not is_at_end()) {
      if (match(XPathTokenType::SEMICOLON)) {
         saw_prolog = true;
         continue;
      }

      if (check_identifier_keyword("declare")) {
         advance();
         saw_prolog = true;
         if (not parse_declare_statement(prolog)) return false;
         bool consumed_separator = consume_declaration_separator();
         if (not consumed_separator) {
            if (check_identifier_keyword("declare") or check_literal_keyword("import")) {
               report_error("Expected ';' between prolog declarations");
               return false;
            }
         }
         continue;
      }

      if (match_literal_keyword("import")) {
         saw_prolog = true;
         if (not parse_import_statement(prolog)) return false;
         bool consumed_separator = consume_declaration_separator();
         if (not consumed_separator) {
            if (check_identifier_keyword("declare") or check_literal_keyword("import")) {
               report_error("Expected ';' between prolog declarations");
               return false;
            }
         }
         continue;
      }

      break;
   }

   return saw_prolog;
}

//********************************************************************************************************************

bool XPathParser::parse_module_decl(XQueryProlog &prolog)
{
   if (prolog.is_library_module) {
      report_error("XQST0031: Multiple module declarations are not permitted");
      return false;
   }

   if (not match_literal_keyword("namespace")) {
      report_error("XPST0003: Expected 'namespace' after 'module'");
      return false;
   }

   auto prefix = parse_ncname();
   if (not prefix) return false;

   if (not consume_token(XPathTokenType::EQUALS, "Expected '=' in module declaration")) return false;

   auto uri = parse_uri_literal();
   if (not uri) return false;

   std::string normalised_uri = xml::uri::normalise_uri_separators(*uri);

   if (not prolog.declare_namespace(*prefix, normalised_uri, nullptr)) {
      report_error("XQST0033: Duplicate namespace prefix declaration");
      return false;
   }

   prolog.is_library_module = true;
   prolog.module_namespace_prefix = *prefix;
   prolog.module_namespace_uri = std::move(normalised_uri);

   if (not match(XPathTokenType::SEMICOLON)) {
      report_error("XQST0038: Expected ';' after module declaration");
      return false;
   }

   while (match(XPathTokenType::SEMICOLON)) {}

   return true;
}

//********************************************************************************************************************
// Parses a "declare" statement and dispatches to the relevant declaration parser.

bool XPathParser::parse_declare_statement(XQueryProlog &prolog)
{
   // Dispatches to the relevant declaration parser based on the keyword following "declare".
   if (match_literal_keyword("namespace")) return parse_namespace_decl(prolog);

   if (check_literal_keyword("default")) {
      match_literal_keyword("default");

      if (match_literal_keyword("element")) return parse_default_namespace_decl(prolog, false);
      if (match_literal_keyword("function")) return parse_default_namespace_decl(prolog, true);
      if (match_literal_keyword("collation")) return parse_default_collation_decl(prolog);
      if (match_literal_keyword("order")) return parse_empty_order_decl(prolog);

      report_error("XPST0003: Unsupported default declaration");
      return false;
   }

   if (match_literal_keyword("variable")) return parse_variable_decl(prolog);
   if (match_literal_keyword("function")) return parse_function_decl(prolog);
   if (match_literal_keyword("boundary-space")) return parse_boundary_space_decl(prolog);
   if (match_literal_keyword("base-uri")) return parse_base_uri_decl(prolog);
   if (match_literal_keyword("construction")) return parse_construction_decl(prolog);
   if (match_literal_keyword("ordering")) return parse_ordering_decl(prolog);
   if (match_literal_keyword("copy-namespaces")) return parse_copy_namespaces_decl(prolog);
   if (match_literal_keyword("decimal-format")) return parse_decimal_format_decl(prolog);
   if (match_literal_keyword("option")) return parse_option_decl(prolog);

   report_error("XPST0003: Unsupported declaration in prolog");
   return false;
}

//********************************************************************************************************************
// Parses a "declare namespace" statement and stores the prefix binding inside the active prolog.

bool XPathParser::parse_namespace_decl(XQueryProlog &prolog)
{
   auto prefix = parse_ncname();
   if (not prefix) return false;

   if (not consume_token(XPathTokenType::EQUALS, "Expected '=' in namespace declaration")) return false;

   auto uri = parse_uri_literal();
   if (not uri) return false;

   if (not prolog.declare_namespace(*prefix, *uri, nullptr)) {
      report_error("Duplicate namespace prefix declaration");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Handles "declare default element|function namespace" declarations.

bool XPathParser::parse_default_namespace_decl(XQueryProlog &prolog, bool IsFunctionNamespace)
{
   if (not match_literal_keyword("namespace")) {
      report_error("XPST0003: Expected 'namespace' in default namespace declaration");
      return false;
   }

   auto uri = parse_uri_literal();
   if (not uri) return false;

   std::string cleaned = xml::uri::normalise_uri_separators(*uri);
   uint32_t hash = pf::strhash(cleaned);
   if (IsFunctionNamespace) {
      prolog.default_function_namespace = hash;
      prolog.default_function_namespace_uri = cleaned;
   }
   else {
      prolog.default_element_namespace = hash;
      prolog.default_element_namespace_uri = cleaned;
   }

   return true;
}

//********************************************************************************************************************
// Consumes a "declare default collation" declaration and records the URI.

bool XPathParser::parse_default_collation_decl(XQueryProlog &prolog)
{
   auto collation = parse_uri_literal();
   if (not collation) return false;

   if (prolog.default_collation_declared) {
      report_error("Duplicate default collation declaration");
      return false;
   }

   prolog.default_collation = *collation;
   prolog.default_collation_declared = true;
   return true;
}

//********************************************************************************************************************
// Parses a prolog variable declaration, supporting both inline initialisers and external markers.

bool XPathParser::parse_variable_decl(XQueryProlog &prolog)
{
   if (not consume_token(XPathTokenType::DOLLAR, "Expected '$' in variable declaration")) return false;

   auto name = parse_qname_string();
   if (not name) return false;

   if (match_literal_keyword("as")) {
      auto sequence_type = collect_sequence_type();
      if (not sequence_type) {
         report_error("XPST0003: Expected sequence type after 'as'");
         return false;
      }
   }

   XQueryVariable variable;
   variable.qname = *name;

   if (match_literal_keyword("external")) {
      variable.is_external = true;
      auto variable_name = variable.qname;
      if (not prolog.declare_variable(variable_name, std::move(variable))) {
         report_error("Duplicate variable declaration");
         return false;
      }
      return true;
   }

   if (not consume_token(XPathTokenType::ASSIGN, "Expected ':=' in variable declaration")) return false;

   auto initializer = parse_expr_single();
   if (not initializer) return false;

   variable.initializer = std::move(initializer);
   auto variable_name = variable.qname;
   if (not prolog.declare_variable(variable_name, std::move(variable))) {
      report_error("Duplicate variable declaration");
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Parses a prolog function declaration, capturing parameters, optional types, and the function body.

bool XPathParser::parse_function_decl(XQueryProlog &prolog)
{
   auto qname = parse_qname_string();
   if (not qname) return false;

   if (not consume_token(XPathTokenType::LPAREN, "Expected '(' after function name")) return false;

   std::vector<std::string> parameter_names;
   std::vector<std::string> parameter_types;

   if (not check(XPathTokenType::RPAREN)) {
      while (true) {
         if (not consume_token(XPathTokenType::DOLLAR, "Expected '$' at start of parameter")) return false;

         auto param_name = parse_qname_string();
         if (not param_name) return false;

         if (std::find(parameter_names.begin(), parameter_names.end(), *param_name) not_eq parameter_names.end()) {
            report_error("Duplicate parameter name in function declaration");
            return false;
         }

         parameter_names.push_back(*param_name);

         std::optional<std::string> type_annotation;
         if (match_literal_keyword("as")) {
            type_annotation = collect_sequence_type();
            if (not type_annotation) {
               report_error("XPST0003: Expected sequence type after 'as'");
               return false;
            }
         }

         if (type_annotation) parameter_types.push_back(*type_annotation);
         else parameter_types.emplace_back();

         if (not match(XPathTokenType::COMMA)) break;
      }
   }

   if (not consume_token(XPathTokenType::RPAREN, "Expected ')' after parameters")) return false;

   std::string function_signature = *qname + "/" + std::to_string(parameter_names.size());

   std::optional<std::string> return_type;
   if (match_literal_keyword("as")) {
      return_type = collect_sequence_type();
      if (not return_type) {
         report_error("XPST0003: Expected sequence type after 'as'");
         return false;
      }
   }

   XQueryFunction function;
   if (active_prolog) function.qname = active_prolog->normalise_function_qname(*qname);
   else function.qname = *qname;
   function.parameter_names = std::move(parameter_names);
   function.parameter_types = std::move(parameter_types);

   if (return_type and !return_type->empty()) function.return_type = return_type;

   if (match_literal_keyword("external")) {
      function.is_external = true;
      if (prolog.declare_function(std::move(function))) return true;
      report_error("Duplicate function declaration");
      return false;
   }

   auto body = parse_enclosed_expr();
   if (not body) return false;

   function.body = std::move(body);
   if (prolog.declare_function(std::move(function))) return true;
   report_error("Duplicate function declaration");
   return false;
}

//********************************************************************************************************************
// Applies the "declare boundary-space" policy to the active prolog.

bool XPathParser::parse_boundary_space_decl(XQueryProlog &prolog)
{
   if (prolog.boundary_space_declared) {
      report_error("Duplicate boundary-space declaration");
      return false;
   }

   if (match_literal_keyword("preserve")) {
      prolog.boundary_space = XQueryProlog::BoundarySpace::Preserve;
      prolog.boundary_space_declared = true;
      return true;
   }

   if (match_literal_keyword("strip")) {
      prolog.boundary_space = XQueryProlog::BoundarySpace::Strip;
      prolog.boundary_space_declared = true;
      return true;
   }

   report_error("XQST0068: Expected 'preserve' or 'strip' in boundary-space declaration");
   return false;
}

//********************************************************************************************************************
// Parses a "declare base-uri" statement and stores the literal URI.

bool XPathParser::parse_base_uri_decl(XQueryProlog &prolog)
{
   auto uri = parse_uri_literal();
   if (not uri) return false;

   if (prolog.static_base_uri_declared) {
      report_error("Duplicate base-uri declaration");
      return false;
   }

   prolog.static_base_uri = *uri;
   prolog.static_base_uri_declared = true;
   return true;
}

//********************************************************************************************************************
// Records the "declare construction" policy that governs node construction during evaluation.

bool XPathParser::parse_construction_decl(XQueryProlog &prolog)
{
   if (prolog.construction_declared) {
      report_error("Duplicate construction mode declaration");
      return false;
   }

   if (match_literal_keyword("preserve")) {
      prolog.construction_mode = XQueryProlog::ConstructionMode::Preserve;
      prolog.construction_declared = true;
      return true;
   }

   if (match_literal_keyword("strip")) {
      prolog.construction_mode = XQueryProlog::ConstructionMode::Strip;
      prolog.construction_declared = true;
      return true;
   }

   report_error("XQST0067: Expected 'preserve' or 'strip' in construction declaration");
   return false;
}

//********************************************************************************************************************
// Stores the "declare ordering" mode controlling sequence ordering expectations.

bool XPathParser::parse_ordering_decl(XQueryProlog &prolog)
{
   if (prolog.ordering_declared) {
      report_error("Duplicate ordering mode declaration");
      return false;
   }

   if (match_literal_keyword("ordered")) {
      prolog.ordering_mode = XQueryProlog::OrderingMode::Ordered;
      prolog.ordering_declared = true;
      return true;
   }

   if (match_literal_keyword("unordered")) {
      prolog.ordering_mode = XQueryProlog::OrderingMode::Unordered;
      prolog.ordering_declared = true;
      return true;
   }

   report_error("XQST0065: Expected 'ordered' or 'unordered' in ordering declaration");
   return false;
}

//********************************************************************************************************************
// Handles the "declare default order empty" declaration.

bool XPathParser::parse_empty_order_decl(XQueryProlog &prolog)
{
   if (not match_literal_keyword("empty")) {
      report_error("XPST0003: Expected 'empty' in default order declaration");
      return false;
   }

   if (prolog.empty_order_declared) {
      report_error("Duplicate default empty order declaration");
      return false;
   }

   if (match_literal_keyword("greatest")) {
      prolog.empty_order = XQueryProlog::EmptyOrder::Greatest;
      prolog.empty_order_declared = true;
      return true;
   }

   if (match_literal_keyword("least")) {
      prolog.empty_order = XQueryProlog::EmptyOrder::Least;
      prolog.empty_order_declared = true;
      return true;
   }

   report_error("XPST0003: Expected 'greatest' or 'least' after 'empty'");
   return false;
}

//********************************************************************************************************************
// Parses "declare copy-namespaces" preserving the preserve/no-preserve and inherit/no-inherit flags.

bool XPathParser::parse_copy_namespaces_decl(XQueryProlog &prolog)
{
   if (prolog.copy_namespaces_declared) {
      report_error("Duplicate copy-namespaces declaration");
      return false;
   }

   bool preserve = true;
   bool inherit = true;

   if (match_literal_keyword("preserve")) preserve = true;
   else if (match_literal_keyword("no-preserve")) preserve = false;
   else {
      report_error("XPST0003: Expected 'preserve' or 'no-preserve' in copy-namespaces declaration");
      return false;
   }

   if (not consume_token(XPathTokenType::COMMA, "Expected ',' in copy-namespaces declaration")) return false;

   if (match_literal_keyword("inherit")) inherit = true;
   else if (match_literal_keyword("no-inherit")) inherit = false;
   else {
      report_error("XPST0003: Expected 'inherit' or 'no-inherit' in copy-namespaces declaration");
      return false;
   }

   prolog.copy_namespaces.preserve = preserve;
   prolog.copy_namespaces.inherit = inherit;
   prolog.copy_namespaces_declared = true;
   return true;
}

//********************************************************************************************************************
// Parses a "declare decimal-format" block, recording the named or default format settings.

bool XPathParser::parse_decimal_format_decl(XQueryProlog &prolog)
{
   std::string format_name;

   auto is_property_name = [](std::string_view text) -> bool {
      return (text IS "decimal-separator") or (text IS "grouping-separator") or (text IS "infinity") or
         (text IS "minus-sign") or (text IS "NaN") or (text IS "percent") or (text IS "per-mille") or
         (text IS "zero-digit") or (text IS "digit") or (text IS "pattern-separator");
   };

   if (is_identifier_token(peek())) {
      std::string candidate(peek().text);
      bool treat_as_property = is_property_name(candidate);
      if (not treat_as_property) {
         if ((current_token + 1 < tokens.size()) and (tokens[current_token + 1].type IS XPathTokenType::COLON)) {
            treat_as_property = false;
         }
      }

      if (not treat_as_property) {
         auto qname = parse_qname_string();
         if (not qname) return false;
         format_name = *qname;
      }
   }

   DecimalFormat format;
   format.name = format_name;

   if (format_name.empty()) {
      if (prolog.default_decimal_format_declared) {
         report_error("Duplicate decimal-format declaration");
         return false;
      }
   }
   else if (prolog.decimal_formats.contains(format_name)) {
      report_error("Duplicate decimal-format declaration");
      return false;
   }

   bool saw_property = false;
   while (true) {
      if (not is_identifier_token(peek())) break;

      std::string property(peek().text);
      if (not is_property_name(property)) break;
      advance();

      if (not consume_token(XPathTokenType::EQUALS, "Expected '=' in decimal-format declaration")) return false;

      auto value = parse_string_literal_value();
      if (not value) return false;

      if (property IS "decimal-separator") format.decimal_separator = *value;
      else if (property IS "grouping-separator") format.grouping_separator = *value;
      else if (property IS "infinity") format.infinity = *value;
      else if (property IS "minus-sign") format.minus_sign = *value;
      else if (property IS "NaN") format.nan = *value;
      else if (property IS "percent") format.percent = *value;
      else if (property IS "per-mille") format.per_mille = *value;
      else if (property IS "zero-digit") format.zero_digit = *value;
      else if (property IS "digit") format.digit = *value;
      else if (property IS "pattern-separator") format.pattern_separator = *value;

      saw_property = true;

      if (not match(XPathTokenType::COMMA)) break;
   }

   if (not saw_property) {
      report_error("XPST0003: Expected decimal-format property declaration");
      return false;
   }

   prolog.decimal_formats[format_name] = std::move(format);
   if (format_name.empty()) prolog.default_decimal_format_declared = true;
   return true;
}

//********************************************************************************************************************
// Records a "declare option" entry consisting of a QName and literal value.

bool XPathParser::parse_option_decl(XQueryProlog &prolog)
{
   auto name = parse_qname_string();
   if (not name) return false;

   auto value = parse_string_literal_value();
   if (not value) return false;

   prolog.options[*name] = *value;
   return true;
}

//********************************************************************************************************************
// Parses an "import module" or "import schema" statement, deferring schema handling to a stub.

bool XPathParser::parse_import_statement(XQueryProlog &prolog)
{
   if (match_literal_keyword("module")) return parse_import_module_decl(prolog);
   if (match_literal_keyword("schema")) return parse_import_schema_decl();

   report_error("XPST0003: Expected 'module' or 'schema' after import");
   return false;
}

//********************************************************************************************************************
// Parses an "import module" declaration and records the namespace plus location hints.

bool XPathParser::parse_import_module_decl(XQueryProlog &prolog)
{
   if (not match_literal_keyword("namespace")) {
      report_error("XPST0003: Expected 'namespace' in module import");
      return false;
   }

   auto prefix = parse_ncname();
   if (not prefix) return false;

   if (not consume_token(XPathTokenType::EQUALS, "Expected '=' in module import")) return false;

   auto uri = parse_uri_literal();
   if (not uri) return false;

   XQueryModuleImport module_import;
   module_import.target_namespace = xml::uri::normalise_uri_separators(*uri);

   if (match_literal_keyword("at")) {
      while (true) {
         auto location = parse_string_literal_value();
         if (not location) return false;
         module_import.location_hints.push_back(*location);
         if (not match(XPathTokenType::COMMA)) break;
      }
   }

   if (not prolog.declare_namespace(*prefix, module_import.target_namespace, nullptr)) {
      report_error("Duplicate namespace prefix declaration");
      return false;
   }
   prolog.module_imports.push_back(std::move(module_import));
   return true;
}

//********************************************************************************************************************
// Emits an error for unsupported "import schema" declarations.

bool XPathParser::parse_import_schema_decl()
{
   report_error("XQST0009: Schema imports are not supported");
   return false;
}

//********************************************************************************************************************
// Checks if the current token represents the specified keyword, accepting either dedicated keyword token types
// produced by the tokeniser or identifiers containing the keyword text.

bool XPathParser::check_identifier_keyword(std::string_view Keyword) const
{
   const XPathToken &token = peek();

   std::string_view token_keyword = keyword_from_token_type(token.type);
   if (not token_keyword.empty()) return token_keyword IS Keyword;

   return (token.type IS XPathTokenType::IDENTIFIER) and (token.text IS Keyword);
}

//********************************************************************************************************************
// Attempts to match and consume a keyword token, accepting either the dedicated token type or an identifier with
// matching text, returning the matched token via OutToken.

bool XPathParser::match_identifier_keyword(std::string_view Keyword, XPathTokenType KeywordType, XPathToken &OutToken)
{
   if (match(KeywordType)) {
      OutToken = previous();
      return true;
   }

   if (check(XPathTokenType::IDENTIFIER) and (peek().text IS Keyword)) {
      OutToken = peek();
      OutToken.type = KeywordType;
      advance();
      return true;
   }

   return false;
}

//********************************************************************************************************************
// Constructs a binary operation AST node from left operand, operator token, and right operand.

std::unique_ptr<XPathNode> XPathParser::create_binary_op(std::unique_ptr<XPathNode> Left, const XPathToken &Op, std::unique_ptr<XPathNode> Right)
{
   auto binary_op = std::make_unique<XPathNode>(XQueryNodeType::BINARY_OP, std::string(Op.text));
   binary_op->add_child(std::move(Left));
   binary_op->add_child(std::move(Right));
   if (auto cached_kind = lookup_binary_operator_kind(binary_op->get_value_view()); cached_kind.has_value()) {
      binary_op->set_cached_binary_kind(*cached_kind);
   }
   return binary_op;
}

//********************************************************************************************************************
// Constructs a unary operation AST node from operator token and operand.

std::unique_ptr<XPathNode> XPathParser::create_unary_op(const XPathToken &Op, std::unique_ptr<XPathNode> Operand)
{
   auto unary_op = std::make_unique<XPathNode>(XQueryNodeType::UNARY_OP, std::string(Op.text));
   unary_op->add_child(std::move(Operand));
   if (auto cached_kind = lookup_unary_operator_kind(unary_op->get_value_view()); cached_kind.has_value()) {
      unary_op->set_cached_unary_kind(*cached_kind);
   }
   return unary_op;
}

//********************************************************************************************************************
// Parses location path expressions handling both absolute (starting with / or //) and relative paths, collecting
// individual steps separated by path separators.

std::unique_ptr<XPathNode> XPathParser::parse_location_path()
{
   auto path = std::make_unique<XPathNode>(XQueryNodeType::LOCATION_PATH);

   bool is_absolute = false;
   if (match(XPathTokenType::SLASH)) {
      is_absolute = true;
      path->add_child(std::make_unique<XPathNode>(XQueryNodeType::ROOT, "/"));
   }
   else if (match(XPathTokenType::DOUBLE_SLASH)) {
      is_absolute = true;
      path->add_child(std::make_unique<XPathNode>(XQueryNodeType::ROOT, "//"));
   }

   // Parse steps

   while (not is_at_end()) {
      if (check(XPathTokenType::RBRACKET) or
          check(XPathTokenType::RPAREN) or
          check(XPathTokenType::COMMA) or
          check(XPathTokenType::PIPE) or
          check(XPathTokenType::UNION) or
          check(XPathTokenType::INTERSECT) or
          check(XPathTokenType::EXCEPT)) {
         break;
      }

      if (not is_step_start_token(peek().type)) break;

      auto step = parse_step();
      if (step) path->add_child(std::move(step));
      else break;

      if (match(XPathTokenType::SLASH)) continue;

      if (match(XPathTokenType::DOUBLE_SLASH)) {
         auto descendant_step = std::make_unique<XPathNode>(XQueryNodeType::STEP);
         descendant_step->add_child(std::make_unique<XPathNode>(XQueryNodeType::AXIS_SPECIFIER, "descendant-or-self"));
         descendant_step->add_child(std::make_unique<XPathNode>(XQueryNodeType::NODE_TYPE_TEST, "node"));
         path->add_child(std::move(descendant_step));
         continue;
      }

      break;
   }

   return path;
}

//********************************************************************************************************************
// Parses a single location path step, including abbreviated steps (. and ..), axis specifiers, node tests,
// and predicates attached to the step.

std::unique_ptr<XPathNode> XPathParser::parse_step()
{
   auto step = std::make_unique<XPathNode>(XQueryNodeType::STEP);

   // Handle abbreviated steps
   if (check(XPathTokenType::DOT)) {
      advance();
      step->add_child(std::make_unique<XPathNode>(XQueryNodeType::AXIS_SPECIFIER, "self"));
      step->add_child(std::make_unique<XPathNode>(XQueryNodeType::NODE_TYPE_TEST, "node"));
      return step;
   }

   if (check(XPathTokenType::DOUBLE_DOT)) {
      advance();
      step->add_child(std::make_unique<XPathNode>(XQueryNodeType::AXIS_SPECIFIER, "parent"));
      step->add_child(std::make_unique<XPathNode>(XQueryNodeType::NODE_TYPE_TEST, "node"));
      return step;
   }

   // Handle explicit axis specifiers (axis::node-test)
   if (is_identifier_token(peek())) {
      // Look ahead for axis separator
      if (current_token + 1 < tokens.size() and tokens[current_token + 1].type IS XPathTokenType::AXIS_SEPARATOR) {
         std::string axis_name(peek().text);
         advance(); // consume axis name
         advance(); // consume "::"
         auto axis = std::make_unique<XPathNode>(XQueryNodeType::AXIS_SPECIFIER, axis_name);
         step->add_child(std::move(axis));
      }
   }
   else if (match(XPathTokenType::AT)) { // Handle attribute axis (@)
      auto axis = std::make_unique<XPathNode>(XQueryNodeType::AXIS_SPECIFIER, "attribute");
      step->add_child(std::move(axis));
   }

   // Parse node test
   auto node_test = parse_node_test();
   if (node_test) step->add_child(std::move(node_test));

   // Parse predicates (square brackets and round brackets)
   while (true) {
      if (check(XPathTokenType::LBRACKET)) {
         auto predicate = parse_predicate();
         if (predicate) step->add_child(std::move(predicate));
         else break;
      }
      else break;
   }

   return step;
}

//********************************************************************************************************************
// Parses node tests including wildcards, name tests (element names), qualified names with namespaces,
// and node type tests like node(), text(), comment(), and processing-instruction().

std::unique_ptr<XPathNode> XPathParser::parse_node_test()
{
   if (check(XPathTokenType::WILDCARD)) {
      advance();

      // Check for QName wildcard pattern like *:item
      if (check(XPathTokenType::COLON)) {
         if (current_token + 1 < tokens.size() and is_identifier_token(tokens[current_token + 1])) {
            advance(); // consume ':'
            std::string name = std::format("*:{}", peek().text);
            advance();
            return std::make_unique<XPathNode>(XQueryNodeType::NAME_TEST, name);
         }
      }

      return std::make_unique<XPathNode>(XQueryNodeType::WILDCARD, "*");
   }
   else if (is_identifier_token(peek())) {
      std::string name(peek().text);

      bool is_node_type = false;
      if ((name IS "node") or (name IS "text") or (name IS "comment") or (name IS "processing-instruction")) {
         if ((current_token + 1 < tokens.size()) and (tokens[current_token + 1].type IS XPathTokenType::LPAREN)) {
            is_node_type = true;
         }
      }

      if (is_node_type) {
         advance(); // consume identifier

         if (not match(XPathTokenType::LPAREN)) {
            report_error("XPST0003: Expected '(' after node type test");
            return nullptr;
         }

         if (name IS "processing-instruction") {
            std::string target;

            if (not check(XPathTokenType::RPAREN)) {
               if (check(XPathTokenType::STRING) or is_identifier_token(peek())) {
                  target = peek().text;
                  advance();
               }
               else report_error("XPST0003: Expected literal target in processing-instruction()");
            }

            if (not match(XPathTokenType::RPAREN)) {
               report_error("XPST0003: Expected ')' after processing-instruction() test");
            }

            if (has_errors()) return nullptr;
            else return std::make_unique<XPathNode>(XQueryNodeType::PROCESSING_INSTRUCTION_TEST, target);
         }

         if (not match(XPathTokenType::RPAREN)) {
            report_error("XPST0003: Expected ')' after node type test");
            return nullptr;
         }

         return std::make_unique<XPathNode>(XQueryNodeType::NODE_TYPE_TEST, name);
      }

      advance();

      if (check(XPathTokenType::COLON)) {
         if (current_token + 1 < tokens.size() and is_identifier_token(tokens[current_token + 1])) {
            advance(); // consume ':'
            name = std::format("{}:{}", name, peek().text);
            advance();
         }
      }

      return std::make_unique<XPathNode>(XQueryNodeType::NAME_TEST, name);
   }

   return nullptr;
}

//********************************************************************************************************************
// Parses predicate expressions enclosed in square brackets, handling index predicates, content equality tests,
// attribute tests, and general expressions for filtering node sets.

std::unique_ptr<XPathNode> XPathParser::parse_predicate()
{
   if (not match(XPathTokenType::LBRACKET)) return nullptr;

   auto predicate = std::make_unique<XPathNode>(XQueryNodeType::PREDICATE);

   if (check(XPathTokenType::NUMBER)) {
      // Index predicate [1], [2], etc.
      std::string index(peek().text);
      advance();
      predicate->add_child(std::make_unique<XPathNode>(XQueryNodeType::NUMBER, index));
   }
   else if (check(XPathTokenType::EQUALS)) {
      // Content predicate [=value]
      advance(); // consume =
      auto content_value = parse_predicate_value();

      if (content_value) {
         auto content_test = std::make_unique<XPathNode>(XQueryNodeType::BINARY_OP, "content-equals");
         content_test->add_child(std::move(content_value));
         predicate->add_child(std::move(content_test));
      }
      else report_error("XPST0003: Expected literal after '=' in content predicate");
   }
   else if (check(XPathTokenType::AT)) {
      size_t attribute_token_index = current_token;
      advance(); // consume '@'

      bool handled_attribute = false;
      std::unique_ptr<XPathNode> attribute_expression;

      if (is_identifier_token(peek()) or check(XPathTokenType::WILDCARD)) {
         std::string attr_name(peek().text);
         advance();

         if (match(XPathTokenType::COLON)) {
            if (is_identifier_token(peek()) or check(XPathTokenType::WILDCARD)) {
               attr_name = std::format("{}:{}", attr_name, peek().text);
               advance();
            }
            else report_error("XPST0003: Expected identifier or wildcard after ':' in attribute name");
         }

         if (check(XPathTokenType::EQUALS) or check(XPathTokenType::RBRACKET)) {
            if (match(XPathTokenType::EQUALS)) {
               auto attr_value = parse_predicate_value();

               if (attr_value) {
                  auto attr_test = std::make_unique<XPathNode>(XQueryNodeType::BINARY_OP, "attribute-equals");
                  attr_test->add_child(std::make_unique<XPathNode>(XQueryNodeType::LITERAL, attr_name));
                  attr_test->add_child(std::move(attr_value));
                  attribute_expression = std::move(attr_test);
               }
               else report_error("XPST0003: Expected literal after '=' in attribute predicate");
            }
            else {
               auto attr_exists = std::make_unique<XPathNode>(XQueryNodeType::BINARY_OP, "attribute-exists");
               attr_exists->add_child(std::make_unique<XPathNode>(XQueryNodeType::LITERAL, attr_name));
               attribute_expression = std::move(attr_exists);
            }

            if (attribute_expression and check(XPathTokenType::RBRACKET)) {
               predicate->add_child(std::move(attribute_expression));
               handled_attribute = true;
            }
         }
      }

      if (not handled_attribute) {
         current_token = attribute_token_index;
         if (auto expression = parse_expr(); expression) predicate->add_child(std::move(expression));
      }
   }
   else { // Complex expression
      if (auto expression = parse_expr(); expression) {
         predicate->add_child(std::move(expression));
      }
   }

   if (has_errors()) return nullptr;

   (void)match(XPathTokenType::RBRACKET); // consume closing bracket
   return predicate;
}

//********************************************************************************************************************
// Parses values within predicates, handling strings, identifiers, numbers, wildcards, and variable references.

std::unique_ptr<XPathNode> XPathParser::parse_predicate_value()
{
   if (check(XPathTokenType::STRING)) {
      std::string value(peek().text);
      advance();
      return std::make_unique<XPathNode>(XQueryNodeType::LITERAL, value);
   }

   if (is_identifier_token(peek()) or check(XPathTokenType::NUMBER)) {
      std::string value(peek().text);
      advance();

      while (check(XPathTokenType::MULTIPLY) or check(XPathTokenType::WILDCARD)) {
         value += '*';
         advance();
      }

      return std::make_unique<XPathNode>(XQueryNodeType::LITERAL, value);
   }

   if (check(XPathTokenType::DOLLAR)) return parse_variable_reference();

   return nullptr;
}

//********************************************************************************************************************
// Parses a single XPath expression, dispatching to control flow (if, for, let, some, every) or operator parsing.

std::unique_ptr<XPathNode> XPathParser::parse_expr_single()
{
   if (check(XPathTokenType::IF)) return parse_if_expr();

   if (check_identifier_keyword("typeswitch")) return parse_typeswitch_expr();

   if (check(XPathTokenType::FOR) or check(XPathTokenType::LET) or check_identifier_keyword("let")) {
      return parse_flwor_expr();
   }

   if (check(XPathTokenType::SOME) or check(XPathTokenType::EVERY)) {
      return parse_quantified_expr();
   }

   return parse_or_expr();
}

//********************************************************************************************************************
// Parses comma-separated expressions building a sequence from multiple expressions.

std::unique_ptr<XPathNode> XPathParser::parse_expr()
{
   auto expression = parse_expr_single();
   if (not expression) return nullptr;

   while (match(XPathTokenType::COMMA)) {
      XPathToken comma = previous();
      auto right = parse_expr_single();
      if (not right) return nullptr;
      expression = create_binary_op(std::move(expression), comma, std::move(right));
   }

   return expression;
}

//********************************************************************************************************************
// Parses FLWOR (For, Let, Where, Order by, Return) expressions with ordered optional clauses.

std::unique_ptr<XPathNode> XPathParser::parse_flwor_expr()
{
   std::vector<std::unique_ptr<XPathNode>> binding_nodes;
   std::vector<std::unique_ptr<XPathNode>> clause_nodes;
   bool saw_for = false;
   bool saw_let = false;

   while (true) {
      if (match(XPathTokenType::FOR)) {
         saw_for = true;

         bool expect_binding = true;
         while (expect_binding) {
            if (not match(XPathTokenType::DOLLAR)) {
               report_error("XPST0003: Expected '$' after 'for'");
               return nullptr;
            }

            auto variable_name_opt = parse_qname_string();
            if (not variable_name_opt) return nullptr;
            std::string variable_name = *variable_name_opt;

            if (not match(XPathTokenType::IN)) {
               report_error("XPST0003: Expected 'in' in for expression");
               return nullptr;
            }

            auto sequence_expr = parse_expr_single();
            if (not sequence_expr) return nullptr;

            auto binding_node = std::make_unique<XPathNode>(XQueryNodeType::FOR_BINDING, variable_name);
            binding_node->add_child(std::move(sequence_expr));
            binding_nodes.push_back(std::move(binding_node));

            expect_binding = match(XPathTokenType::COMMA);
         }

         continue;
      }

      if (check(XPathTokenType::LET) or check_identifier_keyword("let")) {
         XPathToken let_token(XPathTokenType::UNKNOWN, std::string_view());
         if (not match_identifier_keyword("let", XPathTokenType::LET, let_token)) {
            report_error("XPST0003: Expected 'let' expression");
            return nullptr;
         }

         saw_let = true;

         bool parsing_bindings = true;
         while (parsing_bindings) {
            if (not match(XPathTokenType::DOLLAR)) {
               report_error("XPST0003: Expected '$' after 'let'");
               return nullptr;
            }

            auto variable_name_opt = parse_qname_string();
            if (not variable_name_opt) return nullptr;
            std::string variable_name = *variable_name_opt;

            if (not match(XPathTokenType::ASSIGN)) {
               report_error("XPST0003: Expected ':=' in let binding");
               return nullptr;
            }

            // Save current position to detect if we consumed a structural keyword as an element name
            size_t expr_start_pos = current_token;

            auto binding_expr = parse_expr_single();
            if (not binding_expr) {
               report_error("XPST0003: Expected expression after ':=' in let binding");
               return nullptr;
            }

            // Check if the expression only consumed a single token that's a FLWOR structural keyword.
            // This indicates the parser mistakenly treated a keyword like 'return' as an element name.

            if ((current_token IS expr_start_pos + 1) and (expr_start_pos < tokens.size())) {
               const auto &consumed_token = tokens[expr_start_pos];
               std::string_view keyword = keyword_from_token_type(consumed_token.type);
               if (keyword IS "return" or keyword IS "where" or keyword IS "group" or
                   keyword IS "order" or keyword IS "count" or keyword IS "stable") {
                  report_error("XPST0003: Expected expression after ':=' in let binding");
                  return nullptr;
               }
            }

            auto binding_node = std::make_unique<XPathNode>(XQueryNodeType::LET_BINDING, variable_name);
            binding_node->add_child(std::move(binding_expr));
            binding_nodes.push_back(std::move(binding_node));

            parsing_bindings = match(XPathTokenType::COMMA);
         }

         continue;
      }

      break;
   }

   if (binding_nodes.empty()) {
      report_error("XPST0003: Expected 'for' or 'let' expression");
      return nullptr;
   }

   bool saw_where = false;
   bool saw_group = false;
   bool saw_order = false;
   bool saw_count_clause = false;
   bool has_non_binding_clause = false;

   while (true) {
      if (check_identifier_keyword("where")) {
         if (saw_where) {
            report_error("XPST0003: Multiple where clauses are not permitted in FLWOR expression");
            return nullptr;
         }

         if (saw_group) {
            report_error("XPST0003: where clause must precede group by clause");
            return nullptr;
         }

         if (saw_order) {
            report_error("XPST0003: where clause must precede order by clause");
            return nullptr;
         }

         if (saw_count_clause) {
            report_error("XPST0003: where clause must precede count clause");
            return nullptr;
         }

         auto where_clause = parse_where_clause();
         if (not where_clause) return nullptr;
         clause_nodes.push_back(std::move(where_clause));
         saw_where = true;
         has_non_binding_clause = true;
         continue;
      }

      if (check_identifier_keyword("group")) {
         if (saw_group) {
            report_error("XPST0003: Multiple group by clauses are not permitted in FLWOR expression");
            return nullptr;
         }

         if (saw_order) {
            report_error("XPST0003: group by clause must precede order by clause");
            return nullptr;
         }

         if (saw_count_clause) {
            report_error("XPST0003: group by clause must precede count clause");
            return nullptr;
         }

         auto group_clause = parse_group_clause();
         if (not group_clause) return nullptr;
         clause_nodes.push_back(std::move(group_clause));
         saw_group = true;
         has_non_binding_clause = true;
         continue;
      }

      if (check_identifier_keyword("stable")) {
         if (saw_order) {
            report_error("XPST0003: Multiple order by clauses are not permitted in FLWOR expression");
            return nullptr;
         }

         if (saw_count_clause) {
            report_error("XPST0003: order by clause must precede count clause");
            return nullptr;
         }

         auto order_clause = parse_order_clause(true);
         if (not order_clause) return nullptr;
         clause_nodes.push_back(std::move(order_clause));
         saw_order = true;
         has_non_binding_clause = true;
         continue;
      }

      if (check_identifier_keyword("order")) {
         if (saw_order) {
            report_error("XPST0003: Multiple order by clauses are not permitted in FLWOR expression");
            return nullptr;
         }

         if (saw_count_clause) {
            report_error("XPST0003: order by clause must precede count clause");
            return nullptr;
         }

         auto order_clause = parse_order_clause(false);
         if (not order_clause) return nullptr;
         clause_nodes.push_back(std::move(order_clause));
         saw_order = true;
         has_non_binding_clause = true;
         continue;
      }

      if (check_identifier_keyword("count")) {
         if (saw_count_clause) {
            report_error("XPST0003: Multiple count clauses are not permitted in FLWOR expression");
            return nullptr;
         }

         auto count_clause = parse_count_clause();
         if (not count_clause) return nullptr;
         clause_nodes.push_back(std::move(count_clause));
         saw_count_clause = true;
         has_non_binding_clause = true;
         continue;
      }

      break;
   }

   XPathToken return_token(XPathTokenType::UNKNOWN, std::string_view());
   if (not match_identifier_keyword("return", XPathTokenType::RETURN, return_token)) {
      report_error("XPST0003: Expected 'return' in FLWOR expression");
      return nullptr;
   }

   auto return_expr = parse_expr_single();
   if (not return_expr) {
      report_error("XPST0003: Expected expression after 'return'");
      return nullptr;
   }

   if (not has_non_binding_clause and saw_for and !saw_let) {
      auto for_node = std::make_unique<XPathNode>(XQueryNodeType::FOR_EXPRESSION);
      for (auto &binding : binding_nodes) {
         if ((not binding) or !(binding->type IS XQueryNodeType::FOR_BINDING)) {
            report_error("XPST0003: Invalid for binding in FLWOR expression");
            return nullptr;
         }
         for_node->add_child(std::move(binding));
      }
      for_node->add_child(std::move(return_expr));
      return for_node;
   }

   if (not has_non_binding_clause and saw_let and !saw_for) {
      auto let_node = std::make_unique<XPathNode>(XQueryNodeType::LET_EXPRESSION);
      for (auto &binding : binding_nodes) {
         if ((not binding) or !(binding->type IS XQueryNodeType::LET_BINDING)) {
            report_error("XPST0003: Invalid let binding in FLWOR expression");
            return nullptr;
         }
         let_node->add_child(std::move(binding));
      }
      let_node->add_child(std::move(return_expr));
      return let_node;
   }

   auto flwor_node = std::make_unique<XPathNode>(XQueryNodeType::FLWOR_EXPRESSION);
   for (auto &binding : binding_nodes) {
      if (not binding) {
         report_error("XPST0003: Invalid clause in FLWOR expression");
         return nullptr;
      }
      flwor_node->add_child(std::move(binding));
   }

   for (auto &clause : clause_nodes) {
      if (not clause) {
         report_error("XPST0003: Invalid clause in FLWOR expression");
         return nullptr;
      }
      flwor_node->add_child(std::move(clause));
   }
   flwor_node->add_child(std::move(return_expr));
   return flwor_node;
}

//********************************************************************************************************************
// Parses a where clause in a FLWOR expression, consuming the 'where' keyword and filtering condition expression.

std::unique_ptr<XPathNode> XPathParser::parse_where_clause()
{
   XPathToken where_token(XPathTokenType::UNKNOWN, std::string_view());
   if (not match_identifier_keyword("where", XPathTokenType::WHERE, where_token)) {
      report_error("XPST0003: Expected 'where' clause");
      return nullptr;
   }

   auto predicate = parse_expr_single();
   if (not predicate) {
      report_error("XPST0003: Expected expression after 'where'");
      return nullptr;
   }

   auto clause = std::make_unique<XPathNode>(XQueryNodeType::WHERE_CLAUSE);
   clause->add_child(std::move(predicate));
   return clause;
}

//********************************************************************************************************************
// Parses a group by clause with comma-separated variable bindings and key expressions for grouping.

std::unique_ptr<XPathNode> XPathParser::parse_group_clause()
{
   XPathToken group_token(XPathTokenType::UNKNOWN, std::string_view());
   if (not match_identifier_keyword("group", XPathTokenType::GROUP, group_token)) {
      report_error("XPST0003: Expected 'group' clause");
      return nullptr;
   }

   XPathToken by_token(XPathTokenType::UNKNOWN, std::string_view());
   if (not match_identifier_keyword("by", XPathTokenType::BY, by_token)) {
      report_error("XPST0003: Expected 'by' after 'group'");
      return nullptr;
   }

   auto clause = std::make_unique<XPathNode>(XQueryNodeType::GROUP_CLAUSE);

   bool expect_key = true;
   while (expect_key) {
      if (not match(XPathTokenType::DOLLAR)) {
         report_error("XPST0003: Expected '$' to begin group by key binding");
         return nullptr;
      }

      auto variable_name_opt = parse_qname_string();
      if (not variable_name_opt) return nullptr;
      std::string variable_name = *variable_name_opt;

      if (not match(XPathTokenType::ASSIGN)) {
         report_error("XPST0003: Expected ':=' after group by variable name");
         return nullptr;
      }

      auto key_expr = parse_expr_single();
      if (not key_expr) {
         report_error("XPST0003: Expected expression after ':=' in group by clause");
         return nullptr;
      }

      auto key_node = std::make_unique<XPathNode>(XQueryNodeType::GROUP_KEY);
      XPathGroupKeyInfo info;
      info.variable_name = std::move(variable_name);
      key_node->set_group_key_info(std::move(info));
      key_node->add_child(std::move(key_expr));
      clause->add_child(std::move(key_node));

      expect_key = match(XPathTokenType::COMMA);
   }

   return clause;
}

//********************************************************************************************************************
// Parses an order by clause with optional stability modifier and comma-separated ordering specifications.

std::unique_ptr<XPathNode> XPathParser::parse_order_clause(bool StartsWithStable)
{
   bool clause_is_stable = false;

   if (StartsWithStable) {
      XPathToken stable_token(XPathTokenType::UNKNOWN, std::string_view());
      if (not match_identifier_keyword("stable", XPathTokenType::STABLE, stable_token)) {
         report_error("XPST0003: Expected 'stable' keyword to start stable order by clause");
         return nullptr;
      }
      clause_is_stable = true;
   }

   XPathToken order_token(XPathTokenType::UNKNOWN, std::string_view());
   if (not match_identifier_keyword("order", XPathTokenType::ORDER, order_token)) {
      report_error("XPST0003: Expected 'order' in order by clause");
      return nullptr;
   }

   XPathToken by_token(XPathTokenType::UNKNOWN, std::string_view());
   if (not match_identifier_keyword("by", XPathTokenType::BY, by_token)) {
      report_error("XPST0003: Expected 'by' after 'order'");
      return nullptr;
   }

   auto clause = std::make_unique<XPathNode>(XQueryNodeType::ORDER_CLAUSE);
   clause->order_clause_is_stable = clause_is_stable;

   auto first_spec = parse_order_spec();
   if (not first_spec) return nullptr;
   clause->add_child(std::move(first_spec));

   while (match(XPathTokenType::COMMA)) {
      auto next_spec = parse_order_spec();
      if (not next_spec) return nullptr;
      clause->add_child(std::move(next_spec));
   }

   return clause;
}

//********************************************************************************************************************
// Parses an individual order specification with direction and empty value handling options.

std::unique_ptr<XPathNode> XPathParser::parse_order_spec()
{
   auto order_expr = parse_expr_single();
   if (not order_expr) {
      report_error("XPST0003: Expected expression in order by clause");
      return nullptr;
   }

   auto spec_node = std::make_unique<XPathNode>(XQueryNodeType::ORDER_SPEC);
   spec_node->add_child(std::move(order_expr));

   XPathOrderSpecOptions options;
   bool has_options = false;

   XPathToken keyword_token(XPathTokenType::UNKNOWN, std::string_view());
   if (match_identifier_keyword("ascending", XPathTokenType::ASCENDING, keyword_token)) {
      has_options = true;
      options.is_descending = false;
   }
   else if (match_identifier_keyword("descending", XPathTokenType::DESCENDING, keyword_token)) {
      has_options = true;
      options.is_descending = true;
   }

   if (match_identifier_keyword("empty", XPathTokenType::EMPTY, keyword_token)) {
      has_options = true;
      options.has_empty_mode = true;
      if (match_identifier_keyword("greatest", XPathTokenType::GREATEST, keyword_token)) {
         options.empty_is_greatest = true;
      }
      else if (match_identifier_keyword("least", XPathTokenType::LEAST, keyword_token)) {
         options.empty_is_greatest = false;
      }
      else {
         report_error("XPST0003: Expected 'greatest' or 'least' after 'empty' in order by clause");
         return nullptr;
      }
   }

   if (match_identifier_keyword("collation", XPathTokenType::COLLATION, keyword_token)) {
      has_options = true;
      if (not check(XPathTokenType::STRING)) {
         report_error("XPST0003: Expected string literal after 'collation' in order by clause");
         return nullptr;
      }

      std::string collation_value(peek().text);
      advance();
      options.collation_uri = std::move(collation_value);
   }

   if (has_options) spec_node->set_order_spec_options(std::move(options));
   return spec_node;
}

//********************************************************************************************************************
// Parses a count clause binding a variable to a position counter in a FLWOR expression.

std::unique_ptr<XPathNode> XPathParser::parse_count_clause()
{
   XPathToken count_token(XPathTokenType::UNKNOWN, std::string_view());
   if (not match_identifier_keyword("count", XPathTokenType::COUNT, count_token)) {
      report_error("XPST0003: Expected 'count' clause");
      return nullptr;
   }

   if (not match(XPathTokenType::DOLLAR)) {
      report_error("XPST0003: Expected '$' after 'count'");
      return nullptr;
   }

   auto variable_name_opt = parse_qname_string();
   if (not variable_name_opt) return nullptr;

   return std::make_unique<XPathNode>(XQueryNodeType::COUNT_CLAUSE, *variable_name_opt);
}

//********************************************************************************************************************
// Parses logical OR expressions, building left-associative binary operation trees from 'or' operators.

std::unique_ptr<XPathNode> XPathParser::parse_or_expr()
{
   auto left = parse_and_expr();

   while (check(XPathTokenType::OR)) {
      XPathToken op = peek();
      advance();
      auto right = parse_and_expr();
      left = create_binary_op(std::move(left), op, std::move(right));
   }

   return left;
}

//********************************************************************************************************************
// Parses logical AND expressions, building left-associative binary operation trees from 'and' operators.

std::unique_ptr<XPathNode> XPathParser::parse_and_expr()
{
   auto left = parse_equality_expr();

   while (check(XPathTokenType::AND)) {
      XPathToken op = peek();
      advance();
      auto right = parse_equality_expr();
      left = create_binary_op(std::move(left), op, std::move(right));
   }

   return left;
}

//********************************************************************************************************************
// Parses equality expressions, handling =, !=, eq, and ne operators with left-associative binding.

std::unique_ptr<XPathNode> XPathParser::parse_equality_expr() {
   auto left = parse_relational_expr();

   while (check(XPathTokenType::EQUALS) or check(XPathTokenType::NOT_EQUALS) or
          check(XPathTokenType::EQ) or check(XPathTokenType::NE)) {
      XPathToken op = peek();
      advance();
      auto right = parse_relational_expr();
      left = create_binary_op(std::move(left), op, std::move(right));
   }

   return left;
}

//********************************************************************************************************************
// Parses relational comparison expressions, handling <, <=, >, >=, lt, le, gt, and ge operators.

std::unique_ptr<XPathNode> XPathParser::parse_relational_expr()
{
   auto left = parse_instance_of_expr();

   while (check(XPathTokenType::LESS_THAN) or check(XPathTokenType::LESS_EQUAL) or
          check(XPathTokenType::GREATER_THAN) or check(XPathTokenType::GREATER_EQUAL) or
          check(XPathTokenType::LT) or check(XPathTokenType::LE) or
          check(XPathTokenType::GT) or check(XPathTokenType::GE)) {
      XPathToken op = peek();
      advance();
      auto right = parse_instance_of_expr();
      left = create_binary_op(std::move(left), op, std::move(right));
   }

   return left;
}

//********************************************************************************************************************
// Parses instance of expressions of the form Expr instance of SequenceType.

std::unique_ptr<XPathNode> XPathParser::parse_instance_of_expr()
{
   auto operand = parse_range_expr();
   if (not operand) return nullptr;

   while (true) {
      XPathToken instance_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
      if (not match_identifier_keyword("instance", XPathTokenType::INSTANCE, instance_token)) break;

      if (not match_literal_keyword("of")) {
         report_error("XPST0003: Expected 'of' after 'instance'.");
         return nullptr;
      }

      auto sequence_type_literal = collect_sequence_type();
      if (not sequence_type_literal) {
         report_error("XPST0003: Expected sequence type after 'instance of'.");
         return nullptr;
      }

      std::string sequence_type = *sequence_type_literal;
      if (sequence_type.empty()) {
         report_error("XPST0003: Instance of expression requires a sequence type.");
         return nullptr;
      }

      auto instance_node = std::make_unique<XPathNode>(XQueryNodeType::INSTANCE_OF_EXPRESSION, std::move(sequence_type));
      instance_node->add_child(std::move(operand));
      operand = std::move(instance_node);
   }

   return operand;
}

//********************************************************************************************************************
// Parses range expressions of the form Expr to Expr, binding less tightly than additive operators.

std::unique_ptr<XPathNode> XPathParser::parse_range_expr()
{
   auto left = parse_additive_expr();
   if (not left) return nullptr;

   XPathToken to_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
   if (not match_identifier_keyword("to", XPathTokenType::TO, to_token)) return left;

   auto right = parse_additive_expr();
   if (not right) {
      report_error("XPST0003: Range expression requires an expression after 'to'.");
      return nullptr;
   }

   return create_binary_op(std::move(left), to_token, std::move(right));
}

//********************************************************************************************************************
// Parses additive expressions for + and - operators with left-associative binding.

std::unique_ptr<XPathNode> XPathParser::parse_additive_expr()
{
   auto left = parse_multiplicative_expr();

   while (check(XPathTokenType::PLUS) or check(XPathTokenType::MINUS)) {
      XPathToken op = peek();
      advance();
      auto right = parse_multiplicative_expr();
      left = create_binary_op(std::move(left), op, std::move(right));
   }

   return left;
}

//********************************************************************************************************************
// Parses multiplicative expressions for *, div, and mod operators with left-associative binding.

std::unique_ptr<XPathNode> XPathParser::parse_multiplicative_expr() {
   auto left = parse_cast_expr();

   while (check(XPathTokenType::MULTIPLY) or check(XPathTokenType::DIVIDE) or
          check(XPathTokenType::MODULO)) {
      XPathToken op = peek();
      advance();
      auto right = parse_cast_expr();
      left = create_binary_op(std::move(left), op, std::move(right));
   }

   return left;
}

//********************************************************************************************************************
// Parses cast expressions of the form Expr cast as SingleType.

std::unique_ptr<XPathNode> XPathParser::parse_cast_expr()
{
   auto operand = parse_unary_expr();
   if (not operand) return nullptr;

   XPathToken keyword_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
   bool is_cast = false;

   if (match_identifier_keyword("cast", XPathTokenType::CAST, keyword_token)) is_cast = true;
   else if (not match_identifier_keyword("castable", XPathTokenType::CASTABLE, keyword_token)) return operand;

   if (not match_literal_keyword("as")) {
      if (is_cast) report_error("XPST0003: Expected 'as' after 'cast'.");
      else report_error("XPST0003: Expected 'as' after 'castable'.");
      return nullptr;
   }

   auto type_name = parse_qname_string();
   if (not type_name) {
      report_error("XPST0003: Expected type name after 'as'.");
      return nullptr;
   }

   std::string target_type(trim_view(*type_name));
   if (target_type.empty()) {
      if (is_cast) report_error("XPST0003: Cast expression requires a target type.");
      else report_error("XPST0003: Castable expression requires a target type.");
      return nullptr;
   }

   if (match(XPathTokenType::QUESTION_MARK)) target_type.push_back('?');

   auto node_type = is_cast ? XQueryNodeType::CAST_EXPRESSION : XQueryNodeType::CASTABLE_EXPRESSION;
   auto cast_node = std::make_unique<XPathNode>(node_type, std::move(target_type));
   cast_node->add_child(std::move(operand));
   return cast_node;
}

//********************************************************************************************************************
// Parses unary expressions, handling unary minus and logical NOT operators, allowing recursive unary operator
// application.

std::unique_ptr<XPathNode> XPathParser::parse_unary_expr()
{
   if (match(XPathTokenType::MINUS)) {
      XPathToken op = previous();
      auto operand = parse_unary_expr();
      return create_unary_op(op, std::move(operand));
   }

   if (check(XPathTokenType::NOT)) {
      XPathToken op = peek();
      advance();

      std::unique_ptr<XPathNode> operand;
      if (match(XPathTokenType::LPAREN)) {
         operand = parse_expr();
         (void)match(XPathTokenType::RPAREN);
      }
      else operand = parse_unary_expr();

      return create_unary_op(op, std::move(operand));
   }

   auto operand = parse_union_expr();
   if (not operand) return nullptr;

   while (true) {
      XPathToken treat_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
      if (not match_identifier_keyword("treat", XPathTokenType::TREAT, treat_token)) break;

      if (not match_literal_keyword("as")) {
         report_error("XPST0003: Expected 'as' after 'treat'.");
         return nullptr;
      }

      auto sequence_type_literal = collect_sequence_type();
      if (not sequence_type_literal) {
         report_error("XPST0003: Expected sequence type after 'treat as'.");
         return nullptr;
      }

      std::string sequence_type = *sequence_type_literal;
      if (sequence_type.empty()) {
         report_error("XPST0003: Treat as expression requires a sequence type.");
         return nullptr;
      }

      auto treat_node = std::make_unique<XPathNode>(XQueryNodeType::TREAT_AS_EXPRESSION, std::move(sequence_type));
      treat_node->add_child(std::move(operand));
      operand = std::move(treat_node);
   }

   return operand;
}

//********************************************************************************************************************
// Parses set intersection and exception expressions using 'intersect' and 'except' operators.

std::unique_ptr<XPathNode> XPathParser::parse_intersect_expr() {
   auto left = parse_path_expr();
   if (not left) return nullptr;

   while (true) {
      XPathToken op(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
      if (not match_identifier_keyword("intersect", XPathTokenType::INTERSECT, op)) {
         if (not match_identifier_keyword("except", XPathTokenType::EXCEPT, op)) break;
      }

      auto right = parse_path_expr();
      if (not right) {
         report_error("XPST0003: Expected expression after set operator");
         return nullptr;
      }

      left = create_binary_op(std::move(left), op, std::move(right));
   }

   return left;
}

//********************************************************************************************************************
// Parses union expressions combining multiple node sets with the '|' or 'union' operator into a single UNION node.

std::unique_ptr<XPathNode> XPathParser::parse_union_expr()
{
   auto left = parse_intersect_expr();
   if (not left) return nullptr;

   if (not check(XPathTokenType::PIPE) and !check_identifier_keyword("union")) return left;

   auto union_node = std::make_unique<XPathNode>(XQueryNodeType::UNION);
   union_node->add_child(std::move(left));

   while (true) {
      if (not match(XPathTokenType::PIPE)) {
         XPathToken union_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
         if (not match_identifier_keyword("union", XPathTokenType::UNION, union_token)) break;
      }

      auto branch = parse_intersect_expr();
      if (not branch) {
         report_error("XPST0003: Expected expression after union operator");
         return nullptr;
      }
      union_node->add_child(std::move(branch));
   }

   return union_node;
}

//********************************************************************************************************************
// Parses path expressions, distinguishing between location paths and filter expressions by examining token patterns.

std::unique_ptr<XPathNode> XPathParser::parse_path_expr()
{
   bool looks_like_path = false;

   if (check(XPathTokenType::SLASH) or check(XPathTokenType::DOUBLE_SLASH)) looks_like_path = true;
   else if (is_step_start_token(peek().type)) {
      looks_like_path = true;

      if (is_function_call_ahead(current_token)) looks_like_path = false;
      else if (is_identifier_token(peek())) {
         if (is_constructor_keyword(peek())) {
            size_t lookahead = current_token + 1;
            while ((lookahead < tokens.size()) and is_identifier_token(tokens[lookahead])) {
               lookahead++;
            }

            if (lookahead < tokens.size()) {
               auto next_type = tokens[lookahead].type;
               if ((next_type IS XPathTokenType::LBRACE) or (next_type IS XPathTokenType::STRING)) {
                  looks_like_path = false;
               }
            }
         }
      }
   }

   if (looks_like_path) {
      auto location = parse_location_path();
      if (not location) return nullptr;

      auto path_node = std::make_unique<XPathNode>(XQueryNodeType::PATH);
      path_node->add_child(std::move(location));
      return path_node;
   }

   return parse_filter_expr();
}

//********************************************************************************************************************
// Parses filter expressions consisting of a primary expression optionally followed by predicates and path
// continuations using / or // operators.

std::unique_ptr<XPathNode> XPathParser::parse_filter_expr()
{
   auto primary = parse_primary_expr();
   if (not primary) return nullptr;

   std::unique_ptr<XPathNode> current = std::move(primary);

   bool has_predicate = false;
   XPathNode *predicate_node = nullptr;
   while (true) {
      if (check(XPathTokenType::LBRACKET)) {
         auto predicate = parse_predicate();
         if (not predicate) return nullptr;

         if (not has_predicate) {
            auto filter = std::make_unique<XPathNode>(XQueryNodeType::FILTER);
            filter->add_child(std::move(current));
            predicate_node = filter.get();
            current = std::move(filter);
            has_predicate = true;
         }

         predicate_node->add_child(std::move(predicate));
         continue;
      }

      if (check(XPathTokenType::LOOKUP)) {
         auto lookup_expr = parse_lookup_expression(std::move(current));
         if (not lookup_expr) return nullptr;
         current = std::move(lookup_expr);
         has_predicate = false;
         predicate_node = nullptr;
         continue;
      }

      break;
   }

   while (true) {
      XPathTokenType slash_type = XPathTokenType::UNKNOWN;

      if (match(XPathTokenType::SLASH)) slash_type = XPathTokenType::SLASH;
      else if (match(XPathTokenType::DOUBLE_SLASH)) slash_type = XPathTokenType::DOUBLE_SLASH;

      if (slash_type IS XPathTokenType::UNKNOWN) break;

      auto relative = parse_location_path();
      if (not relative) return nullptr;

      auto path_node = std::make_unique<XPathNode>(XQueryNodeType::PATH);
      path_node->add_child(std::move(current));

      if (slash_type IS XPathTokenType::DOUBLE_SLASH) {
         auto descendant_step = std::make_unique<XPathNode>(XQueryNodeType::STEP);
         descendant_step->add_child(std::make_unique<XPathNode>(XQueryNodeType::AXIS_SPECIFIER, "descendant-or-self"));
         descendant_step->add_child(std::make_unique<XPathNode>(XQueryNodeType::NODE_TYPE_TEST, "node"));
         path_node->add_child(std::move(descendant_step));
      }

      for (auto &child : relative->children) {
         if (child and (child->type IS XQueryNodeType::STEP)) {
            path_node->add_child(std::move(child));
         }
      }

      current = std::move(path_node);
   }

   return current;
}

//********************************************************************************************************************
// Parses chained lookup operators applied to a base expression, accumulating lookup specifiers on the node.

std::unique_ptr<XPathNode> XPathParser::parse_lookup_expression(std::unique_ptr<XPathNode> Base)
{
   if (not Base) return nullptr;

   auto lookup_node = std::make_unique<XPathNode>(XQueryNodeType::LOOKUP_EXPRESSION);
   lookup_node->add_child(std::move(Base));

   bool consumed = false;
   while (match(XPathTokenType::LOOKUP)) {
      auto specifier = parse_lookup_specifier();
      if (not specifier) return nullptr;
      lookup_node->add_lookup_specifier(std::move(*specifier));
      consumed = true;
   }

   if (not consumed) {
      report_error("XPST0003: Lookup operator requires a key");
      return nullptr;
   }

   return lookup_node;
}

//********************************************************************************************************************
// Parses the lookup key specifier which may be an NCName, wildcard, integer literal, or a parenthesised expression.

std::optional<XPathLookupSpecifier> XPathParser::parse_lookup_specifier()
{
   XPathLookupSpecifier specifier;

   if (match(XPathTokenType::LPAREN)) {
      auto expr = parse_expr();
      if (not expr) return std::nullopt;

      if (not match(XPathTokenType::RPAREN)) {
         report_error("XPST0003: Expected ')' after lookup expression");
         return std::nullopt;
      }

      specifier.kind = XPathLookupSpecifierKind::Expression;
      specifier.expression = std::move(expr);
      return specifier;
   }

   if (check(XPathTokenType::WILDCARD)) {
      specifier.kind = XPathLookupSpecifierKind::Wildcard;
      specifier.literal_value = "*";
      advance();
      return specifier;
   }

   if (check(XPathTokenType::NUMBER)) {
      specifier.kind = XPathLookupSpecifierKind::IntegerLiteral;
      specifier.literal_value = std::string(peek().text);
      advance();
      return specifier;
   }

   if (is_identifier_token(peek())) {
      auto name = parse_ncname();
      if (not name) return std::nullopt;

      if (check(XPathTokenType::COLON)) {
         report_error("XPST0003: Lookup NCName must not include ':'");
         return std::nullopt;
      }

      specifier.kind = XPathLookupSpecifierKind::NCName;
      specifier.literal_value = std::move(*name);
      return specifier;
   }

   report_error("XPST0003: Invalid lookup specifier");
   return std::nullopt;
}

//********************************************************************************************************************
// Parses conditional if-then-else expressions with mandatory condition, then branch, and else branch.

std::unique_ptr<XPathNode> XPathParser::parse_if_expr()
{
   if (not match(XPathTokenType::IF)) return nullptr;

   if (not match(XPathTokenType::LPAREN)) {
      report_error("XPST0003: Expected '(' after 'if'");
      return nullptr;
   }

   auto condition = parse_expr();

   if (not match(XPathTokenType::RPAREN)) {
      report_error("XPST0003: Expected ')' after condition in if expression");
      return nullptr;
   }

   if (not match(XPathTokenType::THEN)) {
      report_error("XPST0003: Expected 'then' in if expression");
      return nullptr;
   }

   auto then_branch = parse_expr_single();

   if (not match(XPathTokenType::ELSE)) {
      report_error("XPST0003: Expected 'else' in if expression");
      return nullptr;
   }

   auto else_branch = parse_expr_single();

   auto conditional = std::make_unique<XPathNode>(XQueryNodeType::CONDITIONAL);
   conditional->add_child(std::move(condition));
   conditional->add_child(std::move(then_branch));
   conditional->add_child(std::move(else_branch));
   return conditional;
}

//********************************************************************************************************************
// Parses XPath 2.0 typeswitch expressions including case clauses and the mandatory default clause.

std::unique_ptr<XPathNode> XPathParser::parse_typeswitch_expr()
{
   XPathToken keyword_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
   if (not match_identifier_keyword("typeswitch", XPathTokenType::TYPESWITCH, keyword_token)) {
      report_error("XPST0003: Expected 'typeswitch' expression.");
      return nullptr;
   }

   if (not match(XPathTokenType::LPAREN)) {
      report_error("XPST0003: Expected '(' after 'typeswitch'.");
      return nullptr;
   }

   auto operand_expr = parse_expr();
   if (not operand_expr) return nullptr;

   if (not match(XPathTokenType::RPAREN)) {
      report_error("XPST0003: Expected ')' after typeswitch operand.");
      return nullptr;
   }

   std::vector<std::unique_ptr<XPathNode>> case_nodes;
   bool saw_case_clause = false;

   while (true) {
      XPathToken case_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
      if (not match_identifier_keyword("case", XPathTokenType::CASE, case_token)) break;

      saw_case_clause = true;

      std::string variable_name;
      if (match(XPathTokenType::DOLLAR)) {
         auto variable_qname = parse_qname_string();
         if (not variable_qname) return nullptr;
         variable_name = *variable_qname;

         if (not match_literal_keyword("as")) {
            report_error("XPST0003: Expected 'as' after case variable name.");
            return nullptr;
         }
      }

      auto sequence_literal = collect_sequence_type(true);
      if (not sequence_literal) {
         report_error("XPST0003: Case clause requires a sequence type.");
         return nullptr;
      }

      std::string sequence_type(trim_view(*sequence_literal));
      if (sequence_type.empty()) {
         report_error("XPST0003: Case clause requires a sequence type.");
         return nullptr;
      }

      XPathToken return_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
      if (not match_identifier_keyword("return", XPathTokenType::RETURN, return_token)) {
         report_error("XPST0003: Expected 'return' in typeswitch case clause.");
         return nullptr;
      }

      auto branch_expr = parse_expr_single();
      if (not branch_expr) return nullptr;

      auto case_node = std::make_unique<XPathNode>(XQueryNodeType::TYPESWITCH_CASE);
      XPathTypeswitchCaseInfo info;
      info.variable_name = std::move(variable_name);
      info.sequence_type = std::move(sequence_type);
      case_node->set_typeswitch_case_info(std::move(info));
      case_node->add_child(std::move(branch_expr));
      case_nodes.push_back(std::move(case_node));
   }

   if (not saw_case_clause) {
      report_error("XPST0003: Typeswitch expression requires at least one case clause.");
      return nullptr;
   }

   XPathToken default_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
   if (not match_identifier_keyword("default", XPathTokenType::DEFAULT, default_token)) {
      report_error("XPST0003: Typeswitch expression requires a default clause.");
      return nullptr;
   }

   std::string default_variable;
   if (match(XPathTokenType::DOLLAR)) {
      auto variable_qname = parse_qname_string();
      if (not variable_qname) return nullptr;
      default_variable = *variable_qname;
   }

   XPathToken return_token(XPathTokenType::UNKNOWN, std::string_view(), 0, 0);
   if (not match_identifier_keyword("return", XPathTokenType::RETURN, return_token)) {
      report_error("XPST0003: Expected 'return' in typeswitch default clause.");
      return nullptr;
   }

   auto default_expr = parse_expr_single();
   if (not default_expr) return nullptr;

   auto typeswitch_node = std::make_unique<XPathNode>(XQueryNodeType::TYPESWITCH_EXPRESSION);
   typeswitch_node->add_child(std::move(operand_expr));

   for (auto &node : case_nodes) typeswitch_node->add_child(std::move(node));

   auto default_node = std::make_unique<XPathNode>(XQueryNodeType::TYPESWITCH_DEFAULT_CASE);
   XPathTypeswitchCaseInfo default_info;
   default_info.variable_name = std::move(default_variable);
   default_info.is_default = true;
   default_node->set_typeswitch_case_info(std::move(default_info));
   default_node->add_child(std::move(default_expr));
   typeswitch_node->add_child(std::move(default_node));

   return typeswitch_node;
}

//********************************************************************************************************************
// Parses quantified expressions using 'some' or 'every' keywords with variable bindings and a 'satisfies' condition.

std::unique_ptr<XPathNode> XPathParser::parse_quantified_expr()
{
   bool is_some = match(XPathTokenType::SOME);
   bool is_every = false;

   if (not is_some) {
      if (not match(XPathTokenType::EVERY)) return nullptr;
      is_every = true;
   }

   auto quant_node = std::make_unique<XPathNode>(XQueryNodeType::QUANTIFIED_EXPRESSION, is_some ? "some" : "every");

   bool expect_binding = true;
   while (expect_binding) {
      if (not match(XPathTokenType::DOLLAR)) {
         report_error("XPST0003: Expected '$' after quantified expression keyword");
         return nullptr;
      }

      auto variable_name_opt = parse_qname_string();
      if (not variable_name_opt) return nullptr;
      std::string variable_name = *variable_name_opt;

      if (not match(XPathTokenType::IN)) {
         report_error("XPST0003: Expected 'in' in quantified expression");
         return nullptr;
      }

      auto sequence_expr = parse_expr_single();
      if (not sequence_expr) return nullptr;

      auto binding_node = std::make_unique<XPathNode>(XQueryNodeType::QUANTIFIED_BINDING, variable_name);
      binding_node->add_child(std::move(sequence_expr));
      quant_node->add_child(std::move(binding_node));

      if (match(XPathTokenType::COMMA)) expect_binding = true;
      else expect_binding = false;
   }

   if (not match(XPathTokenType::SATISFIES)) {
      report_error("XPST0003: Expected 'satisfies' in quantified expression");
      return nullptr;
   }

   auto condition_expr = parse_expr_single();
   if (not condition_expr) return nullptr;

   quant_node->add_child(std::move(condition_expr));
   return quant_node;
}

//********************************************************************************************************************
// Parses primary expressions including parenthesised expressions, direct and computed constructors, literals,
// numbers, variable references, function calls, and bare identifiers.

std::unique_ptr<XPathNode> XPathParser::parse_primary_expr()
{
   if (match(XPathTokenType::LPAREN)) {
      // Check for empty sequence ()
      if (check(XPathTokenType::RPAREN)) {
         advance(); // consume RPAREN
         return std::make_unique<XPathNode>(XQueryNodeType::EMPTY_SEQUENCE);
      }

      auto expr = parse_expr();
      (void)match(XPathTokenType::RPAREN);
      return expr;
   }

   if (check(XPathTokenType::TAG_OPEN)) {
      return parse_direct_constructor();
   }

   if (check(XPathTokenType::MAP)) {
      return parse_map_constructor();
   }

   if (check(XPathTokenType::ARRAY)) {
      return parse_array_constructor();
   }

   if (is_constructor_keyword(peek())) {
      size_t next_index = current_token + 1;
      bool is_function_call = (next_index < tokens.size()) and (tokens[next_index].type IS XPathTokenType::LPAREN);
      if (not is_function_call) return parse_computed_constructor();
   }

   if (check(XPathTokenType::STRING)) {
      std::string value(peek().text);
      advance();
      return std::make_unique<XPathNode>(XQueryNodeType::LITERAL, value);
   }

   if (check(XPathTokenType::NUMBER)) {
      std::string value(peek().text);
      advance();
      return std::make_unique<XPathNode>(XQueryNodeType::NUMBER, value);
   }

   if (check(XPathTokenType::DOLLAR)) {
      return parse_variable_reference();
   }

   if (is_function_call_ahead(current_token)) {
      return parse_function_call();
   }

   if (is_identifier_token(peek())) {
      size_t saved_index = current_token;
      auto qname = parse_qname_string();
      if (qname) return std::make_unique<XPathNode>(XQueryNodeType::LITERAL, *qname);

      current_token = saved_index;
      std::string value(peek().text);
      advance();
      return std::make_unique<XPathNode>(XQueryNodeType::LITERAL, value);
   }

   return nullptr;
}

//********************************************************************************************************************
// Parses function call expressions with optional comma-separated arguments enclosed in parentheses.

std::unique_ptr<XPathNode> XPathParser::parse_function_call()
{
   auto function_name = parse_qname_string();
   if (not function_name) return nullptr;

   if (not match(XPathTokenType::LPAREN)) return nullptr;

   std::string canonical_name(*function_name);
   if (active_prolog) {
      canonical_name = active_prolog->normalise_function_qname(canonical_name);
   }

   auto function_node = std::make_unique<XPathNode>(XQueryNodeType::FUNCTION_CALL, std::move(canonical_name));

   while (not check(XPathTokenType::RPAREN) and !is_at_end()) {
      auto arg = parse_expr_single();
      if (not arg) break;
      function_node->add_child(std::move(arg));

      if (not match(XPathTokenType::COMMA)) break;
   }

   (void)match(XPathTokenType::RPAREN);
   return function_node;
}

//********************************************************************************************************************
// Wrapper for parse_location_path() to maintain API compatibility for absolute location path parsing.

std::unique_ptr<XPathNode> XPathParser::parse_absolute_location_path() {
   return parse_location_path();
}

std::unique_ptr<XPathNode> XPathParser::parse_relative_location_path() {
   return parse_location_path();
}

std::unique_ptr<XPathNode> XPathParser::parse_axis_specifier() {
   return nullptr; // Handled in parse_step
}

std::unique_ptr<XPathNode> XPathParser::parse_abbreviated_step() {
   return nullptr; // Handled in parse_step
}

std::unique_ptr<XPathNode> XPathParser::parse_argument() {
   return parse_expr_single();
}

std::unique_ptr<XPathNode> XPathParser::parse_number()
{
   if (check(XPathTokenType::NUMBER)) {
      std::string value(peek().text);
      advance();
      return std::make_unique<XPathNode>(XQueryNodeType::NUMBER, value);
   }
   return nullptr;
}

std::unique_ptr<XPathNode> XPathParser::parse_literal()
{
   if (check(XPathTokenType::STRING)) {
      std::string value(peek().text);
      advance();
      return std::make_unique<XPathNode>(XQueryNodeType::STRING, value);
   }
   return nullptr;
}

std::unique_ptr<XPathNode> XPathParser::parse_variable_reference()
{
   if (check(XPathTokenType::DOLLAR)) {
      advance();
      auto name = parse_qname_string();
      if (name) return std::make_unique<XPathNode>(XQueryNodeType::VARIABLE_REFERENCE, *name);
   }
   return nullptr;
}

//********************************************************************************************************************
// Determines whether the supplied token introduces a computed constructor keyword,
// enabling the parser to divert from normal name lookup rules.

bool XPathParser::is_constructor_keyword(const XPathToken &Token) const
{
   if (Token.type IS XPathTokenType::IDENTIFIER) {
      std::string_view keyword = Token.text;
      if (keyword IS "element") return true;
      if (keyword IS "attribute") return true;
      if (keyword IS "text") return true;
      if (keyword IS "comment") return true;
      if (keyword IS "processing-instruction") return true;
      if (keyword IS "document") return true;
   }
   return false;
}

bool XPathParser::consume_token(XPathTokenType type, std::string_view ErrorMessage)
{
   if (match(type)) return true;
   report_error(ErrorMessage);
   return false;
}

//********************************************************************************************************************
// Parses the QName that follows constructor tokens, handling prefixed names and returning both the prefix and local
// part for later namespace resolution.

std::optional<XPathParser::ConstructorName> XPathParser::parse_constructor_qname()
{
   ConstructorName name;

   if (not is_identifier_token(peek())) {
      report_error("XPST0003: Expected name in constructor");
      return std::nullopt;
   }

   name.LocalName = std::string(peek().text);
   advance();

   if (match(XPathTokenType::COLON)) {
      name.Prefix = name.LocalName;
      if (not is_identifier_token(peek())) {
         report_error("XPST0003: Expected local name after ':' in constructor");
         return std::nullopt;
      }
      name.LocalName = std::string(peek().text);
      advance();
   }

   return name;
}

//********************************************************************************************************************
// Parses direct element constructors beginning with '<', capturing namespace
// declarations, attribute value templates, and nested content until the matching
// closing tag is encountered.

std::unique_ptr<XPathNode> XPathParser::parse_direct_constructor()
{
   if (not consume_token(XPathTokenType::TAG_OPEN, "Expected '<' to start direct constructor")) {
      return nullptr;
   }

   auto element_node = std::make_unique<XPathNode>(XQueryNodeType::DIRECT_ELEMENT_CONSTRUCTOR);
   XPathConstructorInfo info;
   info.is_direct = true;
   info.is_empty_element = false;

   auto element_name = parse_constructor_qname();
   if (not element_name) return nullptr;

   info.prefix = element_name->Prefix;
   info.name = element_name->LocalName;

   std::vector<XPathConstructorAttribute> attributes;

   while (not check(XPathTokenType::TAG_CLOSE) and !check(XPathTokenType::EMPTY_TAG_CLOSE)) {
      if (is_at_end()) {
         report_error("XPST0003: Unexpected end of input in direct constructor start tag");
         return nullptr;
      }

      auto attribute_name = parse_constructor_qname();
      if (not attribute_name) return nullptr;

      XPathConstructorAttribute attribute;
      attribute.prefix = attribute_name->Prefix;
      attribute.name = attribute_name->LocalName;
      attribute.is_namespace_declaration = (attribute.prefix.empty() and attribute.name IS "xmlns") or (attribute.prefix IS "xmlns");

      if (not consume_token(XPathTokenType::EQUALS, "Expected '=' after attribute name")) {
         return nullptr;
      }

      if (not check(XPathTokenType::STRING)) {
         report_error("XPST0003: Expected quoted attribute value in direct constructor");
         return nullptr;
      }

      XPathToken attribute_token = peek();
      advance();

      std::vector<XPathAttributeValuePart> parts;
      if (not attribute_token.attribute_value_parts.empty()) {
         parts.reserve(attribute_token.attribute_value_parts.size());
         size_t part_index = 0;
         for (const auto &token_part : attribute_token.attribute_value_parts) {
            XPathAttributeValuePart part;
            part.is_expression = token_part.is_expression;
            part.text = token_part.text;
            part.text_kind = token_part.text_kind;
            if (part.is_expression) {
               auto expr = parse_embedded_expr(part.text);
               if (not expr) return nullptr;
               attribute.set_expression_for_part(part_index, std::move(expr));
            }
            parts.push_back(std::move(part));
            part_index++;
         }
      }
      else {
         XPathAttributeValuePart literal_part;
         literal_part.is_expression = false;
         if (attribute_token.text_kind == TokenTextKind::BorrowedInput) {
            std::string_view literal_text = attribute_token.text;
            if (!token_storage) token_storage = std::make_shared<TokenBuffer>();
            literal_text = token_storage->write_copy(literal_text);
            literal_part.text = literal_text;
            literal_part.text_kind = TokenTextKind::ArenaOwned;
         } else {
            literal_part.text = attribute_token.text;
            literal_part.text_kind = attribute_token.text_kind;
         }
         parts.push_back(std::move(literal_part));
      }

      if (attribute.is_namespace_declaration and !parts.empty() and !parts.front().is_expression) {
         attribute.namespace_uri = parts.front().text;
      }

      attribute.value_storage = token_storage;
      attribute.value_parts = std::move(parts);
      attributes.push_back(std::move(attribute));
   }

   if (check(XPathTokenType::EMPTY_TAG_CLOSE)) {
      (void)match(XPathTokenType::EMPTY_TAG_CLOSE);
      info.is_empty_element = true;
      info.attributes = std::move(attributes);
      element_node->set_constructor_info(std::move(info));
      return element_node;
   }

   if (not consume_token(XPathTokenType::TAG_CLOSE, "Expected '>' to close start tag")) {
      return nullptr;
   }

   info.attributes = std::move(attributes);

   std::string text_buffer;

   auto flush_text = [&]() {
      if (text_buffer.empty()) return;
      auto text_node = std::make_unique<XPathNode>(XQueryNodeType::CONSTRUCTOR_CONTENT, text_buffer);
      element_node->add_child(std::move(text_node));
      text_buffer.clear();
   };

   while (not check(XPathTokenType::CLOSE_TAG_OPEN)) {
      if (is_at_end()) {
         report_error("XPST0003: Unexpected end of input in direct constructor content");
         return nullptr;
      }

      if (check(XPathTokenType::TAG_OPEN)) {
         flush_text();
         auto child = parse_direct_constructor();
         if (not child) return nullptr;
         element_node->add_child(std::move(child));
         continue;
      }

      if (check(XPathTokenType::LBRACE)) {
         flush_text();
         auto expr = parse_enclosed_expr();
         if (not expr) return nullptr;
         auto content_node = std::make_unique<XPathNode>(XQueryNodeType::CONSTRUCTOR_CONTENT);
         content_node->add_child(std::move(expr));
         element_node->add_child(std::move(content_node));
         continue;
      }

      const XPathToken &token = peek();
      if (token.type IS XPathTokenType::END_OF_INPUT) {
         report_error("XPST0003: Unexpected end of input in direct constructor content");
         return nullptr;
      }

      text_buffer += std::string(token.text);
      advance();
   }

   flush_text();

   if (not consume_token(XPathTokenType::CLOSE_TAG_OPEN, "Expected closing tag")) {
      return nullptr;
   }

   auto closing_name = parse_constructor_qname();
   if (not closing_name) return nullptr;

   if (not consume_token(XPathTokenType::TAG_CLOSE, "Expected '>' to close end tag")) {
      return nullptr;
   }

   if (not (closing_name->Prefix IS info.prefix and closing_name->LocalName IS info.name)) {
      report_error("XPST0003: Mismatched closing tag in direct constructor");
      return nullptr;
   }

   element_node->set_constructor_info(std::move(info));
   return element_node;
}

//********************************************************************************************************************

std::unique_ptr<XPathNode> XPathParser::parse_enclosed_expr()
{
   if (not consume_token(XPathTokenType::LBRACE, "Expected '{' to begin expression")) {
      return nullptr;
   }

   auto expr = parse_expr();
   if (not expr) return nullptr;

   if (not consume_token(XPathTokenType::RBRACE, "Expected '}' to close expression")) {
      return nullptr;
   }

   return expr;
}

//********************************************************************************************************************

std::unique_ptr<XPathNode> XPathParser::parse_embedded_expr(std::string_view Source)
{
   XPathTokeniser embedded_tokeniser;
   auto token_block = embedded_tokeniser.tokenize(Source);

   XPathParser embedded_parser;
   auto embedded_result = embedded_parser.parse(std::move(token_block));
   auto expr = std::move(embedded_result.expression);

   if (not expr or embedded_parser.has_errors()) {
      if (embedded_parser.has_errors()) {
         for (const auto &message : embedded_parser.get_errors()) {
            report_error(message);
         }
      }

      if (not expr) report_error("XPST0003: Failed to parse embedded expression");

      return nullptr;
   }

   return expr;
}

//********************************************************************************************************************
// Dispatches to the appropriate computed constructor parser based on the leading keyword so each form can apply its
// specialised grammar.

std::unique_ptr<XPathNode> XPathParser::parse_computed_constructor()
{
   std::string keyword(peek().text);
   advance();

   if (keyword IS "element") return parse_computed_element_constructor();
   if (keyword IS "attribute") return parse_computed_attribute_constructor();
   if (keyword IS "text") return parse_computed_text_constructor();
   if (keyword IS "comment") return parse_computed_comment_constructor();
   if (keyword IS "processing-instruction") return parse_computed_pi_constructor();
   if (keyword IS "document") return parse_computed_document_constructor();

   report_error("XPST0003: Unsupported computed constructor keyword");
   return nullptr;
}

//********************************************************************************************************************
// Parses computed element constructors, optionally accepting enclosed expressions for the element name and always
// parsing the content expression sequence.

std::unique_ptr<XPathNode> XPathParser::parse_computed_element_constructor()
{
   auto node = std::make_unique<XPathNode>(XQueryNodeType::COMPUTED_ELEMENT_CONSTRUCTOR);
   XPathConstructorInfo info;
   info.is_direct = false;
   info.is_empty_element = false;

   if (check(XPathTokenType::LBRACE)) {
      auto name_expr = parse_enclosed_expr();
      if (not name_expr) return nullptr;
      node->set_name_expression(std::move(name_expr));
   }
   else {
      auto name = parse_constructor_qname();
      if (not name) return nullptr;
      info.prefix = name->Prefix;
      info.name = name->LocalName;
   }

   auto content_expr = parse_enclosed_expr();
   if (not content_expr) return nullptr;

   auto content_node = std::make_unique<XPathNode>(XQueryNodeType::CONSTRUCTOR_CONTENT);
   content_node->add_child(std::move(content_expr));
   node->add_child(std::move(content_node));

   node->set_constructor_info(std::move(info));
   return node;
}

//********************************************************************************************************************
// Parses computed attribute constructors which may provide the attribute name either as a literal QName or as an
// enclosed expression, followed by the attribute value expression.

std::unique_ptr<XPathNode> XPathParser::parse_computed_attribute_constructor()
{
   auto node = std::make_unique<XPathNode>(XQueryNodeType::COMPUTED_ATTRIBUTE_CONSTRUCTOR);
   XPathConstructorInfo info;
   info.is_direct = false;
   info.is_empty_element = false;

   if (check(XPathTokenType::LBRACE)) {
      auto name_expr = parse_enclosed_expr();
      if (not name_expr) return nullptr;
      node->set_name_expression(std::move(name_expr));
   }
   else {
      auto name = parse_constructor_qname();
      if (not name) return nullptr;
      info.prefix = name->Prefix;
      info.name = name->LocalName;
   }

   auto value_expr = parse_enclosed_expr();
   if (not value_expr) return nullptr;

   auto content_node = std::make_unique<XPathNode>(XQueryNodeType::CONSTRUCTOR_CONTENT);
   content_node->add_child(std::move(value_expr));
   node->add_child(std::move(content_node));

   node->set_constructor_info(std::move(info));
   return node;
}

//********************************************************************************************************************
// Parses computed text constructors by wrapping the enclosed expression inside a CONSTRUCTOR_CONTENT node so the
// evaluator can produce the resulting text node.

std::unique_ptr<XPathNode> XPathParser::parse_computed_text_constructor()
{
   auto node = std::make_unique<XPathNode>(XQueryNodeType::TEXT_CONSTRUCTOR);
   auto content_expr = parse_enclosed_expr();
   if (not content_expr) return nullptr;

   auto content_node = std::make_unique<XPathNode>(XQueryNodeType::CONSTRUCTOR_CONTENT);
   content_node->add_child(std::move(content_expr));
   node->add_child(std::move(content_node));
   return node;
}

//********************************************************************************************************************
// Parses computed comment constructors mirroring the text constructor structure but targeting comment nodes in the AST.

std::unique_ptr<XPathNode> XPathParser::parse_computed_comment_constructor()
{
   auto node = std::make_unique<XPathNode>(XQueryNodeType::COMMENT_CONSTRUCTOR);
   auto content_expr = parse_enclosed_expr();
   if (not content_expr) return nullptr;

   auto content_node = std::make_unique<XPathNode>(XQueryNodeType::CONSTRUCTOR_CONTENT);
   content_node->add_child(std::move(content_expr));
   node->add_child(std::move(content_node));
   return node;
}

//********************************************************************************************************************
// Parses computed processing-instruction constructors, supporting literal or computed targets along with the
// required content expression.

std::unique_ptr<XPathNode> XPathParser::parse_computed_pi_constructor()
{
   auto node = std::make_unique<XPathNode>(XQueryNodeType::PI_CONSTRUCTOR);
   XPathConstructorInfo info;
   info.is_direct = false;
   info.is_empty_element = false;

   if (check(XPathTokenType::LBRACE)) {
      auto target_expr = parse_enclosed_expr();
      if (not target_expr) return nullptr;
      node->set_name_expression(std::move(target_expr));
   }
   else if (check(XPathTokenType::STRING)) {
      info.name = std::string(peek().text);
      advance();
   }
   else if (is_identifier_token(peek())) {
      info.name = std::string(peek().text);
      advance();
      if (check(XPathTokenType::COLON)) {
         report_error("XPST0003: Processing-instruction target must be an NCName");
         return nullptr;
      }
   }
   else {
      report_error("XPST0003: Expected processing-instruction target");
      return nullptr;
   }

   auto content_expr = parse_enclosed_expr();
   if (not content_expr) return nullptr;

   auto content_node = std::make_unique<XPathNode>(XQueryNodeType::CONSTRUCTOR_CONTENT);
   content_node->add_child(std::move(content_expr));
   node->add_child(std::move(content_node));

   node->set_constructor_info(std::move(info));
   return node;
}

//********************************************************************************************************************
// Parses computed document constructors that evaluate their enclosed expression to populate a synthetic document
// node.

std::unique_ptr<XPathNode> XPathParser::parse_computed_document_constructor()
{
   auto node = std::make_unique<XPathNode>(XQueryNodeType::DOCUMENT_CONSTRUCTOR);
   auto content_expr = parse_enclosed_expr();
   if (not content_expr) return nullptr;

   auto content_node = std::make_unique<XPathNode>(XQueryNodeType::CONSTRUCTOR_CONTENT);
   content_node->add_child(std::move(content_expr));
   node->add_child(std::move(content_node));
   return node;
}

//********************************************************************************************************************
// Parses XQuery 3.0 map constructors of the form map { ExprSingle : ExprSingle, ... }.

std::unique_ptr<XPathNode> XPathParser::parse_map_constructor()
{
   if (not match(XPathTokenType::MAP)) return nullptr;

   auto node = std::make_unique<XPathNode>(XQueryNodeType::MAP_CONSTRUCTOR);

   if (not consume_token(XPathTokenType::LBRACE, "XPST0003: Expected '{' after 'map'")) {
      return nullptr;
   }

   if (match(XPathTokenType::RBRACE)) return node;

   while (true) {
      auto key_expr = parse_expr_single();
      if (not key_expr) return nullptr;

      if (not match(XPathTokenType::COLON)) {
         report_error("XPST0003: Map constructor requires ':' between key and value");
         return nullptr;
      }

      auto value_expr = parse_expr_single();
      if (not value_expr) return nullptr;

      XPathMapConstructorEntry entry;
      entry.key_expression = std::move(key_expr);
      entry.value_expression = std::move(value_expr);
      node->add_map_entry(std::move(entry));

      if (match(XPathTokenType::COMMA)) {
         if (check(XPathTokenType::RBRACE)) {
            report_error("XPST0003: Map constructor cannot end with a trailing comma");
            return nullptr;
         }
         continue;
      }

      break;
   }

   if (not match(XPathTokenType::RBRACE)) {
      report_error("XPST0003: Expected '}' to close map constructor");
      return nullptr;
   }

   return node;
}

//********************************************************************************************************************
// Parses XQuery 3.0 array constructors of the form array { ExprSingle, ... }.

std::unique_ptr<XPathNode> XPathParser::parse_array_constructor()
{
   if (not match(XPathTokenType::ARRAY)) return nullptr;

   auto node = std::make_unique<XPathNode>(XQueryNodeType::ARRAY_CONSTRUCTOR);

   if (not consume_token(XPathTokenType::LBRACE, "XPST0003: Expected '{' after 'array'")) {
      return nullptr;
   }

   if (match(XPathTokenType::RBRACE)) return node;

   while (true) {
      auto member_expr = parse_expr_single();
      if (not member_expr) return nullptr;
      node->add_array_member(std::move(member_expr));

      if (not match(XPathTokenType::COMMA)) break;

      if (check(XPathTokenType::RBRACE)) {
         report_error("XPST0003: Array constructor cannot end with a trailing comma");
         return nullptr;
      }
   }

   if (not match(XPathTokenType::RBRACE)) {
      report_error("XPST0003: Expected '}' to close array constructor");
      return nullptr;
   }

   return node;
}
