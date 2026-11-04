// Owner-task interface notifications with bounded polling and deterministic debounce.

#pragma once

#include "ice.h"

#include <atomic>
#include <memory>
#include <vector>

namespace rtc {

struct InterfaceAddress {
   ice::Endpoint endpoint;
   uint32_t index = 0, scope = 0; // Interface index and IPv6 scope identifier
   bool operator==(const InterfaceAddress &) const = default;
};

//********************************************************************************************************************
// Reports changes to the host's interface addresses.  The operating system's change notifications are used to
// trigger an immediate snapshot, and a periodic snapshot recovers from missed notifications.

class InterfaceMonitor {
public:
   InterfaceMonitor();
   ~InterfaceMonitor();
   InterfaceMonitor(const InterfaceMonitor &) = delete;
   InterfaceMonitor & operator=(const InterfaceMonitor &) = delete;

   bool poll(uint64_t NowUs);

   // Injection seam: the same debounce used for real snapshots, without modifying the host network.

   bool sample(std::vector<InterfaceAddress> Values, uint64_t NowUs);
   const std::vector<InterfaceAddress> & addresses() const { return committed; }
   bool subscribed() const;

private:
   struct Subscription;
   std::unique_ptr<Subscription> subscription;
   std::vector<InterfaceAddress> committed, pending;
   uint64_t next_poll = 0, first_change = 0, last_change = 0;
   bool initialised = false, changing = false;
};

} // namespace rtc
