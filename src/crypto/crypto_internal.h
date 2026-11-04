#pragma once

#include <kotuku/main.h>
#include <kotuku/modules/crypto.h>
#include <span>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

namespace crypto_backend {
template<class Operation, class Wipe>
ERR checked_output(std::span<int8_t> Output, Operation Run, Wipe Clear) {
   if (Run()) return ERR::Okay;
   Clear(Output);
   return ERR::Failed;
}

ERR random(std::span<int8_t> Output);
#ifdef _WIN32
ERR generate_p256_certificate(std::string_view CommonName, std::string &CertificatePEM,
   std::string &PrivateKeyPEM, std::string &Fingerprint);
#endif
size_t digest_size(HASH Algorithm);
ERR hash(HASH Algorithm, std::span<const int8_t> Input, std::span<int8_t> Output);
ERR hmac(HASH Algorithm, std::span<const int8_t> Key, std::span<const int8_t> Input, std::span<int8_t> Output);

// In-place AES operations for protocol layers.  CTR uses a 96-bit nonce followed by a big-endian 32-bit counter.
// The caller must never reuse a key/nonce/counter stream or a GCM key/nonce pair.
inline bool valid_ctr_range(uint32_t Counter, size_t Size) {
   auto blocks = Size / 16 + size_t(Size % 16 != 0);
   return blocks <= uint64_t(std::numeric_limits<uint32_t>::max()) - Counter + 1;
}

ERR aes_ctr(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   uint32_t Counter, std::span<uint8_t> Data);
ERR aes_gcm_encrypt(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   std::span<const uint8_t> AAD, std::span<uint8_t> Data, std::span<uint8_t, 16> Tag);
ERR aes_gcm_decrypt(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   std::span<const uint8_t> AAD, std::span<uint8_t> Data, std::span<const uint8_t, 16> Tag);

class HashContext {
public:
   virtual ~HashContext() = default;
   virtual ERR update(std::span<const int8_t> Input) = 0;
   virtual ERR digest(std::span<int8_t> Output) = 0;
   virtual ERR reset() = 0;
};

std::unique_ptr<HashContext> make_hash_context(HASH Algorithm, std::span<const int8_t> Key, bool UseHMAC);
ERR verify_rs256(std::span<const uint8_t> Modulus, std::span<const uint8_t> Exponent,
   std::span<const int8_t> Message, std::span<const int8_t> Signature);
ERR write_protected_file(std::string_view Path, std::span<const int8_t> Data);
}
