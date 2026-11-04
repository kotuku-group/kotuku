#include "../sctp/sctp_association.h"
#include "../sctp/dcep.h"

#include <algorithm>
#include <cstdio>
#include <memory>

using namespace rtc;
using namespace rtc::sctp;
namespace {
int glFailures = 0;
#define CHECK(Condition) do { if (not (Condition)) { \
   std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #Condition); ++glFailures; } } while (false)
SctpConfig config(uint16_t Local, uint16_t Remote) {
   SctpConfig result;
   result.local_port = Local; result.remote_port = Remote;
   result.initial_rto_us = 10000; result.min_rto_us = 1000; result.max_rto_us = 160000;
   result.delayed_ack_us = 2000; result.heartbeat_us = 1000000;
   auto calls = std::make_shared<unsigned>(0);
   result.random = [calls, Local](std::span<uint8_t> Bytes) {
      std::fill(Bytes.begin(), Bytes.end(), uint8_t(Local));
      if (++*calls IS 2 and Bytes.size() IS 8) {
         Bytes[4] = 255; Bytes[5] = 255; Bytes[6] = 255; Bytes[7] = 254;
      }
      return true;
   };
   return result;
}
struct Pair {
   SctpAssociation a, b;
   SctpChannels ca{a, true}, cb{b, false};
   uint64_t now = 0;
   unsigned drop_data = 0, drop_forward = 0, drop_reset = 0, data_packets = 0, forwards = 0, resets = 0;
   Pair(SctpConfig A = config(5000, 5001), SctpConfig B = config(5001, 5000)) : a(A), b(B) {
      CHECK(a.start(now) IS ERR::Okay);
      for (int i = 0; i < 8; ++i) wire();
      CHECK(a.state() IS SctpState::ESTABLISHED and b.state() IS SctpState::ESTABLISHED);
      CHECK(a.peer().forward_tsn and a.peer().reconfig);
   }
   void wire() {
      for (auto &packet : a.take_output()) {
         if (packet[12] IS DATA) { ++data_packets; if (drop_data) { --drop_data; continue; } }
         if (packet[12] IS FORWARD_TSN) { ++forwards; if (drop_forward) { --drop_forward; continue; } }
         if (packet[12] IS RE_CONFIG) { ++resets; if (drop_reset) { --drop_reset; continue; } }
         CHECK(b.feed(packet, now) IS ERR::Okay);
      }
      for (auto &packet : b.take_output()) {
         if (packet[12] IS RE_CONFIG) { ++resets; if (drop_reset) { --drop_reset; continue; } }
         CHECK(a.feed(packet, now) IS ERR::Okay);
      }
   }
   void step(bool Channels = false) {
      now += 1000;
      CHECK(a.tick(now) IS ERR::Okay); CHECK(b.tick(now) IS ERR::Okay);
      wire();
      if (Channels) { CHECK(ca.pump(now) IS ERR::Okay); CHECK(cb.pump(now) IS ERR::Okay); }
   }
   void run(unsigned Steps, bool Channels = false) { for (unsigned i = 0; i < Steps; ++i) step(Channels); }
};
std::vector<uint8_t> packet(SctpAssociation &Sender, uint8_t Type, std::span<const uint8_t> Value) {
   Encoder encoder(5000, 5001, Sender.peer().tag, 1100);
   CHECK(encoder.chunk(Type, 0, Value)); return encoder.finish();
}
void codecs() {
   SctpChannelOptions options;
   options.label = "chat\xe2\x82\xac"; options.protocol = "test";
   for (unsigned mode = 0; mode < 6; ++mode) {
      options.ordered = mode < 3;
      options.reliability = {};
      if (mode % 3 IS 1) options.reliability.max_retransmits = 0;
      if (mode % 3 IS 2) options.reliability.max_lifetime_ms = 65535;
      std::vector<uint8_t> wire;
      CHECK(dcep::encode_open(options, wire) IS ERR::Okay);
      CHECK(wire[1] IS uint8_t((mode < 3 ? 0 : 128) | (mode % 3)));
      auto invalid_type = wire; invalid_type[1] = 0x7f;
      SctpChannelOptions invalid;
      CHECK(dcep::decode_open(invalid_type, invalid) IS ERR::InvalidData);
      SctpChannelOptions decoded;
      CHECK(dcep::decode_open(wire, decoded) IS ERR::Okay);
      CHECK(decoded.label IS options.label and decoded.protocol IS options.protocol);
      CHECK(decoded.reliability.max_retransmits IS options.reliability.max_retransmits);
      CHECK(decoded.reliability.max_lifetime_ms IS options.reliability.max_lifetime_ms);
      for (size_t i = 0; i < wire.size(); ++i) {
         CHECK(dcep::decode_open(std::span(wire).first(i), decoded) != ERR::Okay);
      }
      wire.push_back(0); CHECK(dcep::decode_open(wire, decoded) IS ERR::InvalidData);
   }
   options.reliability.max_retransmits = 0; options.reliability.max_lifetime_ms = 0;
   CHECK(dcep::validate(options) IS ERR::InvalidValue);
   options.reliability = {}; options.label = "\xc0\x80";
   CHECK(dcep::validate(options) IS ERR::InvalidData);
   for (auto bytes : {std::vector<uint8_t>{0xed, 0xa0, 0x80}, std::vector<uint8_t>{0xf4, 0x90, 0x80, 0x80},
       std::vector<uint8_t>{0xe2, 0x82}, std::vector<uint8_t>{0xff}}) CHECK(not dcep::utf8(bytes));
   CHECK(dcep::utf8(std::vector<uint8_t>{0, 0xf0, 0x9f, 0x98, 0x80}));
   bool text = false;
   std::vector<uint8_t> empty{123};
   CHECK(dcep::decode_payload(56, empty, text) IS ERR::Okay and text and empty.empty());
   empty = {0}; CHECK(dcep::decode_payload(57, empty, text) IS ERR::Okay and not text and empty.empty());
   CHECK(dcep::decode_payload(52, empty, text) IS ERR::NoSupport);
}
void partial_reliability() {
   for (unsigned mode = 0; mode < 6; ++mode) {
      Pair pair;
      SctpReliability policy;
      if (mode < 3) policy.max_retransmits = mode;
      else policy.max_lifetime_ms = mode IS 3 ? 0 : mode IS 4 ? 5 : 50;
      pair.drop_data = mode IS 3 ? 0 : 100;
      pair.drop_forward = 2;
      std::vector<uint8_t> abandoned(1000, 1), later{7, 8, 9};
      CHECK(pair.a.enqueue(0, 53, abandoned, false, pair.now, policy) IS ERR::Okay);
      pair.run(100);
      pair.drop_data = 0;
      CHECK(pair.a.enqueue(0, 53, later, false, pair.now) IS ERR::Okay);
      pair.run(500);
      auto messages = pair.b.take_messages();
      CHECK(messages.size() IS 1);
      if (messages.size() IS 1) CHECK(messages[0].payload IS later);
      CHECK(pair.a.diagnostics().retained_bytes IS 0);
      CHECK(pair.a.diagnostics().flight_size IS 0);
      CHECK(pair.a.diagnostics().unsent_bytes IS 0);
      CHECK(pair.forwards >= 3);
      if (mode IS 3) CHECK(pair.data_packets IS 1); // Only the later reliable message was sent.
   }
   // Lose a middle fragment, accept its siblings, and abandon every fragment of the message.
   Pair pair;
   SctpReliability policy; policy.max_retransmits = 0;
   CHECK(pair.a.enqueue(0, 53, std::vector<uint8_t>(300, 2), false, 0, policy) IS ERR::Okay);
   auto wires = pair.a.take_output();
   CHECK(wires.size() IS 3);
   if (wires.size() IS 3) {
      CHECK(pair.b.feed(wires[0], 0) IS ERR::Okay);
      CHECK(pair.b.feed(wires[2], 0) IS ERR::Okay);
   }
   pair.run(300);
   CHECK(pair.b.take_messages().empty());
   CHECK(pair.b.diagnostics().receive_bytes IS 0);
   CHECK(pair.a.diagnostics().retained_bytes IS 0);
   CHECK(pair.a.enqueue(0, 51, std::vector<uint8_t>{42}, false, pair.now) IS ERR::Okay);
   pair.run(50);
   CHECK(pair.b.take_messages().size() IS 1);
}
void queued_expiry_and_credit() {
   Pair pair;
   std::vector<uint8_t> reliable(2000, 3), later{9};
   CHECK(pair.a.enqueue(0, 53, reliable, false, 0) IS ERR::Okay);
   SctpReliability policy; policy.max_lifetime_ms = 0;
   CHECK(pair.a.enqueue(1, 53, later, false, 0, policy) IS ERR::Okay);
   CHECK(pair.a.enqueue(1, 53, later, false, 0) IS ERR::Okay);
   pair.run(200);
   auto messages = pair.b.take_messages();
   CHECK(messages.size() IS 2);
   if (messages.size() IS 2) {
      CHECK(messages[0].payload IS reliable);
      CHECK(messages[1].payload IS later and messages[1].stream IS 1);
   }
   CHECK(pair.a.diagnostics().retained_bytes IS 0 and pair.b.diagnostics().receive_bytes IS 0);
   Pair count;
   policy = {}; policy.max_retransmits = 1; count.drop_data = 100;
   CHECK(count.a.enqueue(0, 53, later, false, 0, policy) IS ERR::Okay);
   count.run(100);
   CHECK(count.a.diagnostics().retransmissions IS 1);
   CHECK(count.b.take_messages().empty());
   Pair credit;
   SctpChannelOptions options; options.negotiated = true; options.id = 10;
   uint16_t id = 0;
   CHECK(credit.ca.open(options, 0, id) IS ERR::Okay);
   CHECK(credit.cb.open(options, 0, id) IS ERR::Okay);
   (void)credit.ca.take_events(); (void)credit.cb.take_events();
   CHECK(credit.ca.send(id, reliable, false, 0) IS ERR::Okay);
   CHECK(credit.ca.send(id, {}, true, 0) IS ERR::Okay);
   credit.run(200, true);
   CHECK(credit.b.diagnostics().receive_bytes IS reliable.size() + 1);
   CHECK(credit.cb.take_events().size() IS 2);
   CHECK(credit.b.diagnostics().receive_bytes IS 0);
}
void maximum_dcep_labels() {
   auto a = config(5000, 5001), b = config(5001, 5000);
   a.receive_limit = b.receive_limit = 1;
   Pair pair(a, b);
   SctpChannelOptions options; options.label.assign(65535, 'l'); options.protocol.assign(65535, 'p');
   uint16_t id = 0;
   CHECK(pair.ca.open(options, 0, id) IS ERR::Okay);
   for (unsigned i = 0; i < 10000 and pair.ca.channel(id)->state != SctpChannelState::OPEN; ++i) pair.step(true);
   CHECK(pair.ca.channel(id)->state IS SctpChannelState::OPEN);
   CHECK(pair.cb.channel(id) and pair.cb.channel(id)->options.label IS options.label);
   CHECK(pair.cb.channel(id) and pair.cb.channel(id)->options.protocol IS options.protocol);
}
void partial_sequence_wrap() {
   Pair pair;
   SctpReliability policy; policy.max_lifetime_ms = 0;
   for (unsigned i = 0; i < 65537; ++i) {
      CHECK(pair.a.enqueue(0, 53, std::vector<uint8_t>{1}, false, pair.now, policy) IS ERR::Okay);
      pair.run(2);
   }
   CHECK(pair.a.enqueue(0, 53, std::vector<uint8_t>{9}, false, pair.now) IS ERR::Okay);
   pair.run(100);
   auto messages = pair.b.take_messages();
   CHECK(messages.size() IS 1);
   if (messages.size() IS 1) CHECK(messages[0].payload IS std::vector<uint8_t>{9});
}
void lifecycle() {
   for (unsigned mode = 0; mode < 6; ++mode) {
      Pair pair;
      SctpChannelOptions options; options.label = "channel"; options.protocol = "binary-test";
      options.ordered = mode < 3;
      if (mode % 3 IS 1) options.reliability.max_retransmits = 0;
      if (mode % 3 IS 2) options.reliability.max_lifetime_ms = 1000;
      uint16_t id = 99;
      pair.drop_data = 1; // OPEN retransmits reliably even on MaxRetransmits=0 channels.
      CHECK(pair.ca.open(options, 0, id) IS ERR::Okay and id IS 0);
      pair.run(100, true);
      CHECK(pair.ca.channel(id) and pair.ca.channel(id)->state IS SctpChannelState::OPEN);
      CHECK(pair.cb.channel(id) and pair.cb.channel(id)->state IS SctpChannelState::OPEN);
      auto remote = pair.cb.take_events();
      CHECK(remote.size() IS 2);
      if (remote.size() IS 2) {
         CHECK(remote[0].type IS SctpChannelEventType::REMOTE_OPEN);
         CHECK(remote[1].type IS SctpChannelEventType::OPEN);
      }
      CHECK(pair.ca.take_events().size() IS 1);
      CHECK(pair.ca.send(id, {}, true, pair.now) IS ERR::Okay);
      CHECK(pair.ca.send(id, {}, false, pair.now) IS ERR::Okay);
      std::vector<uint8_t> binary{0, 255, 1, 0}, text{'h', 'i'};
      CHECK(pair.ca.send(id, text, true, pair.now) IS ERR::Okay);
      CHECK(pair.cb.send(id, binary, false, pair.now) IS ERR::Okay);
      CHECK(pair.ca.send(id, binary, true, pair.now) IS ERR::InvalidData);
      pair.run(100, true);
      auto a = pair.ca.take_events(), b = pair.cb.take_events();
      CHECK(a.size() IS 1 and b.size() IS 3);
      if (a.size() IS 1) CHECK(a[0].payload IS binary and not a[0].text);
      if (b.size() IS 3) {
         CHECK(b[0].payload.empty() and b[0].text);
         CHECK(b[1].payload.empty() and not b[1].text);
         CHECK(b[2].payload IS text and b[2].text);
      }
      pair.drop_reset = 3;
      CHECK(pair.ca.close(id, pair.now) IS ERR::Okay);
      CHECK(pair.ca.close(id, pair.now) IS ERR::Okay);
      CHECK(pair.ca.send(id, text, true, pair.now) IS ERR::InvalidState);
      if (mode & 1) CHECK(pair.cb.close(id, pair.now) IS ERR::Okay);
      pair.run(700, true);
      CHECK(pair.ca.channel(id)->state IS SctpChannelState::CLOSED);
      CHECK(pair.cb.channel(id)->state IS SctpChannelState::CLOSED);
      options.id = id;
      CHECK(pair.ca.open(options, pair.now, id) IS ERR::InvalidValue);
      a = pair.ca.take_events(); b = pair.cb.take_events();
      CHECK(a.size() IS 1 and b.size() IS 1);
      if (a.size() IS 1) CHECK(a[0].type IS SctpChannelEventType::CLOSE);
      // Reuse the exact ID, with SSN=0 again, only after both directions complete.
      options.id = id;
      CHECK(pair.ca.open(options, pair.now, id) IS ERR::Okay);
      pair.run(100, true);
      CHECK(pair.ca.channel(id)->state IS SctpChannelState::OPEN);
   }
}
void allocation_and_negotiated() {
   Pair pair;
   SctpChannelOptions options; uint16_t even = 99, odd = 99;
   CHECK(pair.ca.open(options, 0, even) IS ERR::Okay and even IS 0);
   CHECK(pair.cb.open(options, 0, odd) IS ERR::Okay and odd IS 1);
   pair.run(100, true);
   options.id = even;
   CHECK(pair.ca.open(options, pair.now, even) IS ERR::InvalidValue);
   options.id = 3;
   CHECK(pair.ca.open(options, pair.now, even) IS ERR::InvalidValue);
   options.id = 512;
   CHECK(pair.cb.open(options, pair.now, odd) IS ERR::OutOfRange);
   options.negotiated = true; options.id = 12;
   CHECK(pair.ca.open(options, pair.now, even) IS ERR::Okay);
   CHECK(pair.cb.open(options, pair.now, odd) IS ERR::Okay);
   (void)pair.ca.take_events(); (void)pair.cb.take_events();
   CHECK(pair.ca.send(even, std::vector<uint8_t>{4}, false, pair.now) IS ERR::Okay);
   pair.run(100, true);
   auto events = pair.cb.take_events(); CHECK(events.size() IS 1);
   if (events.size() IS 1) CHECK(events[0].payload IS std::vector<uint8_t>{4});
   for (unsigned i = 0; i < 100; ++i) {
      CHECK(pair.ca.close(12, pair.now) IS ERR::Okay);
      pair.run(50, true);
      (void)pair.ca.take_events(); (void)pair.cb.take_events();
      CHECK(not pair.ca.channel(12) and not pair.cb.channel(12));
      CHECK(pair.ca.open(options, pair.now, even) IS ERR::Okay);
      CHECK(pair.cb.open(options, pair.now, odd) IS ERR::Okay);
   }
}
void opening_data_and_errors() {
   Pair pair; uint16_t id = 0; SctpChannelOptions options;
   CHECK(pair.ca.open(options, 0, id) IS ERR::Okay);
   pair.wire(); CHECK(pair.cb.pump(0) IS ERR::Okay);
   // Native accepts pre-ACK user data from the remote endpoint but defers its event until OPEN.
   auto ack_wires = pair.b.take_output();
   CHECK(pair.b.enqueue(id, 53, std::vector<uint8_t>{8}, true, 0) IS ERR::Okay);
   for (auto &wire : pair.b.take_output()) CHECK(pair.a.feed(wire, 0) IS ERR::Okay);
   CHECK(pair.ca.pump(0) IS ERR::Okay);
   CHECK(pair.ca.take_events().empty());
   for (auto &wire : ack_wires) CHECK(pair.a.feed(wire, 0) IS ERR::Okay);
   CHECK(pair.ca.pump(0) IS ERR::Okay);
   auto events = pair.ca.take_events(); CHECK(events.size() IS 2);
   if (events.size() IS 2) {
      CHECK(events[0].type IS SctpChannelEventType::OPEN);
      CHECK(events[1].type IS SctpChannelEventType::MESSAGE);
   }
   CHECK(pair.b.enqueue(id, 52, std::vector<uint8_t>{0}, false, 0) IS ERR::Okay);
   pair.run(100, true);
   events = pair.ca.take_events();
   CHECK(events.size() IS 2);
   if (events.size() IS 2) {
      CHECK(events[0].type IS SctpChannelEventType::ERROR and events[0].error IS ERR::NoSupport);
      CHECK(events[1].type IS SctpChannelEventType::CLOSE);
   }
}
void control_reservation() {
   Pair pair;
   std::vector<uint8_t> payload(16 * 1024 * 1024, 1);
   CHECK(pair.a.enqueue(0, 53, payload, false, 0) IS ERR::Okay);
   CHECK(pair.a.enqueue(2, 53, payload, false, 0) IS ERR::Okay);
   std::vector<uint8_t> ack{2};
   CHECK(pair.a.enqueue(4, 53, ack, false, 0) IS ERR::BufferOverflow);
   CHECK(pair.a.enqueue(4, 50, ack, false, 0) IS ERR::Okay);
   CHECK(pair.a.diagnostics().retained_bytes IS 32 * 1024 * 1024 + 1);
}
void quota_and_close_timeout() {
   Pair pair;
   SctpChannelOptions options; options.negotiated = true;
   uint16_t id = 0;
   for (unsigned i = 0; i < 256; ++i) {
      options.id = uint16_t(i);
      CHECK(pair.ca.open(options, pair.now, id) IS ERR::Okay);
   }
   options.id = 256;
   CHECK(pair.ca.open(options, pair.now, id) IS ERR::BufferOverflow);
   (void)pair.ca.take_events();
   CHECK(pair.ca.close(0, pair.now) IS ERR::Okay);
   (void)pair.a.take_output(); // Peer never receives the reset request.
   for (unsigned i = 0; i < 6000 and pair.a.state() != SctpState::FAILED; ++i) {
      pair.now += 1000;
      auto error = pair.a.tick(pair.now);
      CHECK(error IS ERR::Okay or error IS ERR::Timeout);
      (void)pair.a.take_output();
   }
   CHECK(pair.a.state() IS SctpState::FAILED and pair.a.error() IS ERR::Timeout);
   CHECK(pair.ca.pump(pair.now) IS ERR::Timeout);
   auto events = pair.ca.take_events();
   CHECK(events.size() IS 512);
   CHECK(events[0].type IS SctpChannelEventType::ERROR and events[1].type IS SctpChannelEventType::CLOSE);
   CHECK(pair.ca.pump(pair.now) IS ERR::Timeout and pair.ca.take_events().empty());
}
// Exercise metadata limits independently of payload byte limits, without ACKs releasing reservations.
void resource_boundaries() {
   const std::vector<uint8_t> byte{1};
   {
      Pair pair;
      for (unsigned i = 0; i < 4096; ++i) {
         CHECK(pair.a.enqueue(uint16_t(i / 1024), 53, byte, false, 0) IS ERR::Okay);
         if (i IS 1023) CHECK(pair.a.enqueue(0, 53, byte, false, 0) IS ERR::BufferOverflow);
      }
      CHECK(pair.a.enqueue(4, 53, byte, false, 0) IS ERR::BufferOverflow);
      CHECK(pair.a.diagnostics().retained_bytes IS 4096);
      CHECK(pair.a.enqueue(4, 50, byte, false, 0) IS ERR::Okay);
   }
   {
      Pair pair;
      for (unsigned i = 0; i < 512; ++i) CHECK(pair.a.enqueue(0, 50, byte, false, 0) IS ERR::Okay);
      CHECK(pair.a.enqueue(1, 50, byte, false, 0) IS ERR::BufferOverflow);
      CHECK(pair.a.diagnostics().retained_bytes IS 512);
      CHECK(pair.a.enqueue(1, 53, byte, false, 0) IS ERR::Okay);
   }
   {
      Pair pair;
      std::vector<uint8_t> control(65536, 1);
      for (unsigned i = 0; i < 4; ++i) CHECK(pair.a.enqueue(0, 50, control, false, 0) IS ERR::Okay);
      CHECK(pair.a.enqueue(1, 50, byte, false, 0) IS ERR::BufferOverflow);
      CHECK(pair.a.diagnostics().retained_bytes IS 256 * 1024);
   }
   {
      Pair pair;
      SctpChannelOptions options; options.negotiated = true;
      options.label.assign(65535, 'x');
      uint16_t assigned = 0;
      for (unsigned i = 0; i < 4; ++i) {
         options.id = uint16_t(i);
         CHECK(pair.ca.open(options, 0, assigned) IS ERR::Okay);
      }
      options.id = 4; options.label = "xxxx";
      CHECK(pair.ca.open(options, 0, assigned) IS ERR::Okay);
      options.id = 5; options.label = "x";
      CHECK(pair.ca.open(options, 0, assigned) IS ERR::BufferOverflow);
      CHECK(not pair.ca.channel(5));
      options.label.clear();
      CHECK(pair.ca.open(options, 0, assigned) IS ERR::Okay);
   }
}
void extension_gating() {
   SctpAssociation a(config(5000, 5001)), b(config(5001, 5000));
   CHECK(a.start(0) IS ERR::Okay);
   auto output = a.take_output();
   Packet parsed; CHECK(parse(output[0], parsed));
   // Simulate an INIT from a legacy peer without either extension advertisement.
   Encoder initial(5000, 5001, 0, 1100);
   CHECK(initial.chunk(INIT, 0, parsed.chunks[0].value.first(16)));
   CHECK(b.feed(initial.finish(), 0) IS ERR::Okay);
   CHECK(a.feed(b.take_output()[0], 0) IS ERR::Okay);
   CHECK(b.feed(a.take_output()[0], 0) IS ERR::Okay);
   CHECK(a.feed(b.take_output()[0], 0) IS ERR::Okay);
   CHECK(not b.peer().forward_tsn and not b.peer().reconfig);
   CHECK(b.require_extensions(false, false) IS ERR::Okay);
   CHECK(b.require_extensions(true, false) IS ERR::NoSupport);
   CHECK(b.require_extensions(false, true) IS ERR::NoSupport);
   SctpReliability policy; policy.max_retransmits = 0;
   CHECK(b.enqueue(0, 53, std::vector<uint8_t>{1}, false, 0, policy) IS ERR::NoSupport);
   CHECK(b.reset_stream(0, 0) IS ERR::NoSupport);
   SctpChannels channels(b, false); SctpChannelOptions options; uint16_t id = 0;
   CHECK(channels.open(options, 0, id) IS ERR::NoSupport);
   CHECK(b.enqueue(0, 53, std::vector<uint8_t>{1}, false, 0) IS ERR::Okay);
}
void reset_denial_and_sequence() {
   Pair denied;
   SctpChannelOptions options; options.negotiated = true; options.id = 0;
   uint16_t id = 0;
   CHECK(denied.ca.open(options, 0, id) IS ERR::Okay);
   (void)denied.ca.take_events();
   CHECK(denied.ca.close(id, 0) IS ERR::Okay);
   std::vector<uint8_t> response, params;
   put32(response, denied.a.initial_tsn()); put32(response, 2);
   CHECK(append_parameter(params, 16, response));
   Encoder wire(5001, 5000, denied.a.tag(), 1100);
   CHECK(wire.chunk(RE_CONFIG, 0, params));
   CHECK(denied.a.feed(wire.finish(), 0) IS ERR::NoSupport);
   CHECK(denied.ca.pump(0) IS ERR::NoSupport);
   auto events = denied.ca.take_events();
   CHECK(events.size() IS 2);
   if (events.size() IS 2) {
      CHECK(events[0].type IS SctpChannelEventType::ERROR and events[0].error IS ERR::NoSupport);
      CHECK(events[1].type IS SctpChannelEventType::CLOSE);
   }
   Pair sequence;
   std::vector<uint8_t> request; params.clear();
   put32(request, sequence.a.initial_tsn() + 1); put32(request, sequence.b.initial_tsn() - 1);
   put32(request, sequence.a.initial_tsn() - 1); put16(request, 0);
   CHECK(append_parameter(params, 13, request));
   CHECK(sequence.b.feed(packet(sequence.a, RE_CONFIG, params), 0) IS ERR::Okay);
   CHECK(sequence.b.take_resets().empty());
   bool rejected = false;
   for (const auto &output : sequence.b.take_output()) {
      Packet parsed; CHECK(parse(output, parsed));
      for (const auto &chunk : parsed.chunks) {
         if (chunk.type != RE_CONFIG) continue;
         std::vector<Item> values; CHECK(parameters(chunk.value, values));
         if (values.size() IS 1 and read32(values[0].value, 4) IS 5) rejected = true;
      }
   }
   CHECK(rejected);
}
void reset_barrier() {
   Pair pair;
   CHECK(pair.a.enqueue(0, 53, std::vector<uint8_t>(300, 6), false, 0) IS ERR::Okay);
   pair.drop_data = 1;
   pair.wire();
   std::vector<uint8_t> value, params;
   put32(value, pair.a.initial_tsn()); put32(value, pair.b.initial_tsn() - 1);
   put32(value, pair.a.initial_tsn() + 2); put16(value, 0);
   CHECK(append_parameter(params, 13, value));
   auto request = packet(pair.a, RE_CONFIG, params);
   CHECK(pair.b.feed(request, 0) IS ERR::Okay);
   CHECK(pair.b.take_resets().empty());
   std::vector<uint8_t> next;
   put32(next, pair.a.initial_tsn() + 3); put16(next, 0); put16(next, 0); put32(next, 53); next.push_back(9);
   Encoder future(5000, 5001, pair.b.tag(), 1100);
   CHECK(future.chunk(DATA, 3, next));
   CHECK(pair.b.feed(future.finish(), 0) IS ERR::Okay);
   CHECK(pair.b.take_messages().empty()); // SSN=0 of the new generation is held behind the old TSN barrier.
   pair.run(200);
   auto messages = pair.b.take_messages();
   CHECK(messages.size() IS 2);
   if (messages.size() IS 2) {
      CHECK(messages[0].payload.size() IS 300);
      CHECK(messages[1].payload IS std::vector<uint8_t>{9});
   }
   auto resets = pair.b.take_resets();
   CHECK(std::any_of(resets.begin(), resets.end(), [](const SctpResetEvent &Reset) { return Reset.incoming; }));
   CHECK(pair.b.feed(request, pair.now) IS ERR::Okay); // Replays the successful response without resetting twice.
   CHECK(pair.b.take_resets().empty());
}
void adapter_accounting() {
   Pair pair;
   std::vector<uint8_t> bytes{0, 255, 7};
   CHECK(pair.a.enqueue(0, 53, bytes, false, pair.now) IS ERR::Okay);
   CHECK(pair.a.buffered_amounts()[0] IS bytes.size());
   auto output = pair.a.take_output();
   CHECK(not output.empty());
   for (const auto &wire : output) pair.a.output_accepted(wire);
   CHECK(pair.a.buffered_amounts()[0] IS 0);
   CHECK(pair.a.diagnostics().retained_bytes IS bytes.size());
   // Drop the accepted originals.  Retransmissions must not re-charge public buffering.
   pair.run(300);
   CHECK(pair.a.buffered_amounts()[0] IS 0);
   CHECK(pair.a.diagnostics().retained_bytes IS 0);
   CHECK(pair.b.take_messages().size() IS 1);
   Pair expiry;
   SctpReliability policy; policy.max_lifetime_ms = 1;
   CHECK(expiry.a.enqueue(0, 53, std::vector<uint8_t>(5000, 9), false, 0, policy) IS ERR::Okay);
   CHECK(expiry.a.buffered_amounts()[0] IS 5000);
   CHECK(expiry.a.tick(1000) IS ERR::Okay);
   CHECK(expiry.a.buffered_amounts()[0] IS 0);
   Pair control;
   CHECK(control.a.enqueue(0, 50, bytes, false, 0) IS ERR::Okay);
   CHECK(control.a.enqueue(1, 57, std::vector<uint8_t>{0}, false, 0) IS ERR::Okay);
   CHECK(control.a.buffered_amounts().empty());
}

}
int main() {
   codecs(); partial_reliability(); queued_expiry_and_credit(); partial_sequence_wrap();
   maximum_dcep_labels(); lifecycle(); allocation_and_negotiated();
   opening_data_and_errors(); control_reservation(); quota_and_close_timeout();
   resource_boundaries(); extension_gating(); reset_denial_and_sequence(); reset_barrier(); adapter_accounting();
   if (not glFailures) std::puts("SCTP partial reliability, DCEP and lifecycle checks passed");
   return glFailures ? 1 : 0;
}
