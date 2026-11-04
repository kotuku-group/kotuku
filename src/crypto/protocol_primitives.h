// Small protocol primitives shared by Crypto and WebRTC without object-system dependencies.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#if defined(CRYPTO_PROTOCOL_OPENSSL)
#include <openssl/evp.h>
#include <openssl/x509.h>
#endif

namespace crypto_protocol {

uint32_t crc32(std::span<const uint8_t> Data);

#if defined(CRYPTO_PROTOCOL_OPENSSL)
bool hmac_sha1(std::span<const uint8_t> Key, std::span<const uint8_t> Data, std::span<uint8_t, 20> Result);
bool hmac_sha256(std::span<const uint8_t> Key, std::span<const uint8_t> Data, std::span<uint8_t, 32> Result);
bool long_term_key(std::string_view Username, std::string_view Realm, std::string_view Password,
   std::vector<uint8_t> &Result, bool Sha256);
bool constant_time_equal(std::span<const uint8_t> A, std::span<const uint8_t> B);
bool generate_p256_certificate(std::string_view CommonName, EVP_PKEY *&Key, X509 *&Certificate);
#endif

}
