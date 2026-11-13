// Backend-independent protocol primitives.  See protocol_primitives.h.

#include "protocol_primitives.h"

#include <kotuku/config.h>

#include <array>

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

}

inline namespace CRYPTO_PROTOCOL_ABI {

uint32_t crc32(std::span<const uint8_t> Data) {
   uint32_t value = 0xffffffffu;
   for (uint8_t byte : Data) value = CRC_TABLE[(value ^ byte) & 0xffu] ^ (value >> 8);
   return value ^ 0xffffffffu;
}

// The accumulator is volatile so that the compiler cannot exit the loop early once a difference is found.

bool constant_time_equal(std::span<const uint8_t> A, std::span<const uint8_t> B) {
   if (A.size() != B.size()) return false;
   volatile uint8_t difference = 0;
   for (size_t i = 0; i < A.size(); ++i) difference = uint8_t(difference | (A[i] ^ B[i]));
   return difference IS 0;
}

} // inline namespace
} // namespace crypto_protocol
