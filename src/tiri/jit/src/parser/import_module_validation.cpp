#include "import_module_validation.h"
#include "../lib/load.h"

#include <ranges>
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
      else if (this->environment.ResolveLibrary) current = this->environment.ResolveLibrary(input.Name);

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
      tiri::import_cache::CompilationRequest request;
      request.ExpectedIdentity.BuildIdentity = IdentityValue.BuildIdentity;
      request.ExpectedIdentity.LogicalRequest = dependency.OriginalRequest;
      request.ExpectedIdentity.Source.ResolvedPath = dependency.ResolvedPath;
      request.ExpectedIdentity.Options = IdentityValue.Options;
      request.ExpectedIdentity.ImportedRoot = true;

      tiri::import_cache::ModuleLookup lookup;
      if (auto error = this->ensure_validated(request, lookup); error != ERR::Okay) {
         (void)error;
         Reason = "an imported module dependency is unavailable";
         return false;
      }
      if (not lookup.Cached.CacheHit or
          lookup.Cached.CompileTimeInterface->digest() != dependency.InterfaceDigest or
          lookup.Cached.CompiledIdentity != dependency.CompiledIdentity) {
         Reason = lookup.Cached.Diagnostic.empty() ?
            "an imported module dependency has changed or is unavailable" : lookup.Cached.Diagnostic;
         return false;
      }
   }
   return true;
}

//********************************************************************************************************************
// Validates bytecode in an isolated state and retains only decoded portable dependency records.

bool ImportModuleValidationSession::validate_payload(std::string_view Payload, std::string &Reason,
   std::vector<tiri::import_cache::RootModuleRecord> *EmbeddedModules)
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
   if (lj_load_with_bytecode_metadata(validation.get(), Payload, "=import-cache-validation", metadata) != 0) {
      auto message = lua_tostringview(validation.get(), -1);
      Reason.assign(message.data(), message.size());
      return false;
   }

   if (not metadata.Bytecode) {
      Reason = "the imported-module cache payload did not contain bytecode metadata";
      return false;
   }
   this->counters.PayloadBundleDecodes++;
   if (EmbeddedModules) *EmbeddedModules = std::move(metadata.ImportedModules);
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

   auto [entry, inserted] = this->nodes.emplace(key, Node{});
   (void)inserted;
   Node *node = &entry->second;
   auto unwind = kt::deferred_call([&] {
      if (node->State IS NodeState::IN_PROGRESS) this->nodes.erase(key);
   });

   auto identity_validator = [&](const tiri::import_cache::Identity &IdentityValue, std::string &Reason) {
      return this->validate_identity(IdentityValue, Reason);
   };
   std::vector<tiri::import_cache::RootModuleRecord> embedded_modules;
   auto payload_validator = [&](std::string_view Payload, std::string &Reason) {
      return this->validate_payload(Payload, Reason, &embedded_modules);
   };

   auto error = tiri::import_cache::lookup_module(
      Request, *source, identity_validator, payload_validator, this->counters, node->Lookup);
   if (error != ERR::Okay) return error;

   if (node->Lookup.Cached.CacheHit) node->Lookup.EmbeddedModules = std::move(embedded_modules);

   node->State = node->Lookup.Cached.CacheHit ? NodeState::VALID : NodeState::INVALID;
   Output = node->Lookup;
   return ERR::Okay;
}
