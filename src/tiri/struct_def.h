#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <string_view>
#include <kotuku/strings.hpp>
#include <kotuku/system/registry.h>

enum class NativeStructType : uint8_t {
   Legacy,
   Bool,
   Char,
   Int8,
   UInt8,
   Int16,
   UInt16,
   Int32,
   UInt32,
   Int64,
   UInt64,
   Float,
   Double,
   String,
   CStr,
   Pointer,
   Struct,
   Object,
   Function
};

struct struct_record;

struct struct_field {
   std::string Name;      // Field name
   uint32_t StructRef = 0; // struct_key() of a referenced structure; 0 = no reference
   CLASSID ObjectClassID = CLASSID::NIL; // Optional class constraint for obj<Class> fields
   struct_record *StructDefinition = nullptr; // Resolved definition; registry ownership remains external
   uint16_t Offset = 0;   // Offset to the field value.
   int  Type      = 0;    // FD flags
   int  ArraySize = 0;    // Set if the field is an array
   uint16_t ElementStride = 0; // Byte stride for dynamically sized struct elements
   bool TrivialElements = false; // Struct vectors may use type-erased ownership only when validated as trivial
   NativeStructType NativeType = NativeStructType::Legacy;

   void precomputeNameHash() { NameHash = kt::strihash(Name); }
   [[nodiscard]] uint32_t nameHash() const { return NameHash; }

   private:
   uint32_t NameHash = 0;     // Lowercase hash of the field name
};

struct struct_record {
   std::string Name;
   std::vector<struct_field> Fields;
   int Size = 0; // Total byte size of the structure
   int Alignment = 1; // Strictest native member alignment, including tail padding
   std::string DeclarationSource;
   uint32_t DeclarationLine = 0;
   struct_record(std::string_view pName) : Name(pName) { }
   struct_record() = default;
};

//********************************************************************************************************************
// Struct references may include a colon-delimited field suffix, i.e. "OfficialStruct:SomeName".

[[nodiscard]] constexpr inline std::string_view struct_name_prefix(std::string_view Name) noexcept
{
   auto colon = Name.find(':');
   return (colon IS std::string_view::npos) ? Name : Name.substr(0, colon);
}

[[nodiscard]] constexpr inline bool valid_struct_name(std::string_view Name) noexcept
{
   Name = struct_name_prefix(Name);
   if (Name.empty()) return false;

   auto first = uint8_t(Name.front());
   if (not (((first >= 'A') and (first <= 'Z')) or ((first >= 'a') and (first <= 'z')))) return false;

   for (auto value : Name.substr(1)) {
      auto c = uint8_t(value);
      if (not (((c >= 'A') and (c <= 'Z')) or ((c >= 'a') and (c <= 'z')) or
            ((c >= '0') and (c <= '9')))) return false;
   }
   return true;
}

// Struct names are case-sensitive.  Field names remain case-insensitive via struct_field::precomputeNameHash().

[[nodiscard]] constexpr inline uint32_t struct_key(std::string_view Name) noexcept
{
   return kt::strhash(struct_name_prefix(Name));
}
