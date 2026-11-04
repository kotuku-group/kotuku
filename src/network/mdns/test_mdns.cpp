#include "mdns.h"
#include <algorithm>
#include <cstdio>

using namespace net::mdns;

static int fail(int Step) { std::fprintf(stderr, "mDNS failed at %d\n", Step); return Step; }

static Message answer(std::string Name, Address Address, uint32_t TTL = 120) {
   Message result; result.response = true;
   result.records.push_back({Name, Address, TTL});
   return result;
}

static bool saturated_queries(unsigned ScopeCount) {
   Engine engine;
   std::vector<Address> scopes;

   for (unsigned i = 0; i < ScopeCount; ++i) scopes.push_back({{192, 0, 2, 1}, i + 1});

   for (unsigned i = 0; i < 64; ++i) {
      if (not engine.resolve(std::to_string(i) + ".local", scopes, 0)) return false;
   }

   for (uint64_t time : {0, 500000, 1500000}) {
      std::vector<Packet> packets;
      for (unsigned batch = 0; batch < (64 * ScopeCount + 127) / 128; ++batch) {
         engine.tick(time);
         engine.tick(time); // A full queue must not consume retries or repeat already queued scopes.
         auto output = engine.take_output();
         if (output.size() > 128) return false;
         for (auto &packet : output) packets.push_back(std::move(packet));
      }

      if (packets.size() != 64 * ScopeCount) return false;

      std::array<std::array<unsigned, 32>, 64> counts{};

      for (const auto &packet : packets) {
         Message message;
         if (not decode(packet.bytes, message) or message.questions.size() != 1 or
             packet.interface_index < 1 or packet.interface_index > ScopeCount or packet.ipv6) return false;
         bool found = false;
         for (unsigned i = 0; i < 64; ++i) {
            if (message.questions[0].name != std::to_string(i) + ".local") continue;
            ++counts[i][packet.interface_index - 1];
            found = true;
            break;
         }
         if (not found) return false;
      }

      for (const auto &count : counts) {
         for (unsigned i = 0; i < ScopeCount; ++i) if (count[i] != 1) return false;
      }

      engine.tick(time + 499999);
      if (not engine.take_output().empty()) return false;
   }

   engine.tick(3000000);
   return not engine.pending() and engine.take_results().size() IS 64;
}

int main() {
   Address a{{192, 0, 2, 1}, 2}, b{{192, 0, 2, 2}, 2};
   Address v6{{0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 2, true};
   if (not valid_name("1234-AbCd.local") or valid_name("a.b.local") or valid_name("-a.local") or
       valid_name("a.local.") or valid_name("example.com") or
       valid_name(std::string(64, 'a') + ".local")) return fail(1);
   Message parsed;
   auto wire = encode(answer("random.local", a));
   if (not decode(wire, parsed) or parsed.records.size() != 1 or
       parsed.records[0].address.bytes != a.bytes) return fail(2);
   auto both = answer("random.local", a); both.records.push_back({"random.local", v6});
   if (not decode(encode(both), parsed) or parsed.records.size() != 2 or not parsed.records[1].address.ipv6) {
      return fail(3);
   }
   // Independent compressed owner fixture: second answer points at the first owner at byte 12.
   auto compressed = wire; compressed[7] = 2;
   compressed.insert(compressed.end(), {0xc0, 0x0c, 0, 1, 0x80, 1, 0, 0, 0, 120, 0, 4, 192, 0, 2, 2});
   if (not decode(compressed, parsed) or parsed.records.size() != 2) return fail(4);
   for (size_t length = 0; length < wire.size(); ++length) {
      if (decode(std::span(wire).first(length), parsed)) return fail(5);
   }
   auto loop = wire; loop[12] = 0xc0; loop[13] = 12;
   if (decode(loop, parsed)) return fail(6);
   loop = wire; loop[12] = 0xc0; loop[13] = 14; loop[14] = 0xc0; loop[15] = 12;
   if (decode(loop, parsed)) return fail(7);
   loop = wire; loop[12] = 0xff; loop[13] = 0xff;
   if (decode(loop, parsed)) return fail(8);
   loop = wire; loop.push_back(0);
   if (decode(loop, parsed)) return fail(9);
   loop = wire; loop[4] = 1;
   if (decode(loop, parsed)) return fail(10);
   Engine engine;
   if (not engine.register_name("random.local", a, 1000000) or
       engine.register_name("random.local", b, 1000000)) return fail(11);
   for (uint64_t time = 1000000; time <= 2250000; time += 250000) engine.tick(time);
   auto outgoing = engine.take_output();
   unsigned probes = 0, responses = 0;
   for (const auto &packet : outgoing) {
      if (not decode(packet.bytes, parsed)) return fail(12);
      if (parsed.response) ++responses; else { ++probes; if (parsed.records[0].section != 1) return fail(12); }
   }
   if (probes != 3 or not responses or engine.registrations()[0].status != Status::READY) return fail(13);
   Message query; query.questions.push_back({"random.local", 1});
   engine.receive(encode(query), 2, false, 5353, 255, 3000000);
   if (engine.take_output().size() != 1) return fail(14);
   query.records = answer("random.local", a).records;
   engine.receive(encode(query), 2, false, 5353, 255, 4000000);
   if (not engine.take_output().empty()) return fail(15); // Known-answer suppression.
   engine.withdraw(); outgoing = engine.take_output();
   if (outgoing.size() != 1 or not decode(outgoing[0].bytes, parsed) or parsed.records[0].ttl != 0 or
       not engine.registrations().empty()) return fail(16);
   std::array<Address, 2> scopes{a, v6};
   if (not engine.resolve("remote.local", scopes, 5000000)) return fail(17);
   auto reply = encode(answer("remote.local", a));
   engine.receive(reply, 3, false, 5353, 255, 5010000); // Wrong interface.
   engine.receive(reply, 2, false, 1234, 255, 5010000); // Non-mDNS source.
   engine.receive(reply, 2, false, 5353, 254, 5010000); // Routed/off-link source.
   engine.receive(encode(answer("unsolicited.local", a)), 2, false, 5353, 255, 5010000);
   engine.tick(5200000);
   if (not engine.take_results().empty()) return fail(18);
   auto replies = answer("remote.local", a, 1); replies.records.push_back({"remote.local", v6, 1});
   for (int i = 0; i < 20; ++i) engine.receive(encode(replies), 2, false, 5353, 255, 5210000);
   engine.tick(5310000);
   auto results = engine.take_results();
   if (results.size() != 1 or results[0].addresses.size() != 2) return fail(19);
   if (not engine.resolve("remote.local", scopes, 5400000)) return fail(20);
   engine.tick(5500000); results = engine.take_results();
   if (results.size() != 1 or results[0].addresses.size() != 2) return fail(21); // Scoped live cache.
   if (not engine.resolve("remote.local", scopes, 7000000)) return fail(22);
   engine.tick(7100000); if (not engine.take_results().empty()) return fail(23); // TTL expired.
   engine.tick(10000000); results = engine.take_results();
   if (results.size() != 1 or not results[0].addresses.empty()) return fail(24);
   if (not engine.resolve("remote.local", scopes, 11000000)) return fail(25);
   auto flooded = answer("remote.local", a);
   for (int i = 2; i < 30; ++i) {
      auto next = a; next.bytes[3] = uint8_t(i); flooded.records.push_back({"remote.local", next});
   }
   engine.receive(encode(flooded), 2, false, 5353, 255, 11000001); engine.tick(11100001);
   results = engine.take_results(); if (results.size() != 1 or results[0].addresses.size() != 8) return fail(26);
   engine.withdraw(); (void)engine.take_output();
   engine.receive(reply, 2, false, 5353, 255, 12000000); engine.tick(15000000);
   if (not engine.take_results().empty()) return fail(27); // Cancelled generation cannot be revived.
   if (not engine.register_name("conflict.local", a, 20000000)) return fail(28);
   engine.receive(encode(answer("conflict.local", b)), 2, false, 5353, 255, 20000001);
   if (engine.registrations()[0].status != Status::FAILED) return fail(29);
   engine.withdraw();
   if (not engine.register_name("live.local", a, 21000000)) return fail(30);
   for (uint64_t time = 21000000; time < 22500000; time += 250000) engine.tick(time);
   (void)engine.take_output();
   engine.receive(encode(answer("live.local", b)), 2, false, 5353, 255, 23000000);
   if (engine.registrations()[0].status != Status::READY or engine.take_output().size() != 1) return fail(31);
   engine.receive(encode(answer("live.local", b)), 2, false, 5353, 255, 23000001);
   if (engine.registrations()[0].status != Status::FAILED) return fail(32);
   engine.withdraw();
   for (int i = 0; i < 64; ++i) if (not engine.resolve(std::to_string(i) + ".local", scopes, 25000000)) return fail(33);
   if (engine.resolve("overflow.local", scopes, 25000000)) return fail(34);
   engine.tick(25000000); if (engine.take_output().size() > 128) return fail(35);
   engine.withdraw();
   for (int i = 0; i < 32; ++i) {
      if (not engine.register_name(std::to_string(i) + ".local", a, 30000000)) return fail(36);
   }
   if (engine.register_name("overflow.local", a, 30000000)) return fail(37);
   engine.failed(2, false); engine.tick(33000000);
   if (engine.pending() or not engine.take_output().empty()) return fail(38);
   // Three scopes splits a query across batches; four reproduces the review case; 32 is the maximum.
   if (not saturated_queries(3)) return fail(39);
   if (not saturated_queries(4)) return fail(40);
   if (not saturated_queries(32)) return fail(41);
   std::puts("mDNS codec, scoped cache, bounds, probing, conflict, withdrawal and cancellation passed");
}
