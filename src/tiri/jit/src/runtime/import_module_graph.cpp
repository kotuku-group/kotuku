#include "import_module_graph.h"

#include "lj_bc.h"
#include "lj_ffid.h"
#include "lj_gc.h"
#include "lj_str.h"

#include <array>
#include <limits>
#include <unordered_set>

namespace {

//********************************************************************************************************************
// Collects either every input record or the dependency closure of the supplied roots in original input order.
// The iterative traversal also rejects invalid dependency indices and cycles before graph assembly begins.

bool collect_selected_records(std::span<const ImportModuleGraphInput> Inputs, std::span<const uint32_t> Roots,
   bool SelectAll, std::vector<uint32_t> &Selected)
{
   enum class Visit { Unseen, Visiting, Complete };
   struct Frame { uint32_t Index; size_t NextDependency; };

   std::vector<Visit> visits(Inputs.size(), Visit::Unseen);
   std::vector<Frame> stack;
   std::vector<uint32_t> starts;
   if (SelectAll) {
      starts.reserve(Inputs.size());
      for (size_t i = 0; i < Inputs.size(); ++i) starts.push_back(uint32_t(i));
   }
   else starts.assign(Roots.begin(), Roots.end());

   for (uint32_t start : starts) {
      if (start >= Inputs.size()) return false;
      if (visits[start] IS Visit::Complete) continue;
      stack.push_back({ start, 0 });
      while (not stack.empty()) {
         Frame &frame = stack.back();
         if (visits[frame.Index] IS Visit::Unseen) visits[frame.Index] = Visit::Visiting;
         if (frame.NextDependency < Inputs[frame.Index].dependencies.size()) {
            const uint32_t dependency = Inputs[frame.Index].dependencies[frame.NextDependency++];
            if (dependency >= Inputs.size() or visits[dependency] IS Visit::Visiting) return false;
            if (visits[dependency] IS Visit::Unseen) stack.push_back({ dependency, 0 });
            continue;
         }
         visits[frame.Index] = Visit::Complete;
         stack.pop_back();
      }
   }
   Selected.clear();
   for (size_t i = 0; i < visits.size(); ++i) {
      if (visits[i] IS Visit::Complete) Selected.push_back(uint32_t(i));
   }
   return true;
}

//********************************************************************************************************************
// Scans each prototype tree once for compiler-generated module activation sequences and records validated relocation
// sites.  Every source and mapped index must fit the existing signed BC_KSHORT operand.

bool collect_relocation_sites(GCproto *Root, std::span<const uint32_t> Mapping,
   std::unordered_set<GCproto *> &Visited, ImportModuleRelocationPlan &Plan)
{
   if (not Root or not Visited.insert(Root).second) return Root != nullptr;
   BCIns *bytecode = proto_bc(Root);
   for (MSize i = 1; i < Root->sizebc; ++i) {
      if (bc_op(bytecode[i]) != BC_BFUNC or
          bc_d(bytecode[i]) != builtin_callable_index(BuiltinCallableID::ImportModuleActivate)) continue;
      const BCREG base = bc_a(bytecode[i]);
      const BCREG identity_slot = base + 1 + LJ_FR2;
      const BCREG reference_slot = base + 2 + LJ_FR2;
      if (i + 3 >= Root->sizebc or bc_op(bytecode[i + 1]) != BC_KSTR or
          bc_a(bytecode[i + 1]) != identity_slot or bc_op(bytecode[i + 2]) != BC_KSHORT or
          bc_a(bytecode[i + 2]) != reference_slot or bc_op(bytecode[i + 3]) != BC_CALL or
          bc_a(bytecode[i + 3]) != base or bc_b(bytecode[i + 3]) != 1 or bc_c(bytecode[i + 3]) != 3) return false;
      const MSize reference = i + 2;
      const int32_t old_index = int32_t(int16_t(bc_d(bytecode[reference])));
      if (old_index < 0 or uint32_t(old_index) >= Mapping.size()) return false;
      const uint32_t mapped = Mapping[uint32_t(old_index)];
      if (mapped IS UINT32_MAX or mapped > uint32_t(INT16_MAX)) return false;
      Plan.sites.push_back({ Root, reference, uint32_t(old_index), uint16_t(mapped) });
   }

   if (Root->flags & PROTO_CHILD) {
      GCRef *constant = mref<GCRef>(Root->k) - 1;
      for (MSize i = 0; i < Root->sizekgc; ++i, --constant) {
         GCobj *object = gcref(*constant);
         if (object->gch.gct IS ~LJ_TPROTO and
             not collect_relocation_sites(gco_to_proto(object), Mapping, Visited, Plan)) return false;
      }
   }
   return true;
}

} // namespace

//********************************************************************************************************************
// Validates a canonical executable graph and calculates the exact entry, dependency and allocation sizes required by
// its runtime directory without allocating or changing VM state.

bool measure_import_module_directory(std::span<const tiri::import_cache::RootModuleRecord> Records,
   std::span<GCproto *const> Initialisers, ImportModuleDirectoryLayout &Result)
{
   Result = {};
   if (Records.size() != Initialisers.size() or Records.size() > std::numeric_limits<uint32_t>::max()) return false;
   size_t dependency_count = 0;
   for (size_t i = 0; i < Records.size(); ++i) {
      if (not Initialisers[i] or Initialisers[i]->sizeuv != 0) return false;
      if (Records[i].SourceIndex != FILESOURCE_OVERFLOW_INDEX and Records[i].SourceIndex >= FILESOURCE_MAX_COUNT) {
         return false;
      }
      if (Records[i].Dependencies.size() > std::numeric_limits<uint32_t>::max() - dependency_count) return false;
      dependency_count += Records[i].Dependencies.size();
      for (uint32_t dependency : Records[i].Dependencies) if (dependency >= i) return false;
   }
   if (dependency_count > tiri::import_cache::MAX_ROOT_MODULES) return false;
   const size_t byte_size = sizeof(ImportModuleTable) + Records.size() * sizeof(ImportModuleTableEntry) +
      dependency_count * sizeof(uint32_t);
   if (byte_size > std::numeric_limits<uint32_t>::max()) return false;
   Result = { uint32_t(Records.size()), uint32_t(dependency_count), uint32_t(byte_size) };
   return true;
}

//********************************************************************************************************************
// Allocates, fills and attaches a previously measured executable directory.  Placeholder identities make the attached
// table safe for GC traversal while identity strings are interned through the VM's protected allocation path.

ImportModuleTable * install_import_module_directory(lua_State *L, GCproto *Root,
   std::span<const tiri::import_cache::RootModuleRecord> Records, std::span<GCproto *const> Initialisers,
   const ImportModuleDirectoryLayout &Layout)
{
   ImportModuleDirectoryLayout measured;
   if (not L or not Root or not measure_import_module_directory(Records, Initialisers, measured) or
       measured.entry_count != Layout.entry_count or measured.dependency_count != Layout.dependency_count or
       measured.byte_size != Layout.byte_size) return nullptr;
   if (not Layout.entry_count) return nullptr;

   uint32_t bundle_size = 0;
   const uint8_t *bundle = proto_import_module_bundle(Root, &bundle_size);
   std::vector<tiri::import_cache::RootModuleRecordLocation> locations;
   if (not bundle or tiri::import_cache::index_root_module_bundle(
         std::string_view((const char *)bundle, bundle_size), locations) != tiri::cache::FormatError::OKAY or
       locations.size() != Records.size()) return nullptr;
   for (size_t i = 0; i < Records.size(); ++i) {
      const auto &location = locations[i];
      if (std::string_view((const char *)bundle + location.InterfaceOffset, location.InterfaceSize) !=
          Records[i].InterfaceBytes) return nullptr;
   }

   auto table = (ImportModuleTable *)lj_mem_new(L, MSize(Layout.byte_size));
   table->version = IMPORT_MODULE_TABLE_VERSION;
   memset(table->reserved, 0, sizeof(table->reserved));
   table->entry_count = Layout.entry_count;
   table->dependency_count = Layout.dependency_count;
   table->byte_size = Layout.byte_size;
   auto entries = import_module_table_entries(table);
   auto dependencies = import_module_table_dependencies(table);
   uint32_t next_dependency = 0;

   for (size_t i = 0; i < Records.size(); ++i) {
      setgcref(entries[i].compiled_identity, obj2gco(&G(L)->strempty));
      setgcref(entries[i].initialiser, obj2gco(Initialisers[i]));
      entries[i].first_dependency = next_dependency;
      entries[i].dependency_count = uint32_t(Records[i].Dependencies.size());
      entries[i].interface_offset = locations[i].InterfaceOffset;
      entries[i].interface_size = locations[i].InterfaceSize;
      entries[i].source_index = Records[i].SourceIndex;
      memset(entries[i].reserved, 0, sizeof(entries[i].reserved));
      for (uint32_t dependency : Records[i].Dependencies) dependencies[next_dependency++] = dependency;
   }

   setmref(Root->import_module_table, table);

   for (size_t i = 0; i < Records.size(); ++i) {
      attach_compilation_source_root(Initialisers[i], Root);
      const std::string &identity = Records[i].CompiledIdentity;
      setgcref(entries[i].compiled_identity, obj2gco(lj_str_new(L, identity.data(), identity.size())));
   }
   return table;
}

//********************************************************************************************************************
// Measures metadata allocations owned directly by one prototype without following its source_root link.

ProtoRootMetadataSize measure_proto_root_metadata(const GCproto *Prototype) noexcept
{
   ProtoRootMetadataSize result;
   if (not Prototype) return result;
   if (const auto map = Prototype->compilation_sources.get<const CompilationSourceMap>()) {
      result.compilation_sources = sizeof(CompilationSourceMap) + map->count * sizeof(CompilationSourceEntry);
   }
   if (Prototype->package_metadata.get<const ProtoPackageMetadata>()) {
      result.package_metadata = sizeof(ProtoPackageMetadata);
   }
   if (Prototype->compatibility_manifest.get<const uint8_t>()) {
      result.compatibility_manifest = Prototype->compatibility_manifest_size;
   }
   if (Prototype->struct_manifest.get<const uint8_t>()) result.struct_manifest = Prototype->struct_manifest_size;
   if (Prototype->import_module_bundle.get<const uint8_t>()) {
      result.import_module_bundle = Prototype->import_module_bundle_size;
   }
   if (const auto table = Prototype->import_module_table.get<const ImportModuleTable>()) {
      result.import_module_table = table->byte_size;
   }
   return result;
}

//********************************************************************************************************************
// Releases metadata stored directly on one prototype.  source_root is preserved so retained descendants continue to
// resolve diagnostics, persistence metadata and executable ownership through the final compilation root.

void release_proto_root_metadata(global_State *State, GCproto *Prototype) noexcept
{
   if (not State or not Prototype) return;
   if (auto map = Prototype->compilation_sources.get<CompilationSourceMap>()) {
      const MSize bytes = MSize(sizeof(CompilationSourceMap) + map->count * sizeof(CompilationSourceEntry));
      setmref(Prototype->compilation_sources, nullptr);
      lj_mem_free(State, map, bytes);
   }
   if (auto package = Prototype->package_metadata.get<ProtoPackageMetadata>()) {
      setmref(Prototype->package_metadata, nullptr);
      lj_mem_free(State, package, sizeof(ProtoPackageMetadata));
   }
   if (auto compatibility = Prototype->compatibility_manifest.get<uint8_t>()) {
      const uint32_t bytes = Prototype->compatibility_manifest_size;
      setmref(Prototype->compatibility_manifest, nullptr);
      Prototype->compatibility_manifest_size = 0;
      lj_mem_free(State, compatibility, bytes);
   }
   if (auto manifest = Prototype->struct_manifest.get<uint8_t>()) {
      const uint32_t bytes = Prototype->struct_manifest_size;
      setmref(Prototype->struct_manifest, nullptr);
      Prototype->struct_manifest_size = 0;
      lj_mem_free(State, manifest, bytes);
   }
   if (auto bundle = Prototype->import_module_bundle.get<uint8_t>()) {
      const uint32_t bytes = Prototype->import_module_bundle_size;
      setmref(Prototype->import_module_bundle, nullptr);
      Prototype->import_module_bundle_size = 0;
      lj_mem_free(State, bundle, bytes);
   }
   if (auto table = Prototype->import_module_table.get<ImportModuleTable>()) {
      const uint32_t bytes = table->byte_size;
      setmref(Prototype->import_module_table, nullptr);
      lj_mem_free(State, table, bytes);
   }
}

//********************************************************************************************************************
// Installs immutable package metadata on a standalone root.  The absent state is represented by a null sidecar.

void install_proto_package_metadata(
   lua_State *State, GCproto *Prototype, const std::optional<tiri::PackageIdentity> &Package)
{
   if (not State or not Prototype or not Package) return;
   auto metadata = (ProtoPackageMetadata *)lj_mem_new(State, sizeof(ProtoPackageMetadata));
   metadata->version = PROTO_PACKAGE_METADATA_VERSION;
   metadata->flags = PROTO_PACKAGE_PRESENT;
   metadata->reserved[0] = 0;
   metadata->reserved[1] = 0;
   setgcrefnull(metadata->name);
   setgcrefnull(metadata->package_version);
   setmref(Prototype->package_metadata, metadata);
   setgcref(metadata->name, obj2gco(lj_str_new(State, Package->Name.data(), Package->Name.size())));
   setgcref(metadata->package_version,
      obj2gco(lj_str_new(State, Package->Version.data(), Package->Version.size())));
}

//********************************************************************************************************************
// Installs canonical compatibility bytes on a standalone root.

void install_proto_compatibility_manifest(lua_State *State, GCproto *Prototype, std::string_view Manifest)
{
   if (not State or not Prototype or Manifest.empty()) return;
   auto bytes = (uint8_t *)lj_mem_new(State, MSize(Manifest.size()));
   memcpy(bytes, Manifest.data(), Manifest.size());
   setmref(Prototype->compatibility_manifest, bytes);
   Prototype->compatibility_manifest_size = uint32_t(Manifest.size());
}

//********************************************************************************************************************
// Builds one self-contained canonical graph for either all compilation records or a selected dependency closure,
// including source translation, executable selection, wire mapping, encoded bundle and directory measurement.

bool prepare_import_module_graph(std::span<const ImportModuleGraphInput> Inputs,
   std::span<const CompilationSourceRecord> Sources, std::span<const uint32_t> Roots, bool SelectAll,
   PreparedImportModuleGraph &Result, GCproto *StandaloneRoot)
{
   Result = PreparedImportModuleGraph {};
   std::array<int16_t, 256> source_lookup;
   source_lookup.fill(-1);
   for (size_t i = 0; i < Sources.size(); ++i) {
      if (i > FILESOURCE_MAX_INDEX) return false;
      const uint8_t runtime = Sources[i].runtime_index;
      if (runtime IS FILESOURCE_OVERFLOW_INDEX) continue;
      if (source_lookup[runtime] >= 0 and source_lookup[runtime] != int16_t(i)) return false;
      source_lookup[runtime] = int16_t(i);
   }

   std::vector<uint32_t> selected;
   if (not collect_selected_records(Inputs, Roots, SelectAll, selected)) return false;
   std::vector<uint32_t> compilation_to_selected(Inputs.size(), UINT32_MAX);
   for (size_t i = 0; i < selected.size(); ++i) compilation_to_selected[selected[i]] = uint32_t(i);

   std::vector<tiri::import_cache::RootModuleRecord> records;
   std::vector<GCproto *> initialisers;
   std::vector<uint8_t> module_sources;
   records.reserve(selected.size());
   initialisers.reserve(selected.size());
   module_sources.reserve(selected.size());
   for (uint32_t original : selected) {
      const ImportModuleGraphInput &input = Inputs[original];
      uint8_t source = FILESOURCE_OVERFLOW_INDEX;
      if (input.runtime_source_index != FILESOURCE_OVERFLOW_INDEX) {
         const int16_t mapped = source_lookup[input.runtime_source_index];
         if (mapped < 0) return false;
         source = uint8_t(mapped);
      }

      std::vector<uint32_t> dependencies;
      dependencies.reserve(input.dependencies.size());
      for (uint32_t dependency : input.dependencies) {
         if (dependency >= compilation_to_selected.size() or
             compilation_to_selected[dependency] IS UINT32_MAX) return false;
         dependencies.push_back(compilation_to_selected[dependency]);
      }

      records.push_back({ std::string(input.lookup_identity), std::string(input.compiled_identity),
         std::string(input.interface_bytes), std::move(dependencies), source });
      initialisers.push_back(input.initialiser);
      module_sources.push_back(input.runtime_source_index);
   }

   PreparedImportModuleGraph prepared;
   if (StandaloneRoot) {
      if (not prepare_standalone_compilation_sources(
          Sources, StandaloneRoot, initialisers, module_sources, prepared.sources)) return false;
      for (auto &record : records) {
         if (record.SourceIndex IS FILESOURCE_OVERFLOW_INDEX) continue;
         if (record.SourceIndex >= prepared.sources.compilation_to_wire.size()) return false;
         const uint8_t mapped = prepared.sources.compilation_to_wire[record.SourceIndex];
         if (mapped IS FILESOURCE_OVERFLOW_INDEX) return false;
         record.SourceIndex = mapped;
      }
   }

   if (tiri::import_cache::assemble_root_module_graph(records, prepared.assembly) !=
       tiri::cache::FormatError::OKAY) return false;

   prepared.initialisers.assign(prepared.assembly.records().size(), nullptr);
   prepared.compilation_to_canonical.assign(Inputs.size(), UINT32_MAX);
   for (size_t i = 0; i < selected.size(); ++i) {
      const uint32_t canonical = prepared.assembly.input_to_canonical()[i];
      if (canonical >= prepared.initialisers.size()) return false;
      prepared.compilation_to_canonical[selected[i]] = canonical;
      if (not prepared.initialisers[canonical]) prepared.initialisers[canonical] = initialisers[i];
   }

   if (tiri::import_cache::encode_root_module_bundle(prepared.assembly, prepared.bundle) !=
       tiri::cache::FormatError::OKAY or
       not measure_import_module_directory(prepared.assembly.records(), prepared.initialisers, prepared.directory)) {
      return false;
   }
   Result = std::move(prepared);
   return true;
}

//********************************************************************************************************************
// Preflights all relocation roots as one transaction, deduplicating shared prototype trees and publishing a plan only
// after every activation site and mapped operand has been validated.

bool preflight_import_module_relocations(std::span<GCproto *const> Roots, std::span<const uint32_t> Mapping,
   ImportModuleRelocationPlan &Result)
{
   Result = ImportModuleRelocationPlan {};
   ImportModuleRelocationPlan plan;
   std::unordered_set<GCproto *> visited;
   for (GCproto *root : Roots) {
      if (not collect_relocation_sites(root, Mapping, visited, plan)) return false;
   }
   Result = std::move(plan);
   return true;
}

//********************************************************************************************************************
// Confirms that every planned relocation still refers to the same live BC_KSHORT instruction observed at preflight.

bool validate_import_module_relocation_plan(const ImportModuleRelocationPlan &Plan) noexcept
{
   for (const ImportModuleRelocationSite &site : Plan.sites) {
      if (not site.prototype or site.instruction >= site.prototype->sizebc) return false;
      const BCIns instruction = proto_bc(site.prototype)[site.instruction];
      if (bc_op(instruction) != BC_KSHORT or int32_t(int16_t(bc_d(instruction))) != int32_t(site.old_index)) {
         return false;
      }
   }
   return true;
}

//********************************************************************************************************************
// Commits an already validated relocation plan to live bytecode as an infallible sequence of operand replacements.

void apply_import_module_relocations(const ImportModuleRelocationPlan &Plan) noexcept
{
   for (const ImportModuleRelocationSite &site : Plan.sites) {
      setbc_d(&proto_bc(site.prototype)[site.instruction], site.mapped_index);
   }
}
