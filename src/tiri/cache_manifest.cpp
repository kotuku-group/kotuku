#include <kotuku/main.h>

#include "cache_manifest.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <ranges>
#include <tuple>

namespace tiri::cache {
namespace {

constexpr std::array<uint8_t, 8> CACHE_MAGIC = { 'T', 'I', 'R', 'I', 'C', 'A', 'C', 'H' };
constexpr size_t HEADER_SIZE = CACHE_MAGIC.size() + 4 + 4 + 8 + DIGEST_SIZE;

constexpr std::array<uint32_t, 64> SHA256_CONSTANTS = {
   0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
   0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
   0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
   0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
   0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
   0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
   0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
   0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

class Encoder {
   size_t Limit;

   bool available(size_t Size) {
      if (Error != FormatError::OKAY) return false;
      if ((Bytes.size() > Limit) or (Size > Limit - Bytes.size())) {
         Error = FormatError::SIZE_LIMIT;
         return false;
      }
      return true;
   }

public:
   explicit Encoder(size_t SizeLimit = std::numeric_limits<size_t>::max()) : Limit(SizeLimit) { }

   std::string Bytes;
   FormatError Error = FormatError::OKAY;

   void byte(uint8_t Value) {
      if (available(1)) Bytes.push_back(char(Value));
   }

   void u32(uint32_t Value) {
      if (not available(4)) return;
      for (int shift = 0; shift < 32; shift += 8) Bytes.push_back(char(uint8_t(Value >> shift)));
   }

   void u64(uint64_t Value) {
      if (not available(8)) return;
      for (int shift = 0; shift < 64; shift += 8) Bytes.push_back(char(uint8_t(Value >> shift)));
   }

   void i64(int64_t Value) { u64(std::bit_cast<uint64_t>(Value)); }

   void string(std::string_view Value) {
      if (Error != FormatError::OKAY) return;
      if (Value.size() > MAX_STRING_SIZE) {
         Error = FormatError::STRING_LIMIT;
         return;
      }
      if (not available(4 + Value.size())) return;
      u32(uint32_t(Value.size()));
      Bytes.append(Value);
   }

   void digest(const Digest &Value) {
      if (available(Value.size())) Bytes.append((const char *)Value.data(), Value.size());
   }
};

class Decoder {
public:
   explicit Decoder(std::string_view Input) : Bytes(Input) { }

   std::string_view Bytes;
   size_t Position = 0;
   FormatError Error = FormatError::OKAY;

   bool available(size_t Size) {
      if ((Size > Bytes.size()) or (Position > Bytes.size() - Size)) {
         Error = FormatError::TRUNCATED;
         return false;
      }
      return true;
   }

   uint8_t byte() { return available(1) ? uint8_t(Bytes[Position++]) : 0; }

   uint32_t u32() {
      if (not available(4)) return 0;
      uint32_t value = 0;
      for (int shift = 0; shift < 32; shift += 8) value |= uint32_t(byte()) << shift;
      return value;
   }

   uint64_t u64() {
      if (not available(8)) return 0;
      uint64_t value = 0;
      for (int shift = 0; shift < 64; shift += 8) value |= uint64_t(byte()) << shift;
      return value;
   }

   int64_t i64() { return std::bit_cast<int64_t>(u64()); }

   std::string string() {
      auto size = u32();
      if (Error != FormatError::OKAY) return {};
      if (size > MAX_STRING_SIZE) {
         Error = FormatError::STRING_LIMIT;
         return {};
      }
      if (not available(size)) return {};
      std::string value(Bytes.substr(Position, size));
      Position += size;
      return value;
   }

   Digest digest() {
      Digest value = {};
      if (not available(value.size())) return value;
      std::memcpy(value.data(), Bytes.data() + Position, value.size());
      Position += value.size();
      return value;
   }
};

uint32_t rotate_right(uint32_t Value, unsigned Count) noexcept
{
   return (Value >> Count) | (Value << (32 - Count));
}

void sha256_block(std::array<uint32_t, 8> &State, const uint8_t *Block) noexcept
{
   uint32_t words[64];
   for (int i = 0; i < 16; ++i) {
      const auto *input = Block + (i * 4);
      words[i] = (uint32_t(input[0]) << 24) | (uint32_t(input[1]) << 16) |
         (uint32_t(input[2]) << 8) | uint32_t(input[3]);
   }
   for (int i = 16; i < 64; ++i) {
      auto s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
      auto s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
   }

   auto a = State[0], b = State[1], c = State[2], d = State[3];
   auto e = State[4], f = State[5], g = State[6], h = State[7];
   for (int i = 0; i < 64; ++i) {
      auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
      auto choice = (e & f) ^ ((~e) & g);
      auto temporary1 = h + sum1 + choice + SHA256_CONSTANTS[i] + words[i];
      auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
      auto majority = (a & b) ^ (a & c) ^ (b & c);
      auto temporary2 = sum0 + majority;
      h = g; g = f; f = e; e = d + temporary1;
      d = c; c = b; b = a; a = temporary1 + temporary2;
   }

   State[0] += a; State[1] += b; State[2] += c; State[3] += d;
   State[4] += e; State[5] += f; State[6] += g; State[7] += h;
}

bool option_less(const CompilationOption *Left, const CompilationOption *Right)
{
   return std::tie(Left->Name, Left->Value) < std::tie(Right->Name, Right->Value);
}

std::vector<const CompilationOption *> sorted_options(const std::vector<CompilationOption> &Options)
{
   std::vector<const CompilationOption *> result;
   result.reserve(Options.size());
   for (const auto &option : Options) result.push_back(&option);
   std::ranges::sort(result, option_less);
   return result;
}

bool same_options(const std::vector<CompilationOption> &Left, const std::vector<CompilationOption> &Right)
{
   if (Left.size() != Right.size()) return false;
   auto left = sorted_options(Left);
   auto right = sorted_options(Right);
   for (size_t i = 0; i < left.size(); ++i) {
      if ((left[i]->Name != right[i]->Name) or (left[i]->Value != right[i]->Value)) return false;
   }
   return true;
}

void encode_source(Encoder &Output, const SourceIdentity &Source)
{
   Output.string(Source.ResolvedPath);
   Output.u64(Source.Size);
   Output.i64(Source.ModifiedHint);
   Output.digest(Source.ContentDigest);
}

SourceIdentity decode_source(Decoder &Input)
{
   SourceIdentity result;
   result.ResolvedPath = Input.string();
   result.Size = Input.u64();
   result.ModifiedHint = Input.i64();
   result.ContentDigest = Input.digest();
   return result;
}

FormatError encode_metadata(const Manifest &Metadata, std::string &Output)
{
   if (Metadata.Schema != SCHEMA_VERSION) return FormatError::UNSUPPORTED_VERSION;
   if ((Metadata.Options.size() > MAX_OPTIONS) or (Metadata.Imports.size() > MAX_IMPORTS) or
       (Metadata.ResolutionInputs.size() > MAX_RESOLUTION_INPUTS) or
       (Metadata.ConditionalInputs.size() > MAX_CONDITIONAL_INPUTS)) return FormatError::COUNT_LIMIT;
   if (Metadata.InputBomPolicy != BomPolicy::STRIP_RECOGNISED) return FormatError::INVALID_ENUM;

   Encoder encoder(MAX_METADATA_SIZE);
   encoder.byte(uint8_t(Metadata.InputBomPolicy));
   encoder.byte(0); encoder.byte(0); encoder.byte(0);
   encoder.string(Metadata.BuildIdentity);
   encode_source(encoder, Metadata.MainSource);

   auto options = sorted_options(Metadata.Options);
   encoder.u32(uint32_t(options.size()));
   for (const auto *option : options) {
      encoder.string(option->Name);
      encoder.string(option->Value);
   }

   encoder.u32(uint32_t(Metadata.Imports.size()));
   for (const auto &dependency : Metadata.Imports) {
      encoder.string(dependency.ParentPath);
      encoder.string(dependency.OriginalRequest);
      encode_source(encoder, dependency.Source);
   }

   encoder.u32(uint32_t(Metadata.ResolutionInputs.size()));
   for (const auto &input : Metadata.ResolutionInputs) {
      encoder.string(input.Name);
      encoder.string(input.Context);
      encoder.string(input.Value);
   }

   encoder.u32(uint32_t(Metadata.ConditionalInputs.size()));
   for (const auto &input : Metadata.ConditionalInputs) {
      if ((input.Kind < ConditionalKind::IMPORTED) or (input.Kind > ConditionalKind::OTHER)) {
         return FormatError::INVALID_ENUM;
      }
      encoder.byte(uint8_t(input.Kind));
      encoder.byte(0); encoder.byte(0); encoder.byte(0);
      encoder.string(input.Name);
      encoder.string(input.Context);
      encoder.string(input.Value);
   }

   if (encoder.Error != FormatError::OKAY) return encoder.Error;
   Output = std::move(encoder.Bytes);
   return FormatError::OKAY;
}

template <class Record, class Reader>
void decode_records(Decoder &Input, uint32_t Count, size_t Limit, std::vector<Record> &Output, Reader Read)
{
   if (Input.Error != FormatError::OKAY) return;
   if (Count > Limit) {
      Input.Error = FormatError::COUNT_LIMIT;
      return;
   }
   Output.reserve(Count);
   for (uint32_t i = 0; (i < Count) and (Input.Error IS FormatError::OKAY); ++i) Output.push_back(Read(Input));
}

} // namespace

Digest content_digest(std::span<const uint8_t> Bytes) noexcept
{
   std::array<uint32_t, 8> state = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
   };
   size_t position = 0;
   while (Bytes.size() - position >= 64) {
      sha256_block(state, Bytes.data() + position);
      position += 64;
   }

   std::array<uint8_t, 128> tail = {};
   const auto remaining = Bytes.size() - position;
   if (remaining) std::memcpy(tail.data(), Bytes.data() + position, remaining);
   tail[remaining] = 0x80;
   const size_t tail_size = remaining < 56 ? 64 : 128;
   const uint64_t bit_size = uint64_t(Bytes.size()) * 8;
   for (int i = 0; i < 8; ++i) tail[tail_size - 1 - i] = uint8_t(bit_size >> (i * 8));
   sha256_block(state, tail.data());
   if (tail_size IS 128) sha256_block(state, tail.data() + 64);

   Digest result;
   for (size_t i = 0; i < state.size(); ++i) {
      result[i * 4] = uint8_t(state[i] >> 24);
      result[i * 4 + 1] = uint8_t(state[i] >> 16);
      result[i * 4 + 2] = uint8_t(state[i] >> 8);
      result[i * 4 + 3] = uint8_t(state[i]);
   }
   return result;
}

Digest content_digest(std::string_view Bytes) noexcept
{
   return content_digest(std::span((const uint8_t *)Bytes.data(), Bytes.size()));
}

std::string digest_hex(const Digest &DigestValue)
{
   constexpr char HEX[] = "0123456789abcdef";
   std::string result;
   result.reserve(DigestValue.size() * 2);
   for (auto value : DigestValue) {
      result.push_back(HEX[value >> 4]);
      result.push_back(HEX[value & 0x0f]);
   }
   return result;
}

std::string file_key(const Manifest &Metadata)
{
   if ((Metadata.Schema != SCHEMA_VERSION) or
       (Metadata.InputBomPolicy != BomPolicy::STRIP_RECOGNISED) or
       (Metadata.BuildIdentity.size() > MAX_STRING_SIZE) or
       (Metadata.MainSource.ResolvedPath.size() > MAX_STRING_SIZE) or
       (Metadata.Options.size() > MAX_OPTIONS)) return {};
   for (const auto &option : Metadata.Options) {
      if ((option.Name.size() > MAX_STRING_SIZE) or (option.Value.size() > MAX_STRING_SIZE)) return {};
   }

   Encoder identity;
   identity.u32(SCHEMA_VERSION);
   identity.string(Metadata.BuildIdentity);
   identity.string(Metadata.MainSource.ResolvedPath);
   identity.byte(uint8_t(Metadata.InputBomPolicy));
   auto options = sorted_options(Metadata.Options);
   identity.u32(uint32_t(options.size()));
   for (const auto *option : options) {
      identity.string(option->Name);
      identity.string(option->Value);
   }
   auto build = digest_hex(content_digest(Metadata.BuildIdentity));
   auto key = digest_hex(content_digest(identity.Bytes));
   return "v1-" + build.substr(0, 16) + "-" + key;
}

bool lookup_identity_matches(const Manifest &Stored, const Manifest &Expected)
{
   return (Stored.Schema IS Expected.Schema) and (Stored.Schema IS SCHEMA_VERSION) and
      (Stored.InputBomPolicy IS Expected.InputBomPolicy) and (Stored.BuildIdentity IS Expected.BuildIdentity) and
      (Stored.MainSource.ResolvedPath IS Expected.MainSource.ResolvedPath) and
      same_options(Stored.Options, Expected.Options);
}

FormatError encode_envelope(const Manifest &Metadata, std::string_view Payload, std::string &Output)
{
   Output.clear();
   if (Payload.size() > MAX_PAYLOAD_SIZE) return FormatError::SIZE_LIMIT;
   if (not Payload.starts_with("\x1bLJ")) return FormatError::INVALID_PAYLOAD;

   std::string metadata;
   if (auto error = encode_metadata(Metadata, metadata); error != FormatError::OKAY) return error;

   Encoder header;
   header.Bytes.append((const char *)CACHE_MAGIC.data(), CACHE_MAGIC.size());
   header.u32(SCHEMA_VERSION);
   header.u32(uint32_t(metadata.size()));
   header.u64(Payload.size());
   header.digest(content_digest(metadata));
   Output.reserve(header.Bytes.size() + metadata.size() + Payload.size());
   Output = std::move(header.Bytes);
   Output += metadata;
   Output += Payload;
   return FormatError::OKAY;
}

FormatError decode_envelope(std::string_view Input, EnvelopeView &Output)
{
   Output = {};
   if ((Input.size() < CACHE_MAGIC.size()) or
       (std::memcmp(Input.data(), CACHE_MAGIC.data(), CACHE_MAGIC.size()) != 0)) return FormatError::NOT_CACHE;
   if (Input.size() < HEADER_SIZE) return FormatError::TRUNCATED;

   Decoder header(Input.substr(CACHE_MAGIC.size(), HEADER_SIZE - CACHE_MAGIC.size()));
   auto schema = header.u32();
   auto metadata_size = header.u32();
   auto payload_size = header.u64();
   auto expected_digest = header.digest();
   if (schema != SCHEMA_VERSION) return FormatError::UNSUPPORTED_VERSION;
   if ((metadata_size > MAX_METADATA_SIZE) or (payload_size > MAX_PAYLOAD_SIZE)) return FormatError::SIZE_LIMIT;
   if ((uint64_t(HEADER_SIZE) + metadata_size + payload_size) != Input.size()) return FormatError::TRUNCATED;

   auto metadata_bytes = Input.substr(HEADER_SIZE, metadata_size);
   if (content_digest(metadata_bytes) != expected_digest) return FormatError::INVALID_METADATA;
   EnvelopeView decoded;
   Decoder metadata(metadata_bytes);
   auto bom_policy = metadata.byte();
   if (bom_policy != uint8_t(BomPolicy::STRIP_RECOGNISED)) return FormatError::INVALID_ENUM;
   decoded.Metadata.Schema = schema;
   decoded.Metadata.InputBomPolicy = BomPolicy(bom_policy);
   if (metadata.byte() or metadata.byte() or metadata.byte()) return FormatError::INVALID_METADATA;
   decoded.Metadata.BuildIdentity = metadata.string();
   decoded.Metadata.MainSource = decode_source(metadata);

   decode_records<CompilationOption>(metadata, metadata.u32(), MAX_OPTIONS, decoded.Metadata.Options,
      [](Decoder &Input) { return CompilationOption { Input.string(), Input.string() }; });
   decode_records<ImportIdentity>(metadata, metadata.u32(), MAX_IMPORTS, decoded.Metadata.Imports,
      [](Decoder &Input) { return ImportIdentity { Input.string(), Input.string(), decode_source(Input) }; });
   decode_records<ResolutionInput>(metadata, metadata.u32(), MAX_RESOLUTION_INPUTS,
      decoded.Metadata.ResolutionInputs, [](Decoder &Input) {
         return ResolutionInput { Input.string(), Input.string(), Input.string() };
      });
   decode_records<ConditionalInput>(metadata, metadata.u32(), MAX_CONDITIONAL_INPUTS,
      decoded.Metadata.ConditionalInputs, [](Decoder &Input) {
         auto kind = Input.byte();
         if ((kind < uint8_t(ConditionalKind::IMPORTED)) or (kind > uint8_t(ConditionalKind::OTHER))) {
            Input.Error = FormatError::INVALID_ENUM;
         }
         if (Input.byte() or Input.byte() or Input.byte()) Input.Error = FormatError::INVALID_METADATA;
         return ConditionalInput { ConditionalKind(kind), Input.string(), Input.string(), Input.string() };
      });

   if (metadata.Error != FormatError::OKAY) return metadata.Error;
   if (metadata.Position != metadata.Bytes.size()) return FormatError::INVALID_METADATA;
   decoded.Payload = Input.substr(HEADER_SIZE + metadata_size, size_t(payload_size));
   if (not decoded.Payload.starts_with("\x1bLJ")) return FormatError::INVALID_PAYLOAD;
   Output = std::move(decoded);
   return FormatError::OKAY;
}

const char *format_error_name(FormatError Error) noexcept
{
   switch (Error) {
      case FormatError::OKAY: return "okay";
      case FormatError::NOT_CACHE: return "not-cache";
      case FormatError::UNSUPPORTED_VERSION: return "unsupported-version";
      case FormatError::TRUNCATED: return "truncated";
      case FormatError::SIZE_LIMIT: return "size-limit";
      case FormatError::COUNT_LIMIT: return "count-limit";
      case FormatError::STRING_LIMIT: return "string-limit";
      case FormatError::INVALID_ENUM: return "invalid-enum";
      case FormatError::INVALID_METADATA: return "invalid-metadata";
      case FormatError::INVALID_PAYLOAD: return "invalid-payload";
   }
   return "unknown";
}

} // namespace tiri::cache
