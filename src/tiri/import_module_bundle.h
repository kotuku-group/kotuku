#pragma once

#include "cache_manifest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tiri::import_cache {

inline constexpr uint8_t ROOT_BUNDLE_VERSION = 3;
inline constexpr uint32_t MAX_ROOT_MODULES = 4096;
inline constexpr uint32_t MAX_ROOT_MODULE_DEPTH = 256;
inline constexpr size_t MAX_ROOT_BUNDLE_SIZE = 64 * 1024 * 1024;

struct RootModuleRecord {
   std::string LookupIdentity;
   std::string CompiledIdentity;
   std::string InterfaceBytes;
   std::vector<uint32_t> Dependencies;
   uint8_t SourceIndex = 0;

   [[nodiscard]] bool operator==(const RootModuleRecord &) const = default;
};

struct RootModuleRecordLocation {
   uint32_t InterfaceOffset = 0;
   uint32_t InterfaceSize = 0;
};

class RootModuleGraphAssembly {
public:
   [[nodiscard]] const std::vector<RootModuleRecord> & records() const { return records_; }
   [[nodiscard]] const std::vector<uint32_t> & input_to_canonical() const { return input_to_canonical_; }

#ifdef UNIT_TESTS
   struct WorkCounters {
      size_t RecordInspections = 0;
      size_t InterfaceValidations = 0;
      size_t QueuePops = 0;
      size_t EdgeTraversals = 0;
   };

   [[nodiscard]] const WorkCounters & work() const { return work_; }
#endif

private:
   std::vector<RootModuleRecord> records_;
   std::vector<uint32_t> input_to_canonical_;
#ifdef UNIT_TESTS
   WorkCounters work_;
#endif

   friend cache::FormatError assemble_root_module_graph(
      const std::vector<RootModuleRecord> &Input, RootModuleGraphAssembly &Result);
};

// Validates and interns arbitrary records by immutable compiled identity, producing one deterministic,
// dependency-first graph and the matching translation for every input index.  SourceIndex is diagnostic metadata
// and is deliberately excluded from identity matching.  Result is cleared on failure.
[[nodiscard]] cache::FormatError assemble_root_module_graph(
   const std::vector<RootModuleRecord> &Input, RootModuleGraphAssembly &Result);

// Serialises a successfully assembled internal graph without validating interfaces or rebuilding the graph.
[[nodiscard]] cache::FormatError encode_root_module_bundle(
   const RootModuleGraphAssembly &Assembly, std::string &Output);
[[nodiscard]] cache::FormatError index_root_module_bundle(
   std::string_view Input, std::vector<RootModuleRecordLocation> &Output);
[[nodiscard]] cache::FormatError decode_root_module_bundle(
   std::string_view Input, std::vector<RootModuleRecord> &Output);

} // namespace tiri::import_cache
