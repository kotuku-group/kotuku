#define PRV_CRYPTO_MODULE
#include <kotuku/modules/crypto.h>
#include "../crypto_internal.h"

#include <array>
#include <cstdio>
#include <span>
#include <string_view>

static bool matches_hex(std::span<const int8_t> Bytes, std::string_view Hex) {
   if (Hex.size() != Bytes.size() * 2) return false;
   for (size_t i=0; i < Bytes.size(); i++) {
      auto digit = [](char Value) { return (Value >= 'a') ? Value - 'a' + 10 : Value - '0'; };
      if (uint8_t(Bytes[i]) != uint8_t((digit(Hex[i * 2]) << 4) | digit(Hex[(i * 2) + 1]))) return false;
   }
   return true;
}

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

   auto abc = std::span<const int8_t>((const int8_t *)"abc", 3);
   std::array<int8_t, 64> hash_output{};
   if ((crypto::Hash(HASH::MD5, abc, std::span<int8_t>(hash_output.data(), 16)) != ERR::Okay) or
       (not matches_hex(std::span<const int8_t>(hash_output.data(), 16),
          "900150983cd24fb0d6963f7d28e17f72"))) return failure("RFC 1321 MD5 vector differs");
   if ((crypto::Hash(HASH::SHA1, abc, std::span<int8_t>(hash_output.data(), 20)) != ERR::Okay) or
       (not matches_hex(std::span<const int8_t>(hash_output.data(), 20),
          "a9993e364706816aba3e25717850c26c9cd0d89d"))) return failure("FIPS SHA-1 vector differs");
   if ((crypto::Hash(HASH::SHA256, abc, std::span<int8_t>(hash_output.data(), 32)) != ERR::Okay) or
       (not matches_hex(std::span<const int8_t>(hash_output.data(), 32),
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))) {
      return failure("FIPS SHA-256 vector differs");
   }
   if ((crypto::Hash(HASH::SHA512, abc, hash_output) != ERR::Okay) or
       (not matches_hex(hash_output, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"))) {
      return failure("FIPS SHA-512 vector differs");
   }

   std::array<int8_t, 20> hmac_key{};
   hmac_key.fill(0x0b);
   auto hi_there = std::span<const int8_t>((const int8_t *)"Hi There", 8);
   if ((crypto::HMAC(HASH::MD5, std::span<const int8_t>(hmac_key.data(), 16), hi_there,
          std::span<int8_t>(hash_output.data(), 16)) != ERR::Okay) or
       (not matches_hex(std::span<const int8_t>(hash_output.data(), 16),
          "9294727a3638bb1c13f48ef8158bfc9d"))) return failure("RFC 2202 HMAC-MD5 vector differs");
   if ((crypto::HMAC(HASH::SHA1, hmac_key, hi_there, std::span<int8_t>(hash_output.data(), 20)) != ERR::Okay) or
       (not matches_hex(std::span<const int8_t>(hash_output.data(), 20),
          "b617318655057264e28bc0b6fb378c8ef146be00"))) return failure("RFC 2202 HMAC-SHA1 vector differs");
   if ((crypto::HMAC(HASH::SHA256, hmac_key, hi_there, std::span<int8_t>(hash_output.data(), 32)) != ERR::Okay) or
       (not matches_hex(std::span<const int8_t>(hash_output.data(), 32),
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"))) {
      return failure("RFC 4231 HMAC-SHA256 vector differs");
   }
   if ((crypto::HMAC(HASH::SHA512, hmac_key, hi_there, hash_output) != ERR::Okay) or
       (not matches_hex(hash_output, "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
          "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"))) {
      return failure("RFC 4231 HMAC-SHA512 vector differs");
   }
   if (crypto::Hash(HASH::SHA512, abc, std::span<int8_t>(hash_output.data(), 63)) != ERR::Args)
      return failure("Incorrect digest capacity was accepted");
   if (crypto::Hash(HASH::NIL, abc, std::span<int8_t>(hash_output.data(), 16)) != ERR::NoSupport)
      return failure("Unknown digest algorithm was accepted");

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
