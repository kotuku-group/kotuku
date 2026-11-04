#include "../sctp/sctp_association.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>

using namespace rtc;
using namespace rtc::sctp;
namespace {
int glFailures = 0;
#define CHECK(Condition) do { if (not (Condition)) { \
   std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #Condition); ++glFailures; } } while (false)
SctpConfig config(uint16_t Local, uint16_t Remote, uint32_t TSN = 0xfffffffe) {
   SctpConfig result;
   result.local_port = Local; result.remote_port = Remote;
   result.initial_rto_us = 10000; result.min_rto_us = 1000; result.max_rto_us = 160000;
   result.delayed_ack_us = 2000; result.heartbeat_us = 200000;
   auto calls = std::make_shared<unsigned>(0);
   result.random = [calls, TSN, Local](std::span<uint8_t> Bytes) {
      std::fill(Bytes.begin(), Bytes.end(), uint8_t(Local));
      if (++*calls IS 2 and Bytes.size() IS 8) {
         for (int i = 0; i < 4; ++i) Bytes[4 + i] = uint8_t(TSN >> (24 - 8 * i));
      }
      return true;
   };
   return result;
}
std::vector<uint8_t> packet(SctpAssociation &Sender, uint8_t Type, std::span<const uint8_t> Value,
   uint8_t Flags = 0) {
   Encoder encoder(5000, 5001, Sender.peer().tag, 1100);
   CHECK(encoder.chunk(Type, Flags, Value));
   return encoder.finish();
}
struct Pair {
   SctpAssociation a, b;
   uint64_t now = 0;
   Pair(SctpConfig A = config(5000, 5001), SctpConfig B = config(5001, 5000)) : a(A), b(B) {
      CHECK(a.start(now) IS ERR::Okay);
      for (int i = 0; i < 8; ++i) pump();
      CHECK(a.state() IS SctpState::ESTABLISHED and b.state() IS SctpState::ESTABLISHED);
      (void)a.take_events(); (void)b.take_events();
   }
   void pump() {
      for (auto &wire : a.take_output()) CHECK(b.feed(wire, now) IS ERR::Okay);
      for (auto &wire : b.take_output()) CHECK(a.feed(wire, now) IS ERR::Okay);
   }
   void step() {
      now += 1000;
      CHECK(a.tick(now) IS ERR::Okay); CHECK(b.tick(now) IS ERR::Okay);
      pump();
   }
};
std::vector<uint8_t> payload(size_t Size) {
   std::vector<uint8_t> result(Size);
   for (size_t i = 0; i < Size; ++i) result[i] = uint8_t(i * 17 + i / 101);
   return result;
}
void impaired_delivery() {
   for (int mode = 0; mode < 7; ++mode) {
      Pair pair;
      if (mode IS 4) {
         for (int i = 0; i < 150; ++i) pair.step();
         CHECK(pair.a.diagnostics().path_mtu > 512);
      }
      std::deque<std::pair<uint64_t, std::vector<uint8_t>>> delayed;
      auto expected = payload(32768);
      CHECK(pair.a.enqueue(0, 53, expected, false, pair.now) IS ERR::Okay);
      CHECK(pair.a.enqueue(1, 51, payload(350), true, pair.now) IS ERR::Okay);
      unsigned packets = 0, ack_packets = 0;
      std::vector<SctpMessage> delivered;
      for (int i = 0; i < 40000 and delivered.size() < 2; ++i) {
         auto output = pair.a.take_output();
         if (mode IS 2) std::reverse(output.begin(), output.end());
         for (auto &wire : output) {
            ++packets;
            if (wire[12] IS DATA and ((mode IS 1 and packets % 11 IS 0) or
                (mode IS 3 and packets < 6))) continue;
            if (mode IS 4 and wire.size() > 512) continue;
            if (mode IS 6) { delayed.emplace_back(pair.now + 4000, std::move(wire)); continue; }
            CHECK(pair.b.feed(wire, pair.now) IS ERR::Okay);
            if (mode IS 2) CHECK(pair.b.feed(wire, pair.now) IS ERR::Okay);
         }
         while (not delayed.empty() and delayed.front().first <= pair.now) {
            CHECK(pair.b.feed(delayed.front().second, pair.now) IS ERR::Okay);
            delayed.pop_front();
         }
         for (auto &wire : pair.b.take_output()) {
            if (mode IS 4 and wire.size() > 512) continue;
            if (wire[12] IS SACK and mode IS 5) {
               ++ack_packets;
               if (ack_packets < 5 or ack_packets % 5 IS 0) continue;
            }
            CHECK(pair.a.feed(wire, pair.now) IS ERR::Okay);
         }
         for (auto &message : pair.b.take_messages()) delivered.push_back(std::move(message));
         pair.now += 1000;
         CHECK(pair.a.tick(pair.now) IS ERR::Okay); CHECK(pair.b.tick(pair.now) IS ERR::Okay);
      }
      if (delivered.size() != 2) {
         auto a = pair.a.diagnostics(), b = pair.b.diagnostics();
         std::fprintf(stderr, "mode=%d delivered=%zu retained=%zu unsent=%zu flight=%zu rx=%zu retry=%llu mtu=%zu\n",
            mode, delivered.size(), a.retained_bytes, a.unsent_bytes, a.flight_size, b.receive_bytes,
            (unsigned long long)a.retransmissions, a.path_mtu);
      }
      CHECK(delivered.size() IS 2);
      if (delivered.size() IS 2) {
         auto large = std::find_if(delivered.begin(), delivered.end(), [](const SctpMessage &Message) {
            return Message.stream IS 0;
         });
         CHECK(large != delivered.end());
         if (large != delivered.end()) CHECK(large->payload IS expected and not large->unordered and large->ppid IS 53);
      }
      for (int i = 0; i < 500; ++i) pair.step();
      CHECK(pair.a.diagnostics().retained_bytes IS 0 and pair.a.diagnostics().flight_size IS 0);
      CHECK(pair.b.diagnostics().receive_bytes IS 0 and pair.b.take_messages().empty());
      if (mode IS 0) CHECK(pair.a.diagnostics().congestion_window > 512);
      if (mode IS 1 or mode IS 3 or mode IS 4 or mode IS 5) CHECK(pair.a.diagnostics().retransmissions > 0);
   }
}
void ordering_and_fast_retransmit() {
   Pair pair;
   std::vector<std::vector<uint8_t>> wires;
   for (uint16_t stream : {uint16_t(0), uint16_t(1), uint16_t(2), uint16_t(3)}) {
      CHECK(pair.a.enqueue(stream, 51, payload(50), stream IS 3, 0) IS ERR::Okay);
      for (auto &wire : pair.a.take_output()) wires.push_back(std::move(wire));
   }
   CHECK(wires.size() IS 4);
   // Three distinct gap reports trigger fast retransmit without virtual-time timeout.
   for (size_t i = 1; i < wires.size(); ++i) {
      CHECK(pair.b.feed(wires[i], 0) IS ERR::Okay);
      for (auto &wire : pair.b.take_output()) CHECK(pair.a.feed(wire, 0) IS ERR::Okay);
   }
   CHECK(pair.a.diagnostics().fast_retransmissions IS 1);
   auto messages = pair.b.take_messages();
   CHECK(messages.size() IS 3); // Other streams and unordered DATA can deliver across a TSN gap.
   CHECK(pair.a.tick(0) IS ERR::Okay);
   pair.pump();
   messages = pair.b.take_messages();
   CHECK(messages.size() IS 1 and messages.front().stream IS 0);
   for (int i = 0; i < 20; ++i) pair.step();
   CHECK(pair.a.diagnostics().flight_size IS 0);
   // Same stream messages delivered strictly by SSN despite arrival order.
   CHECK(pair.a.enqueue(5, 51, payload(10), false, pair.now) IS ERR::Okay);
   auto first = pair.a.take_output();
   CHECK(pair.a.enqueue(5, 51, payload(20), false, pair.now) IS ERR::Okay);
   auto second = pair.a.take_output();
   for (auto &wire : second) CHECK(pair.b.feed(wire, pair.now) IS ERR::Okay);
   CHECK(pair.b.take_messages().empty());
   for (auto &wire : first) CHECK(pair.b.feed(wire, pair.now) IS ERR::Okay);
   messages = pair.b.take_messages();
   CHECK(messages.size() IS 2);
   if (messages.size() IS 2) CHECK(messages[0].payload.size() IS 10 and messages[1].payload.size() IS 20);
}
void zero_window_and_limits() {
   auto small = config(5001, 5000);
   small.receive_window = 1500; small.receive_limit = 1500;
   Pair pair(config(5000, 5001), small);
   auto expected = payload(1500);
   CHECK(pair.a.enqueue(0, 53, expected, false, 0) IS ERR::Okay);
   CHECK(pair.a.enqueue(1, 53, payload(100), false, 0) IS ERR::Okay);
   for (int i = 0; i < 50; ++i) pair.step();
   CHECK(pair.b.diagnostics().receive_bytes IS 1500);
   CHECK(pair.a.diagnostics().retained_bytes > 0);
   auto cwnd = pair.a.diagnostics().congestion_window;
   for (int i = 0; i < 50; ++i) pair.step();
   CHECK(pair.a.diagnostics().congestion_window >= cwnd);
   auto messages = pair.b.take_messages();
   CHECK(messages.size() IS 1 and messages.front().payload IS expected);
   // Drop the explicit window update.  A backoff DATA probe must recover it.
   (void)pair.b.take_output();
   for (int i = 0; i < 1000; ++i) pair.step();
   messages = pair.b.take_messages();
   CHECK(messages.size() IS 1 and messages.front().stream IS 1);
   CHECK(pair.a.enqueue(512, 51, expected, false, pair.now) IS ERR::InvalidValue);
   CHECK(pair.a.enqueue(0, 51, {}, false, pair.now) IS ERR::InvalidValue);
   auto before = pair.a.diagnostics().retained_bytes;
   CHECK(pair.a.enqueue(0, 53, payload(16 * 1024 * 1024 + 1), false, pair.now) IS ERR::OutOfRange);
   CHECK(pair.a.diagnostics().retained_bytes IS before);
}
void reordered_limit_and_timers() {
   auto small = config(5001, 5000);
   small.receive_window = 1500; small.receive_limit = 1500;
   Pair pair(config(5000, 5001), small);
   auto expected = payload(1500);
   CHECK(pair.a.enqueue(0, 53, expected, false, 0) IS ERR::Okay);
   auto first_flight = pair.a.take_output();
   CHECK(first_flight.size() > 1);
   auto beginning = std::move(first_flight.front());
   for (size_t i = 1; i < first_flight.size(); ++i) CHECK(pair.b.feed(first_flight[i], 0) IS ERR::Okay);
   CHECK(pair.b.take_messages().empty());
   pair.now = 1000;
   CHECK(pair.b.feed(beginning, pair.now) IS ERR::Okay);
   for (int i = 0; i < 100; ++i) pair.step();
   auto messages = pair.b.take_messages();
   CHECK(messages.size() IS 1 and messages.front().payload IS expected);

   Pair timing;
   CHECK(timing.a.enqueue(0, 51, payload(50), false, 0) IS ERR::Okay);
   auto output = timing.a.take_output();
   CHECK(output.size() IS 1);
   CHECK(timing.b.feed(output.front(), 1000) IS ERR::Okay);
   CHECK(timing.b.take_output().empty());
   CHECK(timing.b.next_deadline_us() IS 3000);
   CHECK(timing.b.tick(2999) IS ERR::Okay and timing.b.take_output().empty());
   CHECK(timing.b.tick(3000) IS ERR::Okay);
   auto ack = timing.b.take_output();
   CHECK(ack.size() IS 1 and ack.front()[12] IS SACK);
   CHECK(timing.a.feed(ack.front(), 4000) IS ERR::Okay);
   CHECK(timing.a.diagnostics().rto_us IS 12000); // First RTT 4 ms: SRTT + 4 * RTTVAR.
   CHECK(timing.a.enqueue(0, 51, payload(50), false, 4000) IS ERR::Okay);
   (void)timing.a.take_output();
   CHECK(timing.a.tick(16000) IS ERR::Okay);
   CHECK(timing.a.diagnostics().rto_us IS 24000);
   for (auto &wire : timing.a.take_output()) CHECK(timing.b.feed(wire, 17000) IS ERR::Okay);
   CHECK(timing.b.tick(19000) IS ERR::Okay);
   for (auto &wire : timing.b.take_output()) CHECK(timing.a.feed(wire, 20000) IS ERR::Okay);
   CHECK(timing.a.diagnostics().rto_us IS 24000); // Karn forbids sampling the retransmitted chunk.
   CHECK(timing.a.tick(19999) IS ERR::InvalidValue);

   Pair saturated;
   unsigned accepted = 0;
   while (accepted < 1100 and saturated.a.enqueue(0, 51, payload(1), false, 0) IS ERR::Okay) ++accepted;
   CHECK(accepted >= 1024 and accepted < 1100);
   auto before = saturated.a.diagnostics();
   CHECK(saturated.a.enqueue(0, 51, payload(1), false, 0) IS ERR::BufferOverflow);
   CHECK(saturated.a.diagnostics().retained_bytes IS before.retained_bytes);
   CHECK(saturated.a.shutdown(0) IS ERR::Okay);
   CHECK(saturated.a.tick(5000000) IS ERR::Timeout);
   output = saturated.a.take_output();
   CHECK(output.size() IS 1 and output.front()[12] IS ABORT);
}
void large_message_and_queue_budget() {
   auto a_config = config(5000, 5001), b_config = config(5001, 5000);
   a_config.receive_limit = b_config.receive_limit = 16 * 1024 * 1024;
   Pair pair(a_config, b_config);
   auto expected = payload(16 * 1024 * 1024);
   CHECK(pair.a.enqueue(0, 53, expected, false, 0) IS ERR::Okay);
   CHECK(pair.a.enqueue(1, 53, expected, true, 0) IS ERR::Okay);
   auto before = pair.a.diagnostics();
   CHECK(pair.a.enqueue(2, 53, payload(1), false, 0) IS ERR::BufferOverflow);
   CHECK(pair.a.diagnostics().retained_bytes IS before.retained_bytes);
   std::vector<SctpMessage> messages;
   for (int i = 0; i < 200000 and messages.size() < 2; ++i) {
      pair.step();
      for (auto &message : pair.b.take_messages()) messages.push_back(std::move(message));
   }
   CHECK(messages.size() IS 2);
   for (const auto &message : messages) CHECK(message.payload IS expected);
   for (int i = 0; i < 100; ++i) pair.step();
   CHECK(pair.a.diagnostics().retained_bytes IS 0);
}
void mtu_path_and_idle_failure() {
   Pair pair;
   for (int i = 0; i < 150; ++i) pair.step();
   CHECK(pair.a.diagnostics().path_mtu > 128);
   auto expected = payload(10000);
   CHECK(pair.a.enqueue(0, 53, expected, false, pair.now) IS ERR::Okay);
   CHECK(pair.a.path_changed(256, pair.now) IS ERR::Okay);
   CHECK(pair.a.diagnostics().flight_size <= pair.a.diagnostics().congestion_window);
   for (int i = 0; i < 1000; ++i) {
      for (auto &wire : pair.a.take_output()) {
         CHECK(wire.size() <= 256);
         CHECK(pair.b.feed(wire, pair.now) IS ERR::Okay);
      }
      for (auto &wire : pair.b.take_output()) CHECK(pair.a.feed(wire, pair.now) IS ERR::Okay);
      pair.now += 1000;
      CHECK(pair.a.tick(pair.now) IS ERR::Okay); CHECK(pair.b.tick(pair.now) IS ERR::Okay);
   }
   auto messages = pair.b.take_messages();
   CHECK(messages.size() IS 1 and messages.front().payload IS expected);
   CHECK(pair.a.path_changed(127, pair.now) IS ERR::OutOfRange);
   // Drop every response: idle failure is finite and never schedules an expired timer.
   for (int i = 0; i < 5000 and pair.a.state() != SctpState::FAILED; ++i) {
      (void)pair.a.take_output(); pair.now += 1000; (void)pair.a.tick(pair.now);
      auto due = pair.a.next_deadline_us();
      CHECK(not due or *due > pair.now);
   }
   CHECK(pair.a.state() IS SctpState::FAILED and pair.a.error() IS ERR::Timeout);
}
void shutdown_loss() {
   for (uint8_t drop : {uint8_t(SHUTDOWN), uint8_t(SHUTDOWN_ACK), uint8_t(SHUTDOWN_COMPLETE)}) {
      Pair pair;
      CHECK(pair.a.enqueue(0, 53, payload(4000), false, 0) IS ERR::Okay);
      CHECK(pair.a.shutdown(0) IS ERR::Okay);
      CHECK(pair.a.enqueue(0, 51, payload(1), false, 0) IS ERR::InvalidState);
      bool dropped = false;
      std::vector<SctpMessage> messages;
      for (int i = 0; i < 5000 and (pair.a.state() != SctpState::CLOSED or
           pair.b.state() != SctpState::CLOSED); ++i) {
         for (auto *source : {&pair.a, &pair.b}) {
            auto &target = source IS &pair.a ? pair.b : pair.a;
            for (auto &wire : source->take_output()) {
               if (not dropped and wire[12] IS drop) { dropped = true; continue; }
               auto error = target.feed(wire, pair.now);
               if (error != ERR::Okay) std::fprintf(stderr, "shutdown drop=%u type=%u error=%d state=%d now=%llu\n",
                  drop, wire[12], int(error), int(target.state()), (unsigned long long)pair.now);
               CHECK(error IS ERR::Okay);
            }
         }
         for (auto &message : pair.b.take_messages()) messages.push_back(std::move(message));
         pair.now += 1000;
         CHECK(pair.a.tick(pair.now) IS ERR::Okay); CHECK(pair.b.tick(pair.now) IS ERR::Okay);
      }
      CHECK(dropped and pair.a.state() IS SctpState::CLOSED and pair.b.state() IS SctpState::CLOSED);
      CHECK(messages.size() IS 1 and messages.front().payload IS payload(4000));
   }
   Pair pair;
   CHECK(pair.a.shutdown(0) IS ERR::Okay);
   (void)pair.a.take_output();
   CHECK(pair.a.tick(5000000) IS ERR::Timeout);
   auto output = pair.a.take_output();
   CHECK(output.size() IS 1 and output.front()[12] IS ABORT);
}
void congestion_avoidance() {
   Pair pair;
   auto expected = payload(1000);
   CHECK(pair.a.enqueue(0, 53, expected, false, 0) IS ERR::Okay);
   (void)pair.a.take_output(); // Lose the initial flight to establish a low slow-start threshold.
   pair.now = 10000;
   CHECK(pair.a.tick(pair.now) IS ERR::Okay);
   for (int i = 0; i < 128; ++i) CHECK(pair.a.enqueue(0, 53, expected, false, pair.now) IS ERR::Okay);
   unsigned delivered = 0;
   for (int i = 0; i < 10000 and delivered < 129; ++i) {
      pair.step();
      for (auto &message : pair.b.take_messages()) {
         CHECK(message.payload IS expected);
         ++delivered;
      }
   }
   CHECK(delivered IS 129);
   auto stats = pair.a.diagnostics();
   CHECK(stats.slow_start_updates > 0 and stats.congestion_avoidance_updates > 0);
}
void terminal_and_conflicting_data() {
   Pair simultaneous;
   CHECK(simultaneous.a.shutdown(0) IS ERR::Okay and simultaneous.b.shutdown(0) IS ERR::Okay);
   for (int i = 0; i < 20; ++i) simultaneous.step();
   CHECK(simultaneous.a.state() IS SctpState::CLOSED and simultaneous.b.state() IS SctpState::CLOSED);

   auto limited = config(5000, 5001);
   limited.max_retransmits = 2;
   Pair lost(limited);
   CHECK(lost.a.enqueue(0, 51, payload(100), false, 0) IS ERR::Okay);
   for (int i = 0; i < 1000 and lost.a.state() != SctpState::FAILED; ++i) {
      (void)lost.a.take_output();
      lost.now += 1000; (void)lost.a.tick(lost.now);
   }
   CHECK(lost.a.state() IS SctpState::FAILED and lost.a.error() IS ERR::Timeout);
   CHECK(not lost.a.next_deadline_us() and lost.a.diagnostics().retained_bytes IS 0);

   Pair invalid;
   auto make_data = [&](uint32_t TSN, uint32_t PPID, uint8_t Flags) {
      std::vector<uint8_t> value;
      put32(value, TSN); put16(value, 0); put16(value, 0); put32(value, PPID); value.push_back(1);
      return packet(invalid.a, DATA, value, Flags);
   };
   CHECK(invalid.b.feed(make_data(invalid.a.initial_tsn(), 51, 2), 0) IS ERR::Okay);
   CHECK(invalid.b.feed(make_data(invalid.a.initial_tsn() + 1, 53, 1), 0) IS ERR::InvalidData);
   CHECK(invalid.b.state() IS SctpState::FAILED and invalid.b.take_messages().empty());

   auto large_capacity = config(5001, 5000);
   large_capacity.packet_capacity = 65536;
   Pair control(config(5000, 5001), large_capacity);
   Encoder unknown(5000, 5001, control.a.peer().tag, 65536);
   CHECK(unknown.chunk(0x7f, 0, payload(60000)));
   auto report = unknown.finish();
   for (int i = 0; i < 4; ++i) CHECK(control.b.feed(report, 0) IS ERR::Okay);
   CHECK(control.b.feed(report, 0) IS ERR::BufferOverflow);
   CHECK(control.b.state() IS SctpState::FAILED);

   Pair overflow;
   CHECK(overflow.a.path_changed(1100, std::numeric_limits<uint64_t>::max() - 100) IS ERR::Timeout);
   CHECK(overflow.a.state() IS SctpState::FAILED and not overflow.a.next_deadline_us());
}
void sequence_wrap_and_invalid_sack() {
   Pair pair;
   for (int i = 0; i < 65538; ++i) {
      std::array<uint8_t, 1> value{uint8_t(i)};
      CHECK(pair.a.enqueue(0, 51, value, false, pair.now) IS ERR::Okay);
      pair.step();
      auto messages = pair.b.take_messages();
      CHECK(messages.size() IS 1 and messages.front().payload[0] IS uint8_t(i));
   }
   auto before = pair.b.diagnostics();
   std::vector<uint8_t> value;
   put32(value, pair.b.initial_tsn() + 100); put32(value, 0); put16(value, 1); put16(value, 0);
   put16(value, 2); put16(value, 1); // Invalid gap order; whole packet must be discarded.
   CHECK(pair.b.feed(packet(pair.a, SACK, value), pair.now) IS ERR::Okay);
   CHECK(pair.b.diagnostics().flight_size IS before.flight_size);
}
}
int main(int Argc, char **Argv) {
   if (Argc > 1 and std::string_view(Argv[1]) IS "shutdown") {
      shutdown_loss();
      return glFailures ? 1 : 0;
   }
   impaired_delivery(); ordering_and_fast_retransmit(); zero_window_and_limits();
   reordered_limit_and_timers(); large_message_and_queue_budget(); mtu_path_and_idle_failure();
   shutdown_loss(); congestion_avoidance(); terminal_and_conflicting_data(); sequence_wrap_and_invalid_sack();
   return glFailures ? 1 : 0;
}
