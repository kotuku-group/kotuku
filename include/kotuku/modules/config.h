#pragma once

// Copyright: Paul Manias © 1996-2026
// Generator: idl-c

#include <kotuku/main.h>

class objConfig;

typedef KEYVALUE ConfigKeys;
typedef std::pair<std::string, ConfigKeys> ConfigGroup;
typedef std::vector<ConfigGroup> ConfigGroups;

// Config class definition

#define VER_CONFIG (1.000000)

// Config methods

namespace cfg {
struct ReadValue { std::string_view Group; std::string_view Key; std::string_view Data; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Set { std::string_view Group; std::string_view Key; std::string_view Data; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct WriteValue { std::string_view Group; std::string_view Key; std::string_view Data; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DeleteKey { std::string_view Group; std::string_view Key; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DeleteGroup { std::string_view Group; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetGroupFromIndex { int Index; std::string_view Group; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SortByKey { std::string_view Key; int Descending; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct MergeFile { std::string_view Path; static const AC id = AC(-9); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Merge { OBJECTPTR Source; static const AC id = AC(-10); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objConfig : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CONFIG;
   static constexpr CSTRING CLASS_NAME = "Config";

   using create = kt::Create<objConfig>;
   objConfig(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string Path;         // Set this field to the location of the source configuration file.
   std::string KeyFilter;    // Set this field to enable key filtering.
   std::string GroupFilter;  // Set this field to enable group filtering.
   CNF Flags;                // Optional flags may be set here.
   public:
   ConfigGroups Groups; // Is a std::vector of std::pair and std::map values

   inline ERR getGroups(ConfigGroups * &Groups) {
      APTR ptr;
      if (auto error = getData(ptr); error IS ERR::Okay) {
         Groups = (ConfigGroups *)ptr;
         return error;
      }
      else return error;
   }

   // For C++ only, these read variants avoid method calls for speed, but apply identical logic.

   inline ERR read(std::string_view pGroup, std::string_view pKey, double &pValue) {
      for (auto & [group, keys] : Groups) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = strtod(keys.cbegin()->second.c_str(), nullptr);
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = strtod(it->second.c_str(), nullptr);
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR read(std::string_view pGroup, std::string_view pKey, int &pValue) {
      for (auto & [group, keys] : Groups) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = strtol(keys.cbegin()->second.c_str(), nullptr, 0);
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = strtol(it->second.c_str(), nullptr, 0);
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR read(std::string_view pGroup, std::string_view pKey, std::string &pValue) {
      for (auto & [group, keys] : Groups) {
         if ((!pGroup.empty()) and (group.compare(pGroup))) continue;
         if (pKey.empty()) pValue = keys.cbegin()->second;
         else if (auto it = keys.find(pKey); it != keys.end()) pValue = it->second;
         else return ERR::Search;
         return ERR::Okay;
      }
      return ERR::Search;
   }

   inline ERR write(std::string_view Group, std::string_view Key, std::string_view Value) {
      for (auto & [group, keys] : Groups) {
         if (!group.compare(Group)) {
            if (auto it = keys.find(Key); it != keys.end()) it->second.assign(Value);
            else keys.emplace(Key, Value);
            return ERR::Okay;
         }
      }

      auto &new_group = Groups.emplace_back();
      new_group.first.assign(Group);
      new_group.second.emplace(Key, Value);
      return ERR::Okay;
   }

   // Action stubs

   inline ERR clear() noexcept { return Action(AC::Clear, this, nullptr); }
   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR flush() noexcept { return Action(AC::Flush, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR saveSettings() noexcept { return Action(AC::SaveSettings, this, nullptr); }
   inline ERR saveToObject(OBJECTPTR Dest, CLASSID ClassID = CLASSID::NIL) noexcept {
      struct acSaveToObject args = { Dest, { ClassID } };
      return Action(AC::SaveToObject, this, &args);
   }
   inline ERR readValue(const std::string_view &Group, const std::string_view &Key, std::string_view * Data) noexcept {
      struct cfg::ReadValue args = { Group, Key };
      ERR error = Action(AC(-1), this, &args);
      if (Data) *Data = args.Data;
      return error;
   }
   inline ERR set(const std::string_view &Group, const std::string_view &Key, const std::string_view &Data) noexcept {
      struct cfg::Set args = { Group, Key, Data };
      return Action(AC(-2), this, &args);
   }
   inline ERR writeValue(const std::string_view &Group, const std::string_view &Key, const std::string_view &Data) noexcept {
      struct cfg::WriteValue args = { Group, Key, Data };
      return Action(AC(-3), this, &args);
   }
   inline ERR deleteKey(const std::string_view &Group, const std::string_view &Key) noexcept {
      struct cfg::DeleteKey args = { Group, Key };
      return Action(AC(-4), this, &args);
   }
   inline ERR deleteGroup(const std::string_view &Group) noexcept {
      struct cfg::DeleteGroup args = { Group };
      return Action(AC(-5), this, &args);
   }
   inline ERR getGroupFromIndex(int Index, std::string_view * Group) noexcept {
      struct cfg::GetGroupFromIndex args = { Index };
      ERR error = Action(AC(-6), this, &args);
      if (Group) *Group = args.Group;
      return error;
   }
   inline ERR sortByKey(const std::string_view &Key, int Descending) noexcept {
      struct cfg::SortByKey args = { Key, Descending };
      return Action(AC(-7), this, &args);
   }
   inline ERR mergeFile(const std::string_view &Path) noexcept {
      struct cfg::MergeFile args = { Path };
      return Action(AC(-9), this, &args);
   }
   inline ERR merge(OBJECTPTR Source) noexcept {
      struct cfg::Merge args = { Source };
      return Action(AC(-10), this, &args);
   }

   // Customised field getting

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getKeyFilter(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getGroupFilter(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getFlags(CNF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getData(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->GetValue(this, &Value);
   }

   inline ERR getTotalGroups(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->GetValue(this, &Value);
   }

   inline ERR getTotalKeys(int &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setKeyFilter(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x00904300, &Value);
   }

   inline ERR setGroupFilter(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, 0x00904300, &Value);
   }

   inline ERR setFlags(const CNF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

};