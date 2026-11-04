#include "import_module_validation.h"
#include "../lib/load.h"

#include <algorithm>
#include <ranges>
#include <format>
#include <utility>

ImportModuleValidationSession::ImportModuleValidationSession(lua_State &Lua,
   tiri::import_cache::LifecycleCounters &Counters, Environment EnvironmentValue) :
   lua(Lua), counters(Counters), environment(std::move(EnvironmentValue))
{
}

//********************************************************************************************************************
// Returns the immutable source view for this root compilation, reading a path at most once.

ERR ImportModuleValidationSession::snapshot(
   std::string_view Path, std::shared_ptr<const tiri::import_cache::SourceSnapshot> &Output)
{
   const std::string path(Path);
   if (auto found = this->snapshots.find(path); found != this->snapshots.end()) {
      Output = found->second;
      return ERR::Okay;
   }
   if (auto found = this->snapshot_errors.find(path); found != this->snapshot_errors.end()) return found->second;

   tiri::import_cache::SourceSnapshot captured;
   if (auto error = tiri::import_cache::snapshot_source(Path, this->counters, captured); error != ERR::Okay) {
      this->snapshot_errors.emplace(path, error);
      return error;
   }

   Output = std::make_shared<const tiri::import_cache::SourceSnapshot>(std::move(captured));
   this->snapshots.emplace(Output->Identity.ResolvedPath, Output);
   return ERR::Okay;
}

//********************************************************************************************************************
// Resolve each logical import and constraint once for the immutable package index used by this compilation.

const tiri::ResolvedImport &ImportModuleValidationSession::resolve_library(
   std::string_view Name, std::string_view Constraint)
{
   auto key = std::make_pair(std::string(Name), std::string(Constraint));
   auto found = this->resolved_libraries.find(key);
   if (found != this->resolved_libraries.end()) return found->second;

   auto resolved = this->environment.ResolveLibrary(Name, Constraint);
   return this->resolved_libraries.emplace(std::move(key), std::move(resolved)).first->second;
}

//********************************************************************************************************************
// Replays every non-payload observation retained by a cache candidate.

bool ImportModuleValidationSession::validate_identity(
   const tiri::import_cache::Identity &IdentityValue, std::string &Reason)
{
   for (const auto &dependency : IdentityValue.LocalImports) {
      std::shared_ptr<const tiri::import_cache::SourceSnapshot> source;
      if (auto error = this->snapshot(dependency.Source.ResolvedPath, source); error != ERR::Okay) {
         (void)error;
         Reason = "an imported dependency is unreadable";
         return false;
      }
      if ((source->Identity.Size != dependency.Source.Size) or
          (source->Identity.ContentDigest != dependency.Source.ContentDigest)) {
         Reason = "an imported dependency has changed or is unreadable";
         return false;
      }
   }

   for (const auto &input : IdentityValue.ResolutionInputs) {
      std::string current;
      const bool exists_input = std::ranges::any_of(IdentityValue.ConditionalInputs, [&](const auto &Item) {
         return Item.Kind IS tiri::cache::ConditionalKind::EXISTS and Item.Name IS input.Name and
            Item.Context IS input.Context;
      });

      if (exists_input) {
         auto separator = input.Context.find_last_of("/\\");
         if (separator != std::string::npos) current.assign(input.Context, 0, separator + 1);
         current += input.Name;
      }
      else if (input.Name.starts_with("./") or input.Name.starts_with("../")) {
         auto separator = input.Context.find_last_of("/\\");
         if (separator != std::string::npos) current.assign(input.Context, 0, separator + 1);
         current += input.Name;
         current += ".tiri";
         std::string resolved;
         if (ResolvePath(current, RSF::NO_FILE_CHECK, &resolved) IS ERR::Okay) current = std::move(resolved);
      }
      else if (this->environment.ResolveLibrary) {
         const auto &resolved = this->resolve_library(input.Name, input.Constraint);
         if (resolved.SelectedVersion.value_or("") != input.SelectedVersion or
             resolved.PackageManaged != input.PackageManaged) {
            Reason = "an imported package now selects a different version";
            return false;
         }
         current = resolved.ResolvedPath;
      }

      if (current != input.Value) {
         Reason = "an imported path now resolves to a different source";
         return false;
      }
   }

   for (const auto &input : IdentityValue.ConditionalInputs) {
      std::string current;
      switch (input.Kind) {
         case tiri::cache::ConditionalKind::IMPORTED: current = "true"; break;
         case tiri::cache::ConditionalKind::DEBUG_MODE:
            current = GetResource(RES::LOG_LEVEL) > 2 ? "true" : "false";
            break;
         case tiri::cache::ConditionalKind::LOG_LEVEL:
            current = std::to_string(GetResource(RES::LOG_LEVEL));
            break;
         case tiri::cache::ConditionalKind::PLATFORM: {
            const SystemState *state = GetSystemState();
            current = state->Platform ? state->Platform : "";
            break;
         }
         case tiri::cache::ConditionalKind::EXISTS: {
            std::string resolved;
            auto separator = input.Context.find_last_of("/\\");
            if (separator != std::string::npos) resolved.assign(input.Context, 0, separator + 1);
            resolved += input.Name;
            current = AnalysePath(resolved, nullptr) IS ERR::Okay ? "true" : "false";
            break;
         }
         case tiri::cache::ConditionalKind::MODULE_AVAILABLE:
            current = this->environment.ModuleAvailable and this->environment.ModuleAvailable(input.Name) ?
               "true" : "false";
            break;
         case tiri::cache::ConditionalKind::OTHER:
            Reason = "the module cache contains an unsupported conditional observation";
            return false;
      }

      if (current != input.Value) {
         Reason = "a compile-time condition has changed";
         return false;
      }
   }

   for (const auto &dependency : IdentityValue.ModuleDependencies) {
      tiri::import_cache::ModuleLookup lookup;
      if (not this->validate_dependency(IdentityValue, dependency, lookup, Reason)) return false;
   }
   return true;
}

//********************************************************************************************************************
// Validates one recorded module dependency through this session.  A parent is reusable only if the dependency is a
// cache hit with exactly the compiled identity and interface digest that the parent was compiled against.

bool ImportModuleValidationSession::validate_dependency(const tiri::import_cache::Identity &Parent,
   const tiri::import_cache::ModuleDependencyIdentity &Dependency, tiri::import_cache::ModuleLookup &Lookup,
   std::string &Reason)
{
   tiri::import_cache::CompilationRequest request;
   request.ExpectedIdentity.BuildIdentity = Parent.BuildIdentity;
   request.ExpectedIdentity.LogicalRequest = Dependency.OriginalRequest;
   request.ExpectedIdentity.Source.ResolvedPath = Dependency.ResolvedPath;
   request.ExpectedIdentity.Options = Parent.Options;
   request.ExpectedIdentity.ImportedRoot = true;

   if (auto error = this->ensure_validated(request, Lookup); error != ERR::Okay) {
      (void)error;
      Reason = "an imported module dependency is unavailable";
      return false;
   }
   if (not Lookup.Cached.CacheHit or not Lookup.Artifact or
       Lookup.Cached.CompileTimeInterface->digest() != Dependency.InterfaceDigest or
       Lookup.Cached.CompiledIdentity != Dependency.CompiledIdentity) {
      Reason = Lookup.Cached.Diagnostic.empty() ?
         "an imported module dependency has changed or is unavailable" : Lookup.Cached.Diagnostic;
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Resolves every fragment reference to the validated artifact of a dependency declared by the module's identity, then
// publishes the immutable linked artifact.  The fragment carries no dependency code, so an unresolved reference makes
// the whole module a cache miss before the parser commits to the cached interface.

bool ImportModuleValidationSession::link_references(Node &Entry, PayloadMetadata &Metadata)
{
   auto &cached = Entry.Lookup.Cached;
   const auto &identity = cached.CompilationIdentity;
   // The linker registers a dependency through its fragment's first source, which must be the module itself.
   if (Metadata.Sources.empty() or Metadata.Sources.front().ResolvedPath != identity.Source.ResolvedPath) {
      cached.Diagnostic = "cached bytecode source map does not start with its module source";
      return false;
   }

   auto artifact = std::make_shared<tiri::import_cache::CachedModuleArtifact>();
   artifact->Dependencies.reserve(Metadata.References.size());
   for (const auto &reference : Metadata.References) {
      auto declared = std::ranges::find(identity.ModuleDependencies, reference.CompiledIdentity,
         &tiri::import_cache::ModuleDependencyIdentity::CompiledIdentity);
      if (declared IS identity.ModuleDependencies.end()) {
         cached.Diagnostic = "cached bytecode references an undeclared imported-module dependency";
         return false;
      }
      tiri::import_cache::ModuleLookup lookup;
      if (not this->validate_dependency(identity, *declared, lookup, cached.Diagnostic)) return false;
      artifact->Dependencies.push_back(lookup.Artifact);
   }

   artifact->LookupIdentity = cached.LookupIdentity;
   artifact->CompiledIdentity = cached.CompiledIdentity;
   artifact->Interface = cached.CompileTimeInterface;
   artifact->Package = std::move(Metadata.Package);
   artifact->Payload = std::move(cached.Payload);
   artifact->Sources = std::move(Metadata.Sources);
   artifact->References = std::move(Metadata.References);
   cached.Payload.clear();
   Entry.Lookup.Artifact = std::move(artifact);
   return true;
}

//********************************************************************************************************************
// Validates bytecode in an isolated state and retains only decoded portable dependency records.

bool ImportModuleValidationSession::validate_payload(std::string_view Payload, std::string &Reason,
   PayloadMetadata &Output)
{
   std::unique_ptr<lua_State, decltype(&lua_close)> validation(luaL_newstate(this->lua.script), lua_close);
   if (not validation) {
      Reason = "an isolated validation state could not be created";
      return false;
   }
   this->counters.ValidationStateCreations++;

   if (initialise_tiri_compilation_state(validation.get()) != ERR::Okay) {
      Reason = "an isolated validation state could not be initialised";
      return false;
   }
   BytecodeLoadMetadata metadata;
   if (lj_validate_bytecode(validation.get(), Payload, "=import-cache-validation", metadata,
       BytecodePayloadKind::ModuleFragment) != 0) {
      auto message = lua_tostringview(validation.get(), -1);
      Reason.assign(message.data(), message.size());
      return false;
   }

   if (not metadata.Bytecode) {
      Reason = "the imported-module cache payload did not contain bytecode metadata";
      return false;
   }
   const auto &operations = metadata.Operations;
   this->counters.ValidationPayloadLoads += operations.payload_loads;
   this->counters.PrototypeDecodes += operations.prototype_decodes;
   this->counters.SourceRecordsDecoded += operations.source_records_decoded;
   this->counters.FileSourceRegistrations += operations.file_source_registrations;
   this->counters.LineMapRemaps += operations.line_map_remaps;
   this->counters.StructureCommits += operations.structure_commits;
   this->counters.SourceMapAllocations += operations.source_map_allocations;
   this->counters.ExecutableDirectoryAllocations += operations.executable_directory_allocations;
   this->counters.PayloadBundleDecodes++;
   Output.References = std::move(metadata.FragmentReferences);
   Output.Sources = std::move(metadata.Sources);
   Output.Package = std::move(metadata.Package);
   Output.CompatibilityManifest = std::move(metadata.CompatibilityManifest);
   return true;
}

//********************************************************************************************************************
// Validates one unique preliminary lookup identity and memoises both successful and ordinary rejected candidates.

ERR ImportModuleValidationSession::ensure_validated(
   const tiri::import_cache::CompilationRequest &Request, tiri::import_cache::ModuleLookup &Output)
{
   std::shared_ptr<const tiri::import_cache::SourceSnapshot> source;
   if (auto error = this->snapshot(Request.ExpectedIdentity.Source.ResolvedPath, source); error != ERR::Okay) {
      return error;
   }

   auto expected = Request.ExpectedIdentity;
   expected.Source = source->Identity;
   const std::string key = tiri::import_cache::lookup_key(expected);
   if (key.empty()) return ERR::InvalidData;

   if (auto found = this->nodes.find(key); found != this->nodes.end()) {
      if (found->second.State IS NodeState::IN_PROGRESS) {
         Output = {};
         Output.ExpectedIdentity = std::move(expected);
         Output.Source = source->Source;
         Output.Cached.Diagnostic = "a circular imported-module dependency was found while validating the cache";
         return ERR::Okay;
      }
      this->counters.ValidationReuses++;
      Output = found->second.Lookup;
      return ERR::Okay;
   }

   // Validation recurses through module dependencies.  Bound the active chain by the executable graph's depth limit.
   if (this->active_depth >= tiri::import_cache::MAX_ROOT_MODULE_DEPTH) {
      Output = {};
      Output.ExpectedIdentity = std::move(expected);
      Output.Source = source->Source;
      Output.Cached.Diagnostic = "the imported-module dependency graph exceeds its depth limit";
      return ERR::Okay;
   }
   this->active_depth++;
   auto depth = kt::deferred_call([&] { this->active_depth--; });

   auto [entry, inserted] = this->nodes.emplace(key, Node{});
   (void)inserted;
   Node *node = &entry->second;
   auto unwind = kt::deferred_call([&] {
      if (node->State IS NodeState::IN_PROGRESS) this->nodes.erase(key);
   });

   auto identity_validator = [&](const tiri::import_cache::Identity &IdentityValue, std::string &Reason) {
      return this->validate_identity(IdentityValue, Reason);
   };
   PayloadMetadata payload;
   auto payload_validator = [&](std::string_view Payload, std::string &Reason) {
      return this->validate_payload(Payload, Reason, payload);
   };

   auto error = tiri::import_cache::lookup_module(
      Request, *source, identity_validator, payload_validator, this->counters, node->Lookup);
   if (error != ERR::Okay) return error;

   if (node->Lookup.Cached.CacheHit and
       (payload.Package != node->Lookup.Cached.CompilationIdentity.DeclaredPackage or
        payload.Package != node->Lookup.Cached.CompileTimeInterface->descriptors().Package)) {
      node->Lookup.Cached.CacheHit = false;
      node->Lookup.Cached.Diagnostic = "cached bytecode package metadata does not agree with its envelope";
   }

   if (node->Lookup.Cached.CacheHit) {
      const auto &interface_compatibility =
         node->Lookup.Cached.CompileTimeInterface->descriptors().CompatibilityManifest;
      std::vector<tiri::CompatibilityRecord> records;
      tiri::CompatibilityFailure failure;
      if (payload.CompatibilityManifest != interface_compatibility or
          tiri::decode_compatibility_manifest(interface_compatibility, records)) {
         node->Lookup.Cached.CacheHit = false;
         node->Lookup.Cached.Diagnostic =
            "cached bytecode compatibility metadata does not agree with its interface";
      }
      else if (not tiri::check_compatibility_records(records, tiri::runtime_versions(), &failure)) {
         node->Lookup.Cached.CacheHit = false;
         node->Lookup.Cached.Diagnostic = std::format(
            "unsatisfied {} dependency '{}' for '{}': running version {} fails comparison {}",
            failure.Domain, failure.Constraint->Original, failure.Owner, failure.Running->Text,
            tiri::comparison_text(*failure.Comparison));
      }
   }

   if (node->Lookup.Cached.CacheHit and not this->link_references(*node, payload)) {
      node->Lookup.Cached.CacheHit = false;
      node->Lookup.Cached.Payload.clear();
   }

   node->State = node->Lookup.Cached.CacheHit ? NodeState::VALID : NodeState::INVALID;
   Output = node->Lookup;
   return ERR::Okay;
}
