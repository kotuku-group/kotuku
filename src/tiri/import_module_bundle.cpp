
#include "import_module_bundle.h"
#include "import_module_format.h"
#include "defs.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace tiri::import_cache {

namespace {

//********************************************************************************************************************
// Appends an unsigned integer using the bundle's compact LEB128 representation.

bool append_uleb(std::string &Output, uint32_t Value)
{
   do {
      if (Output.size() >= MAX_ROOT_BUNDLE_SIZE) return false;
      uint8_t byte = uint8_t(Value & 0x7f);
      Value >>= 7;
      if (Value) byte |= 0x80;
      Output.push_back(char(byte));
   } while (Value);
   return true;
}

//********************************************************************************************************************
// Appends bytes while enforcing the complete bundle limit before allocation.

bool append_bytes(std::string &Output, std::string_view Bytes)
{
   if (Bytes.size() > MAX_ROOT_BUNDLE_SIZE - Output.size()) return false;
   Output += Bytes;
   return true;
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
   const std::vector<RootModuleRecord> &Input, RootModuleGraphAssembly &Result)
{
   Result = {};
   if (auto error = validate_record_fields(Input); error != cache::FormatError::OKAY) {
      Result = {};
      return error;
   }
#ifdef UNIT_TESTS
   Result.work_.RecordInspections = Input.size();
   Result.work_.InterfaceValidations = Input.size();
#endif

   struct InternedRecord {
      RootModuleRecord Record;
      std::vector<std::string> DependencyIdentities;
      std::vector<uint32_t> Dependencies;
      std::vector<uint32_t> Dependants;
      uint32_t CanonicalIndex = UINT32_MAX;
      uint32_t Indegree = 0;
      uint32_t Depth = 0;
   };

   std::vector<InternedRecord> interned;
   std::unordered_map<std::string, size_t> identity_map;
   std::vector<uint32_t> input_nodes;
   interned.reserve(Input.size());
   input_nodes.reserve(Input.size());
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
         input_nodes.push_back(uint32_t(interned.size() - 1));
      }
      else {
         InternedRecord &existing = interned[found->second];
         if (existing.Record.LookupIdentity != record.LookupIdentity or
             existing.Record.InterfaceBytes != record.InterfaceBytes or
             existing.DependencyIdentities != dependencies) {
            Result = {};
            return cache::FormatError::INVALID_METADATA;
         }
         existing.Record.SourceIndex = std::min(existing.Record.SourceIndex, record.SourceIndex);
         input_nodes.push_back(uint32_t(found->second));
      }
   }

   for (size_t i = 0; i < interned.size(); ++i) {
      InternedRecord &record = interned[i];
      record.Dependencies.reserve(record.DependencyIdentities.size());
      for (const std::string &identity : record.DependencyIdentities) {
         auto found = identity_map.find(identity);
         if (found IS identity_map.end() or found->second IS i) {
            Result = {};
            return cache::FormatError::INVALID_METADATA;
         }
         const uint32_t dependency = uint32_t(found->second);
         record.Dependencies.push_back(dependency);
         interned[dependency].Dependants.push_back(uint32_t(i));
#ifdef UNIT_TESTS
         Result.work_.EdgeTraversals++;
#endif
      }
      record.Indegree = uint32_t(record.Dependencies.size());
   }

   auto greater_identity = [&](uint32_t Left, uint32_t Right) {
      return interned[Left].Record.CompiledIdentity > interned[Right].Record.CompiledIdentity;
   };
   std::priority_queue<uint32_t, std::vector<uint32_t>, decltype(greater_identity)> ready(greater_identity);
   for (size_t i = 0; i < interned.size(); ++i) {
      if (interned[i].Indegree IS 0) ready.push(uint32_t(i));
   }

   Result.records_.reserve(interned.size());
   while (not ready.empty()) {
      const uint32_t selected = ready.top();
      ready.pop();
#ifdef UNIT_TESTS
      Result.work_.QueuePops++;
#endif
      InternedRecord &record = interned[selected];
      record.Depth = 1;
      for (uint32_t dependency : record.Dependencies) {
         const InternedRecord &dependency_record = interned[dependency];
         record.Depth = std::max(record.Depth, dependency_record.Depth + 1);
         record.Record.Dependencies.push_back(dependency_record.CanonicalIndex);
      }
      if (record.Depth > MAX_ROOT_MODULE_DEPTH) {
         Result = {};
         return cache::FormatError::COUNT_LIMIT;
      }
      record.CanonicalIndex = uint32_t(Result.records_.size());
      Result.records_.push_back(std::move(record.Record));
      for (uint32_t dependant : record.Dependants) {
#ifdef UNIT_TESTS
         Result.work_.EdgeTraversals++;
#endif
         if (--interned[dependant].Indegree IS 0) ready.push(dependant);
      }
   }

   if (Result.records_.size() != interned.size()) {
      Result = {};
      return cache::FormatError::INVALID_METADATA;
   }
   Result.input_to_canonical_.reserve(input_nodes.size());
   for (uint32_t node : input_nodes) Result.input_to_canonical_.push_back(interned[node].CanonicalIndex);
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Serialises validated root-module records into the compact bundle format.

cache::FormatError encode_root_module_bundle(const RootModuleGraphAssembly &Assembly, std::string &Output)
{
   const auto &records = Assembly.records();
   if (records.size() > MAX_ROOT_MODULES or records.size() > std::numeric_limits<uint32_t>::max()) {
      return cache::FormatError::COUNT_LIMIT;
   }

   std::string result;
   result.push_back(char(ROOT_BUNDLE_VERSION));
   if (not append_uleb(result, uint32_t(records.size()))) return cache::FormatError::SIZE_LIMIT;
   for (const RootModuleRecord &record : records) {
      if (record.LookupIdentity.empty() or record.LookupIdentity.size() > cache::MAX_STRING_SIZE or
          record.CompiledIdentity.empty() or record.CompiledIdentity.size() > cache::MAX_STRING_SIZE or
          record.Dependencies.size() > MAX_ROOT_MODULES or record.InterfaceBytes.size() > MAX_INTERFACE_SIZE) {
         return cache::FormatError::INVALID_METADATA;
      }
      if (not append_uleb(result, uint32_t(record.LookupIdentity.size())) or
          not append_bytes(result, record.LookupIdentity) or
          not append_uleb(result, uint32_t(record.CompiledIdentity.size())) or
          not append_bytes(result, record.CompiledIdentity) or result.size() >= MAX_ROOT_BUNDLE_SIZE) {
         return cache::FormatError::SIZE_LIMIT;
      }
      result.push_back(char(record.SourceIndex));
      if (not append_uleb(result, uint32_t(record.Dependencies.size()))) return cache::FormatError::SIZE_LIMIT;
      for (uint32_t dependency : record.Dependencies) {
         if (not append_uleb(result, dependency)) return cache::FormatError::SIZE_LIMIT;
      }
      if (not append_uleb(result, uint32_t(record.InterfaceBytes.size())) or
          not append_bytes(result, record.InterfaceBytes)) return cache::FormatError::SIZE_LIMIT;
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
   RootModuleGraphAssembly assembly;
   if (auto error = assemble_root_module_graph(result, assembly); error != cache::FormatError::OKAY) return error;
   if (assembly.records() != result) return cache::FormatError::INVALID_METADATA;
   Output = std::move(result);
   return cache::FormatError::OKAY;
}

} // namespace tiri::import_cache
