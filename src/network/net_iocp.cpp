#ifdef _WIN32

#include "net_platform.h"

class IocpNet : public NetworkPlatform {
public:
   ERR initialise(OBJECTPTR Module) override
   {
      (void)Module;
      return ERR::Okay;
   }

   void expunge() override
   {
   }

   int socket_limit() const override
   {
      return 0x7fffffff;
   }

   SocketHandle create_socket(void *Reference, bool Read, bool Write, bool UDP, bool &IPv6) override
   {
      (void)Reference;
      (void)Read;
      (void)Write;
      (void)UDP;
      IPv6 = false;
      return SocketHandle();
   }

   SocketHandle socket_from_hosthandle(HOSTHANDLE Handle) override
   {
      (void)Handle;
      return SocketHandle();
   }

   void close_socket(SocketHandle Handle) override
   {
      (void)Handle;
   }

   void deregister_socket(SocketHandle Handle) override
   {
      (void)Handle;
   }

   int shutdown_socket(SocketHandle Handle, int How) override
   {
      (void)Handle;
      (void)How;
      return SOCKET_ERROR;
   }

   ERR build_address(const IPAddress &IP, int Port, bool IPv6, NetworkEndpoint &Endpoint) override
   {
      (void)IP;
      (void)Port;
      (void)IPv6;
      (void)Endpoint;
      return ERR::NoSupport;
   }

   ERR connect(SocketHandle Handle, const NetworkEndpoint &Endpoint) override
   {
      (void)Handle;
      (void)Endpoint;
      return ERR::NoSupport;
   }

   ERR begin_connect_wait(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      (void)Handle;
      (void)Callback;
      (void)Data;
      return ERR::NoSupport;
   }

   ERR complete_connect(SocketHandle Handle) override
   {
      (void)Handle;
      return ERR::NoSupport;
   }

   ERR bind(SocketHandle Handle, const NetworkEndpoint &Endpoint) override
   {
      (void)Handle;
      (void)Endpoint;
      return ERR::NoSupport;
   }

   ERR listen(SocketHandle Handle, int Backlog) override
   {
      (void)Handle;
      (void)Backlog;
      return ERR::NoSupport;
   }

   ERR get_local_ip(SocketHandle Handle, IPAddress &Address) override
   {
      (void)Handle;
      (void)Address;
      return ERR::NoSupport;
   }

   AcceptedSocket accept(void *Reference, SocketHandle Server, bool IPv6) override
   {
      (void)Reference;
      (void)Server;
      (void)IPv6;
      return AcceptedSocket();
   }

   void set_socket_reference(SocketHandle Handle, void *Reference) override
   {
      (void)Handle;
      (void)Reference;
   }

   ERR set_non_blocking(SocketHandle Handle) override
   {
      (void)Handle;
      return ERR::NoSupport;
   }

   ERR register_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      (void)Handle;
      (void)Callback;
      (void)Data;
      return ERR::NoSupport;
   }

   ERR register_accept(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      (void)Handle;
      (void)Callback;
      (void)Data;
      return ERR::NoSupport;
   }

   ERR register_write(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      (void)Handle;
      (void)Callback;
      (void)Data;
      return ERR::NoSupport;
   }

   ERR remove_read(SocketHandle Handle) override
   {
      (void)Handle;
      return ERR::NoSupport;
   }

   ERR remove_write(SocketHandle Handle) override
   {
      (void)Handle;
      return ERR::NoSupport;
   }

   ERR register_recall_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      (void)Handle;
      (void)Callback;
      (void)Data;
      return ERR::NoSupport;
   }

   ERR deregister_fd(SocketHandle Handle) override
   {
      (void)Handle;
      return ERR::NoSupport;
   }

   ERR enable_broadcast(SocketHandle Handle) override
   {
      (void)Handle;
      return ERR::NoSupport;
   }

   ERR set_multicast_ttl(SocketHandle Handle, int TTL, bool IPv6) override
   {
      (void)Handle;
      (void)TTL;
      (void)IPv6;
      return ERR::NoSupport;
   }

   ERR parse_multicast_group(CSTRING Group, bool &IPv6) override
   {
      (void)Group;
      IPv6 = false;
      return ERR::NoSupport;
   }

   ERR join_multicast_group(SocketHandle Handle, CSTRING Group, bool IPv6) override
   {
      (void)Handle;
      (void)Group;
      (void)IPv6;
      return ERR::NoSupport;
   }

   ERR leave_multicast_group(SocketHandle Handle, CSTRING Group, bool IPv6) override
   {
      (void)Handle;
      (void)Group;
      (void)IPv6;
      return ERR::NoSupport;
   }

   ERR receive(SocketHandle Handle, APTR Buffer, size_t Length, size_t &Received) override
   {
      (void)Handle;
      (void)Buffer;
      (void)Length;
      Received = 0;
      return ERR::NoSupport;
   }

   ERR append_receive(SocketHandle Handle, std::vector<uint8_t> &Buffer, size_t Length, size_t &Received) override
   {
      (void)Handle;
      (void)Buffer;
      (void)Length;
      Received = 0;
      return ERR::NoSupport;
   }

   ERR send(SocketHandle Handle, CPTR Buffer, size_t &Length) override
   {
      (void)Handle;
      (void)Buffer;
      Length = 0;
      return ERR::NoSupport;
   }

   ERR send_to(SocketHandle Handle, CPTR Buffer, size_t &Length, const NetworkEndpoint &Endpoint) override
   {
      (void)Handle;
      (void)Buffer;
      (void)Endpoint;
      Length = 0;
      return ERR::NoSupport;
   }

   ERR receive_from(SocketHandle Handle, APTR Buffer, size_t BufferSize, size_t &BytesRead,
      IPAddress &SourceAddress) override
   {
      (void)Handle;
      (void)Buffer;
      (void)BufferSize;
      (void)SourceAddress;
      BytesRead = 0;
      return ERR::NoSupport;
   }

   ERR resolve_address(CSTRING Key, const IPAddress &Address, HostLookupResult &Result) override
   {
      (void)Key;
      (void)Address;
      Result.HostName.clear();
      Result.Addresses.clear();
      return ERR::NoSupport;
   }

   ERR resolve_name(CSTRING HostName, HostLookupResult &Result) override
   {
      (void)HostName;
      Result.HostName.clear();
      Result.Addresses.clear();
      return ERR::NoSupport;
   }

   ERR sync_host_proxies(objConfig *Config) override
   {
      (void)Config;
      return ERR::NoSupport;
   }

   ERR save_host_proxy(CSTRING Server, int ServerPort, int Port, bool Enabled) override
   {
      (void)Server;
      (void)ServerPort;
      (void)Port;
      (void)Enabled;
      return ERR::NoSupport;
   }

   CSTRING address_to_string(const IPAddress &Address, STRING Dest, size_t Size) override
   {
      (void)Address;
      if ((Dest) and (Size > 0)) Dest[0] = 0;
      return nullptr;
   }

   uint32_t host_to_long(uint32_t Value) override
   {
      return Value;
   }

   uint32_t long_to_host(uint32_t Value) override
   {
      return Value;
   }

   uint16_t host_to_short(uint16_t Value) override
   {
      return Value;
   }

   uint16_t short_to_host(uint16_t Value) override
   {
      return Value;
   }
};

std::unique_ptr<NetworkPlatform> create_platform()
{
   return std::make_unique<IocpNet>();
}

#endif
