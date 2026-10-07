#pragma once

// Name:      websocket.h
// Copyright: Paul Manias © 2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_WEBSOCKET (1)

#include <kotuku/modules/network.h>

class objWebSocketServer;
class objWebSocket;

// WebSocket connection states.

enum class WSS : int {
   NIL = 0,
   CONNECTING = 0,
   OPEN = 1,
   CLOSING = 2,
   CLOSED = 3,
};

// WebSocket message types.

enum class WSM : int {
   NIL = 0,
   TEXT = 1,
   BINARY = 2,
};

// WebSocket flags.

enum class WSF : uint32_t {
   NIL = 0,
   STREAM_MESSAGES = 0x00000001,
   NO_UTF8_CHECK = 0x00000002,
   DISABLE_SERVER_VERIFY = 0x00000004,
   NO_AUTO_PONG = 0x00000008,
   SSL = 0x00000010,
   EXTERNAL_LISTENER = 0x00000020,
};

DEFINE_ENUM_FLAG_OPERATORS(WSF)

// External listener events forwarded to WebSocketServer.Dispatch().

enum class WSE : int {
   NIL = 0,
   INCOMING = 0,
   OUTGOING = 1,
   DISCONNECTED = 2,
};

// WebSocket close status codes, as defined by RFC 6455 section 7.4.1.

enum class WSC : int {
   NIL = 0,
   NORMAL = 1000,
   GOING_AWAY = 1001,
   PROTOCOL_ERROR = 1002,
   UNSUPPORTED_DATA = 1003,
   NO_STATUS = 1005,
   ABNORMAL = 1006,
   INVALID_PAYLOAD = 1007,
   POLICY_VIOLATION = 1008,
   MESSAGE_TOO_BIG = 1009,
   MANDATORY_EXTENSION = 1010,
   INTERNAL_ERROR = 1011,
   TLS_HANDSHAKE = 1015,
};

struct WSRequest {
   std::string Path;         // The request path, excluding the query string.
   std::string Query;        // The query string without its leading ?, or empty.
   std::string Host;         // The value of the Host header.
   std::string Origin;       // The value of the Origin header, or empty.
   std::string Protocols;    // The requested subprotocols as a comma-separated list, or empty.
   std::string Protocol;     // The subprotocol that the server will select, or empty.
   std::string Address;      // The client's IP address.
   std::string Headers;      // The request's header fields, one Name: Value pair per line.
};

// WebSocketServer class definition

#define VER_WEBSOCKETSERVER (1.000000)

// WebSocketServer methods

namespace wsv {
struct Broadcast { std::span<const int8_t> Data; WSM Type; int Recipients; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct DisconnectClient { objWebSocket *WebSocket; int Code; CSTRING Reason; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Adopt { objClientSocket *Socket; std::span<const int8_t> RequestData; objWebSocket *Connection; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Dispatch { objClientSocket *Socket; WSE Event; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objWebSocketServer : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::WEBSOCKETSERVER;
   static constexpr CSTRING CLASS_NAME = "WebSocketServer";

   using create = kt::Create<objWebSocketServer>;
   objWebSocketServer(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   int64_t MaxMessageSize;        // The maximum size of a received message, in bytes.
   int64_t MaxFrameSize;          // The maximum size of a single received frame, in bytes.
   int64_t SendLimit;             // The maximum number of queued bytes awaiting transmission on each connection.
   double  PingInterval;          // Enables keep-alive pings after a period of inactivity, in seconds.
   double  CloseTimeout;          // The time allowed for the closing handshake, in seconds.
   double  HandshakeTimeout;      // The time allowed for a client to send its opening handshake request, in seconds.
   APTR    ClientData;            // Free for user data storage.
   std::string Address;           // The local address to bind.
   std::string Path;              // Restricts the server to one request path.
   std::string Protocols;         // Comma-separated subprotocols supported by the server, in order of preference.
   std::string SSLCertificate;    // The TLS certificate file for wss:// connections.
   std::string SSLPrivateKey;     // The TLS private key file for wss:// connections.
   std::string SSLKeyPassword;    // The password for an encrypted TLS private key.
   int     Port;                  // The local port to bind.
   WSF     Flags;                 // Optional flags.
   int     TotalConnections;      // The number of open connections.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR broadcast(std::span<const int8_t> Data, WSM Type, int * Recipients) noexcept {
      struct wsv::Broadcast args = { Data, Type, (int)0 };
      ERR error = Action(AC(-1), this, &args);
      if (Recipients) *Recipients = args.Recipients;
      return error;
   }
   inline ERR disconnectClient(objWebSocket * WebSocket, int Code, CSTRING Reason) noexcept {
      struct wsv::DisconnectClient args = { WebSocket, Code, Reason };
      return Action(AC(-2), this, &args);
   }
   inline ERR adopt(objClientSocket * Socket, std::span<const int8_t> RequestData, objWebSocket ** Connection) noexcept {
      struct wsv::Adopt args = { Socket, RequestData, (objWebSocket *)0 };
      ERR error = Action(AC(-3), this, &args);
      if (Connection) *Connection = args.Connection;
      return error;
   }
   inline ERR dispatch(objClientSocket * Socket, WSE Event) noexcept {
      struct wsv::Dispatch args = { Socket, Event };
      return Action(AC(-4), this, &args);
   }

   // Customised field getting

   inline ERR getMaxMessageSize(int64_t &Value) noexcept {
      Value = this->MaxMessageSize;
      return ERR::Okay;
   }

   inline ERR getMaxFrameSize(int64_t &Value) noexcept {
      Value = this->MaxFrameSize;
      return ERR::Okay;
   }

   inline ERR getSendLimit(int64_t &Value) noexcept {
      Value = this->SendLimit;
      return ERR::Okay;
   }

   inline ERR getPingInterval(double &Value) noexcept {
      Value = this->PingInterval;
      return ERR::Okay;
   }

   inline ERR getCloseTimeout(double &Value) noexcept {
      Value = this->CloseTimeout;
      return ERR::Okay;
   }

   inline ERR getHandshakeTimeout(double &Value) noexcept {
      Value = this->HandshakeTimeout;
      return ERR::Okay;
   }

   inline ERR getClientData(APTR &Value) noexcept {
      Value = this->ClientData;
      return ERR::Okay;
   }

   inline ERR getAddress(std::string_view &Value) noexcept {
      Value = this->Address;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getProtocols(std::string_view &Value) noexcept {
      Value = this->Protocols;
      return ERR::Okay;
   }

   inline ERR getSSLCertificate(std::string_view &Value) noexcept {
      Value = this->SSLCertificate;
      return ERR::Okay;
   }

   inline ERR getSSLPrivateKey(std::string_view &Value) noexcept {
      Value = this->SSLPrivateKey;
      return ERR::Okay;
   }

   inline ERR getSSLKeyPassword(std::string_view &Value) noexcept {
      Value = this->SSLKeyPassword;
      return ERR::Okay;
   }

   inline ERR getPort(int &Value) noexcept {
      Value = this->Port;
      return ERR::Okay;
   }

   inline ERR getFlags(WSF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getTotalConnections(int &Value) noexcept {
      Value = this->TotalConnections;
      return ERR::Okay;
   }

   inline ERR getAccept(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getConnected(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getDisconnected(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setMaxMessageSize(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setMaxFrameSize(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[23];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setSendLimit(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setPingInterval(const double Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setCloseTimeout(const double Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setHandshakeTimeout(const double Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setClientData(APTR Value) noexcept {
      this->ClientData = Value;
      return ERR::Okay;
   }

   inline ERR setAddress(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Address = Value;
      return ERR::Okay;
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      this->Path = Value;
      return ERR::Okay;
   }

   inline ERR setProtocols(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setSSLCertificate(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->SSLCertificate = Value;
      return ERR::Okay;
   }

   inline ERR setSSLPrivateKey(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->SSLPrivateKey = Value;
      return ERR::Okay;
   }

   inline ERR setSSLKeyPassword(const std::string_view &Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->SSLKeyPassword = Value;
      return ERR::Okay;
   }

   inline ERR setPort(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Port = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const WSF Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setAccept(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[11];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setConnected(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setDisconnected(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};

// WebSocket class definition

#define VER_WEBSOCKET (1.000000)

// WebSocket methods

namespace wsk {
struct Send { std::span<const int8_t> Data; WSM Type; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Ping { std::span<const int8_t> Data; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Close { int Code; CSTRING Reason; static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objWebSocket : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::WEBSOCKET;
   static constexpr CSTRING CLASS_NAME = "WebSocket";

   using create = kt::Create<objWebSocket>;
   objWebSocket(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   int64_t MaxMessageSize;    // The maximum size of a received message, in bytes.
   int64_t MaxFrameSize;      // The maximum size of a single received frame, in bytes.
   int64_t SendLimit;         // The maximum number of queued bytes awaiting transmission.
   double  ConnectTimeout;    // The time allowed to connect to the server, in seconds.
   double  DataTimeout;       // The time allowed for the server's handshake response, in seconds.
   double  PingInterval;      // Enables keep-alive pings after a period of inactivity, in seconds.
   double  CloseTimeout;      // The time allowed for the closing handshake, in seconds.
   APTR    ClientData;        // Free for user data storage.
   std::string Location;      // The ws:// or wss:// URL to connect to.
   std::string Protocols;     // Comma-separated subprotocols to request, in order of preference.
   std::string Protocol;      // The subprotocol selected by the server.
   std::string Origin;        // An optional Origin request header.
   std::string ProxyServer;   // An HTTP proxy server for the connection.
   std::string CloseReason;   // The received or generated close reason.
   std::string Redirect;      // The target of a redirecting handshake response.
   int     ProxyPort;         // The port of the ProxyServer.
   WSF     Flags;             // Optional flags.
   WSS     State;             // The current connection state.
   ERR     Error;             // The last error.
   int     CloseCode;         // The received or generated close status code.
   int     Status;            // The HTTP status of the handshake response.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR send(std::span<const int8_t> Data, WSM Type) noexcept {
      struct wsk::Send args = { Data, Type };
      return Action(AC(-1), this, &args);
   }
   inline ERR ping(std::span<const int8_t> Data) noexcept {
      struct wsk::Ping args = { Data };
      return Action(AC(-2), this, &args);
   }
   inline ERR close(int Code, CSTRING Reason) noexcept {
      struct wsk::Close args = { Code, Reason };
      return Action(AC(-3), this, &args);
   }

   // Customised field getting

   inline ERR getMaxMessageSize(int64_t &Value) noexcept {
      Value = this->MaxMessageSize;
      return ERR::Okay;
   }

   inline ERR getMaxFrameSize(int64_t &Value) noexcept {
      Value = this->MaxFrameSize;
      return ERR::Okay;
   }

   inline ERR getSendLimit(int64_t &Value) noexcept {
      Value = this->SendLimit;
      return ERR::Okay;
   }

   inline ERR getConnectTimeout(double &Value) noexcept {
      Value = this->ConnectTimeout;
      return ERR::Okay;
   }

   inline ERR getDataTimeout(double &Value) noexcept {
      Value = this->DataTimeout;
      return ERR::Okay;
   }

   inline ERR getPingInterval(double &Value) noexcept {
      Value = this->PingInterval;
      return ERR::Okay;
   }

   inline ERR getCloseTimeout(double &Value) noexcept {
      Value = this->CloseTimeout;
      return ERR::Okay;
   }

   inline ERR getClientData(APTR &Value) noexcept {
      Value = this->ClientData;
      return ERR::Okay;
   }

   inline ERR getLocation(std::string_view &Value) noexcept {
      Value = this->Location;
      return ERR::Okay;
   }

   inline ERR getProtocols(std::string_view &Value) noexcept {
      Value = this->Protocols;
      return ERR::Okay;
   }

   inline ERR getProtocol(std::string_view &Value) noexcept {
      Value = this->Protocol;
      return ERR::Okay;
   }

   inline ERR getOrigin(std::string_view &Value) noexcept {
      Value = this->Origin;
      return ERR::Okay;
   }

   inline ERR getProxyServer(std::string_view &Value) noexcept {
      Value = this->ProxyServer;
      return ERR::Okay;
   }

   inline ERR getCloseReason(std::string_view &Value) noexcept {
      Value = this->CloseReason;
      return ERR::Okay;
   }

   inline ERR getRedirect(std::string_view &Value) noexcept {
      Value = this->Redirect;
      return ERR::Okay;
   }

   inline ERR getProxyPort(int &Value) noexcept {
      Value = this->ProxyPort;
      return ERR::Okay;
   }

   inline ERR getFlags(WSF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getState(WSS &Value) noexcept {
      Value = this->State;
      return ERR::Okay;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getCloseCode(int &Value) noexcept {
      Value = this->CloseCode;
      return ERR::Okay;
   }

   inline ERR getStatus(int &Value) noexcept {
      Value = this->Status;
      return ERR::Okay;
   }

   inline ERR getHeartbeat(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getIncoming(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOutgoing(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getPending(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getStateChanged(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setMaxMessageSize(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setMaxFrameSize(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[30];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setSendLimit(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setConnectTimeout(const double Value) noexcept {
      this->ConnectTimeout = Value;
      return ERR::Okay;
   }

   inline ERR setDataTimeout(const double Value) noexcept {
      this->DataTimeout = Value;
      return ERR::Okay;
   }

   inline ERR setPingInterval(const double Value) noexcept {
      auto field = &this->Class->Dictionary[27];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setCloseTimeout(const double Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(this, field, FD_DOUBLE, &Value);
   }

   inline ERR setClientData(APTR Value) noexcept {
      this->ClientData = Value;
      return ERR::Okay;
   }

   inline ERR setLocation(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[24];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setProtocols(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setOrigin(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setProxyServer(const std::string_view &Value) noexcept {
      this->ProxyServer = Value;
      return ERR::Okay;
   }

   inline ERR setProxyPort(const int Value) noexcept {
      this->ProxyPort = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const WSF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setHeartbeat(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setIncoming(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOutgoing(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setStateChanged(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

};