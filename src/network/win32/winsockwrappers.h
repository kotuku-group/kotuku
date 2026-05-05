#pragma once

#include <optional>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <functional>
#include <cstdint>

typedef void * IOCP_OPERATION_HANDLE;
typedef void * IOCP_KEY;
typedef void * HANDLE; // Generic handle capable of handling ints or pointers.
typedef void * HOSTHANDLE; // Forward declaration for core system handle type
enum class ERR : int;
struct socket_info;
typedef unsigned int WSW_SOCKET; // Identical to the windows SOCKET type
struct sockaddr;
struct hostent;

// Unified handle wrapper that eliminates the need for manual casting between
// WSW_SOCKET and HOSTHANDLE when interfacing with the core system

class SocketHandle {
private:
   WSW_SOCKET socket_val;

public:
   static constexpr WSW_SOCKET INVALID_SOCKET_VAL = static_cast<WSW_SOCKET>(~0);

   // Constructors
   SocketHandle() : socket_val(INVALID_SOCKET_VAL) {}
   SocketHandle(WSW_SOCKET sock) : socket_val(sock) {}
   SocketHandle(int sock) : socket_val(static_cast<WSW_SOCKET>(sock)) {}
   SocketHandle(HOSTHANDLE handle) : socket_val(static_cast<WSW_SOCKET>(reinterpret_cast<uintptr_t>(handle))) {}

   // Conversion operators for seamless integration
   operator WSW_SOCKET() const { return socket_val; }
   operator HOSTHANDLE() const { return reinterpret_cast<HOSTHANDLE>(static_cast<uintptr_t>(socket_val)); }
   operator bool() const { return socket_val != INVALID_SOCKET_VAL; } // For boolean tests

   // Explicit accessors for clarity when needed
   WSW_SOCKET socket() const { return socket_val; }
   HOSTHANDLE hosthandle() const { return reinterpret_cast<HOSTHANDLE>(static_cast<uintptr_t>(socket_val)); }
   int int_value() const { return static_cast<int>(socket_val); } // For printf-style formatting with %d

   // Comparison operators
   bool operator==(const SocketHandle& other) const { return socket_val == other.socket_val; }
   bool operator!=(const SocketHandle& other) const { return socket_val != other.socket_val; }
   bool operator==(WSW_SOCKET sock) const { return socket_val == sock; }
   bool operator!=(WSW_SOCKET sock) const { return socket_val != sock; }

   // Validity check
   bool is_valid() const { return socket_val != INVALID_SOCKET_VAL; }
   bool is_invalid() const { return socket_val == INVALID_SOCKET_VAL; }

   // Assignment operators
   SocketHandle& operator=(WSW_SOCKET sock) { socket_val = sock; return *this; }
   SocketHandle& operator=(int sock) { socket_val = static_cast<WSW_SOCKET>(sock); return *this; }
};
void win32_netresponse(struct Object *, WSW_SOCKET, int, ERR);
void win_net_processing(int, void *);
WSW_SOCKET win_accept(void *, WSW_SOCKET, sockaddr *, int *);
ERR win_bind(WSW_SOCKET, const sockaddr *, int);
void win_closesocket(WSW_SOCKET);
void win_deregister_socket(WSW_SOCKET);
ERR win_connect(WSW_SOCKET, const sockaddr *, int);
hostent * win_gethostbyaddr(const IPAddress *);
hostent * win_gethostbyname(const char *);
int win_getpeername(WSW_SOCKET, sockaddr *, int *);
int win_getsockname(WSW_SOCKET, sockaddr *, int *);
uint32_t win_inet_addr(const char *);
char * win_inet_ntoa(uint32_t);
ERR win_listen(WSW_SOCKET, int);
ERR win_socketstate(WSW_SOCKET, std::optional<bool>, std::optional<bool>);
ERR win_socket_connect_complete(WSW_SOCKET);
ERR WIN_SEND(WSW_SOCKET, const void *, size_t *, int);
ERR WIN_SENDTO(WSW_SOCKET, const void *, size_t *, const sockaddr *, int);
ERR WIN_RECVFROM(WSW_SOCKET, void *, size_t, size_t *, sockaddr *, int *);
ERR win_enable_broadcast(WSW_SOCKET);
ERR win_set_multicast_ttl(WSW_SOCKET, int, bool);
ERR win_join_multicast_group(WSW_SOCKET, const char *, bool);
ERR win_leave_multicast_group(WSW_SOCKET, const char *, bool);
int win_shutdown(WSW_SOCKET, int);
WSW_SOCKET win_socket_ipv6(void *, char, char, bool &, bool = false);/*creates IPv6 dual-stack socket */
ERR WIN_RECEIVE(WSW_SOCKET, void *, size_t, size_t *);
template <class T> ERR WIN_APPEND(WSW_SOCKET, std::vector<uint8_t> &, size_t, T &);
void winCloseResolveHandle(void *);
void win_socket_reference(WSW_SOCKET, void *);

// IPv6 wrapper functions
int win_inet_pton(int af, const char *src, void *dst);
const char *win_inet_ntop(int af, const void *src, char *dst, size_t size);
WSW_SOCKET win_accept_ipv6(void *, WSW_SOCKET, sockaddr *, int *, int *);

#ifdef USE_ARES
int win_ares_resolveaddr(IPAddress *, ares_channeldata *, void *);
int win_ares_resolvename(const char *, ares_channeldata *, void *);
void win_ares_deselect(int);
#endif

uint32_t win_htonl(uint32_t);
uint32_t win_ntohl(uint32_t);
uint16_t win_htons(uint16_t);
uint16_t win_ntohs(uint16_t);

// Some functions for checking error codes
int SocketWouldBlock();

int win_WSAGetLastError();

//ioctlsocket(Self->SocketHandle, FIONBIO, &non_blocking);
int SetNonBlocking(WSW_SOCKET);

//getsockopt(Self->SocketHandle, SOL_SOCKET, SO_ERROR, &result, &optlen)
int GetSockOptError(WSW_SOCKET, char *, int *);

const char * StartupWinsock();   // Returns zero if successful
int ShutdownWinsock();  // Returns zero if successful

enum {
   NTE_NONE=0,
   NTE_WRITE,
   NTE_READ,
   NTE_ACCEPT,
   NTE_CONNECT,
   NTE_CLOSE
};
