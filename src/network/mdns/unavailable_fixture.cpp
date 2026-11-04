// Linux test-only fault injection.  Other socket options retain their normal system implementation.
#include <kotuku/config.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <cerrno>

extern "C" int setsockopt(int Socket, int Level, int Option, const void *Value, socklen_t Size) {
   if ((Level IS IPPROTO_IP and Option IS IP_PKTINFO) or
       (Level IS IPPROTO_IPV6 and Option IS IPV6_RECVPKTINFO)) {
      errno = EOPNOTSUPP;
      return -1;
   }
   auto original = (int (*)(int, int, int, const void *, socklen_t))dlsym(RTLD_NEXT, "setsockopt");
   if (not original) { errno = EOPNOTSUPP; return -1; }
   return original(Socket, Level, Option, Value, Size);
}
