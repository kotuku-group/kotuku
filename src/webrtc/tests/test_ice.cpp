#include "../ice/ice.h"
#include "../stun/stun.h"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

using namespace rtc;

static int fail(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

static std::vector<uint8_t> request(std::array<uint8_t, 12> TransactionId, bool Nominate = false,
   bool Controlled = false, std::string Username = "local:remote") {
   std::array<uint8_t, 4> priority{ 0x6e, 0, 1, 0xff };
   std::array<uint8_t, 8> tie_breaker{ 1, 2, 3, 4, 5, 6, 7, 8 };
   std::array<uint8_t, 1> empty{};
   std::array<stun::Attribute, 4> attributes{
      stun::Attribute{ stun::ATTR_USERNAME,
         std::span<const uint8_t>((const uint8_t *)Username.data(), Username.size()), 0 },
      stun::Attribute{ 0x0024, priority, 0 },
      stun::Attribute{ uint16_t(Controlled ? 0x8029 : 0x802a), tie_breaker, 0 },
      stun::Attribute{ 0x0025, std::span<const uint8_t>(empty.data(), 0), 0 }
   };
   std::string password = "local-password-0123456789";
   auto key = std::span<const uint8_t>((const uint8_t *)password.data(), password.size());
   std::vector<uint8_t> result;
   if (not stun::encode(stun::BINDING_REQUEST, TransactionId,
       std::span<const stun::Attribute>(attributes.data(), Nominate ? 4u : 3u),
       key, false, true, result)) return {};
   return result;
}

static bool valid_response(const std::vector<uint8_t> &Response, uint16_t Type,
   const std::array<uint8_t, 12> &TransactionId) {
   stun::Message parsed;
   std::string password = "local-password-0123456789";
   auto key = std::span<const uint8_t>((const uint8_t *)password.data(), password.size());
   return stun::parse(Response, parsed) and parsed.type IS Type and
      parsed.transaction_id IS TransactionId and stun::verify_integrity(Response, parsed, key) and
      stun::verify_fingerprint(Response, parsed);
}

int main() {
   ice::LiteResponder responder("local", "local-password-0123456789", "remote");
   ice::Endpoint first;
   first.address = { 192, 0, 2, 1 };
   first.port = 54321;
   ice::Endpoint second;
   second.address = { 200, 1, 13, 184, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
   second.port = 54322;
   second.ipv6 = true;
   std::array<uint8_t, 12> tx1{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
   std::array<uint8_t, 12> tx2{ 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };

   std::string username = "local:remote";
   std::string password = "local-password-0123456789";
   auto key = std::span<const uint8_t>((const uint8_t *)password.data(), password.size());
   std::array<uint8_t, 4> priority{ 0x6e, 0, 1, 0xff };
   std::array<uint8_t, 8> tie_breaker{ 1, 2, 3, 4, 5, 6, 7, 8 };
   std::array<uint8_t, 4> unknown_value{ 1, 2, 3, 4 };
   std::array<stun::Attribute, 4> unknown_attributes{{
      {stun::ATTR_USERNAME, std::span<const uint8_t>((const uint8_t *)username.data(), username.size())},
      {0x0024, priority}, {0x802a, tie_breaker}, {0x1234, unknown_value}
   }};
   std::vector<uint8_t> unknown_check;
   if (not stun::encode(stun::BINDING_REQUEST, tx2, unknown_attributes, key, false, true,
       unknown_check)) return fail("Unknown-attribute check encoding failed");
   auto unknown_result = responder.receive(unknown_check, first);
   stun::Message unknown_response;
   uint16_t unknown_code = 0;
   if (not unknown_result.authenticated or responder.validated(first) or unknown_result.nominated or
       not valid_response(unknown_result.response, stun::BINDING_ERROR, tx2) or
       not stun::parse(unknown_result.response, unknown_response) or
       not unknown_response.find(stun::ATTR_ERROR_CODE) or
       not stun::decode_error_code(*unknown_response.find(stun::ATTR_ERROR_CODE), unknown_code) or
       unknown_code != 420 or not unknown_response.find(stun::ATTR_UNKNOWN_ATTRIBUTES) or
       unknown_response.find(stun::ATTR_UNKNOWN_ATTRIBUTES)->value.size() != 2 or
       unknown_response.find(stun::ATTR_UNKNOWN_ATTRIBUTES)->value[0] != 0x12 or
       unknown_response.find(stun::ATTR_UNKNOWN_ATTRIBUTES)->value[1] != 0x34) {
      return fail("Unknown required ICE attribute did not receive authenticated 420");
   }

   // An authenticated check opens the source for DTLS before nomination.  Lost replies can be retried.
   auto check = request(tx1);
   auto result = responder.receive(check, first);
   if (not result.authenticated or result.nominated or responder.nominated() or
       not responder.validated(first) or not valid_response(result.response, stun::BINDING_SUCCESS, tx1)) {
      return fail("Initial connectivity check failed");
   }
   stun::Message response;
   if (not stun::parse(result.response, response)) return fail("Success response did not parse");
   std::array<uint8_t, 16> mapped{};
   uint16_t port = 0;
   bool ipv6 = true;
   auto address = response.find(stun::ATTR_XOR_MAPPED_ADDRESS);
   if (not address or not stun::decode_xor_address(*address, response, mapped, port, ipv6) or
       ipv6 or port != first.port or mapped[0] != 192 or mapped[3] != 1) {
      return fail("IPv4 mapped address differs");
   }
   result = responder.receive(check, first);
   if (not valid_response(result.response, stun::BINDING_SUCCESS, tx1)) return fail("Retransmit failed");

   // A check from a second source can arrive out of order and must validate independently.
   result = responder.receive(request(tx2), second);
   if (not result.authenticated or not responder.validated(second) or responder.nominated()) {
      return fail("Reordered IPv6 check failed");
   }
   if (not stun::parse(result.response, response)) return fail("IPv6 response did not parse");
   address = response.find(stun::ATTR_XOR_MAPPED_ADDRESS);
   if (not address or not stun::decode_xor_address(*address, response, mapped, port, ipv6) or
       not ipv6 or port != second.port or mapped != second.address) return fail("IPv6 mapped address differs");

   result = responder.receive(request(tx1, true), first);
   if (not result.nominated or not responder.nominated() or *responder.nominated() != first) {
      return fail("USE-CANDIDATE did not nominate the source");
   }

   auto tampered = request(tx2);
   tampered[32] ^= 1;
   if (responder.receive(tampered, first).authenticated) return fail("Tampered check passed authentication");
   if (responder.receive(request(tx2, false, false, "wrong:remote"), first).authenticated) {
      return fail("Wrong ICE username passed authentication");
   }
   result = responder.receive(request(tx2, false, true), first);
   if (not result.role_conflict or not valid_response(result.response, 0x0111, tx2)) {
      return fail("Controlled role did not receive an authenticated 487 response");
   }
   if (*responder.nominated() != first) return fail("Role conflict changed nomination");

   for (int i = 0; i < 18; ++i) {
      auto source = first;
      source.address[3] = uint8_t(10 + i);
      if (not responder.receive(request(tx2), source).authenticated) return fail("Additional check rejected");
   }
   if (not responder.validated(first) or not responder.nominated() or *responder.nominated() != first) {
      return fail("Validated source eviction discarded the nominated pair");
   }
   return 0;
}
