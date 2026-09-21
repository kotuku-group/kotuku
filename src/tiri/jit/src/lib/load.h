#pragma once

#include "../../../packaging/package_identity.h"
#include "../../../packaging/version_constraints.h"
#include "../../../packaging/import_module_bundle.h"

#include <string_view>
#include <optional>
#include <vector>
#include <cstdint>

struct lua_State;

enum class BytecodeLoadPolicy : uint8_t { Install, ValidateOnly };

struct BytecodeLoadOperationCounters {
   uint32_t payload_loads = 0;
   uint32_t prototype_decodes = 0;
   uint32_t source_records_decoded = 0;
   uint32_t file_source_registrations = 0;
   uint32_t line_map_remaps = 0;
   uint32_t structure_commits = 0;
   uint32_t source_map_allocations = 0;
   uint32_t executable_directory_allocations = 0;
};

// Portable metadata retained from a completely validated bytecode load.  Bytecode distinguishes a valid empty
// imported-module graph from source input or a failed load.
struct BytecodeLoadMetadata {
   std::vector<tiri::import_cache::RootModuleRecord> ImportedModules;
   std::optional<tiri::PackageIdentity> Package;
   std::string CompatibilityManifest;
   BytecodeLoadOperationCounters Operations;
   bool Bytecode = false;
};

[[nodiscard]] int lj_load_with_bytecode_metadata(
   lua_State *, std::string_view, const char *, BytecodeLoadMetadata &);
[[nodiscard]] int lj_validate_bytecode(
   lua_State *, std::string_view, const char *, BytecodeLoadMetadata &);
