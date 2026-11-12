// Small protocol primitives shared by Crypto and WebRTC without object-system dependencies.
//
// The header is backend-neutral.  A target selects one implementation with CRYPTO_PROTOCOL_OPENSSL
// (protocol_openssl.cpp) or CRYPTO_PROTOCOL_CNG (protocol_cng.cpp); with neither, only crc32() and
// constant_time_equal() are available.  Types specific to a backend are declared in protocol_openssl.h and
// protocol_cng.h.
//
// Each backend lives in its own inline namespace so that a CNG Crypto module and an OpenSSL WebRTC module can be
// linked into the same static executable without their definitions colliding.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#if defined(CRYPTO_PROTOCOL_OPENSSL) and defined(CRYPTO_PROTOCOL_CNG)
#error "Define only one of CRYPTO_PROTOCOL_OPENSSL and CRYPTO_PROTOCOL_CNG."
#endif

#if defined(CRYPTO_PROTOCOL_CNG)
#define CRYPTO_PROTOCOL_BACKEND 1
#define CRYPTO_PROTOCOL_ABI cng
#elif defined(CRYPTO_PROTOCOL_OPENSSL)
#define CRYPTO_PROTOCOL_BACKEND 1
#define CRYPTO_PROTOCOL_ABI openssl
#else
#define CRYPTO_PROTOCOL_ABI common
#endif

namespace crypto_protocol {
inline namespace CRYPTO_PROTOCOL_ABI {

uint32_t crc32(std::span<const uint8_t> Data);

// Compares two byte sequences in time that depends only on their length.  Sequences of different length are unequal.

bool constant_time_equal(std::span<const uint8_t> A, std::span<const uint8_t> B);

#ifdef CRYPTO_PROTOCOL_BACKEND
bool hmac_sha1(std::span<const uint8_t> Key, std::span<const uint8_t> Data, std::span<uint8_t, 20> Result);
bool hmac_sha256(std::span<const uint8_t> Key, std::span<const uint8_t> Data, std::span<uint8_t, 32> Result);
bool sha256(std::span<const uint8_t> Data, std::span<uint8_t, 32> Result);

// RFC 8489 long-term credential key: MD5 or SHA-256 of `Username:Realm:Password`.  Result is cleared on failure.

bool long_term_key(std::string_view Username, std::string_view Realm, std::string_view Password,
   std::vector<uint8_t> &Result, bool Sha256);

// Fills Output from the system's cryptographically secure generator.  Returns false if the generator fails, in which
// case the content of Output is unspecified.

bool random_bytes(std::span<uint8_t> Output);

// Erases memory in a way that the compiler cannot elide.

void secure_wipe(void *Data, size_t Size);

inline void secure_wipe(std::span<uint8_t> Data) { secure_wipe(Data.data(), Data.size()); }
#endif

} // inline namespace
} // namespace crypto_protocol
