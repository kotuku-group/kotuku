#include "mdns.h"
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
   auto input = std::span(Data, Size);
   net::mdns::Message message;
   if (net::mdns::decode(input, message)) {
      auto encoded = net::mdns::encode(message);
      net::mdns::Message again;
      (void)net::mdns::decode(encoded, again);
   }
   net::mdns::Engine engine;
   net::mdns::Address address{{192, 0, 2, 1}, 2};
   (void)engine.register_name("fuzz.local", address, 1000000);
   (void)engine.resolve("fuzz.local", std::span(&address, 1), 1000000);
   for (unsigned i = 0; i < 8; ++i) {
      auto time = uint64_t(1000000) + i * 500000;
      engine.receive(input, i % 2 ? 2 : 3, false, 5353, i % 3 ? 255 : 254, time);
      engine.tick(time);
      (void)engine.take_output(); (void)engine.take_results();
   }
   engine.withdraw();
   engine.receive(input, 2, false, 5353, 255, 10000000);
   engine.tick(11000000);
   return 0;
}
