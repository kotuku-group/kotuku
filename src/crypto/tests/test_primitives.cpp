#define PRV_CRYPTO_MODULE
#include <kotuku/modules/crypto.h>
#include "../crypto_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#ifdef CRYPTO_PROTOCOL_OPENSSL
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#endif
#endif

static std::vector<uint8_t> from_hex(std::string_view Hex) {
   auto digit = [](char Value) -> uint8_t {
      if (Value >= '0' and Value <= '9') return uint8_t(Value - '0');
      if (Value >= 'a' and Value <= 'f') return uint8_t(Value - 'a' + 10);
      return uint8_t(Value - 'A' + 10);
   };
   std::vector<uint8_t> result;
   for (size_t i = 0; i + 1 < Hex.size(); i += 2) result.push_back(uint8_t((digit(Hex[i]) << 4) | digit(Hex[i + 1])));
   return result;
}

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

   uint32_t checksum = 0;
   auto digits = std::span<const int8_t>((const int8_t *)"123456789", 9);
   if (crypto::CRC32(digits, &checksum) != ERR::Okay or checksum != 0xcbf43926u)
      return failure("IEEE CRC-32 vector differs");
   if (crypto::CRC32({}, &checksum) != ERR::Okay or checksum != 0)
      return failure("Empty CRC-32 vector differs");
   if (crypto::CRC32(digits, nullptr) != ERR::Args) return failure("Null CRC-32 output was accepted");

#ifdef CRYPTO_PROTOCOL_OPENSSL
   std::string certificate_pem, private_key_pem, fingerprint;
   if (crypto::GenerateP256Certificate("kotuku-test", &certificate_pem, &private_key_pem, &fingerprint) != ERR::Okay)
      return failure("P-256 certificate generation failed");
   auto cert_bio = BIO_new_mem_buf(certificate_pem.data(), int(certificate_pem.size()));
   auto key_bio = BIO_new_mem_buf(private_key_pem.data(), int(private_key_pem.size()));
   auto certificate = cert_bio ? PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr) : nullptr;
   auto private_key = key_bio ? PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr) : nullptr;
   bool p256_key = false;
   if (private_key and EVP_PKEY_base_id(private_key) IS EVP_PKEY_EC) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
      std::array<char, 80> group_name{};
      size_t group_length = 0;
      p256_key = EVP_PKEY_get_utf8_string_param(private_key, OSSL_PKEY_PARAM_GROUP_NAME,
         group_name.data(), group_name.size(), &group_length) IS 1 and
         std::string_view(group_name.data()) IS "prime256v1";
#else
      auto ec_key = EVP_PKEY_get0_EC_KEY(private_key);
      p256_key = ec_key and EC_GROUP_get_curve_name(EC_KEY_get0_group(ec_key)) IS NID_X9_62_prime256v1;
#endif
   }
   std::array<uint8_t, EVP_MAX_MD_SIZE> certificate_digest{};
   unsigned int certificate_digest_size = 0;
   std::string expected_fingerprint;
   if (certificate and X509_digest(certificate, EVP_sha256(), certificate_digest.data(),
       &certificate_digest_size) IS 1 and certificate_digest_size IS 32) {
      static constexpr char HEX[] = "0123456789ABCDEF";
      for (size_t i = 0; i < certificate_digest_size; ++i) {
         if (i) expected_fingerprint += ':';
         expected_fingerprint += HEX[certificate_digest[i] >> 4];
         expected_fingerprint += HEX[certificate_digest[i] & 15];
      }
   }
   bool valid_certificate = certificate and private_key and
      p256_key and
      X509_check_private_key(certificate, private_key) IS 1 and
      X509_verify(certificate, private_key) IS 1 and fingerprint IS expected_fingerprint;
   if (certificate) X509_free(certificate);
   if (private_key) EVP_PKEY_free(private_key);
   if (cert_bio) BIO_free(cert_bio);
   if (key_bio) BIO_free(key_bio);
   std::fill(private_key_pem.begin(), private_key_pem.end(), '\0');
   if (not valid_certificate) return failure("Generated certificate or key is invalid");
   if (crypto::GenerateP256Certificate("", &certificate_pem, &private_key_pem, &fingerprint) != ERR::Args or
       crypto::GenerateP256Certificate("test", &certificate_pem, &certificate_pem, &fingerprint) != ERR::Args or
       crypto::GenerateP256Certificate("test", nullptr, &private_key_pem, &fingerprint) != ERR::NullArgs) {
      return failure("Invalid certificate arguments were accepted");
   }
   certificate_pem = "alias-test";
   if (crypto::GenerateP256Certificate(certificate_pem, &certificate_pem, &private_key_pem,
       &fingerprint) != ERR::Okay or certificate_pem.empty()) {
      return failure("Common name sharing a string with an output failed");
   }
   std::fill(private_key_pem.begin(), private_key_pem.end(), '\0');
#endif

   std::array<int8_t, 32> failure_bytes{};
   auto injected = crypto_backend::checked_output(failure_bytes,
      [&]() { failure_bytes.fill(42); return false; },
      [](std::span<int8_t> Bytes) { for (auto &value : Bytes) value = 0; });
   if (injected != ERR::Failed) return failure("Injected backend failure was not reported");
   for (auto value : failure_bytes) if (value != 0) return failure("Failed backend output was not cleared");

   // NIST SP 800-38A F.5.1/F.5.5: AES-128 and AES-256 CTR with a 96-bit nonce and 32-bit counter.
   auto nonce_bytes = from_hex("f0f1f2f3f4f5f6f7f8f9fafb");
   std::array<uint8_t, 12> nonce{};
   std::copy(nonce_bytes.begin(), nonce_bytes.end(), nonce.begin());
   auto plain = from_hex("6bc1bee22e409f96e93d7e117393172a");
   auto encrypted = plain;
   auto key128 = from_hex("2b7e151628aed2a6abf7158809cf4f3c");
   if (crypto_backend::aes_ctr(key128, nonce, 0xfcfdfeff, encrypted) != ERR::Okay or
       encrypted != from_hex("874d6191b620e3261bef6864990db6ce")) {
      return failure("NIST AES-128-CTR vector differs");
   }
   if (crypto_backend::aes_ctr(key128, nonce, 0xfcfdfeff, encrypted) != ERR::Okay or encrypted != plain) {
      return failure("AES-128-CTR did not decrypt");
   }
   auto key256 = from_hex("603deb1015ca71be2b73aef0857d7781"
      "1f352c073b6108d72d9810a30914dff4");
   encrypted = plain;
   if (crypto_backend::aes_ctr(key256, nonce, 0xfcfdfeff, encrypted) != ERR::Okay or
       encrypted != from_hex("601ec313775789a5b7a7f504bbf3d228")) {
      return failure("NIST AES-256-CTR vector differs");
   }
   auto multi_plain = from_hex("6bc1bee22e409f96e93d7e117393172a"
      "ae2d8a571e03ac9c9eb76fac45af8e51"
      "30c81c46a35ce411e5fbc1191a0a52ef"
      "f69f2445df4f9b17ad2b417be66c3710");
   auto multi_cipher = multi_plain;
   if (crypto_backend::aes_ctr(key128, nonce, 0xfcfdfeff, multi_cipher) != ERR::Okay or
       multi_cipher != from_hex("874d6191b620e3261bef6864990db6ce"
          "9806f66b7970fdff8617187bb9fffdff"
          "5ae4df3edbd5d35e5b4f09020db03eab"
          "1e031dda2fbe03d1792170a0f3009cee")) {
      return failure("NIST AES-128-CTR multi-block vector differs");
   }
   auto partial = std::vector<uint8_t>(multi_plain.begin(), multi_plain.begin() + 23);
   if (crypto_backend::aes_ctr(key128, nonce, 0xfcfdfeff, partial) != ERR::Okay or
       not std::equal(partial.begin(), partial.end(), multi_cipher.begin())) {
      return failure("AES-CTR partial final block differs");
   }
   auto final_counter_block = plain;
   if (crypto_backend::aes_ctr(key128, nonce, UINT32_MAX, final_counter_block) != ERR::Okay) {
      return failure("AES-CTR rejected the final counter block");
   }
   auto overflowing_blocks = std::vector<uint8_t>(17, 0x5a);
   if (crypto_backend::aes_ctr(key128, nonce, UINT32_MAX, overflowing_blocks) != ERR::Args or
       not std::all_of(overflowing_blocks.begin(), overflowing_blocks.end(),
          [](uint8_t Value) { return Value IS 0x5a; })) {
      return failure("AES-CTR counter overflow was accepted or changed input");
   }

   // NIST GCM case 2: one zero block with a zero key and 96-bit nonce.
   std::array<uint8_t, 16> zero_key{};
   std::array<uint8_t, 12> zero_nonce{};
   std::array<uint8_t, 16> gcm_data{};
   std::array<uint8_t, 16> tag{};
   if (crypto_backend::aes_gcm_encrypt(zero_key, zero_nonce, {}, gcm_data, tag) != ERR::Okay or
       not std::equal(gcm_data.begin(), gcm_data.end(),
          from_hex("0388dace60b6a392f328c2b971b2fe78").begin()) or
       not std::equal(tag.begin(), tag.end(), from_hex("ab6e47d42cec13bdf53a67b21257bddf").begin())) {
      return failure("NIST AES-128-GCM vector differs");
   }
   auto bad_tag = tag;
   bad_tag[15] ^= 1;
   auto rejected = gcm_data;
   if (crypto_backend::aes_gcm_decrypt(zero_key, zero_nonce, {}, rejected, bad_tag) != ERR::Mismatch or
       not std::all_of(rejected.begin(), rejected.end(), [](uint8_t Value) { return Value IS 0; })) {
      return failure("GCM authentication failure did not clear plaintext");
   }
   if (crypto_backend::aes_gcm_decrypt(zero_key, zero_nonce, {}, gcm_data, tag) != ERR::Okay or
       not std::all_of(gcm_data.begin(), gcm_data.end(), [](uint8_t Value) { return Value IS 0; })) {
      return failure("AES-128-GCM did not decrypt");
   }
   // NIST GCM case 1: authentication of an empty plaintext.
   std::vector<uint8_t> empty;
   if (crypto_backend::aes_gcm_encrypt(zero_key, zero_nonce, {}, empty, tag) != ERR::Okay or
       not std::equal(tag.begin(), tag.end(), from_hex("58e2fccefa7e3061367f1d57a4e7455a").begin()) or
       crypto_backend::aes_gcm_decrypt(zero_key, zero_nonce, {}, empty, tag) != ERR::Okay) {
      return failure("NIST empty AES-GCM vector differs");
   }
   // NIST AES-256-GCM example reproduced by the OpenSSL AES-GCM demonstration, including associated data.
   auto gcm_key256 = from_hex("eebc1f57487f51921c0465665f8ae6d1658bb26de6f8a069a3520293a572078f");
   auto gcm_nonce_bytes = from_hex("99aa3e68ed8173a0eed06684");
   std::array<uint8_t, 12> gcm_nonce{};
   std::copy(gcm_nonce_bytes.begin(), gcm_nonce_bytes.end(), gcm_nonce.begin());
   auto gcm_aad = from_hex("4d23c3cec334b49bdb370c437fec78de");
   auto gcm_plain = from_hex("f56e87055bc32d0eeb31b2eacc2bf2a5");
   auto gcm_cipher = gcm_plain;
   std::array<uint8_t, 16> gcm_tag256{};
   if (crypto_backend::aes_gcm_encrypt(gcm_key256, gcm_nonce, gcm_aad, gcm_cipher, gcm_tag256) != ERR::Okay or
       gcm_cipher != from_hex("f7264413a84c0e7cd536867eb9f21736") or
       not std::equal(gcm_tag256.begin(), gcm_tag256.end(),
          from_hex("67ba0510262ae487d737ee6298f77e0c").begin())) {
      return failure("NIST AES-256-GCM vector differs");
   }
   if (crypto_backend::aes_gcm_decrypt(gcm_key256, gcm_nonce, gcm_aad, gcm_cipher, gcm_tag256) != ERR::Okay or
       gcm_cipher != gcm_plain) return failure("AES-256-GCM did not decrypt");
   if (crypto_backend::aes_ctr(std::span<const uint8_t>(zero_key.data(), 15), zero_nonce, 0, gcm_data) != ERR::Args) {
      return failure("Invalid AES key length accepted");
   }
   return 0;
}
