//********************************************************************************************************************
// XPath Value System Implementation
//
// Implements the XPathVal class and value conversion functions for XPath evaluation.  This file provides the core
// value model used throughout XPath processing, handling conversions between node-sets, strings, numbers, and
// booleans according to XPath 2.0 specifications.
//
// Key functionality:
//   - Type conversion methods (to_boolean, to_number, to_string, to_node_set)
//   - Node-set string value extraction
//   - Numeric conversion with NaN and infinity handling
//   - Schema type integration and validation
//   - String normalisation and formatting for XPath numeric output
//
// The implementation ensures consistent type coercion semantics across all XPath operations, integrating with
// the schema type system for enhanced type awareness in XPath 2.0 expressions.

#include "xpath_value.h"

#include <kotuku/modules/xml.h>

#include "../xml/schema/schema_types.h"
#include "../xml/schema/type_checker.h"
#include "../xml/xml.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
   std::string_view trim_view(std::string_view Value) noexcept
   {
      auto start = Value.find_first_not_of(" \t\r\n");
      if (start IS std::string_view::npos) return std::string_view();

      auto end = Value.find_last_not_of(" \t\r\n");
      return Value.substr(start, end - start + 1);
   }

   void append_node_text(XTag *Node, std::string &Output)
   {
      if (!Node) return;

      if (Node->isContent()) {
         if ((!Node->Attribs.empty()) and Node->Attribs[0].isContent()) {
            Output.append(Node->Attribs[0].Value);
         }

         for (auto &child : Node->Children) {
            append_node_text(&child, Output);
         }

         return;
      }

      for (auto &child : Node->Children) {
         if (child.Attribs.empty()) continue;

         if (child.Attribs[0].isContent()) {
            Output.append(child.Attribs[0].Value);
         }
         else append_node_text(&child, Output);
      }
   }
}

std::string format_xpath_number(double Value)
{
   if (std::isnan(Value)) return std::string("NaN");
   if (std::isinf(Value)) return (Value > 0) ? std::string("Infinity") : std::string("-Infinity");
   if (Value IS 0.0) return std::string("0");

   std::ostringstream stream;
   stream.setf(std::ios::fmtflags(0), std::ios::floatfield);
   stream.precision(15);
   stream << Value;

   std::string result = stream.str();

   if (not result.empty() and (result[0] IS '+')) result.erase(result.begin());

   auto decimal = result.find('.');
   if (decimal != std::string::npos)
   {
      while ((!result.empty()) and (result.back() IS '0')) result.pop_back();
      if ((!result.empty()) and (result.back() IS '.')) result.pop_back();
   }

   return result;
}

std::optional<bool> parse_schema_boolean(std::string_view Value)
{
   auto trimmed = trim_view(Value);
   if (trimmed.empty()) return std::nullopt;

   if ((trimmed.length() IS 1) and (trimmed[0] IS '1')) return true;
   if ((trimmed.length() IS 1) and (trimmed[0] IS '0')) return false;

   std::string lowered(trimmed);
   std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char Ch) { return char(std::tolower(Ch)); });

   if (lowered.compare("true") IS 0) return true;
   if (lowered.compare("false") IS 0) return false;

   return std::nullopt;
}

bool XPathVal::to_boolean() const
{
   auto schema_type = get_schema_type();
   if ((schema_type IS xml::schema::SchemaType::XPathBoolean) or (schema_type IS xml::schema::SchemaType::XSBoolean)) {
      if (Type IS XPVT::String) {
         auto parsed = parse_schema_boolean(StringValue);
         if (parsed.has_value()) return *parsed;
      }
   }

   switch (Type) {
      case XPVT::Boolean: return NumberValue != 0.0 and !std::isnan(NumberValue);
      case XPVT::Number: return NumberValue != 0.0 and !std::isnan(NumberValue);
      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime:
         return !StringValue.empty();
      case XPVT::NodeSet: return !node_set.empty();
      case XPVT::Map: return map_storage and !map_storage->empty();
      case XPVT::Array: return array_storage and !array_storage->empty();
   }
   return false;
}

double XPathVal::string_to_number(const std::string &Value) {
   auto trimmed = trim_view(Value);
   if (trimmed.empty()) return std::numeric_limits<double>::quiet_NaN();

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
   if ((end_ptr IS lexical.c_str()) or (*end_ptr != '\0')) {
      return std::numeric_limits<double>::quiet_NaN();
   }

   return result;
}

std::string XPathVal::node_string_value(XTag *Node)
{
   if (not Node) return std::string();

   std::string value;
   append_node_text(Node, value);
   return value;
}

double XPathVal::to_number() const
{
   auto schema_type = get_schema_type();
   if ((schema_type IS xml::schema::SchemaType::XPathBoolean) or (schema_type IS xml::schema::SchemaType::XSBoolean)) {
      if (Type IS XPVT::String) {
         auto parsed = parse_schema_boolean(StringValue);
         if (parsed.has_value()) return *parsed ? 1.0 : 0.0;
      }
   }

   switch (Type)
   {
      case XPVT::Boolean: return NumberValue;
      case XPVT::Number: return NumberValue;
      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime: {
         return string_to_number(StringValue);
      }
      case XPVT::NodeSet: {
         if (node_set.empty()) return std::numeric_limits<double>::quiet_NaN();
         if (node_set_string_override.has_value()) return string_to_number(*node_set_string_override);
         if (not node_set_attributes.empty()) {
            const XMLAttrib *attribute = node_set_attributes[0];
            if (attribute) return string_to_number(attribute->Value);
         }
         if (not node_set_string_values.empty()) return string_to_number(node_set_string_values[0]);
         return string_to_number(node_string_value(node_set[0]));
      }
      case XPVT::Map:
      case XPVT::Array:
         return std::numeric_limits<double>::quiet_NaN();
   }
   return 0.0;
}

std::string XPathVal::to_string() const
{
   auto schema_type = get_schema_type();
   if (xml::schema::is_numeric(schema_type) and (Type != XPVT::NodeSet)
      and (Type != XPVT::Map) and (Type != XPVT::Array)) {
      double numeric_value = to_number();
      if (!std::isnan(numeric_value)) return format_xpath_number(numeric_value);
   }

   if ((schema_type IS xml::schema::SchemaType::XPathBoolean) or (schema_type IS xml::schema::SchemaType::XSBoolean)) {
      if (Type IS XPVT::String) {
         auto parsed = parse_schema_boolean(StringValue);
         if (parsed.has_value()) return *parsed ? std::string("true") : std::string("false");
      }
   }

   switch (Type) {
      case XPVT::Boolean: return (NumberValue != 0.0 and !std::isnan(NumberValue)) ? "true" : "false";
      case XPVT::Number: return format_xpath_number(NumberValue);

      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime:
         return StringValue;

      case XPVT::NodeSet: {
         if (node_set_string_override.has_value()) return *node_set_string_override;

         if (not node_set_string_values.empty()) {
            if (node_set_string_values.size() IS 1) return node_set_string_values[0];

            std::string joined;
            for (size_t i = 0; i < node_set_string_values.size(); ++i) {
               if (i > 0) joined.push_back(':');
               joined += node_set_string_values[i];
            }
            return joined;
         }

         if (not node_set_attributes.empty()) {
            const XMLAttrib *attribute = node_set_attributes[0];
            if (attribute) return attribute->Value;
         }

         if (node_set.empty()) return std::string();
         return node_string_value(node_set[0]);
      }
      case XPVT::Map: {
         size_t entry_count = (map_storage) ? map_storage->size() : 0u;
         std::string summary("[map entries=");
         summary += std::to_string(entry_count);
         summary.push_back(']');
         return summary;
      }
      case XPVT::Array: {
         size_t member_count = (array_storage) ? array_storage->size() : 0u;
         std::string summary("[array members=");
         summary += std::to_string(member_count);
         summary.push_back(']');
         return summary;
      }
   }
   return "";
}

NODES XPathVal::to_node_set() const
{
   if (Type IS XPVT::NodeSet) return node_set;
   return {};
}

bool XPathVal::is_empty() const
{
   switch (Type) {
      case XPVT::Boolean: return false;
      case XPVT::Number: return false;
      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime:
         return StringValue.empty();
      case XPVT::NodeSet: return node_set.empty();
      case XPVT::Map: return (!map_storage) or map_storage->empty();
      case XPVT::Array: return (!array_storage) or array_storage->empty();
   }
   return true;
}

size_t XPathVal::size() const
{
   switch (Type)
   {
      case XPVT::NodeSet: return node_set.size();
      case XPVT::Map: return (map_storage) ? map_storage->size() : 0;
      case XPVT::Array: return (array_storage) ? array_storage->size() : 0;
      case XPVT::String:
      case XPVT::Date:
      case XPVT::Time:
      case XPVT::DateTime:
         return is_empty() ? 0 : 1;
      default: return is_empty() ? 0 : 1;
   }
}

bool XPathVal::has_schema_info() const
{
   return schema_type_info != nullptr;
}

void XPathVal::set_schema_type(std::shared_ptr<xml::schema::SchemaTypeDescriptor> TypeInfo)
{
   schema_type_info = TypeInfo;
   schema_validated = false;
}

bool XPathVal::validate_against_schema() const
{
   if (schema_validated and schema_type_info) return true;

   auto descriptor = schema_type_info;
   auto &registry_ref = xml::schema::registry();
   if (not descriptor) {
      auto inferred_type = xml::schema::schema_type_for_xpath(Type);
      descriptor = registry_ref.find_descriptor(inferred_type);
      if (not descriptor) return false;
      schema_type_info = descriptor;
   }

   xml::schema::TypeChecker checker(registry_ref);
   schema_validated = checker.validate_value(*this, *descriptor);
   return schema_validated;
}

xml::schema::SchemaType XPathVal::get_schema_type() const
{
   if (schema_type_info) return schema_type_info->schema_type;
   return xml::schema::schema_type_for_xpath(Type);
}

XPathVal xpath_nodeset_from_components(pf::vector<XTag *> Nodes,
   std::vector<const XMLAttrib *> Attributes,
   std::vector<std::string> Strings,
   std::optional<std::string> Override)
{
   XPathVal value;
   value.Type = XPVT::NodeSet;
   value.node_set = std::move(Nodes);
   value.node_set_attributes = std::move(Attributes);
   value.node_set_string_values = std::move(Strings);
   value.node_set_string_override = std::move(Override);
   return value;
}

XPathVal xpath_nodeset_singleton(XTag *Node, const XMLAttrib *Attribute,
   std::string StringValue)
{
   pf::vector<XTag *> nodes;
   nodes.push_back(Node);

   std::vector<const XMLAttrib *> attributes;
   attributes.push_back(Attribute);

   std::vector<std::string> strings;
   strings.push_back(StringValue);

   std::optional<std::string> override_value(StringValue);
   return xpath_nodeset_from_components(std::move(nodes), std::move(attributes), std::move(strings), std::move(override_value));
}
