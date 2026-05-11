#pragma once
#define KOTUKU_NET_PLATFORM_H TRUE

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <kotuku/main.h>
#include <kotuku/modules/network.h>

#ifdef _WIN32
   #define INADDR_NONE 0xffffffff

   #define SOCK_STREAM 1
   #define SOCK_DGRAM 2

   struct hostent {
      char  *h_name;
      char  **h_aliases;
      short h_addrtype;
      short h_length;
      char  **h_addr_list;
      #define h_addr h_addr_list[0]
   };

   struct in_addr {
      union {
         struct { uint8_t s_b1,s_b2,s_b3,s_b4; } S_un_b;
         struct { uint16_t s_w1,s_w2; } S_un_w;
         uint32_t S_addr;
      } S_un;
      #define s_addr S_un.S_addr
      #define s_host S_un.S_un_b.s_b2
      #define s_net S_un.S_un_b.s_b1
      #define s_imp S_un.S_un_w.s_w2
      #define s_impno S_un.S_un_b.s_b4
      #define s_lh S_un.S_un_b.s_b3
   };

   struct sockaddr {
      uint16_t sa_family;
      char sa_data[14];
   };

   struct sockaddr_in {
      short sin_family;
      uint16_t sin_port;
      struct in_addr sin_addr;
      char sin_zero[8];
   };

   struct addrinfo {
      int ai_flags;
      int ai_family;
      int ai_socktype;
      int ai_protocol;
      size_t ai_addrlen;
      char *ai_canonname;
      struct sockaddr *ai_addr;
      struct addrinfo *ai_next;
   };

   struct in6_addr {
      uint8_t s6_addr[16];
   };

   struct sockaddr_in6 {
      short sin6_family;
      uint16_t sin6_port;
      uint32_t sin6_flowinfo;
      struct in6_addr sin6_addr;
      uint32_t sin6_scope_id;
   };

   struct sockaddr_storage {
      short ss_family;
      char __ss_pad1[6];
      int64_t __ss_align;
      char __ss_pad2[112];
   };

   constexpr int SOCKET_ERROR = -1;
   constexpr int AF_INET = 2;
   constexpr int AF_INET6 = 23;
   constexpr int INADDR_ANY = 0;
   constexpr int MSG_PEEK = 2;
   constexpr int IPPROTO_IPV6 = 41;
   constexpr int IPV6_V6ONLY = 27;
   constexpr int AF_UNSPEC = 0;
   constexpr int AI_CANONNAME = 2;
   constexpr int EAI_AGAIN = 2;
   constexpr int EAI_FAIL = 3;
   constexpr int EAI_MEMORY = 4;
   constexpr int EAI_SYSTEM = 5;

   static const struct in6_addr in6addr_any = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}};

   extern "C" {
      int getaddrinfo(const char *Node, const char *Service, const struct addrinfo *Hints, struct addrinfo **Result);
      void freeaddrinfo(struct addrinfo *Result);
   }
#else
   #include <arpa/inet.h>
   #include <netdb.h>
   #include <unistd.h>
   #include <fcntl.h>
   #include <sys/ioctl.h>
   #include <sys/socket.h>
   #include <netinet/tcp.h>
   #include <netinet/in.h>
   #include <errno.h>
#endif

class SocketHandle {
private:
   SOCKET_HANDLE socket_val;

public:
   #ifdef _WIN32
      static constexpr SOCKET_HANDLE INVALID_SOCKET_VAL = SOCKET_HANDLE(~0u);
   #else
      static constexpr SOCKET_HANDLE INVALID_SOCKET_VAL = -1;
   #endif

   SocketHandle() : socket_val(INVALID_SOCKET_VAL) {}
   SocketHandle(SOCKET_HANDLE Socket) : socket_val(Socket) {}
   #ifdef _WIN32
      SocketHandle(int Socket) : socket_val(SOCKET_HANDLE(Socket)) {}
   #endif
   #ifdef _WIN32
      SocketHandle(HOSTHANDLE Handle) : socket_val(SOCKET_HANDLE(uintptr_t(Handle))) {}
   #endif

   operator SOCKET_HANDLE() const { return socket_val; }
   operator bool() const { return socket_val != INVALID_SOCKET_VAL; }

   SOCKET_HANDLE socket() const { return socket_val; }
   int int_value() const { return int(socket_val); }

   HOSTHANDLE hosthandle() const {
      #ifdef _WIN32
         return (HOSTHANDLE)(uintptr_t(socket_val));
      #else
         return HOSTHANDLE(socket_val);
      #endif
   }

   bool is_valid() const { return socket_val != INVALID_SOCKET_VAL; }
   bool is_invalid() const { return socket_val IS INVALID_SOCKET_VAL; }

   bool operator==(const SocketHandle &Other) const { return socket_val == Other.socket_val; }
   bool operator!=(const SocketHandle &Other) const { return socket_val != Other.socket_val; }
   bool operator==(SOCKET_HANDLE Socket) const { return socket_val == Socket; }
   bool operator!=(SOCKET_HANDLE Socket) const { return socket_val != Socket; }

   SocketHandle & operator=(SOCKET_HANDLE Socket) { socket_val = Socket; return *this; }
   #ifdef _WIN32
      SocketHandle & operator=(int Socket) { socket_val = SOCKET_HANDLE(Socket); return *this; }
   #endif
};

static constexpr SOCKET_HANDLE NOHANDLE = SocketHandle::INVALID_SOCKET_VAL;

struct socket_endpoint {
   struct sockaddr_storage storage;
   int size = 0;
   CSTRING label = nullptr;
};

class NetworkPlatform {
public:
   virtual ~NetworkPlatform() = default;

   virtual ERR initialise(OBJECTPTR Module) = 0;
   virtual void expunge() = 0;
   virtual int socket_limit() const = 0;

   virtual SocketHandle create_socket(void *Reference, bool Read, bool Write, bool UDP, bool &IPv6) = 0;
   virtual void close_socket(SocketHandle Handle) = 0;
   virtual void deregister_socket(SocketHandle Handle) = 0;
   virtual int shutdown_socket(SocketHandle Handle, int How) = 0;

   virtual ERR connect(SocketHandle Handle, const socket_endpoint &Endpoint) = 0;
   virtual ERR begin_connect_wait(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) = 0;
   virtual ERR complete_connect(SocketHandle Handle) = 0;
   virtual ERR bind(SocketHandle Handle, const socket_endpoint &Endpoint) = 0;
   virtual ERR listen(SocketHandle Handle, int Backlog) = 0;
   virtual ERR get_local_address(SocketHandle Handle, sockaddr_storage &Address, int &Length) = 0;
   virtual SocketHandle accept(void *Reference, SocketHandle Server, bool IPv6, sockaddr_storage &Address, int &Family) = 0;
   virtual void set_socket_reference(SocketHandle Handle, void *Reference) = 0;
   virtual ERR set_non_blocking(SocketHandle Handle) = 0;

   virtual ERR register_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) = 0;
   virtual ERR register_accept(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) = 0;
   virtual ERR register_write(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) = 0;
   virtual ERR remove_write(SocketHandle Handle) = 0;
   virtual ERR register_recall_read(SocketHandle Handle, void (*Callback)(HOSTHANDLE, APTR), APTR Data) = 0;
   virtual ERR deregister_fd(SocketHandle Handle) = 0;

   virtual ERR enable_broadcast(SocketHandle Handle) = 0;
   virtual ERR set_multicast_ttl(SocketHandle Handle, int TTL, bool IPv6) = 0;
   virtual ERR join_multicast_group(SocketHandle Handle, CSTRING Group, bool IPv6) = 0;
   virtual ERR leave_multicast_group(SocketHandle Handle, CSTRING Group, bool IPv6) = 0;

   virtual ERR receive(SocketHandle Handle, APTR Buffer, size_t Length, size_t &Received) = 0;
   virtual ERR append_receive(SocketHandle Handle, std::vector<uint8_t> &Buffer, size_t Length, size_t &Received) = 0;
   virtual ERR send(SocketHandle Handle, CPTR Buffer, size_t &Length) = 0;
   virtual ERR send_to(SocketHandle Handle, CPTR Buffer, size_t &Length, const socket_endpoint &Endpoint) = 0;
   virtual ERR receive_from(SocketHandle Handle, APTR Buffer, size_t BufferSize, size_t &BytesRead,
      sockaddr_storage &SourceAddress, int &AddressLength) = 0;

   virtual uint32_t inet_addr(CSTRING Value) = 0;
   virtual int inet_pton(int Family, CSTRING Source, APTR Dest) = 0;
   virtual CSTRING inet_ntop(int Family, CPTR Source, STRING Dest, size_t Size) = 0;
   virtual uint32_t host_to_long(uint32_t Value) = 0;
   virtual uint32_t long_to_host(uint32_t Value) = 0;
   virtual uint16_t host_to_short(uint16_t Value) = 0;
   virtual uint16_t short_to_host(uint16_t Value) = 0;
};

NetworkPlatform & network_platform();
std::unique_ptr<NetworkPlatform> create_platform();
