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

// Interns records by immutable compiled identity, rejects conflicting claims and emits one deterministic,
// dependency-first graph.  SourceIndex is diagnostic metadata and is deliberately excluded from identity matching.
[[nodiscard]] cache::FormatError assemble_root_module_graph(
   const std::vector<RootModuleRecord> &Input, std::vector<RootModuleRecord> &Output);

[[nodiscard]] cache::FormatError encode_root_module_bundle(
   const std::vector<RootModuleRecord> &, std::string &Output);
[[nodiscard]] cache::FormatError decode_root_module_bundle(
   std::string_view Input, std::vector<RootModuleRecord> &Output);

} // namespace tiri::import_cache
