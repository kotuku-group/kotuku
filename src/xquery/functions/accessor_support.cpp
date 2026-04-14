// XPath Accessor Support Utilities
//
// Shared helpers that offer document and schema discovery for accessor-style XPath functions.  The routines
// consolidate logic that was previously embedded inside the document helper functions so that fn:base-uri(),
// fn:document-uri(), fn:data(), fn:nilled() and related calls can reuse consistent behaviour regardless of the
// source document for a node.

#include "accessor_support.h"
#include <filesystem>

namespace xpath::accessor {

//********************************************************************************************************************

[[nodiscard]] static bool attribute_is_xml_base(const XMLAttrib &Attribute)
{
   if (Attribute.Name.empty()) return false;
   return pf::iequals(Attribute.Name, "xml:base");
}

//********************************************************************************************************************

[[nodiscard]] static std::optional<std::string> trim_to_base_directory(std::string candidate)
{
   if (candidate.empty()) return std::nullopt;

   std::error_code status_error;
   auto status = std::filesystem::status(std::filesystem::path(candidate), status_error);
   if ((not status_error) and std::filesystem::is_directory(status)) {
      if ((candidate.back() != '/') and (candidate.back() != '\\')) candidate.push_back('/');
      return xml::uri::normalise_uri_separators(std::move(candidate));
   }

   size_t boundary = candidate.find_last_of("/\\");
   if (boundary IS std::string::npos) boundary = candidate.find_last_of(':');

   if (boundary IS std::string::npos) return std::nullopt;

   std::string folder(candidate.substr(0, boundary + 1));
   return xml::uri::normalise_uri_separators(std::move(folder));
}

//********************************************************************************************************************

std::optional<std::string> resolve_document_base_directory(std::string_view BasePath)
{
   if (not BasePath.empty()) {
      std::string candidate;
      std::string resolved;
      if (ResolvePath(BasePath, RSF::NO_FILE_CHECK, &resolved) IS ERR::Okay) candidate = std::move(resolved);
      else candidate = BasePath;
      if (auto folder = trim_to_base_directory(candidate)) return folder;
      if (auto fallback = trim_to_base_directory(std::string(BasePath))) return fallback;
   }

   if (objTask *task = CurrentTask()) {
      CSTRING task_path = nullptr;
      if ((task->get(FID_Path, task_path) IS ERR::Okay) and task_path) {
         std::string working(task_path);
         if (not working.empty()) {
            char last = working.back();
            if ((last != '/') and (last != '\\')) working.push_back('/');
            return xml::uri::normalise_uri_separators(std::move(working));
         }
      }
   }

   return std::nullopt;
}

//********************************************************************************************************************

[[nodiscard]] static XTag * parent_for_node(extXML *Document, XTag *Node)
{
   if ((!Document) or (!Node)) return nullptr;
   if (Node->ParentID IS 0) return nullptr;
   return Document->getTag(Node->ParentID);
}

//********************************************************************************************************************

[[nodiscard]] static XTag * find_attribute_owner(extXML *Document, const XMLAttrib *Attribute)
{
   if ((!Document) or (!Attribute)) return nullptr;

   auto &map = Document->getMap();
   for (auto &entry : map) {
      XTag *candidate = entry.second;
      if (not candidate) continue;

      for (auto &attrib : candidate->Attribs) {
         const XMLAttrib *attrib_ptr = &attrib;
         if (attrib_ptr IS Attribute) return candidate;
      }
   }

   return nullptr;
}

//********************************************************************************************************************
// Returns the XTag that owns an attribute.  If a NodeHint is provided, it is checked first to see if it owns the attribute.

[[nodiscard]] static XTag * resolve_attribute_scope(const XPathContext &Context, XTag *NodeHint, const XMLAttrib *Attribute, extXML *&Document)
{
   if (not Attribute) return NodeHint;

   if (NodeHint) {
      for (auto &attrib : NodeHint->Attribs) {
         const XMLAttrib *attrib_ptr = &attrib;
         if (attrib_ptr IS Attribute) return NodeHint;
      }
   }

   auto locate_in_document = [&](extXML *Candidate) -> XTag * {
      if (not Candidate) return nullptr;
      if (XTag *owner = find_attribute_owner(Candidate, Attribute); owner) {
         Document = Candidate;
         return owner;
      }
      return nullptr;
   };

   if (Document) {
      if (auto owner = locate_in_document(Document)) return owner;
   }

   if (Context.xml) {
      if (auto owner = locate_in_document(Context.xml)) return owner;

      for (auto &entry : Context.eval->parse_context->XMLCache) {
         if (auto owner = locate_in_document(entry.second)) return owner;
      }
   }

   return NodeHint;
}

//********************************************************************************************************************

[[nodiscard]] static std::shared_ptr<xml::schema::ElementDescriptor> find_element_descriptor(extXML *Document,
   std::string_view Name)
{
   if ((!Document) or (!Document->SchemaContext)) return nullptr;

   auto &context = *Document->SchemaContext;

   auto lookup = context.elements.find(std::string(Name));
   if (lookup != context.elements.end()) return lookup->second;

   auto local = std::string(xml::schema::extract_local_name(Name));
   lookup = context.elements.find(local);
   if (lookup != context.elements.end()) return lookup->second;

   if (not context.target_namespace_prefix.empty()) {
      std::string qualified = std::format("{}:{}", context.target_namespace_prefix, local);
      lookup = context.elements.find(qualified);
      if (lookup != context.elements.end()) return lookup->second;
   }

   return nullptr;
}

//********************************************************************************************************************

[[nodiscard]] static std::shared_ptr<xml::schema::SchemaTypeDescriptor> resolve_type_descriptor(
   const XPathContext &Context, extXML *Document, const std::string &TypeName)
{
   if (TypeName.empty()) return nullptr;

   if ((Document) and Document->SchemaContext) {
      auto &types = Document->SchemaContext->types;
      auto iter = types.find(TypeName);
      if (iter != types.end()) return iter->second;

      auto local = std::string(xml::schema::extract_local_name(TypeName));
      iter = types.find(local);
      if (iter != types.end()) return iter->second;
   }

   if (not Context.schema_registry) return nullptr;

   if (auto descriptor = Context.schema_registry->find_descriptor(TypeName)) return descriptor;

   auto local = std::string(xml::schema::extract_local_name(TypeName));
   return Context.schema_registry->find_descriptor(local);
}

//********************************************************************************************************************

[[nodiscard]] static bool attribute_matches_nil(const XMLAttrib &Attribute, XTag *Scope, extXML *Document)
{
   if (Attribute.Name.empty()) return false;

   std::string_view name(Attribute.Name);
   auto colon = name.find(':');
   if (colon IS std::string::npos) return false;

   std::string prefix(name.substr(0, colon));
   std::string local(name.substr(colon + 1));

   if (not pf::iequals(local, "nil")) return false;

   if (pf::iequals(prefix, "xml")) return false;
   if (pf::iequals(prefix, "xmlns")) return false;

   std::string uri;
   if (Document) uri = find_in_scope_namespace(Scope, Document, prefix);
   if (uri.empty()) return false;

   return pf::iequals(uri, "http://www.w3.org/2001/XMLSchema-instance");
}

//********************************************************************************************************************
// Locates the document that contains a particular node.

[[nodiscard]] extXML * locate_node_document(const XPathContext &Context, XTag *Node)
{
   if (not Node) return nullptr;

   if (Context.xml) {
      auto &map = Context.xml->getMap();
      auto it = map.find(Node->ID);
      if ((it != map.end()) and (it->second IS Node)) {
         return Context.xml;
      }

      for (auto &it : Context.eval->parse_context->XMLCache) {
         extXML *cached_xml = it.second;
         auto &cached_map = cached_xml->getMap();
         auto cit = cached_map.find(Node->ID);
         if ((cit != cached_map.end()) and (cit->second IS Node)) return cached_xml;
      }
   }
   return nullptr;
}

//********************************************************************************************************************
// Builds the base URI chain for a node or attribute-only node-set.  Note that setting AttributeNode will result in
// Node being recomputed.

std::optional<std::string> build_base_uri_chain(const XPathContext &Context, XTag *Node, const XMLAttrib *AttributeNode)
{
   extXML *document;
   if (extXML *origin = locate_node_document(Context, Node)) document = origin;
   else document = Context.xml;

   if (AttributeNode) {
      Node = resolve_attribute_scope(Context, Node, AttributeNode, document);

      if (Node) {
         extXML *owner_origin = locate_node_document(Context, Node);
         if (owner_origin) document = owner_origin;
      }
   }

   if (not Node) {
      std::string_view path;
      if ((document) and (document->Path)) path = document->Path;
      else if ((Context.xml) and (Context.xml->Path)) path = Context.xml->Path;
      auto base = resolve_document_base_directory(path);
      if (base.has_value()) return xml::uri::normalise_uri_separators(*base);
      return std::nullopt;
   }

   if (not document) {
      extXML *owner_origin = locate_node_document(Context, Node);
      if (owner_origin) document = owner_origin;
      else document = Context.xml;
   }

   if ((Node->ParentID IS 0) and (!AttributeNode)) { // Root-level node with no attribute
      std::string_view path;
      if ((document) and (document->Path)) path = document->Path;
      else if ((Context.xml) and (Context.xml->Path)) path = Context.xml->Path;
      auto base = resolve_document_base_directory(path);
      if (base.has_value()) return xml::uri::normalise_uri_separators(*base);
   }

   const std::string * cached_base = nullptr;
   if (document) cached_base = document->findBaseURI(Node->ID);
   else if (Context.xml) cached_base = Context.xml->findBaseURI(Node->ID);

   if (cached_base) return xml::uri::normalise_uri_separators(*cached_base);

   std::vector<std::string> chain;
   for (XTag *current = Node; current; ) {
      bool skip_current_xml_base = (not current->ParentID) and (current IS Node) and (not AttributeNode);

      for (size_t index = 1; index < current->Attribs.size(); ++index) {
         const XMLAttrib &attrib = current->Attribs[index];
         if (not attribute_is_xml_base(attrib)) continue;
         if (skip_current_xml_base) continue;
         chain.push_back(attrib.Value);
      }

      XTag *parent = parent_for_node(document, current);
      if (not parent) break;
      current = parent;
   }

   std::optional<std::string> base = resolve_document_base_directory((document and document->Path) ? document->Path : std::string_view{});

   for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator) {
      if (base.has_value()) base = xml::uri::resolve_relative_uri(*iterator, *base);
      else base = *iterator;
   }

   if (base.has_value()) return xml::uri::normalise_uri_separators(*base);
   return std::nullopt;
}

//********************************************************************************************************************
// Resolves the document URI for a node.

std::optional<std::string> resolve_document_uri(const XPathContext &Context, XTag *Node)
{
   if (not Node) return std::nullopt;

   extXML *document = locate_node_document(Context, Node);
   if (not document) return std::nullopt;
   if (document->Path and document->Path[0]) {
      return xml::uri::normalise_uri_separators(std::string(document->Path));
   }

   // Perform a reverse lookup in the XML cache to find the document URI.

   if (Context.xml) {
      for (auto &entry : Context.eval->parse_context->XMLCache) {
         if (entry.second IS document) {
            return xml::uri::normalise_uri_separators(entry.first); // TODO: Is normalisation needed here?
         }
      }
   }

   return std::nullopt;
}

//********************************************************************************************************************
// Infers the schema type for an element node.

std::shared_ptr<xml::schema::SchemaTypeDescriptor> infer_schema_type(const XPathContext &Context, XTag *Node,
   const XMLAttrib *AttributeNode)
{
   if (not Context.schema_registry) return nullptr;
   if (not Node) return nullptr;
   if (AttributeNode) return nullptr;
   if (Node->Attribs.empty()) return nullptr;
   if (Node->Attribs[0].Name.empty()) return nullptr;

   extXML *origin = locate_node_document(Context, Node);
   extXML *document = origin ? origin : Context.xml;
   if ((!document) or (!document->SchemaContext)) return nullptr;

   auto descriptor = find_element_descriptor(document, Node->Attribs[0].Name);
   if (not descriptor) return nullptr;

   if (descriptor->type) return descriptor->type;

   if (not descriptor->type_name.empty()) {
      if (auto resolved = resolve_type_descriptor(Context, document, descriptor->type_name)) return resolved;
   }

   return nullptr;
}

//********************************************************************************************************************
// Determines whether an element is explicitly marked as nilled via the xsi:nil attribute.

bool is_element_explicitly_nilled(const XPathContext &Context, XTag *Node)
{
   if ((!Node) or Node->Attribs.empty()) return false;
   if (Node->Attribs[0].Name.empty()) return false;

   extXML *origin = locate_node_document(Context, Node);
   extXML *document = origin ? origin : Context.xml;

   for (size_t index = 1; index < Node->Attribs.size(); ++index) {
      const XMLAttrib &attrib = Node->Attribs[index];
      if (not attribute_matches_nil(attrib, Node, document)) continue;

      auto parsed = parse_schema_boolean(attrib.Value);
      if (parsed.has_value()) return parsed.value();
   }

   return false;
}

} // namespace xpath::accessor
