#ifdef _WIN32

#include "net_platform.h"
#include "win32/winsockwrappers.h"

class WindowsNet : public NetworkPlatform {
public:
   ERR initialise(OBJECTPTR Module) override
   {
      kt::Log log(__FUNCTION__);

      CSTRING msg;
      if ((msg = StartupWinsock()) != 0) {
         log.warning("Winsock initialisation failed: %s", msg);
         return ERR::SystemCall;
      }

      SetResourcePtr(RES::NET_PROCESSING, (APTR)win_net_processing);
      return ERR::Okay;
   }

   void expunge() override
   {
      kt::Log log(__FUNCTION__);

      SetResourcePtr(RES::NET_PROCESSING, nullptr);
      log.msg("Closing winsock.");
      if (ShutdownWinsock() != 0) log.warning("Warning: Winsock DLL Cleanup failed.");
   }

   int socket_limit() const override
   {
      return 0x7fffffff;
   }

   SocketHandle create_socket(void *Reference, bool Read, bool Write, bool UDP, bool &IPv6) override
   {
      return SocketHandle(win_socket_ipv6(Reference, Read, Write, IPv6, UDP));
   }

   void close_socket(SocketHandle Handle) override
   {
      win_closesocket(Handle.socket());
   }

   void deregister_socket(SocketHandle Handle) override
   {
      win_deregister_socket(Handle.socket());
   }

   int shutdown_socket(SocketHandle Handle, int How) override
   {
      return win_shutdown(Handle.socket(), How);
   }

   ERR connect(SocketHandle Handle, const socket_endpoint &Endpoint) override
   {
      return win_connect(Handle.socket(), (struct sockaddr *)&Endpoint.storage, Endpoint.size);
   }

   ERR begin_connect_wait(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return ERR::Okay;
   }

   ERR complete_connect(SocketHandle Handle) override
   {
      return win_socket_connect_complete(Handle.socket());
   }

   ERR bind(SocketHandle Handle, const socket_endpoint &Endpoint) override
   {
      return win_bind(Handle.socket(), (struct sockaddr *)&Endpoint.storage, Endpoint.size);
   }

   ERR listen(SocketHandle Handle, int Backlog) override
   {
      return win_listen(Handle.socket(), Backlog);
   }

   ERR get_local_address(SocketHandle Handle, sockaddr_storage &Address, int &Length) override
   {
      Length = sizeof(Address);
      int result = win_getsockname(Handle.socket(), (struct sockaddr *)&Address, &Length);
      return result ? ERR::SystemCall : ERR::Okay;
   }

   SocketHandle accept(void *Reference, SocketHandle Server, bool IPv6, sockaddr_storage &Address, int &Family) override
   {
      int len = sizeof(Address);
      if (IPv6) return SocketHandle(win_accept_ipv6(Reference, Server.socket(), (struct sockaddr *)&Address, &len, &Family));

      Family = AF_INET;
      return SocketHandle(win_accept(Reference, Server.socket(), (struct sockaddr *)&Address, &len));
   }

   void set_socket_reference(SocketHandle Handle, void *Reference) override
   {
      win_socket_reference(Handle.socket(), Reference);
   }

   ERR set_non_blocking(SocketHandle Handle) override
   {
      return ERR::Okay;
   }

   ERR register_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return win_socketstate(Handle.socket(), true, std::nullopt);
   }

   ERR register_accept(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return ERR::Okay;
   }

   ERR register_write(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return win_socketstate(Handle.socket(), std::nullopt, true);
   }

   ERR remove_write(SocketHandle Handle) override
   {
      return win_socketstate(Handle.socket(), std::nullopt, false);
   }

   ERR register_recall_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return win_socketstate(Handle.socket(), true, std::nullopt);
   }

   ERR deregister_fd(SocketHandle Handle) override
   {
      return ERR::Okay;
   }

   ERR enable_broadcast(SocketHandle Handle) override
   {
      return win_enable_broadcast(Handle.socket());
   }

   ERR set_multicast_ttl(SocketHandle Handle, int TTL, bool IPv6) override
   {
      return win_set_multicast_ttl(Handle.socket(), TTL, IPv6);
   }

   ERR join_multicast_group(SocketHandle Handle, CSTRING Group, bool IPv6) override
   {
      return win_join_multicast_group(Handle.socket(), Group, IPv6);
   }

   ERR leave_multicast_group(SocketHandle Handle, CSTRING Group, bool IPv6) override
   {
      return win_leave_multicast_group(Handle.socket(), Group, IPv6);
   }

   ERR receive(SocketHandle Handle, APTR Buffer, size_t Length, size_t &Received) override
   {
      return WIN_RECEIVE(Handle.socket(), Buffer, Length, &Received);
   }

   ERR append_receive(SocketHandle Handle, std::vector<uint8_t> &Buffer, size_t Length, size_t &Received) override
   {
      return WIN_APPEND(Handle.socket(), Buffer, Length, Received);
   }

   ERR send(SocketHandle Handle, CPTR Buffer, size_t &Length) override
   {
      return WIN_SEND(Handle.socket(), Buffer, &Length, 0);
   }

   ERR send_to(SocketHandle Handle, CPTR Buffer, size_t &Length, const socket_endpoint &Endpoint) override
   {
      return WIN_SENDTO(Handle.socket(), Buffer, &Length, (struct sockaddr *)&Endpoint.storage, Endpoint.size);
   }

   ERR receive_from(SocketHandle Handle, APTR Buffer, size_t BufferSize, size_t &BytesRead,
      sockaddr_storage &SourceAddress, int &AddressLength) override
   {
      return WIN_RECVFROM(Handle.socket(), Buffer, BufferSize, &BytesRead, (struct sockaddr *)&SourceAddress,
         &AddressLength);
   }

   uint32_t inet_addr(CSTRING Value) override
   {
      return win_inet_addr(Value);
   }

   int inet_pton(int Family, CSTRING Source, APTR Dest) override
   {
      return win_inet_pton(Family, Source, Dest);
   }

   CSTRING inet_ntop(int Family, CPTR Source, STRING Dest, size_t Size) override
   {
      return win_inet_ntop(Family, Source, Dest, Size);
   }

   uint32_t host_to_long(uint32_t Value) override
   {
      return win_htonl(Value);
   }

   uint32_t long_to_host(uint32_t Value) override
   {
      return win_ntohl(Value);
   }

   uint16_t host_to_short(uint16_t Value) override
   {
      return win_htons(Value);
   }

   uint16_t short_to_host(uint16_t Value) override
   {
      return win_ntohs(Value);
   }
};

std::unique_ptr<NetworkPlatform> create_platform()
{
   return std::make_unique<WindowsNet>();
}

#endif
