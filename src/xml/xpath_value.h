// XPath Value System Interface
//
// Defines the XPathVal class, which represents typed values in XPath evaluation.  XPathVal extends the public
// XPathValue interface with implementation-specific functionality for type conversions, node-set operations,
// and schema integration.
//
// This class provides:
//   - Type conversion methods for XPath's four basic types (node-set, string, number, boolean)
//   - Schema-aware type information and validation
//   - Node-set string and numeric value extraction
//   - Format functions for XPath numeric output
//
// The value system ensures consistent type coercion semantics across XPath operations whilst integrating with
// XML Schema type descriptors for enhanced type awareness in XPath 2.0 expressions.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/xquery.h>

using NODES = pf::vector<XTag *>;

namespace xml::schema {

enum class SchemaType;
class SchemaTypeDescriptor;
class SchemaTypeRegistry;
[[nodiscard]] SchemaType schema_type_for_xpath(XPVT) noexcept;
[[nodiscard]] bool is_numeric(SchemaType) noexcept;
SchemaTypeRegistry & registry();

} // namespace

class XPathVal : public XPathValue
{
   public:
   mutable std::shared_ptr<xml::schema::SchemaTypeDescriptor> schema_type_info;
   mutable bool schema_validated = false;

   // Constructors

   XPathVal() : XPathValue(XPVT::Boolean) {}
   explicit XPathVal(bool value) : XPathValue(XPVT::Boolean) { NumberValue = value ? 1.0 : 0.0; }
   explicit XPathVal(double value) : XPathValue(XPVT::Number) { NumberValue = value; }
   explicit XPathVal(std::string value) : XPathValue(XPVT::String) { StringValue = std::move(value); }
   explicit XPathVal(XPVT ValueType, std::string value) : XPathValue(ValueType) { StringValue = std::move(value); }

   explicit XPathVal(const pf::vector<XTag *> &Nodes, std::optional<std::string> NodeSetString = std::nullopt,
      std::vector<std::string> NodeSetStrings = {}, std::vector<const XMLAttrib *> NodeSetAttributes = {})
      : XPathValue(Nodes, NodeSetString, NodeSetStrings, NodeSetAttributes) {}

   // Methods

   bool to_boolean() const;
   double to_number() const;
   std::string to_string() const;
   NODES to_node_set() const;
   bool is_empty() const;
   size_t size() const;
   bool has_schema_info() const;
   void set_schema_type(std::shared_ptr<xml::schema::SchemaTypeDescriptor> TypeInfo);
   bool validate_against_schema() const;
   xml::schema::SchemaType get_schema_type() const;

   static std::string node_string_value(XTag *Node);
   static double string_to_number(const std::string &Value);
};

std::string format_xpath_number(double Value);
std::optional<bool> parse_schema_boolean(std::string_view);
XPathVal xpath_nodeset_from_components(pf::vector<XTag *>, std::vector<const XMLAttrib *> Attributes = {},
   std::vector<std::string> Strings = {}, std::optional<std::string> Override = std::nullopt);
XPathVal xpath_nodeset_singleton(XTag *, const XMLAttrib *, std::string);
