#ifdef __linux__

#include "net_platform.h"
#include "socket_errors.h"

#include <array>
#include <cstring>
#include <sys/resource.h>

class LinuxNet : public NetworkPlatform {
private:
   int socket_limit_value = 0x7fffffff;

public:
   ERR initialise(OBJECTPTR Module) override
   {
      struct rlimit fd_limit;
      if (getrlimit(RLIMIT_NOFILE, &fd_limit) IS 0) {
         socket_limit_value = int(fd_limit.rlim_cur * 0.8);
      }
      return ERR::Okay;
   }

   void expunge() override
   {
   }

   int socket_limit() const override
   {
      return socket_limit_value;
   }

   SocketHandle create_socket(void *Reference, bool Read, bool Write, bool UDP, bool &IPv6) override
   {
      int socket_type = UDP ? SOCK_DGRAM : SOCK_STREAM;
      int protocol = UDP ? IPPROTO_UDP : IPPROTO_TCP;
      SocketHandle handle;

      if (auto sock = socket(PF_INET6, socket_type, protocol); sock != -1) {
         handle = SocketHandle(sock);
         IPv6 = true;

         int v6only = 0;
         setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

         if (not UDP) {
            int nodelay = 1;
            setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
         }
      }
      else if (auto sock = socket(PF_INET, socket_type, protocol); sock != -1) {
         handle = SocketHandle(sock);
         IPv6 = false;

         if (not UDP) {
            int nodelay = 1;
            setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
         }
      }

      if (handle.is_valid()) {
         if (fcntl(handle, F_SETFL, fcntl(handle, F_GETFL) | O_NONBLOCK)) {
            close_socket(handle);
            return SocketHandle();
         }
      }

      return handle;
   }

   void close_socket(SocketHandle Handle) override
   {
      if (Handle.is_invalid()) return;

      kt::Log log(__FUNCTION__);
      log.traceBranch("Handle: %d", Handle.int_value());

      shutdown(Handle, SHUT_RDWR);

      struct timeval timeout;
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;
      setsockopt(Handle, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
      setsockopt(Handle, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

      std::array<char, 1024> buffer;
      int bytes_received;
      do {
         bytes_received = recv(Handle, buffer.data(), buffer.size(), 0);
      } while (bytes_received > 0);

      close(Handle);
   }

   void deregister_socket(SocketHandle Handle) override
   {
   }

   int shutdown_socket(SocketHandle Handle, int How) override
   {
      return shutdown(Handle, How);
   }

   ERR connect(SocketHandle Handle, const socket_endpoint &Endpoint) override
   {
      int result = ::connect(Handle, (struct sockaddr *)&Endpoint.storage, Endpoint.size);
      if (result != -1) return ERR::Okay;

      if ((errno IS EINPROGRESS) or (errno IS EWOULDBLOCK) or (errno IS EAGAIN)) return ERR::Busy;
      return convert_socket_error(errno);
   }

   ERR begin_connect_wait(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return register_write(Handle, Callback, Data);
   }

   ERR complete_connect(SocketHandle Handle) override
   {
      int result = EHOSTUNREACH;
      socklen_t optlen = sizeof(result);
      if (getsockopt(Handle, SOL_SOCKET, SO_ERROR, &result, &optlen) != 0) return convert_socket_error(errno);
      return result ? convert_socket_error(result) : ERR::Okay;
   }

   ERR bind(SocketHandle Handle, const socket_endpoint &Endpoint) override
   {
      int value = 1;
      setsockopt(Handle, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));

      if (::bind(Handle, (struct sockaddr *)&Endpoint.storage, Endpoint.size) IS -1) {
         return convert_socket_error(errno);
      }
      return ERR::Okay;
   }

   ERR listen(SocketHandle Handle, int Backlog) override
   {
      if (::listen(Handle, Backlog) IS -1) return convert_socket_error(errno);
      return ERR::Okay;
   }

   ERR get_local_address(SocketHandle Handle, sockaddr_storage &Address, int &Length) override
   {
      socklen_t addr_length = sizeof(Address);
      auto result = getsockname(Handle, (struct sockaddr *)&Address, &addr_length);
      Length = int(addr_length);
      return result ? convert_socket_error(errno) : ERR::Okay;
   }

   SocketHandle accept(void *Reference, SocketHandle Server, bool IPv6, sockaddr_storage &Address, int &Family) override
   {
      socklen_t len = sizeof(Address);
      auto client = ::accept(Server, (struct sockaddr *)&Address, &len);
      if (client IS -1) return SocketHandle();

      Family = Address.ss_family;

      int non_blocking = 1;
      ioctl(client, FIONBIO, &non_blocking);

      int nodelay = 1;
      setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
      return SocketHandle(client);
   }

   void set_socket_reference(SocketHandle Handle, void *Reference) override
   {
   }

   ERR set_non_blocking(SocketHandle Handle) override
   {
      int non_blocking = 1;
      return ioctl(Handle, FIONBIO, &non_blocking) ? convert_socket_error(errno) : ERR::Okay;
   }

   ERR register_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return RegisterFD(Handle.hosthandle(), RFD::READ|RFD::SOCKET, Callback, Data);
   }

   ERR register_accept(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return register_read(Handle, Callback, Data);
   }

   ERR register_write(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return RegisterFD(Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, Callback, Data);
   }

   ERR remove_write(SocketHandle Handle) override
   {
      return RegisterFD(Handle.hosthandle(), RFD::REMOVE|RFD::WRITE|RFD::SOCKET, nullptr, nullptr);
   }

   ERR register_recall_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return RegisterFD(Handle.hosthandle(), RFD::RECALL|RFD::READ|RFD::SOCKET, Callback, Data);
   }

   ERR deregister_fd(SocketHandle Handle) override
   {
      return DeregisterFD(Handle.hosthandle());
   }

   ERR enable_broadcast(SocketHandle Handle) override
   {
      int broadcast = 1;
      return setsockopt(Handle, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) ? ERR::SystemCall : ERR::Okay;
   }

   ERR set_multicast_ttl(SocketHandle Handle, int TTL, bool IPv6) override
   {
      if (IPv6) {
         return setsockopt(Handle, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &TTL, sizeof(TTL)) ? ERR::SystemCall : ERR::Okay;
      }
      else {
         return setsockopt(Handle, IPPROTO_IP, IP_MULTICAST_TTL, &TTL, sizeof(TTL)) ? ERR::SystemCall : ERR::Okay;
      }
   }

   ERR join_multicast_group(SocketHandle Handle, CSTRING Group, bool IPv6) override
   {
      if (IPv6) {
         struct ipv6_mreq mreq;
         kt::clearmem(&mreq, sizeof(mreq));
         if (::inet_pton(AF_INET6, Group, &mreq.ipv6mr_multiaddr) != 1) return ERR::Args;
         mreq.ipv6mr_interface = 0;
         return setsockopt(Handle, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char *)&mreq, sizeof(mreq)) ? ERR::Failed : ERR::Okay;
      }
      else {
         struct ip_mreq mreq;
         kt::clearmem(&mreq, sizeof(mreq));
         if (::inet_pton(AF_INET, Group, &mreq.imr_multiaddr) != 1) return ERR::Args;
         mreq.imr_interface.s_addr = INADDR_ANY;
         return setsockopt(Handle, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) ? ERR::Failed : ERR::Okay;
      }
   }

   ERR leave_multicast_group(SocketHandle Handle, CSTRING Group, bool IPv6) override
   {
      if (IPv6) {
         struct ipv6_mreq mreq;
         kt::clearmem(&mreq, sizeof(mreq));
         if (::inet_pton(AF_INET6, Group, &mreq.ipv6mr_multiaddr) != 1) return ERR::Args;
         mreq.ipv6mr_interface = 0;
         return setsockopt(Handle, IPPROTO_IPV6, IPV6_LEAVE_GROUP, (char *)&mreq, sizeof(mreq)) ? ERR::Failed : ERR::Okay;
      }
      else {
         struct ip_mreq mreq;
         kt::clearmem(&mreq, sizeof(mreq));
         if (::inet_pton(AF_INET, Group, &mreq.imr_multiaddr) != 1) return ERR::Args;
         mreq.imr_interface.s_addr = INADDR_ANY;
         return setsockopt(Handle, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) ? ERR::Failed : ERR::Okay;
      }
   }

   ERR receive(SocketHandle Handle, APTR Buffer, size_t Length, size_t &Received) override
   {
      Received = 0;
      auto result = recv(Handle, Buffer, Length, 0);
      if (result > 0) {
         Received = size_t(result);
         return ERR::Okay;
      }
      else if (result IS 0) return ERR::Disconnected;
      else if ((errno IS EAGAIN) or (errno IS EINTR)) return ERR::Okay;
      else return convert_socket_error(errno);
   }

   ERR append_receive(SocketHandle Handle, std::vector<uint8_t> &Buffer, size_t Length, size_t &Received) override
   {
      std::vector<uint8_t> temp(Length);
      auto error = receive(Handle, temp.data(), temp.size(), Received);
      if ((error IS ERR::Okay) and (Received > 0)) {
         Buffer.insert(Buffer.end(), temp.begin(), temp.begin() + Received);
      }
      return error;
   }

   ERR send(SocketHandle Handle, CPTR Buffer, size_t &Length) override
   {
      auto result = ::send(Handle, Buffer, Length, 0);
      if (result >= 0) {
         Length = size_t(result);
         return ERR::Okay;
      }

      Length = 0;
      if ((errno IS EAGAIN) or (errno IS EWOULDBLOCK)) return ERR::BufferOverflow;
      else if (errno IS EMSGSIZE) return ERR::DataSize;
      else return convert_socket_error(errno, ERR::Failed);
   }

   ERR send_to(SocketHandle Handle, CPTR Buffer, size_t &Length, const socket_endpoint &Endpoint) override
   {
      auto result = sendto(Handle, Buffer, Length, MSG_DONTWAIT, (sockaddr *)&Endpoint.storage, Endpoint.size);
      if (result >= 0) {
         Length = size_t(result);
         return ERR::Okay;
      }

      Length = 0;
      switch (errno) {
         #if EAGAIN == EWOULDBLOCK
            case EAGAIN: return ERR::BufferOverflow;
         #else
            case EAGAIN:
            case EWOULDBLOCK: return ERR::BufferOverflow;
         #endif
         case ENETUNREACH: return ERR::NetworkUnreachable;
         case EINVAL: return ERR::Args;
         default: return convert_socket_error(errno);
      }
   }

   ERR receive_from(SocketHandle Handle, APTR Buffer, size_t BufferSize, size_t &BytesRead,
      sockaddr_storage &SourceAddress, int &AddressLength) override
   {
      BytesRead = 0;
      socklen_t addr_len = sizeof(SourceAddress);
      auto result = recvfrom(Handle, Buffer, BufferSize, MSG_DONTWAIT, (struct sockaddr *)&SourceAddress, &addr_len);
      AddressLength = int(addr_len);

      if (result > 0) {
         BytesRead = size_t(result);
         return ERR::Okay;
      }
      else if (result IS 0) return ERR::Okay;

      switch (errno) {
         #if EAGAIN == EWOULDBLOCK
            case EAGAIN: return ERR::Okay;
         #else
            case EAGAIN:
            case EWOULDBLOCK: return ERR::Okay;
         #endif
         case EMSGSIZE: return ERR::BufferOverflow;
         default: return convert_socket_error(errno);
      }
   }

   uint32_t inet_addr(CSTRING Value) override
   {
      return ::inet_addr(Value);
   }

   int inet_pton(int Family, CSTRING Source, APTR Dest) override
   {
      return ::inet_pton(Family, Source, Dest);
   }

   CSTRING inet_ntop(int Family, CPTR Source, STRING Dest, size_t Size) override
   {
      return ::inet_ntop(Family, Source, Dest, Size);
   }

   uint32_t host_to_long(uint32_t Value) override
   {
      return htonl(Value);
   }

   uint32_t long_to_host(uint32_t Value) override
   {
      return ntohl(Value);
   }

   uint16_t host_to_short(uint16_t Value) override
   {
      return htons(Value);
   }

   uint16_t short_to_host(uint16_t Value) override
   {
      return ntohs(Value);
   }
};

std::unique_ptr<NetworkPlatform> create_platform()
{
   return std::make_unique<LinuxNet>();
}

#endif
