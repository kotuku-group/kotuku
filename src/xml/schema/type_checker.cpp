// type_checker.cpp - Contains the runtime validation engine that applies schema-derived type and
// element descriptors to concrete XML instance data.  The implementation cross-references the
// registry, performs value coercion checks, and surfaces detailed diagnostic messages so that
// callers can enforce XSD constraints when loading or manipulating documents through Kōtuku's XML
// facilities.

#include "type_checker.h"
#include <cmath>
#include <format>
#include <limits>

namespace xml::schema
{
   namespace
   {
      // Follows user-defined types to find the underlying built-in descriptor.
      const SchemaTypeDescriptor * resolve_effective_descriptor(const SchemaTypeDescriptor &Descriptor)
      {
         const SchemaTypeDescriptor *current = &Descriptor;
         while (current and (current->schema_type IS SchemaType::UserDefined)) {
            auto base = current->base();
            if (!base) break;
            current = base.get();
         }
         return current ? current : &Descriptor;
      }

      // Determines whether the supplied string value represents a valid boolean literal.
      bool is_valid_boolean(std::string_view Value)
      {
         if (pf::iequals(Value, "true")) return true;
         if (pf::iequals(Value, "false")) return true;
         if (Value.length() IS 1) return (Value[0] IS '0') or (Value[0] IS '1');
         return false;
      }
   }

   TypeChecker::TypeChecker(SchemaTypeRegistry &Registry, const SchemaContext *Context, std::string *ErrorSink)
      : registry_ref(&Registry), context_ref(Context), error_sink(ErrorSink)
   {
   }

   void TypeChecker::set_context(const SchemaContext *Context)
   {
      context_ref = Context;
   }

   void TypeChecker::set_error_sink(std::string *ErrorSink)
   {
      error_sink = ErrorSink;
      if (error_sink and not last_error_message.empty()) *error_sink = last_error_message;
   }

   void TypeChecker::clear_error() const
   {
      last_error_message.clear();
      if (error_sink) error_sink->clear();
   }

   const SchemaContext * TypeChecker::schema_context() const
   {
      return context_ref;
   }

   const std::string & TypeChecker::last_error() const
   {
      return last_error_message;
   }

   void TypeChecker::assign_error(std::string Message) const
   {
      last_error_message.assign(std::move(Message));
      if (error_sink) *error_sink = last_error_message;
   }

   // Validates that the provided XPath value conforms to the supplied schema descriptor.
   bool TypeChecker::validate_value(const XPathVal &Value, const SchemaTypeDescriptor &Descriptor) const
   {
      auto effective = resolve_effective_descriptor(Descriptor);
      auto target_type = effective->schema_type;

      if (Value.has_schema_info())
      {
         auto annotated_type = Value.get_schema_type();
         auto &registry_ref = registry();
         auto annotated_descriptor = registry_ref.find_descriptor(annotated_type);

         if (annotated_descriptor)
         {
            if ((annotated_type IS target_type) or annotated_descriptor->is_derived_from(target_type) or
                effective->is_derived_from(annotated_type))
            {
               return true;
            }
         }
      }

      if (is_numeric(target_type)) {
         auto coerced = effective->coerce_value(Value, target_type);
         if (!std::isnan(coerced.to_number())) return true;
         assign_error(std::format("Value '{}' is not valid for type {}", Value.to_string(), effective->type_name));
         return false;
      }

      if ((target_type IS SchemaType::XPathBoolean) or (target_type IS SchemaType::XSBoolean)) {
         if (Value.Type IS XPVT::Boolean) return true;
         auto string_value = Value.to_string();
         if (is_valid_boolean(string_value)) return true;

         assign_error(std::format("Value '{}' is not a recognised boolean value.", string_value));
         return false;
      }

      if ((target_type IS SchemaType::XPathString) or (target_type IS SchemaType::XSString)) return true;
      if (target_type IS SchemaType::XPathNodeSet) {
         if (Value.Type IS XPVT::NodeSet) return true;
         assign_error("Expected a node-set value.");
         return false;
      }

      auto SourceType = schema_type_for_xpath(Value.Type);
      auto SourceDescriptor = registry().find_descriptor(SourceType);
      if (not SourceDescriptor) {
         assign_error("Unsupported value type for schema coercion.");
         return false;
      }

      if (SourceDescriptor->can_coerce_to(Descriptor.schema_type)) return true;

      assign_error(std::format("Cannot coerce value of type {} to required type {}.", SourceDescriptor->type_name, Descriptor.type_name));
      return false;
   }

   // Validates an attribute against the descriptor and records detailed errors when it fails.
   bool TypeChecker::validate_attribute(const XMLAttrib &Attribute, const SchemaTypeDescriptor &Descriptor) const
   {
      XPathVal value(Attribute.Value);
      if (validate_value(value, Descriptor)) return true;

      std::string previous_error(last_error_message);
      auto attr_name = !Attribute.Name.empty() ? Attribute.Name : "(unnamed)";

      if (!previous_error.empty()) assign_error(std::format("Attribute {}: {}", attr_name, previous_error));
      else assign_error(std::format("Attribute {}: Value does not match expected type {}", attr_name, Descriptor.type_name));

      return false;
   }

   // Validates that the tag node satisfies the structural requirements of the descriptor.
   bool TypeChecker::validate_node(const XTag &Tag, const SchemaTypeDescriptor &Descriptor) const
   {
      if (Descriptor.schema_type IS SchemaType::XPathNodeSet) return true;
      if (Descriptor.can_coerce_to(SchemaType::XPathString)) {
         if (Tag.hasContent()) return true;
         if (not Tag.Children.empty()) return true;

         auto elem_name = (!Tag.Attribs.empty() and !Tag.Attribs[0].Name.empty()) ? Tag.Attribs[0].Name : "(unnamed)";
         assign_error(std::format("Element {} is missing required textual content.", elem_name));
         return false;
      }
      if (Descriptor.can_coerce_to(SchemaType::XPathNodeSet)) return true;

      assign_error("Element does not satisfy required node constraints.");
      return false;
   }

   // Validates an element against the descriptor, recursively checking child elements as required.
   bool TypeChecker::validate_element(const XTag &Tag, const ElementDescriptor &Descriptor) const
   {
      if (Descriptor.type and Descriptor.children.empty()) {
         XPathVal value(Tag.getContent());
         if (validate_value(value, *Descriptor.type)) return true;

         std::string previous_error(last_error_message);
         auto elem_name = (!Tag.Attribs.empty() and !Tag.Attribs[0].Name.empty()) ? Tag.Attribs[0].Name : "(unnamed)";

         if (!previous_error.empty()) assign_error(std::format("Element {}: {}", elem_name, previous_error));
         else assign_error(std::format("Element {}: Content does not match expected type {}",elem_name, Descriptor.type_name));

         return false;
      }

      if (Descriptor.children.empty()) return true;

      ankerl::unordered_dense::map<const ElementDescriptor *, size_t> counters;
      ankerl::unordered_dense::map<std::string, const ElementDescriptor *> lookup;

      for (const auto &child : Descriptor.children) {
         if (!child) continue;
         lookup[child->name] = child.get();
         if (!child->qualified_name.empty()) lookup[child->qualified_name] = child.get();

         auto local_name = std::string(extract_local_name(child->qualified_name.empty() ? child->name : child->qualified_name));
         if (!local_name.empty()) lookup[local_name] = child.get();
         counters[child.get()] = 0u;
      }

      for (const auto &Child : Tag.Children) {
         if (Child.Attribs.empty()) continue;
         if (Child.Attribs[0].isContent()) continue;

         std::string_view child_name(Child.Attribs[0].Name);
         const ElementDescriptor *rule = nullptr;

         auto iter = lookup.find(std::string(child_name));
         if (iter != lookup.end()) rule = iter->second;
         else {
            auto local_name = std::string(extract_local_name(child_name));
            auto local_iter = lookup.find(local_name);
            if (local_iter != lookup.end()) rule = local_iter->second;
         }

         if (!rule) {
            auto elem_name = (!Tag.Attribs.empty() and !Tag.Attribs[0].Name.empty()) ? Tag.Attribs[0].Name : "(unnamed)";
            assign_error(std::format("Element {} contains unexpected child element {}", elem_name, child_name));
            return false;
         }

         counters[rule]++;

         if (rule->type and rule->children.empty()) {
            XPathVal child_value(Child.getContent());
            if (!validate_value(child_value, *rule->type)) {
               std::string previous_error(last_error_message);
               auto child_elem_name = (!Child.Attribs.empty() and !Child.Attribs[0].Name.empty()) ? Child.Attribs[0].Name : "(unnamed)";

               if (!previous_error.empty()) assign_error(std::format("Element {}: {}", child_elem_name, previous_error));
               else assign_error(std::format("Element {}: Content does not match expected type {}", child_elem_name, rule->type->type_name));

               return false;
            }
         }
         else if (!rule->children.empty()) {
            if (!validate_element(Child, *rule)) return false;
         }
      }

      for (const auto &child : Descriptor.children) {
         if (!child) continue;
         size_t count = counters[child.get()];
         auto elem_name = (!Tag.Attribs.empty() and !Tag.Attribs[0].Name.empty()) ? Tag.Attribs[0].Name : "(unnamed)";

         if (count < child->min_occurs) {
            assign_error(std::format("Element {} is missing required child element {} (expected at least {}).", elem_name, child->name, child->min_occurs));
            return false;
         }
         if ((child->max_occurs != std::numeric_limits<size_t>::max()) and (count > child->max_occurs)) {
            assign_error(std::format("Element {} contains too many {} elements (maximum allowed is {}).", elem_name, child->name, child->max_occurs));
            return false;
         }
      }

      return true;
   }

   SchemaTypeRegistry & TypeChecker::registry() const
   {
      return *registry_ref;
   }
}
