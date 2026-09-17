#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/filesystem.h>
#include <kotuku/modules/processes.h>

#include "../import_module_cache.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <thread>

#ifdef UNIT_TESTS
namespace {

using namespace tiri::import_cache;

bool write_file(std::string_view Path, std::string_view Content)
{
   DeleteFile(std::string(Path), nullptr);
   objFile::create file = { fl::Path(Path), fl::Flags(FL::NEW|FL::WRITE) };
   if (not file.ok()) return false;
   int written = 0;
   return file->write(std::span((const int8_t *)Content.data(), Content.size()), &written) IS ERR::Okay and
      written IS int(Content.size()) and file->flush() IS ERR::Okay;
}

std::string unique_path(std::string_view Name)
{
   static std::atomic<uint32_t> sequence = 0;
   int process_id = CurrentTask() ? CurrentTask()->ProcessID : 0;
   return std::format("temp:tiri-i03-{}-{}-{}.tiri", Name, process_id, sequence.fetch_add(1));
}

CompilationRequest request_for(const std::string &Path)
{
   CompilationRequest result;
   result.ExpectedIdentity.BuildIdentity = "i03-test-build";
   result.ExpectedIdentity.LogicalRequest = "i03/library";
   result.ExpectedIdentity.Source.ResolvedPath = Path;
   result.CacheDirectory = "temp:tiri/i03-cache/";
   return result;
}

Interface interface_for(const Identity &IdentityValue)
{
   Interface result;
   std::string filename = IdentityValue.Source.ResolvedPath;
   auto separator = filename.find_last_of("/:\\");
   if (separator != std::string::npos) filename.erase(0, separator + 1);
   result.Sources.push_back({ IdentityValue.Source.ResolvedPath, IdentityValue.LogicalRequest, filename,
      "i03_fixture", "", 1, 1, 0 });
   result.Namespaces.push_back({ "i03_fixture", NamespaceMode::DECLARE });
   return result;
}

PayloadValidator payload_validator()
{
   return [](std::string_view Payload, std::string &Reason) {
      if (Payload.starts_with("\x1bLJ") and Payload.ends_with("complete")) return true;
      Reason = "invalid test bytecode";
      return false;
   };
}

struct FixtureArtifact {
   Identity CompilationIdentity;
   Interface CompileTimeInterface;
   std::string Payload;
};

bool prepare_artifact(const ModuleLookup &Lookup, FixtureArtifact &Output)
{
   Output = {};
   Output.CompilationIdentity = Lookup.ExpectedIdentity;
   Output.CompilationIdentity.Schema = SCHEMA_VERSION;
   Output.CompilationIdentity.ImportedRoot = true;
   Output.CompilationIdentity.ConditionalInputs.push_back({ tiri::cache::ConditionalKind::IMPORTED,
      "imported", Output.CompilationIdentity.Source.ResolvedPath, "true" });
   if (finalise_identity(Output.CompilationIdentity) != tiri::cache::FormatError::OKAY) return false;
   Output.CompileTimeInterface = interface_for(Output.CompilationIdentity);
   Output.Payload.assign("\x1bLJ", 3);
   Output.Payload.append(Lookup.Source);
   Output.Payload.append("-complete");
   return true;
}

bool has_staging_file(std::string_view Directory, std::string_view Destination)
{
   auto separator = Destination.find_last_of("/:\\");
   const std::string marker = "." + std::string(Destination.substr(separator + 1)) + ".tmp.";
   objFile::create folder = { fl::Path(Directory), fl::Flags(FL::FOLDER|FL::READ|FL::EXCLUDE_FOLDERS) };
   if (not folder.ok()) return false;
   for (;;) {
      objFile *entry = nullptr;
      if (folder->next(&entry) != ERR::Okay) break;
      const bool staging = entry->Path.find(marker) != std::string::npos;
      FreeResource(entry->UID);
      if (staging) return true;
   }
   return false;
}

struct Cleanup {
   std::vector<std::string> Paths;
   ~Cleanup() { for (const auto &path : Paths) DeleteFile(path, nullptr); }
};

bool cold_hit_and_invalidation(kt::Log &Log)
{
   const std::string source_path = unique_path("source");
   if (not write_file(source_path, "first")) return false;
   Cleanup cleanup { { source_path } };

   auto request = request_for(source_path);
   LifecycleCounters counters;
   SourceSnapshot cold_snapshot;
   ModuleLookup cold_lookup;
   if (snapshot_source(source_path, counters, cold_snapshot) != ERR::Okay or
       lookup_module(request, cold_snapshot, {}, payload_validator(), counters, cold_lookup) != ERR::Okay or
       cold_lookup.Cached.CacheHit or cold_lookup.Source != "first" or counters.SourceReads != 1 or
       counters.LookupMisses != 1 or counters.SourceCompilations != 0) {
      Log.error("Cold imported-module lookup did not preserve one parser-ready source snapshot");
      return false;
   }

   counters.SourceCompilations++;
   FixtureArtifact cold;
   ModulePublication publication;
   if (not prepare_artifact(cold_lookup, cold) or
       publish_module(request, cold.CompilationIdentity, cold.CompileTimeInterface, cold.Payload,
          counters, publication) != ERR::Okay or publication.StorageError != ERR::Okay or
       counters.Publications != 1) {
      Log.error("Direct imported-module publication did not store one complete generation");
      return false;
   }
   cleanup.Paths.push_back(publication.CachePath);

   request.ExpectedIdentity = cold.CompilationIdentity;
   SourceSnapshot warm_snapshot;
   ModuleLookup warm;
   if (snapshot_source(source_path, counters, warm_snapshot) != ERR::Okay or
       lookup_module(request, warm_snapshot, {}, payload_validator(), counters, warm) != ERR::Okay or
       not warm.Cached.CacheHit or counters.CacheHits != 1 or counters.SourceReads != 2 or
       counters.EnvelopeDecodes != 1 or counters.PayloadValidations != 1 or
       counters.SourceCompilations != 1 or warm.Cached.CompilationIdentity.LookupIdentity !=
          cold.CompilationIdentity.LookupIdentity or warm.Cached.CompileTimeInterface != cold.CompileTimeInterface or
       warm.Cached.Payload != cold.Payload) {
      Log.error("A fresh imported-module lookup did not return the exact published generation");
      return false;
   }

   if (not write_file(source_path, "second")) return false;
   ModuleLookup stable;
   if (lookup_module(request, warm_snapshot, {}, payload_validator(), counters, stable) != ERR::Okay or
       not stable.Cached.CacheHit or stable.Source != "first" or counters.SourceReads != 2 or
       counters.CacheHits != 2) {
      Log.error("A captured source snapshot changed within its validation session");
      return false;
   }

   SourceSnapshot changed_snapshot;
   ModuleLookup changed_lookup;
   if (snapshot_source(source_path, counters, changed_snapshot) != ERR::Okay or
       lookup_module(request, changed_snapshot, {}, payload_validator(), counters, changed_lookup) != ERR::Okay or
       changed_lookup.Cached.CacheHit or changed_lookup.Source != "second" or counters.LookupMisses != 2 or
       counters.SourceReads != 3 or counters.SourceCompilations != 1) {
      Log.error("Changed imported-module source did not select a fresh cold lookup");
      return false;
   }

   counters.SourceCompilations++;
   FixtureArtifact changed;
   ModulePublication changed_publication;
   if (not prepare_artifact(changed_lookup, changed) or
       publish_module(request, changed.CompilationIdentity, changed.CompileTimeInterface, changed.Payload,
          counters, changed_publication) != ERR::Okay or changed_publication.StorageError != ERR::Okay or
       changed_publication.CachePath IS publication.CachePath) {
      Log.error("Changed imported-module source did not publish a distinct generation");
      return false;
   }
   cleanup.Paths.push_back(changed_publication.CachePath);
   return true;
}

bool malformed_rejection_and_publication_failure(kt::Log &Log)
{
   const std::string source_path = unique_path("fallback");
   if (not write_file(source_path, "fallback")) return false;
   Cleanup cleanup { { source_path } };
   auto request = request_for(source_path);
   LifecycleCounters counters;
   SourceSnapshot snapshot;
   ModuleLookup cold;
   FixtureArtifact artifact;
   ModulePublication initial;
   if (snapshot_source(source_path, counters, snapshot) != ERR::Okay or
       lookup_module(request, snapshot, {}, payload_validator(), counters, cold) != ERR::Okay or
       not prepare_artifact(cold, artifact) or
       publish_module(request, artifact.CompilationIdentity, artifact.CompileTimeInterface, artifact.Payload,
          counters, initial) != ERR::Okay or initial.StorageError != ERR::Okay) return false;
   cleanup.Paths.push_back(initial.CachePath);
   request.ExpectedIdentity = artifact.CompilationIdentity;

   if (not write_file(initial.CachePath, "TIRIMOD1-truncated")) return false;
   ModuleLookup malformed;
   if (lookup_module(request, snapshot, {}, payload_validator(), counters, malformed) != ERR::Okay or
       malformed.Cached.CacheHit or malformed.Source != "fallback") {
      Log.error("Malformed imported-module cache did not leave source available for parser fallback");
      return false;
   }

   ModulePublication replacement;
   if (publish_module(request, artifact.CompilationIdentity, artifact.CompileTimeInterface, artifact.Payload,
       counters, replacement) != ERR::Okay or replacement.StorageError != ERR::Okay) return false;

   IdentityValidator reject = [](const Identity &, std::string &Reason) {
      Reason = "redirected resolver observation";
      return false;
   };
   ModuleLookup rejected;
   if (lookup_module(request, snapshot, reject, payload_validator(), counters, rejected) != ERR::Okay or
       rejected.Cached.CacheHit or rejected.Cached.Diagnostic != "redirected resolver observation" or
       rejected.Source != "fallback") {
      Log.error("A rejected cache candidate did not retain its diagnostic and parser fallback source");
      return false;
   }

   ModulePublication accepted_replacement;
   if (publish_module(request, artifact.CompilationIdentity, artifact.CompileTimeInterface, artifact.Payload,
       counters, accepted_replacement) != ERR::Okay or accepted_replacement.StorageError != ERR::Okay) return false;

   DeleteFile(accepted_replacement.CachePath, nullptr);
   set_module_publish_failure(ModulePublishFailure::WRITE);
   ModulePublication unpublished;
   if (publish_module(request, artifact.CompilationIdentity, artifact.CompileTimeInterface, artifact.Payload,
       counters, unpublished) != ERR::Okay or unpublished.StorageError IS ERR::Okay or
       AnalysePath(unpublished.CachePath, nullptr) IS ERR::Okay or
       has_staging_file(request.CacheDirectory, unpublished.CachePath)) {
      Log.error("A publication failure exposed a destination or leaked its staging file");
      return false;
   }
   return true;
}

bool snapshot_normalisation_and_size_change(kt::Log &Log)
{
   constexpr std::array<std::string_view, 3> marks = { "\xef\xbb\xbf", "\xfe\xff", "\xff\xfe" };
   LifecycleCounters counters;
   Cleanup cleanup;
   for (size_t i = 0; i < marks.size(); ++i) {
      const std::string path = unique_path(std::format("bom-{}", i));
      cleanup.Paths.push_back(path);
      std::string content(marks[i]);
      content += "normalised";
      SourceSnapshot snapshot;
      if (not write_file(path, content) or snapshot_source(path, counters, snapshot) != ERR::Okay or
          snapshot.Source != "normalised" or snapshot.Identity.Size != snapshot.Source.size() or
          snapshot.Identity.ContentDigest != tiri::cache::content_digest(snapshot.Source)) {
         Log.error("A recognised source BOM was not removed before identity capture");
         return false;
      }
   }

   const std::string changed_path = unique_path("size-change");
   cleanup.Paths.push_back(changed_path);
   if (not write_file(changed_path, "stable bytes")) return false;
   force_snapshot_final_size_change();
   SourceSnapshot rejected;
   const uint32_t reads_before = counters.SourceReads;
   if (snapshot_source(changed_path, counters, rejected) != ERR::Read or not rejected.Source.empty() or
       not rejected.Identity.ResolvedPath.empty() or counters.SourceReads != reads_before) {
      Log.error("A final source-size change produced a reusable snapshot");
      return false;
   }
   return true;
}

bool concurrent_complete_generations(kt::Log &Log)
{
   const std::string source_path = unique_path("concurrent");
   if (not write_file(source_path, "concurrent")) return false;
   Cleanup cleanup { { source_path } };
   auto request = request_for(source_path);
   LifecycleCounters lookup_counters;
   SourceSnapshot snapshot;
   ModuleLookup cold;
   FixtureArtifact artifact;
   if (snapshot_source(source_path, lookup_counters, snapshot) != ERR::Okay or
       lookup_module(request, snapshot, {}, payload_validator(), lookup_counters, cold) != ERR::Okay or
       not prepare_artifact(cold, artifact)) return false;

   constexpr size_t producer_count = 4;
   std::array<LifecycleCounters, producer_count> counters;
   std::array<ModulePublication, producer_count> outputs;
   std::array<ERR, producer_count> errors;
   std::array<std::thread, producer_count> producers;
   std::atomic<size_t> ready = 0;
   std::atomic<bool> start = false;
   for (size_t i = 0; i < producer_count; ++i) {
      producers[i] = std::thread([&, i] {
         ready.fetch_add(1, std::memory_order_release);
         while (not start.load(std::memory_order_acquire)) std::this_thread::yield();
         errors[i] = publish_module(request, artifact.CompilationIdentity, artifact.CompileTimeInterface,
            artifact.Payload, counters[i], outputs[i]);
      });
   }
   while (ready.load(std::memory_order_acquire) != producer_count) std::this_thread::yield();
   start.store(true, std::memory_order_release);
   for (auto &producer : producers) producer.join();

   bool published = false;
   for (size_t i = 0; i < producer_count; ++i) {
      if (errors[i] != ERR::Okay) {
         Log.error("A concurrent publisher failed before the best-effort storage boundary");
         return false;
      }
      if (outputs[i].StorageError IS ERR::Okay) published = true;
      cleanup.Paths.push_back(outputs[i].CachePath);
   }
   if (not published or has_staging_file(request.CacheDirectory, outputs[0].CachePath)) {
      Log.error("Concurrent publishers left no complete generation or leaked a staging file");
      return false;
   }

   request.ExpectedIdentity = artifact.CompilationIdentity;
   LifecycleCounters reader_counters;
   ModuleLookup reader;
   if (lookup_module(request, snapshot, {}, payload_validator(), reader_counters, reader) != ERR::Okay or
       not reader.Cached.CacheHit or reader.Cached.CompilationIdentity.LookupIdentity !=
          artifact.CompilationIdentity.LookupIdentity or
       reader.Cached.CompileTimeInterface != artifact.CompileTimeInterface or
       reader.Cached.Payload != artifact.Payload) {
      Log.error("A reader did not observe one complete generation after concurrent publication");
      return false;
   }
   return true;
}

} // namespace

void import_module_cache_unit_tests(int &Passed, int &Total)
{
   kt::Log log("ImportModuleCacheTests");
   for (auto test : { cold_hit_and_invalidation, malformed_rejection_and_publication_failure,
      snapshot_normalisation_and_size_change, concurrent_complete_generations }) {
      Total++;
      if (test(log)) Passed++;
   }
}
#endif
