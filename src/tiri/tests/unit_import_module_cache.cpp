#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/filesystem.h>

#include "../import_module_cache.h"

#include <array>
#include <atomic>
#include <cstring>
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

ModuleCompiler counting_compiler(int &Calls)
{
   return [&](std::string_view Source, Identity &IdentityValue, Interface &InterfaceValue,
      std::string &Payload, std::string &) {
      Calls++;
      IdentityValue.ConditionalInputs.push_back({ tiri::cache::ConditionalKind::IMPORTED,
         "imported", IdentityValue.Source.ResolvedPath, "true" });
      InterfaceValue = interface_for(IdentityValue);
      Payload.assign("\x1bLJ", 3);
      Payload.append(Source);
      Payload.append("-complete");
      return ERR::Okay;
   };
}

struct Cleanup {
   std::vector<std::string> Paths;
   ~Cleanup() { for (const auto &path : Paths) DeleteFile(path, nullptr); }
};

bool cold_hit_and_invalidation(kt::Log &Log)
{
   const std::string source_path = "temp:tiri-i03-source.tiri";
   if (not write_file(source_path, "first")) return false;
   Cleanup cleanup { { source_path } };

   auto request = request_for(source_path);
   LifecycleCounters counters;
   CompiledModule cold;
   int compile_calls = 0;
   if (load_or_compile_module(request, counting_compiler(compile_calls), {}, payload_validator(), counters, cold) !=
       ERR::Okay or cold.CacheHit or cold.PublicationError != ERR::Okay or compile_calls != 1 or
       counters.SourceCompilations != 1 or counters.Misses != 1 or counters.Publications != 1) {
      Log.error("Cold imported-module compilation did not publish one complete generation");
      return false;
   }
   cleanup.Paths.push_back(cold.CachePath);

   // The compiler contributes observations to the final identity.  A parent retains that identity for the warm
   // lookup, exactly as nested module descriptors do for transitive imports.
   request.ExpectedIdentity = cold.CompilationIdentity;
   CompiledModule warm;
   if (load_or_compile_module(request, counting_compiler(compile_calls), {}, payload_validator(), counters, warm) !=
       ERR::Okay or not warm.CacheHit or compile_calls != 1 or counters.Hits != 1 or warm.Payload != cold.Payload) {
      Log.error("A fresh imported-module lookup did not consume the published generation");
      return false;
   }

   if (not write_file(source_path, "second")) return false;
   CompiledModule changed;
   if (load_or_compile_module(request, counting_compiler(compile_calls), {}, payload_validator(), counters, changed) !=
       ERR::Okay or changed.CacheHit or compile_calls != 2 or counters.Misses != 2 or
       changed.CachePath IS cold.CachePath) {
      Log.error("Changed imported-module source did not select a new cold generation");
      return false;
   }
   cleanup.Paths.push_back(changed.CachePath);
   return true;
}

bool malformed_fallback_and_publication_failure(kt::Log &Log)
{
   const std::string source_path = "temp:tiri-i03-malformed.tiri";
   if (not write_file(source_path, "fallback")) return false;
   Cleanup cleanup { { source_path } };
   auto request = request_for(source_path);
   LifecycleCounters counters;
   CompiledModule initial;
   int compile_calls = 0;
   if (load_or_compile_module(request, counting_compiler(compile_calls), {}, payload_validator(), counters, initial) !=
       ERR::Okay or initial.PublicationError != ERR::Okay) return false;
   cleanup.Paths.push_back(initial.CachePath);

   request.ExpectedIdentity = initial.CompilationIdentity;
   if (not write_file(initial.CachePath, "TIRIMOD1-truncated")) return false;
   CompiledModule recovered;
   if (load_or_compile_module(request, counting_compiler(compile_calls), {}, payload_validator(), counters,
       recovered) !=
       ERR::Okay or recovered.CacheHit or compile_calls != 2 or recovered.Payload.empty()) {
      Log.error("Malformed imported-module cache did not fall back to source");
      return false;
   }

   DeleteFile(recovered.CachePath, nullptr);
   set_module_publish_failure(ModulePublishFailure::WRITE);
   CompiledModule unpublished;
   if (load_or_compile_module(request, counting_compiler(compile_calls), {}, payload_validator(), counters,
       unpublished) != ERR::Okay or unpublished.PublicationError IS ERR::Okay or unpublished.Payload.empty() or
       (AnalysePath(unpublished.CachePath, nullptr) IS ERR::Okay)) {
      Log.error("A publication failure prevented or exposed an in-memory imported module");
      return false;
   }
   return true;
}

bool cycle_and_validation_rejection(kt::Log &Log)
{
   const std::string source_path = "temp:tiri-i03-cycle.tiri";
   if (not write_file(source_path, "cycle")) return false;
   Cleanup cleanup { { source_path } };
   auto request = request_for(source_path);
   std::vector<std::string> stack { source_path };
   request.ImportStack = &stack;
   LifecycleCounters counters;
   CompiledModule output;
   int compile_calls = 0;
   if (load_or_compile_module(request, counting_compiler(compile_calls), {}, payload_validator(), counters, output) !=
       ERR::Loop or compile_calls or stack.size() != 1) {
      Log.error("Recursive imported-module compilation was not rejected before compiling");
      return false;
   }

   stack.clear();
   if (load_or_compile_module(request, counting_compiler(compile_calls), {}, payload_validator(), counters, output) !=
       ERR::Okay) return false;
   cleanup.Paths.push_back(output.CachePath);
   request.ExpectedIdentity = output.CompilationIdentity;

   IdentityValidator reject = [](const Identity &, std::string &Reason) {
      Reason = "redirected resolver observation";
      return false;
   };
   CompiledModule rejected;
   if (load_or_compile_module(request, counting_compiler(compile_calls), reject, payload_validator(), counters,
       rejected) != ERR::Okay or rejected.CacheHit or compile_calls != 2) {
      Log.error("A rejected resolver/dependency observation did not force source compilation");
      return false;
   }
   return true;
}

bool concurrent_complete_generations(kt::Log &Log)
{
   const std::string source_path = "temp:tiri-i03-concurrent.tiri";
   if (not write_file(source_path, "concurrent")) return false;
   Cleanup cleanup { { source_path } };
   auto request = request_for(source_path);
   request.ExpectedIdentity.ConditionalInputs.push_back({ tiri::cache::ConditionalKind::IMPORTED,
      "imported", source_path, "true" });

   constexpr size_t producer_count = 4;
   std::array<LifecycleCounters, producer_count> counters;
   std::array<CompiledModule, producer_count> outputs;
   std::array<ERR, producer_count> errors;
   std::array<std::thread, producer_count> producers;
   std::atomic<size_t> ready = 0;
   std::atomic<bool> start = false;
   ModuleCompiler compile = [](std::string_view Source, Identity &IdentityValue, Interface &InterfaceValue,
      std::string &Payload, std::string &) {
      InterfaceValue = interface_for(IdentityValue);
      Payload.assign("\x1bLJ", 3);
      Payload.append(Source);
      Payload.append("-complete");
      return ERR::Okay;
   };

   for (size_t i = 0; i < producer_count; ++i) {
      producers[i] = std::thread([&, i] {
         ready.fetch_add(1, std::memory_order_release);
         while (not start.load(std::memory_order_acquire)) std::this_thread::yield();
         errors[i] = load_or_compile_module(request, compile, {}, payload_validator(), counters[i], outputs[i]);
      });
   }
   while (ready.load(std::memory_order_acquire) != producer_count) std::this_thread::yield();
   start.store(true, std::memory_order_release);
   for (auto &producer : producers) producer.join();

   bool published = false;
   for (size_t i = 0; i < producer_count; ++i) {
      if (errors[i] != ERR::Okay or outputs[i].Payload.empty()) {
         Log.error("A concurrent imported-module producer failed to retain its complete in-memory result");
         return false;
      }
      if (outputs[i].PublicationError IS ERR::Okay) published = true;
      cleanup.Paths.push_back(outputs[i].CachePath);
   }
   if (not published) {
      Log.error("Concurrent imported-module producers published no complete generation");
      return false;
   }

   int unexpected_compiles = 0;
   LifecycleCounters reader_counters;
   CompiledModule reader;
   if (load_or_compile_module(request, counting_compiler(unexpected_compiles), {}, payload_validator(),
       reader_counters, reader) != ERR::Okay or not reader.CacheHit or unexpected_compiles) {
      Log.error("A reader did not observe a complete generation after concurrent publication");
      return false;
   }
   return true;
}

} // namespace

void import_module_cache_unit_tests(int &Passed, int &Total)
{
   kt::Log log("ImportModuleCacheTests");
   for (auto test : { cold_hit_and_invalidation, malformed_fallback_and_publication_failure,
      cycle_and_validation_rejection, concurrent_complete_generations }) {
      Total++;
      if (test(log)) Passed++;
   }
}
#endif
