#include <kotuku/main.h>

#include "../cache_manifest.h"

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

} // namespace

void cache_manifest_unit_tests(int &Passed, int &Total)
{
   kt::Log log("CacheManifestTests");
   for (auto test : { round_trip_contract, malformed_and_bounds_contract }) {
      Total++;
      if (test(log)) Passed++;
   }
}
#endif
