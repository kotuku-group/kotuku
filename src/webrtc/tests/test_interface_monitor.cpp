#include "../ice/interface_monitor.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <algorithm>

#define CHECK(Value) do { if (not (Value)) { std::fprintf(stderr, "Line %d failed\n", __LINE__); return 1; } } while (0)

int main(int Argc, char **Argv) {
   if (Argc > 1) {
      rtc::InterfaceMonitor live;
      auto now = []() {
         return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
      };
      CHECK(not live.poll(now()));
      CHECK(live.subscribed());
      auto original = live.addresses();
      rtc::ice::Endpoint watched;
      if (Argc > 2) {
         unsigned a, b, c, d;
         CHECK(std::sscanf(Argv[2], "%u.%u.%u.%u", &a, &b, &c, &d) IS 4);
         CHECK(a <= 255 and b <= 255 and c <= 255 and d <= 255);
         watched.address = {uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(d)};
      }
      auto present = [&]() {
         return std::any_of(live.addresses().begin(), live.addresses().end(), [&](const auto &Value) {
            return Value.endpoint IS watched;
         });
      };
      if (Argc > 2) CHECK(not present());
      std::puts("READY"); std::fflush(stdout);
      int changes = 0;
      auto deadline = now() + 10000000;
      while (now() < deadline) {
         if (live.poll(now())) {
            if (Argc > 2) {
               // Other adapters can change during the fixture; require both transitions of its owned address.
               if (not changes and present()) {
                  changes = 1; std::puts("CHANGED"); std::fflush(stdout);
               } else if (changes and not present()) return 0;
               continue;
            }
            ++changes;
            if (changes IS 1) CHECK(live.addresses().size() > original.size());
            if (changes IS 2) { CHECK(live.addresses() IS original); return 0; }
            std::puts("CHANGED"); std::fflush(stdout);
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return 1;
   }
   rtc::InterfaceMonitor monitor;
   rtc::InterfaceAddress first, second;
   first.index = 1; first.endpoint.address[0] = 127; first.endpoint.address[3] = 1;
   second.index = 2; second.endpoint.ipv6 = true; second.endpoint.address[15] = 1;
   CHECK(not monitor.sample({first, second}, 0));
   CHECK(not monitor.sample({second, first, first}, 1)); // Ordering and duplicates are not changes.
   CHECK(not monitor.sample({first}, 100000));
   CHECK(not monitor.sample({first}, 349999));
   CHECK(monitor.sample({first}, 350000));
   CHECK(monitor.addresses().size() IS 1);
   CHECK(not monitor.sample({first, second}, 400000));
   CHECK(not monitor.sample({first}, 500000)); // Transient reversal cancels a pending burst.
   CHECK(not monitor.sample({first}, 1000000));
   CHECK(not monitor.sample({first, second}, 1100000));
   for (uint64_t i = 1; i < 5; ++i) {
      second.scope = uint32_t(i);
      CHECK(not monitor.sample({first, second}, 1100000 + i * 200000));
   }
   second.scope = 5;
   CHECK(monitor.sample({first, second}, 2100000)); // Maximum burst delay is one second.
   CHECK(not monitor.sample({}, 2200000));
   CHECK(monitor.sample({}, 2450000)); // Removal of every usable address is meaningful.
   rtc::InterfaceMonitor live;
   CHECK(not live.poll(1));
   CHECK(not live.addresses().empty());
   std::printf("Debounce, bounds, reorder, reversal, scope and live snapshot passed; subscription=%d\n",
      int(live.subscribed()));
   return 0;
}
