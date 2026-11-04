#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tiri::bytecode_storage {

constexpr uint64_t MAX_DECODED_SIZE = uint64_t(1024) * 1024 * 1024;
constexpr uint64_t MAX_ENCODED_SIZE = MAX_DECODED_SIZE + (2 * 1024 * 1024);
constexpr uint64_t MAX_PERSISTED_SIZE = MAX_ENCODED_SIZE + (32 * 1024 * 1024);
constexpr size_t MAX_IDENTITY_TOKEN = 128;

enum class Error : uint8_t {
   OKAY,
   INVALID_WRAPPER,
   INVALID_GZIP,
   ENCODED_LIMIT,
   DECODED_LIMIT,
   COMPRESSION,
   DECOMPRESSION,
   TRAILING_DATA,
   INVALID_BYTECODE
};

[[nodiscard]] Error compress(std::string_view Input, std::string &Output);
[[nodiscard]] Error decompress(std::string_view Input, std::string &Output, uint64_t ExpectedSize = UINT64_MAX, uint64_t DecodedLimit = MAX_DECODED_SIZE);
[[nodiscard]] Error parse_wrapper(std::string_view Input, std::string_view &EncodedPayload, std::string_view *IdentityToken = nullptr);
[[nodiscard]] Error decode_wrapper(std::string_view Input, std::string &Output, std::string_view *IdentityToken = nullptr);
[[nodiscard]] const char *error_name(Error ErrorValue) noexcept;

} // namespace tiri::bytecode_storage
