/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
XML: Provides an interface for the management of structured data.

The XML module provides comprehensive support for XML 1.0 document parsing, manipulation, and querying.  It integrates
with the XPath module to deliver a standards-compliant XML processing environment with extensive querying capabilities.

<header>XML Processing</header>

The module implements full XML 1.0 parsing and serialisation, including support for namespaces, attributes, CDATA
sections, processing instructions, and DOCTYPE declarations.  Documents can be loaded from files, strings, or streams,
and modified programmatically through a tree-based API.  The parser validates well-formedness and provides detailed
error reporting for malformed documents.

<header>XPath 2.0+ Support</header>

All aspects of XPath 2.0 are supported except for the following:

<list type="bullet">
<li>Namespace axis: The `namespace::*` and `namespace::prefix` axis expressions are not supported.</li>
<li>Schema imports: Schema import declarations are not recognised.</li>
<li>External variables: External variable declarations (`declare variable $name external`) are not supported.</li>
<li>Custom collations: Only the W3C codepoint collation (`http://www.w3.org/2005/xpath-functions/collation/codepoint`) is supported.  Custom collation URIs are rejected.</li>
</list>

<header>XQuery 1.0+ Support</header>

The module implements core XQuery 1.0 functionality, including FLWOR expressions (`for`, `let`, `where`, `order by`,
`return`, `group by`, `count` clauses), node constructors (element, attribute, document, text, comment, processing
instruction), and a comprehensive function library covering strings, numbers, sequences, dates, durations, QNames, and
document access.  XQuery support excludes the following:

<list type="bullet">
<li>Schema-aware processing: Type validation against XML Schema is not supported.</li>
</list>

-END-

*********************************************************************************************************************/

#include <kotuku/modules/xml.h>
#include <kotuku/modules/xquery.h>
#include <kotuku/strings.hpp>
#include <array>
#include <format>
#include <functional>
#include <sstream>
#include <vector>

#include "../link/unicode.h"
#include "xml.h"
#include "schema/schema_parser.h"
#include "schema/type_checker.h"
#include "uri_utils.h"
#include "unescape.h"
#include "xpath_value.h"

JUMPTABLE_CORE

static OBJECTPTR clXML = nullptr;
static OBJECTPTR modContext = nullptr;
static std::atomic<uint32_t> glTagID = 1;

#include "xml_def.c"

//*********************************************************************************************************************

#include "xml_functions.cpp"

[[nodiscard]] static bool attribute_is_xml_base(const XMLAttrib &Attribute)
{
   if (Attribute.Name.empty()) return false;
   return pf::iequals(Attribute.Name, "xml:base");
}

[[nodiscard]] static std::string document_base(extXML *Document)
{
   if ((!Document) or (!Document->Path) or (!*Document->Path)) return std::string();
   return xml::uri::normalise_uri_separators(std::string(Document->Path));
}

[[nodiscard]] static std::string resolve_inherited_base(extXML *Document, XTag *Parent)
{
   if (!Document) return std::string();
   if (!Parent) return document_base(Document);

   if (auto cached = Document->findBaseURI(Parent->ID)) return *cached;

   std::vector<std::string> chain;
   for (XTag *current = Parent; current; ) {
      for (size_t index = 1; index < current->Attribs.size(); ++index) {
         const XMLAttrib &attrib = current->Attribs[index];
         if (attribute_is_xml_base(attrib)) chain.push_back(attrib.Value);
      }

      if (!current->ParentID) break;
      current = Document->getTag(current->ParentID);
   }

   std::string base = document_base(Document);
   for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator) {
      if (base.empty()) base = *iterator;
      else base = xml::uri::resolve_relative_uri(*iterator, base);
      base = xml::uri::normalise_uri_separators(std::move(base));
   }

   return base;
}

static void refresh_base_uris(extXML *Document, XTag &Node, const std::string &InheritedBase)
{
   std::string node_base = InheritedBase;

   for (size_t index = 1; index < Node.Attribs.size(); ++index) {
      const XMLAttrib &attrib = Node.Attribs[index];
      if (!attribute_is_xml_base(attrib)) continue;

      std::string resolved;
      if (attrib.Value.empty()) resolved = InheritedBase;
      else if (InheritedBase.empty()) resolved = attrib.Value;
      else resolved = xml::uri::resolve_relative_uri(attrib.Value, InheritedBase);

      node_base = xml::uri::normalise_uri_separators(std::move(resolved));
   }

   Document->BaseURIMap[Node.ID] = node_base;

   for (auto &child : Node.Children) refresh_base_uris(Document, child, node_base);
}

static void refresh_base_uris_for_insert(extXML *Document, TAGS &Inserted, XTag *Parent)
{
   if ((!Document) or Inserted.empty()) return;

   std::string inherited = resolve_inherited_base(Document, Parent);
   for (auto &node : Inserted) refresh_base_uris(Document, node, inherited);
}

static ERR add_xml_class(void);
static ERR SET_Statement(extXML *, CSTRING);
static ERR SET_Source(extXML *, OBJECTPTR);

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR pModule, struct CoreBase *pCore)
{
   CoreBase = pCore;
   modContext = CurrentContext();
   return add_xml_class();
}

static ERR MODExpunge(void)
{
   if (clXML) { FreeResource(clXML); clXML = nullptr; }
   return ERR::Okay;
}

static ERR MODOpen(OBJECTPTR Module)
{
   Module->set(FID_FunctionList, glFunctions);
   return ERR::Okay;
}

namespace xml {

/*********************************************************************************************************************

-FUNCTION-
XValueToNumber: Converts an XPathValue to a 64-bit floating point value.

Call XValueToNumber() to convert an XPathValue object to a 64-bit floating point number.  This function
also includes cover support for boolean types, converting true to 1.0 and false to 0.

-INPUT-
ptr(struct(XPathValue)) Value: The XPathValue to convert.
&double Result: The numeric representation of the value is returned here.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

ERR XValueToNumber(XPathValue *Value, double *Result)
{
   pf::Log log(__FUNCTION__);

   if ((not Value) or (not Result)) return log.warning(ERR::NullArgs);
   if (Value->Type IS XPVT::NIL) return log.warning(ERR::NoData);

   auto val = (XPathVal *)Value;
   *Result = val->to_number();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
XValueNodes: For node-set XPathValue objects, returns the node-set as an array.

If an XPathValue represents a node-set (type `XPVT::NODE_SET`) then XValueToNodes() will return a direct pointer to
the node-set array.

Note: The integrity of the array is not guaranteed if the original XML document is modified or freed.

-INPUT-
ptr(struct(XPathValue)) Value: The XPathValue to convert.
&cpp(array(ptr(struct(XTag)))) Result: The node-set is returned here as an array of !XTag structures.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

ERR XValueNodes(XPathValue *Value, pf::vector<XTag *> *Result)
{
   pf::Log log(__FUNCTION__);
   if ((not Value) or (not Result)) return log.warning(ERR::NullArgs);
   if (Value->Type IS XPVT::NIL) return log.warning(ERR::NoData);
   if (Value->Type != XPVT::NodeSet) return log.warning(ERR::Mismatch);
   auto val = (XPathVal *)Value;
   *Result = val->to_node_set();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
XValueToString: Converts an XPathValue to its string representation.

Call XValueToString() to convert an XPathValue object into its string representation.

-INPUT-
ptr(cstruct(XPathValue)) Value: The XPathValue to convert.
&cpp(str) Result: Receives the string representation of the value.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

ERR XValueToString(const XPathValue *Value, std::string *Result)
{
   pf::Log log(__FUNCTION__);

   if ((not Value) or (not Result)) return log.warning(ERR::NullArgs);
   if (Value->Type IS XPVT::NIL) return log.warning(ERR::NoData);

   auto val = (XPathVal *)Value;

   if (Value->Type IS XPVT::NodeSet) {
      // Provide a user-friendly sequence string for NodeSet values that encode sequences of atomic items.
      // Prefer explicit string vectors when present; fall back to attribute/text extraction.
      if (val->node_set_string_override.has_value()) {
         *Result = *val->node_set_string_override;
         return ERR::Okay;
      }

      if (!val->node_set_string_values.empty()) {
         if (val->node_set_string_values.size() IS 1) {
            *Result = val->node_set_string_values[0];
            return ERR::Okay;
         }

         std::string joined;
         for (size_t i = 0; i < val->node_set_string_values.size(); ++i) {
            if (i > 0) joined.push_back(':');
            joined += val->node_set_string_values[i];
         }
         *Result = std::move(joined);
         return ERR::Okay;
      }

      // Attributes: join attribute string values if multiple
      if (!val->node_set_attributes.empty()) {
         std::string joined;
         size_t count = std::max(val->node_set_attributes.size(), val->node_set.size());
         for (size_t i = 0; i < count; ++i) {
            if (i > 0) joined.push_back(':');
            if (i < val->node_set_attributes.size() && val->node_set_attributes[i]) joined += val->node_set_attributes[i]->Value;
            else if (i < val->node_set.size() && val->node_set[i]) joined += XPathVal::node_string_value(val->node_set[i]);
         }
         *Result = std::move(joined);
         return ERR::Okay;
      }

      // Generic nodes: join text values if multiple
      if (!val->node_set.empty()) {
         if (val->node_set.size() IS 1) {
            *Result = XPathVal::node_string_value(val->node_set[0]);
            return ERR::Okay;
         }

         std::string joined;
         for (size_t i = 0; i < val->node_set.size(); ++i) {
            if (i > 0) joined.push_back(':');
            joined += XPathVal::node_string_value(val->node_set[i]);
         }
         *Result = std::move(joined);
         return ERR::Okay;
      }
   }

   *Result = val->to_string();
   return ERR::Okay;
}

//********************************************************************************************************************

} // namespace xml

//********************************************************************************************************************

#include "xml_class.cpp"

static STRUCTS glStructures = {
   { "XTag", sizeof(XTag) },
   { "XPathValue", sizeof(XPathValue) }
};

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, &glStructures);
extern "C" struct ModHeader * register_xml_module() { return &ModHeader; }
