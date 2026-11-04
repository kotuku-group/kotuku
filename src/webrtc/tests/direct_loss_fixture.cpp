// Native restart fixture: lose direct traffic for relay selection, then lose TURN for direct recovery.
#include <kotuku/config.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <atomic>
#include <cerrno>

static std::atomic<bool> glBlock{false};
static std::atomic<int> glServerPort{0};
static std::atomic<unsigned> glDropped{0}, glRejected{0};

extern "C" void rtc_test_select_path(int ServerPort, bool Block) {
   glServerPort = ServerPort;
   glDropped = 0; glRejected = 0; glBlock = Block;
}
extern "C" unsigned rtc_test_dropped() { return glDropped.load(); }
extern "C" unsigned rtc_test_rejected() { return glRejected.load(); }

static int destination_port(const sockaddr *Address, socklen_t AddressLength) {
   if (not Address) return 0;
   if (Address->sa_family IS AF_INET and AddressLength >= sizeof(sockaddr_in)) {
      return ntohs(((const sockaddr_in *)Address)->sin_port);
   }
   if (Address->sa_family IS AF_INET6 and AddressLength >= sizeof(sockaddr_in6)) {
      return ntohs(((const sockaddr_in6 *)Address)->sin6_port);
   }
   return 0;
}

extern "C" int connect(int Socket, const sockaddr *Address, socklen_t AddressLength) {
   using Routine = int (*)(int, const sockaddr *, socklen_t);
   auto routine = (Routine)dlsym(RTLD_NEXT, "connect");
   if (not routine) return -1;
   if (not glBlock and glServerPort and destination_port(Address, AddressLength) IS glServerPort) {
      ++glRejected; errno = ECONNREFUSED; return -1;
   }
   return routine(Socket, Address, AddressLength);
}

extern "C" ssize_t sendto(int Socket, const void *Buffer, size_t Length, int Flags,
   const sockaddr *Address, socklen_t AddressLength) {
   using Routine = ssize_t (*)(int, const void *, size_t, int, const sockaddr *, socklen_t);
   auto routine = (Routine)dlsym(RTLD_NEXT, "sendto");
   if (not routine) return -1;
   auto port = destination_port(Address, AddressLength);
   if (glBlock and port and port != glServerPort) { ++glDropped; return ssize_t(Length); }
   if (not glBlock and glServerPort and port IS glServerPort) {
      ++glRejected; errno = ENETUNREACH; return -1;
   }
   return routine(Socket, Buffer, Length, Flags, Address, AddressLength);
}
