#include <kotuku/main.h>
#include <kotuku/modules/core.h>
#include <kotuku/modules/filesystem.h>
#include <kotuku/modules/processes.h>

#include "import_module_cache.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>

namespace tiri::import_cache {
namespace {

constexpr int MAX_TEMP_ATTEMPTS = 16;
std::atomic<uint64_t> glModuleTemporarySequence = 0;

//********************************************************************************************************************
// Injects a requested publication failure once for unit-test coverage.

#ifdef UNIT_TESTS
ModulePublishFailure glModulePublishFailure = ModulePublishFailure::NIL;

bool fail_publication(ModulePublishFailure Stage)
{
   if (glModulePublishFailure != Stage) return false;
   glModulePublishFailure = ModulePublishFailure::NIL;
   return true;
}
#else
enum class ModulePublishFailure : uint8_t { NIL, CREATE, WRITE, FLUSH, MOVE };
bool fail_publication(ModulePublishFailure) { return false; }
#endif

//********************************************************************************************************************
// Reads a stable snapshot of an open file and records its best-effort modification timestamp.

ERR read_open_file(objFile *File, std::string &Output, int64_t &ModifiedHint)
{
   int64_t initial_size = 0;
   if (auto error = File->getSize(initial_size); error != ERR::Okay) return error;
   if ((initial_size < 0) or (uint64_t(initial_size) > cache::MAX_PAYLOAD_SIZE)) return ERR::BufferOverflow;

   Output.assign(size_t(initial_size), '\0');
   int64_t total = 0;
   while (total < initial_size) {
      int bytes_read = 0;
      auto output = std::span((int8_t *)Output.data() + total, size_t(initial_size - total));
      auto error = File->read(output, &bytes_read);
      if ((error != ERR::Okay) or (bytes_read <= 0)) return error IS ERR::Okay ? ERR::Read : error;
      total += bytes_read;
   }

   int64_t final_size = 0;
   if (File->getSize(final_size) != ERR::Okay or final_size != initial_size) return ERR::Read;
   ModifiedHint = 0;
   File->getTimestamp(ModifiedHint);
   return ERR::Okay;
}

//********************************************************************************************************************
// Removes a recognised byte-order mark before compiling source text.

void normalise_bom(std::string &Source)
{
   std::string_view content = Source;
   if (content.starts_with("\xef\xbb\xbf")) content.remove_prefix(3);
   else if (content.starts_with("\xfe\xff") or content.starts_with("\xff\xfe")) content.remove_prefix(2);
   if (content.data() != Source.data()) Source.assign(content);
}

//********************************************************************************************************************
// Builds the cache filename for the dependency-independent lookup identity.

std::string cache_path(std::string Directory, const Identity &IdentityValue)
{
   if (not Directory.ends_with('/') and not Directory.ends_with('\\')) Directory.push_back('/');
   return Directory + lookup_key(IdentityValue) + ".tbc";
}

//********************************************************************************************************************
// Creates a process-, thread- and sequence-specific staging path beside the destination.

std::string temporary_path(std::string_view Destination)
{
   std::string path;
   auto separator = Destination.find_last_of(":/\\");
   if (separator IS std::string_view::npos) path.push_back('.');
   else {
      path.assign(Destination, 0, separator + 1);
      path.push_back('.');
      Destination.remove_prefix(separator + 1);
   }
   path.append(Destination);

   int process_id = 0;
   if (auto task = CurrentTask()) process_id = task->ProcessID;
   auto sequence = glModuleTemporarySequence.fetch_add(1, std::memory_order_relaxed) + 1;
   path.append(std::format(".tmp.{}.{}.{}", process_id, GetThreadID(), sequence));
   return path;
}

//********************************************************************************************************************
// Publishes complete content through an exclusive temporary file and atomic move.

ERR write_complete_file(const std::string &Destination, std::string_view Content, PERMIT Permissions)
{
   kt::Log log(__FUNCTION__);
   for (int attempt = 0; attempt < MAX_TEMP_ATTEMPTS; ++attempt) {
      auto staging = temporary_path(Destination);
      bool owns_staging = false;
      auto cleanup = kt::deferred_call([&] {
         if (owns_staging and (AnalysePath(staging, nullptr) IS ERR::Okay)) {
            if (auto error = DeleteFile(staging, nullptr); error != ERR::Okay) {
               log.warning("Failed to remove temporary module cache '%s': %s", staging.c_str(), GetErrorMsg(error));
            }
         }
      });

      if (fail_publication(ModulePublishFailure::CREATE)) return ERR::TestFailed;
      ERR error = ERR::Okay;
      {
         objFile::create output = {
            fl::Path(staging), fl::Flags(FL::NEW|FL::WRITE|FL::EXCLUSIVE), fl::Permissions(Permissions)
         };
         error = output.error;
         if (error IS ERR::FileExists) continue;
         if (not output.ok()) return error;
         owns_staging = true;

         if (fail_publication(ModulePublishFailure::WRITE)) error = ERR::TestFailed;
         size_t total = 0;
         while ((error IS ERR::Okay) and (total < Content.size())) {
            auto count = std::min(Content.size() - total, size_t(std::numeric_limits<int>::max()));
            int written = 0;
            error = output->write(std::span((const int8_t *)Content.data() + total, count), &written);
            if ((error IS ERR::Okay) and (written != int(count))) error = ERR::Write;
            if (written > 0) total += size_t(written);
         }
         if (error IS ERR::Okay) {
            if (fail_publication(ModulePublishFailure::FLUSH)) error = ERR::TestFailed;
            else error = output->flush();
         }
      }
      if (error != ERR::Okay) return error;
      if (fail_publication(ModulePublishFailure::MOVE)) return ERR::TestFailed;

      error = MoveFile(staging, Destination, nullptr);
      if (error IS ERR::Okay) owns_staging = false;
      return error;
   }
   return ERR::FileExists;
}

//********************************************************************************************************************
// Compares identities through the canonical dependency-independent lookup key.

bool identity_matches(const Identity &Stored, const Identity &Expected)
{
   auto expected_key = lookup_key(Expected);
   return not Stored.LookupIdentity.empty() and not expected_key.empty() and
      Stored.LookupIdentity IS expected_key;
}

//********************************************************************************************************************
// Loads and validates a cache candidate, leaving the output ready for immediate use on success.

bool try_cache(const std::string &Path, const Identity &Expected, const IdentityValidator &ValidateIdentity,
   const PayloadValidator &ValidatePayload, LifecycleCounters &Counters, CompiledModule &Output)
{
   objFile::create input = { fl::Path(Path), fl::Flags(FL::READ) };
   if (not input.ok()) return false;

   std::string content;
   int64_t modified = 0;
   if (read_open_file(*input, content, modified) != ERR::Okay) return false;

   EnvelopeView envelope;
   Counters.EnvelopeDecodes++;
   if (decode_envelope(content, envelope, &Counters.InterfaceOperations) != cache::FormatError::OKAY or
       not identity_matches(envelope.CompilationIdentity, Expected)) return false;

   const auto &stored_identity = envelope.CompilationIdentity;
   const auto &interface_package = envelope.CompileTimeInterface->descriptors().Package;
   if (stored_identity.ExpectedPackage != Expected.ExpectedPackage or
       stored_identity.DeclaredPackage != interface_package or
       (Expected.ExpectedPackage and interface_package != Expected.ExpectedPackage)) {
      Output.Diagnostic = "imported-module package identity metadata does not agree";
      return false;
   }

   std::string reason;
   if (ValidateIdentity and not ValidateIdentity(envelope.CompilationIdentity, reason)) {
      Output.Diagnostic = std::move(reason);
      return false;
   }
   if (ValidatePayload) {
      Counters.PayloadValidations++;
      if (not ValidatePayload(envelope.Payload, reason)) {
         Output.Diagnostic = std::move(reason);
         return false;
      }
   }
   Output.CompilationIdentity = std::move(envelope.CompilationIdentity);
   Output.LookupIdentity = std::move(envelope.LookupIdentity);
   Output.CompiledIdentity = std::move(envelope.CompiledIdentity);
   Output.CompileTimeInterface = std::move(envelope.CompileTimeInterface);
   Output.Payload.assign(envelope.Payload);
   Output.CachePath = Path;
   Output.CacheHit = true;
   return true;
}

} // namespace

#ifdef UNIT_TESTS
// Selects the next publication stage that a unit test should fail.

void set_module_publish_failure(ModulePublishFailure Failure)
{
   glModulePublishFailure = Failure;
}

#endif

//********************************************************************************************************************
// Captures one stable, BOM-normalised source snapshot and its complete identity.

ERR snapshot_source(std::string_view Path, LifecycleCounters &Counters, SourceSnapshot &Output)
{
   Output = {};
   if (Path.empty()) return ERR::NullArgs;

   objFile::create source_file = { fl::Path(std::string(Path)), fl::Flags(FL::READ) };
   if (not source_file.ok()) return source_file.error;

   SourceSnapshot captured;
   if (auto error = read_open_file(*source_file, captured.Source, captured.Identity.ModifiedHint);
       error != ERR::Okay) return error;
   normalise_bom(captured.Source);

   captured.Identity.ResolvedPath.assign(Path);
   captured.Identity.Size = captured.Source.size();
   captured.Identity.ContentDigest = cache::content_digest(captured.Source);
   Output = std::move(captured);
   Counters.SourceReads++;
   return ERR::Okay;
}

//********************************************************************************************************************
// Captures source identity and attempts a cache lookup before parser-led compilation.

ERR lookup_module(const CompilationRequest &Request, const SourceSnapshot &Snapshot,
   const IdentityValidator &ValidateIdentity, const PayloadValidator &ValidatePayload,
   LifecycleCounters &Counters, ModuleLookup &Output)
{
   kt::Log log(__FUNCTION__);
   Output = {};
   if (Request.ExpectedIdentity.Source.ResolvedPath.empty() or
       Request.ExpectedIdentity.LogicalRequest.empty()) return ERR::NullArgs;

   const auto &source_path = Request.ExpectedIdentity.Source.ResolvedPath;
   if (Snapshot.Identity.ResolvedPath != source_path) return ERR::InvalidData;

   Output.ExpectedIdentity = Request.ExpectedIdentity;
   Output.ExpectedIdentity.Source = Snapshot.Identity;
   Output.Source = Snapshot.Source;
   const auto selected_path = cache_path(Request.CacheDirectory, Output.ExpectedIdentity);

   if (try_cache(selected_path, Output.ExpectedIdentity, ValidateIdentity, ValidatePayload, Counters, Output.Cached)) {
      Counters.CacheHits++;
      log.detail("Imported-module cache hit '%s'.", selected_path.c_str());
   }
   else {
      Counters.LookupMisses++;
      log.detail("Imported-module source rebuild '%s'%s%s.", selected_path.c_str(),
         Output.Cached.Diagnostic.empty() ? "" : ": ", Output.Cached.Diagnostic.c_str());
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Convenience lookup for callers that do not own a compilation-scoped snapshot cache.

ERR lookup_module(const CompilationRequest &Request, const IdentityValidator &ValidateIdentity,
   const PayloadValidator &ValidatePayload, LifecycleCounters &Counters, ModuleLookup &Output)
{
   SourceSnapshot snapshot;
   if (auto error = snapshot_source(Request.ExpectedIdentity.Source.ResolvedPath, Counters, snapshot);
       error != ERR::Okay) return error;
   return lookup_module(Request, snapshot, ValidateIdentity, ValidatePayload, Counters, Output);
}

//********************************************************************************************************************
// Encodes and best-effort publishes a parser-compiled module.

ERR publish_module(const CompilationRequest &Request, const Identity &IdentityValue,
   const FinalisedInterface &InterfaceValue, std::string_view Payload, LifecycleCounters &Counters,
   ModulePublication &Output)
{
   Output = {};
   if (IdentityValue.Source.ResolvedPath.empty() or IdentityValue.LogicalRequest.empty() or Payload.empty()) {
      return ERR::NullArgs;
   }

   Identity final_identity = IdentityValue;
   if (finalise_identity(final_identity) != cache::FormatError::OKAY) return ERR::InvalidData;
   if (final_identity.DeclaredPackage != InterfaceValue.descriptors().Package or
       (final_identity.ExpectedPackage and final_identity.ExpectedPackage != final_identity.DeclaredPackage)) {
      return ERR::InvalidData;
   }

   std::string envelope;
   if (encode_envelope(final_identity, InterfaceValue, Payload, envelope) != cache::FormatError::OKAY) {
      return ERR::InvalidData;
   }

   Output.CachePath = cache_path(Request.CacheDirectory, final_identity);

   auto folder_error = CreateFolder(Request.CacheDirectory, PERMIT::USER);
   if ((folder_error IS ERR::Okay) or (folder_error IS ERR::FileExists)) {
      Output.StorageError = write_complete_file(Output.CachePath, envelope, Request.Permissions);
      if (Output.StorageError IS ERR::Okay) Counters.Publications++;
   }
   else Output.StorageError = folder_error;
   return ERR::Okay;
}

} // namespace tiri::import_cache
