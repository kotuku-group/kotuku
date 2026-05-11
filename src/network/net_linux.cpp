#ifdef __linux__

#include "net_platform.h"
#include "socket_errors.h"

#include <array>
#include <cstring>
#include <strings.h>
#include <sys/resource.h>

static_assert(sizeof(struct sockaddr_storage) <= NETWORK_ENDPOINT_STORAGE_SIZE);

static struct sockaddr_storage & endpoint_storage(NetworkEndpoint &Endpoint)
{
   return *(struct sockaddr_storage *)Endpoint.Storage;
}

static const struct sockaddr_storage & endpoint_storage(const NetworkEndpoint &Endpoint)
{
   return *(const struct sockaddr_storage *)Endpoint.Storage;
}

static bool same_text(CSTRING Left, CSTRING Right)
{
   if ((!Left) or (!Right)) return false;
   return strcasecmp(Left, Right) IS 0;
}

static ERR endpoint_to_ip(const struct sockaddr_storage &Address, IPAddress &IP)
{
   kt::clearmem(&IP, sizeof(IP));

   if (Address.ss_family IS AF_INET) {
      auto addr = (const struct sockaddr_in *)&Address;
      IP.Type = IPADDR::V4;
      IP.Port = network_platform().short_to_host(addr->sin_port);
      IP.Data[0] = network_platform().long_to_host(addr->sin_addr.s_addr);
      return ERR::Okay;
   }
   else if (Address.ss_family IS AF_INET6) {
      auto addr = (const struct sockaddr_in6 *)&Address;
      IP.Type = IPADDR::V6;
      IP.Port = network_platform().short_to_host(addr->sin6_port);
      kt::copymem((APTR)addr->sin6_addr.s6_addr, IP.Data, 16);
      return ERR::Okay;
   }
   else return ERR::Args;
}

static ERR copy_accepted_ip(const struct sockaddr_storage &Address, int Family, uint8_t *IP)
{
   if (!IP) return ERR::NullArgs;

   kt::clearmem(IP, 8);

   if (Family IS AF_INET) {
      auto addr = (const struct sockaddr_in *)&Address;
      kt::copymem(&addr->sin_addr.s_addr, IP, 4);
      return ERR::Okay;
   }
   else if (Family IS AF_INET6) {
      auto addr = (const struct sockaddr_in6 *)&Address;
      kt::copymem((APTR)addr->sin6_addr.s6_addr, IP, 8);
      return ERR::Okay;
   }
   else return ERR::Args;
}

static ERR convert_lookup_error(int Result)
{
   switch (Result) {
      case 0: return ERR::Okay;
      case EAI_AGAIN: return ERR::Retry;
      case EAI_FAIL: return ERR::Failed;
      case EAI_MEMORY: return ERR::Memory;
      #ifdef EAI_OVERFLOW
         case EAI_OVERFLOW: return ERR::BufferOverflow;
      #endif
      case EAI_SYSTEM: return ERR::SystemCall;
      default: return ERR::Failed;
   }
}

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

   SocketHandle socket_from_hosthandle(HOSTHANDLE Handle) override
   {
      return SocketHandle(SOCKET_HANDLE(Handle));
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

   ERR build_address(const IPAddress &IP, int Port, bool IPv6, NetworkEndpoint &Endpoint) override
   {
      kt::clearmem(&Endpoint, sizeof(Endpoint));
      auto &storage = endpoint_storage(Endpoint);

      if ((Port < 0) or (Port > 65535)) return ERR::OutOfRange;

      if (IP.Type IS IPADDR::V6) {
         if (!IPv6) return ERR::InvalidValue;

         auto addr6 = (struct sockaddr_in6 *)&storage;
         addr6->sin6_family = AF_INET6;
         addr6->sin6_port = host_to_short(uint16_t(Port));
         kt::copymem((CPTR)IP.Data, &addr6->sin6_addr.s6_addr, 16);

         Endpoint.Size = sizeof(struct sockaddr_in6);
         Endpoint.Family = AF_INET6;
         Endpoint.Label = "IPv6";
         return ERR::Okay;
      }
      else if (IP.Type IS IPADDR::V4) {
         if (IPv6) {
            auto addr6 = (struct sockaddr_in6 *)&storage;
            uint32_t ipv4_net = host_to_long(IP.Data[0]);

            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = host_to_short(uint16_t(Port));
            addr6->sin6_addr.s6_addr[10] = 0xff;
            addr6->sin6_addr.s6_addr[11] = 0xff;
            kt::copymem(&ipv4_net, &addr6->sin6_addr.s6_addr[12], sizeof(ipv4_net));

            Endpoint.Size = sizeof(struct sockaddr_in6);
            Endpoint.Family = AF_INET6;
            Endpoint.Label = "IPv4-mapped IPv6";
            return ERR::Okay;
         }
         else {
            auto addr4 = (struct sockaddr_in *)&storage;
            addr4->sin_family = AF_INET;
            addr4->sin_port = host_to_short(uint16_t(Port));
            addr4->sin_addr.s_addr = host_to_long(IP.Data[0]);

            Endpoint.Size = sizeof(struct sockaddr_in);
            Endpoint.Family = AF_INET;
            Endpoint.Label = "IPv4";
            return ERR::Okay;
         }
      }
      else return ERR::InvalidData;
   }

   ERR prepare_bind_address(CSTRING Address, int Port, bool IPv6, NetworkEndpoint &Endpoint) override
   {
      kt::clearmem(&Endpoint, sizeof(Endpoint));
      auto &storage = endpoint_storage(Endpoint);

      if ((Port < 0) or (Port > 65535)) return ERR::OutOfRange;

      if (Address) {
         IPAddress ip;
         kt::clearmem(&ip, sizeof(ip));

         if (same_text(Address, "localhost") or same_text(Address, "127.0.0.1")) {
            ip.Type = IPADDR::V4;
            ip.Data[0] = 0x7f000001;
         }
         else if (same_text(Address, "::1")) {
            ip.Type = IPADDR::V6;
            ((uint8_t *)ip.Data)[15] = 1;
         }
         else if (same_text(Address, "::")) {
            ip.Type = IPADDR::V6;
         }
         else if (same_text(Address, "0.0.0.0") or same_text(Address, "*") or same_text(Address, "")) {
            ip.Type = IPADDR::V4;
         }
         else if (same_text(Address, "255.255.255.255")) {
            ip.Type = IPADDR::V4;
            ip.Data[0] = 0xffffffff;
         }
         else if (strchr(Address, ':')) {
            struct in6_addr ipv6_addr;
            if (::inet_pton(AF_INET6, Address, &ipv6_addr) != 1) return ERR::InvalidValue;
            ip.Type = IPADDR::V6;
            kt::copymem(ipv6_addr.s6_addr, ip.Data, 16);
         }
         else {
            uint32_t result = ::inet_addr(Address);
            if (result IS INADDR_NONE) return ERR::InvalidValue;
            ip.Type = IPADDR::V4;
            ip.Data[0] = long_to_host(result);
         }

         return build_address(ip, Port, IPv6, Endpoint);
      }

      if (IPv6) {
         auto addr6 = (struct sockaddr_in6 *)&storage;
         addr6->sin6_family = AF_INET6;
         addr6->sin6_addr = in6addr_any;
         addr6->sin6_port = host_to_short(uint16_t(Port));

         Endpoint.Size = sizeof(struct sockaddr_in6);
         Endpoint.Family = AF_INET6;
         Endpoint.Label = "IPv6";
      }
      else {
         auto addr4 = (struct sockaddr_in *)&storage;
         addr4->sin_family = AF_INET;
         addr4->sin_addr.s_addr = INADDR_ANY;
         addr4->sin_port = host_to_short(uint16_t(Port));

         Endpoint.Size = sizeof(struct sockaddr_in);
         Endpoint.Family = AF_INET;
         Endpoint.Label = "IPv4";
      }

      return ERR::Okay;
   }

   ERR connect(SocketHandle Handle, const NetworkEndpoint &Endpoint) override
   {
      int result = ::connect(Handle, (struct sockaddr *)&endpoint_storage(Endpoint), Endpoint.Size);
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

   ERR bind(SocketHandle Handle, const NetworkEndpoint &Endpoint) override
   {
      int value = 1;
      setsockopt(Handle, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));

      if (::bind(Handle, (struct sockaddr *)&endpoint_storage(Endpoint), Endpoint.Size) IS -1) {
         return convert_socket_error(errno);
      }
      return ERR::Okay;
   }

   ERR listen(SocketHandle Handle, int Backlog) override
   {
      if (::listen(Handle, Backlog) IS -1) return convert_socket_error(errno);
      return ERR::Okay;
   }

   ERR get_local_ip(SocketHandle Handle, IPAddress &Address) override
   {
      struct sockaddr_storage storage;
      socklen_t addr_length = sizeof(storage);
      auto result = getsockname(Handle, (struct sockaddr *)&storage, &addr_length);
      if (result) return convert_socket_error(errno);
      return endpoint_to_ip(storage, Address);
   }

   AcceptedSocket accept(void *Reference, SocketHandle Server, bool IPv6) override
   {
      AcceptedSocket accepted;
      struct sockaddr_storage address;
      socklen_t len = sizeof(address);
      auto client = ::accept(Server, (struct sockaddr *)&address, &len);
      if (client IS -1) return accepted;

      accepted.Family = address.ss_family;

      int non_blocking = 1;
      ioctl(client, FIONBIO, &non_blocking);

      int nodelay = 1;
      setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
      accepted.Handle = SocketHandle(client);

      if (copy_accepted_ip(address, accepted.Family, accepted.IP) != ERR::Okay) {
         close_socket(accepted.Handle);
         accepted.Handle = SocketHandle();
      }

      return accepted;
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

   ERR remove_read(SocketHandle Handle) override
   {
      return RegisterFD(Handle.hosthandle(), RFD::REMOVE|RFD::READ|RFD::SOCKET, nullptr, nullptr);
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

   ERR parse_multicast_group(CSTRING Group, bool &IPv6) override
   {
      if (!Group) return ERR::Args;

      struct in6_addr addr6;
      struct in_addr addr4;
      kt::clearmem(&addr6, sizeof(addr6));
      kt::clearmem(&addr4, sizeof(addr4));

      if (::inet_pton(AF_INET6, Group, &addr6) IS 1) {
         IPv6 = true;
         return ERR::Okay;
      }
      else if (::inet_pton(AF_INET, Group, &addr4) IS 1) {
         IPv6 = false;
         return ERR::Okay;
      }
      else return ERR::Args;
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

   ERR send_to(SocketHandle Handle, CPTR Buffer, size_t &Length, const NetworkEndpoint &Endpoint) override
   {
      auto result = sendto(Handle, Buffer, Length, MSG_DONTWAIT, (sockaddr *)&endpoint_storage(Endpoint),
         Endpoint.Size);
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
      IPAddress &SourceAddress) override
   {
      BytesRead = 0;
      struct sockaddr_storage source_address;
      socklen_t addr_len = sizeof(source_address);
      auto result = recvfrom(Handle, Buffer, BufferSize, MSG_DONTWAIT, (struct sockaddr *)&source_address, &addr_len);

      if (result > 0) {
         BytesRead = size_t(result);
         return endpoint_to_ip(source_address, SourceAddress);
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

   ERR resolve_address(CSTRING Key, const IPAddress &Address, HostLookupResult &Result) override
   {
      char host_name[256];
      char service[128];
      int result;

      if (Address.Type IS IPADDR::V4) {
         struct sockaddr_in sa;
         kt::clearmem(&sa, sizeof(sa));
         sa.sin_family = AF_INET;
         sa.sin_addr.s_addr = host_to_long(Address.Data[0]);
         result = getnameinfo((struct sockaddr *)&sa, sizeof(sa), host_name, sizeof(host_name), service,
            sizeof(service), NI_NAMEREQD);
      }
      else {
         struct sockaddr_in6 sa;
         kt::clearmem(&sa, sizeof(sa));
         sa.sin6_family = AF_INET6;
         kt::copymem((CPTR)Address.Data, sa.sin6_addr.s6_addr, 16);
         result = getnameinfo((struct sockaddr *)&sa, sizeof(sa), host_name, sizeof(host_name), service,
            sizeof(service), NI_NAMEREQD);
      }

      if (result) return convert_lookup_error(result);

      Result.HostName = host_name;
      Result.Addresses.clear();
      Result.Addresses.push_back(Address);
      return ERR::Okay;
   }

   ERR resolve_name(CSTRING HostName, HostLookupResult &Result) override
   {
      struct addrinfo hints;
      struct addrinfo *servinfo = nullptr;

      kt::clearmem(&hints, sizeof(hints));
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_flags = AI_CANONNAME;

      auto lookup_result = getaddrinfo(HostName, nullptr, &hints, &servinfo);
      if (lookup_result) return convert_lookup_error(lookup_result);

      if (servinfo->ai_canonname) Result.HostName = servinfo->ai_canonname;
      else Result.HostName = HostName;
      Result.Addresses.clear();

      for (auto scan = servinfo; scan; scan = scan->ai_next) {
         if (!scan->ai_addr) continue;

         IPAddress ip_address;
         kt::clearmem(&ip_address, sizeof(ip_address));

         if (scan->ai_family IS AF_INET) {
            auto addr = (struct sockaddr_in *)scan->ai_addr;
            ip_address.Type = IPADDR::V4;
            ip_address.Data[0] = long_to_host(addr->sin_addr.s_addr);
            Result.Addresses.push_back(ip_address);
         }
         else if (scan->ai_family IS AF_INET6) {
            auto addr = (struct sockaddr_in6 *)scan->ai_addr;
            ip_address.Type = IPADDR::V6;
            kt::copymem(addr->sin6_addr.s6_addr, ip_address.Data, 16);
            Result.Addresses.push_back(ip_address);
         }
      }

      freeaddrinfo(servinfo);
      return ERR::Okay;
   }

   ERR sync_host_proxies(objConfig *Config) override
   {
      return ERR::Okay;
   }

   ERR save_host_proxy(CSTRING Server, int ServerPort, int Port, bool Enabled) override
   {
      return ERR::Okay;
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
