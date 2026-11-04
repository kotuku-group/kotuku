// STUN wire format and authentication helpers (RFC 8489).  No object-system dependencies.

#pragma once

#include <kotuku/config.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtc::stun {

constexpr uint32_t MAGIC_COOKIE = 0x2112a442;

// Message types

constexpr uint16_t BINDING_REQUEST = 0x0001;
constexpr uint16_t BINDING_SUCCESS = 0x0101;
constexpr uint16_t BINDING_ERROR = 0x0111;

// Attribute types

constexpr uint16_t ATTR_MAPPED_ADDRESS = 0x0001;
constexpr uint16_t ATTR_USERNAME = 0x0006;
constexpr uint16_t ATTR_ERROR_CODE = 0x0009;
constexpr uint16_t ATTR_UNKNOWN_ATTRIBUTES = 0x000a;
constexpr uint16_t ATTR_REALM = 0x0014;
constexpr uint16_t ATTR_NONCE = 0x0015;
constexpr uint16_t ATTR_MESSAGE_INTEGRITY = 0x0008;
constexpr uint16_t ATTR_XOR_MAPPED_ADDRESS = 0x0020;
constexpr uint16_t ATTR_MESSAGE_INTEGRITY_SHA256 = 0x001c;
constexpr uint16_t ATTR_PASSWORD_ALGORITHM = 0x001d;
constexpr uint16_t ATTR_USERHASH = 0x001e;
constexpr uint16_t ATTR_PASSWORD_ALGORITHMS = 0x8002;
constexpr uint16_t ATTR_ALTERNATE_SERVER = 0x8023;
constexpr uint16_t ATTR_FINGERPRINT = 0x8028;

// Password algorithms

constexpr uint16_t PASSWORD_MD5 = 0x0001;
constexpr uint16_t PASSWORD_SHA256 = 0x0002;

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

// Returns unknown comprehension-required attribute types once each, in wire order.
std::vector<uint16_t> unknown_required_attributes(const Message &Parsed);

// Build an authenticated 420 response listing the unknown required types in a request.
bool encode_unknown_attributes_error(const Message &Request, std::span<const uint16_t> Unknown,
   std::span<const uint8_t> Key, std::vector<uint8_t> &Result);

// SHA-256 truncation is accepted only when the STUN usage passes an explicit minimum of 16, 20, 24 or 28 bytes.
bool verify_integrity(std::span<const uint8_t> Datagram, const Message &Parsed,
   std::span<const uint8_t> Key, bool Sha256 = false, size_t MinSha256Length = 32);

bool verify_fingerprint(std::span<const uint8_t> Datagram, const Message &Parsed);

// IPv4 addresses occupy the first four bytes of Address; IPv6 uses all sixteen.  Bytes are in network order.
bool decode_xor_address(const Attribute &AttributeValue, const Message &Parsed,
   std::array<uint8_t, 16> &Address, uint16_t &Port, bool &IPv6);

// Encodes an XOR-MAPPED-ADDRESS value, the format shared by XOR-PEER-ADDRESS and XOR-RELAYED-ADDRESS.  The port and
// address are masked with the magic cookie, and IPv6 addresses also with the transaction ID.  The value occupies the
// first 8 bytes of the result for IPv4 and all 20 bytes for IPv6.
std::array<uint8_t, 20> encode_xor_address(const std::array<uint8_t, 16> &Address, uint16_t Port, bool IPv6,
   std::span<const uint8_t, 12> TransactionId);

// Returns true if the attribute appears at most once with the expected value size, and exactly once if Required.
bool valid_attribute(const Message &Parsed, uint16_t Type, size_t Size, bool Required);

// Decode the unobfuscated MAPPED-ADDRESS and ALTERNATE-SERVER attribute formats.
bool decode_mapped_address(const Attribute &AttributeValue,
   std::array<uint8_t, 16> &Address, uint16_t &Port, bool &IPv6);

// Read the numeric error code; the reason phrase is diagnostic and not returned.
bool decode_error_code(const Attribute &AttributeValue, uint16_t &Code);

// Username, realm and password are prepared with RFC 8265 OpaqueString before derivation.  RFC 5389 uses MD5,
// while RFC 8489 permits SHA-256; the caller selects the algorithm negotiated with the server.
bool long_term_key(std::string_view Username, std::string_view Realm, std::string_view Password,
   std::vector<uint8_t> &Key, bool Sha256 = false);

// A 401/438 challenge is unauthenticated unless the caller verifies its integrity with a previous key.  The caller
// must also match the response transaction ID and source address before accepting it.
struct LongTermChallenge {
   std::string realm;
   std::string nonce;
   std::vector<uint8_t> password_algorithms;
   uint16_t password_algorithm = PASSWORD_MD5;
   bool userhash = false;
   bool sha256_integrity = false;
   uint16_t error_code = 0;
};

bool decode_long_term_challenge(const Message &Parsed, LongTermChallenge &Result);

// Username, realm and password are prepared with the RFC 8265 OpaqueString profile.
bool encode_long_term_request(std::span<const uint8_t, 12> TransactionId, const LongTermChallenge &Challenge,
   std::string_view Username, std::string_view Password, std::vector<uint8_t> &Result);

// Shared long-term encoder for other STUN usages.  Usage attributes precede authentication attributes.
bool encode_long_term_request(uint16_t Type, std::span<const uint8_t, 12> TransactionId,
   std::span<const Attribute> Attributes, const LongTermChallenge &Challenge,
   std::string_view Username, std::string_view Password, std::vector<uint8_t> &Result);

// Build a message with optional MESSAGE-INTEGRITY and FINGERPRINT attributes.  Attribute values are copied.
// Authentication attributes must not appear in Attributes; they are appended in protocol order by this function.
// Sha256Length defaults to the full digest; shorter values require an explicit STUN-usage decision.
bool encode(uint16_t Type, std::span<const uint8_t, 12> TransactionId, std::span<const Attribute> Attributes,
   std::span<const uint8_t> Key, bool Sha256, bool Fingerprint, std::vector<uint8_t> &Result,
   size_t Sha256Length = 32);

// Initial short-term Binding requests carry both integrity algorithms when the peer's preference is unknown.
// Key and the single USERNAME attribute are raw UTF-8 prepared with OpaqueString before encoding.
// Other attributes are encoded before the authenticators.
bool encode_short_term_request(std::span<const uint8_t, 12> TransactionId,
   std::span<const Attribute> Attributes, std::span<const uint8_t> Key, bool Fingerprint,
   std::vector<uint8_t> &Result);

// Key is raw UTF-8 prepared with OpaqueString.  The caller must also match the response source address to the server.
// On success ResponseSha256 identifies the algorithm selected by the server for subsequent requests.
bool verify_short_term_response(std::span<const uint8_t> Datagram, const Message &Parsed,
   std::span<const uint8_t, 12> TransactionId, std::span<const uint8_t> Key,
   bool SentSha1, bool SentSha256, bool &ResponseSha256);

enum class LongTermStatus { IGNORED, RETRY, COMPLETE, FAILED };

// One UDP Binding transaction with a 401 challenge and optional 438 stale-nonce retry.  The caller checks the
// response source, sends the returned request, and handles UDP retransmission of the current request.
// Username and password are prepared with the RFC 8265 OpaqueString profile when start() is called.
class LongTermBindingClient {
public:
   LongTermBindingClient(std::string_view Username, std::string_view Password);
   ~LongTermBindingClient();
   LongTermBindingClient(const LongTermBindingClient &) = delete;
   LongTermBindingClient & operator=(const LongTermBindingClient &) = delete;

   bool start(std::span<const uint8_t, 12> TransactionId, std::vector<uint8_t> &Request);
   LongTermStatus receive(std::span<const uint8_t> Datagram,
      std::span<const uint8_t, 12> NextTransactionId, std::vector<uint8_t> &NextRequest);

private:
   enum class State { NEW, WAIT_FIRST, WAIT_AUTH, COMPLETE, FAILED };
   std::string username;
   std::string password;
   std::array<uint8_t, 12> transaction_id{};
   LongTermChallenge challenge;
   std::vector<uint8_t> key;
   State state = State::NEW;
   unsigned stale_retries = 0;
};

} // namespace rtc::stun
