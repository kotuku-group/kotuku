// STUN wire format and authentication helpers (RFC 8489).  No object-system dependencies.
#pragma once

#include <kotuku/config.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rtc::stun {

constexpr uint32_t MAGIC_COOKIE = 0x2112a442;
constexpr uint16_t BINDING_REQUEST = 0x0001;
constexpr uint16_t BINDING_SUCCESS = 0x0101;
constexpr uint16_t ATTR_USERNAME = 0x0006;
constexpr uint16_t ATTR_MESSAGE_INTEGRITY = 0x0008;
constexpr uint16_t ATTR_XOR_MAPPED_ADDRESS = 0x0020;
constexpr uint16_t ATTR_MESSAGE_INTEGRITY_SHA256 = 0x001c;
constexpr uint16_t ATTR_FINGERPRINT = 0x8028;

struct Attribute {
   uint16_t type = 0;
   std::span<const uint8_t> value;
   size_t offset = 0; // Offset of the attribute header from the start of the datagram.
};

// Attribute spans refer to the caller's datagram.  Keep it alive while using a parsed Message.
struct Message {
   uint16_t type = 0;
   std::array<uint8_t, 12> transaction_id{};
   std::vector<Attribute> attributes;

   const Attribute * find(uint16_t Type) const;
};

bool parse(std::span<const uint8_t> Datagram, Message &Result);
bool verify_integrity(std::span<const uint8_t> Datagram, const Message &Parsed,
   std::span<const uint8_t> Key, bool Sha256 = false);
bool verify_fingerprint(std::span<const uint8_t> Datagram, const Message &Parsed);

// IPv4 addresses occupy the first four bytes of Address; IPv6 uses all sixteen.  Bytes are in network order.
bool decode_xor_address(const Attribute &AttributeValue, const Message &Parsed,
   std::array<uint8_t, 16> &Address, uint16_t &Port, bool &IPv6);

// Password must already be normalised for the long-term credential mechanism.  RFC 5389 uses MD5, while RFC 8489
// permits SHA-256; the caller selects the algorithm negotiated with the server.
bool long_term_key(std::string_view Username, std::string_view Realm, std::string_view Password,
   std::vector<uint8_t> &Key, bool Sha256 = false);

// Build a message with optional MESSAGE-INTEGRITY and FINGERPRINT attributes.  Attribute values are copied.
// Authentication attributes must not appear in Attributes; they are appended in protocol order by this function.
bool encode(uint16_t Type, std::span<const uint8_t, 12> TransactionId, std::span<const Attribute> Attributes,
   std::span<const uint8_t> Key, bool Sha256, bool Fingerprint, std::vector<uint8_t> &Result);

}
