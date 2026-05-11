/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
Network: Provides miscellaneous network functions and hosts the NetSocket and ClientSocket classes.

The Network module exports a few miscellaneous networking functions.  For core network functionality surrounding
sockets and HTTP, please refer to the @NetSocket and @HTTP classes.
-END-

*********************************************************************************************************************/

#define PRV_PROXY
#define PRV_NETLOOKUP
#define PRV_NETWORK
#define PRV_NETWORK_MODULE
#define PRV_NETSOCKET
#define PRV_CLIENTSOCKET
#define PRV_NETCLIENT

#include <stdio.h>
#include <sys/types.h>
#include <unordered_set>
#include <ctime>
#include <type_traits>

#include <string.h>

#include <kotuku/main.h>
#include <kotuku/modules/network.h>
#include <kotuku/strings.hpp>

#include "net_platform.h"
#include "ssl_certificate_policy.h"

#ifndef DISABLE_SSL
  #ifdef _WIN32
    #include "win32/ssl_wrapper.h"
  #else
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/bio.h>
    #include <openssl/bn.h>
    #include <openssl/rsa.h>
    #include <openssl/evp.h>
    #include <openssl/x509.h>
    #include <openssl/pem.h>
    #include <openssl/pkcs12.h>
  #endif
#endif

#include <stack>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <cstring>
#include <thread>
#include <optional>

//********************************************************************************************************************

std::mutex glmThreads;
std::unordered_set<std::shared_ptr<std::jthread>> glThreads;

static int glSocketLimit = 0x7fffffff; // System imposed socket limit

struct NetQueue {
   uint32_t Index;    // The current read/write position within the buffer
   std::vector<uint8_t> Buffer; // The buffer hosting the data
   ERR write(CPTR Message, size_t Length);
};

struct DNSEntry {
   std::string HostName;
   std::vector<IPAddress> Addresses;    // IP address list

   DNSEntry & operator=(DNSEntry other) {
      std::swap(HostName, other.HostName);
      std::swap(Addresses, other.Addresses);
      return *this;
   }
};

enum class SHS : uint8_t {
   NIL = 0,
   READ,
   WRITE
};

DEFINE_ENUM_FLAG_OPERATORS(SHS)

//********************************************************************************************************************

#ifdef _WIN32
   #include "win32/winsockwrappers.h"
#endif

#ifdef __linux__
   #include "socket_errors.h"
#endif

class extClientSocket : public objClientSocket {
   public:
   SocketHandle Handle;
   struct NetQueue WriteQueue; // Writes to the network socket are queued here in a buffer
   uint8_t OutgoingRecursion;  // Recursion manager
   uint8_t InUse;       // Recursion manager
   bool ReadCalled;     // True if the Read action has been called
   bool CloseAfterWrite = false; // True if Deactivate() is waiting for queued data to flush
   uint8_t ErrorCountdown = 8;  // Counts down on each error, disconnect occurs at zero.

   #ifndef DISABLE_SSL
      #ifdef _WIN32
         SSL_HANDLE SSLHandle;
      #else
         SSL *SSLHandle;     // SSL connection handle for this client
         BIO *BIOHandle;     // SSL BIO handle for this client
         SHS HandshakeStatus; // Tracks the current actions of SSL handshaking.
      #endif
   #endif
};

//********************************************************************************************************************

class extNetSocket : public objNetSocket {
   public:
   SocketHandle Handle;   // Handle of the socket
   FUNCTION Outgoing;
   FUNCTION Incoming;
   FUNCTION Feedback;
   objNetLookup *NetLookup;
   objNetClient *LastClient;      // For linked-list management for server sockets.  Points to the last client IP on the chain
   struct NetQueue WriteQueue;
   uint8_t ReadCalled:1;          // The Read() action sets this to TRUE whenever called.
   uint8_t IPV6:1;
   uint8_t Terminating:1;         // Set to TRUE when the NetSocket is marked for deletion.
   uint8_t ExternalSocket:1;      // Set to TRUE if the SocketHandle field was set manually by the client.
   uint8_t InUse;                 // Recursion counter to signal that the object is doing something.
   uint8_t IncomingRecursion;     // Used by netsocket_client to prevent recursive handling of incoming data.
   uint8_t OutgoingRecursion;
   bool CloseAfterWrite = false;  // True if termination is waiting for queued data to flush
   uint8_t ErrorCountdown = 8;    // Counts down on each error, disconnect occurs at zero.
   TIMER   TimerHandle = 0;       // Timer subscription handle for timeout
   #ifdef _WIN32
      int16_t WinRecursion; // For win32_netresponse()
   #endif
   #ifndef DISABLE_SSL
      // These handles are only used when the NetSocket is a client of a server.
      #ifdef _WIN32
         SSL_HANDLE SSLHandle;
      #else
        SSL *SSLHandle;
        SHS HandshakeStatus; // Tracks the current actions of SSL handshaking.
        BIO *BIOHandle;
      #endif
   #endif

   extNetSocket() {
      // objNetSocket defaults
      Error        = ERR::Okay;
      Backlog      = 10;
      State        = NTC::DISCONNECTED;
      MsgLimit     = 1024768;
      ClientLimit  = 1024;
      SocketLimit  = 256;
   }
};

class extNetLookup : public objNetLookup {
   public:
   FUNCTION Callback;
   struct DNSEntry Info;
   std::vector<std::unique_ptr<std::jthread>> Threads; // Simple mechanism for auto-joining all the threads on object destruction
};

//********************************************************************************************************************

#ifdef _WIN32
   #include "win32/winsockwrappers.h"
#endif

#include "module_def.c"

JUMPTABLE_CORE

#ifndef DISABLE_SSL
  #ifdef _WIN32
    // Windows SSL wrapper forward declarations
    template <class T> ERR sslConnect(T *);
    template <class T> void sslDisconnect(T *);
    static ERR sslSetup(extNetSocket *);
  #else
    // OpenSSL forward declarations
    static bool ssl_init = false;
    static ERR sslConnect(extNetSocket *);
    static ERR sslLinkSocket(extNetSocket *);
    static ERR sslSetup(extNetSocket *);
  #endif
#endif

//********************************************************************************************************************

struct CaseInsensitiveMap {
   bool operator() (const std::string &lhs, const std::string &rhs) const {
      return ::strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
   }
};

struct CaseInsensitiveHash {
   std::size_t operator()(const std::string& s) const noexcept {
      std::string lower = s;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      return std::hash<std::string>{}(lower);
   }
};

struct CaseInsensitiveEqual {
   bool operator()(const std::string& lhs, const std::string& rhs) const noexcept {
      return ::strcasecmp(lhs.c_str(), rhs.c_str()) IS 0;
   }
};

typedef ankerl::unordered_dense::map<std::string, DNSEntry, CaseInsensitiveHash, CaseInsensitiveEqual> HOSTMAP;

//********************************************************************************************************************

static std::unique_ptr<NetworkPlatform> glPlatform;

NetworkPlatform & network_platform()
{
   return *glPlatform;
}

//********************************************************************************************************************

static void CLOSESOCKET_THREADED(SocketHandle Handle)
{
   network_platform().deregister_socket(Handle);

   {
      std::lock_guard<std::mutex> lock(glmThreads);
      for (auto it = glThreads.begin(); it != glThreads.end();) {
         if (*it and (*it)->joinable()) (*it)->join();
         it = glThreads.erase(it);
      }
   }

   std::lock_guard<std::mutex> lock(glmThreads);
   auto thread_ptr = std::make_shared<std::jthread>();
   *thread_ptr = std::jthread([] (SocketHandle Handle) { network_platform().close_socket(Handle); }, Handle);
   glThreads.insert(thread_ptr);
}

//********************************************************************************************************************

inline void setIPV4(IPAddress &IP, uint32_t IPV4HostOrder, uint16_t Port) {
   IP.Type = IPADDR::V4;
   IP.Port = Port;
   IP.Data[0] = IPV4HostOrder;
   IP.Data[1] = IP.Data[2] = IP.Data[3] = 0;
}

inline void setIPV6(IPAddress &IP, uint8_t *Address, uint16_t Port) {
   IP.Type = IPADDR::V6;
   IP.Port = Port;
   kt::copymem(Address, &IP.Data, 16);
}

//********************************************************************************************************************
// Unified IP address conversion functions to eliminate platform-specific duplication

static uint32_t unified_inet_addr(CSTRING Str) {
   return network_platform().inet_addr(Str);
}

static int unified_inet_pton(int af, CSTRING src, void *dst) {
   return network_platform().inet_pton(af, src, dst);
}

static CSTRING unified_inet_ntop(int af, const void *src, char *dst, size_t size) {
   return network_platform().inet_ntop(af, src, dst, size);
}

//********************************************************************************************************************

static OBJECTPTR clNetLookup = nullptr;
static OBJECTPTR clProxy = nullptr;
static OBJECTPTR clNetSocket = nullptr;
static OBJECTPTR clClientSocket = nullptr;
static OBJECTPTR clNetClient = nullptr;
static OBJECTPTR glNetworkModule = nullptr;
static HOSTMAP glHosts; // Protected by glHostsMutex
static HOSTMAP glAddresses; // Protected by glAddressesMutex
static std::shared_mutex glHostsMutex;
static std::shared_mutex glAddressesMutex;
static MSGID glResolveNameMsgID = MSGID::NIL;
static MSGID glResolveAddrMsgID = MSGID::NIL;
static std::string glCertPath;

//********************************************************************************************************************

#ifndef DISABLE_SSL
   struct ssl_certificate_paths {
      std::string Certificate;
      std::string PrivateKeyPath;
      std::optional<const std::string> PrivateKey;
      std::optional<const std::string> Password;
      SSLCERTFORMAT Format = SSLCERTFORMAT::NIL;
   };

   static ERR resolve_ssl_certificate_paths(extNetSocket *Self, ssl_certificate_paths &Paths)
   {
      if ((!Self) or (!Self->SSLCertificate) or (!*Self->SSLCertificate)) return ERR::FieldNotSet;

      Paths.Format = ssl_certificate_format(Self->SSLCertificate);
      if (Paths.Format IS SSLCERTFORMAT::NIL) return ERR::InvalidData;

      if (auto error = ResolvePath(Self->SSLCertificate, RSF::NIL, &Paths.Certificate); error != ERR::Okay) {
         return error;
      }

      if (Self->SSLPrivateKey) {
         if (ssl_private_key_format(Self->SSLPrivateKey) IS SSLCERTFORMAT::NIL) return ERR::InvalidData;

         if (auto error = ResolvePath(Self->SSLPrivateKey, RSF::NIL, &Paths.PrivateKeyPath); error != ERR::Okay) {
            return error;
         }
         Paths.PrivateKey.emplace(Paths.PrivateKeyPath);
      }

      if (Self->SSLKeyPassword) Paths.Password.emplace(Self->SSLKeyPassword);

      return ERR::Okay;
   }

//********************************************************************************************************************

  #ifdef _WIN32
    #include "win32/win32_ssl.cpp"
  #else
    static void netsocket_outgoing(HOSTHANDLE, APTR);
    static void clientsocket_outgoing(HOSTHANDLE, APTR);

    #include "openssl.cpp"
  #endif
#endif

//********************************************************************************************************************

static ERR resolve_name_receiver(APTR Custom, MSGID MsgID, int MsgType, APTR Message, int MsgSize);
static ERR resolve_addr_receiver(APTR Custom, MSGID MsgID, int MsgType, APTR Message, int MsgSize);

static void cleanup_proxy_config(void);

static ERR init_netclient(void);
static ERR init_netsocket(void);
static ERR init_clientsocket(void);
static ERR init_proxy(void);
static ERR init_netlookup(void);

static MsgHandler *glResolveNameHandler = nullptr;
static MsgHandler *glResolveAddrHandler = nullptr;

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   kt::Log log;

   CoreBase = argCoreBase;

   argModule->get(FID_Root, glNetworkModule);

   glPlatform = create_platform();
   if (!glPlatform) return ERR::NoSupport;
   if (auto error = glPlatform->initialise(argModule); error != ERR::Okay) return error;
   glSocketLimit = glPlatform->socket_limit();

   if (init_netclient() != ERR::Okay) return ERR::AddClass;
   if (init_netsocket() != ERR::Okay) return ERR::AddClass;
   if (init_clientsocket() != ERR::Okay) return ERR::AddClass;
   if (init_proxy() != ERR::Okay) return ERR::AddClass;
   if (init_netlookup() != ERR::Okay) return ERR::AddClass;

   glResolveNameMsgID = (MSGID)AllocateID(IDTYPE::MESSAGE);
   glResolveAddrMsgID = (MSGID)AllocateID(IDTYPE::MESSAGE);

   auto recv_function = C_FUNCTION(resolve_name_receiver);
   recv_function.Context = CurrentTask();
   if (AddMsgHandler(glResolveNameMsgID, &recv_function, &glResolveNameHandler) != ERR::Okay) {
      return ERR::Failed;
   }

   recv_function.Routine = (APTR)resolve_addr_receiver;
   if (AddMsgHandler(glResolveAddrMsgID, &recv_function, &glResolveAddrHandler) != ERR::Okay) {
      return ERR::Failed;
   }

   ResolvePath("system:config/ssl/", RSF::NO_FILE_CHECK, &glCertPath);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MODOpen(OBJECTPTR Module)
{
   Module->set(FID_FunctionList, glFunctions);
   return ERR::Okay;
}

//********************************************************************************************************************
// Note: Take care and attention with the order of operations during the expunge process, particuarly due to the
// background processes that are managed by the module.

static ERR MODExpunge(void)
{
   kt::Log log;

   cleanup_proxy_config();

   if (glResolveNameHandler) { FreeResource(glResolveNameHandler); glResolveNameHandler = nullptr; }
   if (glResolveAddrHandler) { FreeResource(glResolveAddrHandler); glResolveAddrHandler = nullptr; }

   if (glPlatform) glPlatform->expunge();

   if (clNetClient)    { FreeResource(clNetClient); clNetClient = nullptr; }
   if (clNetSocket)    { FreeResource(clNetSocket); clNetSocket = nullptr; }
   if (clClientSocket) { FreeResource(clClientSocket); clClientSocket = nullptr; }
   if (clProxy)        { FreeResource(clProxy); clProxy = nullptr; }
   if (clNetLookup)    { FreeResource(clNetLookup); clNetLookup = nullptr; }

#ifndef DISABLE_SSL
  #ifdef _WIN32
    ssl_cleanup();
  #else
    if (ssl_init) {
       if (glClientSSL)   { SSL_CTX_free(glClientSSL);   glClientSSL = nullptr; }
       if (glClientSSLNV) { SSL_CTX_free(glClientSSLNV); glClientSSLNV = nullptr; }
       if (glServerSSL)   { SSL_CTX_free(glServerSSL);   glServerSSL = nullptr; }
       ERR_free_strings();
       EVP_cleanup();
       CRYPTO_cleanup_all_ex_data();
    }
  #endif
#endif

   {
      std::lock_guard<std::mutex> lock(glmThreads);

      constexpr auto JOIN_TIMEOUT = std::chrono::milliseconds(2000);
      auto start_time = std::chrono::steady_clock::now();

      auto it = glThreads.begin();
      while (it != glThreads.end() and (std::chrono::steady_clock::now() - start_time) < JOIN_TIMEOUT) {
         if (*it and (*it)->joinable()) {
            (*it)->join();
            it = glThreads.erase(it);
         } else it = glThreads.erase(it);
      }

      glThreads.clear();
   }

   glPlatform.reset();

   return ERR::Okay;
}

namespace net {

/*********************************************************************************************************************

-FUNCTION-
AddressToStr: Converts an IPAddress structure to an IPAddress in dotted string form.

Converts an IPAddress structure to a string containing the IPAddress in dotted format.  Please free the resulting
string with <function>FreeResource</> once it is no longer required.

-INPUT-
struct(IPAddress) IPAddress: A pointer to the IPAddress structure.

-RESULT-
!cstr: The IP address is returned as an allocated string.

*********************************************************************************************************************/

CSTRING AddressToStr(IPAddress *Address)
{
   kt::Log log(__FUNCTION__);

   if (!Address) return nullptr;

   if (Address->Type IS IPADDR::V6) {
      char ipv6_str[46]; // 46 bytes is sufficient for both platforms
      const char *result = unified_inet_ntop(AF_INET6, Address->Data, ipv6_str, sizeof(ipv6_str));
      if (result) return kt::strclone(result);
      return nullptr;
   }
   else if (Address->Type IS IPADDR::V4) {
      struct in_addr addr;
      addr.s_addr = network_platform().host_to_long(Address->Data[0]);

      char buffer[16];
      auto result = network_platform().inet_ntop(AF_INET, &addr, buffer, sizeof(buffer));
      return result ? kt::strclone(result) : nullptr;
   }
   else {
      log.warning("Unsupported address type: %d", int(Address->Type));
      return nullptr;
   }
}

/*********************************************************************************************************************

-FUNCTION-
StrToAddress: Converts an IP Address in string form to an !IPAddress structure.

Converts an IPv4 or an IPv6 address in string format to an !IPAddress structure.  The `String` must be of form
`1.2.3.4` (IPv4) or `2001:db8::1` (IPv6).  IPv6 addresses are automatically detected by the presence of colons.

<pre>
struct IPAddress addr;
if (!StrToAddress("127.0.0.1", &addr)) {
   ...
}
</pre>

-INPUT-
cstr String:  A null-terminated string containing the IP Address in dotted format.
struct(IPAddress) Address: Must point to an !IPAddress structure that will be filled in.

-ERRORS-
Okay:    The `Address` was converted successfully.
NullArgs
Failed:  The `String` was not a valid IP Address.

*********************************************************************************************************************/

ERR StrToAddress(CSTRING Str, IPAddress *Address)
{
   if ((!Str) or (!Address)) return ERR::NullArgs;

   // Handle special cases
   if (kt::iequals(Str, "localhost") or kt::iequals(Str, "127.0.0.1")) {
      Address->Type = IPADDR::V4;
      Address->Data[0] = 0x7f000001; // 127.0.0.1
      Address->Data[1] = Address->Data[2] = Address->Data[3] = 0;
      return ERR::Okay;
   }
   else if (kt::iequals(Str, "::1")) {
      Address->Type = IPADDR::V6;
      kt::clearmem(&Address->Data, sizeof(Address->Data));
      ((uint8_t*)Address->Data)[15] = 1; // ::1 in byte format
      return ERR::Okay;
   }
   else if (kt::iequals(Str, "::")) {
      // Bind to all interfaces (IPv6)
      Address->Type = IPADDR::V6;
      kt::clearmem(&Address->Data, sizeof(Address->Data));
      return ERR::Okay;
   }
   else if (kt::iequals(Str, "0.0.0.0") or kt::iequals(Str, "*") or kt::iequals(Str, "")) {
      // Bind to all interfaces
      Address->Type = IPADDR::V4;
      kt::clearmem(&Address->Data, sizeof(Address->Data));
      return ERR::Okay;
   }
   else if (kt::iequals(Str, "255.255.255.255")) {
      // Needed to prevent confusion with INADDR_NONE
      Address->Type = IPADDR::V4;
      Address->Data[0] = 0xffffffff;
      Address->Data[1] = Address->Data[2] = Address->Data[3] = 0;
      return ERR::Okay;
   }

   // Try IPv6 first (contains colons)
   if (strchr(Str, ':')) {
      struct in6_addr ipv6_addr;
      if (unified_inet_pton(AF_INET6, Str, &ipv6_addr) IS 1) {
         kt::copymem(&ipv6_addr.s6_addr, Address->Data, 16);
         Address->Type = IPADDR::V6;
         return ERR::Okay;
      }
      return ERR::Failed;
   }

   // IPv4
   uint32_t result = unified_inet_addr(Str);

   if (result IS INADDR_NONE) return ERR::Failed;

   Address->Type = IPADDR::V4;
   Address->Data[0] = network_platform().long_to_host(result);
   Address->Data[1] = 0;
   Address->Data[2] = 0;
   Address->Data[3] = 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
HostToShort: Converts a 16 bit (unsigned) word from host to network byte order.

Converts a 16 bit (unsigned) word from host to network byte order.

-INPUT-
uint Value: Data in host byte order to be converted to network byte order

-RESULT-
uint: The word in network byte order

*********************************************************************************************************************/

uint32_t HostToShort(uint32_t Value)
{
   return uint32_t(network_platform().host_to_short(uint16_t(Value)));
}

/*********************************************************************************************************************

-FUNCTION-
HostToLong: Converts a 32 bit (unsigned) long from host to network byte order.

Converts a 32 bit (unsigned) long from host to network byte order.

-INPUT-
uint Value: Data in host byte order to be converted to network byte order

-RESULT-
uint: The long in network byte order

*********************************************************************************************************************/

uint32_t HostToLong(uint32_t Value)
{
   return network_platform().host_to_long(Value);
}

/*********************************************************************************************************************

-FUNCTION-
ShortToHost: Converts a 16 bit (unsigned) word from network to host byte order.

Converts a 16 bit (unsigned) word from network to host byte order.

-INPUT-
uint Value: Data in network byte order to be converted to host byte order

-RESULT-
uint: The Value in host byte order

*********************************************************************************************************************/

uint32_t ShortToHost(uint32_t Value)
{
   return uint32_t(network_platform().short_to_host(uint16_t(Value)));
}

/*********************************************************************************************************************

-FUNCTION-
LongToHost: Converts a 32 bit (unsigned) long from network to host byte order.

Converts a 32 bit (unsigned) long from network to host byte order.

-INPUT-
uint Value: Data in network byte order to be converted to host byte order

-RESULT-
uint: The Value in host byte order.

*********************************************************************************************************************/

uint32_t LongToHost(uint32_t Value)
{
   return network_platform().long_to_host(Value);
}

/*********************************************************************************************************************

-FUNCTION-
SetSSL: Alters SSL settings on an initialised NetSocket object.

Use the SetSSL() function to adjust the SSL capabilities of a NetSocket object.  The following commands are currently
available:

<list type="bullet">
<li><b>EnableSSL</b>: Starts an SSL handshaking process with the remote server.  Does nothing if the socket is already in SSL mode.</li>
<li><b>DisableSSL</b>: Disconnects the SSL connection and reverts to unencrypted mode.</li>
</list>

If a failure occurs when executing a command, the execution of all further commands is aborted and the error code is
returned immediately.

SetSSL() can also be used to check if SSL is supported in the current build, in which case `ERR::NoSecureSockets` will
be the return value if all other arguments are `NULL`.

-INPUT-
obj(NetSocket) NetSocket: The target NetSocket object.
cstr Command: Name of a command or option to set (case-sensitive, camel-case).
cstr Value: Value to set for the command or option.

-ERRORS-
Okay:
NullArgs: The NetSocket argument was not specified.
NoSecureSockets: SSL support is disabled in this build.
-END-

*********************************************************************************************************************/

ERR SetSSL(objNetSocket *Socket, CSTRING Command, CSTRING Value)
{
#ifndef DISABLE_SSL
   kt::Log log(__FUNCTION__);
   log.traceBranch("Command: %s = %s", Command, Value ? Value : "NULL");

   if ((!Socket) or (!Command)) return ERR::NullArgs;
   if (Socket->classID() != CLASSID::NETSOCKET) return ERR::WrongClass;

   auto hash = kt::strhash(Command);
   switch(hash) {
      case kt::strhash("EnableSSL"):
         if ((Socket->Flags & NSF::SSL) IS NSF::NIL) {
            if (auto error = sslSetup((extNetSocket *)Socket); error IS ERR::Okay) {
               if (error = sslConnect((extNetSocket *)Socket); error IS ERR::Okay) {
                  Socket->Flags |= NSF::SSL;
               }
               else sslDisconnect((extNetSocket*)Socket);
               return error;
            }
            else return error;
         }
         else return ERR::Okay; // Already enabled

      case kt::strhash("DisableSSL"): // Disconnect SSL (i.e. go back to unencrypted mode)
         if ((Socket->Flags & NSF::SSL) != NSF::NIL) {
            Socket->Flags &= ~NSF::SSL;
            sslDisconnect((extNetSocket *)Socket);
         }
         break;

      default:
         log.warning("Unknown SSL command: %s", Command);
         break;
   }

   return ERR::Okay;
#else
   return ERR::NoSecureSockets;
#endif
}

} // namespace

//********************************************************************************************************************
// Template function to handle SSL and socket sending for both NetSocket and ClientSocket

template<typename T>
static ERR send_data(T *Self, CPTR Buffer, size_t *Length)
{
   kt::Log log(__FUNCTION__);

   if (!*Length) return ERR::Okay;

#ifndef DISABLE_SSL
   if (Self->SSLHandle) {
      #ifdef _WIN32
         log.traceBranch("SSL Length: %d", int(*Length));

         size_t bytes_sent;
         if (auto error = ssl_write(Self->SSLHandle, Buffer, *Length, &bytes_sent); error IS SSL_OK) {
            if (*Length != bytes_sent) log.traceWarning("Sent %d of %d bytes.", int(bytes_sent), int(*Length));
            *Length = bytes_sent;
            return ERR::Okay;
         }
         else {
            *Length = 0;
            if (error IS SSL_ERROR_WOULD_BLOCK) {
               return log.traceWarning(ERR::BufferOverflow);
            }
            else return log.warning(ERR::Write);
         }
      #else
         log.traceBranch("SSL Length: %d", int(*Length));

         if (Self->HandshakeStatus IS SHS::WRITE) ssl_handshake_write(Self->Handle, Self);
         else if (Self->HandshakeStatus IS SHS::READ) ssl_handshake_read(Self->Handle, Self);

         if (Self->HandshakeStatus != SHS::NIL) {
            *Length = 0;
            if (Self->HandshakeStatus IS SHS::READ) {
               ssl_suspend_write_queue(Self->Handle.hosthandle());
               return ERR::Busy;
            }
            return ERR::BufferOverflow;
         }

         ssl_clear_error_queue();
         auto bytes_sent = SSL_write(Self->SSLHandle, Buffer, *Length);

         if (bytes_sent <= 0) {
            *Length = 0;
            auto ssl_error = SSL_get_error(Self->SSLHandle, bytes_sent);

            switch(ssl_error){
               case SSL_ERROR_WANT_WRITE:
                  log.traceWarning("Buffer overflow (SSL want write)");
                  return ERR::BufferOverflow;

               case SSL_ERROR_WANT_READ: {
                  log.trace("Handshake requested by server.");
                  Self->HandshakeStatus = SHS::READ;
                  auto read_callback = std::is_same<T, extNetSocket>::value ?
                     ssl_handshake_read_netsocket : ssl_handshake_read_clientsocket;
                  ssl_suspend_write_queue(Self->Handle.hosthandle());
                  network_platform().register_read(Self->Handle, read_callback, Self);
                  return ERR::Busy;
               }

               case SSL_ERROR_SYSCALL:
                  log.warning("SSL_write() SysError %d: %s", errno, strerror(errno));
                  return ERR::Write;

               case SSL_ERROR_SSL:
                  log.warning("SSL_write() failed: %s", ssl_error_name(ssl_error));
                  ssl_log_error_queue(log, "SSL_write");
                  return ERR::Write;

               default:
                  log.warning("SSL_write() failed: %s", ssl_error_name(ssl_error));
                  return ERR::Write;
            }
         }
         else {
            if (*Length != size_t(bytes_sent)) {
               log.trace("Sent %d of %d bytes.", int(bytes_sent), int(*Length));
            }
            *Length = bytes_sent;
         }
      #endif
      return ERR::Okay;
   }
#endif

   // Fallback to regular socket send
   size_t sent = *Length;
   auto error = network_platform().send(Self->Handle, Buffer, sent);
   *Length = sent;
   return error;
}

//********************************************************************************************************************

#include "netsocket/netsocket.cpp"
#include "clientsocket/clientsocket.cpp"
#include "class_proxy.cpp"
#include "class_netlookup.cpp"
#include "netclient/netclient.cpp"

//********************************************************************************************************************

static STRUCTS glStructures = {
   { "DNSEntry",  sizeof(DNSEntry) },
   { "IPAddress", sizeof(IPAddress) },
   { "NetQueue",  sizeof(NetQueue) }
};

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, &glStructures)
extern "C" struct ModHeader * register_network_module() { return &ModHeader; }
