// schema_types.h - Defines the SchemaType enumeration, descriptor classes, and registry interface
// that capture XML Schema datatype semantics for the wider XML subsystem.  Consumers include the
// schema parser, type checker, and XPath integration points that require quick lookup of built-in
// and user-defined types, inheritance relationships, and value coercion behaviours.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <ankerl/unordered_dense.h>
#include <kotuku/modules/xml.h>
#include "../xpath_value.h"

namespace xml::schema
{
   enum class SchemaType
   {
      XPathNodeSet,
      XPathBoolean,
      XPathNumber,
      XPathString,
      XSAnyType,
      XSString,
      XSBoolean,
      XSDecimal,
      XSFloat,
      XSDouble,
      XSDuration,
      XSDateTime,
      XSTime,
      XSDate,
      XSInteger,
      XSLong,
      XSInt,
      XSShort,
      XSByte,
      XSQName,
      UserDefined
   };

   class SchemaTypeDescriptor : public std::enable_shared_from_this<SchemaTypeDescriptor>
   {
      private:
      std::weak_ptr<SchemaTypeDescriptor> base_type;
      bool builtin_type;
      uint32_t constructor_arity;
      bool namespace_sensitive;

      public:
      SchemaType schema_type;
      std::string type_name;
      std::string namespace_uri;
      std::string local_name;

      SchemaTypeDescriptor(SchemaType Type, std::string Name, std::string NamespaceURI, std::string LocalName,
                           std::shared_ptr<SchemaTypeDescriptor> Base = nullptr, bool Builtin = false,
                           uint32_t ConstructorArity = 1u, bool NamespaceSensitive = false);

      [[nodiscard]] std::shared_ptr<SchemaTypeDescriptor> base() const;
      [[nodiscard]] bool is_builtin() const noexcept;
      [[nodiscard]] uint32_t arity() const noexcept;
      [[nodiscard]] bool is_namespace_sensitive() const noexcept;
      [[nodiscard]] bool is_derived_from(SchemaType Target) const;
      [[nodiscard]] bool can_coerce_to(SchemaType Target) const;
      [[nodiscard]] XPathVal coerce_value(const XPathVal &Value, SchemaType Target) const;
   };

   class SchemaTypeRegistry
   {
      private:
      ankerl::unordered_dense::map<SchemaType, std::shared_ptr<SchemaTypeDescriptor>> descriptors_by_type;
      ankerl::unordered_dense::map<std::string, std::shared_ptr<SchemaTypeDescriptor>> descriptors_by_name;
      ankerl::unordered_dense::map<std::string, std::shared_ptr<SchemaTypeDescriptor>> descriptors_by_expanded_name;

      void register_builtin_types();

      public:
      SchemaTypeRegistry();

      std::shared_ptr<SchemaTypeDescriptor> register_descriptor(SchemaType Type, std::string Name,
         std::string NamespaceURI, std::string LocalName, std::shared_ptr<SchemaTypeDescriptor> Base = nullptr,
         bool Builtin = false, uint32_t ConstructorArity = 1u, bool NamespaceSensitive = false);
      [[nodiscard]] std::shared_ptr<SchemaTypeDescriptor> find_descriptor(SchemaType Type) const;
      [[nodiscard]] std::shared_ptr<SchemaTypeDescriptor> find_descriptor(std::string_view Name) const;
      [[nodiscard]] std::shared_ptr<SchemaTypeDescriptor> find_descriptor(std::string_view NamespaceURI,
         std::string_view LocalName) const;
      [[nodiscard]] bool namespace_contains_types(std::string_view NamespaceURI) const;
      void clear();
   };

   SchemaTypeRegistry & registry();

   [[nodiscard]] bool is_numeric(SchemaType Type) noexcept;
   [[nodiscard]] bool is_string_like(SchemaType Type) noexcept;
   [[nodiscard]] bool is_duration(SchemaType Type) noexcept;
   [[nodiscard]] bool is_date_or_time(SchemaType Type) noexcept;
   [[nodiscard]] bool is_namespace_sensitive(SchemaType Type) noexcept;
   [[nodiscard]] SchemaType schema_type_for_xpath(XPVT Type) noexcept;
}
