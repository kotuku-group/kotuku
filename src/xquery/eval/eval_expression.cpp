
#include <cassert>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include "eval_detail.h"
#include "date_time_utils.h"
#include "../api/xquery_errors.h"
#include "../../xml/schema/schema_types.h"
#include "../../xml/schema/type_checker.h"
#include "checked_arith.h"

//********************************************************************************************************************
// Helpers for parsing sequence type literals that include nested parentheses and comma-separated arguments.

static bool starts_with_keyword(std::string_view Text, std::string_view Keyword)
{
   if (Text.size() < Keyword.size()) return false;
   if (Text.substr(0, Keyword.size()) != Keyword) return false;

   if (Text.size() IS Keyword.size()) return true;
   char next = Text[Keyword.size()];
   if (next IS '(') return true;
   return is_space_character(next);
}

static std::optional<std::pair<std::string_view, size_t>> extract_parenthesized_segment(
   std::string_view Text, size_t StartIndex)
{
   size_t index = StartIndex;
   while ((index < Text.size()) and is_space_character(Text[index])) index++;
   if ((index >= Text.size()) or (Text[index] != '(')) return std::nullopt;

   size_t begin = index + 1;
   int depth = 1;
   for (size_t cursor = begin; cursor < Text.size(); ++cursor) {
      char ch = Text[cursor];
      if (ch IS '(') depth++;
      else if (ch IS ')') {
         depth--;
         if (depth IS 0) {
            std::string_view inner = Text.substr(begin, cursor - begin);
            return std::make_pair(inner, cursor + 1);
         }
      }
   }

   return std::nullopt;
}

static std::optional<size_t> find_top_level_comma(std::string_view Text)
{
   int depth = 0;
   for (size_t index = 0; index < Text.size(); ++index) {
      char ch = Text[index];
      if (ch IS '(') depth++;
      else if (ch IS ')') {
         if (depth > 0) depth--;
      }
      else if ((ch IS ',') and (depth IS 0)) {
         return index;
      }
   }
   return std::nullopt;
}

//********************************************************************************************************************
// Parses a cast target type specification, extracting the type name and optional empty sequence indicator.

static CastTargetInfo parse_cast_target_literal(std::string_view Literal)
{
   CastTargetInfo info;

   size_t start = 0;
   while ((start < Literal.size()) and is_space_character(Literal[start])) start++;
   if (start >= Literal.size()) return info;

   size_t end = Literal.size();
   while (end > start and is_space_character(Literal[end - 1])) end--;

   std::string_view trimmed = Literal.substr(start, end - start);

   if (trimmed.ends_with('?')) {
      info.allows_empty = true;
      trimmed.remove_suffix(1);
      while ((!trimmed.empty()) and is_space_character(trimmed.back())) trimmed.remove_suffix(1);
   }

   info.type_name.assign(trimmed);
   return info;
}

//********************************************************************************************************************
// Parses a sequence type literal string, extracting cardinality markers and the item kind or atomic type.

static std::optional<SequenceTypeInfo> parse_sequence_type_literal(std::string_view Literal)
{
   SequenceTypeInfo info;
   auto trimmed = trim_view(Literal);
   if (trimmed.empty()) return std::nullopt;

   size_t end = trimmed.size();
   char occurrence_marker = '\0';

   if (end > 0) {
      char marker = trimmed[end - 1];
      if ((marker IS '?') or (marker IS '+') or (marker IS '*')) {
         occurrence_marker = marker;
         end--;
         while ((end > 0) and is_space_character(trimmed[end - 1])) end--;
      }
   }

   switch (occurrence_marker) {
      case '?': info.occurrence = SequenceCardinality::ZeroOrOne; break;
      case '+': info.occurrence = SequenceCardinality::OneOrMore; break;
      case '*': info.occurrence = SequenceCardinality::ZeroOrMore; break;
      default: break;
   }

   std::string_view core = trimmed.substr(0, end);
   core = trim_view(core);
   if (core.empty()) return std::nullopt;

   // Normalise away internal whitespace for node-test tokens like "element()" which may appear as "element ( )"
   std::string core_compact;
   core_compact.reserve(core.size());
   for (char ch : core) { if ((ch != ' ') and (ch != '\t') and (ch != '\r') and (ch != '\n')) core_compact.push_back(ch); }

   if (starts_with_keyword(core, "map")) {
      auto segment = extract_parenthesized_segment(core, 3);
      if (!segment.has_value()) return std::nullopt;
      auto inner = trim_view(segment->first);
      auto remainder = trim_view(core.substr(segment->second));
      if (!remainder.empty()) return std::nullopt;

      info.kind = SequenceItemKind::Map;
      if (inner IS "*") {
         info.map_allows_any = true;
         return info;
      }

      auto comma_position = find_top_level_comma(inner);
      if (!comma_position.has_value()) return std::nullopt;

      auto key_literal = trim_view(inner.substr(0, *comma_position));
      auto value_literal = trim_view(inner.substr(*comma_position + 1));
      if (key_literal.empty() or value_literal.empty()) return std::nullopt;

      info.map_key_type.assign(key_literal);
      auto value_info = parse_sequence_type_literal(value_literal);
      if (!value_info.has_value()) return std::nullopt;
      info.map_value_type = std::make_shared<SequenceTypeInfo>(*value_info);
      return info;
   }

   if (starts_with_keyword(core, "array")) {
      auto segment = extract_parenthesized_segment(core, 5);
      if (!segment.has_value()) return std::nullopt;
      auto inner = trim_view(segment->first);
      auto remainder = trim_view(core.substr(segment->second));
      if (!remainder.empty()) return std::nullopt;

      info.kind = SequenceItemKind::Array;
      if (inner IS "*") {
       info.array_allows_any = true;
       return info;
      }

      auto member_info = parse_sequence_type_literal(inner);
      if (!member_info.has_value()) return std::nullopt;
      info.array_member_type = std::make_shared<SequenceTypeInfo>(*member_info);
      return info;
   }

   if (starts_with_keyword(core, "function")) {
      auto segment = extract_parenthesized_segment(core, 8);
      if (!segment.has_value()) return std::nullopt;
      auto remainder = trim_view(core.substr(segment->second));
      if (!remainder.empty()) {
         if (!starts_with_keyword(remainder, "as")) return std::nullopt;
         auto return_literal = trim_view(remainder.substr(2));
         if (return_literal.empty()) return std::nullopt;
         auto return_info = parse_sequence_type_literal(return_literal);
         if (!return_info.has_value()) return std::nullopt;
      }

      info.kind = SequenceItemKind::Function;
      return info;
   }

   if (core_compact IS "item()") info.kind = SequenceItemKind::Item;
   else if (core_compact IS "node()") info.kind = SequenceItemKind::Node;
   else if (core_compact IS "element()") info.kind = SequenceItemKind::Element;
   else if (core_compact IS "attribute()") info.kind = SequenceItemKind::Attribute;
   else if (core_compact IS "text()") info.kind = SequenceItemKind::Text;
   else if (core_compact IS "empty-sequence()") info.kind = SequenceItemKind::EmptySequence;
   else {
      info.kind = SequenceItemKind::Atomic;
      info.type_name.assign(core);
   }

   return info;
}

//********************************************************************************************************************
// Computes the number of items in a sequence value, accounting for node-sets and scalar values.

static size_t sequence_item_count(const XPathVal &Value)
{
   if (Value.Type IS XPVT::NodeSet) {
      size_t length = Value.node_set.size();
      if (length < Value.node_set_attributes.size()) length = Value.node_set_attributes.size();
      if (length < Value.node_set_string_values.size()) length = Value.node_set_string_values.size();
      if ((length IS 0) and Value.node_set_string_override.has_value()) length = 1;
      return length;
   }

   return Value.is_empty() ? 0 : 1;
}

//********************************************************************************************************************
// Determines whether a value represents the literal empty sequence (()) rather than an atomic with empty content.

static bool value_is_empty_sequence(const XPathVal &Value)
{
   if (Value.Type != XPVT::NodeSet) return false;
   bool has_nodes = !Value.node_set.empty();
   bool has_attributes = !Value.node_set_attributes.empty();
   bool has_strings = !Value.node_set_string_values.empty();
   bool has_override = Value.node_set_string_override.has_value();
   bool has_composites = false;
   if (!Value.node_set_composite_values.empty()) {
      for (const auto &stored : Value.node_set_composite_values) {
         if (stored) {
            has_composites = true;
            break;
         }
      }
   }
   return (!has_nodes) and (!has_attributes) and (!has_strings) and (!has_override) and (!has_composites);
}

//********************************************************************************************************************
// Copies the storage payload from a public XPathValue into a runtime XPathVal instance.

static XPathVal clone_xpath_value(const XPathValue &Source)
{
   XPathVal clone;
   clone.Type = Source.Type;
   clone.NumberValue = Source.NumberValue;
   clone.StringValue = Source.StringValue;
   clone.node_set = Source.node_set;
   clone.node_set_string_override = Source.node_set_string_override;
   clone.node_set_string_values = Source.node_set_string_values;
   clone.node_set_attributes = Source.node_set_attributes;
    clone.node_set_composite_values = Source.node_set_composite_values;
   clone.preserve_node_order = Source.preserve_node_order;
   clone.map_storage = Source.map_storage;
   clone.array_storage = Source.array_storage;
   return clone;
}

//********************************************************************************************************************
// Stores a runtime value into a sequence container, preserving empty-sequence semantics.

static void store_value_in_sequence(const XPathVal &Value, XPathValueSequence &Sequence)
{
   Sequence.reset();
   if (value_is_empty_sequence(Value)) return;
   Sequence.items.push_back(Value);
}

//********************************************************************************************************************
// Renders a human-readable description of a runtime value type for diagnostics.

static std::string describe_value_type(const XPathVal &Value)
{
   switch (Value.Type) {
      case XPVT::Boolean: return std::string("boolean");
      case XPVT::Number: return std::string("numeric");
      case XPVT::String: return std::string("string");
      case XPVT::Date: return std::string("date");
      case XPVT::Time: return std::string("time");
      case XPVT::DateTime: return std::string("dateTime");
      case XPVT::NodeSet: return std::string("node sequence");
      case XPVT::Map: return std::string("map");
      case XPVT::Array: return std::string("array");
   }
   return std::string("value");
}

//********************************************************************************************************************
// Gathers composite items of a given type from a runtime value, supporting node-set backed sequences.

static bool gather_items_of_type(const XPathVal &Value, XPVT ExpectedType, std::vector<XPathVal> &Items)
{
   if (Value.Type IS ExpectedType) {
      Items.push_back(Value);
      return true;
   }

   if (Value.Type IS XPVT::NodeSet) {
      if (Value.node_set_composite_values.empty()) return false;
      bool saw_any = false;
      for (const auto &stored : Value.node_set_composite_values) {
         if (!stored) return false;
         XPathVal clone = clone_xpath_value(*stored);
         if (clone.Type != ExpectedType) return false;
         Items.push_back(std::move(clone));
         saw_any = true;
      }
      return saw_any;
   }

   return false;
}

static bool gather_function_items(const XPathVal &Value, std::vector<XPathVal> &Items)
{
   if ((Value.Type IS XPVT::Map) or (Value.Type IS XPVT::Array)) {
      Items.push_back(Value);
      return true;
   }

   if (Value.Type IS XPVT::NodeSet) {
      if (Value.node_set_composite_values.empty()) return false;
      bool saw_any = false;
      for (const auto &stored : Value.node_set_composite_values) {
         if (!stored) return false;
         XPathVal clone = clone_xpath_value(*stored);
         if ((clone.Type != XPVT::Map) and (clone.Type != XPVT::Array)) return false;
         Items.push_back(std::move(clone));
         saw_any = true;
      }
      return saw_any;
   }

   return false;
}

static std::optional<bool> map_sequence_matches_type(const XPathVal &Value, const SequenceTypeInfo &SequenceInfo,
   XPathEvaluator &Evaluator, const XPathNode *ContextNode)
{
   std::vector<XPathVal> maps;
   if (!gather_items_of_type(Value, XPVT::Map, maps)) return false;
   if (maps.empty()) return false;

   bool check_keys = !SequenceInfo.map_key_type.empty();
   bool check_values = SequenceInfo.map_value_type != nullptr;

   if (SequenceInfo.map_allows_any and (!check_keys) and (!check_values)) return true;

   SequenceTypeInfo key_requirements;
   if (check_keys) {
      key_requirements.kind = SequenceItemKind::Atomic;
      key_requirements.type_name = SequenceInfo.map_key_type;
      key_requirements.occurrence = SequenceCardinality::ExactlyOne;
   }

   for (const auto &map_value : maps) {
      const XPathMapStorage *storage = (map_value.Type IS XPVT::Map) ? map_value.map_storage.get() : nullptr;
      if (!storage) continue;

      if (check_keys) {
         for (const auto &entry : storage->entries) {
            XPathVal key_candidate;
            key_candidate.Type = XPVT::String;
            key_candidate.StringValue = entry.key;
            auto key_match = Evaluator.matches_sequence_type(key_candidate, key_requirements, ContextNode);
            if (!key_match.has_value()) return std::nullopt;
            if (!key_match.value()) return false;
         }
      }

      if (check_values and SequenceInfo.map_value_type) {
         for (const auto &entry : storage->entries) {
            XPathVal entry_value = Evaluator.materialise_sequence_value(entry.value);
            auto value_match = Evaluator.matches_sequence_type(entry_value, *SequenceInfo.map_value_type, ContextNode);
            if (!value_match.has_value()) return std::nullopt;
            if (!value_match.value()) return false;
         }
      }
   }

   return true;
}

static std::optional<bool> array_sequence_matches_type(const XPathVal &Value, const SequenceTypeInfo &SequenceInfo,
   XPathEvaluator &Evaluator, const XPathNode *ContextNode)
{
   std::vector<XPathVal> arrays;
   if (!gather_items_of_type(Value, XPVT::Array, arrays)) return false;
   if (arrays.empty()) return false;

   if (SequenceInfo.array_allows_any or (!SequenceInfo.array_member_type)) return true;

   for (const auto &array_value : arrays) {
      const XPathArrayStorage *storage = (array_value.Type IS XPVT::Array) ? array_value.array_storage.get() : nullptr;
      if (!storage) continue;
      for (const auto &member : storage->members) {
         XPathVal member_value = Evaluator.materialise_sequence_value(member);
         auto member_match = Evaluator.matches_sequence_type(member_value, *SequenceInfo.array_member_type, ContextNode);
         if (!member_match.has_value()) return std::nullopt;
         if (!member_match.value()) return false;
      }
   }

   return true;
}

static bool function_sequence_matches_type(const XPathVal &Value)
{
   std::vector<XPathVal> functions;
   if (!gather_function_items(Value, functions)) return false;
   return !functions.empty();
}

//********************************************************************************************************************
// Atomises a runtime value and enforces single-item cardinality for lookup keys.

//********************************************************************************************************************
// Extracts the string value of a node-set item at the specified index, with fallback to node string conversion.

static std::string nodeset_item_string(const XPathVal &Value, size_t Index)
{
   if (Index < Value.node_set_string_values.size()) return Value.node_set_string_values[Index];

   if ((Index < Value.node_set_attributes.size()) and Value.node_set_attributes[Index]) {
      return Value.node_set_attributes[Index]->Value;
   }

   bool use_override = Value.node_set_string_override.has_value() and Value.node_set_string_values.empty();
   if (use_override and (Index IS 0)) return *Value.node_set_string_override;

   if (Index < Value.node_set.size() and Value.node_set[Index]) {
      return XPathVal::node_string_value(Value.node_set[Index]);
   }

   return std::string();
}

//********************************************************************************************************************
// Atomises a runtime value and enforces single-item cardinality for lookup keys.

static std::optional<XPathVal> atomize_singleton(XPathEvaluator &Evaluator, const XPathVal &Value,
   const XPathNode *ContextNode, std::string_view Description)
{
   size_t item_count = sequence_item_count(Value);

   if (item_count IS 0) {
      auto message = std::format("XPTY0004: {} requires a single item, but the operand was empty.", Description);
      Evaluator.record_error(message, ContextNode, true);
      Evaluator.expression_unsupported = true;
      return std::nullopt;
   }

   if (item_count > 1) {
      auto message = std::format("XPTY0004: {} requires a single item, but the operand had {} items.", Description, item_count);
      Evaluator.record_error(message, ContextNode, true);
      Evaluator.expression_unsupported = true;
      return std::nullopt;
   }

   if (Value.Type IS XPVT::NodeSet) {
      std::string lexical = nodeset_item_string(Value, 0);
      return XPathVal(std::string(lexical));
   }

   if ((Value.Type IS XPVT::Map) or (Value.Type IS XPVT::Array)) {
      auto message = std::format("XPTY0004: {} requires an atomic value, but encountered a {}.",
         Description, describe_value_type(Value));
      Evaluator.record_error(message, ContextNode, true);
      Evaluator.expression_unsupported = true;
      return std::nullopt;
   }

   return Value;
}

//********************************************************************************************************************
// Returns a human-readable description of the node kind (element, attribute, text, comment, processing-instruction).

static std::string describe_nodeset_item_kind(const XTag *Node, const XMLAttrib *Attribute)
{
   if (Attribute) return std::string("attribute()");
   if (!Node) return std::string("item()");
   if (Node->Attribs.empty()) return std::string("node()");
   if (Node->Attribs[0].Name.empty()) return std::string("text()");
   if ((Node->Flags & XTF::COMMENT) != XTF::NIL) return std::string("comment()");
   if ((Node->Flags & XTF::INSTRUCTION) != XTF::NIL) return std::string("processing-instruction()");
   return std::string("element()");
}

//********************************************************************************************************************
// Determines whether the given node is a text node (identified by an empty attribute name in Attribs[0]).

static bool is_text_node(const XTag *Node)
{
   if (!Node) return false;
   if (Node->Attribs.empty()) return false;
   return Node->Attribs[0].Name.empty();
}

//********************************************************************************************************************
// Identifies text nodes that were constructed (have zero parent ID) rather than parsed from a document.

static bool is_constructed_scalar_text(const XTag *Node)
{
   if (!is_text_node(Node)) return false;
   return Node->ParentID IS 0;
}

//********************************************************************************************************************
// Parses IEEE 754 lexical representations including INF/-INF/NaN tokens into double precision values.

static std::optional<double> parse_ieee_lexical_double(std::string_view Value)
{
   auto trimmed = trim_view(Value);
   if (trimmed.empty()) return std::nullopt;

   if ((trimmed IS std::string_view("INF")) or (trimmed IS std::string_view("+INF"))) {
      return std::numeric_limits<double>::infinity();
   }

   if (trimmed IS std::string_view("-INF")) {
      return -std::numeric_limits<double>::infinity();
   }

   if (trimmed IS std::string_view("NaN")) {
      return std::numeric_limits<double>::quiet_NaN();
   }

   std::string lexical(trimmed);
   char *end_ptr = nullptr;
   double result = std::strtod(lexical.c_str(), &end_ptr);
   if ((!end_ptr) or (*end_ptr != '\0')) return std::nullopt;
   return result;
}

//********************************************************************************************************************
// Parses an integer lexical string within the supplied inclusive range.

static std::optional<long long> parse_integer_lexical(std::string_view Value, long long Minimum, long long Maximum)
{
   auto trimmed = trim_view(Value);
   if (trimmed.empty()) return std::nullopt;

   long long parsed_value = 0;
   auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed_value);
   if ((result.ec != std::errc()) or (result.ptr != trimmed.data() + trimmed.size())) return std::nullopt;

   if (parsed_value < Minimum) return std::nullopt;
   if (parsed_value > Maximum) return std::nullopt;
   return parsed_value;
}

//********************************************************************************************************************
// Parses a lookup index from lexical form, enforcing the array index constraints.

static std::optional<long long> resolve_lookup_integer(std::string_view Lexical, XPathEvaluator &Evaluator,
   const XPathNode *ContextNode, std::string_view Description)
{
   auto parsed = parse_integer_lexical(Lexical, 1, std::numeric_limits<long long>::max());
   if (parsed.has_value()) return parsed;

   auto detail = std::format("{} must be a positive integer.", Description);
   auto message = xquery::errors::array_index_out_of_bounds(detail);
   Evaluator.record_error(message, ContextNode, true);
   Evaluator.expression_unsupported = true;
   return std::nullopt;
}

static thread_local std::unordered_map<std::string, std::weak_ptr<xml::schema::SchemaTypeDescriptor>> cast_target_cache;

//********************************************************************************************************************
// Tests whether a value can be safely cast to a target type using schema-aware coercion rules.

static bool is_value_castable_to_type(const XPathVal &Value,
   const std::shared_ptr<xml::schema::SchemaTypeDescriptor> &SourceDescriptor,
   const std::shared_ptr<xml::schema::SchemaTypeDescriptor> &TargetDescriptor,
   std::string_view Lexical)
{
   if (!TargetDescriptor) return false;

   auto target_type = TargetDescriptor->schema_type;

   if ((target_type IS xml::schema::SchemaType::XPathString) or (target_type IS xml::schema::SchemaType::XSString)) {
      return true;
   }

   XPathVal coerced = Value;
   if (SourceDescriptor) coerced = SourceDescriptor->coerce_value(Value, target_type);

   if (xml::schema::is_numeric(target_type)) {
      double numeric_value = coerced.to_number();
      return !std::isnan(numeric_value);
   }

   if ((target_type IS xml::schema::SchemaType::XPathBoolean) or (target_type IS xml::schema::SchemaType::XSBoolean)) {
      if (Value.Type IS XPVT::String) {
         auto parsed_boolean = parse_schema_boolean(Lexical);
         return parsed_boolean.has_value();
      }
      return true;
   }

   if (target_type IS xml::schema::SchemaType::XSDate) {
      if ((Value.Type IS XPVT::Date) or (Value.Type IS XPVT::DateTime)) return true;
      return is_valid_xs_date(Lexical);
   }

   if (target_type IS xml::schema::SchemaType::XSDateTime) {
      if (Value.Type IS XPVT::DateTime) return true;
      return is_valid_xs_datetime(Lexical);
   }

   if (target_type IS xml::schema::SchemaType::XSTime) {
      if (Value.Type IS XPVT::Time) return true;
      return is_valid_xs_time(Lexical);
   }

   if (SourceDescriptor) return SourceDescriptor->can_coerce_to(target_type);
   return false;
}

//********************************************************************************************************************
// Expands a variable QName to its canonical form, resolving namespace prefixes to URI references.

static std::string canonicalise_variable_qname(std::string_view Candidate,
   const XQueryProlog &SourceProlog, const extXML *Document)
{
   if (Candidate.starts_with("Q{")) {
      return std::string(Candidate);
   }

   size_t colon_position = Candidate.find(':');
   if (colon_position != std::string_view::npos) {
      std::string prefix(Candidate.substr(0, colon_position));
      std::string_view local_name_view = Candidate.substr(colon_position + 1);

      auto uri_entry = SourceProlog.declared_namespace_uris.find(prefix);
      if (uri_entry != SourceProlog.declared_namespace_uris.end()) {
         return std::format("Q{{{}}}{}", uri_entry->second, local_name_view);
      }

      if (Document) {
         auto prefix_it = Document->Prefixes.find(prefix);
         if (prefix_it != Document->Prefixes.end()) {
            auto ns_it = Document->NSRegistry.find(prefix_it->second);
            if (ns_it != Document->NSRegistry.end()) {
               return std::format("Q{{{}}}{}", ns_it->second, local_name_view);
            }
         }
      }
   }

   return std::string(Candidate);
}

//********************************************************************************************************************
// Appends a value to a sequence, decomposing node-sets into individual nodes or wrapping scalars as text nodes.

static void append_value_to_sequence(const XPathVal &Value, std::vector<SequenceEntry> &Entries,
   int &NextConstructedNodeId, std::vector<std::unique_ptr<XTag>> &ConstructedNodes)
{
   if (Value.Type IS XPVT::NodeSet) {
      bool use_override = Value.node_set_string_override.has_value() and Value.node_set_string_values.empty();
      for (size_t index = 0; index < Value.node_set.size(); ++index) {
         XTag *node = Value.node_set[index];
         if (not node) continue;

         const XMLAttrib *attribute = nullptr;
         if (index < Value.node_set_attributes.size()) attribute = Value.node_set_attributes[index];

         std::string item_string;
         if (index < Value.node_set_string_values.size()) item_string = Value.node_set_string_values[index];
         else if (use_override) item_string = *Value.node_set_string_override;
         else if (attribute) item_string = attribute->Value;
         else item_string = XPathVal::node_string_value(node);

         SequenceEntry entry;
         entry.node = node;
         entry.attribute = attribute;
         entry.string_value = std::move(item_string);
         if (index < Value.node_set_composite_values.size()) {
            entry.composite_value = Value.node_set_composite_values[index];
         }
         Entries.push_back(std::move(entry));
      }
      return;
   }

   std::string text = Value.to_string();
   pf::vector<XMLAttrib> text_attribs;
   text_attribs.emplace_back("", text);

   XTag text_node(NextConstructedNodeId--, 0, text_attribs);
   text_node.ParentID = 0;

   auto stored = std::make_unique<XTag>(std::move(text_node));
   XTag *root = stored.get();
   ConstructedNodes.push_back(std::move(stored));

   SequenceEntry entry;
   entry.node = root;
   entry.attribute = nullptr;
   entry.string_value = std::move(text);
   if ((Value.Type IS XPVT::Map) or (Value.Type IS XPVT::Array)) {
      entry.composite_value = std::make_shared<XPathValue>(Value);
   }
   Entries.push_back(std::move(entry));
}

//********************************************************************************************************************
// Maps string operator symbols and keywords to their corresponding binary operation kinds.

static BinaryOperationKind map_binary_operation(std::string_view Op)
{
   if (Op IS "and") return BinaryOperationKind::AND;
   if (Op IS "or") return BinaryOperationKind::OR;
   if (Op IS "|") return BinaryOperationKind::UNION;
   if (Op IS "intersect") return BinaryOperationKind::INTERSECT;
   if (Op IS "except") return BinaryOperationKind::EXCEPT;
   if (Op IS ",") return BinaryOperationKind::COMMA;
   if (Op IS "=") return BinaryOperationKind::GENERAL_EQ;
   if (Op IS "!=") return BinaryOperationKind::GENERAL_NE;
   if (Op IS "<") return BinaryOperationKind::GENERAL_LT;
   if (Op IS "<=") return BinaryOperationKind::GENERAL_LE;
   if (Op IS ">") return BinaryOperationKind::GENERAL_GT;
   if (Op IS ">=") return BinaryOperationKind::GENERAL_GE;
   if (Op IS "eq") return BinaryOperationKind::VALUE_EQ;
   if (Op IS "ne") return BinaryOperationKind::VALUE_NE;
   if (Op IS "lt") return BinaryOperationKind::VALUE_LT;
   if (Op IS "le") return BinaryOperationKind::VALUE_LE;
   if (Op IS "gt") return BinaryOperationKind::VALUE_GT;
   if (Op IS "ge") return BinaryOperationKind::VALUE_GE;
   if (Op IS "+") return BinaryOperationKind::ADD;
   if (Op IS "-") return BinaryOperationKind::SUB;
   if (Op IS "*") return BinaryOperationKind::MUL;
   if (Op IS "div") return BinaryOperationKind::DIV;
   if (Op IS "mod") return BinaryOperationKind::MOD;
   if (Op IS "to") return BinaryOperationKind::RANGE;
   return BinaryOperationKind::UNKNOWN;
}

static UnaryOperationKind map_unary_operation(std::string_view Op)
{
   if (Op IS "-") return UnaryOperationKind::NEGATE;
   if (Op IS "not") return UnaryOperationKind::LOGICAL_NOT;
   return UnaryOperationKind::UNKNOWN;
}

static constexpr int64_t RANGE_ITEM_LIMIT = 100000;

//********************************************************************************************************************
// Recursively appends operands from a binary operation chain into the provided container.

static void append_operation_chain_operand(const XPathNode *Node, BinaryOperationKind TargetOp,
   std::vector<const XPathNode *> &Operands)
{
   if (not Node) return;

   if ((Node->type IS XQueryNodeType::BINARY_OP) and
       (map_binary_operation(Node->get_value_view()) IS TargetOp) and (Node->child_count() >= 2))
   {
      const XPathNode *left_child = Node->get_child_safe(0);
      const XPathNode *right_child = Node->get_child_safe(1);

      if (left_child and right_child) {
         append_operation_chain_operand(left_child, TargetOp, Operands);
         append_operation_chain_operand(right_child, TargetOp, Operands);
         return;
      }
   }

   Operands.push_back(Node);
}

//********************************************************************************************************************
// Appends items from an iteration value to combined node-set containers, handling both node-sets and scalars.

static bool append_iteration_value_helper(const XPathVal &IterationValue, NODES &CombinedNodes,
   std::vector<const XMLAttrib *> &CombinedAttributes, std::vector<std::string> &CombinedStrings,
   std::optional<std::string> &CombinedOverride)
{
   if (IterationValue.Type IS XPVT::NodeSet) {
      size_t length = IterationValue.node_set.size();
      if (length < IterationValue.node_set_attributes.size()) length = IterationValue.node_set_attributes.size();
      if (length < IterationValue.node_set_string_values.size()) length = IterationValue.node_set_string_values.size();
      if ((length IS 0) and IterationValue.node_set_string_override.has_value()) length = 1;

      for (size_t node_index = 0; node_index < length; ++node_index) {
         XTag *node = node_index < IterationValue.node_set.size() ? IterationValue.node_set[node_index] : nullptr;
         CombinedNodes.push_back(node);

         const XMLAttrib *attribute = nullptr;
         if (node_index < IterationValue.node_set_attributes.size()) attribute = IterationValue.node_set_attributes[node_index];
         CombinedAttributes.push_back(attribute);

         std::string node_string;
         bool use_override = IterationValue.node_set_string_override.has_value() and IterationValue.node_set_string_values.empty() and (node_index IS 0);
         if (node_index < IterationValue.node_set_string_values.size()) node_string = IterationValue.node_set_string_values[node_index];
         else if (use_override) node_string = *IterationValue.node_set_string_override;
         else if (attribute) node_string = attribute->Value;
         else if (node) node_string = XPathVal::node_string_value(node);

         CombinedStrings.push_back(node_string);
         if (not CombinedOverride.has_value()) CombinedOverride = node_string;
      }

      if (IterationValue.node_set_string_override.has_value() and IterationValue.node_set_string_values.empty()) {
         if (not CombinedOverride.has_value()) CombinedOverride = IterationValue.node_set_string_override;
      }

      return true;
   }

   if (IterationValue.is_empty()) return true;

   std::string atomic_string = IterationValue.to_string();
   CombinedNodes.push_back(nullptr);
   CombinedAttributes.push_back(nullptr);
   CombinedStrings.push_back(atomic_string);
   if (not CombinedOverride.has_value()) CombinedOverride = atomic_string;
   return true;
}

//********************************************************************************************************************
// Recursively evaluates nested for-loop bindings, iterating through sequence items and accumulating results.

static bool evaluate_for_bindings_recursive(XPathEvaluator &Self, XPathContext &Context,
   const std::vector<ForBindingDefinition> &Bindings, size_t BindingIndex,
   const XPathNode *ReturnNode, uint32_t CurrentPrefix, NODES &CombinedNodes,
   std::vector<const XMLAttrib *> &CombinedAttributes, std::vector<std::string> &CombinedStrings,
   std::optional<std::string> &CombinedOverride, bool &ExpressionUnsupported)
{
   if (BindingIndex >= Bindings.size()) {
      auto iteration_value = Self.evaluate_expression(ReturnNode, CurrentPrefix);
      if (ExpressionUnsupported) return false;
      if (not append_iteration_value_helper(iteration_value, CombinedNodes, CombinedAttributes, CombinedStrings, CombinedOverride)) return false;
      return true;
   }

   const auto &binding = Bindings[BindingIndex];
   if (not binding.sequence) {
      ExpressionUnsupported = true;
      return false;
   }

   const std::string variable_name = binding.name;

   auto sequence_value = Self.evaluate_expression(binding.sequence, CurrentPrefix);
   if (ExpressionUnsupported) return false;

   if (sequence_value.Type != XPVT::NodeSet) {
      ExpressionUnsupported = true;
      return false;
   }

   size_t sequence_size = sequence_value.node_set.size();

   if (sequence_size IS 0) return true;

   for (size_t index = 0; index < sequence_size; ++index) {
      XTag *item_node = sequence_value.node_set[index];
      const XMLAttrib *item_attribute = nullptr;
      if (index < sequence_value.node_set_attributes.size()) {
         item_attribute = sequence_value.node_set_attributes[index];
      }

      XPathVal bound_value;
      bound_value.Type = XPVT::NodeSet;
      bound_value.preserve_node_order = false;
      bound_value.node_set.push_back(item_node);
      bound_value.node_set_attributes.push_back(item_attribute);

      std::string item_string;
      bool use_override = sequence_value.node_set_string_override.has_value() and (index IS 0) and sequence_value.node_set_string_values.empty();
      if (index < sequence_value.node_set_string_values.size()) {
         item_string = sequence_value.node_set_string_values[index];
      }
      else if (use_override) item_string = *sequence_value.node_set_string_override;
      else if (item_node) item_string = XPathVal::node_string_value(item_node);

      bound_value.node_set_string_values.push_back(item_string);
      bound_value.node_set_string_override = item_string;

      VariableBindingGuard iteration_guard(Context, variable_name, std::move(bound_value));

      Self.push_context(item_node, index + 1, sequence_size, item_attribute);
      bool iteration_ok = evaluate_for_bindings_recursive(Self, Context, Bindings, BindingIndex + 1,
         ReturnNode, CurrentPrefix, CombinedNodes, CombinedAttributes, CombinedStrings, CombinedOverride, ExpressionUnsupported);
      Self.pop_context();

      if (not iteration_ok) return false;
      if (ExpressionUnsupported) return false;
   }

   return true;
}

//********************************************************************************************************************
// Recursively evaluates nested quantified expression bindings for 'some' and 'every' expressions.

static bool evaluate_quantified_binding_recursive(XPathEvaluator &Self, XPathContext &Context,
   const std::vector<QuantifiedBindingDefinition> &Bindings, size_t BindingIndex,
   bool IsSome, bool IsEvery, const XPathNode *ConditionNode, uint32_t CurrentPrefix,
   bool &ExpressionUnsupported)
{
   if (BindingIndex >= Bindings.size()) {
      auto condition_value = Self.evaluate_expression(ConditionNode, CurrentPrefix);
      if (ExpressionUnsupported) return false;
      return condition_value.to_boolean();
   }

   const auto &binding = Bindings[BindingIndex];
   if (not binding.sequence) {
      ExpressionUnsupported = true;
      return false;
   }

   const std::string variable_name = binding.name;

   auto sequence_value = Self.evaluate_expression(binding.sequence, CurrentPrefix);
   if (ExpressionUnsupported) return false;

   if (sequence_value.Type != XPVT::NodeSet) {
      ExpressionUnsupported = true;
      return false;
   }

   size_t sequence_size = sequence_value.node_set.size();

   if (sequence_size IS 0) return IsEvery;

   for (size_t index = 0; index < sequence_size; ++index) {
      XTag *item_node = sequence_value.node_set[index];
      const XMLAttrib *item_attribute = nullptr;
      if (index < sequence_value.node_set_attributes.size()) {
         item_attribute = sequence_value.node_set_attributes[index];
      }

      XPathVal bound_value;
      bound_value.Type = XPVT::NodeSet;
      bound_value.preserve_node_order = false;
      bound_value.node_set.push_back(item_node);
      bound_value.node_set_attributes.push_back(item_attribute);

      std::string item_string;
      bool use_override = sequence_value.node_set_string_override.has_value() and (index IS 0) and sequence_value.node_set_string_values.empty();
      if (index < sequence_value.node_set_string_values.size()) {
         item_string = sequence_value.node_set_string_values[index];
      }
      else if (use_override) item_string = *sequence_value.node_set_string_override;
      else if (item_node) item_string = XPathVal::node_string_value(item_node);

      bound_value.node_set_string_values.push_back(item_string);
      bound_value.node_set_string_override = item_string;

      VariableBindingGuard iteration_guard(Context, variable_name, std::move(bound_value));

      Self.push_context(item_node, index + 1, sequence_size, item_attribute);
      bool branch_result = evaluate_quantified_binding_recursive(Self, Context, Bindings, BindingIndex + 1,
         IsSome, IsEvery, ConditionNode, CurrentPrefix, ExpressionUnsupported);
      Self.pop_context();

      if (ExpressionUnsupported) return false;

      if (branch_result) {
         if (IsSome) return true;
      }
      else if (IsEvery) return false;
   }

   return IsEvery;
}

//********************************************************************************************************************
// Resolves a variable reference by consulting the dynamic context, document bindings, and finally the prolog.

bool XPathEvaluator::resolve_variable_value(std::string_view QName, uint32_t CurrentPrefix,
   XPathVal &OutValue, const XPathNode *ReferenceNode)
{
   std::string name(QName);
   std::string normalised_name(name);

   if (context.variables) {
      auto local_variable = context.variables->find(name);
      if (local_variable != context.variables->end()) {
         OutValue = local_variable->second;
         return true;
      }
   }

   if (auto var = query->Variables.find(name); var != query->Variables.end()) {
      OutValue = XPathVal(var->second);
      return true;
   }

   auto prolog = context.prolog;
   if (not prolog) return false;

   const XQueryVariable *variable = prolog->find_variable(QName);
   std::shared_ptr<XQueryProlog> owner_prolog = prolog;
   auto active_module_cache = context.module_cache;
   std::string module_uri;
   std::string imported_local_name;
   std::string canonical_lookup;

   if (not variable) {
      uint32_t namespace_hash = 0;

      if ((name.size() > 2) and (name[0] IS 'Q') and (name[1] IS '{')) {
         size_t closing = name.find('}');
         if (closing != std::string::npos) {
            module_uri = name.substr(2, closing - 2);
            imported_local_name = name.substr(closing + 1);
            if (not module_uri.empty()) namespace_hash = pf::strhash(module_uri);
         }
      }

      if (not namespace_hash) {
         auto separator = name.find(':');
         if (separator != std::string::npos) {
            std::string prefix = name.substr(0, separator);
            imported_local_name = name.substr(separator + 1);
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

      if (matched_import) {
         if (module_uri.empty()) {
            std::string message = "Module variable '" + name + "' has an unresolved namespace.";
            record_error(message, ReferenceNode, true);
            return false;
         }

         auto module_cache = context.module_cache;
         if (not module_cache) {
            record_error("Module variable '" + name + "' requires a module cache.", ReferenceNode, true);
            return false;
         }

         (void)module_cache->fetch_or_load(module_uri, *prolog, *this);

         auto module_info = module_cache->find_module(module_uri);
         if (not module_info) {
            // Preserve earlier loader diagnostics when present
            record_error("Module '" + module_uri + "' could not be loaded for variable '" + name + "'.", ReferenceNode, false);
            return false;
         }
         else if (not module_info->prolog) {
            record_error("Module '" + module_uri + "' does not expose a prolog.", ReferenceNode, false);
            return false;
         }

         const XQueryVariable *module_variable = module_info->prolog->find_variable(name);

         if (not module_uri.empty() and not imported_local_name.empty()) {
            canonical_lookup = std::format("Q{{{}}}{}", module_uri, imported_local_name);
         }

         if (not module_variable and not canonical_lookup.empty()) {
            module_variable = module_info->prolog->find_variable(canonical_lookup);
         }

         if (not module_variable) {
            for (const auto &entry : module_info->prolog->variables) {
               const auto &candidate = entry.second;
               if (candidate.qname IS name) {
                  module_variable = &candidate;
                  break;
               }

               if (not canonical_lookup.empty() and (candidate.qname IS canonical_lookup)) {
                  module_variable = &candidate;
                  break;
               }

               size_t colon_pos = candidate.qname.find(':');
               if ((colon_pos != std::string::npos) and !imported_local_name.empty()) {
                  std::string candidate_prefix = candidate.qname.substr(0, colon_pos);
                  std::string candidate_local = candidate.qname.substr(colon_pos + 1);
                  if (candidate_local IS imported_local_name) {
                     uint32_t candidate_hash = module_info->prolog->resolve_prefix(candidate_prefix, nullptr);
                     if (candidate_hash IS namespace_hash) {
                        module_variable = &candidate;
                        break;
                     }
                  }
               }
            }
         }

         if (not module_variable) {
            std::string message = "Module variable '" + name + "' is not declared by namespace '" + module_uri + "'.";
            record_error(message, ReferenceNode, true);
            return false;
         }

         variable = module_variable;
         owner_prolog = module_info->prolog;
         active_module_cache = module_cache;
      }

      if (not variable) return false;
   }

   if (owner_prolog) {
      if (not canonical_lookup.empty()) normalised_name = canonical_lookup;
      else {
         normalised_name = canonicalise_variable_qname(name, *owner_prolog, context.xml);
         if (normalised_name IS name) {
            normalised_name = canonicalise_variable_qname(variable->qname, *owner_prolog, context.xml);
         }
      }
   }

   auto cached_value = prolog_variable_cache.find(normalised_name);
   if (cached_value != prolog_variable_cache.end()) {
      OutValue = cached_value->second;
      return true;
   }

   if (normalised_name != name) {
      auto alias_value = prolog_variable_cache.find(name);
      if (alias_value != prolog_variable_cache.end()) {
         prolog_variable_cache.insert_or_assign(normalised_name, alias_value->second);
         OutValue = alias_value->second;
         return true;
      }
   }

   if (variable->qname != normalised_name) {
      auto declared_value = prolog_variable_cache.find(variable->qname);
      if (declared_value != prolog_variable_cache.end()) {
         prolog_variable_cache.insert_or_assign(normalised_name, declared_value->second);
         if (normalised_name != name) {
            prolog_variable_cache.insert_or_assign(name, declared_value->second);
         }
         OutValue = declared_value->second;
         return true;
      }
   }

   if (variable->is_external) {
      std::string message = "External variable '" + name + "' is not supported.";
      record_error(message, ReferenceNode, true);
      return false;
   }

   if (not variable->initializer) {
      std::string message = "Variable '" + name + "' is missing an initialiser.";
      record_error(message, ReferenceNode, true);
      return false;
   }

   if (variables_in_evaluation.find(normalised_name) != variables_in_evaluation.end()) {
      std::string message = "Variable '" + name + "' has a circular dependency.";
      record_error(message, ReferenceNode, true);
      return false;
   }

   auto previous_prolog = context.prolog;
   auto previous_cache = context.module_cache;

   bool switched_context = false;
   if (owner_prolog and (owner_prolog.get() != previous_prolog.get())) {
      context.prolog = owner_prolog;
      if (active_module_cache) context.module_cache = active_module_cache;
      switched_context = true;
   }

   variables_in_evaluation.insert(normalised_name);
   XPathVal computed_value = evaluate_expression(variable->initializer.get(), CurrentPrefix);
   variables_in_evaluation.erase(normalised_name);

   if (switched_context) {
      context.prolog = previous_prolog;
      context.module_cache = previous_cache;
   }

   if (expression_unsupported) {
      std::string message = "Failed to evaluate initialiser for variable '" + name + "'.";
      record_error(message, ReferenceNode);
      return false;
   }

   auto inserted = prolog_variable_cache.insert_or_assign(normalised_name, computed_value);
   OutValue = inserted.first->second;

   if (normalised_name != name) {
      prolog_variable_cache.insert_or_assign(name, inserted.first->second);
   }

   if (variable->qname != normalised_name and variable->qname != name) {
      prolog_variable_cache.insert_or_assign(variable->qname, inserted.first->second);
   }

   return true;
}

//********************************************************************************************************************
std::optional<bool> XPathEvaluator::matches_sequence_type(const XPathVal &Value, const SequenceTypeInfo &SequenceInfo,
   const XPathNode *ContextNode)
{
   size_t item_count = sequence_item_count(Value);

   if (SequenceInfo.kind IS SequenceItemKind::EmptySequence) return item_count IS 0;

   if ((item_count IS 0) and (not SequenceInfo.allows_empty())) return false;
   if ((item_count > 1) and (not SequenceInfo.allows_multiple())) return false;
   if ((SequenceInfo.occurrence IS SequenceCardinality::ExactlyOne) and (item_count != 1)) return false;

   if (item_count IS 0) return true;

   if (SequenceInfo.kind IS SequenceItemKind::Item) return true;

   if (SequenceInfo.kind IS SequenceItemKind::Map) {
      auto map_match = map_sequence_matches_type(Value, SequenceInfo, *this, ContextNode);
      if (!map_match.has_value()) return std::nullopt;
      return *map_match;
   }

   if (SequenceInfo.kind IS SequenceItemKind::Array) {
      auto array_match = array_sequence_matches_type(Value, SequenceInfo, *this, ContextNode);
      if (!array_match.has_value()) return std::nullopt;
      return *array_match;
   }

   if (SequenceInfo.kind IS SequenceItemKind::Function) {
      return function_sequence_matches_type(Value);
   }

   if (SequenceInfo.kind IS SequenceItemKind::Node) {
      if (Value.Type IS XPVT::NodeSet) {
         for (size_t index = 0; index < item_count; ++index) {
            const XMLAttrib *attribute = (index < Value.node_set_attributes.size()) ?
               Value.node_set_attributes[index] : nullptr;
            XTag *node = (index < Value.node_set.size()) ? Value.node_set[index] : nullptr;

            if (attribute) continue;
            if (!node) return false;
            if (is_constructed_scalar_text(node)) return false;
         }
         return true;
      }
      return false;
   }

   if (SequenceInfo.kind IS SequenceItemKind::Element) {
      if (Value.Type IS XPVT::NodeSet) {
         for (size_t index = 0; index < item_count; ++index) {
            const XMLAttrib *attribute = (index < Value.node_set_attributes.size()) ?
               Value.node_set_attributes[index] : nullptr;
            XTag *node = (index < Value.node_set.size()) ? Value.node_set[index] : nullptr;
            if (attribute or (not node) or (not node->isTag())) return false;
         }
         return true;
      }
      return false;
   }

   if (SequenceInfo.kind IS SequenceItemKind::Attribute) {
      if (Value.Type IS XPVT::NodeSet) {
         for (size_t index = 0; index < item_count; ++index) {
            const XMLAttrib *attribute = (index < Value.node_set_attributes.size()) ?
               Value.node_set_attributes[index] : nullptr;
            if (!attribute) return false;
         }
         return true;
      }
      return false;
   }

   if (SequenceInfo.kind IS SequenceItemKind::Text) {
      if (Value.Type IS XPVT::NodeSet) {
         for (size_t index = 0; index < item_count; ++index) {
            const XMLAttrib *attribute = (index < Value.node_set_attributes.size()) ?
               Value.node_set_attributes[index] : nullptr;
            XTag *node = (index < Value.node_set.size()) ? Value.node_set[index] : nullptr;
            if (attribute or (not is_text_node(node))) return false;
         }
         return true;
      }
      return false;
   }

   auto &registry = xml::schema::registry();
   auto target_descriptor = registry.find_descriptor(SequenceInfo.type_name);
   if (!target_descriptor) {
      auto message = std::format("XPST0052: Sequence type '{}' is not defined.", SequenceInfo.type_name);
      record_error(message, ContextNode, true);
      return std::nullopt;
   }

   xml::schema::TypeChecker checker(registry);

   if (Value.Type IS XPVT::NodeSet) {
      for (size_t index = 0; index < item_count; ++index) {
         const XMLAttrib *attribute = (index < Value.node_set_attributes.size()) ?
            Value.node_set_attributes[index] : nullptr;
         XTag *node = (index < Value.node_set.size()) ? Value.node_set[index] : nullptr;

         bool atomic_source = false;
         if (attribute) atomic_source = true;
         else if (!node) atomic_source = true;
         else if (is_constructed_scalar_text(node)) atomic_source = true;

         if (not atomic_source) return false;

         std::string lexical = nodeset_item_string(Value, index);
         XPathVal item_value(lexical);
         if (not checker.validate_value(item_value, *target_descriptor)) return false;
      }
      return true;
   }

   auto target_schema = target_descriptor->schema_type;
   auto value_schema = Value.get_schema_type();
   auto value_descriptor = registry.find_descriptor(value_schema);

   auto is_boolean_schema = [](xml::schema::SchemaType Type) noexcept {
      return (Type IS xml::schema::SchemaType::XPathBoolean) or (Type IS xml::schema::SchemaType::XSBoolean);
   };

   if (xml::schema::is_numeric(target_schema)) {
      if (not xml::schema::is_numeric(value_schema)) return false;
   }
   else if (is_boolean_schema(target_schema)) {
      if (not is_boolean_schema(value_schema)) return false;
   }
   else if (xml::schema::is_string_like(target_schema)) {
      if (not xml::schema::is_string_like(value_schema)) return false;
   }
   else if (value_descriptor) {
      if (not value_descriptor->is_derived_from(target_schema) and not target_descriptor->is_derived_from(value_schema)) {
         return false;
      }
   }

   if (not checker.validate_value(Value, *target_descriptor)) return false;
   return true;
}

//********************************************************************************************************************
// Evaluates an EMPTY_SEQUENCE node and returns the computed value.
//
// The EMPTY_SEQUENCE node represents the literal empty sequence `()` defined by the XQuery grammar.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::EMPTY_SEQUENCE.
//
// **Behaviour**:
// - Produces an empty node-set value that models the absence of items.
// - Leaves `expression_unsupported` untouched because the node is always valid.
//
// **Returns**:
// - On success: XPathVal containing an empty node-set.
// - On failure: Empty XPathVal with `expression_unsupported` set (not expected for this node).
//
// **Examples**:
// - `()` → empty sequence.
//
// **Related**:
// - XQuery spec section 3.3.1.
// - Called from: evaluate_expression().
// - May recursively call: n/a.

XPathVal XPathEvaluator::handle_empty_sequence(const XPathNode *Node, uint32_t CurrentPrefix)
{
   (void)Node;
   (void)CurrentPrefix;
   return XPathVal(pf::vector<XTag *>{});
}

//********************************************************************************************************************
// Evaluates a NUMBER node and returns the computed value.
//
// The NUMBER node represents an xs:double literal parsed from the query text.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::NUMBER.
// - Node value must contain the lexical form of an xs:double.
//
// **Behaviour**:
// - Parses the lexical representation to double precision using std::strtod.
// - Returns NaN if the literal cannot be parsed completely.
//
// **Returns**:
// - On success: XPathVal containing the parsed double value.
// - On failure: XPathVal containing NaN while leaving `expression_unsupported` false.
//
// **Examples**:
// - `42` → 42.0.
// - `1.2e3` → 1200.0.
//
// **Related**:
// - XQuery spec section 3.1.7.
// - Called from: evaluate_expression().
// - May recursively call: n/a.

XPathVal XPathEvaluator::handle_number(const XPathNode *Node, uint32_t CurrentPrefix)
{
   (void)CurrentPrefix;
   char *end_ptr = nullptr;
   auto literal = Node->get_value_view();
   double value = std::strtod(literal.data(), &end_ptr);
   if ((end_ptr) and (*end_ptr IS '\0')) return XPathVal(value);
   return XPathVal(std::numeric_limits<double>::quiet_NaN());
}

//********************************************************************************************************************
// Evaluates a LITERAL/STRING node and returns the computed value.
//
// Literal nodes represent xs:string values written directly in the query using quotes.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::LITERAL or XQueryNodeType::STRING.
//
// **Behaviour**:
// - Copies the stored lexical value into a result string.
// - Preserves existing whitespace and escapes supplied by the parser.
//
// **Returns**:
// - On success: XPathVal containing the literal string value.
// - On failure: Empty XPathVal with `expression_unsupported` set (not expected for this node).
//
// **Examples**:
// - `'books'` → "books".
//
// **Related**:
// - XQuery spec section 3.1.1.
// - Called from: evaluate_expression().
// - May recursively call: n/a.

XPathVal XPathEvaluator::handle_literal(const XPathNode *Node, uint32_t CurrentPrefix)
{
   (void)CurrentPrefix;
   return XPathVal(std::string(Node->get_value_view()));
}

//********************************************************************************************************************
// Evaluates a map constructor node by materialising each key/value pair into runtime storage.

XPathVal XPathEvaluator::handle_map_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (!Node) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto storage = std::make_shared<XPathMapStorage>();
   storage->entries.reserve(Node->map_entry_count());

   for (size_t index = 0; index < Node->map_entry_count(); ++index) {
      const auto *entry = Node->get_map_entry(index);
      if (!entry or !entry->key_expression or !entry->value_expression) {
         expression_unsupported = true;
         return XPathVal();
      }

      auto key_value = evaluate_expression(entry->key_expression.get(), CurrentPrefix);
      if (expression_unsupported) return XPathVal();

      auto atomised_key = atomize_singleton(*this, key_value, entry->key_expression.get(), "Map constructor key");
      if (!atomised_key.has_value()) return XPathVal();

      std::string canonical_key = atomised_key->to_string();

      auto value_result = evaluate_expression(entry->value_expression.get(), CurrentPrefix);
      if (expression_unsupported) return XPathVal();

      auto update_entry = [&](XPathMapEntry &Target) {
         store_value_in_sequence(value_result, Target.value);
      };

      bool replaced = false;
      for (auto &existing : storage->entries) {
         if (existing.key IS canonical_key) {
            update_entry(existing);
            replaced = true;
            break;
         }
      }

      if (!replaced) {
         XPathMapEntry stored;
         stored.key = std::move(canonical_key);
         update_entry(stored);
         storage->entries.push_back(std::move(stored));
      }
   }

   XPathVal result;
   result.Type = XPVT::Map;
   result.map_storage = std::move(storage);
   return result;
}

//********************************************************************************************************************
// Evaluates an array constructor node and materialises each member expression.

XPathVal XPathEvaluator::handle_array_constructor(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (!Node) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto storage = std::make_shared<XPathArrayStorage>();
   storage->members.reserve(Node->array_member_count());

   for (size_t index = 0; index < Node->array_member_count(); ++index) {
      auto *member = Node->get_array_member(index);
      if (!member) {
         expression_unsupported = true;
         return XPathVal();
      }

      auto member_value = evaluate_expression(member, CurrentPrefix);
      if (expression_unsupported) return XPathVal();

      XPathValueSequence stored;
      store_value_in_sequence(member_value, stored);
      storage->members.push_back(std::move(stored));
   }

   XPathVal result;
   result.Type = XPVT::Array;
   result.array_storage = std::move(storage);
   return result;
}

//********************************************************************************************************************
// Evaluates an XML Schema constructor function call once it has been resolved to a schema type descriptor.

XPathVal XPathEvaluator::evaluate_type_constructor(
   const std::shared_ptr<xml::schema::SchemaTypeDescriptor> &TargetDescriptor,
   const std::vector<XPathVal> &Args, const XPathNode *CallSite)
{
   if (!TargetDescriptor) {
      record_error("XPST0081: Constructor target type is not defined.", CallSite, true);
      return XPathVal();
   }

   if (Args.size() != 1) {
      auto message = std::format("XPTY0004: Constructor '{}' requires exactly one argument.", TargetDescriptor->type_name);
      record_error(message, CallSite, true);
      return XPathVal();
   }

   const XPathVal &operand = Args[0];
   size_t item_count = sequence_item_count(operand);

   if (item_count IS 0) {
      pf::vector<XTag *> empty_nodes;
      return XPathVal(empty_nodes);
   }

   if (item_count > 1) {
      auto message = std::format("XPTY0004: Constructor '{}' accepts a single item, but received {}.",
         TargetDescriptor->type_name, item_count);
      record_error(message, CallSite, true);
      return XPathVal();
   }

   XPathVal atomised = operand;
   if (operand.Type IS XPVT::NodeSet) {
      std::string lexical = nodeset_item_string(operand, 0);
      atomised = XPathVal(lexical);
   }

   auto invalid_lexical = [&](std::string_view Lexical) {
      auto message = std::format("FORG0001: Value '{}' is not valid for constructor '{}'.",
         Lexical, TargetDescriptor->type_name);
      record_error(message, CallSite, true);
      return XPathVal();
   };

   auto assign_schema = [&](XPathVal Value) {
      Value.set_schema_type(TargetDescriptor);
      return Value;
   };

   std::string lexical_string = atomised.to_string();
   std::string_view lexical_view = trim_view(lexical_string);
   auto target_type = TargetDescriptor->schema_type;

   auto integer_with_range = [&](long long Minimum, long long Maximum) -> XPathVal {
      auto parsed = parse_integer_lexical(lexical_view, Minimum, Maximum);
      if (not parsed.has_value()) return invalid_lexical(lexical_view);
      return assign_schema(XPathVal(static_cast<double>(*parsed)));
   };

   switch (target_type) {
      case xml::schema::SchemaType::XPathString:
      case xml::schema::SchemaType::XSString:
         return assign_schema(XPathVal(lexical_string));

      case xml::schema::SchemaType::XPathBoolean:
      case xml::schema::SchemaType::XSBoolean: {
         auto parsed = parse_schema_boolean(lexical_view);
         if (not parsed.has_value()) return invalid_lexical(lexical_view);
         return assign_schema(XPathVal(*parsed));
      }

      case xml::schema::SchemaType::XPathNumber:
      case xml::schema::SchemaType::XSDecimal:
      case xml::schema::SchemaType::XSFloat:
      case xml::schema::SchemaType::XSDouble: {
         auto parsed = parse_ieee_lexical_double(lexical_view);
         if (not parsed.has_value()) return invalid_lexical(lexical_view);
         return assign_schema(XPathVal(*parsed));
      }

      case xml::schema::SchemaType::XSInteger:
         return integer_with_range(std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max());

      case xml::schema::SchemaType::XSLong:
         return integer_with_range(std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max());

      case xml::schema::SchemaType::XSInt:
         return integer_with_range(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());

      case xml::schema::SchemaType::XSShort:
         return integer_with_range(std::numeric_limits<short>::min(), std::numeric_limits<short>::max());

      case xml::schema::SchemaType::XSByte:
         return integer_with_range(std::numeric_limits<signed char>::min(), std::numeric_limits<signed char>::max());

      case xml::schema::SchemaType::XSDate: {
         auto canonical_date_value = [&](std::string_view Lexical) -> std::optional<XPathVal> {
            auto canonical = canonicalise_xs_date(Lexical);
            if (not canonical.has_value()) return std::nullopt;
            return XPathVal(XPVT::Date, *canonical);
         };

         auto date_from_datetime = [&](std::string_view Lexical) -> std::optional<XPathVal> {
            auto extracted = extract_date_from_datetime(Lexical);
            if (not extracted.has_value()) return std::nullopt;
            return canonical_date_value(*extracted);
         };

         if (atomised.Type IS XPVT::Date) {
            auto value = canonical_date_value(atomised.StringValue);
            if (not value.has_value()) return invalid_lexical(atomised.StringValue);
            return assign_schema(*value);
         }

         if (atomised.Type IS XPVT::DateTime) {
            std::string_view source = lexical_view;
            if (!atomised.StringValue.empty()) source = atomised.StringValue;
            auto value = date_from_datetime(source);
            if (not value.has_value()) return invalid_lexical(source);
            return assign_schema(*value);
         }

         auto value = canonical_date_value(lexical_view);
         if (not value.has_value()) return invalid_lexical(lexical_view);
         return assign_schema(*value);
      }

      case xml::schema::SchemaType::XSDateTime: {
         auto canonical_datetime_value = [&](std::string_view Lexical) -> std::optional<XPathVal> {
            auto canonical = canonicalise_xs_datetime(Lexical);
            if (not canonical.has_value()) return std::nullopt;
            return XPathVal(XPVT::DateTime, *canonical);
         };

         std::string_view source = lexical_view;
         if ((atomised.Type IS XPVT::DateTime) and (!atomised.StringValue.empty())) source = atomised.StringValue;

         auto value = canonical_datetime_value(source);
         if (not value.has_value()) return invalid_lexical(source);
         return assign_schema(*value);
      }

      case xml::schema::SchemaType::XSTime: {
         auto canonical_time_value = [&](std::string_view Lexical) -> std::optional<XPathVal> {
            auto canonical = canonicalise_xs_time(Lexical);
            if (not canonical.has_value()) return std::nullopt;
            return XPathVal(XPVT::Time, *canonical);
         };

         auto time_from_datetime = [&](std::string_view Lexical) -> std::optional<XPathVal> {
            auto extracted = extract_time_from_datetime(Lexical);
            if (not extracted.has_value()) return std::nullopt;
            return canonical_time_value(*extracted);
         };

         if (atomised.Type IS XPVT::Time) {
            auto value = canonical_time_value(atomised.StringValue);
            if (not value.has_value()) return invalid_lexical(atomised.StringValue);
            return assign_schema(*value);
         }

         if (atomised.Type IS XPVT::DateTime) {
            std::string_view source = lexical_view;
            if (!atomised.StringValue.empty()) source = atomised.StringValue;
            auto value = time_from_datetime(source);
            if (not value.has_value()) return invalid_lexical(source);
            return assign_schema(*value);
         }

         auto value = canonical_time_value(lexical_view);
         if (not value.has_value()) return invalid_lexical(lexical_view);
         return assign_schema(*value);
      }

      case xml::schema::SchemaType::XSDuration: {
         if (atomised.has_schema_info() and (atomised.get_schema_type() IS xml::schema::SchemaType::XSDuration)) {
            XPathVal typed_value = atomised;
            typed_value.set_schema_type(TargetDescriptor);
            return typed_value;
         }

         DurationComponents components;
         if (!parse_xs_duration(lexical_view, components)) return invalid_lexical(lexical_view);
         return assign_schema(XPathVal(std::string(lexical_view)));
      }

      case xml::schema::SchemaType::XSQName: {
         if (atomised.has_schema_info() and (atomised.get_schema_type() IS xml::schema::SchemaType::XSQName)) {
            XPathVal typed_value = atomised;
            typed_value.set_schema_type(TargetDescriptor);
            return typed_value;
         }

         std::string prefix;
         std::string local;
         if (!parse_qname_lexical_value(lexical_view, prefix, local)) return invalid_lexical(lexical_view);

         std::string namespace_uri;
         if (prefix.empty()) {
            auto default_ns = resolve_default_element_namespace(context);
            if (default_ns.has_value()) namespace_uri = *default_ns;
         }
         else {
            auto resolved = resolve_namespace_uri(context, prefix);
            if (!resolved.has_value()) {
               auto message = std::format(
                  "FONS0004: QName prefix '{}' is not in scope for constructor '{}'.",
                  prefix, TargetDescriptor->type_name);
               record_error(message, CallSite, true);
               return XPathVal();
            }
            namespace_uri = *resolved;
         }

         auto canonical = canonicalise_qname_value(namespace_uri, prefix, local);
         return assign_schema(XPathVal(std::move(canonical)));
      }

      default:
         break;
   }

   auto message = std::format("XPST0051: Constructor for type '{}' is not supported yet.", TargetDescriptor->type_name);
   record_error(message, CallSite, true);
   return XPathVal();
}

//********************************************************************************************************************
// Evaluates a CAST_EXPRESSION node and returns the computed value.
//
// Cast expressions translate a sequence to a requested atomic type using the rules from the XQuery type system.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::CAST_EXPRESSION.
// - Node must own at least one child providing the operand expression.
//
// **Behaviour**:
// - Parses the target type literal stored on the node.
// - Validates operand cardinality and type compatibility against the schema registry.
// - Delegates to xml::schema::TypeChecker for actual conversion logic.
// - Records XPST/ XPTY errors via record_error() when requirements are not met.
//
// **Returns**:
// - On success: XPathVal containing the converted atomic value (or empty sequence for zero-arity operands).
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `xs:integer("5") cast as xs:double` → 5.0.
//
// **Related**:
// - XQuery spec section 3.12.1.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for the operand node.

XPathVal XPathEvaluator::handle_cast_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() IS 0) {
      record_error("Cast expression requires an operand.", Node, true);
      return XPathVal();
   }

   auto target_info = parse_cast_target_literal(Node->get_value_view());
   if (target_info.type_name.empty()) {
      record_error("XPST0003: Cast expression is missing its target type.", Node, true);
      return XPathVal();
   }

   auto &registry = xml::schema::registry();
   auto target_descriptor = registry.find_descriptor(target_info.type_name);
   if (!target_descriptor) {
      auto message = std::format("XPST0052: Cast target type '{}' is not defined.", target_info.type_name);
      record_error(message, Node, true);
      return XPathVal();
   }

   const XPathNode *operand_node = Node->get_child_safe(0);
   if (!operand_node) {
      record_error("Cast expression requires an operand.", Node, true);
      return XPathVal();
   }

   XPathVal operand_value = evaluate_expression(operand_node, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   if (operand_value.Type IS XPVT::NodeSet) {
      size_t item_count = operand_value.node_set.size();
      if (item_count IS 0) {
         if (target_info.allows_empty) return XPathVal(pf::vector<XTag *>{});
         auto message = std::format("XPTY0004: Cast to '{}' requires a single item, but the operand was empty.",
            target_descriptor->type_name);
         record_error(message, Node, true);
         return XPathVal();
      }

      if (item_count > 1) {
         auto message = std::format("XPTY0004: Cast to '{}' requires a single item, but the operand had {} items.",
            target_descriptor->type_name, item_count);
         record_error(message, Node, true);
         return XPathVal();
      }

      std::string atomised_string = operand_value.to_string();
      operand_value = XPathVal(atomised_string);
      auto string_descriptor = registry.find_descriptor(xml::schema::SchemaType::XPathString);
      if (string_descriptor) operand_value.set_schema_type(string_descriptor);
   }

   auto source_descriptor = schema_descriptor_for_value(operand_value);
   if (!source_descriptor) source_descriptor = registry.find_descriptor(xml::schema::schema_type_for_xpath(operand_value.Type));
   if (!source_descriptor) {
      record_error("XPTY0006: Cast operand type could not be determined.", Node, true);
      return XPathVal();
   }

   std::string operand_lexical = operand_value.to_string();
   XPathVal coerced = source_descriptor->coerce_value(operand_value, target_descriptor->schema_type);

   if (xml::schema::is_numeric(target_descriptor->schema_type)) {
      double numeric_value = coerced.to_number();
      if (std::isnan(numeric_value)) {
         auto message = std::format("XPTY0006: Value '{}' cannot be cast to numeric type '{}'.",
            operand_lexical, target_descriptor->type_name);
         record_error(message, Node, true);
         return XPathVal();
      }
      coerced = XPathVal(numeric_value);
   }
   else if ((target_descriptor->schema_type IS xml::schema::SchemaType::XPathBoolean) or
            (target_descriptor->schema_type IS xml::schema::SchemaType::XSBoolean)) {
      bool lexical_valid = true;
      bool boolean_result = coerced.to_boolean();

      if (operand_value.Type IS XPVT::String) {
         auto parsed_boolean = parse_schema_boolean(operand_lexical);
         if (not parsed_boolean.has_value()) lexical_valid = false;
         else boolean_result = *parsed_boolean;
      }

      if (not lexical_valid) {
         auto message = std::format("XPTY0006: Value '{}' cannot be cast to boolean type '{}'.",
            operand_lexical, target_descriptor->type_name);
         record_error(message, Node, true);
         return XPathVal();
      }

      coerced = XPathVal(boolean_result);
   }
   else if ((target_descriptor->schema_type IS xml::schema::SchemaType::XPathString) or
            (target_descriptor->schema_type IS xml::schema::SchemaType::XSString)) {
      coerced = XPathVal(operand_lexical);
   }

   coerced.set_schema_type(target_descriptor);
   return coerced;
}

//********************************************************************************************************************
// Evaluates a CONDITIONAL node and returns the computed value.
//
// Conditional nodes implement the XQuery `if (Cond) then A else B` expression form.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::CONDITIONAL.
// - Node must provide exactly three children: condition, then-branch, else-branch.
//
// **Behaviour**:
// - Evaluates the condition child and converts it to effective boolean value.
// - Evaluates either the then or else branch based on the EBV result.
// - Propagates `expression_unsupported` from child evaluations.
//
// **Returns**:
// - On success: XPathVal produced by the selected branch.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `if ($flag) then "yes" else "no"` → "yes" or "no".
//
// **Related**:
// - XQuery spec section 3.8.3.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for child nodes.

XPathVal XPathEvaluator::handle_conditional(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() < 3) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto *condition_node = Node->get_child_safe(0);
   auto *then_node = Node->get_child_safe(1);
   auto *else_node = Node->get_child_safe(2);

   if ((not condition_node) or (not then_node) or (not else_node)) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto condition_value = evaluate_expression(condition_node, CurrentPrefix);
   if (expression_unsupported) return XPathVal();
   bool condition_boolean = condition_value.to_boolean();
   auto *selected_node = condition_boolean ? then_node : else_node;
   return evaluate_expression(selected_node, CurrentPrefix);
}

//********************************************************************************************************************
// Evaluates a TREAT_AS_EXPRESSION node and returns the computed value.
//
// Treat-as expressions assert that a sequence conforms to a requested type at runtime.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::TREAT_AS_EXPRESSION.
// - Node must provide one child expression supplying the operand.
//
// **Behaviour**:
// - Evaluates the operand expression.
// - Applies sequence type validation via parse_sequence_type_literal() and schema descriptors.
// - Raises dynamic errors when the operand violates the asserted type constraints.
//
// **Returns**:
// - On success: XPathVal identical to the operand result.
// - On failure: Empty XPathVal with `expression_unsupported` set after recording XPTY errors.
//
// **Examples**:
// - `$value treat as element()?` ensures optional element result.
//
// **Related**:
// - XQuery spec section 3.12.4.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for the operand.

XPathVal XPathEvaluator::handle_treat_as_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() IS 0) {
      record_error("Treat as expression requires an operand.", Node, true);
      return XPathVal();
   }

   auto node_value = Node->get_value_view();
   auto sequence_info = parse_sequence_type_literal(node_value);
   if (not sequence_info.has_value()) {
      record_error("XPST0003: Treat as expression is missing its sequence type.", Node, true);
      return XPathVal();
   }

   const XPathNode *operand_node = Node->get_child_safe(0);
   if (!operand_node) {
      record_error("Treat as expression requires an operand.", Node, true);
      return XPathVal();
   }

   XPathVal operand_value = evaluate_expression(operand_node, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   size_t item_count = sequence_item_count(operand_value);

   if (sequence_info->kind IS SequenceItemKind::EmptySequence) {
      if (item_count IS 0) return operand_value;

      auto message = std::format("XPTY0004: Treat as expression for 'empty-sequence()' requires an empty operand, "
         "but it contained {} item(s).", item_count);
      record_error(message, Node, true);
      return XPathVal();
   }

   if ((item_count IS 0) and (not sequence_info->allows_empty())) {
      auto message = std::format("XPTY0004: Treat as expression for '{}' requires at least one item, "
         "but the operand was empty.", node_value);
      record_error(message, Node, true);
      return XPathVal();
   }

   if ((item_count > 1) and (not sequence_info->allows_multiple())) {
      auto message = std::format("XPTY0004: Treat as expression for '{}' allows at most one item, "
         "but the operand had {} item(s).", node_value, item_count);
      record_error(message, Node, true);
      return XPathVal();
   }

   if ((sequence_info->occurrence IS SequenceCardinality::ExactlyOne) and (item_count != 1)) {
      auto message = std::format("XPTY0004: Treat as expression for '{}' requires exactly one item, "
         "but the operand had {} item(s).", node_value, item_count);
      record_error(message, Node, true);
      return XPathVal();
   }

   if (item_count IS 0) return operand_value;

   if (sequence_info->kind IS SequenceItemKind::Item) return operand_value;

   if (sequence_info->kind IS SequenceItemKind::Node) {
      if (operand_value.Type IS XPVT::NodeSet) return operand_value;

      auto message = std::format("XPTY0004: Treat as expression for 'node()' requires node values, but received '{}'.",
         operand_value.to_string());
      record_error(message, Node, true);
      return XPathVal();
   }

   if (sequence_info->kind IS SequenceItemKind::Element) {
      if (operand_value.Type IS XPVT::NodeSet) {
         for (size_t index = 0; index < operand_value.node_set.size(); ++index) {
            const XMLAttrib *attribute = (index < operand_value.node_set_attributes.size()) ?
               operand_value.node_set_attributes[index] : nullptr;
            XTag *node = operand_value.node_set[index];

            if (attribute or (not node) or (not node->isTag())) {
               auto encountered = describe_nodeset_item_kind(node, attribute);
               auto message = std::format("XPTY0004: Treat as expression for 'element()' encountered {}.", encountered);
               record_error(message, Node, true);
               return XPathVal();
            }
         }

         return operand_value;
      }

      auto message = std::format("XPTY0004: Treat as expression for 'element()' requires node values, "
         "but received '{}'.", operand_value.to_string());
      record_error(message, Node, true);
      return XPathVal();
   }

   if (sequence_info->kind IS SequenceItemKind::Attribute) {
      if (operand_value.Type IS XPVT::NodeSet) {
         for (size_t index = 0; index < operand_value.node_set.size(); ++index) {
            const XMLAttrib *attribute = (index < operand_value.node_set_attributes.size()) ?
               operand_value.node_set_attributes[index] : nullptr;
            if (not attribute) {
               auto node = operand_value.node_set[index];
               auto encountered = describe_nodeset_item_kind(node, nullptr);
               auto message = std::format("XPTY0004: Treat as expression for 'attribute()' encountered {}.", encountered);
               record_error(message, Node, true);
               return XPathVal();
            }
         }

         return operand_value;
      }

      auto message = std::format("XPTY0004: Treat as expression for 'attribute()' requires attribute nodes, "
         "but received '{}'.", operand_value.to_string());
      record_error(message, Node, true);
      return XPathVal();
   }

   if (sequence_info->kind IS SequenceItemKind::Text) {
      if (operand_value.Type IS XPVT::NodeSet) {
         for (size_t index = 0; index < operand_value.node_set.size(); ++index) {
            const XMLAttrib *attribute = (index < operand_value.node_set_attributes.size()) ?
               operand_value.node_set_attributes[index] : nullptr;
            XTag *node = operand_value.node_set[index];

            bool text_node = node and (not node->Attribs.empty()) and node->Attribs[0].Name.empty();
            if (attribute or (not text_node)) {
               auto encountered = describe_nodeset_item_kind(node, attribute);
               auto message = std::format("XPTY0004: Treat as expression for 'text()' encountered {}.", encountered);
               record_error(message, Node, true);
               return XPathVal();
            }
         }

         return operand_value;
      }

      auto message = std::format("XPTY0004: Treat as expression for 'text()' requires text nodes, but received '{}'.",
         operand_value.to_string());
      record_error(message, Node, true);
      return XPathVal();
   }

   if (sequence_info->kind IS SequenceItemKind::Map) {
      auto match_result = matches_sequence_type(operand_value, *sequence_info, Node);
      if (!match_result.has_value()) return XPathVal();
      if (*match_result) return operand_value;

      auto message = std::format("XPTY0004: Treat as expression for '{}' requires map values, but received '{}'.",
         node_value, describe_value_type(operand_value));
      record_error(message, Node, true);
      return XPathVal();
   }

   if (sequence_info->kind IS SequenceItemKind::Array) {
      auto match_result = matches_sequence_type(operand_value, *sequence_info, Node);
      if (!match_result.has_value()) return XPathVal();
      if (*match_result) return operand_value;

      auto message = std::format("XPTY0004: Treat as expression for '{}' requires array values, but received '{}'.",
         node_value, describe_value_type(operand_value));
      record_error(message, Node, true);
      return XPathVal();
   }

   if (sequence_info->kind IS SequenceItemKind::Function) {
      auto match_result = matches_sequence_type(operand_value, *sequence_info, Node);
      if (!match_result.has_value()) return XPathVal();
      if (*match_result) return operand_value;

      auto message = std::format("XPTY0004: Treat as expression for '{}' requires function items, but received '{}'.",
         node_value, describe_value_type(operand_value));
      record_error(message, Node, true);
      return XPathVal();
   }

   auto &registry = xml::schema::registry();
   auto target_descriptor = registry.find_descriptor(sequence_info->type_name);
   if (!target_descriptor) {
      auto message = std::format("XPST0052: Treat target type '{}' is not defined.", sequence_info->type_name);
      record_error(message, Node, true);
      return XPathVal();
   }

   xml::schema::TypeChecker checker(registry);

   if (operand_value.Type IS XPVT::NodeSet) {
      size_t length = sequence_item_count(operand_value);
      for (size_t index = 0; index < length; ++index) {
         const XMLAttrib *attribute = (index < operand_value.node_set_attributes.size()) ?
            operand_value.node_set_attributes[index] : nullptr;
         XTag *node = (index < operand_value.node_set.size()) ? operand_value.node_set[index] : nullptr;

         if (attribute) {
            auto encountered = describe_nodeset_item_kind(node, attribute);
            auto message = std::format(
               "XPTY0004: Treat as expression for '{}' encountered {} which is not an atomic value.",
               node_value, encountered);
            record_error(message, Node, true);
            return XPathVal();
         }

         if (node) {
            bool is_text = (not node->Attribs.empty()) and node->Attribs[0].Name.empty();
            bool constructed_scalar = is_text and (node->ParentID IS 0);
            if (not constructed_scalar) {
               auto encountered = describe_nodeset_item_kind(node, attribute);
               auto message = std::format(
                  "XPTY0004: Treat as expression for '{}' encountered {} which is not an atomic value.",
                  node_value, encountered);
               record_error(message, Node, true);
               return XPathVal();
            }
         }

         std::string lexical = nodeset_item_string(operand_value, index);
         XPathVal item_value(lexical);
         if (not checker.validate_value(item_value, *target_descriptor)) {
            std::string detail = checker.last_error();
            if (detail.empty()) detail = std::format("Value '{}' is not valid for type {}.", lexical, target_descriptor->type_name);
            auto message = std::format("XPTY0004: {}", detail);
            record_error(message, Node, true);
            return XPathVal();
         }
      }

      return operand_value;
   }

   if (not checker.validate_value(operand_value, *target_descriptor)) {
      std::string detail = checker.last_error();
      if (detail.empty()) detail = std::format("Value '{}' is not valid for type {}.", operand_value.to_string(),
         target_descriptor->type_name);
      auto message = std::format("XPTY0004: {}", detail);
      record_error(message, Node, true);
      return XPathVal();
   }

   operand_value.set_schema_type(target_descriptor);
   return operand_value;
}

//********************************************************************************************************************
// Evaluates an INSTANCE_OF_EXPRESSION node and returns the computed value.
//
// Instance-of expressions test whether a value matches a supplied sequence type literal.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::INSTANCE_OF_EXPRESSION.
// - Node must expose one child expression providing the value under test.
//
// **Behaviour**:
// - Evaluates the operand expression.
// - Interprets the sequence type literal using parse_sequence_type_literal().
// - Performs cardinality and item type checks against the computed value.
//
// **Returns**:
// - On success: XPathVal containing boolean true or false depending on the match.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `$item instance of xs:string` → true if operand is a string.
//
// **Related**:
// - XQuery spec section 3.12.5.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for the operand.

XPathVal XPathEvaluator::handle_instance_of_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() IS 0) {
      record_error("Instance of expression requires an operand.", Node, true);
      return XPathVal();
   }

   auto sequence_info = parse_sequence_type_literal(Node->get_value_view());
   if (not sequence_info.has_value()) {
      record_error("XPST0003: Instance of expression is missing its sequence type.", Node, true);
      return XPathVal();
   }

   const XPathNode *operand_node = Node->get_child_safe(0);
   if (!operand_node) {
      record_error("Instance of expression requires an operand.", Node, true);
      return XPathVal();
   }

   XPathVal operand_value = evaluate_expression(operand_node, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   auto match_result = matches_sequence_type(operand_value, *sequence_info, Node);
   if (not match_result.has_value()) return XPathVal();
   return XPathVal(*match_result);
}

//********************************************************************************************************************
// Evaluates a CASTABLE_EXPRESSION node and returns the computed value.
//
// Castable expressions check whether a value can be cast to a requested atomic type.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::CASTABLE_EXPRESSION.
// - Node must provide one child expression representing the operand.
//
// **Behaviour**:
// - Parses the target type literal stored on the node.
// - Evaluates the operand and assesses casting viability without performing conversion.
// - Applies XML Schema rules to determine compatibility.
//
// **Returns**:
// - On success: XPathVal containing boolean true/false for castability.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `$value castable as xs:integer` → true when the value can become an integer.
//
// **Related**:
// - XQuery spec section 3.12.2.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for the operand.

XPathVal XPathEvaluator::handle_castable_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() IS 0) {
      record_error("Castable expression requires an operand.", Node, true);
      return XPathVal();
   }

   auto target_info = parse_cast_target_literal(Node->get_value_view());
   if (target_info.type_name.empty()) {
      record_error("XPST0003: Castable expression is missing its target type.", Node, true);
      return XPathVal();
   }

   auto &registry = xml::schema::registry();
   std::shared_ptr<xml::schema::SchemaTypeDescriptor> target_descriptor;

   auto cache_entry = cast_target_cache.find(target_info.type_name);
   if (cache_entry != cast_target_cache.end()) target_descriptor = cache_entry->second.lock();

   if (!target_descriptor) {
      target_descriptor = registry.find_descriptor(target_info.type_name);
      if (!target_descriptor) {
         auto message = std::format("XPST0052: Cast target type '{}' is not defined.", target_info.type_name);
         record_error(message, Node, true);
         return XPathVal();
      }
      cast_target_cache.insert_or_assign(target_info.type_name, target_descriptor);
   }

   const XPathNode *operand_node = Node->get_child_safe(0);
   if (!operand_node) {
      record_error("Castable expression requires an operand.", Node, true);
      return XPathVal();
   }

   XPathVal operand_value = evaluate_expression(operand_node, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   if (operand_value.Type IS XPVT::NodeSet) {
      size_t item_count = operand_value.node_set.size();
      if (item_count IS 0) return XPathVal(target_info.allows_empty);
      if (item_count > 1) return XPathVal(false);

      std::string atomised_string = operand_value.to_string();
      operand_value = XPathVal(atomised_string);
      auto string_descriptor = registry.find_descriptor(xml::schema::SchemaType::XPathString);
      if (string_descriptor) operand_value.set_schema_type(string_descriptor);
   }

   auto source_descriptor = schema_descriptor_for_value(operand_value);
   if (!source_descriptor) source_descriptor = registry.find_descriptor(xml::schema::schema_type_for_xpath(operand_value.Type));
   if (!source_descriptor) return XPathVal(false);

   std::string operand_lexical = operand_value.to_string();
   bool castable_success = is_value_castable_to_type(operand_value, source_descriptor, target_descriptor, operand_lexical);
   return XPathVal(castable_success);
}

//********************************************************************************************************************
// Evaluates a TYPESWITCH_EXPRESSION node and returns the computed value.
//
// Typeswitch expressions provide multi-branch type pattern matching akin to switch statements.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::TYPESWITCH_EXPRESSION.
// - Node must contain at least two children: switch operand and one case/default clause.
//
// **Behaviour**:
// - Evaluates the operand once and caches the resulting value.
// - Iterates case clauses, checking sequence type matches and binding variables as required.
// - Evaluates the first matching branch or the default when no case succeeds.
// - Propagates `expression_unsupported` and errors from nested evaluations.
//
// **Returns**:
// - On success: XPathVal produced by the selected branch.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `typeswitch ($n) case xs:string return string-length($n) default return 0`.
//
// **Related**:
// - XQuery spec section 3.12.3.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for branch bodies.

XPathVal XPathEvaluator::handle_typeswitch_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() < 2) {
      record_error("Typeswitch expression requires at least one clause.", Node, true);
      return XPathVal();
   }

   const XPathNode *operand_node = Node->get_child_safe(0);
   if (!operand_node) {
      record_error("Typeswitch expression is missing its operand.", Node, true);
      return XPathVal();
   }

   XPathVal operand_value = evaluate_expression(operand_node, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   const XPathNode *default_clause = nullptr;

   for (size_t index = 1; index < Node->child_count(); ++index) {
      const XPathNode *clause_node = Node->get_child_safe(index);
      if (!clause_node) continue;

      if (clause_node->type IS XQueryNodeType::TYPESWITCH_CASE) {
         const auto *info = clause_node->get_typeswitch_case_info();
         if ((not info) or (not info->has_sequence_type())) {
            record_error("Typeswitch case clause is missing its sequence type.", clause_node, true);
            return XPathVal();
         }

         auto sequence_info = parse_sequence_type_literal(info->sequence_type);
         if (not sequence_info.has_value()) {
            record_error("XPST0003: Typeswitch case sequence type could not be parsed.", clause_node, true);
            return XPathVal();
         }

         auto match_result = matches_sequence_type(operand_value, *sequence_info, clause_node);
         if (not match_result.has_value()) return XPathVal();
         if (*match_result) {
            if (clause_node->child_count() IS 0) {
               record_error("Typeswitch case clause requires a return expression.", clause_node, true);
               return XPathVal();
            }

            const XPathNode *branch_expr = clause_node->get_child_safe(0);
            if (!branch_expr) {
               record_error("Typeswitch case clause requires a return expression.", clause_node, true);
               return XPathVal();
            }

            std::optional<VariableBindingGuard> binding_guard;
            if (info->has_variable()) binding_guard.emplace(context, info->variable_name, operand_value);

            auto branch_value = evaluate_expression(branch_expr, CurrentPrefix);
            if (expression_unsupported) return XPathVal();
            return branch_value;
         }

         continue;
      }

      if (clause_node->type IS XQueryNodeType::TYPESWITCH_DEFAULT_CASE) {
         default_clause = clause_node;
         continue;
      }

      record_error("Typeswitch expression encountered an unknown clause.", clause_node, true);
      return XPathVal();
   }

   if (!default_clause) {
      record_error("Typeswitch expression requires a default clause.", Node, true);
      return XPathVal();
   }

   if (default_clause->child_count() IS 0) {
      record_error("Typeswitch default clause requires a return expression.", default_clause, true);
      return XPathVal();
   }

   const XPathNode *default_expr = default_clause->get_child_safe(0);
   if (!default_expr) {
      record_error("Typeswitch default clause requires a return expression.", default_clause, true);
      return XPathVal();
   }

   std::optional<VariableBindingGuard> default_guard;
   const auto *default_info = default_clause->get_typeswitch_case_info();
   if (default_info and default_info->has_variable()) {
      default_guard.emplace(context, default_info->variable_name, operand_value);
   }

   auto default_value = evaluate_expression(default_expr, CurrentPrefix);
   if (expression_unsupported) return XPathVal();
   return default_value;
}

//********************************************************************************************************************
// Evaluates a UNION node and returns the computed value.
//
// Union nodes represent set-combining operators (union/intersect/except) normalised by the parser.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::UNION.
// - Node must have at least one child describing the operand sequence.
//
// **Behaviour**:
// - Delegates to evaluate_union_value() which applies the correct set semantics.
// - Propagates `expression_unsupported` when the delegated call fails.
//
// **Returns**:
// - On success: XPathVal containing the combined node-set.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `($a | $b)`.
//
// **Related**:
// - XQuery spec section 3.3.3.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() via evaluate_union_value().
XPathVal XPathEvaluator::handle_union_node(const XPathNode *Node, uint32_t CurrentPrefix)
{
   std::vector<const XPathNode *> branches;
   branches.reserve(Node->child_count());
   for (size_t index = 0; index < Node->child_count(); ++index) {
      auto *branch = Node->get_child_safe(index);
      if (branch) branches.push_back(branch);
   }
   return evaluate_union_value(branches, CurrentPrefix);
}

//********************************************************************************************************************
// Evaluates a LET_EXPRESSION node and returns the computed value.
//
// Let expressions bind variables to sequence values before evaluating a return clause.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::LET_EXPRESSION.
// - Node children must alternate between binding clauses and the return expression.
//
// **Behaviour**:
// - Iterates binding clauses, evaluating each initializer and installing the variable in scope.
// - Restores previous variable state after evaluation using scoped bindings.
// - Evaluates the final return expression in the extended context.
//
// **Returns**:
// - On success: XPathVal returned by the final expression.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `let $x := 5 return $x + 1`.
//
// **Related**:
// - XQuery spec section 3.8.4.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for bindings and return body.

XPathVal XPathEvaluator::handle_let_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() < 2) {
      record_error("LET expression requires at least one binding and a return clause.", Node, true);
      return XPathVal();
   }

   const XPathNode *return_node = Node->get_child_safe(Node->child_count() - 1);
   if (not return_node) {
      record_error("LET expression is missing its return clause.", Node, true);
      return XPathVal();
   }

   std::vector<VariableBindingGuard> binding_guards;
   binding_guards.reserve(Node->child_count() - 1);

   for (size_t index = 0; index + 1 < Node->child_count(); ++index) {
      const XPathNode *binding_node = Node->get_child_safe(index);
      if ((not binding_node) or !(binding_node->type IS XQueryNodeType::LET_BINDING)) {
         record_error("LET expression contains an invalid binding clause.", binding_node ? binding_node : Node, true);
         return XPathVal();
      }

      if ((binding_node->get_value_view().empty()) or (binding_node->child_count() IS 0)) {
         record_error("Let binding requires a variable name and expression.", binding_node, true);
         return XPathVal();
      }

      const XPathNode *binding_expr = binding_node->get_child_safe(0);
      if (not binding_expr) {
         record_error("Let binding requires an expression node.", binding_node, true);
         return XPathVal();
      }

      XPathVal bound_value = evaluate_expression(binding_expr, CurrentPrefix);
      if (expression_unsupported) {
         record_error("Let binding expression could not be evaluated.", binding_expr);
         return XPathVal();
      }

      binding_guards.emplace_back(context, std::string(binding_node->get_value_view()), std::move(bound_value));
   }

   auto result_value = evaluate_expression(return_node, CurrentPrefix);
   if (expression_unsupported) {
      record_error("Let return expression could not be evaluated.", return_node);
      return XPathVal();
   }
   return result_value;
}

//********************************************************************************************************************
// Evaluates a FOR_EXPRESSION node and returns the computed value.
//
// For expressions iterate over input sequences, binding variables before computing the result.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::FOR_EXPRESSION.
// - Node must provide binding clauses followed by a return expression.
//
// **Behaviour**:
// - Processes each binding, evaluating the source sequence.
// - Iterates through the sequence, updating position/size context and variable bindings.
// - Accumulates results from evaluating the return clause for each tuple.
//
// **Returns**:
// - On success: XPathVal representing the concatenated results of each iteration.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `for $x in /book return $x/title`.
//
// **Related**:
// - XQuery spec section 3.8.4.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for bindings and return clause.

XPathVal XPathEvaluator::handle_for_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() < 2) {
      expression_unsupported = true;
      return XPathVal();
   }

   const XPathNode *return_node = Node->get_child_safe(Node->child_count() - 1);
   if (not return_node) {
      expression_unsupported = true;
      return XPathVal();
   }

   std::vector<ForBindingDefinition> bindings;
   bindings.reserve(Node->child_count());

   bool legacy_layout = false;

   for (size_t index = 0; index + 1 < Node->child_count(); ++index) {
      const XPathNode *binding_node = Node->get_child_safe(index);
      if (binding_node and (binding_node->type IS XQueryNodeType::FOR_BINDING)) {
         if ((binding_node->get_value_view().empty()) or (binding_node->child_count() IS 0)) {
            expression_unsupported = true;
            return XPathVal();
         }

         bindings.push_back({ std::string(binding_node->get_value_view()), binding_node->get_child_safe(0) });
         continue;
      }

      legacy_layout = true;
      break;
   }

   if (legacy_layout) {
      if (Node->child_count() < 2) {
         expression_unsupported = true;
         return XPathVal();
      }

      const XPathNode *sequence_node = Node->get_child_safe(0);
      if ((not sequence_node) or (not return_node) or Node->get_value_view().empty()) {
         expression_unsupported = true;
         return XPathVal();
      }

      bindings.clear();
      bindings.push_back({ std::string(Node->get_value_view()), sequence_node });
   }

   if (bindings.empty()) {
      expression_unsupported = true;
      return XPathVal();
   }

   NODES combined_nodes;
   std::vector<std::string> combined_strings;
   std::vector<const XMLAttrib *> combined_attributes;
   std::optional<std::string> combined_override;

   bool evaluation_ok = evaluate_for_bindings_recursive(*this, context, bindings, 0, return_node, CurrentPrefix,
      combined_nodes, combined_attributes, combined_strings, combined_override, expression_unsupported);
   if (not evaluation_ok) return XPathVal();
   if (expression_unsupported) return XPathVal();

   XPathVal result;
   result.Type = XPVT::NodeSet;
   result.preserve_node_order = false;
   result.node_set = std::move(combined_nodes);
   result.node_set_string_values = std::move(combined_strings);
   result.node_set_attributes = std::move(combined_attributes);
   if (combined_override.has_value()) result.node_set_string_override = combined_override;
   else result.node_set_string_override.reset();
   return result;
}

//********************************************************************************************************************
// Evaluates a QUANTIFIED_EXPRESSION node and returns the computed value.
//
// Quantified expressions implement the `some` and `every` constructs that test predicates across bindings.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::QUANTIFIED_EXPRESSION.
// - Node must offer binding sequences and a predicate expression.
//
// **Behaviour**:
// - Iterates binding combinations using evaluate_quantified_binding_recursive().
// - Evaluates the predicate for each combination and applies the some/every semantics.
// - Aborts early when the quantifier condition is satisfied.
//
// **Returns**:
// - On success: XPathVal containing boolean true or false.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `some $x in $items satisfies $x gt 0`.
//
// **Related**:
// - XQuery spec section 3.13.4.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for binding sources and predicate.

XPathVal XPathEvaluator::handle_quantified_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() < 2) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto quantifier = Node->get_value_view();
   bool is_some = quantifier IS "some";
   bool is_every = quantifier IS "every";

   if ((not is_some) and (not is_every)) {
      expression_unsupported = true;
      return XPathVal();
   }

   const XPathNode *condition_node = Node->get_child_safe(Node->child_count() - 1);
   if (!condition_node) {
      expression_unsupported = true;
      return XPathVal();
   }

   std::vector<QuantifiedBindingDefinition> bindings;
   bindings.reserve(Node->child_count() - 1);

   for (size_t index = 0; index + 1 < Node->child_count(); ++index) {
      const XPathNode *binding_node = Node->get_child_safe(index);
      if ((not binding_node) or !(binding_node->type IS XQueryNodeType::QUANTIFIED_BINDING)) {
         expression_unsupported = true;
         return XPathVal();
      }

      if ((binding_node->get_value_view().empty()) or (binding_node->child_count() IS 0)) {
         expression_unsupported = true;
         return XPathVal();
      }

      const XPathNode *sequence_expr = binding_node->get_child_safe(0);
      if (!sequence_expr) {
         expression_unsupported = true;
         return XPathVal();
      }

      bindings.push_back({ std::string(binding_node->get_value_view()), sequence_expr });
   }

   if (bindings.empty()) {
      expression_unsupported = true;
      return XPathVal();
   }

   bool quant_result = evaluate_quantified_binding_recursive(*this, context, bindings, 0,
      is_some, is_every, condition_node, CurrentPrefix, expression_unsupported);
   if (expression_unsupported) return XPathVal();
   return XPathVal(quant_result);
}

//********************************************************************************************************************
// Evaluates a FILTER node and returns the computed value.
//
// Filter nodes apply predicate expressions to the result of an input sequence.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::FILTER.
// - Node must provide two children: the input sequence and the predicate expression.
//
// **Behaviour**:
// - Evaluates the input sequence.
// - Applies evaluate_predicate() to retain items whose predicate yields true.
// - Propagates context position/size while iterating.
//
// **Returns**:
// - On success: XPathVal representing the filtered sequence.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `/books/book[price lt 20]`.
//
// **Related**:
// - XQuery spec section 3.3.1.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for input and predicate.

XPathVal XPathEvaluator::handle_filter(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() IS 0) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto *base_node = Node->get_child_safe(0);
   if (not base_node) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto base_value = evaluate_expression(base_node, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   if (base_value.Type != XPVT::NodeSet) {
      expression_unsupported = true;
      return XPathVal();
   }

   std::vector<size_t> working_indices(base_value.node_set.size());
   for (size_t index = 0; index < working_indices.size(); ++index) {
      working_indices[index] = index;
   }

   for (size_t predicate_index = 1; predicate_index < Node->child_count(); ++predicate_index) {
      auto *predicate_node = Node->get_child_safe(predicate_index);
      if (not predicate_node) continue;

      std::vector<size_t> passed;
      passed.reserve(working_indices.size());

      for (size_t position = 0; position < working_indices.size(); ++position) {
         size_t base_index = working_indices[position];
         XTag *candidate = base_value.node_set[base_index];
         const XMLAttrib *attribute = nullptr;
         if (base_index < base_value.node_set_attributes.size()) {
            attribute = base_value.node_set_attributes[base_index];
         }

         push_context(candidate, position + 1, working_indices.size(), attribute);
         auto predicate_result = evaluate_predicate(predicate_node, CurrentPrefix);
         pop_context();

         if (predicate_result IS PredicateResult::UNSUPPORTED) {
            expression_unsupported = true;
            return XPathVal();
         }

         if (predicate_result IS PredicateResult::MATCH) passed.push_back(base_index);
      }

      working_indices.swap(passed);
      if (working_indices.empty()) break;
   }

   NODES filtered_nodes;
   filtered_nodes.reserve(working_indices.size());

   std::vector<std::string> filtered_strings;
   filtered_strings.reserve(working_indices.size());

   std::vector<const XMLAttrib *> filtered_attributes;
   filtered_attributes.reserve(working_indices.size());

   for (size_t index : working_indices) {
      filtered_nodes.push_back(base_value.node_set[index]);
      if (index < base_value.node_set_string_values.size()) {
         filtered_strings.push_back(base_value.node_set_string_values[index]);
      }
      const XMLAttrib *attribute = nullptr;
      if (index < base_value.node_set_attributes.size()) {
         attribute = base_value.node_set_attributes[index];
      }
      filtered_attributes.push_back(attribute);
   }

   std::optional<std::string> first_value;
   if (not working_indices.empty()) {
      size_t first_index = working_indices[0];
      if (base_value.node_set_string_override.has_value() and (first_index IS 0)) {
         first_value = base_value.node_set_string_override;
      }
      else if (first_index < base_value.node_set_string_values.size()) {
         first_value = base_value.node_set_string_values[first_index];
      }
   }

   return XPathVal(filtered_nodes, first_value, std::move(filtered_strings), std::move(filtered_attributes));
}

//********************************************************************************************************************
// Evaluates a PATH node and returns the computed value.
//
// Path nodes represent general path expressions that may mix location steps and expression segments.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::PATH.
// - Node must contain at least one child describing the base expression.
//
// **Behaviour**:
// - Evaluates the first child to obtain the starting sequence.
// - Iterates subsequent children, delegating to evaluate_path_expression_value() for navigation.
// - Maintains context updates for each navigation step.
//
// **Returns**:
// - On success: XPathVal representing the resulting node sequence.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `/bookstore/book/title`.
//
// **Related**:
// - XQuery spec section 3.3.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for intermediate steps.

XPathVal XPathEvaluator::handle_path(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() IS 0) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto *first_child = Node->get_child_safe(0);
   if (first_child and (first_child->type IS XQueryNodeType::LOCATION_PATH)) {
      return evaluate_path_expression_value(Node, CurrentPrefix);
   }

   auto base_value = evaluate_expression(first_child, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   if (base_value.Type != XPVT::NodeSet) {
      return XPathVal(base_value.to_node_set());
   }

   std::vector<const XPathNode *> steps;
   for (size_t index = 1; index < Node->child_count(); ++index) {
      auto *child = Node->get_child_safe(index);
      if (child and (child->type IS XQueryNodeType::STEP)) steps.push_back(child);
   }

   if (steps.empty()) return base_value;

   const XPathNode *attribute_step = nullptr;
   const XPathNode *attribute_test = nullptr;

   if (not steps.empty()) {
      auto *last_step = steps.back();
      const XPathNode *axis_node = nullptr;
      const XPathNode *node_test = nullptr;

      for (size_t index = 0; index < last_step->child_count(); ++index) {
         auto *child = last_step->get_child_safe(index);
         if (not child) continue;

         if (child->type IS XQueryNodeType::AXIS_SPECIFIER) axis_node = child;
         else if ((not node_test) and ((child->type IS XQueryNodeType::NAME_TEST) or
            (child->type IS XQueryNodeType::WILDCARD) or (child->type IS XQueryNodeType::NODE_TYPE_TEST))) node_test = child;
      }

      AxisType axis = axis_node ? AxisEvaluator::parse_axis_name(axis_node->get_value_view()) : AxisType::CHILD;
      if (axis IS AxisType::ATTRIBUTE) {
         attribute_step = last_step;
         attribute_test = node_test;
      }
   }

   return evaluate_path_from_nodes(base_value.node_set, base_value.node_set_attributes, steps,
      attribute_step, attribute_test, CurrentPrefix);
}

//********************************************************************************************************************
// Determines whether the provided binary operation kind supports chain flattening.

bool XPathEvaluator::is_arithmetic_chain_candidate(BinaryOperationKind OpKind) const
{
   return (OpKind IS BinaryOperationKind::ADD) or (OpKind IS BinaryOperationKind::MUL);
}

//********************************************************************************************************************
// Collects the operand nodes participating in a flattened arithmetic chain.

std::vector<const XPathNode *> XPathEvaluator::collect_operation_chain(const XPathNode *Node,
   BinaryOperationKind OpKind) const
{
   std::vector<const XPathNode *> operands;
   if (not Node) return operands;

   operands.reserve(Node->child_count());
   append_operation_chain_operand(Node, OpKind, operands);
   return operands;
}

//********************************************************************************************************************
// Iteratively evaluates a flattened arithmetic chain and combines the results.

XPathVal XPathEvaluator::evaluate_arithmetic_chain(const std::vector<const XPathNode *> &Operands,
   BinaryOperationKind OpKind, uint32_t CurrentPrefix)
{
   assert(not Operands.empty());
   assert(is_arithmetic_chain_candidate(OpKind));

   auto first_value = evaluate_expression(Operands[0], CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   double accumulator = first_value.to_number();

   for (size_t index = 1; index < Operands.size(); ++index) {
      auto operand_value = evaluate_expression(Operands[index], CurrentPrefix);
      if (expression_unsupported) return XPathVal();

      double operand_number = operand_value.to_number();
      if (OpKind IS BinaryOperationKind::ADD) accumulator += operand_number;
      else if (OpKind IS BinaryOperationKind::MUL) accumulator *= operand_number;
      else {
         expression_unsupported = true;
         return XPathVal();
      }
   }

   return XPathVal(accumulator);
}

//********************************************************************************************************************
// Evaluates a BINARY_OP node and returns the computed value.
//
// Binary operator nodes cover logical, arithmetic, comparison, sequence, and set operations.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::BINARY_OP.
// - Node must provide exactly two child operands.  Cached operator metadata is preferred but the evaluator
//   falls back to string-based mapping when absent.
//
// **Behaviour**:
// - Dispatches to specialised helpers based on BinaryOperationKind.
// - Ensures short-circuit semantics for logical operators via dedicated handlers.
// - Propagates evaluation errors and maintains constructed node ownership.
//
// **Returns**:
// - On success: XPathVal containing the combined result as defined by the operator semantics.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `$a + $b`, `$left and $right`, `$seq1 union $seq2`.
//
// **Related**:
// - XQuery spec sections 3.3.1, 3.4, 3.5, 3.6.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for operand nodes.

XPathVal XPathEvaluator::handle_binary_op(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() < 2) {
      expression_unsupported = true;
      return XPathVal();
   }

   const XPathNode *left_node = Node->get_child_safe(0);
   const XPathNode *right_node = Node->get_child_safe(1);
   if ((not left_node) or (not right_node)) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto operation = Node->get_value_view();
   BinaryOperationKind op_kind;

   if (auto cached_kind = Node->get_cached_binary_kind(); cached_kind.has_value()) {
      op_kind = *cached_kind;
   }
   else {
      op_kind = map_binary_operation(operation);
      binary_operator_cache_fallbacks++;
      if (is_trace_enabled()) {
         pf::Log log("XPath");
         log.msg(VLF::TRACE, "Binary operator cache miss for '%.*s'", (int)operation.size(), operation.data());
      }
   }

   if (is_arithmetic_chain_candidate(op_kind)) {
      auto operands = collect_operation_chain(Node, op_kind);
      if (operands.size() >= 3) {
         return evaluate_arithmetic_chain(operands, op_kind, CurrentPrefix);
      }
   }

   switch (op_kind) {
      case BinaryOperationKind::AND:
      case BinaryOperationKind::OR:
         return handle_binary_logical(Node, left_node, right_node, CurrentPrefix, op_kind);

      case BinaryOperationKind::UNION:
      case BinaryOperationKind::INTERSECT:
      case BinaryOperationKind::EXCEPT:
         return handle_binary_set_ops(Node, left_node, right_node, CurrentPrefix, op_kind);

      case BinaryOperationKind::COMMA:
      case BinaryOperationKind::RANGE:
         return handle_binary_sequence(Node, left_node, right_node, CurrentPrefix, op_kind);

      case BinaryOperationKind::GENERAL_EQ:
      case BinaryOperationKind::GENERAL_NE:
      case BinaryOperationKind::GENERAL_LT:
      case BinaryOperationKind::GENERAL_LE:
      case BinaryOperationKind::GENERAL_GT:
      case BinaryOperationKind::GENERAL_GE:
      case BinaryOperationKind::VALUE_EQ:
      case BinaryOperationKind::VALUE_NE:
      case BinaryOperationKind::VALUE_LT:
      case BinaryOperationKind::VALUE_LE:
      case BinaryOperationKind::VALUE_GT:
      case BinaryOperationKind::VALUE_GE:
         return handle_binary_comparison(Node, left_node, right_node, CurrentPrefix, op_kind);

      case BinaryOperationKind::ADD:
      case BinaryOperationKind::SUB:
      case BinaryOperationKind::MUL:
      case BinaryOperationKind::DIV:
      case BinaryOperationKind::MOD:
         return handle_binary_arithmetic(Node, left_node, right_node, CurrentPrefix, op_kind);

      default:
         expression_unsupported = true;
         return XPathVal();
   }
}

//********************************************************************************************************************
// Evaluates logical binary operators (and/or) and returns the computed value.
//
// **Preconditions**:
// - Node and operands must be non-null.
// - OpKind must be BinaryOperationKind::AND or BinaryOperationKind::OR.
//
// **Behaviour**:
// - Evaluates operands lazily to respect short-circuit semantics.
// - Converts each operand to effective boolean value before combining results.
//
// **Returns**:
// - On success: XPathVal containing boolean true/false.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `$a and $b`, `$a or $b`.
//
// **Related**:
// - XQuery spec section 3.5.1.
// - Called from: handle_binary_op().
// - May recursively call: evaluate_expression() for operand nodes.

XPathVal XPathEvaluator::handle_binary_logical(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
   uint32_t CurrentPrefix, BinaryOperationKind OpKind)
{
   (void)Node;

   auto left_value = evaluate_expression(Left, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   bool left_boolean = left_value.to_boolean();
   if (OpKind IS BinaryOperationKind::AND) {
      if (not left_boolean) return XPathVal(false);
      auto right_value = evaluate_expression(Right, CurrentPrefix);
      if (expression_unsupported) return XPathVal();
      bool right_boolean = right_value.to_boolean();
      return XPathVal(right_boolean);
   }

   if (left_boolean) return XPathVal(true);

   auto right_value = evaluate_expression(Right, CurrentPrefix);
   if (expression_unsupported) return XPathVal();
   bool right_boolean = right_value.to_boolean();
   return XPathVal(right_boolean);
}

//********************************************************************************************************************
// Evaluates set-combining binary operators and returns the computed value.
//
// **Preconditions**:
// - Node and operands must be non-null.
// - OpKind must be BinaryOperationKind::UNION, ::INTERSECT, or ::EXCEPT.
//
// **Behaviour**:
// - Evaluates both operands to obtain node sequences.
// - Delegates to evaluate_union_value(), evaluate_intersect_value(), or evaluate_except_value().
// - Preserves document order where mandated by the specification.
//
// **Returns**:
// - On success: XPathVal with the resulting node-set.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `$a union $b`, `$a intersect $b`, `$a except $b`.
//
// **Related**:
// - XQuery spec section 3.3.1.
// - Called from: handle_binary_op().
// - May recursively call: evaluate_expression() for operands.

XPathVal XPathEvaluator::handle_binary_set_ops(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
   uint32_t CurrentPrefix, BinaryOperationKind OpKind)
{
   switch (OpKind) {
      case BinaryOperationKind::UNION: {
         std::vector<const XPathNode *> branches;
         branches.reserve(2);
         if (Left) branches.push_back(Left);
         if (Right) branches.push_back(Right);
         return evaluate_union_value(branches, CurrentPrefix);
      }
      case BinaryOperationKind::INTERSECT:
         return evaluate_intersect_value(Left, Right, CurrentPrefix);

      case BinaryOperationKind::EXCEPT:
         return evaluate_except_value(Left, Right, CurrentPrefix);

      default:
         expression_unsupported = true;
         return XPathVal();
   }
}

//********************************************************************************************************************
// Evaluates sequence-concatenation and range binary operators and returns the computed value.
//
// **Preconditions**:
// - Node and operands must be non-null.
// - OpKind must be BinaryOperationKind::COMMA or BinaryOperationKind::RANGE.
//
// **Behaviour**:
// - Evaluates both operands, appending their contents in document order.
// - Expands range expressions by generating numeric sequences where required.
//
// **Returns**:
// - On success: XPathVal representing the concatenated or expanded sequence.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `(1, 2, 3)` via comma chaining, `1 to 5` via range expansion.
//
// **Related**:
// - XQuery spec section 3.3.1.
// - Called from: handle_binary_op().
// - May recursively call: evaluate_expression() for operands.

XPathVal XPathEvaluator::handle_binary_sequence(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
   uint32_t CurrentPrefix, BinaryOperationKind OpKind)
{
   if (OpKind IS BinaryOperationKind::COMMA) {
      auto left_value = evaluate_expression(Left, CurrentPrefix);
      if (expression_unsupported) return XPathVal();
      auto right_value = evaluate_expression(Right, CurrentPrefix);
      if (expression_unsupported) return XPathVal();

      std::vector<SequenceEntry> entries;
      entries.reserve(left_value.node_set.size() + right_value.node_set.size());

      append_value_to_sequence(left_value, entries, next_constructed_node_id, constructed_nodes);
      append_value_to_sequence(right_value, entries, next_constructed_node_id, constructed_nodes);

      XPathVal result = nodeset_from_sequence_entries(entries);
      if (not prolog_ordering_is_ordered()) result.preserve_node_order = true;
      return result;
   }

   auto left_value = evaluate_expression(Left, CurrentPrefix);
   if (expression_unsupported) return XPathVal();
   auto right_value = evaluate_expression(Right, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   size_t start_count = sequence_item_count(left_value);
   if (start_count IS 0) {
      record_error("XPTY0004: Range start requires a single numeric value, but the operand was empty.", Node, true);
      return XPathVal();
   }
   if (start_count > 1) {
      auto message = std::format(
         "XPTY0004: Range start requires a single numeric value, but the operand had {} items.", start_count);
      record_error(message, Node, true);
      return XPathVal();
   }

   size_t end_count = sequence_item_count(right_value);
   if (end_count IS 0) {
      record_error("XPTY0004: Range end requires a single numeric value, but the operand was empty.", Node, true);
      return XPathVal();
   }
   if (end_count > 1) {
      auto message = std::format(
         "XPTY0004: Range end requires a single numeric value, but the operand had {} items.", end_count);
      record_error(message, Node, true);
      return XPathVal();
   }

   double start_numeric = left_value.to_number();
   double end_numeric = right_value.to_number();

   if (not std::isfinite(start_numeric) or not std::isfinite(end_numeric)) {
      record_error("XPTY0004: Range boundaries must be finite numeric values.", Node, true);
      return XPathVal();
   }

   double start_integral = 0.0;
   double end_integral = 0.0;
   double start_fraction = std::modf(start_numeric, &start_integral);
   double end_fraction = std::modf(end_numeric, &end_integral);

   if (not (start_fraction IS 0.0)) {
      auto lexical = left_value.to_string();
      auto message = std::format(
         "XPTY0004: Range start value '{}' is not an integer.", lexical);
      record_error(message, Node, true);
      return XPathVal();
   }

   if (not (end_fraction IS 0.0)) {
      auto lexical = right_value.to_string();
      auto message = std::format(
         "XPTY0004: Range end value '{}' is not an integer.", lexical);
      record_error(message, Node, true);
      return XPathVal();
   }

   if ((start_integral < (double)std::numeric_limits<int64_t>::min()) or
       (start_integral > (double)std::numeric_limits<int64_t>::max()) or
       (end_integral < (double)std::numeric_limits<int64_t>::min()) or
       (end_integral > (double)std::numeric_limits<int64_t>::max())) {
      record_error("FOAR0002: Range boundaries fall outside supported integer limits.", Node, true);
      return XPathVal();
   }

   int64_t start_int = (int64_t)start_integral;
   int64_t end_int = (int64_t)end_integral;

   if (start_int > end_int) {
      NODES empty_nodes;
      XPathVal empty_result(empty_nodes);
      empty_result.preserve_node_order = true;
      return empty_result;
   }

   uint64_t length_u64 = 0;
   (void)compute_range_length_s64(start_int, end_int, length_u64);
   if ((length_u64 IS 0) or (length_u64 > (uint64_t)RANGE_ITEM_LIMIT)) {
      auto start_lexical = format_xpath_number(start_numeric);
      auto end_lexical = format_xpath_number(end_numeric);
      auto length_string = std::to_string(length_u64);
      auto message = std::format(
         "FOAR0002: Range from {} to {} produces {} items which exceeds the supported limit of {}.",
         start_lexical, end_lexical, length_string, RANGE_ITEM_LIMIT);
      record_error(message, Node, true);
      return XPathVal();
   }

   int64_t length = (int64_t)length_u64;

   NODES range_nodes;
   range_nodes.reserve((size_t)length);
   std::vector<std::string> range_strings;
   range_strings.reserve((size_t)length);

   for (int64_t value = start_int; value <= end_int; ++value) {
      range_nodes.push_back(nullptr);
      range_strings.push_back(format_xpath_number((double)value));
   }

   XPathVal range_result;
   range_result.Type = XPVT::NodeSet;
   range_result.preserve_node_order = true;
   range_result.node_set = std::move(range_nodes);
   range_result.node_set_string_values = std::move(range_strings);
   range_result.node_set_attributes.clear();
   range_result.node_set_string_override.reset();
   return range_result;
}

//********************************************************************************************************************
// Materialises a vector of sequence entries into a runtime node-set value.

XPathVal XPathEvaluator::nodeset_from_sequence_entries(const std::vector<SequenceEntry> &Entries)
{
   if (Entries.empty()) {
      return XPathVal(pf::vector<XTag *>{});
   }

   NODES combined_nodes;
   combined_nodes.reserve(Entries.size());
   std::vector<const XMLAttrib *> combined_attributes;
   combined_attributes.reserve(Entries.size());
   std::vector<std::string> combined_strings;
   combined_strings.reserve(Entries.size());
   std::vector<std::shared_ptr<XPathValue>> combined_composites;
   combined_composites.reserve(Entries.size());

   for (const auto &entry : Entries) {
      combined_nodes.push_back(entry.node);
      combined_attributes.push_back(entry.attribute);
      combined_strings.push_back(entry.string_value);
      combined_composites.push_back(entry.composite_value);
   }

   auto result = XPathVal(combined_nodes, std::nullopt, std::move(combined_strings), std::move(combined_attributes));
   result.node_set_composite_values = std::move(combined_composites);
   result.preserve_node_order = false;
   return result;
}

//********************************************************************************************************************
// Converts a stored XPathValueSequence back into a runtime value, preserving sequence semantics.

XPathVal XPathEvaluator::materialise_sequence_value(const XPathValueSequence &Sequence)
{
   if (Sequence.items.empty()) {
      return XPathVal(pf::vector<XTag *>{});
   }

   if (Sequence.items.size() IS 1) {
      return clone_xpath_value(Sequence.items.front());
   }

   std::vector<SequenceEntry> entries;
   entries.reserve(Sequence.items.size());
   for (const auto &stored : Sequence.items) {
      XPathVal restored = clone_xpath_value(stored);
      append_value_to_sequence(restored, entries, next_constructed_node_id, constructed_nodes);
   }

   return nodeset_from_sequence_entries(entries);
}

//********************************************************************************************************************
// Concatenates multiple XPath values into a single sequence, used for lookup wildcards.

XPathVal XPathEvaluator::concatenate_sequence_values(const std::vector<XPathVal> &Values)
{
   if (Values.empty()) {
      return XPathVal(pf::vector<XTag *>{});
   }

   if (Values.size() IS 1) {
      return Values.front();
   }

   std::vector<SequenceEntry> entries;
   for (const auto &value : Values) {
      append_value_to_sequence(value, entries, next_constructed_node_id, constructed_nodes);
   }

   return nodeset_from_sequence_entries(entries);
}

//********************************************************************************************************************
// Evaluates chained lookup operators against the supplied base expression.

XPathVal XPathEvaluator::handle_lookup_expression(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (!Node) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto *base = Node->get_child_safe(0);
   if (!base) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto current_value = evaluate_expression(base, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   for (size_t index = 0; index < Node->lookup_specifier_count(); ++index) {
      const auto *specifier = Node->get_lookup_specifier(index);
      if (!specifier) {
         expression_unsupported = true;
         return XPathVal();
      }

      current_value = apply_lookup_to_value(current_value, *specifier, CurrentPrefix, Node);
      if (expression_unsupported) return XPathVal();
   }

   return current_value;
}

//********************************************************************************************************************
// Applies a single lookup specifier to the provided value, dispatching by runtime type.

XPathVal XPathEvaluator::apply_lookup_to_value(const XPathVal &BaseValue, const XPathLookupSpecifier &Specifier,
   uint32_t CurrentPrefix, const XPathNode *ContextNode)
{
   switch (BaseValue.Type) {
      case XPVT::Map:
         return lookup_map_value(BaseValue, Specifier, CurrentPrefix, ContextNode);

      case XPVT::Array:
         return lookup_array_value(BaseValue, Specifier, CurrentPrefix, ContextNode);

      case XPVT::NodeSet:
         return lookup_nodeset_value(BaseValue, Specifier, CurrentPrefix, ContextNode);

      default: {
         auto detail = std::format("Lookup operator cannot be applied to {} values.",
            describe_value_type(BaseValue));
         auto message = xquery::errors::json_not_schema_aware(detail);
         record_error(message, ContextNode, true);
         expression_unsupported = true;
         return XPathVal();
      }
   }
}

//********************************************************************************************************************
// Performs lookup semantics for map values, supporting literal, expression, and wildcard keys.

XPathVal XPathEvaluator::lookup_map_value(const XPathVal &BaseValue, const XPathLookupSpecifier &Specifier,
   uint32_t CurrentPrefix, const XPathNode *ContextNode)
{
   auto storage = BaseValue.map_storage;
   if (!storage) return XPathVal(pf::vector<XTag *>{});

   if (Specifier.kind IS XPathLookupSpecifierKind::Wildcard) {
      std::vector<XPathVal> concatenated;
      concatenated.reserve(storage->entries.size());
      for (const auto &entry : storage->entries) {
         concatenated.push_back(materialise_sequence_value(entry.value));
      }
      return concatenate_sequence_values(concatenated);
   }

   std::string lookup_key;
   if ((Specifier.kind IS XPathLookupSpecifierKind::NCName) or (Specifier.kind IS XPathLookupSpecifierKind::IntegerLiteral)) {
      lookup_key = Specifier.literal_value;
   }
   else if (Specifier.kind IS XPathLookupSpecifierKind::Expression) {
      if (!Specifier.expression) {
         expression_unsupported = true;
         return XPathVal();
      }

      auto key_value = evaluate_expression(Specifier.expression.get(), CurrentPrefix);
      if (expression_unsupported) return XPathVal();

      auto atomised_key = atomize_singleton(*this, key_value, Specifier.expression.get(), "Lookup expression");
      if (!atomised_key.has_value()) return XPathVal();
      lookup_key = atomised_key->to_string();
   }
   else {
      record_error("XPTY0004: Map lookup requires an NCName, integer, or expression key.", ContextNode, true);
      expression_unsupported = true;
      return XPathVal();
   }

   for (const auto &entry : storage->entries) {
      if (entry.key IS lookup_key) {
         return materialise_sequence_value(entry.value);
      }
   }

   return XPathVal(pf::vector<XTag *>{});
}

//********************************************************************************************************************
// Performs lookup semantics for array values including wildcard expansion.

XPathVal XPathEvaluator::lookup_array_value(const XPathVal &BaseValue, const XPathLookupSpecifier &Specifier,
   uint32_t CurrentPrefix, const XPathNode *ContextNode)
{
   auto storage = BaseValue.array_storage;
   if (!storage) return XPathVal(pf::vector<XTag *>{});

   if (Specifier.kind IS XPathLookupSpecifierKind::Wildcard) {
      std::vector<XPathVal> concatenated;
      concatenated.reserve(storage->members.size());
      for (const auto &member : storage->members) {
         concatenated.push_back(materialise_sequence_value(member));
      }
      return concatenate_sequence_values(concatenated);
   }

   std::optional<long long> resolved_index;
   if (Specifier.kind IS XPathLookupSpecifierKind::IntegerLiteral) {
      resolved_index = resolve_lookup_integer(Specifier.literal_value, *this, ContextNode, "Array lookup index");
   }
   else if (Specifier.kind IS XPathLookupSpecifierKind::Expression) {
      if (!Specifier.expression) {
         expression_unsupported = true;
         return XPathVal();
      }

      auto key_value = evaluate_expression(Specifier.expression.get(), CurrentPrefix);
      if (expression_unsupported) return XPathVal();

      auto atomised = atomize_singleton(*this, key_value, Specifier.expression.get(), "Array lookup index");
      if (!atomised.has_value()) return XPathVal();
      resolved_index = resolve_lookup_integer(atomised->to_string(), *this, Specifier.expression.get(), "Array lookup index");
   }
   else {
      record_error("XPTY0004: Array lookup requires an integer key or '*'.", ContextNode, true);
      expression_unsupported = true;
      return XPathVal();
   }

   if (!resolved_index.has_value()) return XPathVal();

   long long index_one_based = *resolved_index;
   if ((index_one_based < 1) or ((size_t)index_one_based > storage->members.size())) {
      auto detail = std::format("Array index {} is out of range.", index_one_based);
      auto message = xquery::errors::array_index_out_of_bounds(detail);
      record_error(message, ContextNode, true);
      expression_unsupported = true;
      return XPathVal();
   }

   size_t zero_based = (size_t)(index_one_based - 1);
   return materialise_sequence_value(storage->members[zero_based]);
}

//********************************************************************************************************************
// Performs lookup semantics for node sequences by selecting matching attributes or child elements.

XPathVal XPathEvaluator::lookup_nodeset_value(const XPathVal &BaseValue, const XPathLookupSpecifier &Specifier,
   uint32_t CurrentPrefix, const XPathNode *ContextNode)
{
   if ((Specifier.kind != XPathLookupSpecifierKind::NCName) and (Specifier.kind != XPathLookupSpecifierKind::Wildcard)) {
      record_error("XPTY0004: Node lookup supports only NCName or '*' keys.", ContextNode, true);
      expression_unsupported = true;
      return XPathVal();
   }

   bool wildcard = (Specifier.kind IS XPathLookupSpecifierKind::Wildcard);
   std::string target = wildcard ? std::string() : Specifier.literal_value;

   std::vector<SequenceEntry> matched_entries;
   matched_entries.reserve(BaseValue.node_set.size());

   size_t item_count = sequence_item_count(BaseValue);
   for (size_t index = 0; index < item_count; ++index) {
      std::shared_ptr<XPathValue> composite;
      if (index < BaseValue.node_set_composite_values.size()) {
         composite = BaseValue.node_set_composite_values[index];
      }

      if (composite) {
         XPathVal composite_value = clone_xpath_value(*composite);
         XPathVal composite_result;
         if (composite_value.Type IS XPVT::Map) {
            composite_result = lookup_map_value(composite_value, Specifier, CurrentPrefix, ContextNode);
         }
         else if (composite_value.Type IS XPVT::Array) {
            composite_result = lookup_array_value(composite_value, Specifier, CurrentPrefix, ContextNode);
         }
         else {
            continue;
         }

         if (expression_unsupported) return XPathVal();
         append_value_to_sequence(composite_result, matched_entries, next_constructed_node_id, constructed_nodes);
         continue;
      }

      const XMLAttrib *attribute = (index < BaseValue.node_set_attributes.size()) ?
         BaseValue.node_set_attributes[index] : nullptr;
      XTag *node = (index < BaseValue.node_set.size()) ? BaseValue.node_set[index] : nullptr;
      if (attribute) continue; // Attribute nodes do not support lookup on their own
      if (!node) continue;

      auto collect_attribute = [&](XMLAttrib &Attrib) {
         SequenceEntry entry;
         entry.node = node;
         entry.attribute = &Attrib;
         entry.string_value = Attrib.Value;
         matched_entries.push_back(std::move(entry));
      };

      auto collect_child = [&](XTag &Child) {
         SequenceEntry entry;
         entry.node = &Child;
         entry.attribute = nullptr;
         entry.string_value = XPathVal::node_string_value(&Child);
         matched_entries.push_back(std::move(entry));
      };

      for (size_t attr_index = 1; attr_index < node->Attribs.size(); ++attr_index) {
         auto &attrib = node->Attribs[attr_index];
         if (!wildcard and (attrib.Name != target)) continue;
         collect_attribute(attrib);
      }

      for (auto &child : node->Children) {
         if (!child.isTag()) continue;
         if (!wildcard) {
            if (child.Attribs.empty()) continue;
            if (child.Attribs[0].Name != target) continue;
         }
         collect_child(child);
      }
   }

   if (matched_entries.empty()) return XPathVal(pf::vector<XTag *>{});

   return nodeset_from_sequence_entries(matched_entries);
}


//********************************************************************************************************************
// Evaluates arithmetic binary operators and returns the computed value.
//
// **Preconditions**:
// - Node and operands must be non-null.
// - OpKind must represent one of the arithmetic or concatenation operations.
//
// **Behaviour**:
// - Evaluates operands and normalises them to numeric or string types as required.
// - Applies checked arithmetic helpers for overflow-sensitive calculations.
// - Implements string concatenation for BinaryOperationKind::CONCAT.
//
// **Returns**:
// - On success: XPathVal containing the numeric or string result.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `$a + $b`, `$a div $b`, `$lhs || $rhs`.
//
// **Related**:
// - XQuery spec section 3.4.
// - Called from: handle_binary_op().
// - May recursively call: evaluate_expression() for operands.

XPathVal XPathEvaluator::handle_binary_arithmetic(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
   uint32_t CurrentPrefix, BinaryOperationKind OpKind)
{
   (void)Node;

   auto left_value = evaluate_expression(Left, CurrentPrefix);
   if (expression_unsupported) return XPathVal();
   auto right_value = evaluate_expression(Right, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   switch (OpKind) {
      case BinaryOperationKind::ADD: {
         double result = left_value.to_number() + right_value.to_number();
         return XPathVal(result);
      }
      case BinaryOperationKind::SUB: {
         double result = left_value.to_number() - right_value.to_number();
         return XPathVal(result);
      }
      case BinaryOperationKind::MUL: {
         double result = left_value.to_number() * right_value.to_number();
         return XPathVal(result);
      }
      case BinaryOperationKind::DIV: {
         double result = left_value.to_number() / right_value.to_number();
         return XPathVal(result);
      }
      case BinaryOperationKind::MOD: {
         double left_number = left_value.to_number();
         double right_number = right_value.to_number();
         double result = std::fmod(left_number, right_number);
         return XPathVal(result);
      }
      default:
         expression_unsupported = true;
         return XPathVal();
   }
}

//********************************************************************************************************************
// Evaluates comparison binary operators and returns the computed value.
//
// **Preconditions**:
// - Node and operands must be non-null.
// - OpKind must correspond to a value or general comparison.
//
// **Behaviour**:
// - Evaluates operands and applies general or value comparisons depending on OpKind.
// - Delegates to compare_xpath_relational() and helper utilities for implementation.
// - Handles mixed node/scalar comparisons and cardinality rules from the specification.
//
// **Returns**:
// - On success: XPathVal containing boolean true/false.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `$a = $b`, `$a lt $b`, `$a eq $b`.
//
// **Related**:
// - XQuery spec section 3.5.
// - Called from: handle_binary_op().
// - May recursively call: evaluate_expression() for operands.

XPathVal XPathEvaluator::handle_binary_comparison(const XPathNode *Node, const XPathNode *Left, const XPathNode *Right,
   uint32_t CurrentPrefix, BinaryOperationKind OpKind)
{
   (void)Node;

   auto left_value = evaluate_expression(Left, CurrentPrefix);
   if (expression_unsupported) return XPathVal();
   auto right_value = evaluate_expression(Right, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   switch (OpKind) {
      case BinaryOperationKind::GENERAL_EQ: {
         bool equals = compare_xpath_values(left_value, right_value);
         return XPathVal(equals);
      }
      case BinaryOperationKind::GENERAL_NE: {
         bool equals = compare_xpath_values(left_value, right_value);
         return XPathVal(not equals);
      }
      case BinaryOperationKind::GENERAL_LT: {
         bool result = compare_xpath_relational(left_value, right_value, RelationalOperator::LESS);
         return XPathVal(result);
      }
      case BinaryOperationKind::GENERAL_LE: {
         bool result = compare_xpath_relational(left_value, right_value, RelationalOperator::LESS_OR_EQUAL);
         return XPathVal(result);
      }
      case BinaryOperationKind::GENERAL_GT: {
         bool result = compare_xpath_relational(left_value, right_value, RelationalOperator::GREATER);
         return XPathVal(result);
      }
      case BinaryOperationKind::GENERAL_GE: {
         bool result = compare_xpath_relational(left_value, right_value, RelationalOperator::GREATER_OR_EQUAL);
         return XPathVal(result);
      }
      case BinaryOperationKind::VALUE_EQ: {
         auto left_scalar = promote_value_comparison_operand(left_value);
         auto right_scalar = promote_value_comparison_operand(right_value);
         if (not left_scalar.has_value() or not right_scalar.has_value()) return XPathVal(false);
         bool equals = compare_xpath_values(*left_scalar, *right_scalar);
         return XPathVal(equals);
      }
      case BinaryOperationKind::VALUE_NE: {
         auto left_scalar = promote_value_comparison_operand(left_value);
         auto right_scalar = promote_value_comparison_operand(right_value);
         if (not left_scalar.has_value() or not right_scalar.has_value()) return XPathVal(false);
         bool equals = compare_xpath_values(*left_scalar, *right_scalar);
         return XPathVal(not equals);
      }
      case BinaryOperationKind::VALUE_LT: {
         auto left_scalar = promote_value_comparison_operand(left_value);
         auto right_scalar = promote_value_comparison_operand(right_value);
         if (not left_scalar.has_value() or not right_scalar.has_value()) return XPathVal(false);
         bool result = compare_xpath_relational(*left_scalar, *right_scalar, RelationalOperator::LESS);
         return XPathVal(result);
      }
      case BinaryOperationKind::VALUE_LE: {
         auto left_scalar = promote_value_comparison_operand(left_value);
         auto right_scalar = promote_value_comparison_operand(right_value);
         if (not left_scalar.has_value() or not right_scalar.has_value()) return XPathVal(false);
         bool result = compare_xpath_relational(*left_scalar, *right_scalar, RelationalOperator::LESS_OR_EQUAL);
         return XPathVal(result);
      }
      case BinaryOperationKind::VALUE_GT: {
         auto left_scalar = promote_value_comparison_operand(left_value);
         auto right_scalar = promote_value_comparison_operand(right_value);
         if (not left_scalar.has_value() or not right_scalar.has_value()) return XPathVal(false);
         bool result = compare_xpath_relational(*left_scalar, *right_scalar, RelationalOperator::GREATER);
         return XPathVal(result);
      }
      case BinaryOperationKind::VALUE_GE: {
         auto left_scalar = promote_value_comparison_operand(left_value);
         auto right_scalar = promote_value_comparison_operand(right_value);
         if (not left_scalar.has_value() or not right_scalar.has_value()) return XPathVal(false);
         bool result = compare_xpath_relational(*left_scalar, *right_scalar, RelationalOperator::GREATER_OR_EQUAL);
         return XPathVal(result);
      }
      default:
         expression_unsupported = true;
         return XPathVal();
   }
}

//********************************************************************************************************************
// Evaluates a UNARY_OP node and returns the computed value.
//
// Unary operators support numeric negation and logical not.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::UNARY_OP.
// - Node must provide one operand child.
//
// **Behaviour**:
// - Evaluates the operand before applying the requested unary operator.
// - Uses cached operator metadata when available before falling back to runtime string mapping.
// - Uses numeric_promote() for numeric negation and boolean conversion for logical not.
//
// **Returns**:
// - On success: XPathVal containing the transformed value.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `- $value`, `not($predicate)`.
//
// **Related**:
// - XQuery spec section 3.4.1.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for the operand.

XPathVal XPathEvaluator::handle_unary_op(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (Node->child_count() IS 0) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto *operand_node = Node->get_child_safe(0);
   if (not operand_node) {
      expression_unsupported = true;
      return XPathVal();
   }

   auto operand = evaluate_expression(operand_node, CurrentPrefix);
   if (expression_unsupported) return XPathVal();

   auto operation = Node->get_value_view();
   UnaryOperationKind op_kind;

   if (auto cached_kind = Node->get_cached_unary_kind(); cached_kind.has_value()) {
      op_kind = *cached_kind;
   }
   else {
      op_kind = map_unary_operation(operation);
      unary_operator_cache_fallbacks++;
      if (is_trace_enabled()) {
         pf::Log log("XPath");
         log.msg(VLF::TRACE, "Unary operator cache miss for '%.*s'", (int)operation.size(), operation.data());
      }
   }

   switch (op_kind) {
      case UnaryOperationKind::NEGATE:
         return XPathVal(-operand.to_number());
      case UnaryOperationKind::LOGICAL_NOT:
         return XPathVal(not operand.to_boolean());
      default:
         expression_unsupported = true;
         return XPathVal();
   }
}

//********************************************************************************************************************
// Evaluates an EXPRESSION wrapper node and returns the computed value.
//
// Wrapper nodes encapsulate a single child expression introduced during parsing.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::EXPRESSION.
// - Node must supply at least one child expression.
//
// **Behaviour**:
// - Evaluates the first child expression directly.
// - Propagates `expression_unsupported` when the child evaluation fails.
//
// **Returns**:
// - On success: XPathVal produced by the child expression.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - Parenthesised sub-expressions such as `(1 + 2)`.
//
// **Related**:
// - XQuery spec section 3.3.1.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for the child node.

XPathVal XPathEvaluator::handle_expression_wrapper(const XPathNode *Node, uint32_t CurrentPrefix)
{
   if (auto *child = Node->get_child_safe(0)) return evaluate_expression(child, CurrentPrefix);
   expression_unsupported = true;
   return XPathVal();
}

//********************************************************************************************************************
// Evaluates a VARIABLE_REFERENCE node and returns the computed value.
//
// Variable reference nodes resolve variables declared in the static or dynamic context.
//
// **Preconditions**:
// - Node must be non-null.
// - Node type must match XQueryNodeType::VARIABLE_REFERENCE.
// - Node value must contain the QName of the referenced variable.
//
// **Behaviour**:
// - Resolves the variable via resolve_variable_value().
// - Evaluates deferred initialisers when a variable has an inline expression.
// - Records dynamic errors if the variable is not defined.
//
// **Returns**:
// - On success: XPathVal bound to the variable.
// - On failure: Empty XPathVal with `expression_unsupported` set.
//
// **Examples**:
// - `$price` inside FLWOR expressions.
//
// **Related**:
// - XQuery spec section 2.1.1.
// - Called from: evaluate_expression().
// - May recursively call: evaluate_expression() for deferred initialisers.

XPathVal XPathEvaluator::handle_variable_reference(const XPathNode *Node, uint32_t CurrentPrefix)
{
   XPathVal resolved_value;
   auto variable_name = Node->get_value_view();
   if (resolve_variable_value(variable_name, CurrentPrefix, resolved_value, Node)) {
      return resolved_value;
   }

   pf::Log log("XPath");

   if (is_trace_enabled()) {
      log.msg(VLF::TRACE, "Variable lookup failed for '%s'", Node->value.c_str());
      if (context.variables and context.variables->empty() IS false) {
         std::string binding_list;
         auto it = context.variables->begin();
         if (it != context.variables->end()) {
            binding_list = it->first;
            ++it;
            for (; it != context.variables->end(); ++it) {
               binding_list.append(", ");
               binding_list.append(it->first);
            }
         }
         log.msg(VLF::TRACE, "Context bindings available: [%s]", binding_list.c_str());
      }
   }

   expression_unsupported = true;
   return XPathVal();
}

void XPathEvaluator::reset_dispatch_metrics()
{
   node_dispatch_counters.fill(0);
   binary_operator_cache_fallbacks = 0;
   unary_operator_cache_fallbacks = 0;
}

const std::array<uint64_t, 64> &XPathEvaluator::dispatch_metrics() const
{
   return node_dispatch_counters;
}

void XPathEvaluator::record_dispatch_node(XQueryNodeType Type)
{
   size_t index = (size_t)Type;
   if (index < node_dispatch_counters.size()) node_dispatch_counters[index]++;
}

//********************************************************************************************************************
// Evaluates an XPath/XQuery expression node and returns its computed value.  Responsibilities:
//
// - Dispatches on node kind (numbers, literals, constructors, paths, predicates, and control flow).
// - Preserves XPath semantics such as document order, short-circuiting (and/or), and context-sensitive
//   evaluation for filters, paths, and quantified/for expressions.
// - Integrates XQuery Prolog settings (ordering, construction, namespaces) and consults the module cache
//   when user-defined functions or variables require module resolution.
// - Uses push_context/pop_context to manage the evaluation context for node-set operations and predicates.
// - Signals unsupported constructs via 'expression_unsupported' and reports diagnostics with record_error().
// - Produces results as XPathVal, including node-set values with associated attribute/string metadata.
//
// Notes:
// - Function is side-effect free for input XML; constructed text nodes are owned by 'constructed_nodes'.
// - Returns empty values on failure paths; callers must check 'expression_unsupported' when necessary.

static const ankerl::unordered_dense::map<XQueryNodeType, XPathEvaluator::NodeEvaluationHandler> NODE_HANDLERS = {
   {XQueryNodeType::EMPTY_SEQUENCE, &XPathEvaluator::handle_empty_sequence},
   {XQueryNodeType::NUMBER, &XPathEvaluator::handle_number},
   {XQueryNodeType::LITERAL, &XPathEvaluator::handle_literal},
   {XQueryNodeType::STRING, &XPathEvaluator::handle_literal},
   {XQueryNodeType::MAP_CONSTRUCTOR, &XPathEvaluator::handle_map_constructor},
   {XQueryNodeType::ARRAY_CONSTRUCTOR, &XPathEvaluator::handle_array_constructor},
   {XQueryNodeType::LOOKUP_EXPRESSION, &XPathEvaluator::handle_lookup_expression},
   {XQueryNodeType::CAST_EXPRESSION, &XPathEvaluator::handle_cast_expression},
   {XQueryNodeType::TREAT_AS_EXPRESSION, &XPathEvaluator::handle_treat_as_expression},
   {XQueryNodeType::INSTANCE_OF_EXPRESSION, &XPathEvaluator::handle_instance_of_expression},
   {XQueryNodeType::CASTABLE_EXPRESSION, &XPathEvaluator::handle_castable_expression},
   {XQueryNodeType::TYPESWITCH_EXPRESSION, &XPathEvaluator::handle_typeswitch_expression},
   {XQueryNodeType::DIRECT_ELEMENT_CONSTRUCTOR, &XPathEvaluator::evaluate_direct_element_constructor},
   {XQueryNodeType::COMPUTED_ELEMENT_CONSTRUCTOR, &XPathEvaluator::evaluate_computed_element_constructor},
   {XQueryNodeType::COMPUTED_ATTRIBUTE_CONSTRUCTOR, &XPathEvaluator::evaluate_computed_attribute_constructor},
   {XQueryNodeType::TEXT_CONSTRUCTOR, &XPathEvaluator::evaluate_text_constructor},
   {XQueryNodeType::COMMENT_CONSTRUCTOR, &XPathEvaluator::evaluate_comment_constructor},
   {XQueryNodeType::PI_CONSTRUCTOR, &XPathEvaluator::evaluate_pi_constructor},
   {XQueryNodeType::DOCUMENT_CONSTRUCTOR, &XPathEvaluator::evaluate_document_constructor},
   {XQueryNodeType::LOCATION_PATH, &XPathEvaluator::evaluate_path_expression_value},
   {XQueryNodeType::PATH, &XPathEvaluator::handle_path},
   {XQueryNodeType::UNION, &XPathEvaluator::handle_union_node},
   {XQueryNodeType::FUNCTION_CALL, &XPathEvaluator::evaluate_function_call},
   {XQueryNodeType::CONDITIONAL, &XPathEvaluator::handle_conditional},
   {XQueryNodeType::LET_EXPRESSION, &XPathEvaluator::handle_let_expression},
   {XQueryNodeType::FOR_EXPRESSION, &XPathEvaluator::handle_for_expression},
   {XQueryNodeType::QUANTIFIED_EXPRESSION, &XPathEvaluator::handle_quantified_expression},
   {XQueryNodeType::FILTER, &XPathEvaluator::handle_filter},
   {XQueryNodeType::UNARY_OP, &XPathEvaluator::handle_unary_op},
   {XQueryNodeType::BINARY_OP, &XPathEvaluator::handle_binary_op},
   {XQueryNodeType::EXPRESSION, &XPathEvaluator::handle_expression_wrapper},
   {XQueryNodeType::VARIABLE_REFERENCE, &XPathEvaluator::handle_variable_reference},
   {XQueryNodeType::FLWOR_EXPRESSION, &XPathEvaluator::evaluate_flwor_pipeline}
};

XPathVal XPathEvaluator::evaluate_expression(const XPathNode *ExprNode, uint32_t CurrentPrefix)
{
   if (not ExprNode) {
      record_error("Unsupported XPath expression: empty node", (const XPathNode *)nullptr, true);
      return XPathVal();
   }

   record_dispatch_node(ExprNode->type);

   switch (ExprNode->type) {
      case XQueryNodeType::BINARY_OP: [[likely]]
         return handle_binary_op(ExprNode, CurrentPrefix);

      case XQueryNodeType::UNARY_OP: [[likely]]
         return handle_unary_op(ExprNode, CurrentPrefix);

      case XQueryNodeType::VARIABLE_REFERENCE: [[likely]]
         return handle_variable_reference(ExprNode, CurrentPrefix);

      case XQueryNodeType::FUNCTION_CALL: [[likely]]
         return evaluate_function_call(ExprNode, CurrentPrefix);

      case XQueryNodeType::NUMBER:
         return handle_number(ExprNode, CurrentPrefix);

      case XQueryNodeType::LITERAL:
      case XQueryNodeType::STRING:
         return handle_literal(ExprNode, CurrentPrefix);

      default:
         break;
   }

   auto handler_it = NODE_HANDLERS.find(ExprNode->type);
   if (handler_it != NODE_HANDLERS.end()) {
      return (this->*(handler_it->second))(ExprNode, CurrentPrefix);
   }

   if (is_trace_enabled()) {
      pf::Log log("XPath");
      log.msg(VLF::TRACE, "Unsupported expression node type: %d", int(ExprNode->type));
   }

   expression_unsupported = true;
   return XPathVal();
}
