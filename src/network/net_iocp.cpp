#ifdef _WIN32

#include "net_platform.h"
#include "win32/iocp.h"

static_assert(sizeof(struct sockaddr_storage) <= NETWORK_ENDPOINT_STORAGE_SIZE);

static MsgHandler *glIocpCompletionHandler = nullptr;
static MSGID glIocpCompletionMsgID = MSGID::NIL;

static constexpr int IOCP_NI_NAMEREQD = 0x04;

#ifndef WSAAPI
   #define WSAAPI __stdcall
#endif

extern "C" {
   int WSAAPI getaddrinfo(const char *NodeName, const char *ServiceName, const struct addrinfo *Hints,
      struct addrinfo **Result);
   void WSAAPI freeaddrinfo(struct addrinfo *Result);
   int WSAAPI getnameinfo(const struct sockaddr *SockAddr, int SockAddrLength, char *HostName,
      unsigned long HostNameLength, char *ServiceName, unsigned long ServiceNameLength, int Flags);
   const char * WSAAPI inet_ntop(int Family, const void *Address, char *StringBuffer, size_t StringBufferLength);
}

//********************************************************************************************************************

static struct sockaddr_storage & endpoint_storage(NetworkEndpoint &Endpoint)
{
   return *(struct sockaddr_storage *)Endpoint.Storage;
}

static const struct sockaddr_storage & endpoint_storage(const NetworkEndpoint &Endpoint)
{
   return *(const struct sockaddr_storage *)Endpoint.Storage;
}

//********************************************************************************************************************

static ERR endpoint_to_ip(const struct sockaddr_storage &Address, IPAddress &IP)
{
   kt::clearmem(&IP, sizeof(IP));

   if (Address.ss_family IS AF_INET) {
      auto addr = (const struct sockaddr_in *)&Address;
      IP.Type = IPADDR::V4;
      IP.Port = iocp_ntohs(addr->sin_port);
      IP.Data[0] = iocp_ntohl(addr->sin_addr.s_addr);
      return ERR::Okay;
   }
   else if (Address.ss_family IS AF_INET6) {
      auto addr = (const struct sockaddr_in6 *)&Address;
      IP.Type = IPADDR::V6;
      IP.Port = iocp_ntohs(addr->sin6_port);
      kt::copymem((APTR)addr->sin6_addr.s6_addr, IP.Data, 16);
      return ERR::Okay;
   }
   else return ERR::Args;
}

//********************************************************************************************************************

static ERR convert_lookup_error(int Result)
{
   switch (Result) {
      case 0: return ERR::Okay;
      case EAI_AGAIN: return ERR::Retry;
      case EAI_FAIL: return ERR::Failed;
      case EAI_MEMORY: return ERR::Memory;
      case EAI_SYSTEM: return ERR::SystemCall;
      default: return ERR::Failed;
   }
}

//********************************************************************************************************************

static ERR iocp_completion_receiver(APTR Custom, MSGID MsgID, int MsgType, APTR Message, int MsgSize)
{
   (void)Custom;
   (void)MsgID;
   (void)MsgType;

   if ((!Message) or (MsgSize != int(sizeof(iocp_completion_message)))) return ERR::Okay;

   auto completion = (iocp_completion_message *)Message;
   if ((!completion->Callback) or (completion->ObjectID <= 0)) return ERR::Okay;
   if (!iocp_validate_completion(completion->Socket, completion->Generation)) return ERR::Okay;

   kt::ScopedObjectLock lock(completion->ObjectID);
   if (!lock.granted()) return ERR::Okay;

   if ((completion->Type != IocpOperationType::CONNECT) and (completion->Type != IocpOperationType::READ) and
       (completion->Type != IocpOperationType::WRITE) and (completion->Type != IocpOperationType::ACCEPT)) {
      return ERR::Okay;
   }

   auto callback = (void (*)(HOSTHANDLE, APTR))completion->Callback;
   callback(SocketHandle(completion->Socket).hosthandle(), lock.obj);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR post_iocp_completion(int MsgID, const void *Message, int Size)
{
   return SendMessage(MSGID(MsgID), MSF::NIL, (APTR)Message, Size);
}

//********************************************************************************************************************

class IocpNet : public NetworkPlatform {
public:
   ERR initialise(OBJECTPTR Module) override
   {
      (void)Module;

      glIocpCompletionMsgID = (MSGID)AllocateID(IDTYPE::MESSAGE);

      auto completion_function = C_FUNCTION(iocp_completion_receiver);
      completion_function.Context = CurrentTask();
      if (AddMsgHandler(glIocpCompletionMsgID, &completion_function, &glIocpCompletionHandler) != ERR::Okay) {
         return ERR::Failed;
      }

      return iocp_initialise(int(glIocpCompletionMsgID), post_iocp_completion);
   }

   void expunge() override
   {
      if (glIocpCompletionHandler) {
         FreeResource(glIocpCompletionHandler);
         glIocpCompletionHandler = nullptr;
      }

      iocp_expunge();
   }

   int socket_limit() const override
   {
      return 0x7fffffff;
   }

   SocketHandle create_socket(void *Reference, bool Read, bool Write, bool UDP, bool &IPv6) override
   {
      (void)Read;
      (void)Write;
      return SocketHandle(iocp_create_socket(Reference, UDP, IPv6));
   }

   SocketHandle socket_from_hosthandle(HOSTHANDLE Handle) override
   {
      return SocketHandle(Handle);
   }

   void close_socket(SocketHandle Handle) override
   {
      iocp_close_socket(Handle.socket());
   }

   void deregister_socket(SocketHandle Handle) override
   {
      iocp_deregister_socket(Handle.socket());
   }

   int shutdown_socket(SocketHandle Handle, int How) override
   {
      return iocp_shutdown_socket(Handle.socket(), How);
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
         Endpoint.Family = IPADDR::V6;
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
            Endpoint.Family = IPADDR::V6;
            Endpoint.Label = "IPv4-mapped IPv6";
            return ERR::Okay;
         }
         else {
            auto addr4 = (struct sockaddr_in *)&storage;
            addr4->sin_family = AF_INET;
            addr4->sin_port = host_to_short(uint16_t(Port));
            addr4->sin_addr.s_addr = host_to_long(IP.Data[0]);

            Endpoint.Size = sizeof(struct sockaddr_in);
            Endpoint.Family = IPADDR::V4;
            Endpoint.Label = "IPv4";
            return ERR::Okay;
         }
      }
      else return ERR::InvalidData;
   }

   ERR connect(SocketHandle Handle, const NetworkEndpoint &Endpoint) override
   {
      return iocp_prepare_connect(Handle.socket(), &endpoint_storage(Endpoint), Endpoint.Size);
   }

   ERR begin_connect_wait(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      auto object_id = Data ? ((OBJECTPTR)Data)->UID : 0;
      return iocp_begin_connect_wait(Handle.socket(), object_id, uintptr_t(Callback), uintptr_t(Data));
   }

   ERR complete_connect(SocketHandle Handle) override
   {
      return iocp_complete_connect(Handle.socket());
   }

   ERR bind(SocketHandle Handle, const NetworkEndpoint &Endpoint) override
   {
      return iocp_bind(Handle.socket(), &endpoint_storage(Endpoint), Endpoint.Size);
   }

   ERR listen(SocketHandle Handle, int Backlog) override
   {
      return iocp_listen(Handle.socket(), Backlog);
   }

   ERR get_local_ip(SocketHandle Handle, IPAddress &Address) override
   {
      NetworkEndpoint endpoint;
      kt::clearmem(&endpoint, sizeof(endpoint));
      int length = sizeof(endpoint.Storage);

      if (auto error = iocp_get_local_ip(Handle.socket(), endpoint.Storage, &length); error != ERR::Okay) return error;
      return endpoint_to_ip(endpoint_storage(endpoint), Address);
   }

   AcceptedSocket accept(void *Reference, SocketHandle Server, bool IPv6) override
   {
      (void)Reference;
      (void)IPv6;

      AcceptedSocket accepted;
      NetworkEndpoint endpoint;
      kt::clearmem(&endpoint, sizeof(endpoint));
      int length = sizeof(endpoint.Storage);
      WSW_SOCKET client = 0;

      if (iocp_accept(Server.socket(), client, endpoint.Storage, &length) != ERR::Okay) {
         return AcceptedSocket();
      }

      accepted.Handle = SocketHandle(client);
      endpoint.Size = length;
      if (endpoint_to_ip(endpoint_storage(endpoint), accepted.Address) != ERR::Okay) {
         iocp_close_socket(accepted.Handle.socket());
         return AcceptedSocket();
      }

      return accepted;
   }

   void set_socket_reference(SocketHandle Handle, void *Reference) override
   {
      iocp_set_socket_reference(Handle.socket(), Reference);
   }

   ERR set_non_blocking(SocketHandle Handle) override
   {
      (void)Handle;
      return ERR::Okay;
   }

   ERR register_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      auto object_id = Data ? ((OBJECTPTR)Data)->UID : 0;
      return iocp_register_read(Handle.socket(), object_id, uintptr_t(Callback), uintptr_t(Data));
   }

   ERR register_accept(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      auto object_id = Data ? ((OBJECTPTR)Data)->UID : 0;
      return iocp_register_accept(Handle.socket(), object_id, uintptr_t(Callback), uintptr_t(Data));
   }

   ERR register_write(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      auto object_id = Data ? ((OBJECTPTR)Data)->UID : 0;
      return iocp_register_write(Handle.socket(), object_id, uintptr_t(Callback), uintptr_t(Data));
   }

   ERR remove_read(SocketHandle Handle) override
   {
      return iocp_remove_read(Handle.socket());
   }

   ERR remove_write(SocketHandle Handle) override
   {
      return iocp_remove_write(Handle.socket());
   }

   ERR register_recall_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      auto object_id = Data ? ((OBJECTPTR)Data)->UID : 0;
      return iocp_recall_read(Handle.socket(), object_id, uintptr_t(Callback), uintptr_t(Data));
   }

   ERR deregister_fd(SocketHandle Handle) override
   {
      iocp_deregister_socket(Handle.socket());
      return ERR::Okay;
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
      return iocp_receive(Handle.socket(), Buffer, Length, Received);
   }

   ERR append_receive(SocketHandle Handle, std::vector<uint8_t> &Buffer, size_t Length, size_t &Received) override
   {
      return iocp_append_receive(Handle.socket(), Buffer, Length, Received);
   }

   ERR send(SocketHandle Handle, CPTR Buffer, size_t &Length) override
   {
      return iocp_send(Handle.socket(), Buffer, Length);
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

      char host_name[256];
      char service[128];
      int result;

      if (Address.Type IS IPADDR::V4) {
         struct sockaddr_in sa;
         kt::clearmem(&sa, sizeof(sa));
         sa.sin_family = AF_INET;
         sa.sin_addr.s_addr = host_to_long(Address.Data[0]);
         result = getnameinfo((struct sockaddr *)&sa, sizeof(sa), host_name, sizeof(host_name), service,
            sizeof(service), IOCP_NI_NAMEREQD);
      }
      else if (Address.Type IS IPADDR::V6) {
         struct sockaddr_in6 sa;
         kt::clearmem(&sa, sizeof(sa));
         sa.sin6_family = AF_INET6;
         kt::copymem((CPTR)Address.Data, sa.sin6_addr.s6_addr, 16);
         result = getnameinfo((struct sockaddr *)&sa, sizeof(sa), host_name, sizeof(host_name), service,
            sizeof(service), IOCP_NI_NAMEREQD);
      }
      else return ERR::Args;

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
      if (!servinfo) return ERR::Failed;

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
      (void)Config;
      return ERR::Okay;
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
      if (Address.Type IS IPADDR::V6) return inet_ntop(AF_INET6, Address.Data, Dest, Size);
      else if (Address.Type IS IPADDR::V4) {
         struct in_addr addr;
         addr.s_addr = host_to_long(Address.Data[0]);
         return inet_ntop(AF_INET, &addr, Dest, Size);
      }
      else return nullptr;
   }

   uint32_t host_to_long(uint32_t Value) override
   {
      return iocp_htonl(Value);
   }

   uint32_t long_to_host(uint32_t Value) override
   {
      return iocp_ntohl(Value);
   }

   uint16_t host_to_short(uint16_t Value) override
   {
      return iocp_htons(Value);
   }

   uint16_t short_to_host(uint16_t Value) override
   {
      return iocp_ntohs(Value);
   }
};

std::unique_ptr<NetworkPlatform> create_platform()
{
   return std::make_unique<IocpNet>();
}

#endif
