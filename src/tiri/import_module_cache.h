#pragma once

#include <kotuku/main.h>

#include "import_module_format.h"
#include "import_module_bundle.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tiri::import_cache {

struct LifecycleCounters {
   uint32_t CacheHits = 0;
   uint32_t LookupMisses = 0;
   uint32_t SourceReads = 0;
   uint32_t EnvelopeDecodes = 0;
   uint32_t PayloadValidations = 0;
   uint32_t ValidationReuses = 0;
   uint32_t ValidationStateCreations = 0;
   uint32_t SourceCompilations = 0;
   uint32_t Publications = 0;
};

struct SourceSnapshot {
   std::string Source;
   cache::SourceIdentity Identity;
};

struct CompilationRequest {
   Identity ExpectedIdentity;
   std::string CacheDirectory = "temp:tiri/cache/";
   PERMIT Permissions = PERMIT::USER;
};

struct CompiledModule {
   Identity CompilationIdentity;
   std::string LookupIdentity;
   std::string CompiledIdentity;
   Interface CompileTimeInterface;
   std::string Payload;
   std::string CachePath;
   std::string Diagnostic;
   bool CacheHit = false;
};

struct ModulePublication {
   std::string CachePath;
   ERR StorageError = ERR::Okay;
};

struct ModuleLookup {
   Identity ExpectedIdentity;
   std::string Source;
   CompiledModule Cached;
   std::vector<RootModuleRecord> EmbeddedModules;
};

using IdentityValidator = std::function<bool(const Identity &, std::string &)>;
using PayloadValidator = std::function<bool(std::string_view, std::string &)>;

// Imported-module caching follows the parser lifecycle: snapshot-driven lookup validates a candidate against bytes
// owned by the current compilation, and best-effort publication accepts the parser's finished identity, portable
// interface and serialised initialiser.

[[nodiscard]] ERR snapshot_source(std::string_view, LifecycleCounters &, SourceSnapshot &);
[[nodiscard]] ERR lookup_module(const CompilationRequest &, const SourceSnapshot &, const IdentityValidator &,
   const PayloadValidator &, LifecycleCounters &, ModuleLookup &);
[[nodiscard]] ERR lookup_module(const CompilationRequest &, const IdentityValidator &, const PayloadValidator &,
   LifecycleCounters &, ModuleLookup &);
[[nodiscard]] ERR publish_module(const CompilationRequest &, const Identity &, const Interface &, std::string_view,
   LifecycleCounters &, ModulePublication &);

#ifdef UNIT_TESTS
enum class ModulePublishFailure : uint8_t { NIL, CREATE, WRITE, FLUSH, MOVE };
void set_module_publish_failure(ModulePublishFailure);
void force_snapshot_final_size_change();
#endif

} // namespace tiri::import_cache
