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
      if (!NameHash) NameHash = pf::strihash(Name);
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
   std::size_t operator()(const struct_name &k) const {
      uint32_t hash = 5381;
      for (auto c : k.name) {
         if ((c >= 'A') and (c <= 'Z'));
         else if ((c >= 'a') and (c <= 'z'));
         else if ((c >= '0') and (c <= '9'));
         else break;
         hash = ((hash<<5) + hash) + uint8_t(c);
      }
      return hash;
   }

   std::size_t operator()(const std::string_view k) const {
      uint32_t hash = 5381;
      for (auto c : k) {
         if ((c >= 'A') and (c <= 'Z'));
         else if ((c >= 'a') and (c <= 'z'));
         else if ((c >= '0') and (c <= '9'));
         else break;
         hash = ((hash<<5) + hash) + uint8_t(c);
      }
      return hash;
   }
};
