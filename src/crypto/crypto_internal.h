#pragma once

#include <kotuku/main.h>
#include <kotuku/modules/crypto.h>
#include <span>
#include <cstdint>
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
size_t digest_size(HASH Algorithm);
ERR hash(HASH Algorithm, std::span<const int8_t> Input, std::span<int8_t> Output);
ERR hmac(HASH Algorithm, std::span<const int8_t> Key, std::span<const int8_t> Input, std::span<int8_t> Output);

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
