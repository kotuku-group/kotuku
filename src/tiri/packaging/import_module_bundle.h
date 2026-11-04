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
   std::string LookupIdentity;                 // Identifies the module for cache lookup.
   std::string CompiledIdentity;               // Identifies the compiled module content.
   std::string InterfaceBytes;                 // Stores the serialised module interface.
   std::vector<uint32_t> Dependencies;         // Lists canonical indices of module dependencies.
   uint8_t SourceIndex = 0;                    // Identifies the module source for diagnostics.

   [[nodiscard]] bool operator==(const RootModuleRecord &) const = default;
};

// A module-cache fragment stores only its own executable code.  Each compiler-generated activation operand in the
// fragment indexes this table, which names the separately cached dependency by its immutable compiled identity.
// The importing source and line place the dependency's primary source in the diagnostic ancestry when it is linked.

inline constexpr uint8_t FRAGMENT_REFERENCE_VERSION = 1;

struct FragmentReference {
   std::string CompiledIdentity;               // Identifies the referenced dependency module.
   uint8_t ParentSource = 0;                   // Fragment source index that first imports the dependency.
   uint32_t ImportLine = 1;                    // Line in the parent source where the dependency is imported.

   [[nodiscard]] bool operator==(const FragmentReference &) const = default;
};

struct RootModuleRecordLocation {
   uint32_t InterfaceOffset = 0;               // Stores the interface byte offset in the bundle.
   uint32_t InterfaceSize = 0;                 // Stores the interface byte length in the bundle.
};

class RootModuleGraphAssembly {
public:
   [[nodiscard]] const std::vector<RootModuleRecord> & records() const { return records_; }
   [[nodiscard]] const std::vector<uint32_t> & input_to_canonical() const { return input_to_canonical_; }

#ifdef UNIT_TESTS
   struct WorkCounters {
      size_t RecordInspections = 0;            // Counts inspected input records.
      size_t InterfaceValidations = 0;         // Counts validated module interfaces.
      size_t QueuePops = 0;                    // Counts traversal queue removals.
      size_t EdgeTraversals = 0;               // Counts traversed dependency edges.
   };

   [[nodiscard]] const WorkCounters & work() const { return work_; }
#endif

private:
   std::vector<RootModuleRecord> records_;       // Stores the assembled canonical module graph.
   std::vector<uint32_t> input_to_canonical_;    // Maps input records to their canonical indices.
#ifdef UNIT_TESTS
   WorkCounters work_;                           // Tracks graph-assembly work for tests.
#endif

   friend cache::FormatError assemble_root_module_graph(
      const std::vector<RootModuleRecord> &Input, RootModuleGraphAssembly &Result);
};

// Validates and interns arbitrary records by immutable compiled identity, producing one deterministic,
// dependency-first graph and the matching translation for every input index.  SourceIndex is diagnostic metadata
// and is deliberately excluded from identity matching.  Result is cleared on failure.

[[nodiscard]] cache::FormatError assemble_root_module_graph(const std::vector<RootModuleRecord> &Input, RootModuleGraphAssembly &Result);

// Serialises a successfully assembled internal graph without validating interfaces or rebuilding the graph.

[[nodiscard]] cache::FormatError encode_root_module_bundle(const RootModuleGraphAssembly &Assembly, std::string &Output);
[[nodiscard]] cache::FormatError index_root_module_bundle(std::string_view Input, std::vector<RootModuleRecordLocation> &Output);
[[nodiscard]] cache::FormatError decode_root_module_bundle(std::string_view Input, std::vector<RootModuleRecord> &Output);

// Encodes and decodes the canonical reference table of a module-cache fragment.  References are unique and sorted by
// compiled identity, so a valid table has exactly one encoding.

[[nodiscard]] cache::FormatError encode_fragment_references(
   const std::vector<FragmentReference> &References, std::string &Output);
[[nodiscard]] cache::FormatError decode_fragment_references(
   std::string_view Input, std::vector<FragmentReference> &Output);

} // namespace tiri::import_cache
