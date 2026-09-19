#pragma once

#include "../../../import_module_cache.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct lua_State;

class ImportModuleValidationSession {
public:
   struct Environment {
      std::function<std::string(std::string_view)> ResolveLibrary;
      std::function<bool(std::string_view)> ModuleAvailable;
   };

   ImportModuleValidationSession(lua_State &, tiri::import_cache::LifecycleCounters &, Environment);

   [[nodiscard]] ERR ensure_validated(
      const tiri::import_cache::CompilationRequest &, tiri::import_cache::ModuleLookup &);

private:
   enum class NodeState : uint8_t { IN_PROGRESS, VALID, INVALID };

   struct Node {
      NodeState State = NodeState::IN_PROGRESS;
      tiri::import_cache::ModuleLookup Lookup;
   };

   lua_State &lua;
   tiri::import_cache::LifecycleCounters &counters;
   Environment environment;
   std::unordered_map<std::string, std::shared_ptr<const tiri::import_cache::SourceSnapshot>> snapshots;
   std::unordered_map<std::string, ERR> snapshot_errors;
   std::unordered_map<std::string, Node> nodes;

   [[nodiscard]] ERR snapshot(
      std::string_view, std::shared_ptr<const tiri::import_cache::SourceSnapshot> &);
   [[nodiscard]] bool validate_identity(
      const tiri::import_cache::Identity &, std::string &);
   [[nodiscard]] bool validate_payload(std::string_view, std::string &,
      std::vector<tiri::import_cache::RootModuleRecord> *, std::optional<tiri::PackageIdentity> *);
};
