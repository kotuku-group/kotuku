#include "../stun/stun.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
   auto datagram = std::span<const uint8_t>(Data, Size);
   rtc::stun::Message parsed;
   if (rtc::stun::parse(datagram, parsed)) {
      (void)rtc::stun::unknown_required_attributes(parsed);
      (void)rtc::stun::verify_fingerprint(datagram, parsed);
      (void)rtc::stun::verify_integrity(datagram, parsed, {});
      (void)rtc::stun::verify_integrity(datagram, parsed, {}, true);
      rtc::stun::LongTermChallenge challenge;
      (void)rtc::stun::decode_long_term_challenge(parsed, challenge);
      std::array<uint8_t, 16> mapped{};
      uint16_t port = 0;
      bool ipv6 = false;
      for (const auto &attribute : parsed.attributes) {
         if (attribute.type IS rtc::stun::ATTR_XOR_MAPPED_ADDRESS) {
            (void)rtc::stun::decode_xor_address(attribute, parsed, mapped, port, ipv6);
         } else if (attribute.type IS rtc::stun::ATTR_MAPPED_ADDRESS or
             attribute.type IS rtc::stun::ATTR_ALTERNATE_SERVER) {
            (void)rtc::stun::decode_mapped_address(attribute, mapped, port, ipv6);
         } else if (attribute.type IS rtc::stun::ATTR_ERROR_CODE) {
            uint16_t error = 0;
            (void)rtc::stun::decode_error_code(attribute, error);
         }
      }
   }
   return 0;
}
