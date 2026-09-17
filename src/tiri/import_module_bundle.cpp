
#include "import_module_bundle.h"
#include "import_module_format.h"
#include "defs.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace tiri::import_cache {

namespace {

//********************************************************************************************************************
// Appends an unsigned integer using the bundle's compact LEB128 representation.

void append_uleb(std::string &Output, uint32_t Value)
{
   do {
      uint8_t byte = uint8_t(Value & 0x7f);
      Value >>= 7;
      if (Value) byte |= 0x80;
      Output.push_back(char(byte));
   } while (Value);
}

//********************************************************************************************************************
// Reads one bounded unsigned LEB128 integer and advances the input position.

bool read_uleb(std::string_view Input, size_t &Position, uint32_t &Value)
{
   Value = 0;
   for (unsigned shift = 0; shift <= 28; shift += 7) {
      if (Position >= Input.size()) return false;
      uint8_t byte = uint8_t(Input[Position++]);
      if (shift IS 28 and byte > 0x0f) return false;
      Value |= uint32_t(byte & 0x7f) << shift;
      if (not (byte & 0x80)) return true;
   }

   return false;
}

//********************************************************************************************************************
// Validates bounded record fields before graph interning.

cache::FormatError validate_record_fields(const std::vector<RootModuleRecord> &Records)
{
   if (Records.size() > MAX_ROOT_MODULES) return cache::FormatError::COUNT_LIMIT;

   for (size_t i = 0; i < Records.size(); ++i) {
      const RootModuleRecord &record = Records[i];
      if (record.LookupIdentity.empty() or record.LookupIdentity.size() > cache::MAX_STRING_SIZE or
          record.CompiledIdentity.empty() or record.CompiledIdentity.size() > cache::MAX_STRING_SIZE or
          record.InterfaceBytes.size() > MAX_INTERFACE_SIZE or
          record.Dependencies.size() > MAX_ROOT_MODULES) {
         return cache::FormatError::INVALID_METADATA;
      }

      Interface interface_value;
      if (decode_interface(record.InterfaceBytes, interface_value) != cache::FormatError::OKAY) {
         return cache::FormatError::INVALID_METADATA;
      }

      std::unordered_set<uint32_t> dependencies;
      for (uint32_t dependency : record.Dependencies) {
         if (dependency >= Records.size() or dependency IS i or not dependencies.insert(dependency).second) {
            return cache::FormatError::INVALID_METADATA;
         }
      }
   }
   return cache::FormatError::OKAY;
}

} // namespace

//********************************************************************************************************************
// Interns an arbitrary module record collection and emits deterministic dependency-first records.

cache::FormatError assemble_root_module_graph(
   const std::vector<RootModuleRecord> &Input, std::vector<RootModuleRecord> &Output)
{
   Output.clear();
   if (auto error = validate_record_fields(Input); error != cache::FormatError::OKAY) return error;

   struct InternedRecord {
      RootModuleRecord Record;
      std::vector<std::string> DependencyIdentities;
      uint32_t Depth = 0;
      bool Emitted = false;
   };

   std::vector<InternedRecord> interned;
   std::unordered_map<std::string, size_t> identity_map;
   interned.reserve(Input.size());
   for (const RootModuleRecord &record : Input) {
      std::vector<std::string> dependencies;
      dependencies.reserve(record.Dependencies.size());
      for (uint32_t dependency : record.Dependencies) {
         dependencies.push_back(Input[dependency].CompiledIdentity);
      }
      std::ranges::sort(dependencies);
      dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

      auto found = identity_map.find(record.CompiledIdentity);
      if (found IS identity_map.end()) {
         RootModuleRecord canonical = record;
         canonical.Dependencies.clear();
         identity_map.emplace(record.CompiledIdentity, interned.size());
         interned.push_back({ std::move(canonical), std::move(dependencies) });
      }
      else {
         InternedRecord &existing = interned[found->second];
         if (existing.Record.LookupIdentity != record.LookupIdentity or
             existing.Record.InterfaceBytes != record.InterfaceBytes or
             existing.DependencyIdentities != dependencies) return cache::FormatError::INVALID_METADATA;
         existing.Record.SourceIndex = std::min(existing.Record.SourceIndex, record.SourceIndex);
      }
   }

   std::unordered_map<std::string, uint32_t> output_indices;
   Output.reserve(interned.size());
   while (Output.size() < interned.size()) {
      size_t selected = interned.size();
      for (size_t i = 0; i < interned.size(); ++i) {
         const InternedRecord &candidate = interned[i];
         if (candidate.Emitted) continue;
         bool ready = true;
         uint32_t depth = 1;
         for (const std::string &dependency : candidate.DependencyIdentities) {
            auto dependency_index = identity_map.find(dependency);
            if (dependency_index IS identity_map.end()) return cache::FormatError::INVALID_METADATA;
            const InternedRecord &dependency_record = interned[dependency_index->second];
            if (not dependency_record.Emitted) {
               ready = false;
               break;
            }
            depth = std::max(depth, dependency_record.Depth + 1);
         }
         if (not ready) continue;
         if (depth > MAX_ROOT_MODULE_DEPTH) return cache::FormatError::COUNT_LIMIT;
         if (selected IS interned.size() or
             candidate.Record.CompiledIdentity < interned[selected].Record.CompiledIdentity) selected = i;
      }

      if (selected IS interned.size()) return cache::FormatError::INVALID_METADATA;
      InternedRecord &record = interned[selected];
      record.Depth = 1;
      for (const std::string &dependency : record.DependencyIdentities) {
         const InternedRecord &dependency_record = interned[identity_map[dependency]];
         record.Depth = std::max(record.Depth, dependency_record.Depth + 1);
         record.Record.Dependencies.push_back(output_indices[dependency]);
      }
      record.Emitted = true;
      output_indices.emplace(record.Record.CompiledIdentity, uint32_t(Output.size()));
      Output.push_back(std::move(record.Record));
   }
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Serialises validated root-module records into the compact bundle format.

cache::FormatError encode_root_module_bundle(const std::vector<RootModuleRecord> &Records, std::string &Output)
{
   std::vector<RootModuleRecord> canonical;
   if (auto error = assemble_root_module_graph(Records, canonical); error != cache::FormatError::OKAY) return error;

   std::string result;
   result.push_back(char(ROOT_BUNDLE_VERSION));
   append_uleb(result, uint32_t(canonical.size()));
   for (const RootModuleRecord &record : canonical) {
      append_uleb(result, uint32_t(record.LookupIdentity.size()));
      result += record.LookupIdentity;
      append_uleb(result, uint32_t(record.CompiledIdentity.size()));
      result += record.CompiledIdentity;
      result.push_back(char(record.SourceIndex));
      append_uleb(result, uint32_t(record.Dependencies.size()));
      for (uint32_t dependency : record.Dependencies) append_uleb(result, dependency);
      append_uleb(result, uint32_t(record.InterfaceBytes.size()));
      result += record.InterfaceBytes;
      if (result.size() > MAX_ROOT_BUNDLE_SIZE) return cache::FormatError::SIZE_LIMIT;
   }

   Output = std::move(result);
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Decodes a complete root-module bundle and validates every reconstructed record.

cache::FormatError decode_root_module_bundle(std::string_view Input, std::vector<RootModuleRecord> &Output)
{
   if (Input.empty() or Input.size() > MAX_ROOT_BUNDLE_SIZE or uint8_t(Input[0]) != ROOT_BUNDLE_VERSION) {
      return cache::FormatError::UNSUPPORTED_VERSION;
   }
   size_t position = 1;
   uint32_t count = 0;
   if (not read_uleb(Input, position, count) or count > MAX_ROOT_MODULES) return cache::FormatError::COUNT_LIMIT;
   std::vector<RootModuleRecord> result;
   result.reserve(count);
   for (uint32_t i = 0; i < count; ++i) {
      uint32_t lookup_size = 0;
      if (not read_uleb(Input, position, lookup_size) or lookup_size > cache::MAX_STRING_SIZE or
          lookup_size > Input.size() - position) return cache::FormatError::INVALID_METADATA;

      RootModuleRecord record;
      record.LookupIdentity.assign(Input.substr(position, lookup_size));
      position += lookup_size;
      uint32_t compiled_size = 0;
      if (not read_uleb(Input, position, compiled_size) or compiled_size > cache::MAX_STRING_SIZE or
          compiled_size > Input.size() - position) return cache::FormatError::INVALID_METADATA;
      record.CompiledIdentity.assign(Input.substr(position, compiled_size));
      position += compiled_size;
      if (position >= Input.size()) return cache::FormatError::INVALID_METADATA;
      record.SourceIndex = uint8_t(Input[position++]);

      uint32_t dependency_count = 0;
      if (not read_uleb(Input, position, dependency_count) or dependency_count > MAX_ROOT_MODULES) {
         return cache::FormatError::COUNT_LIMIT;
      }

      record.Dependencies.reserve(dependency_count);
      for (uint32_t d = 0; d < dependency_count; ++d) {
         uint32_t dependency = 0;
         if (not read_uleb(Input, position, dependency)) return cache::FormatError::INVALID_METADATA;
         record.Dependencies.push_back(dependency);
      }

      uint32_t interface_size = 0;
      if (not read_uleb(Input, position, interface_size) or interface_size > MAX_INTERFACE_SIZE or
          interface_size > Input.size() - position) return cache::FormatError::INVALID_METADATA;

      record.InterfaceBytes.assign(Input.substr(position, interface_size));
      position += interface_size;
      result.push_back(std::move(record));
   }

   if (position != Input.size()) return cache::FormatError::INVALID_METADATA;
   std::vector<RootModuleRecord> canonical;
   if (auto error = assemble_root_module_graph(result, canonical); error != cache::FormatError::OKAY) return error;
   if (canonical != result) return cache::FormatError::INVALID_METADATA;
   Output = std::move(result);
   return cache::FormatError::OKAY;
}

} // namespace tiri::import_cache
