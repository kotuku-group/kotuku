#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>

#include "../cache_manifest.h"
#include "../defs.h"
#include "../lua.hpp"

#include <cstring>
#include <ranges>

#ifdef UNIT_TESTS
namespace {

using namespace tiri::cache;

Manifest sample_manifest()
{
   Manifest result;
   result.BuildIdentity = "build:0123456789abcdef";
   result.MainSource = { "scripts:Case/../Case/main.tiri", 14, 123456, content_digest("print('main')") };
   result.Options = { { "jit", "on" }, { "log-level", "warning" } };
   result.Imports = {
      { result.MainSource.ResolvedPath, "./library", { "scripts:Case/library.tiri", 12, 123450,
         content_digest("return 'one'") } },
      { "scripts:Case/library.tiri", "nested.value", { "scripts:Case/nested/value.tiri", 0, 123451,
         content_digest("") } }
   };
   result.ResolutionInputs = { { "volume:scripts", result.MainSource.ResolvedPath, "/opt/kotuku/scripts/" } };
   result.ConditionalInputs = {
      { ConditionalKind::EXISTS, "optional.tiri", result.MainSource.ResolvedPath, "false" },
      { ConditionalKind::PLATFORM, "platform", "", "Linux" }
   };
   return result;
}

bool round_trip_contract(kt::Log &Log)
{
   if (digest_hex(content_digest("")) != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" or
       digest_hex(content_digest("abc")) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
      Log.error("SHA-256 known-answer test failed");
      return false;
   }

   auto manifest = sample_manifest();
   std::string encoded;
   const std::string payload("\x1bLJ\x8bnot-executed", 16);
   if (auto error = encode_envelope(manifest, payload, encoded); error != FormatError::OKAY) {
      Log.error("Envelope encode failed: %s", format_error_name(error));
      return false;
   }

   EnvelopeView decoded;
   if (auto error = decode_envelope(encoded, decoded); error != FormatError::OKAY) {
      Log.error("Envelope decode failed: %s", format_error_name(error));
      return false;
   }
   if ((decoded.Payload != payload) or not lookup_identity_matches(decoded.Metadata, manifest) or
       (decoded.Metadata.MainSource.ContentDigest != manifest.MainSource.ContentDigest) or
       (decoded.Metadata.Imports.size() != 2) or
       (decoded.Metadata.Imports[0].OriginalRequest != "./library") or
       (decoded.Metadata.Imports[1].ParentPath != "scripts:Case/library.tiri") or
       (decoded.Metadata.ResolutionInputs.size() != 1) or (decoded.Metadata.ConditionalInputs.size() != 2)) {
      Log.error("Decoded envelope did not preserve its bounded metadata");
      return false;
   }

   auto reordered = manifest;
   std::ranges::reverse(reordered.Options);
   if ((file_key(reordered) != file_key(manifest)) or not lookup_identity_matches(decoded.Metadata, reordered)) {
      Log.error("Effective option order changed cache identity");
      return false;
   }

   auto different_path = manifest;
   different_path.MainSource.ResolvedPath = "scripts:case/../case/main.tiri";
   auto different_option = manifest;
   different_option.Options[0].Value = "off";
   if ((file_key(different_path) IS file_key(manifest)) or (file_key(different_option) IS file_key(manifest)) or
       lookup_identity_matches(decoded.Metadata, different_path) or
       lookup_identity_matches(decoded.Metadata, different_option)) {
      Log.error("Path case or effective option mismatch was merged");
      return false;
   }

   // Lookup rechecks exact metadata even if a caller presents an entry under a colliding or incorrect filename.
   auto collision = manifest;
   collision.MainSource.ResolvedPath += ".collision";
   if (lookup_identity_matches(decoded.Metadata, collision)) {
      Log.error("Filename collision bypassed exact identity validation");
      return false;
   }
   return true;
}

bool malformed_and_bounds_contract(kt::Log &Log)
{
   auto manifest = sample_manifest();
   std::string encoded;
   if (encode_envelope(manifest, std::string("\x1bLJ\x8b", 4), encoded) != FormatError::OKAY) return false;

   EnvelopeView decoded;
   auto expect = [&](std::string_view Input, FormatError Expected) {
      auto result = decode_envelope(Input, decoded);
      if (result != Expected) {
         Log.error("Malformed envelope returned %s instead of %s", format_error_name(result),
            format_error_name(Expected));
         return false;
      }
      return true;
   };

   if (not expect(encoded.substr(0, 7), FormatError::NOT_CACHE) or
       not expect(encoded.substr(0, 20), FormatError::TRUNCATED)) return false;

   auto unsupported = encoded;
   unsupported[8] = 2;
   if (not expect(unsupported, FormatError::UNSUPPORTED_VERSION)) return false;

   auto oversized = encoded;
   uint32_t metadata_size = uint32_t(MAX_METADATA_SIZE + 1);
   for (int i = 0; i < 4; ++i) oversized[12 + i] = char(metadata_size >> (i * 8));
   if (not expect(oversized, FormatError::SIZE_LIMIT)) return false;

   auto damaged = encoded;
   damaged[52] ^= 0x01;
   if (not expect(damaged, FormatError::INVALID_METADATA)) return false;

   auto set_u32 = [](std::string &Bytes, size_t Offset, uint32_t Value) {
      for (int i = 0; i < 4; ++i) Bytes[Offset + i] = char(Value >> (i * 8));
   };
   auto refresh_metadata_digest = [](std::string &Bytes) {
      uint32_t size = 0;
      for (int i = 0; i < 4; ++i) size |= uint32_t(uint8_t(Bytes[12 + i])) << (i * 8);
      auto digest = content_digest(std::string_view(Bytes).substr(56, size));
      std::memcpy(Bytes.data() + 24, digest.data(), digest.size());
   };

   auto long_decoded_string = encoded;
   set_u32(long_decoded_string, 60, uint32_t(MAX_STRING_SIZE + 1));
   refresh_metadata_digest(long_decoded_string);
   if (not expect(long_decoded_string, FormatError::STRING_LIMIT)) return false;

   // Skip the BOM field, build identity and complete main-source record to locate the option count.
   const size_t option_count_offset = 56 + 4 + 4 + manifest.BuildIdentity.size() + 4 +
      manifest.MainSource.ResolvedPath.size() + 8 + 8 + DIGEST_SIZE;
   auto excessive_decoded_count = encoded;
   set_u32(excessive_decoded_count, option_count_offset, uint32_t(MAX_OPTIONS + 1));
   refresh_metadata_digest(excessive_decoded_count);
   if (not expect(excessive_decoded_count, FormatError::COUNT_LIMIT)) return false;

   auto trailing = encoded + "x";
   if (not expect(trailing, FormatError::TRUNCATED)) return false;

   auto bad_payload = encoded;
   bad_payload[bad_payload.size() - 4] = 'x';
   if (not expect(bad_payload, FormatError::INVALID_PAYLOAD)) return false;

   auto long_string = manifest;
   long_string.BuildIdentity.assign(MAX_STRING_SIZE + 1, 'x');
   if (encode_envelope(long_string, std::string("\x1bLJ", 3), encoded) != FormatError::STRING_LIMIT) return false;

   auto too_many = manifest;
   too_many.Options.resize(MAX_OPTIONS + 1);
   if (encode_envelope(too_many, std::string("\x1bLJ", 3), encoded) != FormatError::COUNT_LIMIT) return false;

   auto excessive_metadata = manifest;
   excessive_metadata.ResolutionInputs.clear();
   excessive_metadata.ResolutionInputs.resize((MAX_METADATA_SIZE / MAX_STRING_SIZE) + 1);
   for (auto &input : excessive_metadata.ResolutionInputs) input.Value.assign(MAX_STRING_SIZE, 'x');
   excessive_metadata.ConditionalInputs.push_back({ ConditionalKind::OTHER,
      std::string(MAX_STRING_SIZE + 1, 'x'), "", "" });
   if (encode_envelope(excessive_metadata, std::string("\x1bLJ", 3), encoded) != FormatError::SIZE_LIMIT or
       not encoded.empty()) {
      Log.error("Aggregate metadata limit was not enforced during encoding");
      return false;
   }

   if (encode_envelope(manifest, "source", encoded) != FormatError::INVALID_PAYLOAD) return false;
   return true;
}

bool write_source(std::string_view Path, std::string_view Source)
{
   DeleteFile(Path, nullptr);
   objFile::create file = { fl::Path(Path), fl::Flags(FL::NEW|FL::WRITE) };
   if (not file.ok()) return false;
   if (Source.empty()) return file->flush() IS ERR::Okay;

   int written = 0;
   auto bytes = std::span((const int8_t *)Source.data(), Source.size());
   return (file->write(bytes, &written) IS ERR::Okay) and (written IS int(Source.size())) and
      (file->flush() IS ERR::Okay);
}

bool compilation_capture_contract(kt::Log &Log)
{
   const std::string main_path = "temp:tiri-c02-main.tiri";
   const std::string child_path = "temp:tiri-c02-child.tiri";
   const std::string nested_path = "temp:tiri-c02-nested.tiri";
   const std::string empty_path = "temp:tiri-c02-empty.tiri";
   const std::array paths = { main_path, child_path, nested_path, empty_path };
   struct Cleanup {
      const std::array<std::string, 4> &Paths;
      ~Cleanup() { for (const auto &path : Paths) DeleteFile(path, nullptr); }
   } cleanup { paths };

   const std::string main_source =
      "include 'core'\n"
      "import './tiri-c02-child'\n"
      "import './tiri-c02-empty'\n"
      "import './tiri-c02-child'\n"
      "@if(exists='./tiri-c02-missing.tiri')\nlocal should_skip = true\n@end\n"
      "@if(debug=false)\nlocal debug_observed = false\n@end\n"
      "@if(platform='definitely-not-a-platform')\nlocal platform_skip = true\n@end\n";
   const std::string child_source =
      "import './tiri-c02-nested'\n"
      "@if(imported=true)\nlocal child_loaded = true\n@end\n";
   const std::string nested_source = "@if(imported=true)\nlocal nested_loaded = true\n@end\n";
   const std::string main_file_source = std::string("\xef\xbb\xbf", 3) + main_source;
   const std::string nested_file_source = std::string("\xef\xbb\xbf", 3) + nested_source;

   if (not write_source(main_path, main_file_source) or not write_source(child_path, child_source) or
       not write_source(nested_path, nested_file_source) or not write_source(empty_path, "")) {
      Log.error("Failed to create compilation-capture fixtures");
      return false;
   }

   objTiri::create holder = { fl::Path(main_path) };
   if (not holder.ok()) return false;
   auto script = (extTiri *)*holder;
   script->JitOptions |= JOF::DISABLE_JIT;
   if (acQuery(script) != ERR::Okay or not script->CompilationManifest) {
      Log.error("A successful file compilation did not retain its manifest");
      return false;
   }

   const Manifest snapshot = *script->CompilationManifest;
   if (snapshot.MainSource.ContentDigest != content_digest(main_source) or snapshot.Imports.size() != 4 or
       snapshot.Imports[0].OriginalRequest != "./tiri-c02-child" or
       snapshot.Imports[1].OriginalRequest != "./tiri-c02-nested" or
       snapshot.Imports[1].Source.ContentDigest != content_digest(nested_source) or
       snapshot.Imports[2].OriginalRequest != "./tiri-c02-empty" or
       snapshot.Imports[2].Source.Size or snapshot.Imports[3].OriginalRequest != "./tiri-c02-child" or
       snapshot.Imports[0].Source.ContentDigest != snapshot.Imports[3].Source.ContentDigest) {
      Log.error("Direct, nested, empty or duplicate import observations were incomplete");
      return false;
   }

   auto negative_exists = std::ranges::find_if(snapshot.ConditionalInputs, [](const auto &Input) {
      return (Input.Kind IS ConditionalKind::EXISTS) and
         (Input.Name IS "./tiri-c02-missing.tiri") and (Input.Value IS "false");
   });
   auto module = std::ranges::find_if(snapshot.ConditionalInputs, [](const auto &Input) {
      return (Input.Kind IS ConditionalKind::MODULE_AVAILABLE) and (Input.Name IS "core") and
         (Input.Value IS "true");
   });
   auto has_kind = [&](ConditionalKind Kind) {
      return std::ranges::find_if(snapshot.ConditionalInputs, [&](const auto &Input) {
         return Input.Kind IS Kind;
      }) != snapshot.ConditionalInputs.end();
   };
   if (negative_exists IS snapshot.ConditionalInputs.end() or module IS snapshot.ConditionalInputs.end() or
       not has_kind(ConditionalKind::IMPORTED) or not has_kind(ConditionalKind::DEBUG_MODE) or
       not has_kind(ConditionalKind::LOG_LEVEL) or not has_kind(ConditionalKind::PLATFORM) or
       not snapshot.Options.empty() or
       snapshot.ResolutionInputs.size() != 5) {
      Log.error("Conditional, module, resolution or JIT-independent Script-option capture was incomplete");
      return false;
   }

   if (lua_load(script->Lua, "@if(exists='temp:tiri-c02-main.tiri')\n@end", "=runtime-capture")) return false;
   lua_pop(script->Lua, 1);
   if (script->CompilationManifest->ConditionalInputs.size() != snapshot.ConditionalInputs.size() or
       script->CompilationManifest->Imports.size() != snapshot.Imports.size()) {
      Log.error("A later runtime compilation contaminated the retained manifest");
      return false;
   }

   objFile::create bytecode_sink = {
      fl::Size(1024 * 1024), fl::Flags(FL::BUFFER|FL::READ|FL::WRITE)
   };
   if (not bytecode_sink.ok() or acSaveToObject(script, *bytecode_sink) != ERR::Okay or
       script->CompilationManifest->ConditionalInputs.size() != snapshot.ConditionalInputs.size() or
       script->CompilationManifest->Imports.size() != snapshot.Imports.size()) {
      Log.error("Isolated SaveToObject compilation altered the execution manifest");
      return false;
   }

   if (not write_source(main_path, "local =")) return false;
   objTiri::create failed_file = { fl::Path(main_path) };
   if (not failed_file.ok()) return false;
   auto failed_file_script = (extTiri *)*failed_file;
   if (acQuery(failed_file_script) IS ERR::Okay or failed_file_script->CompilationManifest or
       script->CompilationManifest->Imports.size() != snapshot.Imports.size()) {
      Log.error("A failed replacement compilation retained or contaminated a manifest");
      return false;
   }

   objTiri::create invalid = { fl::Statement("local =") };
   if (not invalid.ok()) return false;
   auto invalid_script = (extTiri *)*invalid;
   if (acQuery(invalid_script) IS ERR::Okay or invalid_script->CompilationManifest) {
      Log.error("A failed or synthetic compilation retained a cache manifest");
      return false;
   }
   return true;
}

} // namespace

void cache_manifest_unit_tests(int &Passed, int &Total)
{
   kt::Log log("CacheManifestTests");
   for (auto test : { round_trip_contract, malformed_and_bounds_contract, compilation_capture_contract }) {
      Total++;
      if (test(log)) Passed++;
   }
}
#endif
