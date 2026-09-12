#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tiri::cache {

constexpr uint32_t SCHEMA_VERSION = 1;
constexpr size_t DIGEST_SIZE = 32;
constexpr size_t MAX_METADATA_SIZE = 8 * 1024 * 1024;
constexpr size_t MAX_STRING_SIZE = 64 * 1024;
constexpr size_t MAX_OPTIONS = 256;
constexpr size_t MAX_IMPORTS = 4096;
constexpr size_t MAX_RESOLUTION_INPUTS = 4096;
constexpr size_t MAX_CONDITIONAL_INPUTS = 4096;
constexpr uint64_t MAX_PAYLOAD_SIZE = uint64_t(1024) * 1024 * 1024;

using Digest = std::array<uint8_t, DIGEST_SIZE>;

enum class BomPolicy : uint8_t { STRIP_RECOGNISED = 1 };

enum class ConditionalKind : uint8_t {
   IMPORTED = 1,
   DEBUG_MODE,
   LOG_LEVEL,
   PLATFORM,
   EXISTS,
   MODULE_AVAILABLE,
   OTHER
};

struct CompilationOption {
   std::string Name;
   std::string Value;
};

struct SourceIdentity {
   std::string ResolvedPath;
   uint64_t Size = 0;
   int64_t ModifiedHint = 0;
   Digest ContentDigest = {};
};

struct ImportIdentity {
   std::string ParentPath;
   std::string OriginalRequest;
   SourceIdentity Source;
};

struct ResolutionInput {
   std::string Name;
   std::string Context;
   std::string Value;
};

struct ConditionalInput {
   ConditionalKind Kind = ConditionalKind::OTHER;
   std::string Name;
   std::string Context;
   std::string Value;
};

struct Manifest {
   uint32_t Schema = SCHEMA_VERSION;
   BomPolicy InputBomPolicy = BomPolicy::STRIP_RECOGNISED;
   std::string BuildIdentity;
   SourceIdentity MainSource;
   std::vector<CompilationOption> Options;
   std::vector<ImportIdentity> Imports;
   std::vector<ResolutionInput> ResolutionInputs;
   std::vector<ConditionalInput> ConditionalInputs;
};

enum class FormatError : uint8_t {
   OKAY,
   NOT_CACHE,
   UNSUPPORTED_VERSION,
   TRUNCATED,
   SIZE_LIMIT,
   COUNT_LIMIT,
   STRING_LIMIT,
   INVALID_ENUM,
   INVALID_METADATA,
   INVALID_PAYLOAD
};

struct EnvelopeView {
   Manifest Metadata;
   std::string_view Payload;
};

[[nodiscard]] Digest content_digest(std::span<const uint8_t> Bytes) noexcept;
[[nodiscard]] Digest content_digest(std::string_view Bytes) noexcept;
[[nodiscard]] std::string digest_hex(const Digest &DigestValue);

// The key is a schema/build namespace plus a SHA-256 digest of the exact resolved main path, BOM policy and
// canonical effective options.  Path spelling and case are deliberately preserved.
[[nodiscard]] std::string file_key(const Manifest &Metadata);

// A key match is never trusted as identity proof.  Lookup must compare these exact fields after decoding.
[[nodiscard]] bool lookup_identity_matches(const Manifest &Stored, const Manifest &Expected);

[[nodiscard]] FormatError encode_envelope(const Manifest &Metadata, std::string_view Payload, std::string &Output);
[[nodiscard]] FormatError decode_envelope(std::string_view Input, EnvelopeView &Output);
[[nodiscard]] const char *format_error_name(FormatError Error) noexcept;

} // namespace tiri::cache
