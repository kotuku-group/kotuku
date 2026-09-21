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
   uint32_t CacheHits = 0;                       // Counts modules returned from the cache.
   uint32_t LookupMisses = 0;                    // Counts cache lookups without a matching module.
   uint32_t SourceReads = 0;                     // Counts reads of module source content.
   uint32_t EnvelopeDecodes = 0;                 // Counts decoded cache-file envelopes.
   uint32_t PayloadValidations = 0;              // Counts validations of cached payloads.
   uint32_t PayloadBundleDecodes = 0;            // Counts decoded payload bundles.
   uint32_t ValidationReuses = 0;                // Counts reuses of an existing validation state.
   uint32_t ValidationStateCreations = 0;        // Counts newly created validation states.
   uint32_t SourceCompilations = 0;              // Counts modules compiled from source.
   uint32_t Publications = 0;                    // Counts cache-module publication attempts.
   uint32_t ValidationPayloadLoads = 0;          // Counts payload loads used during validation.
   uint32_t DestinationPayloadLoads = 0;         // Counts payload loads for the destination module.
   uint32_t PrototypeDecodes = 0;                // Counts decoded Lua prototypes.
   uint32_t SourceRecordsDecoded = 0;            // Counts decoded source records.
   uint32_t FileSourceRegistrations = 0;         // Counts registered file-source records.
   uint32_t LineMapRemaps = 0;                   // Counts remapped source line maps.
   uint32_t StructureCommits = 0;                // Counts committed module structures.
   uint32_t SourceMapAllocations = 0;            // Counts allocated source maps.
   uint32_t ExecutableDirectoryAllocations = 0;  // Counts allocated executable-directory paths.
   InterfaceOperationCounters InterfaceOperations; // Tracks interface operations performed during the lifecycle.
};

struct SourceSnapshot {
   std::string Source;              // Stores the source bytes captured for compilation.
   cache::SourceIdentity Identity;  // Identifies the captured source content.
};

struct CompilationRequest {
   Identity ExpectedIdentity;                        // Identifies the module expected by the caller.
   std::string CacheDirectory = "temp:tiri/cache/";  // Specifies where compiled modules are cached.
   PERMIT Permissions = PERMIT::USER;                // Specifies permissions for the compilation.
};

struct CompiledModule {
   Identity CompilationIdentity;                // Identifies the source used to compile the module.
   std::string LookupIdentity;                  // Stores the identity used for the cache lookup.
   std::string CompiledIdentity;                // Stores the identity assigned to the compiled module.
   FinalisedInterfacePtr CompileTimeInterface;  // Holds the interface available while compiling the module.
   std::string Payload;                         // Stores the serialised compiled-module payload.
   std::string CachePath;                       // Stores the path of the cached module file.
   std::string Diagnostic;                      // Stores a diagnostic produced during compilation or loading.
   bool CacheHit = false;                       // Indicates whether the module was obtained from the cache.
};

struct ModulePublication {
   std::string CachePath;             // Stores the path of the published cache file.
   ERR StorageError = ERR::Okay;      // Stores the result of the cache-storage operation.
};

struct ModuleLookup {
   Identity ExpectedIdentity;                      // Identifies the module expected by the caller.
   std::string Source;                             // Stores the source used for the lookup.
   CompiledModule Cached;                          // Holds the cached module, when one was found.
   std::vector<RootModuleRecord> EmbeddedModules;  // Holds root modules embedded in the cached payload.
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
[[nodiscard]] ERR publish_module(const CompilationRequest &, const Identity &, const FinalisedInterface &,
   std::string_view, LifecycleCounters &, ModulePublication &);

#ifdef UNIT_TESTS
enum class ModulePublishFailure : uint8_t { NIL, CREATE, WRITE, FLUSH, MOVE };
void set_module_publish_failure(ModulePublishFailure);
#endif

} // namespace tiri::import_cache
