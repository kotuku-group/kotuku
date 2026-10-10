#include "../stun.h"

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

int main() {
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

   auto truncated = request;
   truncated.pop_back();
   if (parse(truncated, parsed)) return fail("Truncated attribute accepted");
   auto wrong_length = request;
   wrong_length[3] -= 4;
   if (parse(wrong_length, parsed)) return fail("Incorrect header length accepted");
   return 0;
}
