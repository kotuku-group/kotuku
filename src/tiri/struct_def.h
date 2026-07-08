#pragma once

#include <string>
#include <vector>
#include <string_view>
#include <kotuku/strings.hpp>

struct struct_field {
   std::string Name;      // Field name
   std::string StructRef; // Named reference to other structure
   uint16_t Offset = 0;   // Offset to the field value.
   int  Type      = 0;    // FD flags
   int  ArraySize = 0;    // Set if the field is an array

   uint32_t nameHash() {
      if (!NameHash) NameHash = kt::strihash(Name);
      return NameHash;
   }

   private:
   uint32_t NameHash = 0;     // Lowercase hash of the field name
};

struct struct_record {
   std::string Name;
   std::vector<struct_field> Fields;
   int Size = 0; // Total byte size of the structure
   struct_record(std::string_view pName) : Name(pName) { }
   struct_record() = default;
};

//********************************************************************************************************************
// Structure names have their own handler due to the use of colons in struct references, i.e. "OfficialStruct:SomeName"

[[nodiscard]] inline bool struct_name_hash_char(char Value) noexcept
{
   auto c = uint8_t(Value);
   if ((c >= 'A') and (c <= 'Z')) return true;
   else if ((c >= 'a') and (c <= 'z')) return true;
   else if ((c >= '0') and (c <= '9')) return true;
   else return false;
}

[[nodiscard]] inline std::string_view struct_name_hash_prefix(std::string_view Name) noexcept
{
   size_t length = 0;
   while ((length < Name.size()) and (struct_name_hash_char(Name[length]))) length++;
   return Name.substr(0, length);
}

struct struct_name {
   std::string name;
   struct_name(const std::string_view pName) {
      auto colon = pName.find(':');

      if (colon IS std::string::npos) name = pName;
      else name = pName.substr(0, colon);
   }

   bool operator==(const std::string_view &other) const {
      return (name == other);
   }

   bool operator==(const struct_name &other) const {
      return (name == other.name);
   }
};

struct struct_hash { // Stops when an invalid character is encountered (typically a colon separator)
   using is_transparent = void; // Enables heterogeneous string_view lookups in std::unordered_map

   std::size_t operator()(const struct_name &Key) const {
      return kt::strhash(struct_name_hash_prefix(Key.name));
   }

   std::size_t operator()(const std::string_view Key) const {
      return kt::strhash(struct_name_hash_prefix(Key));
   }
};

struct struct_equal { // Transparent comparator supporting struct_name and string_view keys
   using is_transparent = void;

   // A raw string_view may carry a colon-delimited suffix (e.g. "TimeZoneInfo:Info").  Struct keys only
   // retain the portion before the colon, so the suffix must be stripped before comparison to mirror the
   // truncation performed by the struct_name constructor.
   static std::string_view prefix(const std::string_view Name) {
      auto colon = Name.find(':');
      return (colon IS std::string_view::npos) ? Name : Name.substr(0, colon);
   }

   bool operator()(const struct_name &a, const struct_name &b) const { return a == b; }
   bool operator()(const struct_name &a, const std::string_view b) const { return a == prefix(b); }
   bool operator()(const std::string_view a, const struct_name &b) const { return b == prefix(a); }
};
