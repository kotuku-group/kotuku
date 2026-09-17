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
   uint32_t SourceCompilations = 0;
   uint32_t Hits = 0;
   uint32_t Misses = 0;
   uint32_t Publications = 0;
};

struct CompilationRequest {
   Identity ExpectedIdentity;
   std::string CacheDirectory = "temp:tiri/cache/";
   PERMIT Permissions = PERMIT::USER;
   std::vector<std::string> *ImportStack = nullptr;
};

struct CompiledModule {
   Identity CompilationIdentity;
   std::string LookupIdentity;
   std::string CompiledIdentity;
   Interface CompileTimeInterface;
   std::string Payload;
   std::string CachePath;
   std::string Diagnostic;
   ERR PublicationError = ERR::Okay;
   bool CacheHit = false;
};

struct ModuleLookup {
   Identity ExpectedIdentity;
   std::string Source;
   CompiledModule Cached;
   std::vector<RootModuleRecord> EmbeddedModules;
};

using ModuleCompiler = std::function<ERR(std::string_view, Identity &, Interface &, std::string &, std::string &)>;
using IdentityValidator = std::function<bool(const Identity &, std::string &)>;
using PayloadValidator = std::function<bool(std::string_view, std::string &)>;

// Resolve the request before calling this service.  The service owns the remaining lifecycle: a single source
// snapshot, exact candidate validation, cold compilation, validated in-memory handoff and best-effort publication.
// ModuleCompiler is expected to compile in an isolated state and transfer only the portable interface and bytecode.

[[nodiscard]] ERR load_or_compile_module(const CompilationRequest &, const ModuleCompiler &,
   const IdentityValidator &, const PayloadValidator &, LifecycleCounters &, CompiledModule &);

// Split lookup/publication is used by the parser: a cold module must first participate in the importing root's
// analysis before its finished initialiser prototype can be serialised.  Lookup still owns the single source snapshot
// and all cache validation; publication accepts only the identity derived from that snapshot.

[[nodiscard]] ERR lookup_module(const CompilationRequest &, const IdentityValidator &, const PayloadValidator &,
   LifecycleCounters &, ModuleLookup &);
[[nodiscard]] ERR publish_module(const CompilationRequest &, const Identity &, const Interface &, std::string_view,
   LifecycleCounters &, CompiledModule &);

#ifdef UNIT_TESTS
enum class ModulePublishFailure : uint8_t { NIL, CREATE, WRITE, FLUSH, MOVE };
void set_module_publish_failure(ModulePublishFailure);
#endif

} // namespace tiri::import_cache
