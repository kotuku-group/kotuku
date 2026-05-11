#ifdef _WIN32

#include "net_platform.h"
#include "win32/winsockwrappers.h"

#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

#define HKEY_PROXY "\\HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\"

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
   return _stricmp(Left, Right) IS 0;
}

static ERR endpoint_to_ip(const struct sockaddr_storage &Address, IPAddress &IP)
{
   kt::clearmem(&IP, sizeof(IP));

   if (Address.ss_family IS AF_INET) {
      auto addr = (const struct sockaddr_in *)&Address;
      IP.Type = IPADDR::V4;
      IP.Port = win_ntohs(addr->sin_port);
      IP.Data[0] = win_ntohl(addr->sin_addr.s_addr);
      return ERR::Okay;
   }
   else if (Address.ss_family IS AF_INET6) {
      auto addr = (const struct sockaddr_in6 *)&Address;
      IP.Type = IPADDR::V6;
      IP.Port = win_ntohs(addr->sin6_port);
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
      case EAI_SYSTEM: return ERR::SystemCall;
      default: return ERR::Failed;
   }
}

struct WindowsProxyEntry {
   std::string Name;
   std::string Server;
   int Port = 0;
   int ServerPort = 0;
   bool Enabled = false;
};

static std::optional<int> parse_proxy_port(std::string_view Value)
{
   int port = 0;
   auto result = std::from_chars(Value.data(), Value.data() + Value.size(), port);
   if ((result.ec != std::errc()) or (result.ptr != Value.data() + Value.size())) return std::nullopt;
   return port;
}

static std::optional<WindowsProxyEntry> parse_proxy_entry(std::string_view Entry, bool Enabled)
{
   static constexpr std::array<std::pair<std::string_view, int>, 3> protocol_ports = {{
      {"ftp", 21}, {"http", 80}, {"https", 443}
   }};

   auto equal_pos = Entry.find('=');
   if (equal_pos != std::string_view::npos) {
      auto protocol = Entry.substr(0, equal_pos);
      auto server_part = Entry.substr(equal_pos + 1);
      auto colon_pos = server_part.find(':');
      if (colon_pos IS std::string_view::npos) return std::nullopt;

      auto port = parse_proxy_port(server_part.substr(colon_pos + 1));
      if (!port) return std::nullopt;

      for (const auto &protocol_port : protocol_ports) {
         if (protocol_port.first IS protocol) {
            return WindowsProxyEntry {
               std::format("Windows {}", protocol),
               std::string(server_part.substr(0, colon_pos)),
               protocol_port.second,
               *port,
               Enabled
            };
         }
      }
   }
   else {
      auto colon_pos = Entry.find(':');
      if (colon_pos IS std::string_view::npos) return std::nullopt;

      auto port = parse_proxy_port(Entry.substr(colon_pos + 1));
      if (!port) return std::nullopt;

      return WindowsProxyEntry {
         "Windows",
         std::string(Entry.substr(0, colon_pos)),
         0,
         *port,
         Enabled
      };
   }

   return std::nullopt;
}

static std::vector<WindowsProxyEntry> parse_proxy_string(std::string_view Servers, bool Enabled)
{
   std::vector<WindowsProxyEntry> entries;

   size_t pos = 0;
   while (pos < Servers.length()) {
      while ((pos < Servers.length()) and (Servers[pos] IS ';')) ++pos;
      if (pos >= Servers.length()) break;

      size_t start = pos;
      while ((pos < Servers.length()) and (Servers[pos] != ';')) ++pos;

      if (auto entry = parse_proxy_entry(Servers.substr(start, pos - start), Enabled)) entries.push_back(*entry);
   }

   return entries;
}

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

   SocketHandle socket_from_hosthandle(HOSTHANDLE Handle) override
   {
      return SocketHandle(Handle);
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
            if (win_inet_pton(AF_INET6, Address, &ipv6_addr) != 1) return ERR::InvalidValue;
            ip.Type = IPADDR::V6;
            kt::copymem(ipv6_addr.s6_addr, ip.Data, 16);
         }
         else {
            uint32_t result = win_inet_addr(Address);
            if (result IS INADDR_NONE) return ERR::InvalidValue;
            ip.Type = IPADDR::V4;
            ip.Data[0] = win_ntohl(result);
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
      return win_connect(Handle.socket(), (struct sockaddr *)&endpoint_storage(Endpoint), Endpoint.Size);
   }

   ERR begin_connect_wait(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) override
   {
      return ERR::Okay;
   }

   ERR complete_connect(SocketHandle Handle) override
   {
      return win_socket_connect_complete(Handle.socket());
   }

   ERR bind(SocketHandle Handle, const NetworkEndpoint &Endpoint) override
   {
      return win_bind(Handle.socket(), (struct sockaddr *)&endpoint_storage(Endpoint), Endpoint.Size);
   }

   ERR listen(SocketHandle Handle, int Backlog) override
   {
      return win_listen(Handle.socket(), Backlog);
   }

   ERR get_local_ip(SocketHandle Handle, IPAddress &Address) override
   {
      struct sockaddr_storage storage;
      int length = sizeof(storage);
      int result = win_getsockname(Handle.socket(), (struct sockaddr *)&storage, &length);
      if (result) return ERR::SystemCall;
      return endpoint_to_ip(storage, Address);
   }

   AcceptedSocket accept(void *Reference, SocketHandle Server, bool IPv6) override
   {
      AcceptedSocket accepted;
      struct sockaddr_storage address;
      int len = sizeof(address);

      if (IPv6) accepted.Handle = SocketHandle(win_accept_ipv6(Reference, Server.socket(), (struct sockaddr *)&address,
         &len, &accepted.Family));
      else {
         accepted.Family = AF_INET;
         accepted.Handle = SocketHandle(win_accept(Reference, Server.socket(), (struct sockaddr *)&address, &len));
      }

      if (accepted.Handle.is_valid()) {
         if (copy_accepted_ip(address, accepted.Family, accepted.IP) != ERR::Okay) {
            close_socket(accepted.Handle);
            accepted.Handle = SocketHandle();
         }
      }

      return accepted;
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

   ERR remove_read(SocketHandle Handle) override
   {
      return win_socketstate(Handle.socket(), false, std::nullopt);
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

   ERR parse_multicast_group(CSTRING Group, bool &IPv6) override
   {
      if (!Group) return ERR::Args;

      struct in6_addr addr6;
      struct in_addr addr4;
      kt::clearmem(&addr6, sizeof(addr6));
      kt::clearmem(&addr4, sizeof(addr4));

      if (win_inet_pton(AF_INET6, Group, &addr6) IS 1) {
         IPv6 = true;
         return ERR::Okay;
      }
      else if (win_inet_pton(AF_INET, Group, &addr4) IS 1) {
         IPv6 = false;
         return ERR::Okay;
      }
      else return ERR::Args;
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

   ERR send_to(SocketHandle Handle, CPTR Buffer, size_t &Length, const NetworkEndpoint &Endpoint) override
   {
      return WIN_SENDTO(Handle.socket(), Buffer, &Length, (struct sockaddr *)&endpoint_storage(Endpoint),
         Endpoint.Size);
   }

   ERR receive_from(SocketHandle Handle, APTR Buffer, size_t BufferSize, size_t &BytesRead,
      IPAddress &SourceAddress) override
   {
      struct sockaddr_storage source_address;
      int address_length = sizeof(source_address);
      auto error = WIN_RECVFROM(Handle.socket(), Buffer, BufferSize, &BytesRead, (struct sockaddr *)&source_address,
         &address_length);
      if ((error IS ERR::Okay) and (BytesRead > 0)) return endpoint_to_ip(source_address, SourceAddress);
      return error;
   }

   ERR resolve_address(CSTRING Key, const IPAddress &Address, HostLookupResult &Result) override
   {
      auto host = win_gethostbyaddr(&Address);
      if (!host) return ERR::Failed;

      Result.HostName = host->h_name ? host->h_name : Key;
      Result.Addresses.clear();

      if (host->h_addr_list) {
         if (host->h_addrtype IS AF_INET) {
            for (unsigned i = 0; host->h_addr_list[i]; ++i) {
               auto addr = *((uint32_t *)host->h_addr_list[i]);
               Result.Addresses.push_back({ win_ntohl(addr), 0, 0, 0, IPADDR::V4 });
            }
         }
         else if (host->h_addrtype IS AF_INET6) {
            for (unsigned i = 0; host->h_addr_list[i]; ++i) {
               auto addr = ((struct in6_addr **)host->h_addr_list)[i];
               IPAddress ip_address = { 0, 0, 0, 0, IPADDR::V6 };
               kt::copymem(addr->s6_addr, ip_address.Data, 16);
               Result.Addresses.push_back(ip_address);
            }
         }
      }

      if (Result.Addresses.empty()) Result.Addresses.push_back(Address);
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

      auto lookup_result = win_getaddrinfo(HostName, nullptr, &hints, &servinfo);
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
            ip_address.Data[0] = win_ntohl(addr->sin_addr.s_addr);
            Result.Addresses.push_back(ip_address);
         }
         else if (scan->ai_family IS AF_INET6) {
            auto addr = (struct sockaddr_in6 *)scan->ai_addr;
            ip_address.Type = IPADDR::V6;
            kt::copymem(addr->sin6_addr.s6_addr, ip_address.Data, 16);
            Result.Addresses.push_back(ip_address);
         }
      }

      win_freeaddrinfo(servinfo);
      return ERR::Okay;
   }

   ERR sync_host_proxies(objConfig *Config) override
   {
      kt::Log log(__FUNCTION__);

      if (!Config) return ERR::NullArgs;

      ConfigGroups *groups;
      if (Config->get(FID_Data, groups) IS ERR::Okay) {
         std::vector<std::string> host_groups;
         for (const auto &[group, keys] : groups[0]) {
            if (keys.contains("Host")) host_groups.push_back(group);
         }

         for (const auto &group : host_groups) {
            Config->deleteGroup(group.c_str());
         }
      }

      auto task = CurrentTask();
      CSTRING value;
      if (task->getEnv(HKEY_PROXY "ProxyEnable", &value) != ERR::Okay) {
         log.msg("Host does not have proxies enabled (registry setting: %s)", HKEY_PROXY);
         return ERR::Okay;
      }

      bool enabled = (strtol(value, nullptr, 0) > 0);

      CSTRING servers;
      if ((task->getEnv(HKEY_PROXY "ProxyServer", &servers) IS ERR::Okay) and servers[0]) {
         log.msg("Host has defined default proxies: %s", servers);

         auto proxy_entries = parse_proxy_string(servers, enabled);

         int id = 0;
         Config->read("ID", "Value", id);

         for (const auto &entry : proxy_entries) {
            ++id;
            Config->write("ID", "Value", std::to_string(id));

            std::string group = std::to_string(id);
            Config->write(group, "Name", entry.Name);
            Config->write(group, "Server", entry.Server);
            Config->write(group, "Port", std::to_string(entry.Port));
            Config->write(group, "ServerPort", std::to_string(entry.ServerPort));
            Config->write(group, "Enabled", std::to_string(entry.Enabled ? 1 : 0));
            Config->write(group, "Host", "1");

            log.trace("Added Windows proxy: %s -> %s:%d", entry.Name.c_str(), entry.Server.c_str(), entry.ServerPort);
         }
      }

      return ERR::Okay;
   }

   ERR save_host_proxy(CSTRING Server, int ServerPort, int Port, bool Enabled) override
   {
      kt::Log log(__FUNCTION__);
      objTask *task = CurrentTask();

      ERR error;
      if (Enabled) error = task->setEnv(HKEY_PROXY "ProxyEnable", "1");
      else error = task->setEnv(HKEY_PROXY "ProxyEnable", "0");
      if (error != ERR::Okay) return log.warning(error);

      if ((!Server) or (!Server[0])) {
         log.trace("Clearing proxy server value.");
         if (auto error = task->setEnv(HKEY_PROXY "ProxyServer", ""); error != ERR::Okay) return log.warning(error);
      }
      else if (Port IS 0) {
         std::string buffer = std::format("{}:{}", Server, ServerPort);
         log.trace("Changing all-port proxy to: %s", buffer.c_str());
         if (auto error = task->setEnv(HKEY_PROXY "ProxyServer", buffer.c_str()); error != ERR::Okay) {
            return log.warning(error);
         }
      }
      else {
         std::string port_name;
         switch(Port) {
            case 21: port_name = "ftp"; break;
            case 80: port_name = "http"; break;
            case 443: port_name = "https"; break;
         }

         if (!port_name.empty()) {
            CSTRING servers;
            task->getEnv(HKEY_PROXY "ProxyServer", &servers);
            std::string server_list = servers ? servers : "";

            const std::string search_pattern = std::format("{}=", port_name);
            if (auto pos = server_list.find(search_pattern); pos != std::string::npos) {
               auto end_pos = server_list.find(';', pos);
               if (end_pos IS std::string::npos) end_pos = server_list.length();
               else ++end_pos;
               server_list.erase(pos, end_pos - pos);
            }

            const std::string new_entry = std::format("{}={}:{}", port_name, Server, ServerPort);
            if (!server_list.empty() and server_list.back() != ';') {
               server_list += ';';
            }
            server_list += new_entry;

            if (auto error = task->setEnv(HKEY_PROXY "ProxyServer", server_list.c_str()); error != ERR::Okay) {
               return log.warning(error);
            }
         }
         else return log.error(ERR::NoSupport);
      }

      return ERR::Okay;
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
