#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace crypto_native {

enum class Algorithm : uint8_t {
   Invalid,
   MD5,
   SHA1,
   SHA256,
   SHA512
};

enum class Result : uint8_t {
   Okay,
   Failed,
   Mismatch,
   Args,
   NoPermission,
   OutOfSpace
};

struct HashContext;

size_t digest_size(Algorithm Algorithm);
HashContext * create_hash_context(Algorithm Algorithm, std::span<const int8_t> Key, bool UseHMAC);
void destroy_hash_context(HashContext *Context);
Result update_hash(HashContext *Context, std::span<const int8_t> Input);
Result digest_hash(HashContext *Context, std::span<int8_t> Output);
Result reset_hash(HashContext *Context);
Result hash(Algorithm Algorithm, std::span<const int8_t> Input, std::span<int8_t> Output);
Result hmac(Algorithm Algorithm, std::span<const int8_t> Key, std::span<const int8_t> Input,
   std::span<int8_t> Output);
Result random(std::span<int8_t> Output);
Result verify_rs256(std::span<const uint8_t> Modulus, std::span<const uint8_t> Exponent,
   std::span<const int8_t> Message, std::span<const int8_t> Signature);
Result write_protected_file(std::string_view Path, std::span<const int8_t> Data);

} // namespace crypto_native
