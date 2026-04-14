// schema_types.cpp - Provides the backing logic for schema type descriptors and their registry,
// modelling the primitive, derived, and user-defined datatypes referenced by XML Schema documents.
// The functions here create descriptors, maintain inheritance relationships, and expose coercion
// helpers so that other parts of the XML stack can interpret values in the context of XSD typing
// rules.

#include "schema_types.h"
#include "../xml.h"

namespace xml::schema
{
   namespace
   {
      constexpr std::string_view xml_schema_namespace_uri("http://www.w3.org/2001/XMLSchema");
      constexpr std::string_view xpath_functions_namespace_uri("http://www.w3.org/2005/xpath-functions");

      // Creates a descriptor instance with the supplied metadata for registration.

      std::shared_ptr<SchemaTypeDescriptor> make_descriptor(SchemaType Type, std::string Name,
         std::string NamespaceURI, std::string LocalName, const std::shared_ptr<SchemaTypeDescriptor> &Base, bool Builtin,
         uint32_t ConstructorArity, bool NamespaceSensitive)
      {
         return std::make_shared<SchemaTypeDescriptor>(Type, std::move(Name), std::move(NamespaceURI), std::move(LocalName),
            Base, Builtin, ConstructorArity, NamespaceSensitive);
      }

      [[nodiscard]] std::string make_expanded_key(std::string_view NamespaceURI, std::string_view LocalName)
      {
         if (LocalName.empty()) return std::string();
         std::string key;
         key.reserve(NamespaceURI.size() + LocalName.size() + 1);
         key.append(NamespaceURI);
         key.push_back('\x1F');
         key.append(LocalName);
         return key;
      }

      // Tests whether the provided schema type represents a string-like value.

      constexpr bool is_schema_string(SchemaType Type) noexcept
      {
         switch (Type) {
            case SchemaType::XPathString:
            case SchemaType::XSString:
               return true;
            default:
               return false;
         }
      }

      // Tests whether the provided schema type represents a numeric value category.

      constexpr bool is_schema_numeric(SchemaType Type) noexcept
      {
         switch (Type) {
            case SchemaType::XPathNumber:
            case SchemaType::XSDecimal:
            case SchemaType::XSFloat:
            case SchemaType::XSDouble:
            case SchemaType::XSInteger:
            case SchemaType::XSLong:
            case SchemaType::XSInt:
            case SchemaType::XSShort:
            case SchemaType::XSByte:
               return true;
            default:
               return false;
         }
      }
   }

   SchemaTypeDescriptor::SchemaTypeDescriptor(SchemaType Type, std::string Name, std::string NamespaceURI,
      std::string LocalName, std::shared_ptr<SchemaTypeDescriptor> Base, bool Builtin, uint32_t ConstructorArity,
      bool NamespaceSensitive)
      : base_type(Base), builtin_type(Builtin), constructor_arity(ConstructorArity), namespace_sensitive(NamespaceSensitive),
        schema_type(Type), type_name(std::move(Name)), namespace_uri(std::move(NamespaceURI)),
        local_name(std::move(LocalName))
   {
      if (local_name.empty()) local_name = type_name;
   }

   std::shared_ptr<SchemaTypeDescriptor> SchemaTypeDescriptor::base() const
   {
      return base_type.lock();
   }

   bool SchemaTypeDescriptor::is_builtin() const noexcept
   {
      return builtin_type;
   }

   uint32_t SchemaTypeDescriptor::arity() const noexcept
   {
      return constructor_arity;
   }

   bool SchemaTypeDescriptor::is_namespace_sensitive() const noexcept
   {
      return namespace_sensitive;
   }

   // Determines whether the descriptor ultimately derives from the requested schema type.

   bool SchemaTypeDescriptor::is_derived_from(SchemaType Target) const
   {
      if (schema_type IS Target) return true;
      if (Target IS SchemaType::XSAnyType) return true;

      if (auto Parent = base_type.lock()) {
         return Parent->is_derived_from(Target);
      }

      return false;
   }

   // Reports whether the descriptor can legally coerce values into the requested type.

   bool SchemaTypeDescriptor::can_coerce_to(SchemaType Target) const
   {
      if (schema_type IS Target) return true;
      if (Target IS SchemaType::XSAnyType) return true;

      if (is_schema_numeric(schema_type) and is_schema_numeric(Target)) return true;
      if (is_schema_string(Target)) return true;

      if (auto Parent = base_type.lock()) {
         return Parent->can_coerce_to(Target);
      }

      return false;
   }

   // Converts an XPath value into the requested schema type when permitted.

   XPathVal SchemaTypeDescriptor::coerce_value(const XPathVal &Value, SchemaType Target) const
   {
      if ((schema_type IS Target) or (Target IS SchemaType::XSAnyType)) return Value;

      if ((Target IS SchemaType::XPathBoolean) or (Target IS SchemaType::XSBoolean)) {
         return XPathVal(Value.to_boolean());
      }
      else if (is_schema_numeric(Target)) {
         return XPathVal(Value.to_number());
      }
      else if (is_schema_string(Target)) {
         return XPathVal(Value.to_string());
      }
      else return Value;
   }

   SchemaTypeRegistry::SchemaTypeRegistry()
   {
      register_builtin_types();
   }

   // Registers a descriptor for the given type if one does not already exist.

   std::shared_ptr<SchemaTypeDescriptor> SchemaTypeRegistry::register_descriptor(SchemaType Type, std::string Name,
      std::string NamespaceURI, std::string LocalName, std::shared_ptr<SchemaTypeDescriptor> Base, bool Builtin,
      uint32_t ConstructorArity, bool NamespaceSensitive)
   {
      auto Existing = find_descriptor(Type);
      if (Existing) return Existing;

      auto Descriptor = make_descriptor(Type, std::move(Name), std::move(NamespaceURI), std::move(LocalName), Base, Builtin,
         ConstructorArity, NamespaceSensitive);
      descriptors_by_type.emplace(Type, Descriptor);
      descriptors_by_name.emplace(Descriptor->type_name, Descriptor);

      auto expanded_key = make_expanded_key(Descriptor->namespace_uri, Descriptor->local_name);
      if (!expanded_key.empty()) descriptors_by_expanded_name.emplace(expanded_key, Descriptor);
      return Descriptor;
   }

   std::shared_ptr<SchemaTypeDescriptor> SchemaTypeRegistry::find_descriptor(SchemaType Type) const
   {
      auto Iter = descriptors_by_type.find(Type);
      if (Iter != descriptors_by_type.end()) return Iter->second;
      return nullptr;
   }

   std::shared_ptr<SchemaTypeDescriptor> SchemaTypeRegistry::find_descriptor(std::string_view Name) const
   {
      auto Iter = descriptors_by_name.find(std::string(Name));
      if (Iter != descriptors_by_name.end()) return Iter->second;
      return nullptr;
   }

   std::shared_ptr<SchemaTypeDescriptor> SchemaTypeRegistry::find_descriptor(std::string_view NamespaceURI,
      std::string_view LocalName) const
   {
      auto expanded_key = make_expanded_key(NamespaceURI, LocalName);
      if (expanded_key.empty()) return nullptr;

      auto Iter = descriptors_by_expanded_name.find(expanded_key);
      if (Iter != descriptors_by_expanded_name.end()) return Iter->second;
      return nullptr;
   }

   bool SchemaTypeRegistry::namespace_contains_types(std::string_view NamespaceURI) const
   {
      if (NamespaceURI.empty()) return false;

      std::string key_prefix(NamespaceURI);
      key_prefix.push_back('\x1F');

      for (const auto &entry : descriptors_by_expanded_name) {
         if (entry.first.rfind(key_prefix, 0) IS 0) return true;
      }

      return false;
   }

   void SchemaTypeRegistry::clear()
   {
      descriptors_by_type.clear();
      descriptors_by_name.clear();
      descriptors_by_expanded_name.clear();
   }

   // Populates the registry with the built-in schema types recognised by Kotuku.

   void SchemaTypeRegistry::register_builtin_types()
   {
      clear();

      auto AnyType = register_descriptor(SchemaType::XSAnyType, "xs:anyType", std::string(xml_schema_namespace_uri), "anyType",
         nullptr, true);

      register_descriptor(SchemaType::XPathNodeSet, "xpath:node-set", std::string(xpath_functions_namespace_uri), "node-set",
         nullptr, true, 0u);
      register_descriptor(SchemaType::XPathBoolean, "xpath:boolean", std::string(xpath_functions_namespace_uri), "boolean",
         nullptr, true);
      register_descriptor(SchemaType::XPathNumber, "xpath:number", std::string(xpath_functions_namespace_uri), "number",
         nullptr, true);
      register_descriptor(SchemaType::XPathString, "xpath:string", std::string(xpath_functions_namespace_uri), "string",
         nullptr, true);

      register_descriptor(SchemaType::XSString, "xs:string", std::string(xml_schema_namespace_uri), "string", AnyType, true);
      register_descriptor(SchemaType::XSBoolean, "xs:boolean", std::string(xml_schema_namespace_uri), "boolean", AnyType,
         true);
      auto DecimalType = register_descriptor(SchemaType::XSDecimal, "xs:decimal", std::string(xml_schema_namespace_uri),
         "decimal", AnyType, true);
      auto FloatType = register_descriptor(SchemaType::XSFloat, "xs:float", std::string(xml_schema_namespace_uri), "float",
         DecimalType, true);
      register_descriptor(SchemaType::XSDouble, "xs:double", std::string(xml_schema_namespace_uri), "double", FloatType,
         true);
      register_descriptor(SchemaType::XSDuration, "xs:duration", std::string(xml_schema_namespace_uri), "duration", AnyType,
         true);
      register_descriptor(SchemaType::XSDateTime, "xs:dateTime", std::string(xml_schema_namespace_uri), "dateTime", AnyType,
         true);
      register_descriptor(SchemaType::XSTime, "xs:time", std::string(xml_schema_namespace_uri), "time", AnyType, true);
      register_descriptor(SchemaType::XSDate, "xs:date", std::string(xml_schema_namespace_uri), "date", AnyType, true);
      auto IntegerType = register_descriptor(SchemaType::XSInteger, "xs:integer", std::string(xml_schema_namespace_uri),
         "integer", DecimalType, true);
      auto LongType = register_descriptor(SchemaType::XSLong, "xs:long", std::string(xml_schema_namespace_uri), "long",
         IntegerType, true);
      auto IntType = register_descriptor(SchemaType::XSInt, "xs:int", std::string(xml_schema_namespace_uri), "int", LongType,
         true);
      auto ShortType = register_descriptor(SchemaType::XSShort, "xs:short", std::string(xml_schema_namespace_uri), "short",
         IntType, true);
      register_descriptor(SchemaType::XSByte, "xs:byte", std::string(xml_schema_namespace_uri), "byte", ShortType, true);
      register_descriptor(SchemaType::XSQName, "xs:QName", std::string(xml_schema_namespace_uri), "QName", AnyType, true,
         1u, true);
   }

   SchemaTypeRegistry & registry()
   {
      static SchemaTypeRegistry global_registry;
      return global_registry;
   }

   bool is_numeric(SchemaType Type) noexcept
   {
      return is_schema_numeric(Type);
   }

   bool is_string_like(SchemaType Type) noexcept
   {
      return is_schema_string(Type);
   }

   bool is_duration(SchemaType Type) noexcept
   {
      return Type IS SchemaType::XSDuration;
   }

   bool is_date_or_time(SchemaType Type) noexcept
   {
      switch (Type) {
         case SchemaType::XSDate:
         case SchemaType::XSTime:
         case SchemaType::XSDateTime:
            return true;
         default:
            return false;
      }
   }

   bool is_namespace_sensitive(SchemaType Type) noexcept
   {
      switch (Type) {
         case SchemaType::XSQName:
            return true;
         default:
            break;
      }
      return false;
   }

   // Maps an XPath runtime value type onto the corresponding schema type.
   SchemaType schema_type_for_xpath(XPVT Type) noexcept
   {
      switch (Type) {
         case XPVT::NodeSet:  return SchemaType::XPathNodeSet;
         case XPVT::Boolean:  return SchemaType::XPathBoolean;
         case XPVT::Number:   return SchemaType::XPathNumber;
         case XPVT::String:   return SchemaType::XPathString;
         case XPVT::Date:
         case XPVT::Time:
         case XPVT::DateTime: return SchemaType::XSDateTime;
         case XPVT::Map:
         case XPVT::Array:
            return SchemaType::XPathString;
      }

      return SchemaType::XPathString;
   }
}
