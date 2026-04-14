
#ifndef WIN32
#define WIN32
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "ankerl/unordered_dense.h"

struct IPAddress {
   union {
      char Data[16];  // Bytes 0-3 are IPv4 bytes.  In host byte order
      unsigned int Data32[4];
   };
   int Type;  // IPADDR_V4 or IPADDR_V6
   int Port;  // Port number in host byte order (UDP only)
};

#define IPADDR_V4 0
#define IPADDR_V6 1

#include <kotuku/system/errors.h>

#include "winsockwrappers.h"

#include <unordered_map>
#include <cstdio>
#include <mutex>
#include <optional>

using namespace std;

enum {
   NETMSG_START=0,
   NETMSG_END
};

enum {
   WM_NETWORK = WM_USER + 101, // WM_USER = 1024, 1125
   WM_RESOLVENAME // 1126
};

#define IS ==

constexpr int MAX_SOCKETS = 40;

class socket_info { // Only the SocketHandle FD is unique.  NetSocket may be referenced multiple times for as many clients exist.
public:
   void *Reference = nullptr;      // Reference to a NetSocket, ClientSocket
   void *NetHost = nullptr;        // For win_async_resolvename() and WM_RESOLVENAME
   HANDLE ResolveHandle = INVALID_HANDLE_VALUE; // For win_async_resolvename() and WM_RESOLVENAME
   WSW_SOCKET SocketHandle = 0; // Winsock socket FD (same as the key)
   int Flags = 0;
};

static std::recursive_mutex csNetLookup;
static ankerl::unordered_dense::map<WSW_SOCKET, socket_info> glNetLookup;
static char glSocketsDisabled = 0; // Thread-safe, only the main thread modifies this
static HWND glNetWindow = 0;
static char glNetClassInit = false;
static char glWinsockInitialised = false;

//********************************************************************************************************************

static const struct {
   int WinError;
   ERR PanError;
} glErrors[] = {
   { WSAEINTR,              ERR::Cancelled },
   { WSAEACCES,             ERR::PermissionDenied },
   { WSAEFAULT,             ERR::InvalidData },
   { WSAEINVAL,             ERR::Args },
   { WSAEMFILE,             ERR::OutOfSpace },
   { WSAEWOULDBLOCK,        ERR::InvalidState },
   { WSAEINPROGRESS,        ERR::Busy },
   { WSAEALREADY,           ERR::Busy },
   { WSAENOTSOCK,           ERR::Args },
   { WSAEDESTADDRREQ,       ERR::Args },
   { WSAEMSGSIZE,           ERR::DataSize },
   { WSAEPROTOTYPE,         ERR::Args },
   { WSAENOPROTOOPT,        ERR::Args },
   { WSAEPROTONOSUPPORT,    ERR::NoSupport },
   { WSAESOCKTNOSUPPORT,    ERR::NoSupport },
   { WSAEOPNOTSUPP,         ERR::NoSupport },
   { WSAEPFNOSUPPORT,       ERR::NoSupport },
   { WSAEAFNOSUPPORT,       ERR::NoSupport },
   { WSAEADDRINUSE,         ERR::InUse },
   { WSAEADDRNOTAVAIL,      ERR::HostUnreachable },
   { WSAENETDOWN,           ERR::NetworkUnreachable },
   { WSAENETUNREACH,        ERR::NetworkUnreachable },
   { WSAENETRESET,          ERR::Disconnected },
   { WSAECONNABORTED,       ERR::ConnectionAborted },
   { WSAECONNRESET,         ERR::Disconnected },
   { WSAENOBUFS,            ERR::BufferOverflow },
   { WSAEISCONN,            ERR::DoubleInit },
   { WSAENOTCONN,           ERR::Disconnected },
   { WSAESHUTDOWN,          ERR::Disconnected },
   { WSAETIMEDOUT,          ERR::TimeOut },
   { WSAECONNREFUSED,       ERR::ConnectionRefused },
   { WSAEHOSTDOWN,          ERR::HostUnreachable },
   { WSAEHOSTUNREACH,       ERR::HostUnreachable },
   { WSAHOST_NOT_FOUND,     ERR::HostNotFound },
   { WSASYSCALLFAILURE,     ERR::SystemCall },
   { 0, ERR::NIL }
};

//********************************************************************************************************************

static ERR convert_error(int error = 0)
{
   if (!error) error = WSAGetLastError();

   for (int i=0; glErrors[i].WinError; i++) {
      if (glErrors[i].WinError IS error) return glErrors[i].PanError;
   }
   return ERR::SystemCall;
}

//********************************************************************************************************************

void winCloseResolveHandle(void *Handle)
{
   WSACancelAsyncRequest(Handle);
   CloseHandle(Handle);
}

//********************************************************************************************************************

struct hostent * win_gethostbyaddr(const struct IPAddress *Address)
{
   if (Address->Type IS IPADDR_V4) return gethostbyaddr((const char *)&Address->Data, 4, AF_INET);
   else return gethostbyaddr((const char *)&Address->Data, 16, AF_INET6);
}

//********************************************************************************************************************

static LRESULT CALLBACK win_messages(HWND window, UINT msgcode, WPARAM wParam, LPARAM lParam)
{
   int winerror = WSAGETSELECTERROR(lParam);
   int event = WSAGETSELECTEVENT(lParam);

   if (msgcode IS WM_NETWORK) {
      const lock_guard<recursive_mutex> lock(csNetLookup);
      auto socket_handle = (WSW_SOCKET)wParam;
      if (glNetLookup.contains(socket_handle)) {
         int state;
         int resub_write = false;
         switch (event) {
            case FD_READ:    state = NTE_READ; break;
            case FD_WRITE:   state = NTE_WRITE; resub_write = true; break; // Keep the socket subscribed while writing
            case FD_ACCEPT:  state = NTE_ACCEPT; break;
            case FD_CLOSE:   state = NTE_CLOSE; break;
            case FD_CONNECT: state = NTE_CONNECT; break;
            default:         state = 0; break;
         }

         ERR error;
         if (winerror IS WSAEWOULDBLOCK) error = ERR::Okay;
         else if (winerror) error = convert_error(winerror);
         else error = ERR::Okay;

         socket_info &info = glNetLookup[socket_handle];
         bool read_disabled = false;
         if ((info.Flags & FD_READ) and (!glSocketsDisabled)) {
            WSAAsyncSelect(socket_handle, glNetWindow, WM_NETWORK, info.Flags & (~FD_READ));
            read_disabled = true;
         }

         if (info.Reference) {
            if ((state IS NTE_WRITE) and (!(info.Flags & FD_WRITE))) {
               // Do nothing when receiving queued write messages for a socket that has turned them off.
               return 0;
            }
            else win32_netresponse((struct Object *)info.Reference, socket_handle, state, error);
         }
         else printf("win_messages() Missing reference for FD %d, state %d\n", socket_handle, state);

         // Re-enable read events if we disabled them and sockets are still active
         if ((read_disabled) and (!glSocketsDisabled)) {
            if (glNetLookup.contains(socket_handle) and (glNetLookup[socket_handle].Flags & FD_READ)) {
               WSAAsyncSelect(socket_handle, glNetWindow, WM_NETWORK, glNetLookup[socket_handle].Flags);
            }
         }

         if ((resub_write) and (!glSocketsDisabled)) {
            if (glNetLookup.contains(socket_handle) and (glNetLookup[socket_handle].Flags & FD_WRITE)) { // Sanity check in case write was switched off in win32_netresponse()
               WSAAsyncSelect(socket_handle, glNetWindow, WM_NETWORK, glNetLookup[socket_handle].Flags);
            }
         }
         return 0;
      }
   }
   else return DefWindowProc(window, msgcode, wParam, lParam);

   return 0;
}

//********************************************************************************************************************
// This function is called by ProcessMesages() before and after windows messages are processed.  We tell windows to
// not produce any new network events during the message processing by turning off the flags for each socket.  This
// stops Windows from flooding our application with messages when downloading over a fast connection for example.
//
// The state of each socket is restored when we are called with NETMSG_END.

void win_net_processing(int Status, void *Args)
{
   if (Status IS NETMSG_START) {
      glSocketsDisabled++;
      if (glSocketsDisabled == 1) {
         const lock_guard<recursive_mutex> lock(csNetLookup);
         for (auto it=glNetLookup.begin(); it != glNetLookup.end(); it++) {
            WSAAsyncSelect(it->first, glNetWindow, 0, 0); // Turn off network messages
         }
      }
   }
   else if (Status IS NETMSG_END) {
      glSocketsDisabled--;
      if (glSocketsDisabled == 0) {
         const lock_guard<recursive_mutex> lock(csNetLookup);
         for (auto it=glNetLookup.begin(); it != glNetLookup.end(); it++) {
            WSAAsyncSelect(it->first, glNetWindow, WM_NETWORK, it->second.Flags); // Turn on network messages
         }
      }
   }
}

//********************************************************************************************************************
// Sets the read/write state for a socket.
// Use std::nullopt if state should be unchanged

ERR win_socketstate(WSW_SOCKET Socket, std::optional<bool> Read, std::optional<bool> Write)
{
   const lock_guard<recursive_mutex> lock(csNetLookup);
   socket_info &sock = glNetLookup[Socket];

   if (Read.has_value()) {
      if (Read.value() IS false) sock.Flags &= ~FD_READ;
      else if (Read.value() IS true) sock.Flags |= FD_READ;
   }

   if (Write.has_value()) {
      if (Write.value() IS false) sock.Flags &= ~FD_WRITE;
      else if (Write.value() IS true) sock.Flags |= FD_WRITE;
   }

   if (!glSocketsDisabled) {
      auto winerror = WSAAsyncSelect(Socket, glNetWindow, WM_NETWORK, sock.Flags);
      if (winerror) return convert_error(winerror);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Initially when accepting a connection from a client, the ClientSocket object won't exist yet.  This is rectified later
// with a call to win_socket_reference()

WSW_SOCKET win_accept(void *NetSocket, WSW_SOCKET SocketHandle, struct sockaddr *Addr, int *AddrLen)
{
   auto client_handle = WSW_SOCKET(accept(SocketHandle, Addr, AddrLen));
   //printf("win_accept() FD %d, NetSocket %p\n", client_handle, NetSocket);

   // Enable TCP_NODELAY by default for better responsiveness
   DWORD nodelay = 1;
   setsockopt(client_handle, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

   u_long non_blocking = 1;
   ioctlsocket(client_handle, FIONBIO, &non_blocking);

   int flags = FD_CLOSE|FD_ACCEPT|FD_CONNECT|FD_READ;
   if (!glSocketsDisabled) WSAAsyncSelect(client_handle, glNetWindow, WM_NETWORK, flags);

   const lock_guard<recursive_mutex> lock(csNetLookup);
   glNetLookup[client_handle] = { .Reference = NetSocket, .SocketHandle = client_handle, .Flags = flags };
   return client_handle;
}

//********************************************************************************************************************
// Replace the reference on a known socket handle

void win_socket_reference(WSW_SOCKET SocketHandle, void *Reference)
{
   const lock_guard<recursive_mutex> lock(csNetLookup);
   glNetLookup[SocketHandle].Reference = Reference;
}

//********************************************************************************************************************

ERR win_bind(WSW_SOCKET SocketHandle, const struct sockaddr *Name, int NameLen)
{
   if (::bind(SocketHandle, Name, NameLen) == SOCKET_ERROR) return convert_error();
   else return ERR::Okay;
}

//********************************************************************************************************************
// If win_closesocket() is going to be called from a thread, deregister the socket ahead of time because you're
// otherwise likely to get calls to that socket going through the Win32 message queue and causing a segfault.

void win_deregister_socket(WSW_SOCKET SocketHandle)
{
   const lock_guard<recursive_mutex> lock(csNetLookup);
   glNetLookup.erase(SocketHandle);

   // Cancel all pending async events for SocketHandle
   // This prevents stale events from being delivered to new sockets that reuse the handle
   WSAAsyncSelect(SocketHandle, glNetWindow, 0, 0);

   // Process pending Windows messages for this socket to clear the queue
   // NB: closesocket() does not perform this task in spite of MSDN implying it does.

   std::vector<MSG> other_messages;
   MSG msg;
   while (PeekMessage(&msg, glNetWindow, WM_NETWORK, WM_NETWORK, PM_REMOVE)) {
      if (msg.wParam IS SocketHandle) {
         // Discard this message - it's for the socket we're closing
      }
      else other_messages.push_back(msg);
   }

   for (const auto &saved_msg : other_messages) {
      PostMessage(glNetWindow, saved_msg.message, saved_msg.wParam, saved_msg.lParam);
   }
}

//********************************************************************************************************************
// Wrapped by CLOSESOCKET()
// NOTE: Windows reuses socket handles frequently.  Our closure process is designed to overcome many of these problems,
// but if you notice any weird socket behaviour then give consideration to the possibility of handle re-use.

void win_closesocket(WSW_SOCKET SocketHandle)
{
   if (SocketHandle IS INVALID_SOCKET) return;

   win_deregister_socket(SocketHandle);

   // Perform graceful disconnect before closing

   shutdown(SocketHandle, SD_BOTH);

   // Set a short timeout to allow pending data to be transmitted
   struct timeval timeout;
   timeout.tv_sec = 0;
   timeout.tv_usec = 100000; // 100ms timeout
   setsockopt(SocketHandle, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
   setsockopt(SocketHandle, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

   // Drain any remaining data in the receive buffer
   char buffer[1024];
   int bytes_received;
   do {
      bytes_received = recv(SocketHandle, buffer, sizeof(buffer), 0);
   } while (bytes_received > 0);

   closesocket(SocketHandle);
}

//********************************************************************************************************************

ERR win_connect(WSW_SOCKET SocketHandle, const struct sockaddr *Name, int NameLen)
{
   if (connect(SocketHandle, Name, NameLen) IS SOCKET_ERROR) {
      if (WSAGetLastError() IS WSAEWOULDBLOCK) return ERR::Okay; // connect() will always 'fail' for non-blocking sockets (however it will continue to connect/succeed...!)
      return convert_error();
   }
   else return ERR::Okay;
}

//********************************************************************************************************************

struct hostent *win_gethostbyname(const char *Name)
{
   // Use WSAAsyncGetHostByName() if you want to do this asynchronously.

   return gethostbyname(Name);
}

//********************************************************************************************************************

int win_getpeername(WSW_SOCKET S, struct sockaddr *Name, int *NameLen)
{
   return getpeername(S, Name, NameLen);
}

//********************************************************************************************************************

int win_getsockname(WSW_SOCKET S, struct sockaddr *Name, int *NameLen)
{
   return getsockname(S, Name, NameLen);
}

//********************************************************************************************************************

uint32_t win_inet_addr(const char *Str)
{
   return inet_addr(Str);
}

//********************************************************************************************************************
// IPv4

char * win_inet_ntoa(uint32_t Addr)
{
   struct in_addr addr;
   addr.s_addr = Addr;
   return inet_ntoa(addr);
}

//********************************************************************************************************************

ERR win_listen(WSW_SOCKET SocketHandle, int BackLog)
{
   if (listen(SocketHandle, BackLog) IS SOCKET_ERROR) return convert_error();
   else return ERR::Okay;
}

//********************************************************************************************************************

ERR WIN_RECEIVE(WSW_SOCKET SocketHandle, void *Buffer, size_t Len, size_t *Result)
{
   *Result = 0;
   if (!Len) return ERR::Okay;
   auto result = recv(SocketHandle, reinterpret_cast<char *>(Buffer), Len, 0);
   if (result > 0) {
      *Result = result;
      return ERR::Okay;
   }
   else if (result IS 0) return ERR::Disconnected;
   else if (WSAGetLastError() IS WSAEWOULDBLOCK) return ERR::Okay;
   else return convert_error();
}

// This variant makes it easier to append data to buffers.

template <class T> ERR WIN_APPEND(WSW_SOCKET SocketHandle, std::vector<uint8_t> &Buffer, size_t Len, T &Result)
{
   Result = 0;
   if (!Len) return ERR::Okay;
   auto offset = Buffer.size();
   Buffer.resize(Buffer.size() + Len);
   auto result = recv(SocketHandle, (char *)Buffer.data() + offset, Len, 0);
   if (result > 0) {
      if (size_t(result) < Len) Buffer.resize(offset + result);
      Result = result;
      return ERR::Okay;
   }
   else if (result IS 0) {
      return ERR::Disconnected;
   }
   else if (WSAGetLastError() IS WSAEWOULDBLOCK) return ERR::Okay;
   else return convert_error();
}

// Explicit template instantiations
template ERR WIN_APPEND<size_t>(WSW_SOCKET, std::vector<uint8_t> &, size_t, size_t &);

//********************************************************************************************************************

ERR WIN_SENDTO(WSW_SOCKET Socket, const void *Buffer, size_t *Length, const struct sockaddr *To, int ToLen)
{
   if (!*Length) return ERR::Okay;
   auto result = sendto(Socket, reinterpret_cast<const char *>(Buffer), *Length, 0, To, ToLen);
   if (result >= 0) {
      *Length = result;
      return ERR::Okay;
   }
   else {
      *Length = 0;
      switch (WSAGetLastError()) {
         case WSAEWOULDBLOCK:
         case WSAEALREADY:
            return ERR::BufferOverflow;
         case WSAEINPROGRESS:
            return ERR::Busy;
         default:
            return convert_error();
      }
   }
}

//********************************************************************************************************************

ERR WIN_RECVFROM(WSW_SOCKET Socket, void *Buffer, size_t BufferSize, size_t *BytesRead, struct sockaddr *From, int *FromLen)
{
   *BytesRead = 0;
   if (!BufferSize) return ERR::Okay;

   auto result = recvfrom(Socket, reinterpret_cast<char *>(Buffer), BufferSize, 0, From, FromLen);
   if (result > 0) {
      *BytesRead = result;
      return ERR::Okay;
   }
   else if (result IS 0) {
      return ERR::Disconnected;
   }
   else if (WSAGetLastError() IS WSAEWOULDBLOCK) {
      return ERR::Okay;
   }
   else {
      return convert_error();
   }
}

//********************************************************************************************************************

ERR win_enable_broadcast(WSW_SOCKET Socket)
{
   BOOL broadcast = TRUE;
   if (setsockopt(Socket, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast)) != 0) {
      return convert_error();
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR win_set_multicast_ttl(WSW_SOCKET Socket, int TTL, bool IPv6)
{
   if (IPv6) {
      DWORD ttl = TTL;
      if (setsockopt(Socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (char*)&ttl, sizeof(ttl)) != 0) {
         return convert_error();
      }
   }
   else {
      DWORD ttl = TTL;
      if (setsockopt(Socket, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&ttl, sizeof(ttl)) != 0) {
         return convert_error();
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR win_join_multicast_group(WSW_SOCKET Socket, const char *Group, bool IPv6)
{
   if (IPv6) {
      struct ipv6_mreq mreq6;
      if (win_inet_pton(AF_INET6, Group, &mreq6.ipv6mr_multiaddr) != 1) {
         return ERR::Args;
      }
      mreq6.ipv6mr_interface = 0; // Use default interface

      if (setsockopt(Socket, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char*)&mreq6, sizeof(mreq6)) != 0) {
         return convert_error();
      }
   }
   else {
      struct ip_mreq mreq;
      if (win_inet_pton(AF_INET, Group, &mreq.imr_multiaddr) != 1) {
         return ERR::Args;
      }
      mreq.imr_interface.s_addr = INADDR_ANY; // Use default interface

      if (setsockopt(Socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) != 0) {
         return convert_error();
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR win_leave_multicast_group(WSW_SOCKET Socket, const char *Group, bool IPv6)
{
   if (IPv6) {
      struct ipv6_mreq mreq6;
      if (win_inet_pton(AF_INET6, Group, &mreq6.ipv6mr_multiaddr) != 1) {
         return ERR::Args;
      }
      mreq6.ipv6mr_interface = 0; // Use default interface

      if (setsockopt(Socket, IPPROTO_IPV6, IPV6_LEAVE_GROUP, (char*)&mreq6, sizeof(mreq6)) != 0) {
         return convert_error();
      }
   }
   else {
      struct ip_mreq mreq;
      if (win_inet_pton(AF_INET, Group, &mreq.imr_multiaddr) != 1) {
         return ERR::Args;
      }
      mreq.imr_interface.s_addr = INADDR_ANY; // Use default interface

      if (setsockopt(Socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) != 0) {
         return convert_error();
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR WIN_SEND(WSW_SOCKET Socket, const void *Buffer, size_t *Length, int Flags)
{
   if (!*Length) return ERR::Okay;
   *Length = send(Socket, reinterpret_cast<const char *>(Buffer), *Length, Flags);
   if (*Length >= 0) return ERR::Okay;
   else {
      *Length = 0;

      switch (WSAGetLastError()) {
         case WSAEWOULDBLOCK:
         case WSAEALREADY:
            return ERR::BufferOverflow;
         case WSAEINPROGRESS:
            return ERR::Busy;
         default:
            return convert_error();
      }
   }
}

//********************************************************************************************************************

int win_shutdown(WSW_SOCKET S, int How)
{
   return shutdown(S, How);
}

//********************************************************************************************************************

int win_WSAGetLastError()
{
   return WSAGetLastError();
}

int win_WSAENETUNREACH()
{
   return WSAGetLastError() IS WSAENETUNREACH;
}

int win_WSAECONNREFUSED()
{
   return WSAGetLastError() IS WSAECONNREFUSED;
}

int SocketWouldBlock()
{
   return WSAGetLastError() IS WSAEWOULDBLOCK;
}

//********************************************************************************************************************

int GetSockOptError(WSW_SOCKET S, char *Result, int *OptLen)
{
   return getsockopt(S, SOL_SOCKET, SO_ERROR, Result, OptLen);
}

//********************************************************************************************************************

uint32_t win_htonl(uint32_t X)
{
   return htonl(X);
}

uint32_t win_ntohl(uint32_t X)
{
   return ntohl(X);
}

unsigned short win_htons(unsigned short X)
{
   return htons(X);
}

unsigned short win_ntohs(unsigned short X)
{
   return ntohs(X);
}

//********************************************************************************************************************
// Note: The startup and shutdown functionality have been tested as working with multiple initialisations and
// module expunges.  Avoid tampering as the Windows functions are a bit sensitive.

const char * StartupWinsock() // Return zero if succesful
{
   if (!glNetClassInit) {
      WNDCLASSEX glNetClass;
      ZeroMemory(&glNetClass, sizeof(glNetClass));
      glNetClass.cbSize        = sizeof(glNetClass);
      glNetClass.style         = CS_DBLCLKS;
      glNetClass.lpfnWndProc   = win_messages;
      glNetClass.hInstance     = GetModuleHandle(nullptr);
      glNetClass.lpszClassName = "NetClass";
      if (!RegisterClassEx(&glNetClass)) {
         return "Failed to register window class for network messages.";
      }
      glNetClassInit = TRUE;
   }

   if (!glNetWindow) {
      // Create an invisible window that we will use to wake us up when network events occur (WSAAsyncSelect() insists on
      // there being a window).

      if (!(glNetWindow = CreateWindowEx(0,
         "NetClass", "NetworkWindow",
         0,
         0, 0,
         CW_USEDEFAULT, CW_USEDEFAULT,
         (HWND)nullptr,
         (HMENU)nullptr,
         GetModuleHandle(nullptr), nullptr))) return "Failed to create Window for receiving network messages.";
   }

   if (!glWinsockInitialised) {
      WSADATA wsadata;
      unsigned short version_requested = MAKEWORD(1, 1);
      if (auto code = WSAStartup(version_requested, &wsadata)) {
         switch (code) {
            case WSASYSNOTREADY: return "WSASYSNOTREADY";
            case WSAVERNOTSUPPORTED: return "WSAVERNOTSUPPORTED";
            case WSAEINPROGRESS: return "WSAEINPROGRESS";
            case WSAEPROCLIM: return "WSAEPROCLIM";
            case WSAEFAULT: return "WSAEFAULT";
            default: return "Reason not given.";
         }
      }
      glWinsockInitialised = TRUE;
   }

   return nullptr;
}

//********************************************************************************************************************

int ShutdownWinsock()
{
   if (glNetWindow) { DestroyWindow(glNetWindow); glNetWindow = 0; }
   if (glNetClassInit) { UnregisterClass("NetClass", GetModuleHandle(nullptr)); glNetClassInit = false; }
   if (glWinsockInitialised) { WSACleanup(); glWinsockInitialised = false; }
   return 0;
}

//********************************************************************************************************************
// IPv6 wrapper functions for Windows with fall-back to IPv4 if IPv6 is not supported or fails.

WSW_SOCKET win_socket_ipv6(void *NetSocket, char Read, char Write, bool &IPV6, bool UDP)
{
   IPV6 = false;
   int socket_type = UDP ? SOCK_DGRAM : SOCK_STREAM;
   int protocol = UDP ? IPPROTO_UDP : IPPROTO_TCP;

   // Try to create IPv6 dual-stack socket first
   auto handle = socket(AF_INET6, socket_type, protocol);
   if (handle != INVALID_SOCKET) {
      // Set dual-stack mode (disable IPv6-only mode to accept IPv4 connections)
      DWORD v6only = 0;
      if (setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&v6only, sizeof(v6only)) != 0) {
         // Log warning but continue - some systems may not support dual-stack
      }

      // Enable TCP_NODELAY by default for better responsiveness (TCP only)
      if (!UDP) {
         DWORD nodelay = 1;
         setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
      }

      u_long non_blocking = 1;
      ioctlsocket(handle, FIONBIO, &non_blocking);

      int flags = FD_CLOSE;
      if (!UDP) flags |= FD_ACCEPT|FD_CONNECT;
      if (Read) flags |= FD_READ;
      if (Write) flags |= FD_WRITE;
      if (!glSocketsDisabled) WSAAsyncSelect(handle, glNetWindow, WM_NETWORK, flags);

      auto sock = WSW_SOCKET(handle);
      const lock_guard<recursive_mutex> lock(csNetLookup);
      glNetLookup[sock] = { .Reference = NetSocket, .SocketHandle = sock, .Flags = flags };
      IPV6 = true;
      return sock;
   }
   else { // Fall back to IPv4-only socket
      if (auto handle = socket(PF_INET, socket_type, protocol); handle != INVALID_SOCKET) {
         // Enable TCP_NODELAY by default for better responsiveness (TCP only)
         if (!UDP) {
            DWORD nodelay = 1;
            setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
         }

         u_long non_blocking = 1;
         ioctlsocket(handle, FIONBIO, &non_blocking);
         int flags = FD_CLOSE;
         if (!UDP) flags |= FD_ACCEPT|FD_CONNECT;
         if (Read) flags |= FD_READ;
         if (Write) flags |= FD_WRITE;
         if (!glSocketsDisabled) WSAAsyncSelect(handle, glNetWindow, WM_NETWORK, flags);

         auto sock = WSW_SOCKET(handle);
         const lock_guard<recursive_mutex> lock(csNetLookup);
         glNetLookup[sock] = { .Reference = NetSocket, .SocketHandle = sock, .Flags = flags };
         return sock;
      }
      else return (WSW_SOCKET)INVALID_SOCKET;
   }
}

//********************************************************************************************************************

int win_inet_pton(int af, const char *src, void *dst)
{
   if (af IS AF_INET6) {
      return inet_pton(af, src, dst);
   }
   else if (af IS AF_INET) { // Use standard IPv4 parsing
      uint32_t result = inet_addr(src);
      if (result != INADDR_NONE) {
         memcpy(dst, &result, 4);
         return 1;
      }
   }

   return 0;
}

//********************************************************************************************************************

const char *win_inet_ntop(int af, const void *src, char *dst, size_t size)
{
   if (af IS AF_INET6) {
      if (size < 46) return nullptr; // Need at least 46 bytes for full IPv6 address
      return inet_ntop(af, src, dst, size);
   }
   else if (af IS AF_INET) {
      if (size < 16) return nullptr; // Need at least 16 bytes for IPv4
      struct in_addr addr;
      memcpy(&addr, src, 4);
      const char *result = inet_ntoa(addr);
      if (result and strlen(result) < size) {
         strcpy(dst, result);
         return dst;
      }
   }

   return nullptr; // Error
}

//********************************************************************************************************************

WSW_SOCKET win_accept_ipv6(void *NetSocket, WSW_SOCKET ServerSocket, struct sockaddr *addr, int *addrlen, int *family)
{
   // Use sockaddr_storage for dual-stack accept
   struct sockaddr_storage storage;
   int storage_len = sizeof(storage);

   SOCKET client_fd = accept(ServerSocket, (struct sockaddr*)&storage, &storage_len);
   if (client_fd IS INVALID_SOCKET) return (WSW_SOCKET)INVALID_SOCKET;

   if (addr and addrlen) {
      int copy_len = (*addrlen < storage_len) ? *addrlen : storage_len;
      memcpy(addr, &storage, copy_len);
      *addrlen = copy_len;
   }

   if (family) *family = storage.ss_family;

   DWORD nodelay = 1;
   setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

   u_long non_blocking = 1;
   ioctlsocket(client_fd, FIONBIO, &non_blocking);

   // Register the client socket for event handling
   int flags = FD_CLOSE | FD_READ | FD_WRITE;
   if (!glSocketsDisabled) WSAAsyncSelect(client_fd, glNetWindow, WM_NETWORK, flags);

   auto sock = WSW_SOCKET(client_fd);

   const lock_guard<recursive_mutex> lock(csNetLookup);
   glNetLookup[sock] = { .Reference = NetSocket, .SocketHandle = sock, .Flags = flags };

   return sock;
}
