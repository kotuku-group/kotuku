#pragma once

#include "lj_obj.h"
#include "../debug/filesource.h"
#include "../../../import_module_bundle.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

struct ImportModuleGraphInput {
   std::string_view lookup_identity;
   std::string_view compiled_identity;
   std::string_view interface_bytes;
   std::span<const uint32_t> dependencies;
   GCproto *initialiser = nullptr;
   uint8_t runtime_source_index = FILESOURCE_OVERFLOW_INDEX;
};

struct ImportModuleDirectoryLayout {
   uint32_t entry_count = 0;
   uint32_t dependency_count = 0;
   uint32_t byte_size = 0;
};

struct ProtoRootMetadataSize {
   size_t compilation_sources = 0;
   size_t struct_manifest = 0;
   size_t import_module_bundle = 0;
   size_t import_module_table = 0;

   [[nodiscard]] constexpr size_t total() const noexcept {
      return compilation_sources + struct_manifest + import_module_bundle + import_module_table;
   }
};

class PreparedImportModuleGraph {
public:
   PreparedImportModuleGraph() = default;
   PreparedImportModuleGraph(PreparedImportModuleGraph &&) = default;
   PreparedImportModuleGraph & operator=(PreparedImportModuleGraph &&) = default;
   PreparedImportModuleGraph(const PreparedImportModuleGraph &) = delete;
   PreparedImportModuleGraph & operator=(const PreparedImportModuleGraph &) = delete;

   tiri::import_cache::RootModuleGraphAssembly assembly;
   std::vector<GCproto *> initialisers;
   std::vector<uint32_t> compilation_to_canonical;
   std::string bundle;
   ImportModuleDirectoryLayout directory;
   PreparedCompilationSources sources;
};

struct ImportModuleRelocationSite {
   GCproto *prototype = nullptr;
   MSize instruction = 0;
   uint32_t old_index = 0;
   uint16_t mapped_index = 0;
};

class ImportModuleRelocationPlan {
public:
   ImportModuleRelocationPlan() = default;
   ImportModuleRelocationPlan(ImportModuleRelocationPlan &&) = default;
   ImportModuleRelocationPlan & operator=(ImportModuleRelocationPlan &&) = default;
   ImportModuleRelocationPlan(const ImportModuleRelocationPlan &) = delete;
   ImportModuleRelocationPlan & operator=(const ImportModuleRelocationPlan &) = delete;

   std::vector<ImportModuleRelocationSite> sites;
};

[[nodiscard]] bool prepare_import_module_graph(
   std::span<const ImportModuleGraphInput> Inputs, std::span<const CompilationSourceRecord> Sources,
   std::span<const uint32_t> Roots, bool SelectAll, PreparedImportModuleGraph &Result,
   GCproto *StandaloneRoot = nullptr);

// Performs allocation-free validation and calculates the exact trailing-array storage required by a directory.
[[nodiscard]] bool measure_import_module_directory(
   std::span<const tiri::import_cache::RootModuleRecord> Records, std::span<GCproto *const> Initialisers,
   ImportModuleDirectoryLayout &Result);

// Allocates, fills and attaches a measured directory.  Empty directories return null without changing Root.
// The table is attached with valid placeholder strings before interning can enter the VM's protected allocation path.
[[nodiscard]] ImportModuleTable * install_import_module_directory(lua_State *L, GCproto *Root,
   std::span<const tiri::import_cache::RootModuleRecord> Records, std::span<GCproto *const> Initialisers,
   const ImportModuleDirectoryLayout &Layout);

// Measures and releases only metadata allocations stored directly on a prototype.  These operations deliberately do
// not follow source_root, because a linked initialiser resolves accessors through the final compilation root.
[[nodiscard]] ProtoRootMetadataSize measure_proto_root_metadata(const GCproto *Prototype) noexcept;
void release_proto_root_metadata(global_State *State, GCproto *Prototype) noexcept;

[[nodiscard]] bool preflight_import_module_relocations(
   std::span<GCproto *const> Roots, std::span<const uint32_t> Mapping, ImportModuleRelocationPlan &Result);

void apply_import_module_relocations(const ImportModuleRelocationPlan &Plan) noexcept;

[[nodiscard]] bool validate_import_module_relocation_plan(const ImportModuleRelocationPlan &Plan) noexcept;
