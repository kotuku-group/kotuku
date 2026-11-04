#pragma once

#ifdef _WIN32
#define OBJECTID WindowsObjectID
#include <winsock2.h>
#undef OBJECTID
#undef __OBJECTID_DEFINED
#undef RELATIVE
#undef DELETE
#undef FD_READ
#undef FD_WRITE
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <cstddef>
#include <limits>

#ifndef IS
#define IS ==
#endif

namespace rtc::test {

#ifdef _WIN32
using Socket = SOCKET;
using AddressSize = int;
constexpr Socket INVALID_SOCKET_VALUE = INVALID_SOCKET;
#else
using Socket = int;
using AddressSize = socklen_t;
constexpr Socket INVALID_SOCKET_VALUE = -1;
#endif

inline void close_socket(Socket Socket) {
#ifdef _WIN32
   closesocket(Socket);
#else
   ::close(Socket);
#endif
}

inline Socket udp_socket(bool IPv6 = false) {
   auto socket = ::socket(IPv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
   if (socket IS INVALID_SOCKET_VALUE) return socket;
#ifdef _WIN32
   u_long nonblocking = 1;
   if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
#else
   if (fcntl(socket, F_SETFL, O_NONBLOCK) != 0) {
#endif
      close_socket(socket);
      return INVALID_SOCKET_VALUE;
   }
   return socket;
}

inline int receive_from(Socket Socket, void *Buffer, size_t Size, int Flags,
   sockaddr *Address, AddressSize *AddressLength) {
   if (Size > size_t(std::numeric_limits<int>::max())) return -1;
   return int(::recvfrom(Socket, (char *)Buffer, int(Size), Flags, Address, AddressLength));
}

inline int send_to(Socket Socket, const void *Buffer, size_t Size, int Flags,
   const sockaddr *Address, AddressSize AddressLength) {
   if (Size > size_t(std::numeric_limits<int>::max())) return -1;
   return int(::sendto(Socket, (const char *)Buffer, int(Size), Flags, Address, AddressLength));
}

inline int receive(Socket Socket, void *Buffer, size_t Size, int Flags) {
   return int(::recv(Socket, (char *)Buffer, int(Size), Flags));
}

inline int send(Socket Socket, const void *Buffer, size_t Size, int Flags) {
   return int(::send(Socket, (const char *)Buffer, int(Size), Flags));
}

}
