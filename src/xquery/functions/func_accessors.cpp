// XPath Accessor Functions
//
// Implements XPath 2.0 accessor helpers (base-uri, data, document-uri, node-name, nilled, static-base-uri and
// default-collation) by combining the shared document/schema utilities in accessor_support.cpp with the
// sequence-building helpers declared in xpath_functions.cpp.

#include "accessor_support.h"
//********************************************************************************************************************
// base-uri() as xs:anyURI?
// Returns the base URI for the specified node, or the context node if no argument is provided

XPathVal XPathFunctionLibrary::function_base_uri(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   XTag *target_node = nullptr;
   const XMLAttrib *target_attribute = nullptr;

   if (Args.empty()) {
      target_node = Context.context_node;
      target_attribute = Context.attribute_node;
   }
   else if (Args[0].Type IS XPVT::NodeSet) {
      const XPathVal &value = Args[0];
      if (!value.node_set_attributes.empty()) target_attribute = value.node_set_attributes[0];
      if (!value.node_set.empty()) target_node = value.node_set[0];
   }
   else return XPathVal(pf::vector<XTag *>());

   auto base = xpath::accessor::build_base_uri_chain(Context, target_node, target_attribute);
   if (!base.has_value()) return XPathVal(pf::vector<XTag *>());

   return XPathVal(*base);
}

//********************************************************************************************************************
// data($arg as item()*) as xs:anyAtomicType*
// Returns the typed value for each item in the specified sequence

XPathVal XPathFunctionLibrary::function_data(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   const XPathVal *sequence_value = nullptr;
   XPathVal context_value;

   if (Args.empty()) {
      if (Context.attribute_node) {
         context_value.Type = XPVT::NodeSet;
         context_value.node_set.push_back(Context.context_node);
         context_value.node_set_attributes.push_back(Context.attribute_node);
         sequence_value = &context_value;
      }
      else if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         context_value = XPathVal(nodes);
         sequence_value = &context_value;
      }
      else return XPathVal(pf::vector<XTag *>());
   }
   else sequence_value = &Args[0];

   size_t length = sequence_length(*sequence_value);
   if (!length) return XPathVal(pf::vector<XTag *>());

   SequenceBuilder builder;

   for (size_t index = 0u; index < length; ++index) {
      XPathVal item = extract_sequence_item(*sequence_value, index);

      if (item.Type IS XPVT::NodeSet) {
         XTag *node = item.node_set.empty() ? nullptr : item.node_set[0];
         const XMLAttrib *attribute = item.node_set_attributes.empty() ? nullptr : item.node_set_attributes[0];

         if (attribute) {
            append_value_to_sequence(XPathVal(attribute->Value), builder);
            continue;
         }

         if (node) {
            auto descriptor = xpath::accessor::infer_schema_type(Context, node, attribute);
            std::string node_value = XPathVal::node_string_value(node);

            if (descriptor) {
               XPathVal base_value(node_value);
               XPathVal coerced = descriptor->coerce_value(base_value, descriptor->schema_type);
               append_value_to_sequence(coerced, builder);
            }
            else append_value_to_sequence(XPathVal(node_value), builder);

            continue;
         }

         if (!item.node_set_string_values.empty()) {
            append_value_to_sequence(XPathVal(item.node_set_string_values[0]), builder);
            continue;
         }

         continue;
      }

      if (item.is_empty()) continue;

      append_value_to_sequence(item, builder);
   }

   return make_sequence_value(std::move(builder));
}

//********************************************************************************************************************
// document-uri($arg as node()?) as xs:anyURI?
// Returns the URI property for the specified node.

XPathVal XPathFunctionLibrary::function_document_uri(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   XTag *target_node = nullptr;

   if (Args.empty()) target_node = Context.context_node;
   else if (Args[0].Type IS XPVT::NodeSet) {
      if (!Args[0].node_set.empty()) target_node = Args[0].node_set[0];
   }
   else return XPathVal(pf::vector<XTag *>());

   auto uri = xpath::accessor::resolve_document_uri(Context, target_node);
   if (!uri.has_value()) return XPathVal(pf::vector<XTag *>());

   return XPathVal(*uri);
}

//********************************************************************************************************************
// node-name($arg as node()?) as xs:QName?
// Returns the node name for the specified node.

XPathVal XPathFunctionLibrary::function_node_name(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   XTag *target_node = nullptr;
   const XMLAttrib *target_attribute = nullptr;

   if (Args.empty()) {
      target_node = Context.context_node;
      target_attribute = Context.attribute_node;
   }
   else if (Args[0].Type IS XPVT::NodeSet) {
      const XPathVal &value = Args[0];
      if (!value.node_set_attributes.empty()) target_attribute = value.node_set_attributes[0];
      if (!value.node_set.empty()) target_node = value.node_set[0];
   }
   else return XPathVal(pf::vector<XTag *>());

   if (target_attribute) {
      if (target_attribute->Name.empty()) return XPathVal(pf::vector<XTag *>());
      return XPathVal(target_attribute->Name);
   }

   if ((!target_node) or target_node->Attribs.empty()) return XPathVal(pf::vector<XTag *>());

   std::string name = target_node->Attribs[0].Name;
   if (name.empty()) return XPathVal(pf::vector<XTag *>());

   return XPathVal(name);
}

//********************************************************************************************************************
// nilled($arg as node()?) as xs:boolean
// Returns true if the specified element node has an explicit xsi:nil="true" attribute.

XPathVal XPathFunctionLibrary::function_nilled(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   XTag *target_node = nullptr;

   if (Args.empty()) target_node = Context.context_node;
   else if (Args[0].Type IS XPVT::NodeSet) {
      if (!Args[0].node_set.empty()) target_node = Args[0].node_set[0];
   }
   else return XPathVal(pf::vector<XTag *>());

   if ((!target_node) or target_node->Attribs.empty()) return XPathVal(pf::vector<XTag *>());
   if (target_node->Attribs[0].Name.empty()) return XPathVal(pf::vector<XTag *>());

   bool nilled = xpath::accessor::is_element_explicitly_nilled(Context, target_node);
   return XPathVal(nilled);
}

//********************************************************************************************************************
// static-base-uri() as xs:anyURI?
// Returns the static base URI from the XQuery prolog, or the document base URI if none is defined.

XPathVal XPathFunctionLibrary::function_static_base_uri(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   XTag *target_node = Context.context_node;

   if (!target_node and Context.xml) {
      for (auto &tag : Context.xml->Tags) {
         if ((tag.Flags & XTF::INSTRUCTION) != XTF::NIL) continue;
         target_node = &tag;
         break;
      }
   }

   auto base = xpath::accessor::build_base_uri_chain(Context, target_node, Context.attribute_node);
   if (not base.has_value()) {
      if (Context.prolog) return XPathVal(Context.prolog->static_base_uri);
      else if (Context.xml) {
         if (Context.xml->Path) return XPathVal(Context.xml->Path);
      }
   }

   if (!base.has_value()) return XPathVal(pf::vector<XTag *>());

   return XPathVal(*base);
}

//********************************************************************************************************************
// default-collation() as xs:anyURI
// Returns the default collation URI from the XQuery prolog, or the codepoint collation if none is defined.

XPathVal XPathFunctionLibrary::function_default_collation(const std::vector<XPathVal> &, const XPathContext &Context)
{
   if (Context.prolog) {
      const std::string &collation = Context.prolog->default_collation;
      if (!collation.empty()) return XPathVal(collation);
   }

   return XPathVal(std::string("http://www.w3.org/2005/xpath-functions/collation/codepoint"));
}
