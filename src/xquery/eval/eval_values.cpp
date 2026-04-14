//********************************************************************************************************************
// XPath Expression and Value Evaluation
//
// This translation unit contains the core expression evaluation engine for XPath. It handles:
//
//   - Location path evaluation (evaluate_path_expression_value, evaluate_path_from_nodes)
//   - Set operations (union, intersect, except)
//   - Expression evaluation for all XPath types (evaluate_expression - the main dispatcher)
//   - Function call evaluation
//   - Top-level expression processing and result handling
//
// All value evaluators consume comparison utilities from xpath_evaluator_detail.h and navigation
// functions from xpath_evaluator_navigation.cpp to maintain clean separation of concerns.

#include "../api/xquery_functions.h"
#include "../../xml/schema/schema_types.h"

//********************************************************************************************************************
// Determines whether a character qualifies as the first character of an XML NCName (letters A-Z, a-z, or
// underscore '_'). Used for validating constructor names and QName components.

inline bool is_ncname_start(char Ch)
{
   if ((Ch >= 'A') and (Ch <= 'Z')) return true;
   if ((Ch >= 'a') and (Ch <= 'z')) return true;
   return Ch IS '_';
}

//********************************************************************************************************************
// Determines whether a character qualifies as a subsequent character in an XML NCName (alpha-numerics, hyphen '-',
// or period '.'). Used in conjunction with is_ncname_start to validate complete NCName strings.

inline bool is_ncname_char(char Ch)
{
   if (is_ncname_start(Ch)) return true;
   if ((Ch >= '0') and (Ch <= '9')) return true;
   if (Ch IS '-') return true;
   return Ch IS '.';
}

//********************************************************************************************************************
// Determines if the supplied string adheres to the NCName production so constructor names can be validated without
// deferring to the XML runtime.

inline bool is_valid_ncname(std::string_view Value)
{
   if (Value.empty()) return false;
   if (not is_ncname_start(Value.front())) return false;

   for (size_t index = 1; index < Value.length(); ++index) {
      if (not is_ncname_char(Value[index])) return false;
   }

   return true;
}

//********************************************************************************************************************
// Removes leading and trailing XML whitespace characters from constructor data so that lexical comparisons can be
// performed using the normalised string.

static std::string trim_constructor_whitespace(std::string_view Value)
{
   size_t start = 0;
   size_t end = Value.length();

   while ((start < end) and (uint8_t(Value[start]) <= 0x20)) ++start;
   while ((end > start) and (uint8_t(Value[end - 1]) <= 0x20)) --end;

   return std::string(Value.substr(start, end - start));
}

static bool is_xml_whitespace_only(std::string_view Value)
{
   for (char ch : Value) {
      if (uint8_t(ch) > 0x20) return false;
   }
   return true;
}

//********************************************************************************************************************
// Represents a QName or expanded QName parsed from constructor syntax, capturing the prefix, local part, and resolved
// namespace URI when known.

struct ConstructorQName {
   bool valid = false;
   std::string prefix;
   std::string local;
   std::string namespace_uri;
};

//********************************************************************************************************************
// Parses a QName or expanded QName literal used by computed constructors.  The function recognises the "Q{uri}local"
// form as well as prefixed names and produces a structured representation that downstream evaluators can inspect.

static ConstructorQName parse_constructor_qname_string(std::string_view Value)
{
   ConstructorQName result;
   if (Value.empty()) return result;

   std::string trimmed = trim_constructor_whitespace(Value);
   if (trimmed.empty()) return result;

   std::string_view working(trimmed);

   if ((working.length() >= 2) and (working[0] IS 'Q') and (working[1] IS '{')) {
      size_t closing = working.find('}');
      if (closing IS std::string_view::npos) return result;

      result.namespace_uri = std::string(working.substr(2, closing - 2));
      std::string_view remainder = working.substr(closing + 1);
      if (remainder.empty()) return result;
      if (not is_valid_ncname(remainder)) return result;

      result.local = std::string(remainder);
      result.valid = true;
      return result;
   }

   size_t colon = working.find(':');
   if (colon IS std::string_view::npos) {
      if (not is_valid_ncname(working)) return result;
      result.local = std::string(working);
      result.valid = true;
      return result;
   }

   std::string_view prefix_view = working.substr(0, colon);
   std::string_view local_view = working.substr(colon + 1);
   if (prefix_view.empty() or local_view.empty()) return result;
   if (not is_valid_ncname(prefix_view) or !is_valid_ncname(local_view)) return result;

   result.prefix = std::string(prefix_view);
   result.local = std::string(local_view);
   result.valid = true;
   return result;
}

namespace {

static constexpr std::string_view xml_schema_namespace_uri("http://www.w3.org/2001/XMLSchema");
static constexpr std::string_view xpath_functions_namespace_uri("http://www.w3.org/2005/xpath-functions");
static constexpr std::string_view xquery_local_namespace_uri("http://www.w3.org/2005/xquery-local-functions");
static constexpr std::string_view xml_namespace_uri("http://www.w3.org/XML/1998/namespace");
static constexpr std::string_view xmlns_namespace_uri("http://www.w3.org/2000/xmlns/");

enum class ConstructorLookupStatus
{
   NotConstructor,
   Resolved,
   UnknownNamespace,
   UnknownType
};

struct ConstructorLookupResult
{
   ConstructorLookupStatus status = ConstructorLookupStatus::NotConstructor;
   std::shared_ptr<xml::schema::SchemaTypeDescriptor> descriptor;
   std::string display_name;
};

[[nodiscard]] static bool descriptor_supports_constructor(const std::shared_ptr<xml::schema::SchemaTypeDescriptor> &Descriptor)
{
   if (!Descriptor) return false;

   switch (Descriptor->schema_type) {
      case xml::schema::SchemaType::XPathNodeSet:
      case xml::schema::SchemaType::XSAnyType:
         return false;
      default:
         break;
   }

   if (Descriptor->type_name.rfind("xpath:", 0) IS 0) return false;
   return true;
}

[[nodiscard]] static std::optional<std::string> resolve_namespace_uri(const XPathContext &Context, std::string_view Prefix)
{
   if (Prefix.empty()) return std::nullopt;

   if (auto prolog = Context.prolog) {
      auto iter = prolog->declared_namespace_uris.find(std::string(Prefix));
      if (iter != prolog->declared_namespace_uris.end()) return iter->second;
   }

   if (Prefix IS std::string_view("xs")) return std::string(xml_schema_namespace_uri);
   if (Prefix IS std::string_view("fn")) return std::string(xpath_functions_namespace_uri);
   if (Prefix IS std::string_view("local")) return std::string(xquery_local_namespace_uri);
   if (Prefix IS std::string_view("xml")) return std::string(xml_namespace_uri);
   if (Prefix IS std::string_view("xmlns")) return std::string(xmlns_namespace_uri);
   return std::nullopt;
}

[[nodiscard]] static ConstructorLookupResult resolve_constructor_descriptor(
   std::string_view FunctionName, const XPathContext &Context)
{
   ConstructorLookupResult result;
   result.display_name.assign(FunctionName);

   auto *registry = Context.schema_registry ? Context.schema_registry : &xml::schema::registry();
   if (!registry) return result;

   auto descriptor = registry->find_descriptor(FunctionName);
   if (descriptor_supports_constructor(descriptor)) {
      result.status = ConstructorLookupStatus::Resolved;
      result.descriptor = descriptor;
      return result;
   }

   auto parsed = parse_constructor_qname_string(FunctionName);
   if ((not parsed.valid) or parsed.local.empty()) return result;

   bool prefix_unresolved = false;
   std::string namespace_uri = parsed.namespace_uri;

   if (namespace_uri.empty() and (not parsed.prefix.empty())) {
      auto resolved = resolve_namespace_uri(Context, parsed.prefix);
      if (resolved.has_value()) namespace_uri = *resolved;
      else prefix_unresolved = true;
   }

   bool namespace_is_schema = (!namespace_uri.empty()) and (namespace_uri IS std::string(xml_schema_namespace_uri));
   bool namespace_is_reserved = (!namespace_uri.empty()) and
      ((namespace_uri IS std::string(xpath_functions_namespace_uri)) or
       (namespace_uri IS std::string(xquery_local_namespace_uri)) or
       (namespace_uri IS std::string(xml_namespace_uri)) or
       (namespace_uri IS std::string(xmlns_namespace_uri)));
   bool namespace_supports_types = namespace_is_schema;

   if ((!namespace_supports_types) and (!namespace_is_reserved) and (!namespace_uri.empty())) {
      namespace_supports_types = registry->namespace_contains_types(namespace_uri);
   }

   if (!namespace_uri.empty()) {
      descriptor = registry->find_descriptor(namespace_uri, parsed.local);
      if (descriptor_supports_constructor(descriptor)) {
         result.status = ConstructorLookupStatus::Resolved;
         result.descriptor = descriptor;
         return result;
      }
   }

   if (!parsed.prefix.empty()) {
      auto lexical_name = std::format("{}:{}", parsed.prefix, parsed.local);
      descriptor = registry->find_descriptor(lexical_name);
      if (descriptor_supports_constructor(descriptor)) {
         result.status = ConstructorLookupStatus::Resolved;
         result.descriptor = descriptor;
         return result;
      }
   }

   if (prefix_unresolved) {
      result.status = ConstructorLookupStatus::UnknownNamespace;
      return result;
   }

   if (namespace_is_schema or namespace_supports_types) {
      result.status = ConstructorLookupStatus::UnknownType;
   }

   return result;
}

static bool parse_qname_lexical_value(std::string_view Value, std::string &Prefix, std::string &Local)
{
   auto trimmed = trim_view(Value);
   if (trimmed.empty()) return false;

   size_t colon = trimmed.find(':');
   if (colon IS std::string_view::npos)
   {
      if (not is_valid_ncname(trimmed)) return false;
      Prefix.clear();
      Local.assign(trimmed);
      return true;
   }

   std::string_view prefix_view = trimmed.substr(0, colon);
   std::string_view local_view = trimmed.substr(colon + 1);
   if (prefix_view.empty() or local_view.empty()) return false;
   if (not is_valid_ncname(prefix_view) or not is_valid_ncname(local_view)) return false;
   if (prefix_view IS std::string_view("xmlns")) return false;

   Prefix.assign(prefix_view);
   Local.assign(local_view);
   return true;
}

static std::string canonicalise_qname_value(std::string_view NamespaceURI, std::string_view Prefix,
   std::string_view Local)
{
   std::string result("Q{");
   result.append(NamespaceURI);
   result.push_back('}');
   if (!Prefix.empty())
   {
      result.append(Prefix);
      result.push_back(':');
   }
   result.append(Local);
   return result;
}

static std::optional<std::string> resolve_default_element_namespace(const XPathContext &Context)
{
   if (auto prolog = Context.prolog)
   {
      if (prolog->default_element_namespace_uri.has_value()) return prolog->default_element_namespace_uri;
   }
   return std::nullopt;
}

} // namespace

//********************************************************************************************************************

std::optional<std::string> XPathEvaluator::prepare_constructor_text(std::string_view Text, bool IsLiteral) const
{
   if (Text.empty()) {
      if (IsLiteral) return std::string();
      if (prolog_has_boundary_space_preserve()) return std::string();
      return std::nullopt;
   }

   bool whitespace_only = is_xml_whitespace_only(Text);

   if (IsLiteral) {
      if (whitespace_only and (not prolog_has_boundary_space_preserve())) return std::nullopt;
      return std::string(Text);
   }

   if (prolog_construction_preserve()) return std::string(Text);

   if (whitespace_only) {
      if (prolog_has_boundary_space_preserve()) return std::string(Text);
      return std::nullopt;
   }

   if (prolog_has_boundary_space_preserve()) return std::string(Text);

   std::string trimmed = trim_constructor_whitespace(Text);
   if (trimmed.empty()) return std::nullopt;
   return trimmed;
}

//********************************************************************************************************************
// Attempts to resolve a function call against the prolog before consulting the built-in library.

std::optional<XPathVal> XPathEvaluator::resolve_user_defined_function(std::string_view FunctionName,
   const std::vector<XPathVal> &Args, uint32_t CurrentPrefix, const XPathNode *FuncNode)
{
   auto prolog = context.prolog;
   if (!prolog) return std::nullopt;

   std::string namespace_uri;
   bool has_expanded_name = false;

   if ((FunctionName.size() > 2) and (FunctionName[0] IS 'Q') and (FunctionName[1] IS '{')) {
      size_t closing = FunctionName.find('}');
      if (closing != std::string::npos) {
         namespace_uri = std::string(FunctionName.substr(2, closing - 2));
         has_expanded_name = true;
      }
   }

   const XQueryFunction *function = prolog->find_function(FunctionName, Args.size());
   if (function) {
      if (function->is_external) {
         auto message = std::format("External function '{}' is not supported.", function->qname);
         record_error(message, FuncNode, true);
         return XPathVal();
      }

      return evaluate_user_defined_function(*function, Args, CurrentPrefix, FuncNode);
   }

   std::string canonical_name(FunctionName);
   bool arity_mismatch = false;
   for (const auto &entry : prolog->functions) {
      if (entry.second.qname IS canonical_name) {
         arity_mismatch = true;
         break;
      }
   }

   if (arity_mismatch) {
      auto message = std::format("Function '{}' does not accept {} {}.",
         canonical_name, Args.size(), Args.size() IS 1 ? "argument" : "arguments");
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   uint32_t namespace_hash = 0;
   std::string module_uri;

   if (has_expanded_name) {
      if (!namespace_uri.empty()) {
         namespace_hash = pf::strhash(namespace_uri);
         module_uri = namespace_uri;
      }
   }
   else {
      auto separator = FunctionName.find(':');
      if (separator != std::string_view::npos) {
         std::string prefix(FunctionName.substr(0, separator));
         namespace_hash = prolog->resolve_prefix(prefix, context.xml);
         if (namespace_hash != 0) {
            auto uri_entry = prolog->declared_namespace_uris.find(prefix);
            if (uri_entry != prolog->declared_namespace_uris.end()) module_uri = uri_entry->second;
            else if (context.xml) {
               auto prefix_it = context.xml->Prefixes.find(prefix);
               if (prefix_it != context.xml->Prefixes.end()) {
                  auto ns_it = context.xml->NSRegistry.find(prefix_it->second);
                  if (ns_it != context.xml->NSRegistry.end()) module_uri = ns_it->second;
               }
            }
         }
      }
   }

   const XQueryModuleImport *matched_import = nullptr;
   if (namespace_hash != 0) {
      for (const auto &import : prolog->module_imports) {
         if (pf::strhash(import.target_namespace) IS namespace_hash) {
            matched_import = &import;
            if (module_uri.empty()) module_uri = import.target_namespace;
            break;
         }
      }
   }

   if (!matched_import) return std::nullopt;

   if (module_uri.empty()) {
      auto message = std::format("Module function '{}' has an unresolved namespace.", canonical_name);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   auto module_cache = context.module_cache;
   if (!module_cache) {
      auto message = std::format("Module function '{}' requires a module cache.", canonical_name);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   (void)module_cache->fetch_or_load(module_uri, *prolog, *this);

   auto module_info = module_cache->find_module(module_uri);
   if (!module_info) {
      auto message = std::format("Module '{}' could not be loaded for function '{}'.", module_uri, canonical_name);
      // Do not force override; preserve earlier, more specific loader diagnostics
      record_error(message, FuncNode, false);
      return XPathVal();
   }

   auto module_prolog = module_info->prolog;
   if (!module_prolog) {
      auto message = std::format("Module '{}' does not expose a prolog.", module_uri);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   auto module_function = module_prolog->find_function(FunctionName, Args.size());
   if (!module_function) {
      auto alternative_name = module_prolog->normalise_function_qname(FunctionName, module_info->expression.get());
      if (alternative_name != FunctionName) {
         module_function = module_prolog->find_function(alternative_name, Args.size());
      }
   }

   if (!module_function) {
      auto message = std::format("Module function '{}' is not exported by namespace '{}'.", canonical_name, module_uri);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   auto previous_prolog = context.prolog;
   auto previous_cache = context.module_cache;

   context.prolog = module_prolog;
   context.module_cache = module_cache;

   XPathVal resolved_value = evaluate_user_defined_function(*module_function, Args, CurrentPrefix, FuncNode);

   context.prolog = previous_prolog;
   context.module_cache = previous_cache;

   return resolved_value;
}

//********************************************************************************************************************
// Evaluates a prolog-defined function by binding arguments and executing the stored body expression.

XPathVal XPathEvaluator::evaluate_user_defined_function(const XQueryFunction &Function,
   const std::vector<XPathVal> &Args, uint32_t CurrentPrefix, const XPathNode *FuncNode)
{
   if (Function.is_external) {
      auto message = std::format("External function '{}' is not supported.", Function.qname);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   if (!Function.body) {
      auto message = std::format("Function '{}' is missing a body.", Function.qname);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   if (Function.parameter_names.size() != Args.size()) {
      auto message = std::format("Function '{}' parameter mismatch.", Function.qname);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   std::vector<VariableBindingGuard> parameter_guards;
   parameter_guards.reserve(Function.parameter_names.size());

   for (size_t index = 0; index < Function.parameter_names.size(); ++index) {
      parameter_guards.emplace_back(context, Function.parameter_names[index], Args[index]);
   }

   auto result = evaluate_expression(Function.body.get(), CurrentPrefix);
   if (expression_unsupported) {
      auto message = std::format("Function '{}' evaluation failed.", Function.qname);
      record_error(message, FuncNode);
   }

   return result;
}

//********************************************************************************************************************
// Evaluates a path expression node and returns the resulting node set as an XPathVal.

XPathVal XPathEvaluator::evaluate_path_expression_value(const XPathNode *PathNode, uint32_t CurrentPrefix)
{
   if (not PathNode) {
      expression_unsupported = true;
      return XPathVal();
   }

   const XPathNode *location = PathNode;
   if (PathNode->type IS XQueryNodeType::PATH) {
      if (PathNode->child_count() IS 0) return XPathVal();
      location = PathNode->get_child(0);
   }

   if ((not location) or (location->type != XQueryNodeType::LOCATION_PATH)) {
      expression_unsupported = true;
      return XPathVal();
   }

   std::vector<const XPathNode *> steps;
   std::vector<std::unique_ptr<XPathNode>> synthetic_steps;

   bool has_root = false;
   bool root_descendant = false;

   for (size_t index = 0; index < location->child_count(); ++index) {
      auto *child = location->get_child(index);
      if (not child) continue;

      if ((index IS 0) and (child->type IS XQueryNodeType::ROOT)) {
         has_root = true;
         root_descendant = child->value IS "//";
         continue;
      }

      if (child->type IS XQueryNodeType::STEP) steps.push_back(child);
   }

   if (root_descendant) {
      auto descendant_step = std::make_unique<XPathNode>(XQueryNodeType::STEP);
      descendant_step->add_child(std::make_unique<XPathNode>(XQueryNodeType::AXIS_SPECIFIER, "descendant-or-self"));
      descendant_step->add_child(std::make_unique<XPathNode>(XQueryNodeType::NODE_TYPE_TEST, "node"));
      steps.insert(steps.begin(), descendant_step.get());
      synthetic_steps.push_back(std::move(descendant_step));
   }

   NODES initial_context;

   if (has_root) initial_context.push_back(nullptr);
   else {
      if (context.context_node) initial_context.push_back(context.context_node);
      else initial_context.push_back(nullptr);
   }

   if (steps.empty()) return XPathVal(initial_context);

   const XPathNode *attribute_step = nullptr;
   const XPathNode *attribute_test = nullptr;

   auto last_step = steps.back();
   if (last_step) {
      const XPathNode *axis_node = nullptr;
      const XPathNode *node_test = nullptr;

      for (size_t index = 0; index < last_step->child_count(); ++index) {
         auto *child = last_step->get_child(index);
         if (not child) continue;

         if (child->type IS XQueryNodeType::AXIS_SPECIFIER) axis_node = child;
         else if ((not node_test) and ((child->type IS XQueryNodeType::NAME_TEST) or
            (child->type IS XQueryNodeType::WILDCARD) or
            (child->type IS XQueryNodeType::NODE_TYPE_TEST))) node_test = child;
      }

      AxisType axis = axis_node ? AxisEvaluator::parse_axis_name(axis_node->value) : AxisType::CHILD;
      if (axis IS AxisType::ATTRIBUTE) {
         attribute_step = last_step;
         attribute_test = node_test;
      }
   }

   std::vector<const XPathNode *> work_steps = steps;
   if (attribute_step) work_steps.pop_back();

   bool unsupported = false;
   NODES node_results;

   if (work_steps.empty()) {
      for (auto *candidate : initial_context) {
         if (candidate) node_results.push_back(candidate);
      }
   }
   else {
      std::vector<AxisMatch> initial_matches;
      initial_matches.reserve(initial_context.size());

      for (auto *candidate : initial_context) {
         const XMLAttrib *attribute = nullptr;
         if ((candidate) and context.attribute_node and (candidate IS context.context_node)) attribute = context.attribute_node;
         initial_matches.push_back({ candidate, attribute });
      }

      node_results = collect_step_results(initial_matches, work_steps, 0, CurrentPrefix, unsupported);
   }

   if (unsupported) {
      expression_unsupported = true;
      return XPathVal();
   }

   axis_evaluator.normalise_node_set(node_results);

   if (context.attribute_node and (steps.size() IS 1)) {
      const XPathNode *step = steps[0];
      const XPathNode *axis_node = nullptr;
      const XPathNode *node_test = nullptr;

      for (size_t index = 0; index < step->child_count(); ++index) {
         auto *child = step->get_child(index);
         if (not child) continue;

         if (child->type IS XQueryNodeType::AXIS_SPECIFIER) axis_node = child;
         else if ((not node_test) and ((child->type IS XQueryNodeType::NAME_TEST) or
            (child->type IS XQueryNodeType::WILDCARD) or
            (child->type IS XQueryNodeType::NODE_TYPE_TEST))) node_test = child;
      }

      AxisType axis = axis_node ? AxisEvaluator::parse_axis_name(axis_node->value) : AxisType::CHILD;

      if ((axis IS AxisType::SELF) and !node_results.empty()) {
         bool accepts_attribute = false;

         if (not node_test) accepts_attribute = true;
         else if (node_test->type IS XQueryNodeType::WILDCARD) accepts_attribute = true;
         else if (node_test->type IS XQueryNodeType::NODE_TYPE_TEST) accepts_attribute = node_test->value IS "node";

         if (accepts_attribute) {
            std::vector<const XMLAttrib *> attribute_refs(node_results.size(), context.attribute_node);
            return XPathVal(node_results, context.attribute_node->Value, {}, std::move(attribute_refs));
         }
      }
   }

   if (attribute_step) {
      std::vector<std::string> attribute_values;
      NODES attribute_nodes;
      std::vector<const XMLAttrib *> attribute_refs;
      std::vector<const XPathNode *> attribute_predicates;

      for (size_t index = 0; index < attribute_step->child_count(); ++index) {
         auto *child = attribute_step->get_child(index);
         if (child and (child->type IS XQueryNodeType::PREDICATE)) attribute_predicates.push_back(child);
      }

      for (auto *candidate : node_results) {
         if (not candidate) continue;

         auto matches = dispatch_axis(AxisType::ATTRIBUTE, candidate);
         if (matches.empty()) continue;

         std::vector<AxisMatch> filtered;
         filtered.reserve(matches.size());

         for (auto &match : matches) {
            if (not match.attribute) continue;
            if (not match_node_test(attribute_test, AxisType::ATTRIBUTE, match.node, match.attribute, CurrentPrefix)) continue;
            filtered.push_back(match);
         }

         if (filtered.empty()) continue;

         if (not attribute_predicates.empty()) {
            std::vector<AxisMatch> predicate_buffer;
            predicate_buffer.reserve(filtered.size());

            for (auto *predicate_node : attribute_predicates) {
               predicate_buffer.clear();
               predicate_buffer.reserve(filtered.size());

               for (size_t index = 0; index < filtered.size(); ++index) {
                  auto &match = filtered[index];

                  push_context(match.node, index + 1, filtered.size(), match.attribute);
                  auto predicate_result = evaluate_predicate(predicate_node, CurrentPrefix);
                  pop_context();

                  if (predicate_result IS PredicateResult::UNSUPPORTED) {
                     expression_unsupported = true;
                     return XPathVal();
                  }

                  if (predicate_result IS PredicateResult::MATCH) predicate_buffer.push_back(match);
               }

               filtered.swap(predicate_buffer);
               if (filtered.empty()) break;
            }

            if (filtered.empty()) continue;
         }

         for (auto &match : filtered) {
            attribute_values.push_back(match.attribute->Value);
            attribute_nodes.push_back(match.node);
            attribute_refs.push_back(match.attribute);
         }
      }

      if (attribute_nodes.empty()) return XPathVal(attribute_nodes);

      std::optional<std::string> first_value;
      if (not attribute_values.empty()) first_value = attribute_values[0];
      return XPathVal(attribute_nodes, first_value, std::move(attribute_values), std::move(attribute_refs));
   }

   return XPathVal(node_results);
}

//********************************************************************************************************************

XPathVal XPathEvaluator::evaluate_path_from_nodes(const NODES &InitialContext,
   const std::vector<const XMLAttrib *> &InitialAttributes, const std::vector<const XPathNode *> &Steps,
   const XPathNode *AttributeStep, const XPathNode *AttributeTest, uint32_t CurrentPrefix)
{
   std::vector<const XPathNode *> work_steps = Steps;

   if (AttributeStep and !work_steps.empty()) work_steps.pop_back();

   NODES node_results;

   if (work_steps.empty()) {
      node_results = InitialContext;
   }
   else {
      std::vector<AxisMatch> initial_matches;
      initial_matches.reserve(InitialContext.size());

      for (size_t index = 0; index < InitialContext.size(); ++index) {
         auto *candidate = InitialContext[index];
         const XMLAttrib *attribute = nullptr;
         if (index < InitialAttributes.size()) attribute = InitialAttributes[index];
         initial_matches.push_back({ candidate, attribute });
      }

      bool unsupported = false;
      node_results = collect_step_results(initial_matches, work_steps, 0, CurrentPrefix, unsupported);

      if (unsupported) {
         expression_unsupported = true;
         return XPathVal();
      }
   }

   axis_evaluator.normalise_node_set(node_results);

   if (AttributeStep) {
      std::vector<std::string> attribute_values;
      NODES attribute_nodes;
      std::vector<const XMLAttrib *> attribute_refs;
      std::vector<const XPathNode *> attribute_predicates;

      for (size_t index = 0; index < AttributeStep->child_count(); ++index) {
         auto *child = AttributeStep->get_child(index);
         if (child and (child->type IS XQueryNodeType::PREDICATE)) attribute_predicates.push_back(child);
      }

      for (auto *candidate : node_results) {
         if (not candidate) continue;

         auto matches = dispatch_axis(AxisType::ATTRIBUTE, candidate);
         if (matches.empty()) continue;

         std::vector<AxisMatch> filtered;
         filtered.reserve(matches.size());

         for (auto &match : matches) {
            if (not match.attribute) continue;
            if (not match_node_test(AttributeTest, AxisType::ATTRIBUTE, match.node, match.attribute, CurrentPrefix)) continue;
            filtered.push_back(match);
         }

         if (filtered.empty()) continue;

         if (not attribute_predicates.empty()) {
            std::vector<AxisMatch> predicate_buffer;
            predicate_buffer.reserve(filtered.size());

            for (auto *predicate_node : attribute_predicates) {
               predicate_buffer.clear();
               predicate_buffer.reserve(filtered.size());

               for (size_t index = 0; index < filtered.size(); ++index) {
                  auto &match = filtered[index];

                  push_context(match.node, index + 1, filtered.size(), match.attribute);
                  auto predicate_result = evaluate_predicate(predicate_node, CurrentPrefix);
                  pop_context();

                  if (predicate_result IS PredicateResult::UNSUPPORTED) {
                     expression_unsupported = true;
                     return XPathVal();
                  }

                  if (predicate_result IS PredicateResult::MATCH) predicate_buffer.push_back(match);
               }

               filtered.swap(predicate_buffer);
               if (filtered.empty()) break;
            }

            if (filtered.empty()) continue;
         }

         for (auto &match : filtered) {
            attribute_values.push_back(match.attribute->Value);
            attribute_nodes.push_back(match.node);
            attribute_refs.push_back(match.attribute);
         }
      }

      if (attribute_nodes.empty()) return XPathVal(attribute_nodes);

      std::optional<std::string> first_value;
      if (not attribute_values.empty()) first_value = attribute_values[0];
      return XPathVal(attribute_nodes, first_value, std::move(attribute_values), std::move(attribute_refs));
   }

   return XPathVal(node_results);
}

//********************************************************************************************************************

XPathVal XPathEvaluator::evaluate_union_value(const std::vector<const XPathNode *> &Branches, uint32_t CurrentPrefix)
{
   struct NodeIdentity {
      XTag * node;
      const XMLAttrib * attribute;
   };

   struct NodeIdentityHash {
      size_t operator()(const NodeIdentity &Value) const {
         size_t node_hash = std::hash<XTag *>()(Value.node);
         size_t attrib_hash = std::hash<const XMLAttrib *>()(Value.attribute);
         return node_hash ^ (attrib_hash << 1);
      }
   };

   struct NodeIdentityEqual {
      bool operator()(const NodeIdentity &Left, const NodeIdentity &Right) const {
         return (Left.node IS Right.node) and (Left.attribute IS Right.attribute);
      }
   };

   auto saved_context = context;
   auto saved_context_stack = context_stack;
   bool saved_expression_unsupported = expression_unsupported;

   std::unordered_set<NodeIdentity, NodeIdentityHash, NodeIdentityEqual> seen_entries;
   seen_entries.reserve(Branches.size() * 4);

   struct UnionEntry {
      XTag * node = nullptr;
      const XMLAttrib * attribute = nullptr;
      std::string string_value;
   };

   std::vector<UnionEntry> entries;
   entries.reserve(Branches.size() * 4);

   std::optional<std::string> combined_override;

   for (auto *branch : Branches) {
      if (not branch) continue;

      context = saved_context;
      context_stack = saved_context_stack;
      expression_unsupported = saved_expression_unsupported;

      auto branch_value = evaluate_expression(branch, CurrentPrefix);
      if (expression_unsupported) {
         context = saved_context;
         context_stack = saved_context_stack;
         expression_unsupported = true;
         return XPathVal();
      }

      if (branch_value.Type != XPVT::NodeSet) {
         context = saved_context;
         context_stack = saved_context_stack;
         expression_unsupported = true;
         return XPathVal();
      }

      for (size_t index = 0; index < branch_value.node_set.size(); ++index) {
         XTag *node = branch_value.node_set[index];
         const XMLAttrib *attribute = nullptr;
         if (index < branch_value.node_set_attributes.size()) attribute = branch_value.node_set_attributes[index];

         NodeIdentity identity { node, attribute };
         if (not seen_entries.insert(identity).second) continue;

         UnionEntry entry;
         entry.node = node;
         entry.attribute = attribute;

         if (index < branch_value.node_set_string_values.size()) entry.string_value = branch_value.node_set_string_values[index];
         else entry.string_value = XPathVal::node_string_value(node);

         if (not combined_override.has_value()) {
            if (branch_value.node_set_string_override.has_value()) combined_override = branch_value.node_set_string_override;
            else combined_override = entry.string_value;
         }

         entries.push_back(std::move(entry));
      }
   }

   bool enforce_document_order = prolog_ordering_is_ordered();

   if (enforce_document_order)
   {
      std::stable_sort(entries.begin(), entries.end(), [this](const UnionEntry &Left, const UnionEntry &Right) {
         if (Left.node IS Right.node) return false;
         return axis_evaluator.is_before_in_document_order(Left.node, Right.node);
      });
   }

   NODES combined_nodes;
   std::vector<const XMLAttrib *> combined_attributes;
   std::vector<std::string> combined_strings;
   combined_nodes.reserve(entries.size());
   combined_attributes.reserve(entries.size());
   combined_strings.reserve(entries.size());

   for (const auto &entry : entries) {
      combined_nodes.push_back(entry.node);
      combined_attributes.push_back(entry.attribute);
      combined_strings.push_back(entry.string_value);
   }

   context         = saved_context;
   context_stack   = saved_context_stack;
   expression_unsupported = saved_expression_unsupported;

   if (combined_nodes.empty()) return XPathVal(NODES());

   XPathVal result(combined_nodes, combined_override, std::move(combined_strings), std::move(combined_attributes));
   if (not enforce_document_order) result.preserve_node_order = true;
   return result;
}

//********************************************************************************************************************

XPathVal XPathEvaluator::evaluate_intersect_value(const XPathNode *Left, const XPathNode *Right, uint32_t CurrentPrefix)
{
   struct NodeIdentity {
      XTag * node;
      const XMLAttrib * attribute;
   };

   struct NodeIdentityHash {
      size_t operator()(const NodeIdentity &Value) const {
         size_t node_hash = std::hash<XTag *>()(Value.node);
         size_t attrib_hash = std::hash<const XMLAttrib *>()(Value.attribute);
         return node_hash ^ (attrib_hash << 1);
      }
   };

   struct NodeIdentityEqual {
      bool operator()(const NodeIdentity &LeftIdentity, const NodeIdentity &RightIdentity) const {
         return (LeftIdentity.node IS RightIdentity.node) and (LeftIdentity.attribute IS RightIdentity.attribute);
      }
   };

   struct SetEntry {
      XTag * node = nullptr;
      const XMLAttrib * attribute = nullptr;
      std::string string_value;
   };

   auto saved_context = context;
   auto saved_context_stack = context_stack;
   bool saved_expression_unsupported = expression_unsupported;

   auto evaluate_operand = [&](const XPathNode *Operand) -> std::optional<XPathVal> {
      context = saved_context;
      context_stack = saved_context_stack;
      expression_unsupported = saved_expression_unsupported;

      auto value = evaluate_expression(Operand, CurrentPrefix);
      if (expression_unsupported) return std::nullopt;

      if (value.Type != XPVT::NodeSet) {
         expression_unsupported = true;
         return std::nullopt;
      }

      return value;
   };

   auto left_value_opt = evaluate_operand(Left);
   if (not left_value_opt.has_value()) {
      context         = saved_context;
      context_stack   = saved_context_stack;
      return XPathVal();
   }

   auto right_value_opt = evaluate_operand(Right);
   if (not right_value_opt.has_value()) {
      context         = saved_context;
      context_stack   = saved_context_stack;
      return XPathVal();
   }

   const XPathVal &left_value = *left_value_opt;
   const XPathVal &right_value = *right_value_opt;

   std::unordered_set<NodeIdentity, NodeIdentityHash, NodeIdentityEqual> right_entries;
   right_entries.reserve(right_value.node_set.size() * 2);

   for (size_t index = 0; index < right_value.node_set.size(); ++index) {
      XTag *node = right_value.node_set[index];
      const XMLAttrib *attribute = nullptr;
      if (index < right_value.node_set_attributes.size()) attribute = right_value.node_set_attributes[index];

      NodeIdentity identity { node, attribute };
      right_entries.insert(identity);
   }

   std::unordered_set<NodeIdentity, NodeIdentityHash, NodeIdentityEqual> inserted;
   inserted.reserve(left_value.node_set.size());

   std::vector<SetEntry> entries;
   entries.reserve(left_value.node_set.size());

   std::optional<std::string> combined_override = left_value.node_set_string_override;

   for (size_t index = 0; index < left_value.node_set.size(); ++index) {
      XTag *node = left_value.node_set[index];
      const XMLAttrib *attribute = nullptr;
      if (index < left_value.node_set_attributes.size()) attribute = left_value.node_set_attributes[index];

      NodeIdentity identity { node, attribute };
      if (not right_entries.contains(identity)) continue;
      if (not inserted.insert(identity).second) continue;

      SetEntry entry;
      entry.node = node;
      entry.attribute = attribute;

      if (index < left_value.node_set_string_values.size()) entry.string_value = left_value.node_set_string_values[index];
      else entry.string_value = XPathVal::node_string_value(node);

      if (not combined_override.has_value()) combined_override = entry.string_value;

      entries.push_back(std::move(entry));
   }

   bool enforce_document_order = prolog_ordering_is_ordered();

   if (enforce_document_order)
   {
      std::stable_sort(entries.begin(), entries.end(), [this](const SetEntry &LeftEntry, const SetEntry &RightEntry) {
         if (LeftEntry.node IS RightEntry.node) return false;
         return axis_evaluator.is_before_in_document_order(LeftEntry.node, RightEntry.node);
      });
   }

   NODES combined_nodes;
   std::vector<const XMLAttrib *> combined_attributes;
   std::vector<std::string> combined_strings;
   combined_nodes.reserve(entries.size());
   combined_attributes.reserve(entries.size());
   combined_strings.reserve(entries.size());

   for (const auto &entry : entries) {
      combined_nodes.push_back(entry.node);
      combined_attributes.push_back(entry.attribute);
      combined_strings.push_back(entry.string_value);
   }

   context = saved_context;
   context_stack   = saved_context_stack;
   expression_unsupported = saved_expression_unsupported;

   if (combined_nodes.empty()) return XPathVal(NODES());

   XPathVal result(combined_nodes, combined_override, std::move(combined_strings), std::move(combined_attributes));
   if (not enforce_document_order) result.preserve_node_order = true;
   return result;
}

//********************************************************************************************************************

XPathVal XPathEvaluator::evaluate_except_value(const XPathNode *Left, const XPathNode *Right, uint32_t CurrentPrefix)
{
   struct NodeIdentity {
      XTag * node;
      const XMLAttrib * attribute;
   };

   struct NodeIdentityHash {
      size_t operator()(const NodeIdentity &Value) const {
         size_t node_hash = std::hash<XTag *>()(Value.node);
         size_t attrib_hash = std::hash<const XMLAttrib *>()(Value.attribute);
         return node_hash ^ (attrib_hash << 1);
      }
   };

   struct NodeIdentityEqual {
      bool operator()(const NodeIdentity &LeftIdentity, const NodeIdentity &RightIdentity) const {
         return (LeftIdentity.node IS RightIdentity.node) and (LeftIdentity.attribute IS RightIdentity.attribute);
      }
   };

   struct SetEntry {
      XTag * node = nullptr;
      const XMLAttrib * attribute = nullptr;
      std::string string_value;
   };

   auto saved_context       = context;
   auto saved_context_stack = context_stack;
   bool saved_expression_unsupported = expression_unsupported;

   auto evaluate_operand = [&](const XPathNode *Operand) -> std::optional<XPathVal> {
      context = saved_context;
      context_stack = saved_context_stack;
      expression_unsupported = saved_expression_unsupported;

      auto value = evaluate_expression(Operand, CurrentPrefix);
      if (expression_unsupported) return std::nullopt;

      if (value.Type != XPVT::NodeSet) {
         expression_unsupported = true;
         return std::nullopt;
      }

      return value;
   };

   auto left_value_opt = evaluate_operand(Left);
   if (not left_value_opt.has_value()) {
      context = saved_context;
      context_stack = saved_context_stack;
      return XPathVal();
   }

   auto right_value_opt = evaluate_operand(Right);
   if (not right_value_opt.has_value()) {
      context = saved_context;
      context_stack = saved_context_stack;
      return XPathVal();
   }

   const XPathVal &left_value = *left_value_opt;
   const XPathVal &right_value = *right_value_opt;

   std::unordered_set<NodeIdentity, NodeIdentityHash, NodeIdentityEqual> right_entries;
   right_entries.reserve(right_value.node_set.size() * 2);

   for (size_t index = 0; index < right_value.node_set.size(); ++index) {
      XTag *node = right_value.node_set[index];
      const XMLAttrib *attribute = nullptr;
      if (index < right_value.node_set_attributes.size()) attribute = right_value.node_set_attributes[index];

      NodeIdentity identity { node, attribute };
      right_entries.insert(identity);
   }

   std::unordered_set<NodeIdentity, NodeIdentityHash, NodeIdentityEqual> inserted;
   inserted.reserve(left_value.node_set.size());

   std::vector<SetEntry> entries;
   entries.reserve(left_value.node_set.size());

   std::optional<std::string> combined_override = left_value.node_set_string_override;

   for (size_t index = 0; index < left_value.node_set.size(); ++index) {
      XTag *node = left_value.node_set[index];
      const XMLAttrib *attribute = nullptr;
      if (index < left_value.node_set_attributes.size()) attribute = left_value.node_set_attributes[index];

      NodeIdentity identity { node, attribute };
      if (right_entries.contains(identity)) continue;
      if (not inserted.insert(identity).second) continue;

      SetEntry entry;
      entry.node = node;
      entry.attribute = attribute;

      if (index < left_value.node_set_string_values.size()) entry.string_value = left_value.node_set_string_values[index];
      else entry.string_value = XPathVal::node_string_value(node);

      if (not combined_override.has_value()) combined_override = entry.string_value;

      entries.push_back(std::move(entry));
   }

   bool enforce_document_order = prolog_ordering_is_ordered();

   if (enforce_document_order)
   {
      std::stable_sort(entries.begin(), entries.end(), [this](const SetEntry &LeftEntry, const SetEntry &RightEntry) {
         if (LeftEntry.node IS RightEntry.node) return false;
         return axis_evaluator.is_before_in_document_order(LeftEntry.node, RightEntry.node);
      });
   }

   NODES combined_nodes;
   std::vector<const XMLAttrib *> combined_attributes;
   std::vector<std::string> combined_strings;
   combined_nodes.reserve(entries.size());
   combined_attributes.reserve(entries.size());
   combined_strings.reserve(entries.size());

   for (const auto &entry : entries) {
      combined_nodes.push_back(entry.node);
      combined_attributes.push_back(entry.attribute);
      combined_strings.push_back(entry.string_value);
   }

   context = saved_context;
   context_stack = saved_context_stack;
   expression_unsupported = saved_expression_unsupported;

   if (combined_nodes.empty()) return XPathVal(NODES());

   XPathVal result(combined_nodes, combined_override, std::move(combined_strings), std::move(combined_attributes));
   if (not enforce_document_order) result.preserve_node_order = true;
   return result;
}

//********************************************************************************************************************
// Registers the supplied namespace URI with the owning XML document so constructed nodes reference consistent
// namespace identifiers.

uint32_t XPathEvaluator::register_constructor_namespace(const std::string &URI) const
{
   if (not xml) return 0;
   return xml->registerNamespace(URI);
}

//********************************************************************************************************************
// Resolves a prefix within the chained constructor namespace scopes, honouring the nearest declaration and falling
// back to the default namespace when the prefix is empty.

std::optional<uint32_t> XPathEvaluator::resolve_constructor_prefix(const ConstructorNamespaceScope &Scope,
   std::string_view Prefix) const
{
   std::string prefix_key(Prefix);
   const ConstructorNamespaceScope *ns_scope = &Scope;

   if (prefix_key.empty()) {
      while (ns_scope) {
         if (ns_scope->default_namespace.has_value()) return ns_scope->default_namespace;
         ns_scope = ns_scope->parent;
      }
      return uint32_t{0};
   }

   while (ns_scope) {
      auto iter = ns_scope->prefix_bindings.find(prefix_key);
      if (iter != ns_scope->prefix_bindings.end()) return iter->second;
      ns_scope = ns_scope->parent;
   }

   return std::nullopt;
}

//********************************************************************************************************************
// Recursively clones an XML node subtree so constructor operations can duplicate existing content without mutating
// the original document tree.

XTag XPathEvaluator::clone_node_subtree(const XTag &Source, int ParentID)
{
   XTag clone(next_constructed_node_id--, Source.LineNo);
   clone.ParentID = ParentID;
   clone.Flags = Source.Flags;
   clone.NamespaceID = Source.NamespaceID;
   clone.Attribs = Source.Attribs;

   clone.Children.reserve(Source.Children.size());
   for (const auto &child : Source.Children) {
      XTag child_clone = clone_node_subtree(child, clone.ID);
      clone.Children.push_back(std::move(child_clone));
   }

   return clone;
}

//********************************************************************************************************************
// Appends a sequence value produced by constructor content into the target element, handling node cloning, attribute
// creation, and text concatenation according to the XPath constructor rules.

bool XPathEvaluator::append_constructor_sequence(XTag &Parent, const XPathVal &Value, uint32_t CurrentPrefix,
   const ConstructorNamespaceScope &Scope, bool PreserveConstruction)
{
   if (Value.Type IS XPVT::NodeSet) {
      Parent.Children.reserve(Parent.Children.size() + Value.node_set.size());

      for (size_t index = 0; index < Value.node_set.size(); ++index) {
         XTag *node = Value.node_set[index];
         if (not node) continue;

         const XMLAttrib *attribute = nullptr;
         if (index < Value.node_set_attributes.size()) attribute = Value.node_set_attributes[index];
         if (attribute) {
            std::string_view attribute_name = attribute->Name;
            if (attribute_name.empty()) continue;

            bool duplicate = false;
            for (size_t attrib_index = 1; attrib_index < Parent.Attribs.size(); ++attrib_index) {
               if (Parent.Attribs[attrib_index].Name IS attribute_name) {
                  duplicate = true;
                  break;
               }
            }

            if (duplicate) {
               record_error("XQDY0025: Duplicate attribute name in constructor content.", nullptr, true);
               return false;
            }

            Parent.Attribs.emplace_back(std::string(attribute_name), attribute->Value);
            continue;
         }

         XTag clone = clone_node_subtree(*node, Parent.ID);
         Parent.Children.push_back(std::move(clone));
      }

      return true;
   }

   std::string text = Value.to_string();

   std::optional<std::string> prepared;
   if (PreserveConstruction) prepared = std::string(text);
   else prepared = prepare_constructor_text(text, false);
   if (not prepared.has_value()) return true;
   text = std::move(*prepared);
   if (text.empty()) return true;

   pf::vector<XMLAttrib> text_attribs;
   text_attribs.emplace_back("", text);

   XTag text_node(next_constructed_node_id--, 0, text_attribs);
   text_node.ParentID = Parent.ID;
   Parent.Children.push_back(std::move(text_node));
   return true;
}

//********************************************************************************************************************
// Evaluates an attribute value template (AVT) collected during parsing.  The template parts alternate between literal
// text and embedded expressions, and the resolved string is returned for assignment to the constructed attribute.

std::optional<std::string> XPathEvaluator::evaluate_attribute_value_template(const XPathConstructorAttribute &Attribute,
   uint32_t CurrentPrefix)
{
   pf::Log log("XPath");
   std::string result;

   for (int index = 0; index < std::ssize(Attribute.value_parts); ++index) {
      const auto &part = Attribute.value_parts[index];
      if (not part.is_expression) {
         result += part.text;
         continue;
      }

      auto *expr = Attribute.get_expression_for_part(index);
      if (not expr) {
         log.detail("AVT failed at part index %d", index);
         record_error("XPST0003: Attribute value template part is missing its expression.", nullptr, true);
         return std::nullopt;
      }

      std::string previous_xml_error;
      if (xml) previous_xml_error = xml->ErrorMsg;

      size_t previous_constructed = constructed_nodes.size();
      auto saved_id = next_constructed_node_id;
      bool previous_flag = expression_unsupported;
      expression_unsupported = false;
      XPathVal value = evaluate_expression(expr, CurrentPrefix);

      bool evaluation_failed = expression_unsupported;
      std::string evaluation_error;
      if (xml) evaluation_error = xml->ErrorMsg;

      if (evaluation_failed) {
         auto build_variable_list = [](const auto &variables) {
            if (variables.empty()) return std::string("[]");
            std::string result = "[";
            bool first = true;
            for (const auto &binding : variables) {
               result += std::format("{}{}", first ? "" : ", ", binding.first);
               first = false;
            }
            return result + "]";
         };

         std::string signature = build_ast_signature(expr);
         std::string variable_list = build_variable_list(*context.variables);

         if (is_trace_enabled()) {
            int variable_count = std::ssize(*context.variables);
            log.msg(VLF::TRACE, "AVT context variable count: %d", variable_count);
            log.msg(VLF::TRACE, "AVT expression failed: %s | context-vars=%s | prev-flag=%s",
               signature.c_str(), variable_list.c_str(), previous_flag ? "true" : "false");
         }

         record_error("Attribute value template expression could not be evaluated.", expr);
         if (xml and xml->ErrorMsg.empty()) xml->ErrorMsg.assign("Attribute value template expression could not be evaluated.");

         constructed_nodes.resize(previous_constructed);
         next_constructed_node_id = saved_id;
         return std::nullopt;
      }

      if ((xml) and (xml->ErrorMsg != previous_xml_error)) xml->ErrorMsg = previous_xml_error;
      result += value.to_string();
      expression_unsupported = previous_flag;
      constructed_nodes.resize(previous_constructed);
      next_constructed_node_id = saved_id;
   }

   return result;
}

//********************************************************************************************************************
// Reduces the child expressions beneath a constructor content node to a single string value.  Each child expression
// is evaluated and the textual representation is concatenated to form the returned content.

std::optional<std::string> XPathEvaluator::evaluate_constructor_content_string(const XPathNode *Node, uint32_t CurrentPrefix,
   bool ApplyWhitespaceRules, bool PreserveConstruction)
{
   if (not Node) return std::string();
   if (not Node->value.empty())
   {
      if (not ApplyWhitespaceRules) return Node->value;
      if (PreserveConstruction) return Node->value;

      auto prepared_literal = prepare_constructor_text(Node->value, not ApplyWhitespaceRules);
      if (prepared_literal.has_value()) return *prepared_literal;
      return std::string();
   }

   if (Node->child_count() IS 0) return std::string();

   const XPathNode *expr = Node->get_child(0);
   if (not expr) return std::string();

   size_t previous_constructed = constructed_nodes.size();
   auto saved_id = next_constructed_node_id;
   XPathVal value = evaluate_expression(expr, CurrentPrefix);
   if (expression_unsupported) {
      if (is_trace_enabled()) {
         std::string signature = build_ast_signature(expr);
         pf::Log("XPath").msg(VLF::TRACE, "Constructor content expression failed: %s", signature.c_str());
      }
      record_error("Constructor content expression could not be evaluated.", expr);
      if (xml and xml->ErrorMsg.empty()) xml->ErrorMsg.assign("Constructor content expression could not be evaluated.");
      constructed_nodes.resize(previous_constructed);
      next_constructed_node_id = saved_id;
      return std::nullopt;
   }

   std::string result;

   if (value.Type IS XPVT::NodeSet) {
      if (value.node_set_string_override.has_value()) result += *value.node_set_string_override;
      else {
         for (size_t index = 0; index < value.node_set.size(); ++index) {
            const XMLAttrib *attribute = nullptr;
            if (index < value.node_set_attributes.size()) attribute = value.node_set_attributes[index];
            if (attribute) {
               result += attribute->Value;
               continue;
            }

            if (not value.node_set_string_values.empty() and (index < value.node_set_string_values.size())) {
               result += value.node_set_string_values[index];
               continue;
            }

            XTag *node = value.node_set[index];
            if (not node) continue;
            result += XPathVal::node_string_value(node);
         }
      }
   }
   else result = value.to_string();

   constructed_nodes.resize(previous_constructed);
   next_constructed_node_id = saved_id;

   if (not ApplyWhitespaceRules) return result;
   if (PreserveConstruction) return result;

   auto prepared = prepare_constructor_text(result, false);
   if (prepared.has_value()) return *prepared;
   return std::string();
}

//********************************************************************************************************************
// Resolves the lexical name of a constructor by evaluating the optional expression or using the literal metadata
// captured by the parser.  The resulting string retains the raw QName form so later stages can validate namespace
// bindings.

std::optional<std::string> XPathEvaluator::evaluate_constructor_name_string(const XPathNode *Node, uint32_t CurrentPrefix)
{
   pf::Log log("XPath");

   if (not Node) return std::string();

   size_t previous_constructed = constructed_nodes.size();
   auto saved_id = next_constructed_node_id;
   XPathVal value = evaluate_expression(Node, CurrentPrefix);
   if (expression_unsupported) {
      if (is_trace_enabled()) {
         std::string signature = build_ast_signature(Node);
         log.msg(VLF::TRACE, "Constructor name expression failed: %s", signature.c_str());
      }
      record_error("Constructor name expression could not be evaluated.", Node);
      if (xml and xml->ErrorMsg.empty()) xml->ErrorMsg.assign("Constructor name expression could not be evaluated.");
      constructed_nodes.resize(previous_constructed);
      next_constructed_node_id = saved_id;
      return std::nullopt;
   }

   std::string raw = value.to_string();
   constructed_nodes.resize(previous_constructed);
   next_constructed_node_id = saved_id;
   return trim_constructor_whitespace(raw);
}

//********************************************************************************************************************
// Builds an XTag representing a direct element constructor.  The function walks the parsed constructor metadata,
// creates namespace scopes, instantiates attributes, and recursively processes nested constructors and enclosed
// expressions.

std::optional<XTag> XPathEvaluator::build_direct_element_node(const XPathNode *Node, uint32_t CurrentPrefix,
   ConstructorNamespaceScope *ParentScope, int ParentID)
{
   pf::Log log("XPath");

   if ((not Node) or (Node->type != XQueryNodeType::DIRECT_ELEMENT_CONSTRUCTOR)) {
      record_error("Invalid direct constructor node encountered.", Node, true);
      return std::nullopt;
   }

   const auto *info = Node->get_constructor_info();
   if (not info) {
      record_error("Direct constructor is missing structural metadata.", Node, true);
      return std::nullopt;
   }

   ConstructorNamespaceScope element_scope;
   element_scope.parent = ParentScope;
   if (ParentScope and ParentScope->default_namespace.has_value()) {
      element_scope.default_namespace = ParentScope->default_namespace;
   }

   struct EvaluatedAttribute {
      const XPathConstructorAttribute *definition = nullptr;
      std::string value;
   };

   std::vector<EvaluatedAttribute> evaluated_attributes;
   evaluated_attributes.reserve(info->attributes.size());

   for (const auto &attribute : info->attributes) {
      auto value = evaluate_attribute_value_template(attribute, CurrentPrefix);
      if (not value) return std::nullopt;

      EvaluatedAttribute evaluated;
      evaluated.definition = &attribute;
      evaluated.value = std::move(*value);
      evaluated_attributes.push_back(std::move(evaluated));
   }

   pf::vector<XMLAttrib> element_attributes;

   std::string element_name;
   if (info->prefix.empty()) element_name = info->name;
   else {
      element_name = info->prefix;
      element_name += ':';
      element_name += info->name;
   }

   element_attributes.emplace_back(element_name, "");

   for (const auto &entry : evaluated_attributes) {
      const auto *attribute = entry.definition;
      const std::string &value = entry.value;

      if (not attribute->is_namespace_declaration) continue;

      if (attribute->prefix.empty() and (attribute->name IS "xmlns")) {
         if (value.empty()) element_scope.default_namespace = uint32_t{0};
         else element_scope.default_namespace = register_constructor_namespace(value);
      }
      else if (attribute->prefix IS "xmlns") {
         if (attribute->name IS "xml") {
            record_error("Cannot redeclare the xml prefix in constructor scope.", Node, true);
            return std::nullopt;
         }

         if (value.empty()) {
            record_error("Namespace prefix declarations require a non-empty URI.", Node, true);
            return std::nullopt;
         }

         element_scope.prefix_bindings[attribute->name] = register_constructor_namespace(value);
      }

      std::string attribute_name;
      if (attribute->prefix.empty()) attribute_name = attribute->name;
      else {
         attribute_name = attribute->prefix;
         attribute_name += ':';
         attribute_name += attribute->name;
      }

      element_attributes.emplace_back(attribute_name, value);
   }

   for (const auto &entry : evaluated_attributes) {
      const auto *attribute = entry.definition;
      const std::string &value = entry.value;

      if (attribute->is_namespace_declaration) continue;

      if (not attribute->prefix.empty()) {
         auto resolved = resolve_constructor_prefix(element_scope, attribute->prefix);
         if (not resolved.has_value())
         {
            record_error("XQDY0064: Attribute prefix is not bound in constructor scope.", Node, true);
            return std::nullopt;
         }
      }

      std::string attribute_name;
      if (attribute->prefix.empty()) attribute_name = attribute->name;
      else {
         attribute_name = attribute->prefix;
         attribute_name += ':';
         attribute_name += attribute->name;
      }

      log.trace("Adding attribute '%s' with value '%s'", attribute_name.c_str(), value.c_str());
      element_attributes.emplace_back(attribute_name, value);
   }

   uint32_t namespace_id = 0;
   if (not info->namespace_uri.empty()) namespace_id = register_constructor_namespace(info->namespace_uri);
   else if (not info->prefix.empty()) {
      auto resolved = resolve_constructor_prefix(element_scope, info->prefix);
      if (not resolved.has_value()) {
         record_error("XQDY0064: Element prefix is not declared within constructor scope.", Node, true);
         return std::nullopt;
      }
      namespace_id = *resolved;
   }
   else if (element_scope.default_namespace.has_value()) namespace_id = *element_scope.default_namespace;

   XTag element(next_constructed_node_id--, 0);
   element.ParentID = ParentID;
   element.Flags = XTF::NIL;
   element.NamespaceID = namespace_id;
   element.Attribs = element_attributes;

   element.Children.reserve(Node->child_count());
   bool preserve_construction = prolog_construction_preserve();

   for (const auto &child_ptr : Node->children)
   {
      const XPathNode *child = child_ptr.get();
      if (not child) continue;

      if (child->type IS XQueryNodeType::DIRECT_ELEMENT_CONSTRUCTOR)
      {
         auto nested = build_direct_element_node(child, CurrentPrefix, &element_scope, element.ID);
         if (not nested) return std::nullopt;
         element.Children.push_back(std::move(*nested));
         continue;
      }

      if (child->type IS XQueryNodeType::CONSTRUCTOR_CONTENT) {
         if (not child->value.empty()) {
            auto text_value = prepare_constructor_text(child->value, true);
            if (not text_value.has_value()) continue;

            pf::vector<XMLAttrib> text_attribs;
            text_attribs.emplace_back("", *text_value);
            XTag text_node(next_constructed_node_id--, 0, text_attribs);
            text_node.ParentID = element.ID;
            element.Children.push_back(std::move(text_node));
            continue;
         }

         const XPathNode *expr = child->get_child_safe(0);
         if (not expr) continue;

         size_t previous_constructed = constructed_nodes.size();
         auto saved_id = next_constructed_node_id;
         XPathVal value = evaluate_expression(expr, CurrentPrefix);
         if (expression_unsupported) return std::nullopt;
         if (not append_constructor_sequence(element, value, CurrentPrefix, element_scope, preserve_construction))
            return std::nullopt;
         constructed_nodes.resize(previous_constructed);
         next_constructed_node_id = saved_id;
         continue;
      }

      record_error("Unsupported node encountered within direct constructor content.", child, true);
      return std::nullopt;
   }

   return element;
}

//********************************************************************************************************************
// Entry point used by the evaluator to execute direct element constructors in the expression tree.  The resulting
// element is appended to the constructed node list and wrapped in an XPathVal for downstream consumers.

XPathVal XPathEvaluator::evaluate_direct_element_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   auto element = build_direct_element_node(Node, CurrentPrefix, nullptr, 0);
   if (not element) {
      if (xml and xml->ErrorMsg.empty()) record_error("Direct element constructor could not be evaluated.", Node, true);
      return XPathVal();
   }

   auto stored = std::make_unique<XTag>(*element);
   XTag *root = stored.get();
   constructed_nodes.push_back(std::move(stored));

   NODES nodes;
   nodes.push_back(root);

   std::vector<std::string> string_values;
   std::string node_string = XPathVal::node_string_value(root);
   string_values.push_back(node_string);

   return XPathVal(nodes, node_string, std::move(string_values));
}

//********************************************************************************************************************
// Handles computed element constructors where the element name or namespace is driven by runtime expressions.  The
// method prepares the namespace scope and evaluates the content sequence before emitting the constructed element.

XPathVal XPathEvaluator::evaluate_computed_element_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if ((not Node) or (Node->type != XQueryNodeType::COMPUTED_ELEMENT_CONSTRUCTOR)) {
      record_error("Invalid computed element constructor node encountered.", Node, true);
      return XPathVal();
   }

   const auto *constructor_info = Node->get_constructor_info();
   if (not constructor_info) {
      record_error("Computed element constructor is missing metadata.", Node, true);
      return XPathVal();
   }

   ConstructorQName name_info;

   if (Node->has_name_expression()) {
      auto name_string = evaluate_constructor_name_string(Node->get_name_expression(), CurrentPrefix);
      if (not name_string) return XPathVal();

      auto parsed = parse_constructor_qname_string(*name_string);
      if (not parsed.valid) {
         record_error("Computed element name must resolve to a QName.", Node, true);
         return XPathVal();
      }

      name_info = std::move(parsed);
   }
   else {
      name_info.valid = true;
      name_info.prefix = constructor_info->prefix;
      name_info.local = constructor_info->name;
      name_info.namespace_uri = constructor_info->namespace_uri;
   }

   if (name_info.local.empty()) {
      record_error("Computed element constructor requires a local name.", Node, true);
      return XPathVal();
   }

   auto resolve_prefix_in_context = [&](std::string_view Prefix) -> std::optional<uint32_t> {
      if (Prefix.empty()) return uint32_t{0};
      if (not xml) [[unlikely]] return std::nullopt;
      if (Prefix.compare("xml") IS 0) return register_constructor_namespace("http://www.w3.org/XML/1998/namespace");
      if (not context.context_node) [[unlikely]] return std::nullopt;

      uint32_t resolved_hash = 0;
      if (xml->resolvePrefix(Prefix, context.context_node->ID, resolved_hash) IS ERR::Okay) return resolved_hash;
      return std::nullopt;
   };

   uint32_t namespace_id = 0;
   if (not name_info.namespace_uri.empty()) namespace_id = register_constructor_namespace(name_info.namespace_uri);
   else if (not name_info.prefix.empty()) {
      auto resolved = resolve_prefix_in_context(name_info.prefix);
      if (not resolved.has_value()) {
         record_error("XQDY0064: Element prefix is not bound in scope.", Node, true);
         return XPathVal();
      }
      namespace_id = *resolved;
   }

   std::string element_name;
   if (name_info.prefix.empty()) element_name = name_info.local;
   else {
      element_name = name_info.prefix;
      element_name += ':';
      element_name += name_info.local;
   }

   pf::vector<XMLAttrib> element_attributes;
   element_attributes.emplace_back(element_name, "");

   XTag element(next_constructed_node_id--, 0, element_attributes);
   element.ParentID = 0;
   element.Flags = XTF::NIL;
   element.NamespaceID = namespace_id;

   ConstructorNamespaceScope scope;
   scope.parent = nullptr;
   bool preserve_construction = prolog_construction_preserve();

   if (const auto *content_node = Node->get_child_safe(0)) {
      if (not content_node->value.empty()) {
         auto text_value = prepare_constructor_text(content_node->value, true);
         if (text_value.has_value()) {
            pf::vector<XMLAttrib> text_attribs;
            text_attribs.emplace_back("", *text_value);
            XTag text_node(next_constructed_node_id--, 0, text_attribs);
            text_node.ParentID = element.ID;
            element.Children.push_back(std::move(text_node));
         }
      }
      else if (const auto *expr = content_node->get_child_safe(0)) {
         size_t previous_constructed = constructed_nodes.size();
         auto saved_id = next_constructed_node_id;
         XPathVal value = evaluate_expression(expr, CurrentPrefix);
         if (expression_unsupported) return XPathVal();
         if (not append_constructor_sequence(element, value, CurrentPrefix, scope, preserve_construction))
            return XPathVal();
         constructed_nodes.resize(previous_constructed);
         next_constructed_node_id = saved_id;
      }
   }

   auto stored = std::make_unique<XTag>(std::move(element));
   XTag *root = stored.get();
   constructed_nodes.push_back(std::move(stored));

   NODES nodes;
   nodes.push_back(root);

   std::vector<std::string> string_values;
   std::string node_string = XPathVal::node_string_value(root);
   string_values.push_back(node_string);

   return XPathVal(nodes, node_string, std::move(string_values));
}

//********************************************************************************************************************
// Implements computed attribute constructors, resolving the attribute name at runtime and constructing a single
// attribute node according to the XPath specification.

XPathVal XPathEvaluator::evaluate_computed_attribute_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if ((not Node) or (Node->type != XQueryNodeType::COMPUTED_ATTRIBUTE_CONSTRUCTOR)) {
      record_error("Invalid computed attribute constructor node encountered.", Node, true);
      return XPathVal();
   }

   const auto *constructor_info = Node->get_constructor_info();
   if (not constructor_info) {
      record_error("Computed attribute constructor is missing metadata.", Node, true);
      return XPathVal();
   }

   ConstructorQName name_info;

   if (Node->has_name_expression()) {
      auto name_string = evaluate_constructor_name_string(Node->get_name_expression(), CurrentPrefix);
      if (not name_string) return XPathVal();

      auto parsed = parse_constructor_qname_string(*name_string);
      if (not parsed.valid) {
         record_error("Computed attribute name must resolve to a QName.", Node, true);
         return XPathVal();
      }

      if (not parsed.prefix.empty()) name_info.prefix = parsed.prefix;
      name_info.local = parsed.local;
      name_info.namespace_uri = parsed.namespace_uri;
      name_info.valid = true;
   }
   else {
      name_info.valid = true;
      name_info.prefix = constructor_info->prefix;
      name_info.local = constructor_info->name;
      name_info.namespace_uri = constructor_info->namespace_uri;
   }

   if (name_info.local.empty()) {
      record_error("Computed attribute constructor requires a local name.", Node, true);
      return XPathVal();
   }

   auto resolve_prefix_in_context = [&](std::string_view Prefix) -> std::optional<uint32_t> {
      if (Prefix.empty()) return uint32_t{0};
      if (not xml) [[unlikely]] return std::nullopt;
      if (Prefix.compare("xml") IS 0) return register_constructor_namespace("http://www.w3.org/XML/1998/namespace");
      if (not context.context_node) [[unlikely]] return std::nullopt;

      uint32_t resolved_hash = 0;
      if (xml->resolvePrefix(Prefix, context.context_node->ID, resolved_hash) IS ERR::Okay) return resolved_hash;
      return std::nullopt;
   };

   uint32_t namespace_id = 0;
   if (not name_info.namespace_uri.empty()) namespace_id = register_constructor_namespace(name_info.namespace_uri);
   else if (not name_info.prefix.empty()) {
      auto resolved = resolve_prefix_in_context(name_info.prefix);
      if (not resolved.has_value()) {
         record_error("XQDY0064: Attribute prefix is not bound in scope.", Node, true);
         return XPathVal();
      }
      namespace_id = *resolved;
   }

   std::string attribute_name;
   if (name_info.prefix.empty()) attribute_name = name_info.local;
   else {
      attribute_name = name_info.prefix;
      attribute_name += ':';
      attribute_name += name_info.local;
   }

   const XPathNode *content_node = Node->get_child_safe(0);
   auto value_string = evaluate_constructor_content_string(content_node, CurrentPrefix, false, false);
   if (not value_string) return XPathVal();

   pf::vector<XMLAttrib> attribute_attribs;
   attribute_attribs.emplace_back("$attribute", "");
   attribute_attribs.emplace_back(attribute_name, *value_string);

   XTag attribute_tag(next_constructed_node_id--, 0, attribute_attribs);
   attribute_tag.ParentID = 0;
   attribute_tag.Flags = XTF::NIL;
   attribute_tag.NamespaceID = namespace_id;

   auto stored = std::make_unique<XTag>(std::move(attribute_tag));
   XTag *owner = stored.get();
   const XMLAttrib *attribute_ptr = owner->Attribs.size() > 1 ? &owner->Attribs[1] : nullptr;
   constructed_nodes.push_back(std::move(stored));

   NODES nodes;
   nodes.push_back(owner);

   std::vector<const XMLAttrib *> attributes;
   attributes.push_back(attribute_ptr);

   return XPathVal(nodes, std::nullopt, {}, std::move(attributes));
}

//********************************************************************************************************************
// Evaluates text constructors by flattening the enclosed expression into a string and returning it as a text node
// for inclusion in the result sequence.

XPathVal XPathEvaluator::evaluate_text_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if ((not Node) or (Node->type != XQueryNodeType::TEXT_CONSTRUCTOR)) {
      record_error("Invalid text constructor node encountered.", Node, true);
      return XPathVal();
   }

   const XPathNode *content_node = Node->get_child_safe(0);
   bool preserve_construction = prolog_construction_preserve();
   auto content = evaluate_constructor_content_string(content_node, CurrentPrefix, true, preserve_construction);
   if (not content) return XPathVal();

   pf::vector<XMLAttrib> text_attribs;
   text_attribs.emplace_back("", *content);

   XTag text_node(next_constructed_node_id--, 0, text_attribs);
   text_node.ParentID = 0;
   text_node.Flags = XTF::NIL;
   text_node.NamespaceID = 0;

   auto stored = std::make_unique<XTag>(std::move(text_node));
   XTag *root = stored.get();
   constructed_nodes.push_back(std::move(stored));

   NODES nodes;
   nodes.push_back(root);

   std::vector<std::string> string_values;
   string_values.push_back(*content);

   return XPathVal(nodes, *content, std::move(string_values));
}

//********************************************************************************************************************
// Evaluates comment constructors by producing the textual content and wrapping it in a
// comment node for downstream processing.

XPathVal XPathEvaluator::evaluate_comment_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if ((not Node) or (Node->type != XQueryNodeType::COMMENT_CONSTRUCTOR)) {
      record_error("Invalid comment constructor node encountered.", Node, true);
      return XPathVal();
   }

   const XPathNode *content_node = Node->get_child_safe(0);
   auto content = evaluate_constructor_content_string(content_node, CurrentPrefix, false, false);
   if (not content) return XPathVal();

   auto double_dash = content->find("--");
   if (not (double_dash IS std::string::npos)) {
      record_error("XQDY0072: Comments cannot contain consecutive hyphen characters.", Node, true);
      return XPathVal();
   }

   if (not content->empty() and (content->back() IS '-')) {
      record_error("XQDY0072: Comments cannot end with a hyphen.", Node, true);
      return XPathVal();
   }

   pf::vector<XMLAttrib> comment_attribs;
   comment_attribs.emplace_back("", *content);

   XTag comment_node(next_constructed_node_id--, 0, comment_attribs);
   comment_node.ParentID = 0;
   comment_node.Flags = XTF::COMMENT;
   comment_node.NamespaceID = 0;

   auto stored = std::make_unique<XTag>(std::move(comment_node));
   XTag *root = stored.get();
   constructed_nodes.push_back(std::move(stored));

   NODES nodes;
   nodes.push_back(root);

   std::vector<std::string> string_values;
   string_values.push_back(*content);

   return XPathVal(nodes, *content, std::move(string_values));
}

//********************************************************************************************************************
// Executes processing-instruction constructors, resolving the target name and content while enforcing NCName rules
// defined by XPath.

XPathVal XPathEvaluator::evaluate_pi_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if ((not Node) or (Node->type != XQueryNodeType::PI_CONSTRUCTOR)) {
      record_error("Invalid processing-instruction constructor encountered.", Node, true);
      return XPathVal();
   }

   std::string target;

   if (Node->has_name_expression()) {
      auto target_string = evaluate_constructor_name_string(Node->get_name_expression(), CurrentPrefix);
      if (not target_string) return XPathVal();
      target = *target_string;
   }
   else if (const auto *info = Node->get_constructor_info()) target = info->name;

   target = trim_constructor_whitespace(target);

   if (target.empty()) {
      record_error("Processing-instruction constructor requires a target name.", Node, true);
      return XPathVal();
   }

   if (not is_valid_ncname(target)) {
      record_error("Processing-instruction target must be an NCName.", Node, true);
      return XPathVal();
   }

   const XPathNode *content_node = Node->get_child_safe(0);
   auto content = evaluate_constructor_content_string(content_node, CurrentPrefix, false, false);
   if (not content) return XPathVal();

   auto terminator = content->find("?>");
   if (not (terminator IS std::string::npos)) {
      record_error("XQDY0026: Processing-instruction content cannot contain '?>'.", Node, true);
      return XPathVal();
   }

   std::string attribute_name = "?" + target;

   pf::vector<XMLAttrib> instruction_attribs;
   instruction_attribs.emplace_back(attribute_name, *content);

   XTag instruction(next_constructed_node_id--, 0, instruction_attribs);
   instruction.ParentID = 0;
   instruction.Flags = XTF::INSTRUCTION;
   instruction.NamespaceID = 0;

   auto stored = std::make_unique<XTag>(std::move(instruction));
   XTag *root = stored.get();
   constructed_nodes.push_back(std::move(stored));

   NODES nodes;
   nodes.push_back(root);

   std::vector<std::string> string_values;
   string_values.push_back(*content);

   return XPathVal(nodes, *content, std::move(string_values));
}

//********************************************************************************************************************
// Produces document nodes by evaluating the enclosed content, constructing a temporary
// root scope, and appending the resulting children to a synthetic document element.

XPathVal XPathEvaluator::evaluate_document_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if ((not Node) or (Node->type != XQueryNodeType::DOCUMENT_CONSTRUCTOR)) {
      record_error("Invalid document constructor node encountered.", Node, true);
      return XPathVal();
   }

   pf::vector<XMLAttrib> document_attribs;
   document_attribs.emplace_back("#document", "");

   XTag document_node(next_constructed_node_id--, 0, document_attribs);
   document_node.ParentID = 0;
   document_node.Flags = XTF::NIL;
   document_node.NamespaceID = 0;

   ConstructorNamespaceScope scope;
   scope.parent = nullptr;
   bool preserve_construction = prolog_construction_preserve();

   if (const auto *content_node = Node->get_child_safe(0)) {
      if (not content_node->value.empty()) {
         auto text_value = prepare_constructor_text(content_node->value, true);
         if (text_value.has_value()) {
            pf::vector<XMLAttrib> text_attribs;
            text_attribs.emplace_back("", *text_value);
            XTag text_node(next_constructed_node_id--, 0, text_attribs);
            text_node.ParentID = document_node.ID;
            document_node.Children.push_back(std::move(text_node));
         }
      }
      else if (const auto *expr = content_node->get_child_safe(0)) {
         size_t previous_constructed = constructed_nodes.size();
         auto saved_id = next_constructed_node_id;
         XPathVal value = evaluate_expression(expr, CurrentPrefix);
         if (expression_unsupported) return XPathVal();
         if (not append_constructor_sequence(document_node, value, CurrentPrefix, scope, preserve_construction))
            return XPathVal();
         constructed_nodes.resize(previous_constructed);
         next_constructed_node_id = saved_id;
      }
   }

   auto stored = std::make_unique<XTag>(std::move(document_node));
   XTag *root = stored.get();
   constructed_nodes.push_back(std::move(stored));

   NODES nodes;
   nodes.push_back(root);

   std::vector<std::string> string_values;
   std::string node_string = XPathVal::node_string_value(root);
   string_values.push_back(node_string);

   return XPathVal(nodes, node_string, std::move(string_values));
}

//********************************************************************************************************************

ERR XPathEvaluator::process_expression_node_set(const XPathVal &Value)
{
   bool tracing_xpath = is_trace_enabled();
   auto trace_nodes_detail = [&](const char *Format, auto ...Args) {
      if (not tracing_xpath) return;
      pf::Log("XPath").msg(VLF::TRACE, Format, Args...);
   };

   auto trace_nodes_verbose = [&](const char *Format, auto ...Args) {
      if (not tracing_xpath) return;
      pf::Log("XPath").msg(VLF::TRACE, Format, Args...);
   };

   struct NodeEntry {
      XTag * node = nullptr;
      const XMLAttrib * attribute = nullptr;
      size_t original_index = 0;
   };

   std::vector<NodeEntry> entries;
   entries.reserve(Value.node_set.size());

   for (size_t index = 0; index < Value.node_set.size(); ++index) {
      XTag *candidate = Value.node_set[index];
      if (not candidate) continue;

      const XMLAttrib *attribute = nullptr;
      if (index < Value.node_set_attributes.size()) attribute = Value.node_set_attributes[index];

      entries.push_back({ candidate, attribute, index });
   }

   if (tracing_xpath) {
      auto build_index_summary = [](const auto &entries_list) {
         std::string result;
         for (size_t i = 0; i < entries_list.size(); ++i) {
            result += std::format("{}{}", i > 0 ? ", " : "", entries_list[i].original_index);
         }
         return result;
      };

      std::string original_summary = build_index_summary(entries);
      trace_nodes_detail("FLWOR emit initial tuple materialisation: nodes=%zu, attributes=%zu, order=[%s]",
         entries.size(), Value.node_set_attributes.size(), original_summary.c_str());

      for (size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
         const auto &entry = entries[entry_index];
         int node_id = entry.node ? entry.node->ID : -1;
         const char *attribute_name = (entry.attribute and !entry.attribute->Name.empty()) ?
            entry.attribute->Name.c_str() : "<node>";
         trace_nodes_verbose("FLWOR emit initial entry[%zu]: node-id=%d, attribute=%s, original=%zu",
            entry_index, node_id, attribute_name, entry.original_index);
      }
   }

   if (entries.empty()) return ERR::Search;

   bool preserve_order = Value.preserve_node_order or (not prolog_ordering_is_ordered());

   if (preserve_order) {
      std::vector<NodeEntry> unique_entries;
      unique_entries.reserve(entries.size());

      for (const auto &entry : entries) {
         bool duplicate = false;
         for (const auto &existing : unique_entries) {
            if ((existing.node IS entry.node) and (existing.attribute IS entry.attribute)) {
               duplicate = true;
               break;
            }
         }
         if (not duplicate) unique_entries.push_back(entry);
      }

      if (tracing_xpath) {
         auto build_index_summary = [](const auto &entries_list) {
            std::string result;
            for (size_t i = 0; i < entries_list.size(); ++i) {
               result += std::format("{}{}", i > 0 ? ", " : "", entries_list[i].original_index);
            }
            return result;
         };

         std::string preserved_summary = build_index_summary(unique_entries);
         trace_nodes_detail("FLWOR emit preserved-order pass: unique=%zu, order=[%s]", unique_entries.size(),
            preserved_summary.c_str());
      }

      entries.swap(unique_entries);
   }
   else {
      std::stable_sort(entries.begin(), entries.end(), [this](const NodeEntry &Left, const NodeEntry &Right) {
         if (Left.node IS Right.node) return Left.original_index < Right.original_index;
         if (not Left.node) return false;
         if (not Right.node) return true;
         return axis_evaluator.is_before_in_document_order(Left.node, Right.node);
      });

      auto unique_end = std::unique(entries.begin(), entries.end(), [](const NodeEntry &Left, const NodeEntry &Right) {
         return (Left.node IS Right.node) and (Left.attribute IS Right.attribute);
      });
      entries.erase(unique_end, entries.end());

      if (tracing_xpath) {
         auto build_index_summary = [](const auto &entries_list) {
            std::string result;
            for (size_t i = 0; i < entries_list.size(); ++i) {
               result += std::format("{}{}", i > 0 ? ", " : "", entries_list[i].original_index);
            }
            return result;
         };

         std::string sorted_summary = build_index_summary(entries);
         trace_nodes_detail("FLWOR emit document-order pass: unique=%zu, order=[%s]", entries.size(),
            sorted_summary.c_str());
      }
   }

   bool matched = false;

   for (size_t index = 0; index < entries.size(); ++index) {
      auto &entry = entries[index];
      XTag *candidate = entry.node;
      push_context(candidate, index + 1, entries.size(), entry.attribute);

      if (not candidate) {
         pop_context();
         continue;
      }

      bool should_terminate = false;
      if (tracing_xpath) {
         int node_id = candidate ? candidate->ID : -1;
         const char *attribute_name = (entry.attribute and !entry.attribute->Name.empty()) ?
            entry.attribute->Name.c_str() : "<node>";
         trace_nodes_detail("FLWOR emit invoking callback index=%zu node-id=%d attribute=%s original=%zu",
            index, node_id, attribute_name, entry.original_index);
      }
      auto callback_error = invoke_callback(candidate, entry.attribute, matched, should_terminate);
      pop_context();

      if (callback_error IS ERR::Terminate) return ERR::Terminate;
      if (callback_error != ERR::Okay) return callback_error;
      if (should_terminate) return ERR::Okay;
   }

   if (matched) return ERR::Okay;
   return ERR::Search;
}

//********************************************************************************************************************

ERR XPathEvaluator::evaluate_top_level_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (not Node) return ERR::Failed;

   const XPathNode *expression = Node;

   if (Node->type IS XQueryNodeType::EXPRESSION) {
      if (Node->child_count() IS 0) return ERR::Search;
      expression = Node->get_child(0);
   }

   expression_unsupported = false;
   auto value = evaluate_expression(expression, CurrentPrefix);
   if (expression_unsupported) {
      if (xml and xml->ErrorMsg.empty()) xml->ErrorMsg = "Unsupported XPath expression.";
      return ERR::Failed;
   }

   switch (value.Type) {
      case XPVT::NodeSet:
         return process_expression_node_set(value);

      case XPVT::Boolean:
      case XPVT::Number:
      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime:
      case XPVT::Map:
      case XPVT::Array:
         return ERR::Okay;
   }

   return ERR::Failed;
}

//********************************************************************************************************************

XPathVal XPathEvaluator::evaluate_function_call(const XPathNode *FuncNode, uint32_t CurrentPrefix) {
   if (not FuncNode or FuncNode->type != XQueryNodeType::FUNCTION_CALL) {
      return XPathVal();
   }

   std::string function_name = FuncNode->value;

   std::vector<XPathVal> args;
   args.reserve(FuncNode->child_count());

   for (size_t index = 0; index < FuncNode->child_count(); ++index) {
      auto *argument_node = FuncNode->get_child(index);
      args.push_back(evaluate_expression(argument_node, CurrentPrefix));
      if (expression_unsupported) return XPathVal();
   }

   // Canonicalise function QName using prolog/document mappings so built-ins and module functions resolve
   if (auto prolog = context.prolog) {
      std::string canonical = prolog->normalise_function_qname(function_name, nullptr);
      if (canonical != function_name) function_name = std::move(canonical);
   }

   std::string builtin_lookup_name = function_name;
   std::string builtin_namespace;
   std::string builtin_local;
   bool builtin_has_expanded = false;

   if ((function_name.size() > 2) and (function_name[0] IS 'Q') and (function_name[1] IS '{')) {
      size_t closing = function_name.find('}');
      if (closing != std::string::npos) {
         builtin_namespace = function_name.substr(2, closing - 2);
         builtin_local = function_name.substr(closing + 1);
         builtin_has_expanded = true;
      }
   }

   if (function_name IS "text") {
      NODES text_nodes;
      std::optional<std::string> first_value;

      if (context.context_node) {
         for (auto &child : context.context_node->Children) {
            if (not child.isContent()) continue;
            text_nodes.push_back(&child);

            if ((not first_value.has_value()) and (not child.Attribs.empty())) {
               first_value = child.Attribs[0].Value;
            }
         }
      }

      return XPathVal(text_nodes, first_value);
   }

   auto constructor_lookup = resolve_constructor_descriptor(function_name, context);

   if (constructor_lookup.status IS ConstructorLookupStatus::Resolved) {
      return evaluate_type_constructor(constructor_lookup.descriptor, args, FuncNode);
   }

   if (constructor_lookup.status IS ConstructorLookupStatus::UnknownNamespace) {
      auto message = std::format("XPST0081: QName '{}' references a namespace prefix that is not in scope.",
         constructor_lookup.display_name);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   if (constructor_lookup.status IS ConstructorLookupStatus::UnknownType) {
      auto message = std::format("XPST0051: Constructor type '{}' is not defined in the in-scope schema types.",
         constructor_lookup.display_name);
      record_error(message, FuncNode, true);
      return XPathVal();
   }

   if (auto user_result = resolve_user_defined_function(function_name, args, CurrentPrefix, FuncNode)) {
      return *user_result;
   }

   auto &library = XPathFunctionLibrary::instance();

   if (builtin_has_expanded) {
      static const std::string builtin_namespace_uri("http://www.w3.org/2005/xpath-functions");
      static const std::string math_namespace_uri("http://www.w3.org/2005/xpath-functions/math");
      static const std::string map_namespace_uri("http://www.w3.org/2005/xpath-functions/map");
      static const std::string array_namespace_uri("http://www.w3.org/2005/xpath-functions/array");
      if (builtin_namespace IS builtin_namespace_uri) {
         builtin_lookup_name = builtin_local;
      }
      else if (builtin_namespace IS math_namespace_uri) {
         std::string prefixed_name = std::string("math:") + builtin_local;
         if (library.has_function(prefixed_name)) builtin_lookup_name = std::move(prefixed_name);
         else if (library.has_function(function_name)) builtin_lookup_name = function_name;
      }
      else if (builtin_namespace IS map_namespace_uri) {
         builtin_lookup_name = std::string("map:") + builtin_local;
      }
      else if (builtin_namespace IS array_namespace_uri) {
         builtin_lookup_name = std::string("array:") + builtin_local;
      }
      else {
         if (library.has_function(function_name)) builtin_lookup_name = function_name;
         else if ((not builtin_local.empty()) and library.has_function(builtin_local)) {
            builtin_lookup_name = builtin_local;
         }
      }
   }

   return library.call_function(builtin_lookup_name, args, context);
}
