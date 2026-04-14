// schema_parser.cpp - Implements the concrete reader that turns parsed XML Schema documents into
// SchemaDocument and SchemaContext structures consumed throughout the XML module.  The routines in
// this file walk the tag tree provided by objXML, extract namespace bindings, assemble element and
// type descriptors, and wire the results into the shared registry so that downstream validation,
// code generation, and tooling layers can reason about schema-constrained XML data.

#include "schema_parser.h"
#include <cstdlib>
#include <format>
#include <kotuku/strings.hpp>

namespace xml::schema
{
   namespace
   {
      bool is_named(std::string_view Name, std::string_view Expected)
      {
         return pf::iequals(Name, Expected);
      }

      std::string make_qualified_name(std::string_view Prefix, std::string_view Local)
      {
         if (Prefix.empty()) return std::string(Local);
         return std::format("{}:{}", Prefix, Local);
      }

      // Retrieves the value of the named attribute from the supplied node if present.
      std::string find_attribute_value(const XTag &Node, std::string_view Name)
      {
         for (size_t index = 1u; index < Node.Attribs.size(); ++index) {
            const auto &Attrib = Node.Attribs[index];
            if (pf::iequals(Attrib.Name, Name)) return Attrib.Value;
         }
         return std::string();
      }

      // Parses the min/max occurs value, supporting defaults and the unbounded keyword.
      size_t parse_occurs_value(const std::string &Value, size_t DefaultValue, bool AllowUnbounded)
      {
         if (Value.empty()) return DefaultValue;
         if (AllowUnbounded and pf::iequals(Value, "unbounded")) return std::numeric_limits<size_t>::max();

         char *end = nullptr;
         auto parsed = std::strtoull(Value.c_str(), &end, 10);
         if ((end) and (*end IS '\0')) return size_t(parsed);
         return DefaultValue;
      }

      // Registers multiple aliases for the supplied element descriptor within the schema context.
      void register_element_aliases(SchemaDocument &Document, const std::shared_ptr<ElementDescriptor> &Descriptor)
      {
         if (!Descriptor) return;
         if (!Document.context) Document.context = std::make_shared<SchemaContext>();

         auto &elements = Document.context->elements;
         elements[Descriptor->name] = Descriptor;
         if (!Descriptor->qualified_name.empty()) elements[Descriptor->qualified_name] = Descriptor;

         auto local_name = std::string(extract_local_name(Descriptor->qualified_name.empty() ? Descriptor->name
                                                                                               : Descriptor->qualified_name));
         if (!local_name.empty()) elements[local_name] = Descriptor;

         if (!Document.target_namespace_prefix.empty()) {
            elements[make_qualified_name(Document.target_namespace_prefix, local_name)] = Descriptor;
         }

         for (const auto &[prefix, ns_uri] : Document.namespace_bindings) {
            if (ns_uri IS Document.target_namespace) {
               if (prefix.empty()) elements[local_name] = Descriptor;
               else elements[make_qualified_name(prefix, local_name)] = Descriptor;
            }
         }
      }
   }

   SchemaDocument::SchemaDocument()
      : context(std::make_shared<SchemaContext>())
   {
   }

   // Populates the schema context with the type descriptors declared in the document.
   void SchemaDocument::merge_types()
   {
      if (!context) context = std::make_shared<SchemaContext>();

      for (const auto &Descriptor : declared_types) {
         if (!Descriptor) continue;

         auto &types = context->types;
         types[Descriptor->type_name] = Descriptor;

         auto local_name = std::string(extract_local_name(Descriptor->type_name));
         if (!local_name.empty()) types[local_name] = Descriptor;

         if (!target_namespace_prefix.empty()) {
            types[make_qualified_name(target_namespace_prefix, local_name)] = Descriptor;
         }

         for (const auto &[prefix, ns_uri] : namespace_bindings) {
            if (ns_uri IS target_namespace) {
               if (prefix.empty()) types[local_name] = Descriptor;
               else types[make_qualified_name(prefix, local_name)] = Descriptor;
            }
         }
      }
   }

   // Resets the document and its context to an empty state ready for reuse.
   void SchemaDocument::clear()
   {
      if (context) {
         context->target_namespace.clear();
         context->schema_prefix.clear();
         context->target_namespace_prefix.clear();
         context->namespace_bindings.clear();
         context->types.clear();
         context->complex_types.clear();
         context->elements.clear();
      }

      target_namespace.clear();
      schema_prefix.clear();
      target_namespace_prefix.clear();
      namespace_bindings.clear();
      declared_types.clear();
   }

   bool SchemaDocument::empty() const noexcept
   {
      if (!context) return true;
      return declared_types.empty() and context->elements.empty() and context->complex_types.empty();
   }

   SchemaParser::SchemaParser(SchemaTypeRegistry &Registry)
      : registry_ref(&Registry)
   {
   }

   SchemaDocument SchemaParser::parse(const objXML::TAGS &Tags) const
   {
      SchemaDocument Document;
      if (Tags.empty()) return Document;
      return parse(Tags[0]);
   }

   // Parses an XML schema root node into a schema document with context and descriptors.
   SchemaDocument SchemaParser::parse(const XTag &Root) const
   {
      SchemaDocument Document;
      if (Root.Attribs.empty()) return Document;
      if (!Document.context) Document.context = std::make_shared<SchemaContext>();

      std::string_view root_name(Root.Attribs[0].Name);
      auto colon = root_name.find(':');
      if (colon != std::string::npos) Document.schema_prefix.assign(root_name.begin(), root_name.begin() + colon);
      else Document.schema_prefix.clear();

      for (size_t index = 1u; index < Root.Attribs.size(); ++index) {
         const auto &Attrib = Root.Attribs[index];
         if (pf::iequals(Attrib.Name, "targetNamespace")) {
            Document.target_namespace = Attrib.Value;
            continue;
         }

         std::string_view attrib_name(Attrib.Name);
         if (attrib_name.starts_with("xmlns")) {
            std::string prefix;
            if ((attrib_name.length() > 5u) and (attrib_name[5] IS ':')) {
               prefix.assign(attrib_name.begin() + 6u, attrib_name.end());
            }
            Document.namespace_bindings[prefix] = Attrib.Value;
            if ((Document.target_namespace_prefix.empty()) and (Attrib.Value IS Document.target_namespace)) {
               Document.target_namespace_prefix = prefix;
            }
         }
      }

      if (Document.target_namespace_prefix.empty()) {
         for (const auto &[prefix, ns_uri] : Document.namespace_bindings) {
            if (ns_uri IS Document.target_namespace) {
               Document.target_namespace_prefix = prefix;
               break;
            }
         }
      }

      if (Document.context) {
         Document.context->target_namespace = Document.target_namespace;
         Document.context->schema_prefix = Document.schema_prefix;
         Document.context->target_namespace_prefix = Document.target_namespace_prefix;
         Document.context->namespace_bindings = Document.namespace_bindings;
      }

      for (const auto &Child : Root.Children) {
         if (Child.Attribs.empty()) continue;

         std::string_view child_name(Child.Attribs[0].Name);
         auto local_name = extract_local_name(child_name);

         if (is_named(local_name, "simpleType")) {
            parse_simple_type(Child, Document);
            continue;
         }

         if (is_named(local_name, "complexType")) {
            parse_complex_type(Child, Document);
            continue;
         }

         if (is_named(local_name, "element")) {
            parse_element(Child, Document);
         }
      }

      Document.merge_types();
      return Document;
   }

   std::shared_ptr<SchemaContext> SchemaParser::parse_context(const XTag &Root) const
   {
      auto Document = parse(Root);
      return Document.context;
   }

   // Extracts a named simple type definition and records it against the document.
   void SchemaParser::parse_simple_type(const XTag &Node, SchemaDocument &Document) const
   {
      auto declared_name = find_attribute_value(Node, "name");
      if (declared_name.empty()) return;

      std::string base_name;
      for (const auto &Child : Node.Children) {
         if (Child.Attribs.empty()) continue;
         auto child_local = extract_local_name(Child.Attribs[0].Name);
         if (!is_named(child_local, "restriction")) continue;

         base_name = find_attribute_value(Child, "base");
         break;
      }

      auto BaseDescriptor = resolve_type(base_name, Document);
      if (!BaseDescriptor) BaseDescriptor = registry_ref->find_descriptor(SchemaType::XSAnyType);

      auto qualified_name = Document.target_namespace_prefix.empty()
         ? declared_name
         : make_qualified_name(Document.target_namespace_prefix, declared_name);

      auto Descriptor = std::make_shared<SchemaTypeDescriptor>(SchemaType::UserDefined, qualified_name,
         Document.target_namespace, declared_name, BaseDescriptor, false);
      Document.declared_types.push_back(Descriptor);

      auto &types = Document.context->types;
      types[qualified_name] = Descriptor;
      types[declared_name] = Descriptor;
   }

   // Extracts a named complex type definition and stores its descriptor in the document context.
   void SchemaParser::parse_complex_type(const XTag &Node, SchemaDocument &Document) const
   {
      auto declared_name = find_attribute_value(Node, "name");
      if (declared_name.empty()) return;

      auto Descriptor = std::make_shared<ElementDescriptor>();
      Descriptor->name = declared_name;
      Descriptor->qualified_name = Document.target_namespace_prefix.empty()
         ? declared_name
         : make_qualified_name(Document.target_namespace_prefix, declared_name);

      Descriptor->children.clear();
      parse_inline_complex_type(Node, Document, *Descriptor);

      Document.context->complex_types[Descriptor->name] = Descriptor;
      Document.context->complex_types[Descriptor->qualified_name] = Descriptor;
   }

   // Parses a top-level element definition and resolves its associated type information.
   void SchemaParser::parse_element(const XTag &Node, SchemaDocument &Document) const
   {
      auto declared_name = find_attribute_value(Node, "name");
      if (declared_name.empty()) return;

      auto Descriptor = std::make_shared<ElementDescriptor>();
      Descriptor->name = declared_name;
      Descriptor->qualified_name = Document.target_namespace_prefix.empty()
         ? declared_name
         : make_qualified_name(Document.target_namespace_prefix, declared_name);

      auto type_name = find_attribute_value(Node, "type");
      Descriptor->type_name = type_name;

      if (!type_name.empty()) {
         Descriptor->type = resolve_type(type_name, Document);

         auto complex_it = Document.context->complex_types.find(type_name);
         if (complex_it != Document.context->complex_types.end()) Descriptor->children = complex_it->second->children;
         else {
            auto local_name = std::string(extract_local_name(type_name));
            complex_it = Document.context->complex_types.find(local_name);
            if (complex_it != Document.context->complex_types.end()) Descriptor->children = complex_it->second->children;
         }
      }
      else {
         for (const auto &Child : Node.Children) {
            if (Child.Attribs.empty()) continue;

            auto child_local = extract_local_name(Child.Attribs[0].Name);
            if (is_named(child_local, "complexType")) {
               Descriptor->children.clear();
               parse_inline_complex_type(Child, Document, *Descriptor);
            }
            else if (is_named(child_local, "simpleType")) {
               auto base_name = std::string();
               for (const auto &SimpleChild : Child.Children) {
                  if (SimpleChild.Attribs.empty()) continue;
                  auto simple_local = extract_local_name(SimpleChild.Attribs[0].Name);
                  if (!is_named(simple_local, "restriction")) continue;

                  base_name = find_attribute_value(SimpleChild, "base");
                  break;
               }

               if (!base_name.empty()) Descriptor->type = resolve_type(base_name, Document);
            }
         }
      }

      register_element_aliases(Document, Descriptor);
   }

   // Processes inline complexType definitions embedded within other schema elements.
   void SchemaParser::parse_inline_complex_type(const XTag &Node, SchemaDocument &Document, ElementDescriptor &Descriptor) const
   {
      for (const auto &Child : Node.Children) {
         if (Child.Attribs.empty()) continue;

         auto child_local = extract_local_name(Child.Attribs[0].Name);
         if (is_named(child_local, "sequence")) {
            parse_sequence(Child, Document, Descriptor);
            continue;
         }

         if (is_named(child_local, "complexContent")) {
            for (const auto &ContentChild : Child.Children) {
               if (ContentChild.Attribs.empty()) continue;

               auto content_local = extract_local_name(ContentChild.Attribs[0].Name);
               if (is_named(content_local, "extension") or is_named(content_local, "restriction")) {
                  auto base_name = find_attribute_value(ContentChild, "base");
                  if (!base_name.empty()) Descriptor.type = resolve_type(base_name, Document);

                  for (const auto &ExtensionChild : ContentChild.Children) {
                     if (ExtensionChild.Attribs.empty()) continue;
                     auto extension_local = extract_local_name(ExtensionChild.Attribs[0].Name);
                     if (is_named(extension_local, "sequence")) parse_sequence(ExtensionChild, Document, Descriptor);
                  }
               }
            }
         }
      }
   }

   // Adds element descriptors defined within a sequence child of a complex type.
   void SchemaParser::parse_sequence(const XTag &Node, SchemaDocument &Document, ElementDescriptor &Descriptor) const
   {
      for (const auto &SequenceChild : Node.Children) {
         if (SequenceChild.Attribs.empty()) continue;

         auto seq_local = extract_local_name(SequenceChild.Attribs[0].Name);
         if (!is_named(seq_local, "element")) continue;

         auto element_descriptor = parse_child_element_descriptor(SequenceChild, Document);
         if (!element_descriptor) continue;

         Descriptor.children.push_back(element_descriptor);
      }
   }

   // Builds a descriptor for a child element, resolving occurrence constraints and type information.
   std::shared_ptr<ElementDescriptor> SchemaParser::parse_child_element_descriptor(const XTag &Node,
                                                                                   SchemaDocument &Document) const
   {
      auto element_name = find_attribute_value(Node, "name");
      if (element_name.empty()) return nullptr;

      auto element_descriptor = std::make_shared<ElementDescriptor>();
      element_descriptor->name = element_name;
      element_descriptor->qualified_name = Document.target_namespace_prefix.empty()
         ? element_name
         : make_qualified_name(Document.target_namespace_prefix, element_name);

      element_descriptor->min_occurs = parse_occurs_value(find_attribute_value(Node, "minOccurs"), 1u, false);
      element_descriptor->max_occurs = parse_occurs_value(find_attribute_value(Node, "maxOccurs"), 1u, true);

      auto type_name = find_attribute_value(Node, "type");
      element_descriptor->type_name = type_name;

      if (!type_name.empty()) {
         element_descriptor->type = resolve_type(type_name, Document);

         auto complex_it = Document.context->complex_types.find(type_name);
         if (complex_it != Document.context->complex_types.end()) element_descriptor->children = complex_it->second->children;
         else {
            auto local_name = std::string(extract_local_name(type_name));
            complex_it = Document.context->complex_types.find(local_name);
            if (complex_it != Document.context->complex_types.end()) element_descriptor->children = complex_it->second->children;
         }
      }

      for (const auto &Child : Node.Children) {
         if (Child.Attribs.empty()) continue;

         auto child_local = extract_local_name(Child.Attribs[0].Name);
         if (is_named(child_local, "complexType")) {
            element_descriptor->children.clear();
            parse_inline_complex_type(Child, Document, *element_descriptor);
         }
         else if (is_named(child_local, "simpleType") and element_descriptor->type_name.empty()) {
            auto base_name = std::string();
            for (const auto &SimpleChild : Child.Children) {
               if (SimpleChild.Attribs.empty()) continue;

               auto simple_local = extract_local_name(SimpleChild.Attribs[0].Name);
               if (!is_named(simple_local, "restriction")) continue;

               base_name = find_attribute_value(SimpleChild, "base");
               break;
            }

            if (!base_name.empty()) element_descriptor->type = resolve_type(base_name, Document);
         }
      }

      return element_descriptor;
   }

   // Resolves the named schema type using the document context and registry fallbacks.
   std::shared_ptr<SchemaTypeDescriptor> SchemaParser::resolve_type(std::string_view Name, SchemaDocument &Document) const
   {
      if (Name.empty()) return registry_ref->find_descriptor(SchemaType::XSAnyType);

      auto lookup_name = std::string(Name);
      auto &types = Document.context->types;
      auto iter = types.find(lookup_name);
      if (iter != types.end()) return iter->second;

      auto descriptor = registry_ref->find_descriptor(Name);
      if (descriptor) return descriptor;

      auto local_name = std::string(extract_local_name(Name));
      iter = types.find(local_name);
      if (iter != types.end()) return iter->second;

      descriptor = registry_ref->find_descriptor(local_name);
      if (descriptor) return descriptor;

      return registry_ref->find_descriptor(SchemaType::XSAnyType);
   }

   std::string_view extract_local_name(std::string_view Qualified) noexcept
   {
      auto colon = Qualified.find(':');
      if (colon != std::string::npos) return Qualified.substr(colon + 1u);
      return Qualified;
   }
}
