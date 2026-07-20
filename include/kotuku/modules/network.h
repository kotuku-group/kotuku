#pragma once

// Name:      network.h
// Copyright: Paul Manias © 2005-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_NETWORK (1)

class objNetClient;
class objClientSocket;
class objProxy;
class objNetLookup;
class objNetSocket;
class objNetServer;

// Address types for the IPAddress structure.

enum class IPADDR : int {
   NIL = 0,
   V4 = 0,
   V6 = 1,
};

// NetSocket options

enum class NSF : uint32_t {
   NIL = 0,
   SSL = 0x00000001,
   SYNCHRONOUS = 0x00000002,
   LOG_ALL = 0x00000004,
   BROADCAST = 0x00000008,
   UDP = 0x00000010,
   DISABLE_SERVER_VERIFY = 0x00000020,
   MULTI_CONNECT = 0x00000040,
   KEEP_ALIVE = 0x00000080,
};

DEFINE_ENUM_FLAG_OPERATORS(NSF)

// Options for NetLookup

enum class NLF : uint32_t {
   NIL = 0,
   NO_CACHE = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(NLF)

// NetSocket states

enum class NTC : int {
   NIL = 0,
   DISCONNECTED = 0,
   RESOLVING = 1,
   CONNECTING = 2,
   HANDSHAKING = 3,
   CONNECTED = 4,
   MULTISTATE = 5,
};

// These error codes for certificate validation match the OpenSSL error codes (X509 definitions)

#define SCV_OK 0
#define SCV_UNABLE_TO_GET_ISSUER_CERT 2
#define SCV_UNABLE_TO_GET_CRL 3
#define SCV_UNABLE_TO_DECRYPT_CERT_SIGNATURE 4
#define SCV_UNABLE_TO_DECRYPT_CRL_SIGNATURE 5
#define SCV_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY 6
#define SCV_CERT_SIGNATURE_FAILURE 7
#define SCV_CRL_SIGNATURE_FAILURE 8
#define SCV_CERT_NOT_YET_VALID 9
#define SCV_CERT_HAS_EXPIRED 10
#define SCV_CRL_NOT_YET_VALID 11
#define SCV_CRL_HAS_EXPIRED 12
#define SCV_ERROR_IN_CERT_NOT_BEFORE_FIELD 13
#define SCV_ERROR_IN_CERT_NOT_AFTER_FIELD 14
#define SCV_ERROR_IN_CRL_LAST_UPDATE_FIELD 15
#define SCV_ERROR_IN_CRL_NEXT_UPDATE_FIELD 16
#define SCV_OUT_OF_MEM 17
#define SCV_DEPTH_ZERO_SELF_SIGNED_CERT 18
#define SCV_SELF_SIGNED_CERT_IN_CHAIN 19
#define SCV_UNABLE_TO_GET_ISSUER_CERT_LOCALLY 20
#define SCV_UNABLE_TO_VERIFY_LEAF_SIGNATURE 21
#define SCV_CERT_CHAIN_TOO_LONG 22
#define SCV_CERT_REVOKED 23
#define SCV_INVALID_CA 24
#define SCV_PATH_LENGTH_EXCEEDED 25
#define SCV_INVALID_PURPOSE 26
#define SCV_CERT_UNTRUSTED 27
#define SCV_CERT_REJECTED 28
#define SCV_SUBJECT_ISSUER_MISMATCH 29
#define SCV_AKID_SKID_MISMATCH 30
#define SCV_AKID_ISSUER_SERIAL_MISMATCH 31
#define SCV_KEYUSAGE_NO_CERTSIGN 32
#define SCV_APPLICATION_VERIFICATION 50


#ifdef __linux__
typedef int SOCKET_HANDLE;
#elif _WIN32
typedef uintptr_t SOCKET_HANDLE; // Pointer-sized storage for Winsock SOCKET handles without including winsock.h
#elif __APPLE__
typedef int SOCKET_HANDLE;
#else
#error "No support for this platform"
#endif
struct IPAddress {
   uint32_t Data[4];    // 128-bit array for supporting both V4 (32-bit host order) and V6 (8-bit byte order) IP addresses.
   IPADDR   Type;       // Identifies the address Data value as a V4 or V6 address type.
   int      Port;       // For UDP packets, identifies the client port number in host byte order.
};

// NetClient class definition

#define VER_NETCLIENT (1.000000)

class objNetClient : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::NETCLIENT;
   static constexpr CSTRING CLASS_NAME = "NetClient";

   using create = kt::Create<objNetClient>;
   objNetClient(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   objNetClient * Next;              // The next client IP with connections to the NetServer.
   objNetClient * Prev;              // The previous client IP with connections to the NetServer.
   objClientSocket * Connections;    // Pointer to the first established socket connection for the client IP.
   APTR ClientData;                  // A custom pointer available for userspace.
   int  TotalConnections;            // The total number of current socket connections for the IP address.

#ifdef PRV_NETCLIENT
    struct IPAddress IP; // IP address of the client.
#endif

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getNext(objNetClient * &Value) noexcept {
      Value = this->Next;
      return ERR::Okay;
   }

   inline ERR getPrev(objNetClient * &Value) noexcept {
      Value = this->Prev;
      return ERR::Okay;
   }

   inline ERR getConnections(objClientSocket * &Value) noexcept {
      Value = this->Connections;
      return ERR::Okay;
   }

   inline ERR getClientData(APTR &Value) noexcept {
      Value = this->ClientData;
      return ERR::Okay;
   }

   inline ERR getTotalConnections(int &Value) noexcept {
      Value = this->TotalConnections;
      return ERR::Okay;
   }

   inline ERR getIP(struct IPAddress * &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->GetValue(this, &Value);
   }


   // Customised field setting

   inline ERR setClientData(APTR Value) noexcept {
      this->ClientData = Value;
      return ERR::Okay;
   }

};

// ClientSocket class definition

#define VER_CLIENTSOCKET (1.000000)

class objClientSocket : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::CLIENTSOCKET;
   static constexpr CSTRING CLASS_NAME = "ClientSocket";

   using create = kt::Create<objClientSocket>;
   objClientSocket(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   int64_t ConnectTime;       // System time for the creation of this socket.
   objClientSocket * Prev;    // Previous socket in the chain.
   objClientSocket * Next;    // Next socket in the chain.
   objNetClient * Client;     // Parent client object (IP address).
   APTR    ClientData;        // Available for client data storage.
   NTC     State;             // The current connection state of the ClientSocket object.

   // Action stubs

   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   template <class T, class U> ERR read(APTR Buffer, T Size, U *Result) noexcept {
      static_assert(std::is_integral<U>::value, "Result value must be an integer type");
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = U(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   template <class T> ERR read(APTR Buffer, T Size) noexcept {
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      return Action(AC::Read, this, &read);
   }
   inline ERR write(CPTR Buffer, int Size, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer.c_str(), int(Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }

   // Customised field getting

   inline ERR getConnectTime(int64_t &Value) noexcept {
      Value = this->ConnectTime;
      return ERR::Okay;
   }

   inline ERR getPrev(objClientSocket * &Value) noexcept {
      Value = this->Prev;
      return ERR::Okay;
   }

   inline ERR getNext(objClientSocket * &Value) noexcept {
      Value = this->Next;
      return ERR::Okay;
   }

   inline ERR getClient(objNetClient * &Value) noexcept {
      Value = this->Client;
      return ERR::Okay;
   }

   inline ERR getClientData(APTR &Value) noexcept {
      Value = this->ClientData;
      return ERR::Okay;
   }

   inline ERR getState(NTC &Value) noexcept {
      Value = this->State;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setState(const NTC Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

};

// Proxy class definition

#define VER_PROXY (1.000000)

// Proxy methods

namespace prx {
struct DeleteRecord { static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Find { int Port; int Enabled; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct FindNext { static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objProxy : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::PROXY;
   static constexpr CSTRING CLASS_NAME = "Proxy";

   using create = kt::Create<objProxy>;
   objProxy(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string NetworkFilter;    // The name of the network that the proxy is limited to.
   std::string GatewayFilter;    // The IP address of the gateway that the proxy is limited to.
   std::string Username;         // The username to use when authenticating against the proxy server.
   std::string Password;         // The password to use when authenticating against the proxy server.
   std::string ProxyName;        // A human readable name for the proxy server entry.
   std::string Server;           // The destination address of the proxy server - may be an IP address or resolvable domain name.
   int Port;                     // Defines the ports supported by this proxy.
   int ServerPort;               // The port that is used for proxy server communication.
   int Enabled;                  // All proxies are enabled by default until this field is set to false.
   int Record;                   // The unique ID of the current proxy record.
   int Host;                     // If true, the proxy settings are derived from the host operating system's default settings.

   // Action stubs

   inline ERR disable() noexcept { return Action(AC::Disable, this, nullptr); }
   inline ERR enable() noexcept { return Action(AC::Enable, this, nullptr); }
   inline ERR saveSettings() noexcept { return Action(AC::SaveSettings, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR deleteRecord() noexcept {
      return Action(AC(-1), this, nullptr);
   }
   inline ERR find(int Port, int Enabled) noexcept {
      struct prx::Find args = { Port, Enabled };
      return Action(AC(-2), this, &args);
   }
   inline ERR findNext() noexcept {
      return Action(AC(-3), this, nullptr);
   }

   // Customised field getting

   inline ERR getNetworkFilter(std::string_view &Value) noexcept {
      Value = this->NetworkFilter;
      return ERR::Okay;
   }

   inline ERR getGatewayFilter(std::string_view &Value) noexcept {
      Value = this->GatewayFilter;
      return ERR::Okay;
   }

   inline ERR getUsername(std::string_view &Value) noexcept {
      Value = this->Username;
      return ERR::Okay;
   }

   inline ERR getPassword(std::string_view &Value) noexcept {
      Value = this->Password;
      return ERR::Okay;
   }

   inline ERR getProxyName(std::string_view &Value) noexcept {
      Value = this->ProxyName;
      return ERR::Okay;
   }

   inline ERR getServer(std::string_view &Value) noexcept {
      Value = this->Server;
      return ERR::Okay;
   }

   inline ERR getPort(int &Value) noexcept {
      Value = this->Port;
      return ERR::Okay;
   }

   inline ERR getServerPort(int &Value) noexcept {
      Value = this->ServerPort;
      return ERR::Okay;
   }

   inline ERR getEnabled(int &Value) noexcept {
      Value = this->Enabled;
      return ERR::Okay;
   }

   inline ERR getRecord(int &Value) noexcept {
      Value = this->Record;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setNetworkFilter(const std::string_view &Value) noexcept {
      this->NetworkFilter = Value;
      return ERR::Okay;
   }

   inline ERR setGatewayFilter(const std::string_view &Value) noexcept {
      this->GatewayFilter = Value;
      return ERR::Okay;
   }

   inline ERR setUsername(const std::string_view &Value) noexcept {
      this->Username = Value;
      return ERR::Okay;
   }

   inline ERR setPassword(const std::string_view &Value) noexcept {
      this->Password = Value;
      return ERR::Okay;
   }

   inline ERR setProxyName(const std::string_view &Value) noexcept {
      this->ProxyName = Value;
      return ERR::Okay;
   }

   inline ERR setServer(const std::string_view &Value) noexcept {
      this->Server = Value;
      return ERR::Okay;
   }

   inline ERR setPort(const int Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setServerPort(const int Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setEnabled(const int Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setRecord(const int Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

};

// NetLookup class definition

#define VER_NETLOOKUP (1.000000)

// NetLookup methods

namespace nl {
struct ResolveName { std::string_view HostName; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct ResolveAddress { std::string_view Address; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct BlockingResolveName { std::string_view HostName; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct BlockingResolveAddress { std::string_view Address; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objNetLookup : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::NETLOOKUP;
   static constexpr CSTRING CLASS_NAME = "NetLookup";

   using create = kt::Create<objNetLookup>;
   objNetLookup(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   int64_t ClientData;    // Optional user data storage
   NLF     Flags;         // Optional flags

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR resolveName(const std::string_view &HostName) noexcept {
      struct nl::ResolveName args = { HostName };
      return Action(AC(-1), this, &args);
   }
   inline ERR resolveAddress(const std::string_view &Address) noexcept {
      struct nl::ResolveAddress args = { Address };
      return Action(AC(-2), this, &args);
   }
   inline ERR blockingResolveName(const std::string_view &HostName) noexcept {
      struct nl::BlockingResolveName args = { HostName };
      return Action(AC(-3), this, &args);
   }
   inline ERR blockingResolveAddress(const std::string_view &Address) noexcept {
      struct nl::BlockingResolveAddress args = { Address };
      return Action(AC(-4), this, &args);
   }

   // Customised field getting

   inline ERR getClientData(int64_t &Value) noexcept {
      Value = this->ClientData;
      return ERR::Okay;
   }

   inline ERR getFlags(NLF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getAddresses(std::span<struct IPAddress> &Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      auto get_field = (ERR (*)(APTR, std::span<struct IPAddress> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getHostName(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }


   // Customised field setting

   inline ERR setClientData(const int64_t Value) noexcept {
      this->ClientData = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const NLF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};

// NetSocket class definition

#define VER_NETSOCKET (1.000000)

// NetSocket methods

namespace ns {
struct Connect { std::string_view Address; int Port; double Timeout; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetLocalIPAddress { struct IPAddress *Address; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SendTo { struct IPAddress *Dest; APTR Data; int Length; int BytesSent; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct RecvFrom { struct IPAddress *Source; APTR Buffer; int BufferSize; int BytesRead; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct JoinMulticastGroup { std::string_view Group; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct LeaveMulticastGroup { std::string_view Group; static const AC id = AC(-6); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objNetSocket : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::NETSOCKET;
   static constexpr CSTRING CLASS_NAME = "NetSocket";

   using create = kt::Create<objNetSocket>;
   objNetSocket(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   APTR ClientData;        // A client-defined value that can be useful in action notify events.
   std::string Address;    // An IP address or domain name to connect to.
   NTC  State;             // The current connection state of the NetSocket object.
   ERR  Error;             // Information about the last error that occurred during a NetSocket operation
   int  Port;              // The port number to use for connections.
   NSF  Flags;             // Optional flags.
   int  MsgLimit;          // Limits the size of incoming and outgoing data packets.
   int  MaxPacketSize;     // Maximum UDP packet size for sending and receiving data.
   int  MulticastTTL;      // Time-to-live (hop limit) for multicast packets.

   // Action stubs

   inline ERR dataFeed(OBJECTPTR Object, DATA Datatype, const void *Buffer, int Size) noexcept {
      struct acDataFeed args = { Object, Datatype, Buffer, Size };
      return Action(AC::DataFeed, this, &args);
   }
   inline ERR disable() noexcept { return Action(AC::Disable, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   template <class T, class U> ERR read(APTR Buffer, T Size, U *Result) noexcept {
      static_assert(std::is_integral<U>::value, "Result value must be an integer type");
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = U(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   template <class T> ERR read(APTR Buffer, T Size) noexcept {
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      return Action(AC::Read, this, &read);
   }
   inline ERR write(CPTR Buffer, int Size, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer.c_str(), int(Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR connect(const std::string_view &Address, int Port, double Timeout) noexcept {
      struct ns::Connect args = { Address, Port, Timeout };
      return Action(AC(-1), this, &args);
   }
   inline ERR getLocalIPAddress(struct IPAddress * Address) noexcept {
      struct ns::GetLocalIPAddress args = { Address };
      return Action(AC(-2), this, &args);
   }
   inline ERR sendTo(struct IPAddress * Dest, APTR Data, int Length, int * BytesSent) noexcept {
      struct ns::SendTo args = { Dest, Data, Length, (int)0 };
      ERR error = Action(AC(-3), this, &args);
      if (BytesSent) *BytesSent = args.BytesSent;
      return error;
   }
   inline ERR recvFrom(struct IPAddress * Source, APTR Buffer, int BufferSize, int * BytesRead) noexcept {
      struct ns::RecvFrom args = { Source, Buffer, BufferSize, (int)0 };
      ERR error = Action(AC(-4), this, &args);
      if (BytesRead) *BytesRead = args.BytesRead;
      return error;
   }
   inline ERR joinMulticastGroup(const std::string_view &Group) noexcept {
      struct ns::JoinMulticastGroup args = { Group };
      return Action(AC(-5), this, &args);
   }
   inline ERR leaveMulticastGroup(const std::string_view &Group) noexcept {
      struct ns::LeaveMulticastGroup args = { Group };
      return Action(AC(-6), this, &args);
   }

   // Customised field getting

   inline ERR getClientData(APTR &Value) noexcept {
      Value = this->ClientData;
      return ERR::Okay;
   }

   inline ERR getAddress(std::string_view &Value) noexcept {
      Value = this->Address;
      return ERR::Okay;
   }

   inline ERR getState(NTC &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getPort(int &Value) noexcept {
      Value = this->Port;
      return ERR::Okay;
   }

   inline ERR getFlags(NSF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getMsgLimit(int &Value) noexcept {
      Value = this->MsgLimit;
      return ERR::Okay;
   }

   inline ERR getMaxPacketSize(int &Value) noexcept {
      Value = this->MaxPacketSize;
      return ERR::Okay;
   }

   inline ERR getMulticastTTL(int &Value) noexcept {
      Value = this->MulticastTTL;
      return ERR::Okay;
   }

   inline ERR getHandle(APTR &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getFeedback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getIncoming(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOutgoing(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOutQueueSize(int &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setClientData(APTR Value) noexcept {
      this->ClientData = Value;
      return ERR::Okay;
   }

   inline ERR setAddress(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Address = Value;
      return ERR::Okay;
   }

   inline ERR setState(const NTC Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setPort(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Port = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const NSF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setMsgLimit(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->MsgLimit = Value;
      return ERR::Okay;
   }

   inline ERR setMaxPacketSize(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->MaxPacketSize = Value;
      return ERR::Okay;
   }

   inline ERR setMulticastTTL(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->MulticastTTL = Value;
      return ERR::Okay;
   }

   inline ERR setHandle(APTR Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, 0x08000508, Value);
   }

   inline ERR setFeedback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setIncoming(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOutgoing(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};

// NetServer class definition

#define VER_NETSERVER (1.000000)

// NetServer methods

namespace ns {
struct DisconnectClient { objNetClient *Client; static const AC id = AC(-7); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DisconnectSocket { objClientSocket *Socket; static const AC id = AC(-8); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objNetServer : public objNetSocket {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::NETSERVER;
   static constexpr CSTRING CLASS_NAME = "NetServer";

   using create = kt::Create<objNetServer>;
   objNetServer(objMetaClass *pClass, OBJECTID pUID) noexcept : objNetSocket(pClass, pUID) {}

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   template <class T, class U> ERR read(APTR Buffer, T Size, U *Result) noexcept {
      static_assert(std::is_integral<U>::value, "Result value must be an integer type");
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      if (auto error = Action(AC::Read, this, &read); error IS ERR::Okay) {
         *Result = U(read.Result);
         return ERR::Okay;
      }
      else { *Result = 0; return error; }
   }
   template <class T> ERR read(APTR Buffer, T Size) noexcept {
      static_assert(std::is_integral<T>::value, "Size value must be an integer type");
      const int bytes = (Size > 0x7fffffff) ? 0x7fffffff : Size;
      struct acRead read = { (int8_t *)Buffer, bytes };
      return Action(AC::Read, this, &read);
   }
   inline ERR write(CPTR Buffer, int Size, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer.c_str(), int(Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR disconnectClient(objNetClient * Client) noexcept {
      struct ns::DisconnectClient args = { Client };
      return Action(AC(-7), this, &args);
   }
   inline ERR disconnectSocket(objClientSocket * Socket) noexcept {
      struct ns::DisconnectSocket args = { Socket };
      return Action(AC(-8), this, &args);
   }

   // Customised field getting

   inline ERR getClients(OBJECTPTR &Value) noexcept {
      Value = *((OBJECTPTR *)(((int8_t *)this) + CLASS_OFFSET_NETSOCKET + 0));
      return ERR::Okay;
   }

   inline ERR getSSLCertificate(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + CLASS_OFFSET_NETSOCKET + 8));
      return ERR::Okay;
   }

   inline ERR getSSLKeyPassword(std::string_view &Value) noexcept {
      Value = *((std::string *)(((int8_t *)this) + CLASS_OFFSET_NETSOCKET + 72));
      return ERR::Okay;
   }

   inline ERR getBacklog(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + CLASS_OFFSET_NETSOCKET + 104));
      return ERR::Okay;
   }

   inline ERR getClientLimit(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + CLASS_OFFSET_NETSOCKET + 108));
      return ERR::Okay;
   }

   inline ERR getSocketLimit(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + CLASS_OFFSET_NETSOCKET + 112));
      return ERR::Okay;
   }

   inline ERR getTotalClients(int &Value) noexcept {
      Value = *((int *)(((int8_t *)this) + CLASS_OFFSET_NETSOCKET + 116));
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setSSLCertificate(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, 0x00804500, &Value);
   }

   inline ERR setSSLKeyPassword(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, 0x00804500, &Value);
   }

   inline ERR setBacklog(const int Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setClientLimit(const int Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setSocketLimit(const int Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

};

inline ERR nsCreate(objNetSocket **NewNetSocketOut, OBJECTID ListenerID, APTR ClientData) {
   if ((*NewNetSocketOut = objNetSocket::create::global(fl::Listener(ListenerID), fl::ClientData(ClientData)))) return ERR::Okay;
   else return ERR::CreateObject;
}
#ifdef KOTUKU_STATIC
#define JUMPTABLE_NETWORK [[maybe_unused]] static struct NetworkBase *NetworkBase = nullptr;
#else
#define JUMPTABLE_NETWORK struct NetworkBase *NetworkBase = nullptr;
#endif

struct NetworkBase {
#ifndef KOTUKU_STATIC
   ERR (*_StrToAddress)(const std::string_view &String, struct IPAddress *Address);
   ERR (*_AddressToStr)(struct IPAddress *IPAddress, std::string *Result);
   uint32_t (*_HostToShort)(uint32_t Value);
   uint32_t (*_HostToLong)(uint32_t Value);
   uint32_t (*_ShortToHost)(uint32_t Value);
   uint32_t (*_LongToHost)(uint32_t Value);
   ERR (*_SetSSL)(objNetSocket *NetSocket, const std::string_view &Command, const std::string_view &Value);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_NETWORK_MODULE)
extern struct NetworkBase *NetworkBase;
namespace net {
inline ERR StrToAddress(const std::string_view &String, struct IPAddress *Address) { return NetworkBase->_StrToAddress(String,Address); }
inline ERR AddressToStr(struct IPAddress *IPAddress, std::string *Result) { return NetworkBase->_AddressToStr(IPAddress,Result); }
inline uint32_t HostToShort(uint32_t Value) { return NetworkBase->_HostToShort(Value); }
inline uint32_t HostToLong(uint32_t Value) { return NetworkBase->_HostToLong(Value); }
inline uint32_t ShortToHost(uint32_t Value) { return NetworkBase->_ShortToHost(Value); }
inline uint32_t LongToHost(uint32_t Value) { return NetworkBase->_LongToHost(Value); }
inline ERR SetSSL(objNetSocket *NetSocket, const std::string_view &Command, const std::string_view &Value) { return NetworkBase->_SetSSL(NetSocket,Command,Value); }
} // namespace
#else
namespace net {
extern ERR StrToAddress(const std::string_view &String, struct IPAddress *Address);
extern ERR AddressToStr(struct IPAddress *IPAddress, std::string *Result);
extern uint32_t HostToShort(uint32_t Value);
extern uint32_t HostToLong(uint32_t Value);
extern uint32_t ShortToHost(uint32_t Value);
extern uint32_t LongToHost(uint32_t Value);
extern ERR SetSSL(objNetSocket *NetSocket, const std::string_view &Command, const std::string_view &Value);
} // namespace
#endif // KOTUKU_STATIC