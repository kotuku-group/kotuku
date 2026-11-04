#include "protocol_primitives.h"

#include <kotuku/config.h>

#include <array>
#include <limits>
#include <string>

#if defined(CRYPTO_PROTOCOL_OPENSSL)
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

namespace crypto_protocol {
namespace {

constexpr std::array<uint32_t, 256> make_crc_table() {
   std::array<uint32_t, 256> result{};
   for (uint32_t i = 0; i < result.size(); ++i) {
      uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
      result[i] = value;
   }
   return result;
}

constexpr auto CRC_TABLE = make_crc_table();

#if defined(CRYPTO_PROTOCOL_OPENSSL)
bool hmac_digest(const EVP_MD *Algorithm, std::span<const uint8_t> Key,
   std::span<const uint8_t> Data, std::span<uint8_t> Result) {
   if (Key.size() > size_t(std::numeric_limits<int>::max())) return false;
   auto key = Key.empty() ? (const uint8_t *)"" : Key.data();
   unsigned int written = 0;
   return HMAC(Algorithm, key, int(Key.size()), Data.data(), Data.size(), Result.data(), &written) and
      written IS Result.size();
}
#endif

}

uint32_t crc32(std::span<const uint8_t> Data) {
   uint32_t value = 0xffffffffu;
   for (uint8_t byte : Data) value = CRC_TABLE[(value ^ byte) & 0xffu] ^ (value >> 8);
   return value ^ 0xffffffffu;
}

#if defined(CRYPTO_PROTOCOL_OPENSSL)
bool hmac_sha1(std::span<const uint8_t> Key, std::span<const uint8_t> Data, std::span<uint8_t, 20> Result) {
   return hmac_digest(EVP_sha1(), Key, Data, Result);
}

bool hmac_sha256(std::span<const uint8_t> Key, std::span<const uint8_t> Data, std::span<uint8_t, 32> Result) {
   return hmac_digest(EVP_sha256(), Key, Data, Result);
}

bool long_term_key(std::string_view Username, std::string_view Realm, std::string_view Password,
   std::vector<uint8_t> &Result, bool Sha256) {
   auto text = std::string(Username) + ":" + std::string(Realm) + ":" + std::string(Password);
   Result.resize(Sha256 ? 32 : 16);
   unsigned int written = 0;
   bool success = EVP_Digest(text.data(), text.size(), Result.data(), &written,
      Sha256 ? EVP_sha256() : EVP_md5(), nullptr) IS 1 and written IS Result.size();
   OPENSSL_cleanse(text.data(), text.size());
   if (not success) {
      OPENSSL_cleanse(Result.data(), Result.size());
      Result.clear();
      return false;
   }
   return true;
}

bool constant_time_equal(std::span<const uint8_t> A, std::span<const uint8_t> B) {
   return A.size() IS B.size() and CRYPTO_memcmp(A.data(), B.data(), A.size()) IS 0;
}

bool generate_p256_certificate(std::string_view CommonName, EVP_PKEY *&Key, X509 *&Certificate) {
   Key = nullptr;
   Certificate = nullptr;
   auto key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
   if (not key_ctx) return false;
   bool key_ok = EVP_PKEY_keygen_init(key_ctx) IS 1 and
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_ctx, NID_X9_62_prime256v1) IS 1 and
      EVP_PKEY_keygen(key_ctx, &Key) IS 1;
   EVP_PKEY_CTX_free(key_ctx);
   if (not key_ok) return false;

   Certificate = X509_new();
   if (not Certificate) return false;
   std::array<uint8_t, 8> serial_bytes;
   if (RAND_bytes(serial_bytes.data(), int(serial_bytes.size())) != 1) return false;
   serial_bytes[0] &= 0x7f;
   auto serial = BN_bin2bn(serial_bytes.data(), int(serial_bytes.size()), nullptr);
   if (not serial) return false;
   bool serial_ok = BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(Certificate)) != nullptr;
   BN_free(serial);
   if (not serial_ok) return false;

   std::string common_name(CommonName);
   auto name = X509_get_subject_name(Certificate);
   return X509_set_version(Certificate, 2) IS 1 and
      X509_gmtime_adj(X509_getm_notBefore(Certificate), -86400) and
      X509_gmtime_adj(X509_getm_notAfter(Certificate), 30L * 86400) and
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char *)common_name.c_str(),
         -1, -1, 0) IS 1 and
      X509_set_issuer_name(Certificate, name) IS 1 and
      X509_set_pubkey(Certificate, Key) IS 1 and
      X509_sign(Certificate, Key, EVP_sha256()) > 0;
}
#endif

}
