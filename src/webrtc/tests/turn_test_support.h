#pragma once
#include "../turn/turn.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace turn_test {
using namespace rtc::turn;
using rtc::stun::Attribute;
using rtc::stun::Message;
void require(bool Condition, const char *Description) {
   if (not Condition) { std::fprintf(stderr, "TURN: %s\n", Description); std::exit(1); }
}
std::span<const uint8_t> bytes(std::string_view Text) { return {(const uint8_t *)Text.data(), Text.size()}; }
Endpoint endpoint(uint8_t Last = 1, uint16_t Port = 3478, bool IPv6 = false) {
   Endpoint result; result.ipv6 = IPv6; result.port = Port;
   if (IPv6) { result.address[0] = 0x20; result.address[1] = 1; result.address[15] = Last; }
   else { result.address[0] = 192; result.address[1] = 0; result.address[2] = 2; result.address[3] = Last; }
   return result;
}
Config config(bool Reliable = false, bool IPv6 = false) {
   Config result; result.identity = {endpoint(), 7, 19}; result.username = "user"; result.password = "pass";
   result.reliable = Reliable; result.relay_ipv6 = IPv6;
   result.random = [serial = uint32_t(0)](TransactionId &Id) mutable {
      Id.fill(0); ++serial;
      for (size_t i = 0; i < 4; ++i) Id[i] = uint8_t(serial >> (i * 8));
      return true;
   };
   return result;
}
Packet pop(Client &ClientValue) {
   Packet result; require(ClientValue.pop_output(result), "Expected output packet"); return result;
}
Message parse(const Packet &PacketValue) {
   Message result; require(rtc::stun::parse(PacketValue.wire, result), "Invalid output STUN"); return result;
}
std::vector<uint8_t> key(bool Sha256 = false, std::string_view Password = "pass") {
   std::vector<uint8_t> result;
   require(rtc::stun::long_term_key("user", "example.org", Password, result, Sha256), "Key derivation");
   return result;
}
std::vector<uint8_t> challenge(const Packet &Request, uint16_t Code = 401,
   std::string_view Nonce = "first-nonce", bool Sha256 = false, bool Signed = false,
   std::string_view Realm = "example.org") {
   auto request = parse(Request);
   std::array<uint8_t, 4> error{0, 0, uint8_t(Code / 100), uint8_t(Code % 100)}, algorithms{0, 2, 0, 0};
   std::vector<Attribute> attributes{{rtc::stun::ATTR_ERROR_CODE, error},
      {rtc::stun::ATTR_REALM, bytes(Realm)}, {rtc::stun::ATTR_NONCE, bytes(Nonce)}};
   if (Sha256) attributes.push_back({rtc::stun::ATTR_PASSWORD_ALGORITHMS, algorithms});
   std::vector<uint8_t> result;
   require(rtc::stun::encode(request.type | 0x0110, request.transaction_id, attributes,
      Signed ? key(Sha256) : std::vector<uint8_t>{}, Sha256, true, result), "Challenge encoder");
   return result;
}
std::vector<uint8_t> success(const Packet &Request, uint32_t Lifetime = 600, bool Sha256 = false,
   bool IPv6 = false, std::string_view Password = "pass") {
   auto request = parse(Request);
   std::array<uint8_t, 4> lifetime{uint8_t(Lifetime >> 24), uint8_t(Lifetime >> 16),
      uint8_t(Lifetime >> 8), uint8_t(Lifetime)};
   std::vector<uint8_t> relayed, mapped;
   std::vector<Attribute> attributes;
   if (request.type IS ALLOCATE or request.type IS REFRESH) attributes.push_back({ATTR_LIFETIME, lifetime});
   if (request.type IS ALLOCATE) {
      require(encode_xor_address(endpoint(2, 50000, IPv6), request.transaction_id, relayed), "Relay address");
      require(encode_xor_address(endpoint(3, 40000), request.transaction_id, mapped), "Mapped address");
      attributes.push_back({ATTR_XOR_RELAYED_ADDRESS, relayed});
      attributes.push_back({rtc::stun::ATTR_XOR_MAPPED_ADDRESS, mapped});
   }
   std::vector<uint8_t> result;
   require(rtc::stun::encode(request.type | 0x0100, request.transaction_id, attributes,
      key(Sha256, Password), Sha256, true, result), "Success encoder");
   return result;
}
void establish(Client &ClientValue, bool Sha256 = false, uint32_t Lifetime = 600, bool IPv6 = false) {
   require(ClientValue.start(0), "Start allocation");
   auto initial = pop(ClientValue);
   auto request = parse(initial);
   require(request.type IS ALLOCATE and not request.find(rtc::stun::ATTR_USERNAME), "Unauthenticated Allocate");
   Datagram data;
   require(ClientValue.receive(initial.identity, challenge(initial, 401, "first-nonce", Sha256), 10, data) IS
      Result::ACCEPTED, "Initial authentication challenge");
   auto authenticated = pop(ClientValue); request = parse(authenticated);
   require(rtc::stun::verify_integrity(authenticated.wire, request, key(Sha256), Sha256), "Request integrity");
   require(request.find(ATTR_REQUESTED_TRANSPORT)->value[0] IS 17, "UDP relay requested on all transports");
   require(ClientValue.receive(authenticated.identity, success(authenticated, Lifetime, Sha256, IPv6), 20, data) IS
      Result::ACCEPTED and ClientValue.state() IS State::ACTIVE and ClientValue.pending() IS 0, "Allocation success");
}
void grant(Client &ClientValue, const Endpoint &Peer, uint64_t Now) {
   require(ClientValue.permission(Peer, Now), "Permission request"); auto request = pop(ClientValue);
   require(parse(request).type IS CREATE_PERMISSION, "CreatePermission method");
   Datagram data;
   require(ClientValue.receive(request.identity, success(request), Now + 1, data) IS Result::ACCEPTED,
      "Permission granted");
}
}
