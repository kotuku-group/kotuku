#define PRV_CRYPTO_MODULE
#include <kotuku/modules/crypto.h>
#include "../crypto_internal.h"

#include <array>
#include <cstdio>
#include <span>

static int failure(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

int main() {
   std::array<int8_t, 256> source{};
   for (size_t i = 0; i < source.size(); i++) source[i] = int8_t(i);
   std::array<int8_t, 344> encoded{};
   std::array<int8_t, 256> decoded{};
   int written = -1;
   if (crypto::Base64Encode(source, CC::STANDARD, encoded, &written) != ERR::Okay or written != 344)
      return failure("256-byte standard encoding failed");
   if (crypto::Base64Decode(encoded, CC::STANDARD, decoded, &written) != ERR::Okay or written != 256)
      return failure("256-byte standard decoding failed");
   for (size_t i = 0; i < source.size(); i++) if (source[i] != decoded[i]) return failure("Binary round trip failed");

   written = -1;
   std::array<int8_t, 1> short_buffer{};
   if (crypto::Base64Encode(source, CC::STANDARD, short_buffer, &written) != ERR::LowCapacity or written != 0)
      return failure("Insufficient capacity must report zero bytes");
   if (crypto::Base64Decode(encoded, CC::STANDARD, short_buffer, &written) != ERR::LowCapacity or written != 0)
      return failure("Insufficient decode capacity must report zero bytes");
   if (crypto::Base64Decode(std::span<const int8_t>((const int8_t *)"Zh==", 4), CC::STANDARD,
       decoded, &written) != ERR::InvalidData or written != 0) return failure("Non-canonical bits were accepted");

   std::array<int8_t, 32> digest{};
   constexpr std::array<uint8_t, 32> empty_digest = { 0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
      0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
      0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55 };
   if (crypto::SHA256({}, digest) != ERR::Okay) return failure("Empty SHA-256 failed");
   for (size_t i = 0; i < digest.size(); i++) if (uint8_t(digest[i]) != empty_digest[i])
      return failure("Empty SHA-256 digest differs from published vector");
   if (crypto::SHA256(std::span<const int8_t>(digest.data(), 1), digest) != ERR::Args)
      return failure("Overlapping digest spans were accepted");

   int equal = -1;
   if (crypto::ConstantTimeEqual(source, source, &equal) != ERR::Okay or equal != 1)
      return failure("Equal bytes should compare equal");
   decoded[255] ^= 1;
   if (crypto::ConstantTimeEqual(source, decoded, &equal) != ERR::Okay or equal != 0)
      return failure("Final-byte mismatch was missed");
   if (crypto::RandomBytes({}) != ERR::Okay) return failure("Empty random request failed");

   std::array<int8_t, 32> failure_bytes{};
   auto injected = crypto_backend::checked_output(failure_bytes,
      [&]() { failure_bytes.fill(42); return false; },
      [](std::span<int8_t> Bytes) { for (auto &value : Bytes) value = 0; });
   if (injected != ERR::Failed) return failure("Injected backend failure was not reported");
   for (auto value : failure_bytes) if (value != 0) return failure("Failed backend output was not cleared");
   return 0;
}
