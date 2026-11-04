#include "../sctp/sctp_association.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>

using namespace rtc;
using namespace rtc::sctp;
namespace {
int glFailures = 0;
#define CHECK(Condition) do { if (not (Condition)) { \
   std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #Condition); ++glFailures; } } while (false)

SctpConfig config(uint16_t Local, uint16_t Remote, uint32_t Seed) {
   SctpConfig result;
   result.local_port = Local;
   result.remote_port = Remote;
   result.initial_rto_us = 1000;
   result.max_rto_us = 8000;
   auto sequence = std::make_shared<uint32_t>(Seed);
   result.random = [sequence](std::span<uint8_t> Bytes) {
      for (auto &byte : Bytes) {
         *sequence = *sequence * 1664525u + 1013904223u;
         byte = uint8_t(*sequence >> 24);
      }
      return true;
   };
   return result;
}
std::vector<uint8_t> wire(uint16_t Source, uint16_t Destination, uint32_t Tag, uint8_t Type,
   std::span<const uint8_t> Value = {}, uint8_t Flags = 0) {
   Encoder encoder(Source, Destination, Tag, 1100);
   CHECK(encoder.chunk(Type, Flags, Value));
   return encoder.finish();
}
std::vector<uint8_t> init(uint32_t Tag, uint16_t Outbound = 512, uint16_t Inbound = 512, uint32_t Window = 32768) {
   std::vector<uint8_t> value;
   put32(value, Tag);
   put32(value, Window);
   put16(value, Outbound);
   put16(value, Inbound);
   put32(value, 0xfffffffe);
   return value;
}
struct Pair {
   SctpAssociation a{config(5000, 5001, 1)}, b{config(5001, 5000, 2)};
   uint64_t now = 0;
   bool pump(uint8_t Drop = 255, bool *Dropped = nullptr) {
      auto from_a = a.take_output();
      auto from_b = b.take_output();
      auto deliver = [&](auto &Packets, SctpAssociation &Target) {
         for (auto &packet : Packets) {
            if (packet[12] IS Drop and Dropped and not *Dropped) { *Dropped = true; continue; }
            CHECK(Target.feed(packet, now) IS ERR::Okay);
         }
      };
      deliver(from_a, b);
      deliver(from_b, a);
      return not from_a.empty() or not from_b.empty();
   }
   void establish(bool Simultaneous = false, uint8_t Drop = 255) {
      CHECK(a.start(now) IS ERR::Okay);
      if (Simultaneous) CHECK(b.start(now) IS ERR::Okay);
      bool dropped = false;
      for (unsigned i = 0; i < 150 and
           (a.state() != SctpState::ESTABLISHED or b.state() != SctpState::ESTABLISHED); ++i) {
         (void)pump(Drop, &dropped);
         now += 1000;
         CHECK(a.tick(now) IS ERR::Okay);
         CHECK(b.tick(now) IS ERR::Okay);
      }
      (void)pump(Drop, &dropped); // Simultaneous initiation may establish before the final ACK flight.
      CHECK(a.state() IS SctpState::ESTABLISHED and b.state() IS SctpState::ESTABLISHED);
      CHECK(a.next_deadline_us() and *a.next_deadline_us() > now);
      CHECK(b.next_deadline_us() and *b.next_deadline_us() > now);
      if (Drop != 255) CHECK(dropped);
   }
};
void handshake_loss() {
   for (bool simultaneous : {false, true}) {
      for (uint8_t type : {uint8_t(INIT), uint8_t(INIT_ACK), uint8_t(COOKIE_ECHO), uint8_t(COOKIE_ACK)}) {
         Pair pair;
         pair.establish(simultaneous, type);
         CHECK(pair.a.peer().tag IS pair.b.tag() and pair.b.peer().tag IS pair.a.tag());
         CHECK(pair.a.peer().initial_tsn IS pair.b.initial_tsn());
         CHECK(pair.a.peer().forward_tsn and pair.a.peer().reconfig);
      }
   }
   auto small = config(5001, 5000, 2);
   small.inbound_streams = 7;
   small.outbound_streams = 11;
   SctpAssociation a(config(5000, 5001, 1)), b(small);
   CHECK(a.start(0) IS ERR::Okay);
   for (int i = 0; i < 8; ++i) {
      for (auto &packet : a.take_output()) CHECK(b.feed(packet, 0) IS ERR::Okay);
      for (auto &packet : b.take_output()) CHECK(a.feed(packet, 0) IS ERR::Okay);
   }
   CHECK(a.peer().outbound_streams IS 7 and a.peer().inbound_streams IS 11);
   CHECK(b.peer().outbound_streams IS 11 and b.peer().inbound_streams IS 7);
}
void cookies() {
   for (int mode = 0; mode < 5; ++mode) {
      Pair pair;
      CHECK(pair.a.start(0) IS ERR::Okay);
      auto request = pair.a.take_output().front();
      CHECK(pair.b.feed(request, 0) IS ERR::Okay);
      auto ack = pair.b.take_output().front();
      CHECK(pair.a.feed(ack, 0) IS ERR::Okay);
      auto echo = pair.a.take_output().front();
      Packet parsed;
      CHECK(parse(echo, parsed));
      auto value = std::vector<uint8_t>(parsed.chunks[0].value.begin(), parsed.chunks[0].value.end());
      uint32_t tag = parsed.verification_tag;
      uint16_t source = 5000;
      if (mode IS 0) value[20] ^= 1; // MAC protects peer tag
      if (mode IS 1) value.back() ^= 1;
      if (mode IS 2) tag ^= 1;
      if (mode IS 3) source = 5002;
      uint64_t now = mode IS 4 ? SctpAssociation::COOKIE_LIFETIME_US + 1 : 0;
      CHECK(pair.b.feed(wire(source, 5001, tag, COOKIE_ECHO, value), now) IS ERR::Okay);
      CHECK(pair.b.state() IS SctpState::CLOSED);
      if (mode IS 4) {
         auto errors = pair.b.take_output();
         CHECK(errors.size() IS 1 and errors[0][12] IS ERROR_CHUNK);
         CHECK(pair.a.feed(errors[0], now) IS ERR::Okay and pair.a.state() IS SctpState::COOKIE_WAIT);
      }
   }
   Pair pair;
   CHECK(pair.a.start(0) IS ERR::Okay);
   auto initial = pair.a.take_output().front();
   CHECK(pair.b.feed(initial, 0) IS ERR::Okay);
   auto ack = pair.b.take_output().front();
   CHECK(pair.a.feed(ack, 0) IS ERR::Okay);
   auto echo = pair.a.take_output().front();
   CHECK(pair.b.feed(echo, 0) IS ERR::Okay);
   auto cookie_ack = pair.b.take_output().front();
   CHECK(pair.a.feed(cookie_ack, 0) IS ERR::Okay);
   (void)pair.b.take_events();
   CHECK(pair.b.feed(echo, SctpAssociation::COOKIE_LIFETIME_US + 1) IS ERR::Okay);
   CHECK(pair.b.take_events().empty()); // Expired duplicate for the active tags is accepted, per RFC 9260 §5.2.4.
   auto duplicate_ack = pair.b.take_output();
   CHECK(duplicate_ack.size() IS 1 and duplicate_ack[0] IS cookie_ack);
   SctpAssociation restarted(config(5000, 5001, 55));
   auto now = SctpAssociation::COOKIE_LIFETIME_US + 2;
   CHECK(restarted.start(now) IS ERR::Okay);
   CHECK(pair.b.feed(restarted.take_output().front(), now) IS ERR::Okay);
   CHECK(pair.b.state() IS SctpState::ESTABLISHED); // Unexpected INIT cannot destroy existing state.
   CHECK(restarted.feed(pair.b.take_output().front(), now) IS ERR::Okay);
   CHECK(pair.b.feed(restarted.take_output().front(), now) IS ERR::Okay);
   CHECK(restarted.feed(pair.b.take_output().front(), now) IS ERR::Okay);
   CHECK(pair.b.peer().tag IS restarted.tag());
   auto events = pair.b.take_events();
   CHECK(events.size() IS 1 and events[0].restarted);
   auto current_tag = pair.b.tag();
   CHECK(pair.b.feed(echo, now) IS ERR::Okay and pair.b.tag() IS current_tag);
}
void extensions_and_bundle_validation() {
   Pair pair;
   CHECK(pair.a.start(0) IS ERR::Okay);
   auto initial = pair.a.take_output().front();
   Packet parsed;
   CHECK(parse(initial, parsed));
   std::vector<uint8_t> value(parsed.chunks[0].value.begin(), parsed.chunks[0].value.end());
   CHECK(append_parameter(value, PR_SUPPORTED, {}));
   std::array<uint8_t, 3> offered{RE_CONFIG, FORWARD_TSN, 64}; // Include I-DATA, which is unsupported.
   CHECK(append_parameter(value, EXTENSIONS, offered));
   CHECK(pair.b.feed(wire(5000, 5001, 0, INIT, value), 0) IS ERR::Okay);
   auto ack = pair.b.take_output().front();
   CHECK(parse(ack, parsed));
   std::vector<Item> params;
   CHECK(parameters(parsed.chunks[0].value.subspan(16), params));
   CHECK(params.size() IS 3 and params.front().type IS STATE_COOKIE);
   CHECK(params[1].type IS PR_SUPPORTED and params[2].type IS EXTENSIONS);
   CHECK(std::ranges::find(params[2].value, uint8_t(64)) IS params[2].value.end()); // No I-DATA advertisement.
   CHECK(pair.a.feed(ack, 0) IS ERR::Okay);
   auto echo = pair.a.take_output().front();
   CHECK(pair.b.feed(echo, 0) IS ERR::Okay);
   CHECK(pair.b.peer().forward_tsn and pair.b.peer().reconfig);
   CHECK(pair.b.require_extensions(true, false) IS ERR::Okay);
   CHECK(pair.b.require_extensions(false, true) IS ERR::Okay);
   CHECK(pair.b.require_extensions(false, false) IS ERR::Okay);
   CHECK(pair.a.feed(pair.b.take_output().front(), 0) IS ERR::Okay);
   CHECK(parse(echo, parsed));
   auto cookie = std::vector<uint8_t>(parsed.chunks.front().value.begin(), parsed.chunks.front().value.end());
   cookie.back() ^= 1;
   Encoder corrupted(5000, 5001, pair.b.tag(), 1100);
   CHECK(corrupted.chunk(COOKIE_ECHO, 0, cookie));
   CHECK(corrupted.chunk(ABORT, 0, {}));
   CHECK(pair.b.feed(corrupted.finish(), 0) IS ERR::Okay);
   CHECK(pair.b.state() IS SctpState::ESTABLISHED); // Invalid cookie discards all subsequent bundled chunks.
   Encoder misplaced(5000, 5001, pair.b.tag(), 1100);
   CHECK(misplaced.chunk(COOKIE_ACK, 0, {}));
   CHECK(misplaced.chunk(COOKIE_ECHO, 0, cookie));
   CHECK(pair.b.feed(misplaced.finish(), 0) IS ERR::Okay and pair.b.state() IS SctpState::ESTABLISHED);
   SctpAssociation passive(config(5001, 5000, 33));
   CHECK(passive.feed(wire(5000, 5001, 123, DATA), 0) IS ERR::Okay);
   auto ootb = passive.take_output();
   CHECK(ootb.size() IS 1 and ootb[0][12] IS ABORT and ootb[0][13] IS 1);
   CHECK(parse(ootb[0], parsed) and parsed.verification_tag IS 123);
   CHECK(passive.feed(wire(5000, 5001, 123, ABORT), 0) IS ERR::Okay and passive.take_output().empty());
}
void cookie_retry_and_duplicates() {
   Pair pair;
   CHECK(pair.a.start(0) IS ERR::Okay);
   auto initial = pair.a.take_output().front();
   CHECK(pair.b.feed(initial, 0) IS ERR::Okay);
   auto ack = pair.b.take_output().front();
   CHECK(pair.a.feed(ack, 0) IS ERR::Okay);
   auto echo = pair.a.take_output().front();
   CHECK(pair.a.feed(ack, 0) IS ERR::Okay and pair.a.take_output().empty());
   CHECK(pair.a.tick(1000) IS ERR::Okay);
   CHECK(pair.a.take_output().front() IS echo); // T1-cookie preserves the cookie byte-for-byte.
   CHECK(pair.a.next_deadline_us() IS 3000);
   CHECK(pair.b.feed(echo, 1000) IS ERR::Okay);
   auto cookie_ack = pair.b.take_output().front();
   CHECK(pair.a.feed(cookie_ack, 1000) IS ERR::Okay);
   (void)pair.a.take_events();
   CHECK(pair.a.feed(cookie_ack, 1000) IS ERR::Okay and pair.a.take_events().empty());
}
void repeated_stale_cookie_errors() {
   Pair pair;
   CHECK(pair.a.start(0) IS ERR::Okay);
   auto initial = pair.a.take_output().front();
   CHECK(pair.b.feed(initial, 0) IS ERR::Okay);
   auto ack = pair.b.take_output().front();
   for (unsigned i = 0; i <= 8; ++i) {
      CHECK(pair.a.feed(ack, i) IS ERR::Okay);
      (void)pair.a.take_output();
      std::vector<uint8_t> stale;
      std::array<uint8_t, 4> age{0, 0, 0, 1};
      CHECK(append_parameter(stale, 3, age));
      auto error = pair.a.feed(wire(5001, 5000, pair.a.tag(), ERROR_CHUNK, stale), i);
      CHECK(error IS (i IS 8 ? ERR::Timeout : ERR::Okay));
      (void)pair.a.take_output();
   }
   CHECK(pair.a.state() IS SctpState::FAILED and not pair.a.next_deadline_us());
}
void retries_and_errors() {
   auto cfg = config(5000, 5001, 1);
   cfg.max_retransmits = 2;
   SctpAssociation association(cfg);
   CHECK(association.start(10) IS ERR::Okay);
   auto original = association.take_output().front();
   CHECK(association.next_deadline_us() IS 1010);
   CHECK(association.tick(9) IS ERR::InvalidValue);
   CHECK(association.tick(1009) IS ERR::Okay and association.take_output().empty());
   CHECK(association.tick(1010) IS ERR::Okay and association.take_output().front() IS original);
   CHECK(association.next_deadline_us() IS 3010);
   CHECK(association.tick(3010) IS ERR::Okay);
   CHECK(association.tick(7010) IS ERR::Timeout and association.state() IS SctpState::FAILED);
   CHECK(not association.next_deadline_us());
   SctpAssociation overflow(cfg);
   CHECK(overflow.start(std::numeric_limits<uint64_t>::max() - 1) IS ERR::Timeout);
   auto invalid = cfg;
   invalid.random = [](std::span<uint8_t>) { return false; };
   SctpAssociation failed(invalid);
   CHECK(failed.start(0) IS ERR::Failed);
   Pair pair;
   pair.establish();
   CHECK(pair.a.feed(wire(5001, 5000, pair.a.tag() ^ 1, ABORT), pair.now) IS ERR::Okay);
   CHECK(pair.a.state() IS SctpState::ESTABLISHED);
   CHECK(pair.a.feed(wire(5001, 5000, pair.a.peer().tag, ABORT, {}, 1), pair.now) IS ERR::Failed);
   CHECK(pair.a.state() IS SctpState::FAILED);
}
void unknown_and_invalid() {
   for (unsigned action = 0; action < 4; ++action) {
      Pair pair;
      CHECK(pair.a.start(0) IS ERR::Okay);
      auto value = init(123);
      std::array<uint8_t, 1> unknown{42};
      CHECK(append_parameter(value, uint16_t((action << 14) | 0x123), unknown));
      CHECK(append_parameter(value, PR_SUPPORTED, {}));
      std::array<uint8_t, 1> extensions{RE_CONFIG};
      CHECK(append_parameter(value, EXTENSIONS, extensions));
      CHECK(pair.a.feed(wire(5001, 5000, 0, INIT, value), 0) IS ERR::Okay);
      CHECK(pair.a.state() IS SctpState::COOKIE_WAIT);
      auto packets = pair.a.take_output();
      CHECK(packets.size() IS 2); // Original INIT and response; existing timer is untouched.
      Packet parsed;
      CHECK(parse(packets.back(), parsed));
      std::vector<Item> params;
      CHECK(parameters(parsed.chunks[0].value.subspan(16), params));
      CHECK(params.size() IS ((action & 1) ? 4u : 3u));
      CHECK(pair.a.next_deadline_us() IS 1000);
      // Force COOKIE-ECHOED, then append COOKIE-ACK after an unknown chunk.
      (void)pair.b.feed(packets.front(), 0);
      CHECK(pair.a.feed(pair.b.take_output().front(), 0) IS ERR::Okay);
      (void)pair.a.take_output();
      Encoder encoder(5001, 5000, pair.a.tag(), 1100);
      CHECK(encoder.chunk(uint8_t((action << 6) | 63), 0, {}));
      CHECK(encoder.chunk(COOKIE_ACK, 0, {}));
      CHECK(pair.a.feed(encoder.finish(), 0) IS ERR::Okay);
      CHECK((pair.a.state() IS SctpState::ESTABLISHED) IS (action >= 2));
      CHECK(pair.a.take_output().size() IS ((action & 1) ? 1u : 0u));
   }
   for (int mode = 0; mode < 4; ++mode) {
      Pair pair;
      CHECK(pair.a.start(0) IS ERR::Okay);
      (void)pair.a.take_output();
      auto value = init(mode IS 0 ? 0 : 123, mode IS 1 ? 0 : 512, 512, mode IS 2 ? 100 : 32768);
      CHECK(pair.a.feed(wire(5001, 5000, pair.a.tag(), INIT_ACK, value), 0) IS ERR::InvalidData);
      CHECK(pair.a.state() IS SctpState::FAILED);
   }
   Pair pair;
   pair.establish();
   auto tag = pair.a.tag();
   CHECK(pair.a.feed(wire(5001, 5000, tag, INIT_ACK, init(0, 0)), pair.now) IS ERR::Okay);
   CHECK(pair.a.state() IS SctpState::ESTABLISHED and pair.a.tag() IS tag);
   for (size_t i = 0; i <= SctpAssociation::MAX_OUTPUT; ++i) {
      (void)pair.a.feed(wire(5001, 5000, 0, INIT, init(123)), pair.now);
   }
   CHECK(pair.a.state() IS SctpState::FAILED and pair.a.error() IS ERR::BufferOverflow);
}
}
int main() {
   handshake_loss();
   cookies();
   extensions_and_bundle_validation();
   cookie_retry_and_duplicates();
   repeated_stale_cookie_errors();
   retries_and_errors();
   unknown_and_invalid();
   return glFailures ? 1 : 0;
}
