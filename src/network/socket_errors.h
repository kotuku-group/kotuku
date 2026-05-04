#pragma once

#include <kotuku/system/errors.h>

#ifdef __linux__
   #include <errno.h>
#endif

struct SocketErrorEntry {
   int SystemError;
   ERR KotukuError;
};

static const SocketErrorEntry glSocketErrors[] = {
#ifdef _WIN32
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
#elif defined(__linux__)
   { EINTR,                 ERR::Cancelled },
   { EACCES,                ERR::PermissionDenied },
   { EPERM,                 ERR::PermissionDenied },
   { EFAULT,                ERR::InvalidData },
   { EINVAL,                ERR::Args },
   { EMFILE,                ERR::OutOfSpace },
   { ENFILE,                ERR::OutOfSpace },
   { EAGAIN,                ERR::InvalidState },
   { EWOULDBLOCK,           ERR::InvalidState },
   { EINPROGRESS,           ERR::Busy },
   { EALREADY,              ERR::Busy },
   { ENOTSOCK,              ERR::Args },
   { EDESTADDRREQ,          ERR::Args },
   { EMSGSIZE,              ERR::DataSize },
   { EPROTOTYPE,            ERR::Args },
   { ENOPROTOOPT,           ERR::Args },
   { EPROTONOSUPPORT,       ERR::NoSupport },
   { ESOCKTNOSUPPORT,       ERR::NoSupport },
   { EOPNOTSUPP,            ERR::NoSupport },
   { EPFNOSUPPORT,          ERR::NoSupport },
   { EAFNOSUPPORT,          ERR::NoSupport },
   { EADDRINUSE,            ERR::InUse },
   { EADDRNOTAVAIL,         ERR::HostUnreachable },
   { ENETDOWN,              ERR::NetworkUnreachable },
   { ENETUNREACH,           ERR::NetworkUnreachable },
   { ENETRESET,             ERR::Disconnected },
   { ECONNABORTED,          ERR::ConnectionAborted },
   { ECONNRESET,            ERR::Disconnected },
   { ENOBUFS,               ERR::BufferOverflow },
   { ENOMEM,                ERR::NoMemory },
   { EISCONN,               ERR::DoubleInit },
   { ENOTCONN,              ERR::Disconnected },
   { ESHUTDOWN,             ERR::Disconnected },
   { ETIMEDOUT,             ERR::TimeOut },
   { ECONNREFUSED,          ERR::ConnectionRefused },
   { EHOSTDOWN,             ERR::HostUnreachable },
   { EHOSTUNREACH,          ERR::HostUnreachable },
   { EPIPE,                 ERR::Disconnected },
#endif
   { 0, ERR::NIL }
};

static ERR convert_socket_error(int Error = 0, ERR Default = ERR::SystemCall)
{
#ifdef _WIN32
   if (Error IS 0) Error = WSAGetLastError();
#elif defined(__linux__)
   if (Error IS 0) Error = errno;
#endif

   for (int i=0; glSocketErrors[i].SystemError; i++) {
      if (glSocketErrors[i].SystemError IS Error) return glSocketErrors[i].KotukuError;
   }

   return Default;
}
