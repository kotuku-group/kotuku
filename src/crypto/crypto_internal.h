#pragma once

#include <kotuku/main.h>
#include <span>
#include <cstdint>

namespace crypto_backend {
template<class Operation, class Wipe>
ERR checked_output(std::span<int8_t> Output, Operation Run, Wipe Clear) {
   if (Run()) return ERR::Okay;
   Clear(Output);
   return ERR::Failed;
}

ERR random(std::span<int8_t> Output);
ERR sha256(std::span<const int8_t> Input, std::span<int8_t> Output);
ERR verify_rs256(std::span<const uint8_t> Modulus, std::span<const uint8_t> Exponent,
   std::span<const int8_t> Message, std::span<const int8_t> Signature);
}
