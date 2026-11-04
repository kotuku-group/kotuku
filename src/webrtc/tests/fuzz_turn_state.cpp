#include "turn_test_support.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
   if (Size > 4096) return 0;
   using namespace turn_test;
   bool reliable = Size and (Data[0] & 1);
   bool sha256 = Size and (Data[0] & 2);
   Client client(config(reliable)); establish(client, sha256, 3600);
   uint64_t now = 20;
   Datagram data;
   for (size_t i = 0; i < Size and i < 128; ++i) {
      uint8_t operation = Data[i];
      auto peer = endpoint(uint8_t(2 + (operation >> 4)), uint16_t(9000 + (operation >> 4)));
      auto payload = std::span<const uint8_t>(Data + i, std::min(Size - i, size_t(1200)));
      switch (operation % 8) {
         case 0: (void)client.permission(peer, now); break;
         case 1: (void)client.bind(peer, uint16_t(0x4000 + (operation >> 4)), now); break;
         case 2: (void)client.send(peer, payload, now); break;
         case 3: (void)client.receive(config().identity, payload, now, data); break;
         case 4: now += uint64_t(operation + 1) * 10000; client.tick(now); break;
         case 5: client.close(now); break;
         case 6: now += 500; client.tick(now); break;
         case 7: (void)client.bind(peer, 0x4000, now); break;
      }
      Packet packet;
      // Generated authenticated replies let mutations reach state behind the integrity boundary.
      for (unsigned j = 0; j < 32 and client.pop_output(packet); ++j) {
         Message message;
         if (not rtc::stun::parse(packet.wire, message) or not packet.maintenance) continue;
         if (operation & 8) continue; // deterministic loss
         auto response = (operation & 32) ? challenge(packet, 438,
            "nonce-" + std::to_string(i), sha256, bool(operation & 64)) : success(packet, 3600, sha256);
         if ((operation & 64) and response.size() > 25) response[24] ^= operation;
         (void)client.receive(packet.identity, response, now, data);
         if (operation & 16) (void)client.receive(packet.identity, response, now, data);
      }
      ++now;
   }
   client.close(now); client.tick(now + 1000);
   return 0;
}
