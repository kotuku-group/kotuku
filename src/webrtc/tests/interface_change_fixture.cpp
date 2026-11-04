// Linux public-lifecycle fixture: change loopback scope metadata without altering the host network.
#include <dlfcn.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <chrono>
#include <cstring>

extern "C" int getifaddrs(ifaddrs **Result) {
   using Routine = int (*)(ifaddrs **);
   auto routine = (Routine)dlsym(RTLD_NEXT, "getifaddrs");
   if (not routine) return -1;
   auto status = routine(Result);
   static const auto started = std::chrono::steady_clock::now();
   if (status != 0 or std::chrono::steady_clock::now() - started < std::chrono::seconds(3)) return status;
   for (auto scan = *Result; scan; scan = scan->ifa_next) {
      if (not scan->ifa_addr or scan->ifa_addr->sa_family != AF_INET6) continue;
      auto address = (sockaddr_in6 *)scan->ifa_addr;
      if (IN6_IS_ADDR_LOOPBACK(&address->sin6_addr)) address->sin6_scope_id = 1;
   }
   return status;
}
