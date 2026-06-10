// XPath Module Unit Tests
// This file contains compiled-in unit tests for the XPath module, primarily for debugging prolog integration.
// Unit tests need to be enabled in the CMakeLists.txt file and then launched from test_unit_tests.tiri

#include <kotuku/modules/xquery.h>
#include "xquery.h"
#include "../xml/xml.h"

#include <iostream>
#include <cmath>
#include <sstream>
#include <string>

//********************************************************************************************************************
// Forward declarations for implementation-local helpers compiled into the same translation unit.

static ERR build_query(extXQuery *Self);

//********************************************************************************************************************
// Test helper functions

static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

static void test_assert(bool Condition, CSTRING TestName, CSTRING Message) {
   kt::Log log("XQueryTests");
   test_count++;
   if (Condition) {
      pass_count++;
      log.msg("PASS: %s", TestName);
   }
   else {
      fail_count++;
      log.msg("FAIL: %s - %s", TestName, Message);
   }
}

static const XPathNode * expression_body(const std::unique_ptr<XPathNode> &node)
{
   const XPathNode *current = node.get();
   if (not current) return nullptr;
   if (current->type IS XQueryNodeType::EXPRESSION) return current->get_child_safe(0);
   return current;
}

//********************************************************************************************************************

struct resolve_variable_test_context {
   int call_count = 0;
   ankerl::unordered_dense::map<std::string, XPathValue> values;
   ankerl::unordered_dense::map<std::string, ERR> errors;
};

struct search_result_context {
   std::vector<std::string> ids;
};

struct registered_function_test_context {
   int call_count = 0;
   std::vector<std::string> names;
};

static XPathValue make_string_value(std::string_view Value)
{
   XPathValue result(XPVT::String);
   result.StringValue.assign(Value);
   return result;
}

static XPathValue make_number_value(double Value)
{
   XPathValue result(XPVT::Number);
   result.NumberValue = Value;
   return result;
}

static XPathValue make_boolean_value(bool Value)
{
   XPathValue result(XPVT::Boolean);
   result.NumberValue = Value ? 1.0 : 0.0;
   return result;
}

static XPathValue make_map_value(std::string_view Key, const XPathValue &Value)
{
   XPathValue result(XPVT::Map);
   result.map_storage = std::make_shared<XPathMapStorage>();
   XPathMapEntry entry;
   entry.key.assign(Key);
   entry.value.items.push_back(Value);
   result.map_storage->entries.push_back(std::move(entry));
   return result;
}

static XPathValue make_array_value(std::initializer_list<XPathValue> Values)
{
   XPathValue result(XPVT::Array);
   result.array_storage = std::make_shared<XPathArrayStorage>();
   for (const auto &value : Values) {
      XPathValueSequence member;
      member.items.push_back(value);
      result.array_storage->members.push_back(std::move(member));
   }
   return result;
}

static ERR resolve_variable_test_callback(objXQuery *Query, std::string_view Name, XPathValue *Result, APTR Meta)
{
   auto &context = *(resolve_variable_test_context *)Meta;
   context.call_count++;

   std::string name(Name);

   if (auto error = context.errors.find(name); error != context.errors.end()) {
      return error->second;
   }

   if (auto value = context.values.find(name); value != context.values.end()) {
      *Result = value->second;
      return ERR::Okay;
   }

   return ERR::Search;
}

static std::string tag_attribute_value(extXML *XML, int TagID, std::string_view AttributeName)
{
   if (not XML) return std::string();

   XTag *tag = XML->getTag(TagID);
   if (not tag) return std::string();

   for (const auto &attrib : tag->Attribs) {
      if (attrib.Name IS AttributeName) return attrib.Value;
   }

   return std::string();
}

static ERR collect_search_result(extXML *XML, int TagID, CSTRING Attrib, APTR Meta)
{
   auto &context = *(search_result_context *)Meta;
   context.ids.push_back(tag_attribute_value(XML, TagID, "id"));
   return ERR::Okay;
}

static ERR registered_function_test_callback(objXQuery *Query, std::string_view Name,
   const std::vector<XPathValue> &Args, XPathValue &Result, APTR Meta)
{
   (void)Query;
   auto &context = *(registered_function_test_context *)Meta;
   context.call_count++;
   context.names.emplace_back(Name);

   if (Args.empty()) return ERR::Args;

   Result = make_number_value(Args[0].NumberValue * 2.0);
   return ERR::Okay;
}

static bool compile_query_for_test(extXQuery &Query, std::string_view Statement, FUNCTION ResolveVariable = FUNCTION())
{
   Query.Statement.assign(Statement);
   Query.ResolveVariable = ResolveVariable;
   Query.ParseResult = CompiledXQuery();
   Query.ModuleCache.reset();
   Query.ErrorMsg.clear();
   Query.StaleBuild = true;
   return build_query(&Query) IS ERR::Okay;
}

static bool evaluate_query_text(extXQuery &Query, std::string_view Statement, XPathVal &Result,
   resolve_variable_test_context *Context = nullptr)
{
   FUNCTION callback;
   if (Context) callback = C_FUNCTION(resolve_variable_test_callback, Context);

   if (not compile_query_for_test(Query, Statement, callback)) return false;

   XPathEvaluator evaluator(&Query, nullptr, Query.ParseResult.expression.get(), &Query.ParseResult);
   return evaluator.evaluate_xpath_expression(*Query.ParseResult.expression.get(), &Result) IS ERR::Okay;
}

static ERR search_query_text(extXQuery &Query, extXML *XML, std::string_view Statement,
   resolve_variable_test_context *ResolveContext, search_result_context &SearchResults)
{
   Query.Callback = C_FUNCTION(collect_search_result, &SearchResults);
   FUNCTION resolver = ResolveContext ? C_FUNCTION(resolve_variable_test_callback, ResolveContext) : FUNCTION();
   if (not compile_query_for_test(Query, Statement, resolver)) return ERR::Syntax;

   XPathEvaluator evaluator(&Query, XML, Query.ParseResult.expression.get(), &Query.ParseResult);
   return evaluator.find_tag(*Query.ParseResult.expression.get(), 0);
}

//********************************************************************************************************************
// XQueryProlog API Tests

static void test_prolog_api() {
   kt::Log log("PrologTests");

   // Test 1: Create empty prolog
   {
      XQueryProlog prolog;
      test_assert(prolog.functions.empty(), "Empty prolog creation", "New prolog should have no functions");
   }

   // Test 2: Declare a function
   {
      XQueryProlog prolog;
      XQueryFunction func;
      func.qname = "local:test";
      func.parameter_names.push_back("x");
      prolog.declare_function(std::move(func));

      auto found = prolog.find_function("local:test", 1);
      test_assert(found not_eq nullptr, "Function declaration", "Declared function should be findable");
   }

   // Test 3: Function arity matching
   {
      XQueryProlog prolog;
      XQueryFunction func;
      func.qname = "local:add";
      func.parameter_names.push_back("a");
      func.parameter_names.push_back("b");
      prolog.declare_function(std::move(func));

      auto found1 = prolog.find_function("local:add", 2);
      auto found2 = prolog.find_function("local:add", 1);

      test_assert((found1 not_eq nullptr) and (found2 IS nullptr),
         "Function arity matching",
         "Function should only match correct arity");
   }

   // Test 4: Variable declaration
   {
      XQueryProlog prolog;
      XQueryVariable var;
      var.qname = "pi";
      prolog.declare_variable("pi", std::move(var));

      auto found = prolog.find_variable("pi");
      test_assert(found not_eq nullptr, "Variable declaration",
         "Declared variable should be findable");
   }

   // Test 6: Multiple functions with same name, different arity
   {
      XQueryProlog prolog;

      XQueryFunction func1;
      func1.qname = "local:format";
      prolog.declare_function(std::move(func1));

      XQueryFunction func2;
      func2.qname = "local:format";
      func2.parameter_names.push_back("fmt");
      prolog.declare_function(std::move(func2));

      XQueryFunction func3;
      func3.qname = "local:format";
      func3.parameter_names.push_back("fmt");
      func3.parameter_names.push_back("arg");
      prolog.declare_function(std::move(func3));

      auto f0 = prolog.find_function("local:format", 0);
      auto f1 = prolog.find_function("local:format", 1);
      auto f2 = prolog.find_function("local:format", 2);
      auto f3 = prolog.find_function("local:format", 3);

      bool all_found = (f0 not_eq nullptr) and (f1 not_eq nullptr) and (f2 not_eq nullptr) and (f3 IS nullptr);
      test_assert(all_found, "Function overloading by arity",
         "Should support multiple arities for same function name");
   }
}

//********************************************************************************************************************
// Prolog Integration Tests

static const char *token_type_name(XPathTokenType Type)
{
   switch (Type)
   {
      case XPathTokenType::IDENTIFIER: return "IDENTIFIER";
      case XPathTokenType::MODULE: return "MODULE";
      case XPathTokenType::IMPORT: return "IMPORT";
      case XPathTokenType::OPTION: return "OPTION";
      case XPathTokenType::ORDER: return "ORDER";
      case XPathTokenType::COLLATION: return "COLLATION";
      case XPathTokenType::ORDERING: return "ORDERING";
      case XPathTokenType::COPY_NAMESPACES: return "COPY_NAMESPACES";
      case XPathTokenType::DECIMAL_FORMAT: return "DECIMAL_FORMAT";
      case XPathTokenType::SCHEMA: return "SCHEMA";
      case XPathTokenType::DEFAULT: return "DEFAULT";
      case XPathTokenType::COLON: return "COLON";
      case XPathTokenType::ASSIGN: return "ASSIGN";
      case XPathTokenType::MAP: return "MAP";
      case XPathTokenType::ARRAY: return "ARRAY";
      case XPathTokenType::LOOKUP: return "LOOKUP";
      case XPathTokenType::QUESTION_MARK: return "QUESTION_MARK";
      default: return "(unclassified)";
   }
}

static void test_tokeniser_prolog_keywords()
{
   kt::Log log("TokeniserTests");

   // Progress marker (2024-10-17): capturing current behaviour before adding DECLARE/FUNCTION/VARIABLE tokens.

   XPathTokeniser tokeniser;

   auto function_block = tokeniser.tokenize("declare function local:square($x) { $x * $x }");
   const auto &function_tokens = function_block.tokens;
   test_assert(function_tokens.size() >= 6, "Function declaration token count",
      "Tokeniser should emit tokens for sample prolog function");

   if (!function_tokens.empty())
   {
      bool declare_keyword = function_tokens[0].type not_eq XPathTokenType::IDENTIFIER;
      std::string declare_message = "Tokeniser reports 'declare' as " +
         std::string(token_type_name(function_tokens[0].type));
      test_assert(declare_keyword, "Prolog keyword: declare",
         declare_message.c_str());
   }

   if (function_tokens.size() > 1)
   {
      bool function_keyword = function_tokens[1].type not_eq XPathTokenType::IDENTIFIER;
      std::string function_message = "Tokeniser reports 'function' as " +
         std::string(token_type_name(function_tokens[1].type));
      test_assert(function_keyword, "Prolog keyword: function",
         function_message.c_str());
   }

   if (function_tokens.size() > 3)
   {
      bool colon_classified = function_tokens[3].type IS XPathTokenType::COLON;
      test_assert(colon_classified, "QName prefix separator",
         "Colon between prefix and local name should be tokenised as COLON");
   }

   auto variable_block = tokeniser.tokenize("declare variable $value := 1");
   const auto &variable_tokens = variable_block.tokens;
   test_assert(variable_tokens.size() >= 5, "Variable declaration token count",
      "Tokeniser should emit tokens for sample variable declaration");

   if (!variable_tokens.empty())
   {
      bool declare_keyword = variable_tokens[0].type not_eq XPathTokenType::IDENTIFIER;
      std::string declare_message = "Tokeniser reports 'declare' as " +
         std::string(token_type_name(variable_tokens[0].type));
      test_assert(declare_keyword, "Prolog keyword reuse: declare",
         declare_message.c_str());
   }

   if (variable_tokens.size() > 1)
   {
      bool variable_keyword = variable_tokens[1].type not_eq XPathTokenType::IDENTIFIER;
      std::string variable_message = "Tokeniser reports 'variable' as " +
         std::string(token_type_name(variable_tokens[1].type));
      test_assert(variable_keyword, "Prolog keyword: variable",
         variable_message.c_str());
   }

   if (variable_tokens.size() > 4)
   {
      bool assign_token = variable_tokens[4].type IS XPathTokenType::ASSIGN;
      test_assert(assign_token, "Variable assignment operator",
         "':=' should be tokenised as ASSIGN for prolog variables");
   }

   auto namespace_block = tokeniser.tokenize("declare namespace ex = \"http://example.org\"");
   const auto &namespace_tokens = namespace_block.tokens;
   test_assert(namespace_tokens.size() >= 4, "Namespace declaration token count",
      "Tokeniser should emit tokens for namespace declaration");

   if (!namespace_tokens.empty())
   {
      bool declare_keyword = namespace_tokens[0].type not_eq XPathTokenType::IDENTIFIER;
      std::string declare_message = "Tokeniser reports 'declare' as " +
         std::string(token_type_name(namespace_tokens[0].type));
      test_assert(declare_keyword, "Prolog keyword reuse: declare (namespace)",
         declare_message.c_str());
   }

   if (namespace_tokens.size() > 1)
   {
      bool namespace_keyword = namespace_tokens[1].type not_eq XPathTokenType::IDENTIFIER;
      std::string namespace_message = "Tokeniser reports 'namespace' as " +
         std::string(token_type_name(namespace_tokens[1].type));
      test_assert(namespace_keyword, "Prolog keyword: namespace",
         namespace_message.c_str());
   }

   auto external_block = tokeniser.tokenize("declare variable $flag external");
   const auto &external_tokens = external_block.tokens;
   test_assert(external_tokens.size() >= 5, "External variable token count",
      "Tokeniser should emit tokens for external variable declaration");

   if (external_tokens.size() > 4)
   {
      bool external_keyword = external_tokens[4].type not_eq XPathTokenType::IDENTIFIER;
      std::string external_message = "Tokeniser reports 'external' as " +
         std::string(token_type_name(external_tokens[4].type));
      test_assert(external_keyword, "Prolog keyword: external",
         external_message.c_str());
   }
}

static void test_tokeniser_map_array_lookup()
{
   kt::Log log("TokeniserMapArray");

   XPathTokeniser tokeniser;

   auto map_block = tokeniser.tokenize("map { \"k\" : 1 }");
   bool map_keyword = !map_block.tokens.empty() and (map_block.tokens[0].type IS XPathTokenType::MAP);
   std::string map_message = "Tokeniser reports 'map' as " +
      std::string(token_type_name(map_block.tokens.empty() ? XPathTokenType::UNKNOWN : map_block.tokens[0].type));
   test_assert(map_keyword, "Map constructor keyword", map_message.c_str());

   auto array_block = tokeniser.tokenize("array { 1, 2 }");
   bool array_keyword = !array_block.tokens.empty() and (array_block.tokens[0].type IS XPathTokenType::ARRAY);
   std::string array_message = "Tokeniser reports 'array' as " +
      std::string(token_type_name(array_block.tokens.empty() ? XPathTokenType::UNKNOWN : array_block.tokens[0].type));
   test_assert(array_keyword, "Array constructor keyword", array_message.c_str());

   auto lookup_block = tokeniser.tokenize("$m?foo?1");
   size_t lookup_tokens = 0;
   for (const auto &token : lookup_block.tokens)
   {
      if (token.type IS XPathTokenType::LOOKUP) lookup_tokens++;
   }
   test_assert(lookup_tokens >= 2, "Lookup operator token",
      "Tokeniser should classify '?foo' and '?1' as LOOKUP tokens");

   auto occurrence_block = tokeniser.tokenize("declare variable $s as xs:string? external");
   bool saw_occurrence = false;
   for (const auto &token : occurrence_block.tokens)
   {
      if (token.type IS XPathTokenType::QUESTION_MARK) saw_occurrence = true;
   }
   test_assert(saw_occurrence, "Occurrence indicator token",
      "Tokeniser should still emit QUESTION_MARK for type occurrence indicators");
}

//********************************************************************************************************************
// Ensures the parser populates cached operator metadata for recognised unary and binary nodes.

static void test_parser_operator_cache_population()
{
   kt::Log log("OperatorTests");

   XPathTokeniser tokeniser;
   auto token_block = tokeniser.tokenize("1 + 2 * 3 and not(-$flag)");

   XPathParser parser;
   auto compiled = parser.parse(std::move(token_block));

   bool has_expression = compiled.expression not_eq nullptr;
   test_assert(has_expression, "Parser expression availability", "Parser should return an expression tree");
   if (not has_expression) return;

   struct CacheFlags {
      bool plus_cached = false;
      bool multiply_cached = false;
      bool logical_and_cached = false;
      bool unary_not_cached = false;
      bool unary_negate_cached = false;
   } flags;

   auto inspect = [&](auto &&self, const XPathNode *node) -> void {
      if (not node) return;

      if ((node->type IS XQueryNodeType::EXPRESSION) and (node->child_count() > 0)) {
         if (auto *child = node->get_child_safe(0)) self(self, child);
         return;
      }

      if (node->type IS XQueryNodeType::BINARY_OP) {
         auto op_text = node->get_value_view();
         if (op_text IS "+") flags.plus_cached = node->has_cached_binary_kind();
         else if (op_text IS "*") flags.multiply_cached = node->has_cached_binary_kind();
         else if (op_text IS "and") flags.logical_and_cached = node->has_cached_binary_kind();
      }
      else if (node->type IS XQueryNodeType::UNARY_OP) {
         auto op_text = node->get_value_view();
         if (op_text IS "not") flags.unary_not_cached = node->has_cached_unary_kind();
         else if (op_text IS "-") flags.unary_negate_cached = node->has_cached_unary_kind();
      }

      size_t child_total = node->child_count();
      for (size_t index = 0; index < child_total; index++) {
         if (auto *child = node->get_child_safe(index)) self(self, child);
      }
   };

   inspect(inspect, compiled.expression.get());

   test_assert(flags.plus_cached, "Binary operator '+' cache", "Parser should cache addition operator kind");
   test_assert(flags.multiply_cached, "Binary operator '*' cache", "Parser should cache multiplication operator kind");
   test_assert(flags.logical_and_cached, "Binary operator 'and' cache", "Parser should cache logical and operator kind");
   test_assert(flags.unary_not_cached, "Unary operator 'not' cache", "Parser should cache logical not operator kind");
   test_assert(flags.unary_negate_cached, "Unary operator '-' cache", "Parser should cache negation operator kind");
}

//********************************************************************************************************************
// ResolveVariable callback integration tests.

static void test_resolve_variable_callback()
{
   kt::Log log("ResolveVariableTests");

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("city", make_string_value("London"));

      XPathVal result;
      bool ok = evaluate_query_text(query, "concat($city, '-', $city)", result, &context);

      test_assert(ok, "ResolveVariable string callback", "String callback-backed variables should evaluate successfully");
      if (ok) {
         test_assert(result.to_string() IS "London-London", "ResolveVariable string result",
            "String callback should supply the requested variable value");
      }
      test_assert(context.call_count IS 1, "ResolveVariable positive cache",
         "Repeated variable references in one evaluation should hit the callback only once");
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("price", make_number_value(12.5));

      XPathVal result;
      bool ok = evaluate_query_text(query, "$price * 2", result, &context);

      test_assert(ok, "ResolveVariable number callback", "Numeric callback-backed variables should evaluate successfully");
      if (ok) {
         test_assert(std::abs(result.to_number() - 25.0) < 0.0001, "ResolveVariable numeric result",
            "Numeric callback should preserve typed arithmetic semantics");
      }
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("enabled", make_boolean_value(true));

      XPathVal result;
      bool ok = evaluate_query_text(query, "if ($enabled) then 'yes' else 'no'", result, &context);

      test_assert(ok, "ResolveVariable boolean callback", "Boolean callback-backed variables should evaluate successfully");
      if (ok) {
         test_assert(result.to_string() IS "yes", "ResolveVariable boolean result",
            "Boolean callback should participate in conditional evaluation");
      }
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("cfg", make_map_value("mode", make_string_value("strict")));

      XPathVal result;
      bool ok = evaluate_query_text(query,
         "declare namespace map = 'http://www.w3.org/2005/xpath-functions/map'; string(map:get($cfg, 'mode'))",
         result, &context);

      test_assert(ok, "ResolveVariable map callback", "Map callback-backed variables should evaluate successfully");
      if (ok) {
         test_assert(result.to_string() IS "strict", "ResolveVariable map result",
            "Map callback should preserve composite map values");
      }
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("items",
         make_array_value({ make_string_value("red"), make_string_value("green"), make_string_value("blue") }));

      XPathVal result;
      bool ok = evaluate_query_text(query,
         "declare namespace array = 'http://www.w3.org/2005/xpath-functions/array'; string-join($items?*, ',')",
         result, &context);

      test_assert(ok, "ResolveVariable array callback", "Array callback-backed variables should evaluate successfully");
      if (ok) {
         test_assert(result.to_string() IS "red,green,blue", "ResolveVariable array result",
            "Array callback should preserve composite array members");
      }
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("city", make_string_value("Ignored"));
      query.Variables["city"] = "Paris";

      XPathVal result;
      bool ok = evaluate_query_text(query, "$city", result, &context);

      test_assert(ok, "ResolveVariable SetKey precedence", "SetKey variables should resolve before the callback");
      if (ok) {
         test_assert(result.to_string() IS "Paris", "ResolveVariable SetKey value",
            "SetKey should win over the callback for matching variable names");
      }
      test_assert(context.call_count IS 0, "ResolveVariable SetKey bypass",
         "SetKey variables should not invoke the callback");
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("city", make_string_value("Ignored"));

      XPathVal result;
      bool ok = evaluate_query_text(query, "let $city := 'Rome' return $city", result, &context);

      test_assert(ok, "ResolveVariable local precedence", "Local FLWOR variables should resolve before the callback");
      if (ok) {
         test_assert(result.to_string() IS "Rome", "ResolveVariable local value",
            "Local FLWOR bindings should override the callback");
      }
      test_assert(context.call_count IS 0, "ResolveVariable local bypass",
         "Local FLWOR bindings should not invoke the callback");
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("city", make_string_value("Ignored"));

      XPathVal result;
      bool ok = evaluate_query_text(query, "declare variable $city := 'Berlin'; $city", result, &context);

      test_assert(ok, "ResolveVariable declared precedence", "Declared prolog variables should resolve before the callback");
      if (ok) {
         test_assert(result.to_string() IS "Berlin", "ResolveVariable declared value",
            "Declared prolog variables should override the callback");
      }
      test_assert(context.call_count IS 0, "ResolveVariable declared bypass",
         "Declared prolog variables should not invoke the callback");
   }

   {
      auto xml = kt::Create<objXML>({ fl::Statement("<root><item id='a'/><item id='b'/><item id='c'/></root>") });
      bool xml_ok = xml.ok() and (*xml);
      test_assert(xml_ok, "ResolveVariable search XML setup", "Search integration test requires a valid XML document");

      if (xml_ok) {
         extXQuery query;
         resolve_variable_test_context resolve_context;
         resolve_context.values.insert_or_assign("wanted", make_string_value("b"));
         search_result_context search_results;

         auto error = search_query_text(query, (extXML *)*xml, "/root/item[@id = $wanted]",
            &resolve_context, search_results);

         bool matched = (!error) and (search_results.ids.size() IS 1) and (search_results.ids[0] IS "b");
         test_assert(matched, "ResolveVariable search callback",
            "Search() evaluation should accept callback-backed variables inside predicates");
         test_assert(resolve_context.call_count IS 1, "ResolveVariable search cache",
            "Search() should still use evaluator-local callback caching");
      }
   }

   {
      extXQuery query;
      resolve_variable_test_context context;

      XPathVal result;
      bool ok = evaluate_query_text(query, "$counter + $counter", result, &context);
      test_assert(not ok, "ResolveVariable missing variable failure",
         "Unknown callback-backed variables should fail when the callback reports ERR::Search");
      test_assert(context.call_count IS 1, "ResolveVariable missing cache",
         "Missing callback-backed variables should be cached as misses within one evaluation");
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.errors.insert_or_assign("explode", ERR::AccessObject);

      XPathVal result;
      bool ok = evaluate_query_text(query, "$explode", result, &context);
      test_assert(not ok, "ResolveVariable callback error", "Callback failures should abort evaluation");
      test_assert(query.ParseResult.error_msg.find("ResolveVariable callback failed") != std::string::npos,
         "ResolveVariable callback error message",
         "Callback failures should report a specific diagnostic");
   }

   {
      extXQuery query;
      resolve_variable_test_context context;
      context.values.insert_or_assign("city", make_string_value("Lisbon"));

      XPathVal first_result;
      XPathVal second_result;

      bool first_ok = evaluate_query_text(query, "concat($city, '')", first_result, &context);
      bool second_ok = evaluate_query_text(query, "concat($city, '')", second_result, &context);

      test_assert(first_ok and second_ok, "ResolveVariable per-evaluation reset",
         "Separate evaluations should each resolve callback-backed variables successfully");
      test_assert(context.call_count IS 2, "ResolveVariable cache lifetime",
         "Callback caches must be evaluator-local and not leak across evaluations");
   }
}

static void test_registered_function_qname_normalisation()
{
   kt::Log log("RegisteredFunctionTests");

   {
      extXQuery query;
      registered_function_test_context context;
      FUNCTION callback = C_FUNCTION(registered_function_test_callback, &context);
      query.RegisteredFunctions["ext:double"] = callback;

      XPathVal result;
      bool ok = evaluate_query_text(query,
         "declare namespace ext = 'urn:test'; ext:double(21)",
         result);

      test_assert(ok, "Registered function prefix lookup",
         "Prefixed registrations should resolve against canonical QNames");
      if (ok) {
         test_assert(std::abs(result.to_number() - 42.0) < 0.0001, "Registered function prefix result",
            "Prefixed registered functions should evaluate successfully");
      }
      test_assert(context.call_count IS 1, "Registered function prefix callback count",
         "Prefixed registered functions should invoke the callback once");
      if (context.call_count IS 1) {
         test_assert(context.names[0] IS "Q{urn:test}double", "Registered function prefix callback name",
            "Callback should receive the canonical expanded QName");
      }
   }

   {
      extXQuery query;
      registered_function_test_context context;
      FUNCTION callback = C_FUNCTION(registered_function_test_callback, &context);
      query.RegisteredFunctions["double"] = callback;

      XPathVal result;
      bool ok = evaluate_query_text(query,
         "declare default function namespace 'urn:test'; double(21)",
         result);

      test_assert(ok, "Registered function default namespace lookup",
         "Default-function-namespace registrations should resolve against canonical QNames");
      if (ok) {
         test_assert(std::abs(result.to_number() - 42.0) < 0.0001, "Registered function default namespace result",
            "Default-function-namespace registered functions should evaluate successfully");
      }
      test_assert(context.call_count IS 1, "Registered function default namespace callback count",
         "Default-function-namespace registered functions should invoke the callback once");
      if (context.call_count IS 1) {
         test_assert(context.names[0] IS "Q{urn:test}double", "Registered function default namespace callback name",
            "Callback should receive the canonical expanded QName");
      }
   }
}

static void test_parser_map_array_lookup_nodes()
{
   kt::Log log("ParserMapArrayNodes");

   {
      XPathTokeniser tokeniser;
      auto block = tokeniser.tokenize("map { \"k\" : 1 }");
      XPathParser parser;
      auto compiled = parser.parse(std::move(block));

      bool has_expression = compiled.expression not_eq nullptr;
      test_assert(has_expression, "Map constructor parse", "Parser should accept map constructors");
      if (not has_expression) return;

      const XPathNode *map_node = expression_body(compiled.expression);
      bool correct_type = map_node and (map_node->type IS XQueryNodeType::MAP_CONSTRUCTOR);
      test_assert(correct_type, "Map constructor node type", "Parser should emit MAP_CONSTRUCTOR nodes");
      if (correct_type) {
         test_assert(map_node->map_entry_count() IS 1, "Map constructor entry count", "Map constructor should store entries");
      }
   }

   {
      XPathTokeniser tokeniser;
      auto block = tokeniser.tokenize("array { 1, 2 }");
      XPathParser parser;
      auto compiled = parser.parse(std::move(block));

      bool has_expression = compiled.expression not_eq nullptr;
      test_assert(has_expression, "Array constructor parse", "Parser should accept array constructors");
      if (not has_expression) return;

      const XPathNode *array_node = expression_body(compiled.expression);
      bool correct_type = array_node and (array_node->type IS XQueryNodeType::ARRAY_CONSTRUCTOR);
      test_assert(correct_type, "Array constructor node type", "Parser should emit ARRAY_CONSTRUCTOR nodes");
      if (correct_type) {
         test_assert(array_node->array_member_count() IS 2, "Array constructor member count", "Array constructor should store members");
      }
   }

   {
      XPathTokeniser tokeniser;
      auto block = tokeniser.tokenize("$m?foo?1");
      XPathParser parser;
      auto compiled = parser.parse(std::move(block));

      bool has_expression = compiled.expression not_eq nullptr;
      test_assert(has_expression, "Lookup expression parse", "Parser should accept lookup expressions");
      if (not has_expression) return;

      const XPathNode *lookup_node = expression_body(compiled.expression);
      bool correct_type = lookup_node and (lookup_node->type IS XQueryNodeType::LOOKUP_EXPRESSION);
      test_assert(correct_type, "Lookup expression node type", "Parser should emit LOOKUP_EXPRESSION nodes");
      if (correct_type) {
         test_assert(lookup_node->lookup_specifier_count() IS 2, "Lookup specifier count", "Lookup expressions should retain chained specifiers");
         auto first = lookup_node->get_lookup_specifier(0);
         bool first_kind = first and (first->kind IS XPathLookupSpecifierKind::NCName);
         test_assert(first_kind, "Lookup NCName specifier", "First lookup should treat NCName keys as literals");
         auto second = lookup_node->get_lookup_specifier(1);
         bool second_kind = second and (second->kind IS XPathLookupSpecifierKind::IntegerLiteral);
         test_assert(second_kind, "Lookup integer specifier", "Second lookup should capture integer literals");
      }
   }
}

static void test_prolog_in_xpath()
{
   kt::Log log("PrologInXPath");

   // Test 1: Check if prolog structure can be accessed
   {
      XQueryProlog prolog;
      XQueryFunction func;
      func.qname = "local:square";
      func.parameter_names.push_back("x");
      prolog.declare_function(std::move(func));

      // Verify the function signature is correct
      auto found = prolog.find_function("local:square", 1);
      bool has_correct_params = false;
      if (found) {
         has_correct_params = (found->parameter_names.size() IS 1) and
                             (found->parameter_names[0] IS "x");
      }

      test_assert(has_correct_params, "Function parameter names",
         "Function should retain parameter names correctly");
   }

   // Test 2: Variable external flag
   {
      XQueryProlog prolog;
      XQueryVariable var;
      var.qname = "external_var";
      var.is_external = true;
      prolog.declare_variable("external_var", std::move(var));

      auto found = prolog.find_variable("external_var");
      test_assert(found and found->is_external, "External variable flag",
         "External variables should be marked correctly");
   }

   // Test 3: Function external flag
   {
      XQueryProlog prolog;
      XQueryFunction func;
      func.qname = "local:external_func";
      func.is_external = true;
      prolog.declare_function(std::move(func));

      auto found = prolog.find_function("local:external_func", 0);
      test_assert(found and found->is_external, "External function flag",
         "External functions should be marked correctly");
   }
}

//********************************************************************************************************************

static void run_unit_tests(std::string_view Options, int &Passed, int &Total)
{
   kt::Log log("XQueryTests");

   test_count = 0;
   pass_count = 0;
   fail_count = 0;

   if (Options IS "resolve_variable") {
      test_resolve_variable_callback();
   }
   else if (Options IS "registered_function") {
      test_registered_function_qname_normalisation();
   }
   else {
      test_tokeniser_prolog_keywords();
      test_tokeniser_map_array_lookup();
      test_parser_operator_cache_population();
      test_parser_map_array_lookup_nodes();
      test_prolog_api();
      test_prolog_in_xpath();
      test_resolve_variable_callback();
      test_registered_function_qname_normalisation();
   }

   Passed = pass_count;
   Total = test_count;
   log.msg("Test Summary: %d of %d tests passed.", Passed, Total);
}
