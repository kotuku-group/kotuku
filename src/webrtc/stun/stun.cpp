// STUN message codec and authentication (RFC 8489), including short-term credentials for ICE and the long-term
// credential mechanism used by TURN.

#include "stun.h"
#include "../precis/opaque_string.h"
#include "../../crypto/protocol_primitives.h"
#include "../../link/unicode.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace rtc::stun {

//********************************************************************************************************************
// Big-endian field accessors.

static uint16_t read16(const uint8_t *Bytes)
{
   return (uint16_t(Bytes[0]) << 8) | uint16_t(Bytes[1]);
}

static uint32_t read32(const uint8_t *Bytes)
{
   return (uint32_t(read16(Bytes)) << 16) | read16(Bytes + 2);
}

static void write16(uint8_t *Bytes, uint16_t Value)
{
   Bytes[0] = uint8_t(Value >> 8);
   Bytes[1] = uint8_t(Value);
}

static void write32(uint8_t *Bytes, uint32_t Value)
{
   write16(Bytes, uint16_t(Value >> 16));
   write16(Bytes + 2, uint16_t(Value));
}

//********************************************************************************************************************
// Erases its content on destruction, for prepared passwords.

struct SensitiveString {
   std::string value;
   ~SensitiveString() { crypto_protocol::secure_wipe(value.data(), value.size()); }
};

//********************************************************************************************************************
// Returns the first attribute of the given type, or null if none is present.

const Attribute * Message::find(uint16_t Type) const
{
   for (const auto &attribute : attributes) if (attribute.type IS Type) return &attribute;
   return nullptr;
}

//********************************************************************************************************************
// Parses the STUN header and attribute framing.  Attribute values are not validated, and remain views of Datagram.

bool parse(std::span<const uint8_t> Datagram, Message &Result)
{
   Result = {};

   if ((Datagram.size() < 20) or (Datagram.size() > 65555)) return false;
   if ((Datagram[0] & 0xc0) or (read32(Datagram.data() + 4) != MAGIC_COOKIE)) return false;

   auto payload_size = read16(Datagram.data() + 2);
   if ((payload_size & 3) or (size_t(payload_size) + 20 != Datagram.size())) return false;

   Message parsed;
   parsed.type = read16(Datagram.data());
   std::copy_n(Datagram.data() + 8, 12, parsed.transaction_id.begin());

   size_t offset = 20;
   while (offset < Datagram.size()) {
      if (Datagram.size() - offset < 4) return false;
      auto length = read16(Datagram.data() + offset + 2);
      auto padded = (size_t(length) + 3) & ~size_t(3);
      if (padded > Datagram.size() - offset - 4) return false;
      parsed.attributes.push_back({read16(Datagram.data() + offset), Datagram.subspan(offset + 4, length), offset});
      offset += 4 + padded;
   }

   Result = std::move(parsed);
   return true;
}

//********************************************************************************************************************
// Returns the comprehension-required attributes (types below 0x8000) that are not understood, without duplicates.

std::vector<uint16_t> unknown_required_attributes(const Message &Parsed)
{
   std::vector<uint16_t> unknown;
   for (const auto &attribute : Parsed.attributes) {
      if (attribute.type >= 0x8000) continue;
      switch (attribute.type) {
         case ATTR_MAPPED_ADDRESS:
         case ATTR_USERNAME:
         case ATTR_MESSAGE_INTEGRITY:
         case ATTR_ERROR_CODE:
         case ATTR_UNKNOWN_ATTRIBUTES:
         case ATTR_REALM:
         case ATTR_NONCE:
         case ATTR_MESSAGE_INTEGRITY_SHA256:
         case ATTR_PASSWORD_ALGORITHM:
         case ATTR_USERHASH:
         case ATTR_XOR_MAPPED_ADDRESS:
         case 0x0024: // ICE PRIORITY
         case 0x0025: // ICE USE-CANDIDATE
            continue;
      }

      if (std::find(unknown.begin(), unknown.end(), attribute.type) IS unknown.end()) {
         unknown.push_back(attribute.type);
      }
   }

   return unknown;
}

//********************************************************************************************************************
// Computes the MESSAGE-INTEGRITY HMAC over the first Offset bytes of Datagram.  The length field in the header is
// adjusted as if the message ended with an integrity attribute of IntegrityLength bytes.

static bool hmac_at(std::span<const uint8_t> Datagram, size_t Offset, std::span<const uint8_t> Key,
   bool Sha256, size_t IntegrityLength, std::span<uint8_t> Result)
{
   if ((Offset < 20) or (Offset > Datagram.size())) return false;

   auto prefix = std::vector<uint8_t>(Datagram.begin(), Datagram.begin() + Offset);
   write16(prefix.data() + 2, uint16_t(Offset + 4 + IntegrityLength - 20));

   if (Sha256) return Result.size() IS 32 and crypto_protocol::hmac_sha256(Key, prefix,
      std::span<uint8_t, 32>(Result.data(), 32));
   return Result.size() IS 20 and crypto_protocol::hmac_sha1(Key, prefix,
      std::span<uint8_t, 20>(Result.data(), 20));
}

//********************************************************************************************************************
// Verifies a MESSAGE-INTEGRITY or MESSAGE-INTEGRITY-SHA256 attribute.  A truncated SHA-256 digest is accepted only if
// it is at least MinSha256Length bytes.

bool verify_integrity(std::span<const uint8_t> Datagram, const Message &Parsed,
   std::span<const uint8_t> Key, bool Sha256, size_t MinSha256Length)
{
   auto attribute_type = Sha256 ? ATTR_MESSAGE_INTEGRITY_SHA256 : ATTR_MESSAGE_INTEGRITY;
   auto attribute = Parsed.find(attribute_type);
   if (not attribute) return false;

   auto length = attribute->value.size();
   if ((Sha256 and (MinSha256Length < 16 or MinSha256Length > 32 or MinSha256Length % 4 or
       length < MinSha256Length or length > 32 or length % 4)) or
       (not Sha256 and length != 20) or attribute->offset + 4 + length > Datagram.size()) return false;

   size_t count = 0;
   for (const auto &entry : Parsed.attributes) if (entry.type IS attribute_type) ++count;
   if (count != 1) return false;

   std::array<uint8_t, 32> expected{};
   if (not hmac_at(Datagram, attribute->offset, Key, Sha256, length,
       std::span<uint8_t>(expected.data(), Sha256 ? 32 : 20))) return false;
   return crypto_protocol::constant_time_equal(std::span<const uint8_t>(expected.data(), length), attribute->value);
}

//********************************************************************************************************************
// Verifies the FINGERPRINT attribute, which must be the last attribute of the message.

bool verify_fingerprint(std::span<const uint8_t> Datagram, const Message &Parsed)
{
   auto attribute = Parsed.find(ATTR_FINGERPRINT);
   if ((!attribute) or (attribute->value.size() != 4) or (attribute->offset + 8 != Datagram.size())) return false;

   size_t count = 0;
   for (const auto &entry : Parsed.attributes) if (entry.type IS ATTR_FINGERPRINT) ++count;
   if (count != 1) return false;

   return read32(attribute->value.data()) IS
      (crypto_protocol::crc32(Datagram.first(attribute->offset)) ^ 0x5354554e);
}

//********************************************************************************************************************
// Decodes an XOR-MAPPED-ADDRESS attribute.  The address is masked with the magic cookie and, for IPv6, the transaction
// ID.

bool decode_xor_address(const Attribute &AttributeValue, const Message &Parsed,
   std::array<uint8_t, 16> &Address, uint16_t &Port, bool &IPv6)
{
   const auto data = AttributeValue.value;
   if ((AttributeValue.type != ATTR_XOR_MAPPED_ADDRESS) or (data.size() < 4)) return false;

   IPv6 = data[1] IS 2;
   if ((data[1] != 1) and (!IPv6)) return false;
   auto length = IPv6 ? 16 : 4;
   if (data.size() != size_t(4 + length)) return false;

   Port = read16(data.data() + 2) ^ uint16_t(MAGIC_COOKIE >> 16);
   std::array<uint8_t, 16> mask{};
   write32(mask.data(), MAGIC_COOKIE);
   std::copy(Parsed.transaction_id.begin(), Parsed.transaction_id.end(), mask.begin() + 4);
   Address.fill(0);
   for (int i = 0; i < length; i++) Address[i] = data[4 + i] ^ mask[i];
   return true;
}

//********************************************************************************************************************

std::array<uint8_t, 20> encode_xor_address(const std::array<uint8_t, 16> &Address, uint16_t Port, bool IPv6,
   std::span<const uint8_t, 12> TransactionId)
{
   std::array<uint8_t, 20> result{};
   result[1] = IPv6 ? 2 : 1;
   write16(result.data() + 2, Port ^ uint16_t(MAGIC_COOKIE >> 16));

   std::array<uint8_t, 16> mask{};
   write32(mask.data(), MAGIC_COOKIE);
   std::copy(TransactionId.begin(), TransactionId.end(), mask.begin() + 4);
   for (size_t i = 0; i < (IPv6 ? 16u : 4u); ++i) result[4 + i] = Address[i] ^ mask[i];
   return result;
}

//********************************************************************************************************************

bool valid_attribute(const Message &Parsed, uint16_t Type, size_t Size, bool Required)
{
   size_t count = 0;
   for (const auto &attribute : Parsed.attributes) {
      if (attribute.type IS Type) {
         if (attribute.value.size() != Size) return false;
         ++count;
      }
   }

   return Required ? count IS 1 : count <= 1;
}

//********************************************************************************************************************
// Decodes a MAPPED-ADDRESS or ALTERNATE-SERVER attribute.

bool decode_mapped_address(const Attribute &AttributeValue,
   std::array<uint8_t, 16> &Address, uint16_t &Port, bool &IPv6)
{
   if (AttributeValue.type != ATTR_MAPPED_ADDRESS and
       AttributeValue.type != ATTR_ALTERNATE_SERVER) return false;

   auto data = AttributeValue.value;
   if (data.size() < 4) return false;
   auto ipv6 = data[1] IS 2;
   if (data[1] != 1 and not ipv6) return false;
   auto length = ipv6 ? 16u : 4u;
   if (data.size() != 4 + length) return false;

   std::array<uint8_t, 16> address{};
   std::copy_n(data.begin() + 4, length, address.begin());
   Address = address;
   Port = read16(data.data() + 2);
   IPv6 = ipv6;
   return true;
}

//********************************************************************************************************************
// Decodes an ERROR-CODE attribute to its numeric code in the range 300-699.

bool decode_error_code(const Attribute &AttributeValue, uint16_t &Code)
{
   auto data = AttributeValue.value;
   if (AttributeValue.type != ATTR_ERROR_CODE or data.size() < 4) return false;

   auto error_class = data[2] & 7;
   if (error_class < 3 or error_class > 6 or data[3] > 99) return false;
   Code = uint16_t(error_class * 100 + data[3]);
   return true;
}

//********************************************************************************************************************
// Derives the long-term credential key from OpaqueString-prepared credentials.

bool long_term_key(std::string_view Username, std::string_view Realm, std::string_view Password,
   std::vector<uint8_t> &Key, bool Sha256)
{
   Key.clear();

   std::string username, realm;
   SensitiveString password;
   if (not precis::prepare_opaque_string(Username, username) or
       not precis::prepare_opaque_string(Realm, realm) or
       not precis::prepare_opaque_string(Password, password.value)) return false;
   return crypto_protocol::long_term_key(username, realm, password.value, Key, Sha256);
}

//********************************************************************************************************************
// Returns the attribute of the given type, or null if it is absent or repeated.

static const Attribute * unique_attribute(const Message &Parsed, uint16_t Type)
{
   const Attribute *found = nullptr;
   for (const auto &attribute : Parsed.attributes) {
      if (attribute.type != Type) continue;
      if (found) return nullptr;
      found = &attribute;
   }

   return found;
}

//********************************************************************************************************************
// Returns true if the attribute type appears no more than once.

static bool single_or_absent(const Message &Parsed, uint16_t Type)
{
   size_t count = 0;
   for (const auto &attribute : Parsed.attributes) if (attribute.type IS Type) ++count;
   return count <= 1;
}

//********************************************************************************************************************
// Returns the value of a base64 digit, or -1 if Digit is not in the base64 alphabet.

static int base64_digit(uint8_t Digit)
{
   if (Digit >= 'A' and Digit <= 'Z') return Digit - 'A';
   if (Digit >= 'a' and Digit <= 'z') return Digit - 'a' + 26;
   if (Digit >= '0' and Digit <= '9') return Digit - '0' + 52;
   if (Digit IS '+') return 62;
   if (Digit IS '/') return 63;
   return -1;
}

//********************************************************************************************************************
// Validates a NONCE, which is limited to 128 characters of qdtext or quoted-pair.

static bool valid_nonce(std::string_view Value)
{
   // NONCE is opaque qdtext/quoted-pair, not an OpaqueString: preserve its exact UTF-8 bytes.
   if (Value.empty() or Value.size() > 763) return false;

   size_t offset = 0, characters = 0;
   while (offset < Value.size()) {
      int count;
      auto codepoint = UTF8ReadValue(Value.substr(offset), &count);
      if (codepoint IS 0 or ++characters >= 128) return false;
      offset += size_t(count);
      if (codepoint IS '\\') {
         if (offset IS Value.size()) return false;
         auto escaped = uint8_t(Value[offset++]);
         if (escaped > 127 or escaped IS '\r' or escaped IS '\n' or ++characters >= 128) return false;
      }
      else if (codepoint IS '\r') {
         if (offset + 1 >= Value.size() or Value[offset] != '\n' or
             (Value[offset + 1] != ' ' and Value[offset + 1] != '\t')) return false;
         ++offset;
         if (++characters >= 128) return false;
      }
      else if (codepoint < 128 and codepoint != '\t' and codepoint != ' ' and
          codepoint != '!' and not (codepoint >= 35 and codepoint <= 91) and
          not (codepoint >= 93 and codepoint <= 126)) return false;
   }

   return true;
}

//********************************************************************************************************************
// Decodes a 401 or 438 error response as a long-term credential challenge.  A nonce that begins with the security
// feature cookie "obMatJos2" encodes the password algorithm and user anonymity features (RFC 8489 §9.2).  The first
// supported password algorithm offered by the server is selected.

bool decode_long_term_challenge(const Message &Parsed, LongTermChallenge &Result)
{
   Result = {};
   if (Parsed.type != BINDING_ERROR or not unknown_required_attributes(Parsed).empty()) return false;

   const auto *error = unique_attribute(Parsed, ATTR_ERROR_CODE);
   const auto *realm = unique_attribute(Parsed, ATTR_REALM);
   const auto *nonce = unique_attribute(Parsed, ATTR_NONCE);
   const auto *algorithms = unique_attribute(Parsed, ATTR_PASSWORD_ALGORITHMS);

   uint16_t error_code = 0;
   if (not error or not realm or not nonce or
       not single_or_absent(Parsed, ATTR_PASSWORD_ALGORITHMS) or
       not decode_error_code(*error, error_code) or
       (error_code != 401 and error_code != 438) or
       realm->value.empty() or nonce->value.empty()) return false;

   bool password_feature = false;
   bool userhash_feature = false;
   constexpr std::string_view cookie = "obMatJos2";
   auto nonce_bytes = nonce->value;

   if (not valid_nonce(std::string_view((const char *)nonce_bytes.data(), nonce_bytes.size()))) return false;
   if (nonce_bytes.size() >= cookie.size() and
       std::equal(cookie.begin(), cookie.end(), nonce_bytes.begin())) {
      if (nonce_bytes.size() < 13) return false;
      for (size_t i = cookie.size(); i < 13; ++i) if (base64_digit(nonce_bytes[i]) < 0) return false;
      auto flags = base64_digit(nonce_bytes[cookie.size()]);
      password_feature = (flags & 0x20) != 0;
      userhash_feature = (flags & 0x10) != 0;
   }

   if (password_feature and not algorithms) return false;

   uint16_t selected = PASSWORD_MD5;
   if (algorithms) {
      selected = 0;
      auto values = algorithms->value;
      size_t offset = 0;
      while (offset < values.size()) {
         if (values.size() - offset < 4) return false;
         auto algorithm = read16(values.data() + offset);
         auto size = read16(values.data() + offset + 2);
         auto padded = (size_t(size) + 3) & ~size_t(3);
         if (padded > values.size() - offset - 4) return false;
         if (not selected and not size and
             (algorithm IS PASSWORD_MD5 or algorithm IS PASSWORD_SHA256)) selected = algorithm;
         offset += 4 + padded;
      }
      if (not selected) return false;
   }

   LongTermChallenge challenge;
   auto raw_realm = std::string_view((const char *)realm->value.data(), realm->value.size());
   if (raw_realm.size() > 763 or not precis::prepare_opaque_string(raw_realm, challenge.realm) or
       challenge.realm.size() > 763 or
       std::count_if(challenge.realm.begin(), challenge.realm.end(),
          [](uint8_t Byte) { return (Byte & 0xc0) != 0x80; }) >= 128) return false;

   challenge.nonce.assign((const char *)nonce_bytes.data(), nonce_bytes.size());
   if (algorithms) challenge.password_algorithms.assign(algorithms->value.begin(), algorithms->value.end());
   challenge.password_algorithm = selected;
   challenge.userhash = userhash_feature;
   challenge.sha256_integrity = algorithms != nullptr;
   challenge.error_code = error_code;
   Result = std::move(challenge);
   return true;
}

//********************************************************************************************************************
// Encodes a Binding request that answers Challenge.

bool encode_long_term_request(std::span<const uint8_t, 12> TransactionId, const LongTermChallenge &Challenge,
   std::string_view Username, std::string_view Password, std::vector<uint8_t> &Result)
{
   return encode_long_term_request(BINDING_REQUEST, TransactionId, {}, Challenge, Username, Password, Result);
}

//********************************************************************************************************************
// Encodes a request of the given Type that answers Challenge.  The authentication attributes are appended to the
// caller's Attributes, followed by MESSAGE-INTEGRITY or MESSAGE-INTEGRITY-SHA256 and FINGERPRINT.

bool encode_long_term_request(uint16_t Type, std::span<const uint8_t, 12> TransactionId,
   std::span<const Attribute> Attributes, const LongTermChallenge &Challenge,
   std::string_view Username, std::string_view Password, std::vector<uint8_t> &Result)
{
   Result.clear();

   std::string username, realm;
   SensitiveString password;
   if (not precis::prepare_opaque_string(Username, username) or
       not precis::prepare_opaque_string(Challenge.realm, realm) or
       not precis::prepare_opaque_string(Password, password.value) or
       not valid_nonce(Challenge.nonce) or
       (Challenge.password_algorithm != PASSWORD_MD5 and Challenge.password_algorithm != PASSWORD_SHA256) or
       (Challenge.sha256_integrity != not Challenge.password_algorithms.empty()) or
       username.size() >= 509 or realm.size() > 763 or
       std::count_if(realm.begin(), realm.end(),
          [](uint8_t Byte) { return (Byte & 0xc0) != 0x80; }) >= 128) return false;

   std::array<uint8_t, 32> userhash{};
   if (Challenge.userhash) {
      auto value = username + ":" + realm;
      if (not crypto_protocol::sha256(std::span<const uint8_t>((const uint8_t *)value.data(), value.size()),
          userhash)) return false;
   }

   std::array<uint8_t, 4> algorithm{};
   write16(algorithm.data(), Challenge.password_algorithm);

   // Reject caller-supplied authentication fields; the shared encoder owns their ordering and values.
   for (const auto &attribute : Attributes) {
      switch (attribute.type) {
         case ATTR_USERNAME: case ATTR_USERHASH: case ATTR_REALM: case ATTR_NONCE:
         case ATTR_PASSWORD_ALGORITHMS: case ATTR_PASSWORD_ALGORITHM:
         case ATTR_MESSAGE_INTEGRITY: case ATTR_MESSAGE_INTEGRITY_SHA256: case ATTR_FINGERPRINT:
            return false;
      }
   }

   std::vector<Attribute> attributes(Attributes.begin(), Attributes.end());
   attributes.resize(Attributes.size() + 5);
   auto text_bytes = [](std::string_view Value) {
      return std::span<const uint8_t>((const uint8_t *)Value.data(), Value.size());
   };

   size_t count = Attributes.size();
   attributes[count++] = {Challenge.userhash ? ATTR_USERHASH : ATTR_USERNAME,
      Challenge.userhash ? std::span<const uint8_t>(userhash) : text_bytes(username)};
   attributes[count++] = {ATTR_REALM, text_bytes(realm)};
   attributes[count++] = {ATTR_NONCE, text_bytes(Challenge.nonce)};
   if (Challenge.sha256_integrity) {
      attributes[count++] = {ATTR_PASSWORD_ALGORITHMS, Challenge.password_algorithms};
      attributes[count++] = {ATTR_PASSWORD_ALGORITHM, algorithm};
   }

   std::vector<uint8_t> key;
   if (not crypto_protocol::long_term_key(username, realm, password.value, key,
       Challenge.password_algorithm IS PASSWORD_SHA256)) return false;

   auto success = encode(Type, TransactionId, std::span(attributes.data(), count), key,
      Challenge.sha256_integrity, true, Result);
   crypto_protocol::secure_wipe(key.data(), key.size());
   return success;
}

//********************************************************************************************************************
// Appends an attribute with padding, and updates the message length in the header.

static bool append_attribute(std::vector<uint8_t> &Result, uint16_t Type, std::span<const uint8_t> Value)
{
   if (Value.size() > 65535) return false;

   auto padded = (Value.size() + 3) & ~size_t(3);
   if (Result.size() + 4 + padded > 65555) return false;

   auto offset = Result.size();
   Result.resize(offset + 4 + padded, 0);
   write16(Result.data() + offset, Type);
   write16(Result.data() + offset + 2, uint16_t(Value.size()));
   std::copy(Value.begin(), Value.end(), Result.begin() + offset + 4);
   write16(Result.data() + 2, uint16_t(Result.size() - 20));
   return true;
}

//********************************************************************************************************************
// Appends a FINGERPRINT attribute, which is the CRC-32 of the message XOR 0x5354554e.

static bool append_fingerprint(std::vector<uint8_t> &Result)
{
   if (Result.size() + 8 > 65555) return false;

   write16(Result.data() + 2, uint16_t(Result.size() + 8 - 20));
   std::array<uint8_t, 4> fingerprint{};
   write32(fingerprint.data(), crypto_protocol::crc32(Result) ^ 0x5354554e);
   return append_attribute(Result, ATTR_FINGERPRINT, fingerprint);
}

//********************************************************************************************************************
// Encodes a STUN message.  If a Key is provided, a message integrity attribute is appended, optionally truncated to
// Sha256Length bytes.  Callers may not supply integrity or fingerprint attributes directly.

bool encode(uint16_t Type, std::span<const uint8_t, 12> TransactionId, std::span<const Attribute> Attributes,
   std::span<const uint8_t> Key, bool Sha256, bool Fingerprint, std::vector<uint8_t> &Result,
   size_t Sha256Length)
{
   Result.assign(20, 0);
   if ((Type & 0xc000) or (Sha256 and (Sha256Length < 16 or Sha256Length > 32 or Sha256Length % 4))) {
      Result.clear();
      return false;
   }

   write16(Result.data(), Type);
   write32(Result.data() + 4, MAGIC_COOKIE);
   std::copy(TransactionId.begin(), TransactionId.end(), Result.begin() + 8);

   for (const auto &attribute : Attributes) {
      if ((attribute.type IS ATTR_MESSAGE_INTEGRITY) or (attribute.type IS ATTR_MESSAGE_INTEGRITY_SHA256) or
          (attribute.type IS ATTR_FINGERPRINT) or (!append_attribute(Result, attribute.type, attribute.value))) {
         Result.clear();
         return false;
      }
   }

   if (not Key.empty()) {
      std::array<uint8_t, 32> digest{};
      auto size = Sha256 ? Sha256Length : 20;
      if ((Result.size() + 4 + size + (Fingerprint ? 8 : 0) > 65555) or
          (not hmac_at(Result, Result.size(), Key, Sha256, size,
             std::span<uint8_t>(digest.data(), Sha256 ? 32 : 20))) or
          (not append_attribute(Result, Sha256 ? ATTR_MESSAGE_INTEGRITY_SHA256 : ATTR_MESSAGE_INTEGRITY,
             std::span<const uint8_t>(digest.data(), size)))) {
         Result.clear();
         return false;
      }
   }

   if (Fingerprint) {
      if (not append_fingerprint(Result)) {
         Result.clear();
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Encodes a 420 (Unknown Attribute) error response to Request.

bool encode_unknown_attributes_error(const Message &Request, std::span<const uint16_t> Unknown,
   std::span<const uint8_t> Key, std::vector<uint8_t> &Result)
{
   Result.clear();
   if (Request.type != BINDING_REQUEST or Unknown.empty() or Key.empty() or Unknown.size() > 32764) return false;

   std::vector<uint8_t> types(Unknown.size() * 2);
   for (size_t i = 0; i < Unknown.size(); ++i) {
      if (Unknown[i] >= 0x8000) return false;
      write16(types.data() + i * 2, Unknown[i]);
   }

   std::array<uint8_t, 4> error_code{0, 0, 4, 20};
   std::array<Attribute, 2> attributes{{
      {ATTR_ERROR_CODE, error_code}, {ATTR_UNKNOWN_ATTRIBUTES, types}
   }};
   return encode(BINDING_ERROR, Request.transaction_id, attributes, Key, false, true, Result);
}

//********************************************************************************************************************
// Encodes a Binding request with short-term credentials.  The USERNAME and password are prepared with the OpaqueString
// profile, and the message is authenticated with MESSAGE-INTEGRITY-SHA256.

bool encode_short_term_request(std::span<const uint8_t, 12> TransactionId,
   std::span<const Attribute> Attributes, std::span<const uint8_t> Key, bool Fingerprint,
   std::vector<uint8_t> &Result)
{
   size_t usernames = 0;
   std::string username;
   SensitiveString password;

   if (Key.empty()) {
      Result.clear();
      return false;
   }

   auto password_text = std::string_view((const char *)Key.data(), Key.size());
   if (not precis::prepare_opaque_string(password_text, password.value)) {
      Result.clear();
      return false;
   }

   std::vector<Attribute> attributes(Attributes.begin(), Attributes.end());
   for (auto &attribute : attributes) {
      if (attribute.type IS ATTR_USERNAME) {
         auto text = std::string_view((const char *)attribute.value.data(), attribute.value.size());
         if (not precis::prepare_opaque_string(text, username) or username.size() >= 509) {
            Result.clear();
            return false;
         }
         attribute.value = std::span<const uint8_t>((const uint8_t *)username.data(), username.size());
         ++usernames;
      }
      else if (attribute.type IS ATTR_USERHASH or attribute.type IS ATTR_REALM or
          attribute.type IS ATTR_NONCE or attribute.type IS ATTR_PASSWORD_ALGORITHM or
          attribute.type IS ATTR_PASSWORD_ALGORITHMS) {
         Result.clear();
         return false;
      }
   }

   auto prepared_key = std::span<const uint8_t>((const uint8_t *)password.value.data(), password.value.size());
   if (usernames != 1 or
       not encode(BINDING_REQUEST, TransactionId, attributes, prepared_key, false, false, Result)) {
      Result.clear();
      return false;
   }

   if (Result.size() + 36 + (Fingerprint ? 8 : 0) > 65555) {
      Result.clear();
      return false;
   }

   std::array<uint8_t, 32> digest{};
   if (not hmac_at(Result, Result.size(), prepared_key, true, digest.size(), digest) or
       not append_attribute(Result, ATTR_MESSAGE_INTEGRITY_SHA256, digest) or
       (Fingerprint and not append_fingerprint(Result))) {
      Result.clear();
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Verifies a response to a short-term credential request.  Exactly one integrity attribute must be present, of an
// algorithm that was used in the request, and only FINGERPRINT may follow it.

bool verify_short_term_response(std::span<const uint8_t> Datagram, const Message &Parsed,
   std::span<const uint8_t, 12> TransactionId, std::span<const uint8_t> Key,
   bool SentSha1, bool SentSha256, bool &ResponseSha256)
{
   if ((Parsed.type != BINDING_SUCCESS and Parsed.type != BINDING_ERROR) or
       not unknown_required_attributes(Parsed).empty() or
       not std::equal(Parsed.transaction_id.begin(), Parsed.transaction_id.end(), TransactionId.begin()) or
       Parsed.find(ATTR_USERNAME) or Parsed.find(ATTR_USERHASH) or Key.empty()) return false;

   SensitiveString password;
   auto password_text = std::string_view((const char *)Key.data(), Key.size());
   if (not precis::prepare_opaque_string(password_text, password.value)) return false;
   auto prepared_key = std::span<const uint8_t>((const uint8_t *)password.value.data(), password.value.size());

   auto sha1 = Parsed.find(ATTR_MESSAGE_INTEGRITY);
   auto sha256 = Parsed.find(ATTR_MESSAGE_INTEGRITY_SHA256);
   auto integrity = sha256 ? sha256 : sha1;
   if (integrity) {
      for (const auto &attribute : Parsed.attributes) {
         if (attribute.offset > integrity->offset and attribute.type != ATTR_FINGERPRINT) return false;
      }
   }

   if (bool(sha1) IS bool(sha256) or (sha1 and not SentSha1) or (sha256 and not SentSha256) or
       (Parsed.find(ATTR_FINGERPRINT) and not verify_fingerprint(Datagram, Parsed)) or
       not verify_integrity(Datagram, Parsed, prepared_key, sha256 != nullptr)) return false;

   ResponseSha256 = sha256 != nullptr;
   return true;
}

//********************************************************************************************************************
// LongTermBindingClient performs a Binding transaction with long-term credentials: an unauthenticated request, a 401
// challenge, an authenticated retry and up to three retries for stale nonces (438).

LongTermBindingClient::LongTermBindingClient(std::string_view Username, std::string_view Password) :
   username(Username), password(Password) { }

LongTermBindingClient::~LongTermBindingClient()
{
   crypto_protocol::secure_wipe(password.data(), password.size());
   crypto_protocol::secure_wipe(key.data(), key.size());
}

//********************************************************************************************************************
// Encodes the initial unauthenticated request.  The credentials are prepared here so that invalid credentials fail
// early.

bool LongTermBindingClient::start(std::span<const uint8_t, 12> TransactionId,
   std::vector<uint8_t> &Request)
{
   Request.clear();
   if (state != State::NEW) return false;

   std::string prepared_username;
   SensitiveString prepared_password;
   if (not precis::prepare_opaque_string(username, prepared_username) or
       not precis::prepare_opaque_string(password, prepared_password.value) or
       prepared_username.size() >= 509 or
       not encode(BINDING_REQUEST, TransactionId, {}, {}, false, true, Request)) return false;

   crypto_protocol::secure_wipe(password.data(), password.size());
   username = std::move(prepared_username);
   password = prepared_password.value;
   std::copy(TransactionId.begin(), TransactionId.end(), transaction_id.begin());
   state = State::WAIT_FIRST;
   return true;
}

//********************************************************************************************************************
// Processes a response to the current request.  Responses that cannot be authenticated are ignored rather than treated
// as failures, so that an off-path attacker cannot terminate the transaction.  NextRequest is set when the status is
// RETRY.

LongTermStatus LongTermBindingClient::receive(std::span<const uint8_t> Datagram,
   std::span<const uint8_t, 12> NextTransactionId, std::vector<uint8_t> &NextRequest)
{
   NextRequest.clear();
   if (state != State::WAIT_FIRST and state != State::WAIT_AUTH) return LongTermStatus::IGNORED;

   Message parsed;
   if (not parse(Datagram, parsed) or parsed.transaction_id != transaction_id or
       (parsed.type != BINDING_SUCCESS and parsed.type != BINDING_ERROR) or
       not unknown_required_attributes(parsed).empty() or
       (parsed.find(ATTR_FINGERPRINT) and not verify_fingerprint(Datagram, parsed))) {
      return LongTermStatus::IGNORED;
   }

   auto fail = [this]() {
      state = State::FAILED;
      return LongTermStatus::FAILED;
   };

   auto next_id = [&]() {
      return not std::equal(NextTransactionId.begin(), NextTransactionId.end(), transaction_id.begin());
   };

   auto retry = [&](const LongTermChallenge &NewChallenge) {
      if (not next_id() or not encode_long_term_request(NextTransactionId, NewChallenge,
          username, password, NextRequest)) return fail();
      std::vector<uint8_t> new_key;
      if (not long_term_key(username, NewChallenge.realm, password, new_key,
          NewChallenge.password_algorithm IS PASSWORD_SHA256)) {
         NextRequest.clear();
         return fail();
      }

      crypto_protocol::secure_wipe(key.data(), key.size());
      key = std::move(new_key);
      challenge = NewChallenge;
      std::copy(NextTransactionId.begin(), NextTransactionId.end(), transaction_id.begin());
      state = State::WAIT_AUTH;
      return LongTermStatus::RETRY;
   };

   if (state IS State::WAIT_FIRST) {
      LongTermChallenge first;
      if (parsed.type != BINDING_ERROR or parsed.find(ATTR_MESSAGE_INTEGRITY) or
          parsed.find(ATTR_MESSAGE_INTEGRITY_SHA256) or
          not decode_long_term_challenge(parsed, first) or first.error_code != 401) return fail();
      return retry(first);
   }

   auto expected_type = challenge.sha256_integrity ? ATTR_MESSAGE_INTEGRITY_SHA256 : ATTR_MESSAGE_INTEGRITY;
   auto other_type = challenge.sha256_integrity ? ATTR_MESSAGE_INTEGRITY : ATTR_MESSAGE_INTEGRITY_SHA256;
   auto integrity = parsed.find(expected_type);

   if (parsed.find(other_type) or parsed.find(ATTR_USERNAME) or parsed.find(ATTR_USERHASH)) {
      return LongTermStatus::IGNORED;
   }

   if (integrity and not verify_integrity(Datagram, parsed, key, challenge.sha256_integrity)) {
      return LongTermStatus::IGNORED;
   }

   if (parsed.type IS BINDING_SUCCESS) {
      if (not integrity) return LongTermStatus::IGNORED;
      for (const auto &attribute : parsed.attributes) {
         if (attribute.offset > integrity->offset and attribute.type != ATTR_FINGERPRINT) {
            return LongTermStatus::IGNORED;
         }
      }
      state = State::COMPLETE;
      return LongTermStatus::COMPLETE;
   }

   LongTermChallenge next;
   if (not decode_long_term_challenge(parsed, next)) {
      return integrity ? fail() : LongTermStatus::IGNORED;
   }

   // Challenge attributes must be covered by the integrity check if one is present.

   if (integrity) {
      for (const auto &attribute : parsed.attributes) {
         if ((attribute.type IS ATTR_ERROR_CODE or attribute.type IS ATTR_REALM or
             attribute.type IS ATTR_NONCE or attribute.type IS ATTR_PASSWORD_ALGORITHMS) and
             attribute.offset >= integrity->offset) return LongTermStatus::IGNORED;
      }
   }

   if (next.error_code IS 401) return fail(); // The same username and password cannot satisfy another 401.
   if (next.error_code != 438 or next.nonce IS challenge.nonce or stale_retries >= 3) return fail();
   if (not integrity and (next.realm != challenge.realm or
       next.password_algorithm != challenge.password_algorithm or
       next.sha256_integrity != challenge.sha256_integrity or next.userhash != challenge.userhash or
       next.password_algorithms != challenge.password_algorithms)) return LongTermStatus::IGNORED;

   ++stale_retries;
   return retry(next);
}

} // namespace rtc::stun
