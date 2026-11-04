#include "../stun/stun.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

using namespace rtc::stun;

static std::vector<uint8_t> from_hex(std::string_view Hex) {
   auto digit = [](char Value) -> uint8_t {
      if ((Value >= '0') and (Value <= '9')) return Value - '0';
      if ((Value >= 'a') and (Value <= 'f')) return Value - 'a' + 10;
      if ((Value >= 'A') and (Value <= 'F')) return Value - 'A' + 10;
      return 0;
   };
   std::vector<uint8_t> bytes;
   for (size_t i = 0; i + 1 < Hex.size(); i += 2) bytes.push_back((digit(Hex[i]) << 4) | digit(Hex[i + 1]));
   return bytes;
}

static int fail(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

//********************************************************************************************************************
// XOR-address encoding against the RFC 5769 sections 2.2 and 2.3 response vectors, and attribute cardinality rules.

static int wire_helpers() {
   std::array<uint8_t, 12> tx{ 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae };

   std::array<uint8_t, 16> ipv4{ 192, 0, 2, 1 };
   auto encoded = encode_xor_address(ipv4, 32853, false, tx);
   auto expected = from_hex("0001a147e112a643");
   if (not std::equal(expected.begin(), expected.end(), encoded.begin())) return fail("IPv4 XOR address mismatch");

   std::array<uint8_t, 16> ipv6{ 0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
   encoded = encode_xor_address(ipv6, 32853, true, tx);
   expected = from_hex("0002a1470113a9faa5d3f179bc25f4b5bed2b9d9");
   if (not std::equal(expected.begin(), expected.end(), encoded.begin())) return fail("IPv6 XOR address mismatch");

   Message message;
   message.transaction_id = tx;
   message.attributes.push_back({ ATTR_XOR_MAPPED_ADDRESS, std::span<const uint8_t>(encoded.data(), 20), 0 });
   std::array<uint8_t, 16> address{};
   uint16_t port = 0;
   bool is_ipv6 = false;
   if (not decode_xor_address(message.attributes[0], message, address, port, is_ipv6) or not is_ipv6 or
       port != 32853 or address != ipv6) return fail("IPv6 XOR address did not round-trip");

   std::array<uint8_t, 4> four{};
   message.attributes.clear();
   if (valid_attribute(message, ATTR_FINGERPRINT, 4, true) or
       not valid_attribute(message, ATTR_FINGERPRINT, 4, false)) return fail("Absent attribute cardinality wrong");
   message.attributes.push_back({ ATTR_FINGERPRINT, four, 0 });
   if (not valid_attribute(message, ATTR_FINGERPRINT, 4, true) or
       valid_attribute(message, ATTR_FINGERPRINT, 8, false)) return fail("Single attribute validation wrong");
   message.attributes.push_back({ ATTR_FINGERPRINT, four, 8 });
   if (valid_attribute(message, ATTR_FINGERPRINT, 4, true) or
       valid_attribute(message, ATTR_FINGERPRINT, 4, false)) return fail("Duplicate attribute accepted");
   return 0;
}

int main() {
   if (auto result = wire_helpers()) return result;
   // RFC 5769 section 2.1: a real ICE Binding Request with HMAC-SHA1, padding and FINGERPRINT.
   auto request = from_hex(
      "000100582112a442b7e7a701bc34d686fa87dfae"
      "802200105354554e207465737420636c69656e74"
      "002400046e0001ff80290008932ff9b151263b36"
      "000600096576746a3a68367659202020"
      "000800149aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a2"
      "80280004e57a3bcf");
   auto password = std::string_view("VOkJxbRl1RmTxUk/WvJxBt");
   auto key = std::span<const uint8_t>((const uint8_t *)password.data(), password.size());
   auto bytes = [](std::string_view Value) {
      return std::span<const uint8_t>((const uint8_t *)Value.data(), Value.size());
   };
   Message parsed;
   if (not parse(request, parsed)) return fail("RFC request did not parse");
   if (parsed.type != BINDING_REQUEST) return fail("Incorrect message type");
   if ((!parsed.find(ATTR_USERNAME)) or (parsed.find(ATTR_USERNAME)->value.size() != 9)) {
      return fail("Padded USERNAME parsed incorrectly");
   }
   if (not verify_integrity(request, parsed, key)) return fail("RFC request integrity failed");
   if (not verify_fingerprint(request, parsed)) return fail("RFC request fingerprint failed");

   auto changed = request;
   changed[46] ^= 1;
   Message tampered;
   if (not parse(changed, tampered)) return fail("Tampered packet did not parse");
   if (verify_integrity(changed, tampered, key)) return fail("Tampered request passed integrity");
   if (verify_fingerprint(changed, tampered)) return fail("Tampered request passed fingerprint");

   // RFC 5769 section 2.2: IPv4 XOR-MAPPED-ADDRESS, HMAC and CRC.
   auto response = from_hex(
      "0101003c2112a442b7e7a701bc34d686fa87dfae"
      "8022000b7465737420766563746f7220"
      "002000080001a147e112a643"
      "000800142b91f599fd9e90c38c7489f92af9ba53f06be7d7"
      "80280004c07d4c96");
   Message response_parsed;
   if (not parse(response, response_parsed)) return fail("RFC response did not parse");
   if (not verify_integrity(response, response_parsed, key)) return fail("RFC response integrity failed");
   if (not verify_fingerprint(response, response_parsed)) return fail("RFC response fingerprint failed");
   std::array<uint8_t, 16> address{};
   uint16_t port = 0;
   bool ipv6 = true;
   auto mapped = response_parsed.find(ATTR_XOR_MAPPED_ADDRESS);
   if ((!mapped) or (not decode_xor_address(*mapped, response_parsed, address, port, ipv6)) or ipv6 or
       (port != 32853) or (address[0] != 192) or (address[1] != 0) or (address[2] != 2) or (address[3] != 1)) {
      return fail("RFC IPv4 mapped address differs");
   }

   // RFC 5769 section 2.3: the IPv6 mask includes the transaction ID.
   auto response_v6 = from_hex(
      "010100482112a442b7e7a701bc34d686fa87dfae"
      "8022000b7465737420766563746f7220"
      "002000140002a1470113a9faa5d3f179bc25f4b5bed2b9d9"
      "00080014a382954e4be67bf11784c97c8292c275bfe3ed41"
      "80280004c8fb0b4c");
   Message response_v6_parsed;
   if ((not parse(response_v6, response_v6_parsed)) or
       (not verify_integrity(response_v6, response_v6_parsed, key)) or
       (not verify_fingerprint(response_v6, response_v6_parsed))) {
      return fail("RFC IPv6 response authentication failed");
   }
   mapped = response_v6_parsed.find(ATTR_XOR_MAPPED_ADDRESS);
   auto expected_v6 = from_hex("20010db8123456780011223344556677");
   if ((!mapped) or (not decode_xor_address(*mapped, response_v6_parsed, address, port, ipv6)) or (!ipv6) or
       (port != 32853) or (not std::equal(address.begin(), address.end(), expected_v6.begin()))) {
      return fail("RFC IPv6 mapped address differs");
   }

   // RFC 8489 section 14.1: receivers ignore the first address byte.  ALTERNATE-SERVER shares this format.
   auto plain_v4 = from_hex("7f018055c0000201");
   Attribute plain{ATTR_MAPPED_ADDRESS, plain_v4};
   address.fill(0xff);
   if (not decode_mapped_address(plain, address, port, ipv6) or ipv6 or port != 32853 or
       address[0] != 192 or address[1] != 0 or address[2] != 2 or address[3] != 1 or address[4] != 0) {
      return fail("Plain IPv4 mapped address differs");
   }
   plain.type = ATTR_ALTERNATE_SERVER;
   if (not decode_mapped_address(plain, address, port, ipv6) or ipv6 or port != 32853) {
      return fail("Alternate server address differs");
   }
   auto plain_v6 = from_hex("ff02805520010db8123456780011223344556677");
   plain = {ATTR_MAPPED_ADDRESS, plain_v6};
   if (not decode_mapped_address(plain, address, port, ipv6) or not ipv6 or port != 32853 or
       not std::equal(address.begin(), address.end(), expected_v6.begin())) {
      return fail("Plain IPv6 mapped address differs");
   }
   plain.value = std::span<const uint8_t>(plain_v6).first(19);
   if (decode_mapped_address(plain, address, port, ipv6)) return fail("Short address was accepted");
   auto xor_v4 = from_hex("aa01a147e112a643");
   Attribute xor_mapped{ATTR_XOR_MAPPED_ADDRESS, xor_v4};
   if (not decode_xor_address(xor_mapped, response_parsed, address, port, ipv6) or ipv6 or
       port != 32853 or address[0] != 192 or address[1] != 0 or address[2] != 2 or address[3] != 1) {
      return fail("XOR address rejected nonzero reserved byte");
   }
   std::array<uint8_t, 4> error_value{0xa5, 0x5a, 0xf4, 87};
   Attribute error_attribute{ATTR_ERROR_CODE, error_value};
   uint16_t decoded_error = 0;
   if (not decode_error_code(error_attribute, decoded_error) or decoded_error != 487) {
      return fail("Error code rejected nonzero reserved bits");
   }
   error_value[3] = 100;
   if (decode_error_code(error_attribute, decoded_error)) return fail("Invalid error number was accepted");
   error_value[2] = 2;
   error_value[3] = 1;
   if (decode_error_code(error_attribute, decoded_error)) return fail("Invalid error class was accepted");

   // RFC 5769 section 2.4: long-term credentials use the MD5 digest of username:realm:normalised password.
   auto long_request = from_hex(
      "000100602112a44278ad3433c6ad72c029da412e"
      "00060012e3839ee38388e383aae38383e382afe382b90000"
      "0015001c662f2f3439396b39353464364f4c33346f4c39465354767936347341"
      "0014000b6578616d706c652e6f726700"
      "00080014f67024656dd64a3e02b8e0712e85c9a28ca89666");
   Message long_parsed;
   std::vector<uint8_t> long_key;
   if ((not parse(long_request, long_parsed)) or
       (not long_term_key("\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9",
          "example.org", "TheMatrIX", long_key)) or
       (not verify_integrity(long_request, long_parsed, long_key))) return fail("RFC long-term request failed");
   if (not long_term_key("user", "example.org", "pass", long_key, true) or
       long_key != from_hex("2eab843d62723b5549ca4dde2641a8c07ba67a3c7df8919c8da81ee206a4b907")) {
      return fail("RFC 8489 SHA-256 long-term key differs");
   }

   // RFC 8489 Appendix B.1 with verified erratum 6268: the corrected long-term SHA-256 request.
   auto sha256_request = from_hex(
      "000100902112a44278ad3433c6ad72c029da412e"
      "001e00204a3cf38fef6992bda952c6780417da0f24819415569e60b205c46e41407f1704"
      "001500296f624d61744a6f733241414143662f2f3439396b39353464364f4c33346f4c39465354767936347341000000"
      "0014000b6578616d706c652e6f726700"
      "001d000400020000"
      "001c0020b5c7bf005b6c52a21c51c5e892f81924136296cb927c43149309278cc6518e65");
   Message sha256_parsed;
   if (not parse(sha256_request, sha256_parsed) or
       not long_term_key("\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9",
          "example.org", "TheMatrIX", long_key, true) or
       not verify_integrity(sha256_request, sha256_parsed, long_key, true) or
       not sha256_parsed.find(ATTR_USERHASH) or
       sha256_parsed.find(ATTR_USERHASH)->value.size() != 32) {
      return fail("RFC 8489 long-term SHA-256 request failed");
   }
   // The RFC vector's authenticated prefix with a 16-byte integrity value; HMAC checked independently.
   auto truncated_sha256 = sha256_request;
   truncated_sha256.resize(sha256_parsed.find(ATTR_MESSAGE_INTEGRITY_SHA256)->offset);
   truncated_sha256[2] = 0;
   truncated_sha256[3] = 0x80;
   auto truncated_integrity = from_hex("001c0010c7d5412b8aa86b8153a94da6eb9ba50f");
   truncated_sha256.insert(truncated_sha256.end(), truncated_integrity.begin(), truncated_integrity.end());
   Message truncated_parsed;
   if (not parse(truncated_sha256, truncated_parsed) or
       verify_integrity(truncated_sha256, truncated_parsed, long_key, true) or
       not verify_integrity(truncated_sha256, truncated_parsed, long_key, true, 16) or
       verify_integrity(truncated_sha256, truncated_parsed, long_key, true, 20)) {
      return fail("Opt-in SHA-256 truncation validation failed");
   }
   truncated_sha256.back() ^= 1;
   if (not parse(truncated_sha256, truncated_parsed) or
       verify_integrity(truncated_sha256, truncated_parsed, long_key, true, 16)) {
      return fail("Modified truncated SHA-256 integrity was accepted");
   }

   // A generated Binding Request must round trip through the parser and both authenticators.
   auto username = std::string_view("local:remote");
   Attribute username_attribute{ATTR_USERNAME,
      std::span<const uint8_t>((const uint8_t *)username.data(), username.size())};
   std::vector<uint8_t> encoded;
   if (not encode(BINDING_REQUEST, parsed.transaction_id, std::span(&username_attribute, 1), key, false, true,
       encoded)) return fail("Encoding failed");
   Message generated;
   if ((not parse(encoded, generated)) or (not verify_integrity(encoded, generated, key)) or
       (not verify_fingerprint(encoded, generated))) return fail("Generated request failed validation");

   if (not encode(BINDING_REQUEST, parsed.transaction_id, std::span(&username_attribute, 1), key, true, true,
       encoded)) return fail("SHA-256 encoding failed");
   if ((not parse(encoded, generated)) or (not verify_integrity(encoded, generated, key, true)) or
       (not verify_fingerprint(encoded, generated))) return fail("Generated SHA-256 request failed validation");
   auto integrity_attribute = generated.find(ATTR_MESSAGE_INTEGRITY_SHA256);
   if (not integrity_attribute) return fail("SHA-256 integrity attribute missing");
   auto duplicate = encoded;
   auto begin = duplicate.begin() + integrity_attribute->offset;
   auto repeated = std::vector<uint8_t>(begin, begin + 36);
   duplicate.insert(duplicate.end() - 8, repeated.begin(), repeated.end());
   auto payload_length = duplicate.size() - 20;
   duplicate[2] = uint8_t(payload_length >> 8);
   duplicate[3] = uint8_t(payload_length);
   Message duplicated;
   if (not parse(duplicate, duplicated) or verify_integrity(duplicate, duplicated, key, true)) {
      return fail("Duplicate SHA-256 integrity attribute was accepted");
   }
   for (size_t length : {16u, 20u, 24u, 28u, 32u}) {
      if (not encode(BINDING_REQUEST, parsed.transaction_id, std::span(&username_attribute, 1),
          key, true, true, encoded, length) or not parse(encoded, generated) or
          not verify_integrity(encoded, generated, key, true, length) or
          not verify_fingerprint(encoded, generated)) return fail("SHA-256 integrity length failed round trip");
   }
   if (encode(BINDING_REQUEST, parsed.transaction_id, std::span(&username_attribute, 1),
       key, true, true, encoded, 12) or not encoded.empty() or
       encode(BINDING_REQUEST, parsed.transaction_id, std::span(&username_attribute, 1),
          key, true, true, encoded, 18) or not encoded.empty()) {
      return fail("Invalid SHA-256 integrity length was encoded");
   }

   // Independent HMAC-SHA1, HMAC-SHA256 and CRC32 values for an RFC 8489 dual-integrity first request.
   auto dual_vector = from_hex(
      "000100502112a4420102030405060708090a0b0c"
      "00060005616c696365000000"
      "00080014b93f2e66f3c7b61fb0147daf18dfec5176572bcc"
      "001c0020f52222305eba5e6fc0a57bb9e9239d94785ed3474cb99e55d9b2eb256efe5eb3"
      "802800047639739b");
   std::array<uint8_t, 12> dual_id{};
   std::copy_n(dual_vector.begin() + 8, dual_id.size(), dual_id.begin());
   auto dual_name = std::string_view("alice");
   auto dual_password = std::string_view("secret");
   Attribute dual_username{ATTR_USERNAME,
      std::span<const uint8_t>((const uint8_t *)dual_name.data(), dual_name.size())};
   auto dual_key = std::span<const uint8_t>((const uint8_t *)dual_password.data(), dual_password.size());
   if (not encode_short_term_request(dual_id, std::span(&dual_username, 1), dual_key, true, encoded) or
       encoded != dual_vector or not parse(encoded, generated) or
       not verify_integrity(encoded, generated, dual_key) or
       not verify_integrity(encoded, generated, dual_key, true) or
       not verify_fingerprint(encoded, generated)) return fail("Dual short-term request differs from HMAC fixture");
   auto raw_short_name = std::string_view("A\xcc\x8a");
   auto raw_short_password = std::string_view("foo\xe1\x9a\x80" "bar");
   auto prepared_short_password = std::string_view("foo bar");
   Attribute raw_short_username{ATTR_USERNAME, bytes(raw_short_name)};
   auto raw_short_key = bytes(raw_short_password);
   auto prepared_short_key = bytes(prepared_short_password);
   if (not encode_short_term_request(dual_id, std::span(&raw_short_username, 1), raw_short_key, true, encoded) or
       not parse(encoded, generated) or not generated.find(ATTR_USERNAME) or
       std::string_view((const char *)generated.find(ATTR_USERNAME)->value.data(),
          generated.find(ATTR_USERNAME)->value.size()) != "\xc3\x85" or
       not verify_integrity(encoded, generated, prepared_short_key) or
       not verify_integrity(encoded, generated, prepared_short_key, true)) {
      return fail("Short-term OpaqueString preparation failed");
   }
   auto invalid_short_password = std::string_view("bad\tpassword");
   if (encode_short_term_request(dual_id, std::span(&raw_short_username, 1),
       bytes(invalid_short_password), true, encoded) or not encoded.empty()) {
      return fail("Invalid short-term password was accepted");
   }
   std::array<Attribute, 2> repeated_username{dual_username, dual_username};
   if (encode_short_term_request(dual_id, repeated_username, dual_key, true, encoded) or
       not encoded.empty()) return fail("Dual short-term request accepted duplicate USERNAME");
   auto response_id = dual_id;
   bool selected_sha256 = false;
   if (not encode(BINDING_SUCCESS, dual_id, {}, dual_key, false, true, encoded) or
       not parse(encoded, generated) or
       not verify_short_term_response(encoded, generated, dual_id, dual_key, true, true, selected_sha256) or
       selected_sha256 or
       verify_short_term_response(encoded, generated, dual_id, dual_key, false, true, selected_sha256)) {
      return fail("SHA-1 short-term response negotiation failed");
   }
   response_id[0] ^= 1;
   if (verify_short_term_response(encoded, generated, response_id, dual_key, true, true, selected_sha256)) {
      return fail("A different short-term response transaction was accepted");
   }
   if (not encode(BINDING_SUCCESS, dual_id, {}, dual_key, true, true, encoded) or
       not parse(encoded, generated) or
       not verify_short_term_response(encoded, generated, dual_id, dual_key, true, true, selected_sha256) or
       not selected_sha256 or
       verify_short_term_response(encoded, generated, dual_id, dual_key, true, false, selected_sha256)) {
      return fail("SHA-256 short-term response negotiation failed");
   }
   std::array<uint8_t, 4> unknown_value{1, 2, 3, 4};
   std::array<Attribute, 3> unknown_attributes{{
      {0x1234, unknown_value}, {0x9234, unknown_value}, {0x1234, unknown_value}
   }};
   if (not encode(BINDING_SUCCESS, dual_id, unknown_attributes, dual_key, true, true, encoded) or
       not parse(encoded, generated) or
       not verify_integrity(encoded, generated, dual_key, true) or
       unknown_required_attributes(generated) != std::vector<uint16_t>{0x1234} or
       verify_short_term_response(encoded, generated, dual_id, dual_key, true, true, selected_sha256)) {
      return fail("Short-term response accepted an unknown required attribute");
   }
   unknown_attributes[0].type = 0x9235;
   unknown_attributes[2].type = 0x9236;
   if (not encode(BINDING_SUCCESS, dual_id, unknown_attributes, dual_key, true, true, encoded) or
       not parse(encoded, generated) or not unknown_required_attributes(generated).empty() or
       not verify_short_term_response(encoded, generated, dual_id, dual_key, true, true, selected_sha256)) {
      return fail("Short-term response rejected unknown optional attributes");
   }
   if (not encode(BINDING_SUCCESS, dual_id, {}, prepared_short_key, true, true, encoded) or
       not parse(encoded, generated) or
       not verify_short_term_response(encoded, generated, dual_id, raw_short_key, true, true, selected_sha256) or
       not selected_sha256 or verify_short_term_response(encoded, generated, dual_id,
          bytes(invalid_short_password), true, true, selected_sha256)) {
      return fail("Short-term response did not prepare its password");
   }
   if (not encode(BINDING_SUCCESS, dual_id, {}, dual_key, true, false, encoded)) {
      return fail("Could not encode a response without fingerprint");
   }
   auto trailing_response = encoded;
   auto unauthenticated_code = from_hex("0009000400000457");
   trailing_response.insert(trailing_response.end(), unauthenticated_code.begin(), unauthenticated_code.end());
   trailing_response[3] = uint8_t(trailing_response.size() - 20);
   if (not parse(trailing_response, generated) or
       verify_short_term_response(trailing_response, generated, dual_id, dual_key, true, true,
          selected_sha256)) return fail("Short-term response accepted an unauthenticated error code");
   if (not encode(BINDING_SUCCESS, dual_id, std::span(&dual_username, 1), dual_key, true, true, encoded) or
       not parse(encoded, generated) or
       verify_short_term_response(encoded, generated, dual_id, dual_key, true, true, selected_sha256)) {
      return fail("Response with USERNAME was accepted");
   }

   // RFC 8489 long-term challenges select the first supported password algorithm and require USERHASH when the
   // nonce advertises username anonymity.  The offered list is echoed verbatim in the authenticated retry.
   std::array<uint8_t, 4> error_code{0, 0, 4, 1};
   auto realm = std::string_view("example.org");
   auto nonce = std::string_view("obMatJos2wAAAnonce");
   std::array<uint8_t, 8> algorithms{0, 3, 0, 0, 0, 2, 0, 0};
   std::array<Attribute, 4> challenge_attributes{{
      {ATTR_ERROR_CODE, error_code}, {ATTR_REALM, bytes(realm)}, {ATTR_NONCE, bytes(nonce)},
      {ATTR_PASSWORD_ALGORITHMS, algorithms}
   }};
   std::vector<uint8_t> challenge_wire;
   if (not encode(BINDING_ERROR, parsed.transaction_id, challenge_attributes, {}, false, true, challenge_wire)) {
      return fail("Long-term challenge encoding failed");
   }
   Message challenge_message;
   LongTermChallenge challenge;
   if (not parse(challenge_wire, challenge_message) or
       not decode_long_term_challenge(challenge_message, challenge) or
       challenge.error_code != 401 or challenge.password_algorithm != 2 or
       not challenge.userhash or not challenge.sha256_integrity) {
      return fail("Long-term challenge decoding failed");
   }
   std::vector<uint8_t> retry;
   if (not encode_long_term_request(parsed.transaction_id, challenge, "user", "pass", retry)) {
      return fail("Long-term authenticated retry failed");
   }
   Message retry_message;
   auto expected_userhash = from_hex("cf9fa894dfc9b7680968aec1efb3a6b8b2a1cac25c0461cab3b0c2f87c25eeaf");
   auto expected_algorithm = from_hex("00020000");
   if (not parse(retry, retry_message) or
       not long_term_key("user", "example.org", "pass", long_key, true) or
       not verify_integrity(retry, retry_message, long_key, true) or
       not verify_fingerprint(retry, retry_message) or
       retry_message.find(ATTR_USERNAME) or not retry_message.find(ATTR_USERHASH) or
       not std::equal(retry_message.find(ATTR_USERHASH)->value.begin(),
          retry_message.find(ATTR_USERHASH)->value.end(), expected_userhash.begin(), expected_userhash.end()) or
       not retry_message.find(ATTR_PASSWORD_ALGORITHMS) or
       not std::equal(retry_message.find(ATTR_PASSWORD_ALGORITHMS)->value.begin(),
          retry_message.find(ATTR_PASSWORD_ALGORITHMS)->value.end(), algorithms.begin(), algorithms.end()) or
       not retry_message.find(ATTR_PASSWORD_ALGORITHM) or
       not std::equal(retry_message.find(ATTR_PASSWORD_ALGORITHM)->value.begin(),
          retry_message.find(ATTR_PASSWORD_ALGORITHM)->value.end(),
          expected_algorithm.begin(), expected_algorithm.end())) {
      return fail("Long-term retry attributes or authentication differ");
   }
   challenge_attributes[3] = {ATTR_PASSWORD_ALGORITHMS, std::span<const uint8_t>()};
   if (not encode(BINDING_ERROR, parsed.transaction_id, std::span(challenge_attributes.data(), 3),
       {}, false, false, challenge_wire) or not parse(challenge_wire, challenge_message) or
       decode_long_term_challenge(challenge_message, challenge)) {
      return fail("A nonce advertising password algorithms without a list was accepted");
   }
   challenge_attributes[3] = {ATTR_PASSWORD_ALGORITHMS, std::span<const uint8_t>(algorithms)};
   algorithms[7] = 1;
   if (not encode(BINDING_ERROR, parsed.transaction_id, challenge_attributes, {}, false, false, challenge_wire) or
       not parse(challenge_wire, challenge_message) or decode_long_term_challenge(challenge_message, challenge)) {
      return fail("A truncated password algorithm entry was accepted");
   }
   realm = "\xff";
   challenge_attributes[1] = {ATTR_REALM, bytes(realm)};
   if (not encode(BINDING_ERROR, parsed.transaction_id, std::span(challenge_attributes.data(), 3),
       {}, false, false, challenge_wire) or not parse(challenge_wire, challenge_message) or
       decode_long_term_challenge(challenge_message, challenge)) {
      return fail("Malformed UTF-8 realm was accepted");
   }
   realm = "example.org";
   challenge_attributes[1] = {ATTR_REALM, bytes(realm)};

   error_code[3] = 38;
   nonce = "legacy-nonce";
   challenge_attributes[2] = {ATTR_NONCE, bytes(nonce)};
   if (not encode(BINDING_ERROR, parsed.transaction_id, std::span(challenge_attributes.data(), 3),
       {}, false, false, challenge_wire) or not parse(challenge_wire, challenge_message) or
       not decode_long_term_challenge(challenge_message, challenge) or challenge.error_code != 438 or
       challenge.password_algorithm != 1 or challenge.userhash or challenge.sha256_integrity or
       not encode_long_term_request(parsed.transaction_id, challenge, "user", "pass", retry) or
       not parse(retry, retry_message) or
       not long_term_key("user", "example.org", "pass", long_key) or
       not verify_integrity(retry, retry_message, long_key) or
       not retry_message.find(ATTR_USERNAME) or retry_message.find(ATTR_USERHASH) or
       retry_message.find(ATTR_PASSWORD_ALGORITHMS) or retry_message.find(ATTR_PASSWORD_ALGORITHM)) {
      return fail("Legacy stale-nonce retry failed");
   }

   auto truncated = request;
   truncated.pop_back();
   if (parse(truncated, parsed)) return fail("Truncated attribute accepted");
   auto wrong_length = request;
   wrong_length[3] -= 4;
   if (parse(wrong_length, parsed)) return fail("Incorrect header length accepted");
   return 0;
}
