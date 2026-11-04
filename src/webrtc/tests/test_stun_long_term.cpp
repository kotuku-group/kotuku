#include "../stun/stun.h"

#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

using namespace rtc::stun;

static int fail(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

int main() {
   std::array<uint8_t, 12> first_id{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
   std::array<uint8_t, 12> second_id{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
   std::array<uint8_t, 12> third_id{3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
   auto bytes = [](std::string_view Value) {
      return std::span<const uint8_t>((const uint8_t *)Value.data(), Value.size());
   };
   std::array<uint8_t, 4> error_code{0xa5, 0x5a, 0xf4, 1};
   std::array<uint8_t, 4> algorithms{0, 2, 0, 0};
   auto realm = std::string_view("example.org");
   auto nonce = std::string_view("obMatJos2wAAAnonce");
   std::array<Attribute, 4> challenge_attributes{{
      {ATTR_ERROR_CODE, error_code}, {ATTR_REALM, bytes(realm)}, {ATTR_NONCE, bytes(nonce)},
      {ATTR_PASSWORD_ALGORITHMS, algorithms}
   }};
   std::vector<uint8_t> challenge_wire;
   if (not encode(BINDING_ERROR, first_id, challenge_attributes, {}, false, true, challenge_wire)) {
      return fail("Could not encode the initial challenge");
   }

   LongTermBindingClient client("user", "pass");
   std::vector<uint8_t> request, next;
   Message parsed;
   if (not client.start(first_id, request) or not parse(request, parsed) or
       parsed.type != BINDING_REQUEST or parsed.find(ATTR_USERNAME) or
       parsed.find(ATTR_MESSAGE_INTEGRITY) or parsed.find(ATTR_MESSAGE_INTEGRITY_SHA256) or
       not verify_fingerprint(request, parsed)) return fail("Initial request included credentials");
   auto wrong_transaction = challenge_wire;
   wrong_transaction[8] ^= 1;
   if (client.receive(wrong_transaction, second_id, next) != LongTermStatus::IGNORED or not next.empty()) {
      return fail("Challenge with a different transaction ID was accepted");
   }
   LongTermBindingClient repeated_id("user", "pass");
   if (not repeated_id.start(first_id, request) or
       repeated_id.receive(challenge_wire, first_id, next) != LongTermStatus::FAILED or
       not next.empty()) return fail("Long-term retry reused a transaction ID");
   if (client.receive(challenge_wire, second_id, next) != LongTermStatus::RETRY or
       not parse(next, parsed) or parsed.transaction_id != second_id or
       not parsed.find(ATTR_USERHASH) or parsed.find(ATTR_USERNAME) or
       not parsed.find(ATTR_PASSWORD_ALGORITHM) or not parsed.find(ATTR_PASSWORD_ALGORITHMS) or
       not verify_fingerprint(next, parsed)) return fail("Long-term challenge retry failed");
   std::vector<uint8_t> key;
   if (not long_term_key("user", realm, "pass", key, true) or
       not verify_integrity(next, parsed, key, true)) return fail("Challenge retry integrity failed");

   std::vector<uint8_t> success;
   if (not encode(BINDING_SUCCESS, second_id, {}, key, true, false, success)) {
      return fail("Could not encode authenticated success");
   }
   auto tampered = success;
   tampered.back() ^= 1;
   std::array<uint8_t, 4> unknown_value{1, 2, 3, 4};
   Attribute unknown{0x1234, unknown_value};
   std::vector<uint8_t> unknown_success;
   if (not encode(BINDING_SUCCESS, second_id, std::span(&unknown, 1), key, true, false,
       unknown_success)) return fail("Could not encode unknown required attribute");
   auto trailing = success;
   std::array<uint8_t, 8> unauthenticated_attribute{0x80, 0x22, 0, 4, 1, 2, 3, 4};
   trailing.insert(trailing.end(), unauthenticated_attribute.begin(), unauthenticated_attribute.end());
   trailing[3] = uint8_t(trailing.size() - 20);
   if (client.receive(tampered, third_id, next) != LongTermStatus::IGNORED or
       client.receive(unknown_success, third_id, next) != LongTermStatus::IGNORED or
       client.receive(trailing, third_id, next) != LongTermStatus::IGNORED or
       client.receive(success, third_id, next) != LongTermStatus::COMPLETE or
       client.receive(success, third_id, next) != LongTermStatus::IGNORED) {
      return fail("Authenticated success validation failed");
   }

   LongTermBindingClient stale_client("user", "pass");
   if (not stale_client.start(first_id, request) or
       stale_client.receive(challenge_wire, second_id, next) != LongTermStatus::RETRY) {
      return fail("Could not start stale-nonce transaction");
   }
   error_code[3] = 38;
   nonce = "obMatJos2wBBBnewnonce";
   challenge_attributes[2] = {ATTR_NONCE, bytes(nonce)};
   std::vector<uint8_t> stale_wire;
   if (not encode(BINDING_ERROR, second_id, challenge_attributes, key, true, false, stale_wire)) {
      return fail("Could not encode stale-nonce challenge");
   }
   auto downgraded_attributes = challenge_attributes;
   downgraded_attributes[2] = {ATTR_NONCE, bytes("obMatJos2gBBBnewnonce")};
   std::vector<uint8_t> downgraded_wire;
   if (not encode(BINDING_ERROR, second_id, downgraded_attributes, {}, false, false, downgraded_wire) or
       stale_client.receive(downgraded_wire, third_id, next) != LongTermStatus::IGNORED or not next.empty()) {
      return fail("Unauthenticated stale-nonce response disabled USERHASH");
   }
   tampered = stale_wire;
   tampered[50] ^= 1;
   if (stale_client.receive(tampered, third_id, next) != LongTermStatus::IGNORED or
       stale_client.receive(stale_wire, third_id, next) != LongTermStatus::RETRY or
       not parse(next, parsed) or parsed.transaction_id != third_id or
       not parsed.find(ATTR_NONCE) or not verify_integrity(next, parsed, key, true) or
       stale_client.receive(success, first_id, next) != LongTermStatus::IGNORED) {
      return fail("Authenticated stale-nonce retry failed");
   }
   if (not encode(BINDING_SUCCESS, third_id, {}, key, true, false, success) or
       stale_client.receive(success, first_id, next) != LongTermStatus::COMPLETE) {
      return fail("Success after stale-nonce retry failed");
   }

   LongTermBindingClient rejected("user", "pass");
   error_code[3] = 1;
   if (not rejected.start(first_id, request) or
       rejected.receive(challenge_wire, second_id, next) != LongTermStatus::RETRY or
       not encode(BINDING_ERROR, second_id, challenge_attributes, {}, false, false, challenge_wire) or
       rejected.receive(challenge_wire, third_id, next) != LongTermStatus::FAILED) {
      return fail("Repeated 401 with unchanged credentials was retried");
   }

   // Long-term credentials are prepared inside the STUN API, including NFC and non-ASCII spaces.
   auto raw_username = std::string_view("A\xcc\x8a");
   auto raw_password = std::string_view("foo\xe1\x9a\x80" "bar");
   std::vector<uint8_t> normalised_key;
   if (not long_term_key(raw_username, realm, raw_password, key, true) or
       not long_term_key("\xc3\x85", realm, "foo bar", normalised_key, true) or
       key != normalised_key or long_term_key("user", realm, "bad\tpassword", key, true) or
       not key.empty()) return fail("Long-term OpaqueString key preparation failed");
   LongTermChallenge unicode_challenge;
   unicode_challenge.realm = std::string("r\xe1\x9a\x80" "ealm");
   unicode_challenge.nonce = "nonce";
   if (not encode_long_term_request(first_id, unicode_challenge, raw_username, raw_password, request) or
       not parse(request, parsed) or not parsed.find(ATTR_USERNAME) or not parsed.find(ATTR_REALM) or
       std::string_view((const char *)parsed.find(ATTR_USERNAME)->value.data(),
          parsed.find(ATTR_USERNAME)->value.size()) != "\xc3\x85" or
       std::string_view((const char *)parsed.find(ATTR_REALM)->value.data(),
          parsed.find(ATTR_REALM)->value.size()) != "r ealm") {
      return fail("Long-term retry did not emit prepared credentials");
   }
   LongTermBindingClient unicode_client(raw_username, raw_password);
   LongTermBindingClient invalid_client("user", "bad\tpassword");
   if (not unicode_client.start(first_id, request) or invalid_client.start(first_id, next)) {
      return fail("Long-term client credential preparation failed");
   }
   for (const auto &invalid_nonce : {std::string(128, 'n'), std::string("bad\nnonce"),
       std::string("bad\"nonce"), std::string("bad\\"), std::string("\xc0\x80"),
       std::string("\0", 1), std::string("\x80"), std::string("\xe2\x82"),
       std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80")}) {
      unicode_challenge.nonce = invalid_nonce;
      challenge_attributes[2] = {ATTR_NONCE, bytes(invalid_nonce)};
      LongTermChallenge decoded;
      if (encode_long_term_request(first_id, unicode_challenge, "user", "pass", request) or
          not encode(BINDING_ERROR, first_id, challenge_attributes, {}, false, false, challenge_wire) or
          not parse(challenge_wire, parsed) or decode_long_term_challenge(parsed, decoded)) {
         return fail("Malformed or oversized nonce was accepted");
      }
   }
   if (encode(0xc001, first_id, {}, {}, false, false, request) or not request.empty()) {
      return fail("Encoder accepted reserved STUN message type bits");
   }
   return 0;
}
