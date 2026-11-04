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
#include <mswsock.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include "mdns.h"
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace net::mdns {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
void close_socket(intptr_t Value) { if (Value != -1) closesocket(Socket(Value)); }
#else
using Socket = int;
void close_socket(intptr_t Value) { if (Value != -1) ::close(int(Value)); }
#endif
bool option(Socket Socket, int Level, int Key, const void *Value, int Size) {
   return setsockopt(Socket, Level, Key, (const char *)Value, Size) IS 0;
}
std::vector<Address> addresses(bool IPv6) {
   std::vector<Address> result;
#ifdef _WIN32
   ULONG size = 16384, status = ERROR_BUFFER_OVERFLOW;
   std::vector<uint8_t> buffer;
   for (int attempt = 0; attempt < 3 and status IS ERROR_BUFFER_OVERFLOW; ++attempt) {
      buffer.resize(size);
      status = GetAdaptersAddresses(IPv6 ? AF_UNSPEC : AF_INET,
         GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
         (IP_ADAPTER_ADDRESSES *)buffer.data(), &size);
   }
   if (status != NO_ERROR) return result;
   for (auto adapter = (IP_ADAPTER_ADDRESSES *)buffer.data(); adapter; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp or adapter->IfType IS IF_TYPE_SOFTWARE_LOOPBACK or
          (adapter->Flags & IP_ADAPTER_NO_MULTICAST)) continue;
      for (auto scan = adapter->FirstUnicastAddress; scan and result.size() < 32; scan = scan->Next) {
         auto socket_address = scan->Address.lpSockaddr;
         if (not socket_address or scan->DadState != IpDadStatePreferred) continue;
         auto family = socket_address->sa_family;
         if (family != AF_INET and (family != AF_INET6 or not IPv6)) continue;
         Address address; address.ipv6 = family IS AF_INET6;
         address.interface_index = address.ipv6 ? adapter->Ipv6IfIndex : adapter->IfIndex;
         std::memcpy(address.bytes.data(), address.ipv6 ? (void *)&((sockaddr_in6 *)socket_address)->sin6_addr :
            (void *)&((sockaddr_in *)socket_address)->sin_addr, address.ipv6 ? 16 : 4);
         // Link-local IPv6 is useful for multicast membership even though ICE cannot represent its scope yet.
         if (address.interface_index) result.push_back(address);
      }
   }
#else
   ifaddrs *list = nullptr;
   if (getifaddrs(&list) != 0) return result;
   for (auto scan = list; scan and result.size() < 32; scan = scan->ifa_next) {
      if (not scan->ifa_addr or not (scan->ifa_flags & IFF_UP) or not (scan->ifa_flags & IFF_MULTICAST) or
          (scan->ifa_flags & IFF_LOOPBACK)) continue;
      auto family = scan->ifa_addr->sa_family;
      if (family != AF_INET and (family != AF_INET6 or not IPv6)) continue;
      Address address; address.interface_index = if_nametoindex(scan->ifa_name); address.ipv6 = family IS AF_INET6;
      std::memcpy(address.bytes.data(), address.ipv6 ? (void *)&((sockaddr_in6 *)scan->ifa_addr)->sin6_addr :
         (void *)&((sockaddr_in *)scan->ifa_addr)->sin_addr, address.ipv6 ? 16 : 4);
      if (address.interface_index) result.push_back(address);
   }
   freeifaddrs(list);
#endif
   return result;
}
intptr_t open_socket(bool IPv6, std::vector<Address> &Interfaces) {
   auto socket = ::socket(IPv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
   if (socket IS INVALID_SOCKET) return -1;
   u_long nonblocking = 1;
   bool ready = ioctlsocket(socket, FIONBIO, &nonblocking) IS 0;
#else
   if (socket < 0) return -1;
   bool ready = fcntl(socket, F_SETFL, O_NONBLOCK) IS 0;
#endif
   int one = 1, hops = 255;
   ready = ready and option(socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   if (IPv6) {
      ready = ready and option(socket, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
#ifdef _WIN32
      ready = ready and option(socket, IPPROTO_IPV6, IPV6_PKTINFO, &one, sizeof(one)) and
         option(socket, IPPROTO_IPV6, IPV6_HOPLIMIT, &one, sizeof(one));
#else
      ready = ready and option(socket, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)) and
         option(socket, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one));
#endif
      ready = ready and option(socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
      sockaddr_in6 bind_address{}; bind_address.sin6_family = AF_INET6; bind_address.sin6_port = htons(5353);
      ready = ready and ::bind(socket, (sockaddr *)&bind_address, sizeof(bind_address)) IS 0;
   } else {
      ready = ready and option(socket, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
#ifdef _WIN32
      ready = ready and option(socket, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one));
#else
      ready = ready and option(socket, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one));
#endif
      ready = ready and option(socket, IPPROTO_IP, IP_MULTICAST_TTL, &hops, sizeof(hops));
      sockaddr_in bind_address{}; bind_address.sin_family = AF_INET; bind_address.sin_port = htons(5353);
      ready = ready and ::bind(socket, (sockaddr *)&bind_address, sizeof(bind_address)) IS 0;
   }
   if (not ready) { close_socket(intptr_t(socket)); return -1; }
   std::vector<uint32_t> joined;
   for (auto it = Interfaces.begin(); it != Interfaces.end();) {
      if (it->ipv6 != IPv6) { ++it; continue; }
      if (std::find(joined.begin(), joined.end(), it->interface_index) != joined.end()) { ++it; continue; }
      bool success = false;
      if (IPv6) {
         ipv6_mreq membership{}; inet_pton(AF_INET6, "ff02::fb", &membership.ipv6mr_multiaddr);
         membership.ipv6mr_interface = it->interface_index;
         success = option(socket, IPPROTO_IPV6, IPV6_JOIN_GROUP, &membership, sizeof(membership));
      } else {
#ifdef _WIN32
         ip_mreq membership{}; std::memcpy(&membership.imr_interface, it->bytes.data(), 4);
#else
         ip_mreqn membership{}; membership.imr_ifindex = int(it->interface_index);
#endif
         inet_pton(AF_INET, "224.0.0.251", &membership.imr_multiaddr);
         success = option(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership));
      }
      if (success) { joined.push_back(it->interface_index); ++it; }
      else it = Interfaces.erase(it);
   }
   if (joined.empty()) { close_socket(intptr_t(socket)); return -1; }
   return intptr_t(socket);
}
}
Service::Service() = default;
Service::~Service() { withdraw(); close_socket(socket4); close_socket(socket6); }
bool Service::open(bool IPv6) {
   if (socket4 != -1 or socket6 != -1) return true;
   interfaces = addresses(IPv6);
   socket4 = open_socket(false, interfaces);
   if (IPv6) socket6 = open_socket(true, interfaces);
   std::erase_if(interfaces, [this](const Address &Value) { return (Value.ipv6 ? socket6 : socket4) IS -1; });
   return not interfaces.empty();
}
bool Service::register_address(std::string_view Name, const Address &Value, uint64_t NowUs) {
   auto found = std::find_if(interfaces.begin(), interfaces.end(), [&](const Address &Address) {
      return Address.bytes IS Value.bytes and Address.ipv6 IS Value.ipv6;
   });
   return found != interfaces.end() and engine.register_name(Name, *found, NowUs);
}
bool Service::resolve(std::string_view Name, uint64_t NowUs) { return engine.resolve(Name, interfaces, NowUs); }
void Service::withdraw() {
   pending_output.clear();
   (void)engine.take_output();
   engine.withdraw();
   flush();
}
void Service::flush() {
   for (auto &packet : engine.take_output()) {
      if (pending_output.size() < 128) pending_output.push_back(std::move(packet));
   }
   for (auto it = pending_output.begin(); it != pending_output.end();) {
      const auto &packet = *it;
      auto socket = Socket(packet.ipv6 ? socket6 : socket4);
      if (intptr_t(socket) IS -1) {
         engine.failed(packet.interface_index, packet.ipv6);
         it = pending_output.erase(it);
         continue;
      }
      sockaddr_storage destination{};
      int length;
      if (packet.ipv6) {
         auto address = (sockaddr_in6 *)&destination; address->sin6_family = AF_INET6;
         address->sin6_port = htons(5353); address->sin6_scope_id = packet.interface_index;
         inet_pton(AF_INET6, "ff02::fb", &address->sin6_addr); length = sizeof(*address);
      } else {
         auto address = (sockaddr_in *)&destination; address->sin_family = AF_INET;
         address->sin_port = htons(5353); inet_pton(AF_INET, "224.0.0.251", &address->sin_addr);
         length = sizeof(*address);
      }
      bool success = false;
      bool retry = false;
#ifdef _WIN32
      // IP_MULTICAST_IF accepts a network-byte-order interface index on Windows.
      auto index = packet.ipv6 ? packet.interface_index : htonl(packet.interface_index);
      if (option(socket, packet.ipv6 ? IPPROTO_IPV6 : IPPROTO_IP,
          packet.ipv6 ? IPV6_MULTICAST_IF : IP_MULTICAST_IF, &index, sizeof(index))) {
         auto sent = sendto(socket, (const char *)packet.bytes.data(), int(packet.bytes.size()), 0,
            (sockaddr *)&destination, length);
         success = sent IS int(packet.bytes.size());
         if (sent IS SOCKET_ERROR) {
            auto error = WSAGetLastError();
            retry = error IS WSAEWOULDBLOCK or error IS WSAENOBUFS or error IS WSAEINTR;
         }
      }
#else
      iovec buffer{(void *)packet.bytes.data(), packet.bytes.size()};
      alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(in6_pktinfo))> control{};
      msghdr message{}; message.msg_name = &destination; message.msg_namelen = socklen_t(length);
      message.msg_iov = &buffer; message.msg_iovlen = 1; message.msg_control = control.data();
      message.msg_controllen = packet.ipv6 ? CMSG_SPACE(sizeof(in6_pktinfo)) : CMSG_SPACE(sizeof(in_pktinfo));
      auto header = CMSG_FIRSTHDR(&message);
      header->cmsg_level = packet.ipv6 ? IPPROTO_IPV6 : IPPROTO_IP;
      header->cmsg_type = packet.ipv6 ? IPV6_PKTINFO : IP_PKTINFO;
      header->cmsg_len = packet.ipv6 ? CMSG_LEN(sizeof(in6_pktinfo)) : CMSG_LEN(sizeof(in_pktinfo));
      if (packet.ipv6) ((in6_pktinfo *)CMSG_DATA(header))->ipi6_ifindex = packet.interface_index;
      else ((in_pktinfo *)CMSG_DATA(header))->ipi_ifindex = int(packet.interface_index);
      auto sent = sendmsg(socket, &message, 0);
      success = sent IS ssize_t(packet.bytes.size());
      if (sent < 0) retry = errno IS EAGAIN or errno IS EWOULDBLOCK or errno IS ENOBUFS or errno IS EINTR;
#endif
      if (retry) break;
      if (not success) engine.failed(packet.interface_index, packet.ipv6);
      it = pending_output.erase(it);
   }
}
void Service::tick(uint64_t NowUs) {
   for (bool ipv6 : {false, true}) {
      auto socket = Socket(ipv6 ? socket6 : socket4);
      if (intptr_t(socket) IS -1) continue;
#ifdef _WIN32
      GUID identifier = WSAID_WSARECVMSG;
      LPFN_WSARECVMSG receive = nullptr;
      DWORD copied = 0;
      if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, sizeof(identifier), &receive,
          sizeof(receive), &copied, nullptr, nullptr) != 0 or not receive) continue;
#endif
      for (unsigned budget = 0; budget < 64; ++budget) {
         std::array<uint8_t, 9000> bytes{};
         sockaddr_storage source{};
         uint32_t interface_index = 0;
         int hops = -1;
         bool multicast = false;
         size_t size = 0;
#ifdef _WIN32
         WSABUF buffer{ULONG(bytes.size()), (char *)bytes.data()};
         alignas(WSACMSGHDR) std::array<char, 256> control{};
         WSAMSG message{}; message.name = (sockaddr *)&source; message.namelen = sizeof(source);
         message.lpBuffers = &buffer; message.dwBufferCount = 1;
         message.Control = {ULONG(control.size()), control.data()};
         DWORD received = 0;
         if (receive(socket, &message, &received, nullptr, nullptr) != 0) break;
         if (message.dwFlags & (MSG_TRUNC | MSG_CTRUNC)) continue;
         size = received;
         for (auto header = WSA_CMSG_FIRSTHDR(&message); header; header = WSA_CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level IS IPPROTO_IP and header->cmsg_type IS IP_PKTINFO and
                header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
               auto info = (IN_PKTINFO *)WSA_CMSG_DATA(header); interface_index = info->ipi_ifindex;
               multicast = ntohl(info->ipi_addr.s_addr) IS 0xe00000fb;
            } else if (header->cmsg_level IS IPPROTO_IPV6 and header->cmsg_type IS IPV6_PKTINFO and
                header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
               auto info = (IN6_PKTINFO *)WSA_CMSG_DATA(header); interface_index = info->ipi6_ifindex;
               in6_addr group{}; inet_pton(AF_INET6, "ff02::fb", &group);
               multicast = std::memcmp(&info->ipi6_addr, &group, 16) IS 0;
            } else if (((header->cmsg_level IS IPPROTO_IP and header->cmsg_type IS IP_TTL) or
                (header->cmsg_level IS IPPROTO_IPV6 and header->cmsg_type IS IPV6_HOPLIMIT)) and
                header->cmsg_len >= WSA_CMSG_LEN(sizeof(int))) std::memcpy(&hops, WSA_CMSG_DATA(header), sizeof(hops));
         }
#else
         iovec buffer{bytes.data(), bytes.size()};
         alignas(cmsghdr) std::array<char, 256> control{};
         msghdr message{}; message.msg_name = &source; message.msg_namelen = sizeof(source);
         message.msg_iov = &buffer; message.msg_iovlen = 1;
         message.msg_control = control.data(); message.msg_controllen = control.size();
         auto received = recvmsg(socket, &message, MSG_DONTWAIT);
         if (received < 0) break;
         if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) continue;
         size = size_t(received);
         for (auto header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level IS IPPROTO_IP and header->cmsg_type IS IP_PKTINFO and
                header->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
               auto info = (in_pktinfo *)CMSG_DATA(header); interface_index = uint32_t(info->ipi_ifindex);
               multicast = ntohl(info->ipi_addr.s_addr) IS 0xe00000fb;
            } else if (header->cmsg_level IS IPPROTO_IPV6 and header->cmsg_type IS IPV6_PKTINFO and
                header->cmsg_len >= CMSG_LEN(sizeof(in6_pktinfo))) {
               auto info = (in6_pktinfo *)CMSG_DATA(header); interface_index = info->ipi6_ifindex;
               in6_addr group{}; inet_pton(AF_INET6, "ff02::fb", &group);
               multicast = std::memcmp(&info->ipi6_addr, &group, 16) IS 0;
            } else if (((header->cmsg_level IS IPPROTO_IP and header->cmsg_type IS IP_TTL) or
                (header->cmsg_level IS IPPROTO_IPV6 and header->cmsg_type IS IPV6_HOPLIMIT)) and
                header->cmsg_len >= CMSG_LEN(sizeof(int))) std::memcpy(&hops, CMSG_DATA(header), sizeof(hops));
         }
#endif
         if (not multicast or std::none_of(interfaces.begin(), interfaces.end(), [&](const Address &Value) {
             return Value.interface_index IS interface_index and Value.ipv6 IS ipv6;
         })) continue;
         auto port = ipv6 ? ntohs(((sockaddr_in6 *)&source)->sin6_port) : ntohs(((sockaddr_in *)&source)->sin_port);
         engine.receive(std::span(bytes).first(size), interface_index, ipv6, port, hops, NowUs);
      }
   }
   engine.tick(NowUs); flush();
}
}
