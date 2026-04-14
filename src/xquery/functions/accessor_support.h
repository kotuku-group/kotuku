// XPath Accessor Support Utilities
//
// These helpers centralise the document and schema lookups required by accessor-style XPath functions.  The
// routines replicate the behaviour previously embedded inside func_documents.cpp so that the accessor
// implementations can determine owning documents, resolve xml:base inheritance, and query schema metadata in a
// consistent manner regardless of the source tree for a node.

#pragma once

#include <memory>
#include <optional>
#include <string>

struct XTag;
class extXML;
struct XPathContext;
struct XMLAttrib;

namespace xml::schema
{
   class SchemaTypeDescriptor;
}

namespace xpath::accessor
{
   [[nodiscard]] extXML * locate_node_document(const XPathContext &Context, XTag *Node);

   std::optional<std::string> build_base_uri_chain(const XPathContext &Context, XTag *Node, const XMLAttrib *AttributeNode);

   std::optional<std::string> resolve_document_uri(const XPathContext &Context, XTag *Node);

   std::optional<std::string> resolve_document_base_directory(std::string_view);

   std::shared_ptr<xml::schema::SchemaTypeDescriptor> infer_schema_type(const XPathContext &Context, XTag *Node,
      const XMLAttrib *AttributeNode);

   bool is_element_explicitly_nilled(const XPathContext &Context, XTag *Node);
}

