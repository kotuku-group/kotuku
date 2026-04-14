// type_checker.h - Declares the TypeChecker façade used by callers to validate XML instance data
// against schema-derived descriptors.  It exposes context wiring, error reporting, and per-value or
// per-element verification entry points that integrate with the schema parser and type registry to
// deliver comprehensive XSD enforcement inside the XML module.

#pragma once

#include "schema_parser.h"
#include <string>

namespace xml::schema
{
   class TypeChecker
   {
      private:
      SchemaTypeRegistry * registry_ref;
      const SchemaContext * context_ref;
      std::string * error_sink;
      mutable std::string last_error_message;

      void assign_error(std::string Message) const;

      public:
      explicit TypeChecker(SchemaTypeRegistry &Registry, const SchemaContext *Context = nullptr,
                           std::string *ErrorSink = nullptr);

      void set_context(const SchemaContext *Context);
      void set_error_sink(std::string *ErrorSink);
      void clear_error() const;
      [[nodiscard]] const SchemaContext * schema_context() const;
      [[nodiscard]] const std::string & last_error() const;

      [[nodiscard]] bool validate_value(const XPathVal &Value, const SchemaTypeDescriptor &Descriptor) const;
      [[nodiscard]] bool validate_attribute(const XMLAttrib &Attribute, const SchemaTypeDescriptor &Descriptor) const;
      [[nodiscard]] bool validate_node(const XTag &Tag, const SchemaTypeDescriptor &Descriptor) const;
      [[nodiscard]] bool validate_element(const XTag &Tag, const ElementDescriptor &Descriptor) const;
      [[nodiscard]] SchemaTypeRegistry & registry() const;
   };
}
