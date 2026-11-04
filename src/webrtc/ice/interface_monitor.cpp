// Host interface monitoring.  Windows uses IP Helper change notifications and Linux uses a netlink route socket.

#ifdef _WIN32
#define OBJECTID WindowsObjectID
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#undef OBJECTID
#undef __OBJECTID_DEFINED
#undef RELATIVE
#undef DELETE
#undef FD_READ
#undef FD_WRITE
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif

#include "interface_monitor.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace rtc {

//********************************************************************************************************************
// Platform change notifications.  changed() returns true if any notification has arrived since the last call.

struct InterfaceMonitor::Subscription {
#ifdef _WIN32
   HANDLE link = nullptr, address = nullptr;
   std::atomic<bool> dirty{false}; // Set from IP Helper threads

   static void WINAPI changed_interface(PVOID Context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
      ((Subscription *)Context)->dirty.store(true, std::memory_order_relaxed);
   }

   static void WINAPI changed_address(PVOID Context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
      ((Subscription *)Context)->dirty.store(true, std::memory_order_relaxed);
   }

   Subscription() {
      (void)NotifyIpInterfaceChange(AF_UNSPEC, changed_interface, this, FALSE, &link);
      (void)NotifyUnicastIpAddressChange(AF_UNSPEC, changed_address, this, FALSE, &address);
   }

   ~Subscription() {
      // Cancellation waits for outstanding IP Helper callbacks before this storage is released.
      if (link) CancelMibChangeNotify2(link);
      if (address) CancelMibChangeNotify2(address);
   }

   bool active() const { return link and address; }
   bool changed() { return dirty.exchange(false, std::memory_order_relaxed); }
#else
   int fd = -1;

   Subscription() {
      fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
      if (fd < 0) return;

      sockaddr_nl address{};
      address.nl_family = AF_NETLINK;
      address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
      if (::bind(fd, (sockaddr *)&address, sizeof(address)) != 0) {
         ::close(fd);
         fd = -1;
      }
   }

   ~Subscription() { if (fd >= 0) ::close(fd); }

   bool active() const { return fd >= 0; }

   bool changed() {
      if (fd < 0) return false;

      bool dirty = false;
      std::array<uint8_t, 8192> packet{};

      // Notifications only trigger a fresh snapshot.  Overflow also triggers the periodic resynchronisation.
      for (int i = 0; i < 32; ++i) {
         auto size = ::recv(fd, packet.data(), packet.size(), MSG_DONTWAIT);
         if (size < 0) break;
         dirty = true;
      }

      return dirty;
   }
#endif
};

//********************************************************************************************************************
// Enumerates the addresses of every interface that is up, to a maximum of 256.  Returns false if the enumeration failed
// or was incomplete.  On Windows, addresses that are still undergoing duplicate address detection are excluded.

static bool snapshot(std::vector<InterfaceAddress> &Result)
{
#ifdef _WIN32
   ULONG size = 16384;
   std::vector<uint8_t> buffer(size);
   ULONG status = ERROR_BUFFER_OVERFLOW;
   for (int i = 0; i < 3 and status IS ERROR_BUFFER_OVERFLOW; ++i) {
      if (size > 1048576) return false;
      buffer.resize(size);
      status = GetAdaptersAddresses(AF_UNSPEC,
         GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
         nullptr, (IP_ADAPTER_ADDRESSES *)buffer.data(), &size);
   }

   if (status != NO_ERROR) return false;

   for (auto adapter = (IP_ADAPTER_ADDRESSES *)buffer.data(); adapter; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp) continue;
      for (auto scan = adapter->FirstUnicastAddress; scan; scan = scan->Next) {
         auto address = scan->Address.lpSockaddr;
         if (not address or scan->DadState != IpDadStatePreferred) continue;

         InterfaceAddress value;
         value.endpoint.ipv6 = address->sa_family IS AF_INET6;
         if (not value.endpoint.ipv6 and address->sa_family != AF_INET) continue;
         value.index = value.endpoint.ipv6 ? adapter->Ipv6IfIndex : adapter->IfIndex;
         if (value.endpoint.ipv6) {
            auto ipv6 = (sockaddr_in6 *)address;
            value.scope = ipv6->sin6_scope_id;
            std::memcpy(value.endpoint.address.data(), &ipv6->sin6_addr, 16);
         }
         else std::memcpy(value.endpoint.address.data(), &((sockaddr_in *)address)->sin_addr, 4);

         if (Result.size() >= 256) return false;
         Result.push_back(value);
      }
   }
#else
   ifaddrs *interfaces = nullptr;
   if (getifaddrs(&interfaces) != 0) return false;

   bool complete = true;
   for (auto scan = interfaces; scan; scan = scan->ifa_next) {
      if (not scan->ifa_addr or not (scan->ifa_flags & IFF_UP)) continue;
      auto family = scan->ifa_addr->sa_family;
      if (family != AF_INET and family != AF_INET6) continue;

      if (Result.size() >= 256) {
         complete = false;
         break;
      }

      InterfaceAddress value;
      value.index = if_nametoindex(scan->ifa_name);
      value.endpoint.ipv6 = family IS AF_INET6;
      if (value.endpoint.ipv6) {
         auto address = (sockaddr_in6 *)scan->ifa_addr;
         value.scope = address->sin6_scope_id;
         std::memcpy(value.endpoint.address.data(), &address->sin6_addr, 16);
      }
      else std::memcpy(value.endpoint.address.data(), &((sockaddr_in *)scan->ifa_addr)->sin_addr, 4);
      Result.push_back(value);
   }

   freeifaddrs(interfaces);
   if (not complete) return false;
#endif

   return true;
}

//********************************************************************************************************************

InterfaceMonitor::InterfaceMonitor() : subscription(std::make_unique<Subscription>()) { }

InterfaceMonitor::~InterfaceMonitor() = default;

bool InterfaceMonitor::subscribed() const { return subscription->active(); }

//********************************************************************************************************************
// Compares a snapshot with the committed address set.  A change is committed once the addresses have been stable for
// 250 ms, or after one second of continuous change.  The first sample initialises the set without reporting a change.
// Returns true if a change was committed.

bool InterfaceMonitor::sample(std::vector<InterfaceAddress> Values, uint64_t NowUs)
{
   std::sort(Values.begin(), Values.end(), [](const auto &Left, const auto &Right) {
      return std::tie(Left.index, Left.scope, Left.endpoint.ipv6, Left.endpoint.address) <
         std::tie(Right.index, Right.scope, Right.endpoint.ipv6, Right.endpoint.address);
   });
   Values.erase(std::unique(Values.begin(), Values.end()), Values.end());

   if (not initialised) {
      committed = std::move(Values);
      initialised = true;
      return false;
   }

   if (Values IS committed) {
      changing = false;
      return false;
   }

   if (not changing) {
      first_change = NowUs;
      last_change = NowUs;
      changing = true;
      pending = Values;
   }
   else if (Values != pending) {
      last_change = NowUs;
      pending = Values;
   }

   if (NowUs - last_change < 250000 and NowUs - first_change < 1000000) return false;

   committed = std::move(Values);
   changing = false;
   return true;
}

//********************************************************************************************************************
// Takes a snapshot if a notification has arrived, a change is being debounced, or two seconds have passed since the
// last snapshot.  Returns true if a change was committed.

bool InterfaceMonitor::poll(uint64_t NowUs)
{
   bool dirty = subscription->changed();
   if (initialised and not dirty and not changing and NowUs < next_poll) return false;

   next_poll = NowUs + 2000000;
   std::vector<InterfaceAddress> values;
   if (not snapshot(values)) return false; // A failed enumeration must not retire live routes.
   return sample(std::move(values), NowUs);
}

} // namespace rtc
