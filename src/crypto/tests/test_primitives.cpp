#define PRV_CRYPTO_MODULE
#include <kotuku/modules/crypto.h>
#include "../crypto_internal.h"
#include "../protocol_primitives.h"
#ifdef CRYPTO_PROTOCOL_CNG
#include "../protocol_cng.h"
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef CRYPTO_TEST_OPENSSL
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

// Published vectors for the protocol primitives used by WebRTC, run against the backend that this platform selects
// (CNG on Windows, OpenSSL elsewhere).  The long-term keys are checked end to end through the MESSAGE-INTEGRITY values
// of the RFC 5769 and RFC 8489 sample requests.

static int test_protocol_primitives() {
#ifdef CRYPTO_PROTOCOL_BACKEND
   auto bytes = [](std::string_view Value) {
      return std::span<const uint8_t>((const uint8_t *)Value.data(), Value.size());
   };
   auto equals = [](std::span<const uint8_t> Value, std::string_view Hex) {
      auto expected = from_hex(Hex);
      return std::equal(Value.begin(), Value.end(), expected.begin(), expected.end());
   };

   std::array<uint8_t, 20> sha1{};
   std::array<uint8_t, 32> sha256{};
   std::vector<uint8_t> key_0b(20, 0x0b), key_aa(131, 0xaa);
   auto large_key_data = bytes("Test Using Larger Than Block-Size Key - Hash Key First");

   if (not crypto_protocol::hmac_sha1(key_0b, bytes("Hi There"), sha1) or
       not equals(sha1, "b617318655057264e28bc0b6fb378c8ef146be00")) {
      return failure("RFC 2202 case 1 HMAC-SHA1 differs");
   }
   if (not crypto_protocol::hmac_sha1(bytes("Jefe"), bytes("what do ya want for nothing?"), sha1) or
       not equals(sha1, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79")) {
      return failure("RFC 2202 case 2 HMAC-SHA1 differs");
   }
   if (not crypto_protocol::hmac_sha1(std::span<const uint8_t>(key_aa.data(), 80), large_key_data, sha1) or
       not equals(sha1, "aa4ae5e15272d00e95705637ce8a3b55ed402112")) {
      return failure("RFC 2202 case 6 HMAC-SHA1 differs");
   }
   if (not crypto_protocol::hmac_sha1({}, {}, sha1) or
       not equals(sha1, "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d")) {
      return failure("Empty-key HMAC-SHA1 differs");
   }
   if (not crypto_protocol::hmac_sha256(key_0b, bytes("Hi There"), sha256) or
       not equals(sha256, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7")) {
      return failure("RFC 4231 case 1 HMAC-SHA256 differs");
   }
   if (not crypto_protocol::hmac_sha256(bytes("Jefe"), bytes("what do ya want for nothing?"), sha256) or
       not equals(sha256, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")) {
      return failure("RFC 4231 case 2 HMAC-SHA256 differs");
   }
   if (not crypto_protocol::hmac_sha256(key_aa, large_key_data, sha256) or
       not equals(sha256, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54")) {
      return failure("RFC 4231 case 6 HMAC-SHA256 differs");
   }
   if (not crypto_protocol::hmac_sha256({}, {}, sha256) or
       not equals(sha256, "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad")) {
      return failure("Empty-key HMAC-SHA256 differs");
   }
   if (not crypto_protocol::sha256(bytes("abc"), sha256) or
       not equals(sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) {
      return failure("FIPS SHA-256 vector differs (protocol)");
   }

   // RFC 8489 Appendix B.1: USERHASH is SHA-256("<username>:<realm>").

   static constexpr std::string_view MATRIX =
      "\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9";
   auto userhash_input = std::string(MATRIX) + ":example.org";
   if (not crypto_protocol::sha256(bytes(userhash_input), sha256) or
       not equals(sha256, "4a3cf38fef6992bda952c6780417da0f24819415569e60b205c46e41407f1704")) {
      return failure("RFC 8489 USERHASH differs");
   }

   // RFC 5769 section 2.4: MD5 long-term key; MESSAGE-INTEGRITY covers the 92 bytes before it.

   auto md5_request = from_hex(
      "000100602112a44278ad3433c6ad72c029da412e"
      "00060012e3839ee38388e383aae38383e382afe382b90000"
      "0015001c662f2f3439396b39353464364f4c33346f4c39465354767936347341"
      "0014000b6578616d706c652e6f726700");
   std::vector<uint8_t> key;
   if (not crypto_protocol::long_term_key(MATRIX, "example.org", "TheMatrIX", key, false) or key.size() != 16 or
       not crypto_protocol::hmac_sha1(key, md5_request, sha1) or
       not equals(sha1, "f67024656dd64a3e02b8e0712e85c9a28ca89666")) {
      return failure("RFC 5769 MD5 long-term key differs");
   }

   // RFC 8489 Appendix B.1 with erratum 6268: SHA-256 long-term key; MESSAGE-INTEGRITY-SHA256 covers 128 bytes.

   auto sha256_request = from_hex(
      "000100902112a44278ad3433c6ad72c029da412e"
      "001e00204a3cf38fef6992bda952c6780417da0f24819415569e60b205c46e41407f1704"
      "001500296f624d61744a6f733241414143662f2f3439396b39353464364f4c33346f4c39465354767936347341000000"
      "0014000b6578616d706c652e6f726700"
      "001d000400020000");
   if (not crypto_protocol::long_term_key(MATRIX, "example.org", "TheMatrIX", key, true) or key.size() != 32 or
       not crypto_protocol::hmac_sha256(key, sha256_request, sha256) or
       not equals(sha256, "b5c7bf005b6c52a21c51c5e892f81924136296cb927c43149309278cc6518e65")) {
      return failure("RFC 8489 SHA-256 long-term key differs");
   }
   if (not crypto_protocol::long_term_key("user", "example.org", "pass", key, true) or
       not equals(key, "2eab843d62723b5549ca4dde2641a8c07ba67a3c7df8919c8da81ee206a4b907")) {
      return failure("SHA-256 long-term key for user:example.org:pass differs");
   }

   std::array<uint8_t, 4> left = { 1, 2, 3, 4 }, right = { 1, 2, 3, 5 };
   if (not crypto_protocol::constant_time_equal(left, left) or
       crypto_protocol::constant_time_equal(left, right) or
       crypto_protocol::constant_time_equal(left, std::span<const uint8_t>(left.data(), 3)) or
       not crypto_protocol::constant_time_equal({}, {})) {
      return failure("Protocol constant_time_equal differs");
   }

   // Two 32-byte random draws colliding, or either being all zero, indicates a broken generator.

   std::array<uint8_t, 32> random_a{}, random_b{}, zero{};
   if (not crypto_protocol::random_bytes(random_a) or not crypto_protocol::random_bytes(random_b) or
       random_a IS random_b or random_a IS zero or random_b IS zero or
       not crypto_protocol::random_bytes({})) {
      return failure("Protocol random_bytes failed");
   }
   crypto_protocol::secure_wipe(random_a);
   if (random_a != zero) return failure("Protocol secure_wipe left data behind");
#endif

#ifdef CRYPTO_PROTOCOL_CNG
   crypto_protocol::CngCertificate certificate;
   if (not crypto_protocol::generate_p256_certificate_cng("kotuku-test", false, certificate) or
       not certificate.context or not certificate.key or certificate.der().empty()) {
      return failure("CNG P-256 certificate generation failed");
   }
   std::array<uint8_t, 32> der_digest{};
   if (not crypto_protocol::sha256(certificate.der(), der_digest) or der_digest != certificate.fingerprint) {
      return failure("CNG certificate fingerprint is not the SHA-256 of its DER encoding");
   }
   certificate.reset();
   if (certificate.context or certificate.key or not certificate.der().empty()) {
      return failure("CNG certificate reset left state behind");
   }
#endif
   return 0;
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

#ifdef CRYPTO_TEST_OPENSSL
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
   return test_protocol_primitives();
}
