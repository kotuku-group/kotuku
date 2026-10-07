/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
WebSocketServer: Accepts RFC 6455 WebSocket connections.

The WebSocketServer class listens for WebSocket clients on a TCP port and represents each accepted connection as a
@WebSocket object.  Set the #Port (zero selects an ephemeral port, which can be read back after initialisation),
optionally the #Path and #Protocols to serve, and a #Connected callback, then initialise the object to start listening.
Set the `SSL` flag to accept `wss://` connections; the #SSLCertificate and #SSLPrivateKey fields select the
certificate, otherwise a localhost development certificate is used.

Each client's opening handshake is validated before it is accepted.  Malformed requests are answered with
`400 Bad Request`, a `Sec-WebSocket-Version` other than 13 with `426 Upgrade Required`, a request for a path other
than #Path with `404 Not Found`, and a request that exceeds 16 KiB with `431 Request Header Fields Too Large`.  A
client that does not complete its request within #HandshakeTimeout seconds is disconnected.  The #Accept callback can
inspect a valid request and reject it with `403 Forbidden`, for example to enforce an `Origin` policy.  The first
subprotocol from #Protocols that the client requested is selected and reported in the connection's
@WebSocket.Protocol field.

Accepted connections are reported to #Connected, which is the place to install the connection's
@WebSocket.Incoming and @WebSocket.StateChanged callbacks.  Messages are exchanged with @WebSocket.Send() as for a
client connection, and #Broadcast() sends a message to every open connection.  When a connection reaches the `CLOSED`
state it is reported to #Disconnected and then freed automatically, so references to it must not be retained beyond
that callback.  #DisconnectClient() starts the closing handshake for a single connection.

The #MaxMessageSize, #MaxFrameSize, #SendLimit, #PingInterval and #CloseTimeout values, and the `STREAM_MESSAGES`,
`NO_UTF8_CHECK` and `NO_AUTO_PONG` flags, are applied to each connection when it is accepted.  They can also be changed
on an individual connection.

Freeing the server closes all of its connections abortively.  Each connection reports `CLOSED` to its own
@WebSocket.StateChanged callback, but #Disconnected is not called.

The following Tiri example runs an echo server at `ws://localhost:8080/echo` until the process is terminated:

<pre>
include 'websocket'

server = obj.new('websocketserver', {
   port = 8080,
   path = '/echo',
   accept = function(Server:obj, Request:table)
      if Request.origin != 'https://example.com' then return ERR_Failed end
      return ERR_Okay
   end,
   connected = function(Server:obj, WebSocket:obj)
      WebSocket.incoming = function(WS:obj, Buffer:array&lt;byte&gt;, Type:num, Final:num)
         WS.mtSend(Buffer.getString(), Type)
      end
   end,
   disconnected = function(Server:obj, WebSocket:obj)
      print(f'Connection closed with code {WebSocket.closeCode}')
   end
})

processing.sleep()
</pre>

Initialisation fails if the #Port cannot be bound, for example because it is already in use.

The WebSocketServer class is not thread-safe.  Create, use and free each object on the thread that processes its
messages.  In `EXTERNAL_LISTENER` mode no listener is created: #Port remains zero and Address and certificate fields
are ignored.  TLS belongs to the external listener, so combining `SSL` and `EXTERNAL_LISTENER` is invalid.  Use
#Adopt() and #Dispatch() to serve WebSockets alongside HTTP on an existing listener.  Freeing this server closes its
adopted sockets without freeing the external listener.

-END-

*********************************************************************************************************************/

constexpr size_t MAX_REQUEST_SIZE  = 16 * 1024; // Largest opening handshake request accepted
constexpr size_t REQUEST_READ_SIZE = 4096;

// A connection that has not completed its opening handshake, or that was rejected and is waiting to be closed.

struct PendingConnection {
   objClientSocket *Socket;
   std::string Buffer;    // Request bytes received so far
   int64_t Since = 0;     // PreciseTime() when the connection was accepted or rejected
   bool Rejected = false; // A rejection response was written; input is discarded until the socket closes
};

class extWebSocketServer : public objWebSocketServer {
   public:
   FUNCTION Accept;
   FUNCTION Connected;
   FUNCTION Disconnected;
   objNetServer *NetServer = nullptr;
   std::vector<std::string> SupportedProtocols;
   std::unordered_map<OBJECTID, PendingConnection> Pending; // Keyed by ClientSocket UID
   std::unordered_map<OBJECTID, extWebSocket *> Upgraded;   // Keyed by ClientSocket UID
   std::unordered_map<OBJECTID, objClientSocket *> Managed; // External sockets, retained until Disconnected
   APTR HandshakeTimer = nullptr;
   bool Terminating = false;

   extWebSocketServer(objMetaClass *ClassPtr, OBJECTID ObjectID) : objWebSocketServer(ClassPtr, ObjectID) {
      MaxMessageSize   = 16 * 1024 * 1024;
      MaxFrameSize     = 0;
      SendLimit        = 16 * 1024 * 1024;
      PingInterval     = 0;
      CloseTimeout     = 5.0;
      HandshakeTimeout = 10.0;
   }

   ~extWebSocketServer();
};

// External listeners share the message-processing thread; a socket may belong to only one protocol server.
static thread_local std::unordered_map<OBJECTID, OBJECTID> glProtocolSockets;

static ERR handshake_timer(extWebSocketServer *, int64_t, int64_t);

//********************************************************************************************************************

static double handshake_timer_interval(double Timeout)
{
   return std::clamp(Timeout / 4.0, 0.02, 1.0);
}

//********************************************************************************************************************

static ERR start_handshake_timer(extWebSocketServer *Self)
{
   if ((Self->HandshakeTimeout <= 0) or Self->Pending.empty() or Self->HandshakeTimer) return ERR::Okay;

   auto error = SubscribeTimer(handshake_timer_interval(Self->HandshakeTimeout), C_FUNCTION(handshake_timer),
      &Self->HandshakeTimer);
   if (error != ERR::Okay) Self->HandshakeTimer = nullptr;
   return error;
}

//********************************************************************************************************************

static void update_total(extWebSocketServer *Self)
{
   Self->TotalConnections = int(Self->Upgraded.size());
}

//********************************************************************************************************************
// Invokes the Connected or Disconnected callback.  Returns false if the WebSocket was freed by the callback.

static bool call_connection_callback(extWebSocketServer *Self, FUNCTION &Callback, extWebSocket *WebSocket)
{
   if (Callback.stale()) clear_callback(Callback);
   if (not Callback.defined()) return not WebSocket->collecting();

   // The server is not locked when a connection is closed by its own timers or actions, so it is pinned in case the
   // callback frees it.  The caller is responsible for the WebSocket.

   auto callback = Callback; // The callback may replace the field while running
   callback.pin();
   Self->pin();
   auto lifetime = kt::Defer([&]() { callback.unpin(); release_pin(Self); });

   if (callback.isC()) {
      auto routine = (void (*)(extWebSocketServer *, extWebSocket *, APTR))callback.Routine;
      kt::SwitchContext context(callback.Context);
      routine(Self, WebSocket, callback.Meta);
   }
   else if (callback.isScript()) {
      sc::Call(callback, std::to_array<ScriptArg>({
         { "WebSocketServer", Self, FD_OBJECTPTR },
         { "WebSocket", WebSocket, FD_OBJECTPTR }
      }));
   }

   return not WebSocket->collecting();
}

//********************************************************************************************************************
// Invokes the Accept callback.  Only an explicit ERR::Okay accepts the request; a Tiri callback that returns nothing
// rejects it.

static ERR call_accept(extWebSocketServer *Self, WSRequest &Request)
{
   if (Self->Accept.stale()) clear_callback(Self->Accept);
   if (not Self->Accept.defined()) return ERR::Okay;

   auto callback = Self->Accept;
   callback.pin();
   auto lifetime = kt::Defer([&]() { callback.unpin(); });

   if (callback.isC()) {
      auto routine = (ERR (*)(extWebSocketServer *, WSRequest *, APTR))callback.Routine;
      kt::SwitchContext context(callback.Context);
      return routine(Self, &Request, callback.Meta);
   }
   else if (callback.isScript()) {
      ERR result = ERR::Okay;
      ERR returned = ERR::Terminate; // Replaced only if the callback returns a value
      if (sc::Call(callback, std::to_array<ScriptArg>({
            { "WebSocketServer", Self, FD_OBJECTPTR },
            { "WSRequest:Request", &Request, FD_PTR|FD_STRUCT },
            { "Result", &returned, FD_PTR|FD_RESULT|FD_ERROR }
         }), result) != ERR::Okay) return ERR::Function;
      if (result != ERR::Okay) return result;
      return returned;
   }
   return ERR::Function;
}

//********************************************************************************************************************
// Called from complete_handover() once the connection is OPEN, before the bytes that followed the request are
// processed.  Returns false if the WebSocket was freed.

static bool server_announce(extWebSocket *WebSocket)
{
   auto Self = WebSocket->Server;
   if ((not Self) or Self->Terminating) return not WebSocket->collecting();
   auto alive = call_connection_callback(Self, Self->Connected, WebSocket);
   return alive and (not Self->collecting());
}

//********************************************************************************************************************
// Called by close_connection() once a connection has reported CLOSED.  The WebSocket is reported to Disconnected and
// freed by a deferred message.

static void server_connection_closed(extWebSocket *WebSocket)
{
   auto Self = WebSocket->Server;
   WebSocket->Server = nullptr;

   if (auto it = Self->Upgraded.find(WebSocket->ServerSocketID); (it != Self->Upgraded.end()) and
       (it->second IS WebSocket)) {
      Self->Upgraded.erase(it);
      update_total(Self);
   }

   if (Self->Terminating) return;

   if (call_connection_callback(Self, Self->Disconnected, WebSocket)) free_deferred(WebSocket->UID);
}

//********************************************************************************************************************
// Called by the WebSocket destructor when a connection is freed before it reached CLOSED, or was freed by one of its
// own callbacks while closing.

static void server_websocket_freed(extWebSocket *WebSocket)
{
   auto Self = WebSocket->Server;
   WebSocket->Server = nullptr;

   if (auto it = Self->Upgraded.find(WebSocket->ServerSocketID); (it != Self->Upgraded.end()) and
       (it->second IS WebSocket)) {
      Self->Upgraded.erase(it);
      update_total(Self);
   }
}

//********************************************************************************************************************
// Reads and discards input from a socket that is not, or is no longer, carrying a handshake or a connection.  The
// socket must be read so that Network does not terminate it, which would discard bytes still queued for the client.

static void discard_input(objClientSocket *Socket)
{
   std::array<int8_t, REQUEST_READ_SIZE> buffer;
   for (int i = 0; i < 64; i++) {
      int length = 0;
      if ((Socket->read(buffer, &length) != ERR::Okay) or (length <= 0)) break;
   }
}

//********************************************************************************************************************
// Answers a handshake request with an error status and closes the connection once the response has been written.

static ERR reject_request(extWebSocketServer *Self, objClientSocket *Socket, int Status, std::string_view Reason)
{
   kt::Log log(__FUNCTION__);
   log.msg("Handshake rejected with %d: %.*s", Status, int(Reason.size()), Reason.data());

   // Closing a socket with unread input sends a reset, which can destroy the response before the client reads it.

   discard_input(Socket);

   auto response = ws::rejection_response(Status);
   int accepted = 0;
   auto error = ws::write_object(Socket, std::span((const uint8_t *)response.data(), response.size()), accepted);
   if (((error != ERR::Okay) and (error != ERR::BufferOverflow)) or (size_t(accepted) != response.size())) {
      return (error != ERR::Okay) ? error : ERR::BufferOverflow;
   }

   if (auto it = Self->Pending.find(Socket->UID); it != Self->Pending.end()) {
      it->second.Rejected = true;
      it->second.Since = PreciseTime();
      std::string().swap(it->second.Buffer);
   }

   // Network frees a terminated ClientSocket immediately, so a response still in its queue is drained first.

   int queued = 0;
   if ((Socket->getOutQueueSize(queued) IS ERR::Okay) and (queued > 0)) {
      Socket->deactivate();
      return ERR::Okay;
   }
   return ERR::Terminate;
}

//********************************************************************************************************************
// Creates the server-role WebSocket for an accepted request.  The connection is announced and the bytes that followed
// the request are processed by complete_handover(), which runs from transport_incoming().

static extWebSocket * create_connection(extWebSocketServer *Self, objClientSocket *Socket,
   const ws::Request &Request, std::string_view Protocol, std::string_view Prefix, ERR &Error)
{
   kt::Log log(__FUNCTION__);

   extWebSocket *websocket;
   Error = NewObject(CLASSID::WEBSOCKET, &websocket); // The connection is owned by the server (the current context)
   if (Error != ERR::Okay) return nullptr;

   constexpr auto inherited = WSF::STREAM_MESSAGES | WSF::NO_UTF8_CHECK | WSF::NO_AUTO_PONG;

   websocket->ExternalListener = (Self->Flags & WSF::EXTERNAL_LISTENER) != WSF::NIL;
   websocket->Role           = ws::Role::SERVER;
   websocket->Server         = Self;
   websocket->ServerSocketID = Socket->UID;
   websocket->MaxMessageSize = Self->MaxMessageSize;
   websocket->MaxFrameSize   = Self->MaxFrameSize;
   websocket->SendLimit      = Self->SendLimit;
   websocket->PingInterval   = Self->PingInterval;
   websocket->CloseTimeout   = Self->CloseTimeout;
   websocket->Flags          = Self->Flags & inherited;
   websocket->Status         = int(HTS::SWITCH_PROTOCOLS);
   websocket->Protocols      = Self->Protocols;
   websocket->Protocol.assign(Protocol);
   websocket->Origin.assign(Request.origin);
   websocket->Location = ((Self->Flags & WSF::SSL) != WSF::NIL) ? "wss://" : "ws://";
   websocket->Location.append(Request.host);
   websocket->Location.append(Request.target);

   Error = InitObject(websocket);
   if (Error != ERR::Okay) {
      websocket->Server = nullptr;
      FreeResource(websocket);
      return nullptr;
   }

   auto frame_limit = websocket->MaxFrameSize ? websocket->MaxFrameSize : websocket->MaxMessageSize;
   websocket->Transport = std::make_unique<ws::ClientSocketTransport>(Socket);
   websocket->Reader = std::make_unique<ws::MessageReader>(ws::Role::SERVER, uint64_t(frame_limit),
      uint64_t(websocket->MaxMessageSize), (websocket->Flags & WSF::NO_UTF8_CHECK) IS WSF::NIL);
   websocket->Queue = std::make_unique<ws::SendQueue>(ws::Role::SERVER, nullptr, uint64_t(frame_limit),
      uint64_t(websocket->SendLimit));

   websocket->Prefix.resize(Prefix.size());
   if (not Prefix.empty()) kt::copymem(Prefix.data(), websocket->Prefix.data(), Prefix.size());

   websocket->State = WSS::CONNECTING; // Not reported; the application has no callbacks until Connected
   websocket->HandoverPending = true;
   return websocket;
}

//********************************************************************************************************************
// Shared opening validation, response and handover for dedicated and external listeners.

static ERR complete_request(extWebSocketServer *Self, objClientSocket *Socket, std::string_view Data,
   objWebSocket **Connection)
{
   kt::Log log(__FUNCTION__);
   const auto socket_id = Socket->UID;
   const auto end = ws::find_request_end(Data);
   if ((not end) or (end > MAX_REQUEST_SIZE)) {
      return reject_request(Self, Socket, end ? ws::STATUS_HEADERS_TOO_BIG : ws::STATUS_BAD_REQUEST,
         "Opening request is incomplete or too large");
   }
   const auto head = std::string_view(Data).substr(0, end);
   const auto prefix = std::string_view(Data).substr(end);

   ws::Request request;
   std::string_view reason;
   if (auto status = ws::parse_request(head, request, reason)) return reject_request(Self, Socket, status, reason);

   if ((not Self->Path.empty()) and (request.path != Self->Path)) {
      return reject_request(Self, Socket, ws::STATUS_NOT_FOUND, "Path is not served");
   }

   auto protocol = ws::select_protocol(Self->SupportedProtocols, request.protocols);

   if (Self->Accept.defined()) {
      WSRequest info;
      info.Path.assign(request.path);
      info.Query.assign(request.query);
      info.Host.assign(request.host);
      info.Origin.assign(request.origin);
      info.Protocol.assign(protocol);

      for (auto requested : request.protocols) {
         if (not info.Protocols.empty()) info.Protocols.append(", ");
         info.Protocols.append(requested);
      }

      for (const auto &field : request.fields) {
         info.Headers.append(field.name);
         info.Headers.append(": ");
         info.Headers.append(field.value);
         info.Headers.append("\n");
      }

      IPAddress *address = nullptr;
      if (Socket->Client and (Socket->Client->get(kt::fieldhash("IP"), address) IS ERR::Okay) and address) {
         net::AddressToStr(address, &info.Address);

         // A dual-stack listener reports IPv4 clients as IPv4-mapped IPv6 addresses
         if (info.Address.starts_with("::ffff:") and (info.Address.find('.') != std::string::npos)) {
            info.Address.erase(0, 7);
         }
      }

      auto error = call_accept(Self, info);

      // The callback may have freed the server or disconnected the client.

      if (Self->collecting()) return ERR::Terminate;

      if ((not Self->Pending.contains(socket_id)) or (Socket->State != NTC::CONNECTED)) return ERR::Okay;

      if (error != ERR::Okay) {
         return reject_request(Self, Socket, ws::STATUS_FORBIDDEN, "Request refused by the Accept callback");
      }
   }

   std::string accept_key;
   if (auto error = ws::compute_accept_key(request.key, accept_key); error != ERR::Okay) {
      log.warning("Accept key computation failed: %s", GetErrorMsg(error));
      Self->Pending.erase(socket_id);
      return error;
   }

   auto response = ws::switching_response(accept_key, protocol);
   int accepted = 0;
   auto error = ws::write_object(Socket, std::span((const uint8_t *)response.data(), response.size()), accepted);
   if (((error != ERR::Okay) and (error != ERR::BufferOverflow)) or (size_t(accepted) != response.size())) {
      log.warning("Handshake response could not be written: %s", GetErrorMsg(error));
      Self->Pending.erase(socket_id);
      return (error != ERR::Okay) ? error : ERR::BufferOverflow;
   }

   Self->Pending.erase(socket_id);

   auto websocket = create_connection(Self, Socket, request, protocol, prefix, error);
   if (not websocket) return error;

   Self->Upgraded[socket_id] = websocket;
   update_total(Self);

   log.msg("Accepted connection #%d for %.*s", websocket->UID, int(request.target.size()), request.target.data());

   // transport_incoming() pins the connection, and a connection freed by a callback is destroyed by a deferred
   // message, so the pointer remains valid afterwards.

   error = transport_incoming(websocket);
   if (Connection) {
      if (Self->collecting() or websocket->collecting() or (websocket->State IS WSS::CLOSED)) *Connection = nullptr;
      else *Connection = websocket;
   }
   return error;
}

//********************************************************************************************************************
// Buffers and validates the opening handshake request of a pending connection.

static ERR handshake_incoming(extWebSocketServer *Self, objClientSocket *Socket)
{
   kt::Log log(__FUNCTION__);

   const auto socket_id = Socket->UID;
   std::string data;

   {
      auto &pending = Self->Pending[socket_id];
      std::array<char, REQUEST_READ_SIZE> buffer;
      while (pending.Buffer.size() <= MAX_REQUEST_SIZE) {
         int length = 0;
         if (Socket->read(std::span<int8_t>((int8_t *)buffer.data(), buffer.size()), &length) != ERR::Okay) {
            return ERR::Okay; // The connection is lost and its pending record may already have been removed
         }

         if (length <= 0) break;
         pending.Buffer.append(buffer.data(), size_t(length));
         if (ws::find_request_end(pending.Buffer)) break;
      }

      auto end = ws::find_request_end(pending.Buffer);
      if (not end) {
         if (pending.Buffer.size() > MAX_REQUEST_SIZE) {
            return reject_request(Self, Socket, ws::STATUS_HEADERS_TOO_BIG, "Request is too large");
         }

         return ERR::Okay; // Waiting for the rest of the request
      }

      if (end > MAX_REQUEST_SIZE) {
         return reject_request(Self, Socket, ws::STATUS_HEADERS_TOO_BIG, "Request is too large");
      }

      data = std::move(pending.Buffer);
      pending.Buffer.clear();
   }

   auto error = complete_request(Self, Socket, data, nullptr);
   if ((error != ERR::Okay) and (error != ERR::Terminate)) return ERR::Terminate;
   return error;
}

//********************************************************************************************************************
// NetServer callbacks.  The server is identified by the callback context, which Network locks for the duration of
// each callback.  A server freed by a user callback is therefore destroyed when the callback returns.

static ERR server_incoming(objNetServer *NetServer, objClientSocket *Socket, APTR Meta)
{
   auto Self = (extWebSocketServer *)CurrentContext();
   if (Self->classID() != CLASSID::WEBSOCKETSERVER) return ERR::Terminate;

   if (not Self->Terminating) {
      if (auto it = Self->Upgraded.find(Socket->UID); it != Self->Upgraded.end()) {
         if (owns_socket(it->second, Socket)) return transport_incoming(it->second);
      }
      else if (auto pending = Self->Pending.find(Socket->UID); pending != Self->Pending.end()) {
         if (not pending->second.Rejected) return handshake_incoming(Self, Socket);
      }
   }

   discard_input(Socket);
   return ERR::Okay;
}

//********************************************************************************************************************

static void server_feedback(objNetServer *NetServer, objClientSocket *Socket, NTC State, APTR Meta)
{
   auto Self = (extWebSocketServer *)CurrentContext();
   if ((Self->classID() != CLASSID::WEBSOCKETSERVER) or Self->Terminating) return;

   if (State IS NTC::CONNECTED) {
      glProtocolSockets[Socket->UID] = Self->UID;
      Self->Pending[Socket->UID] = PendingConnection { Socket, {}, PreciseTime(), false };
      start_handshake_timer(Self);
   }
   else if (State IS NTC::DISCONNECTED) {
      glProtocolSockets.erase(Socket->UID);
      Self->Pending.erase(Socket->UID);

      if (auto it = Self->Upgraded.find(Socket->UID); it != Self->Upgraded.end()) {
         if (owns_socket(it->second, Socket)) transport_disconnected(it->second);
      }

      // The ClientSocket belongs to the NetServer.  A disconnected socket is freed here unless it is already being
      // destroyed, so that its NetClient record is released.

      if (not Socket->collecting()) free_deferred(Socket->UID);
   }
}

//********************************************************************************************************************

static ERR server_outgoing(objNetServer *NetServer, objClientSocket *Socket, APTR Meta)
{
   auto Self = (extWebSocketServer *)CurrentContext();
   if ((Self->classID() != CLASSID::WEBSOCKETSERVER) or Self->Terminating) return ERR::Okay;

   if (auto it = Self->Upgraded.find(Socket->UID); it != Self->Upgraded.end()) {
      if (owns_socket(it->second, Socket)) transport_writable(it->second);
   }
   return ERR::Okay; // Network counts any other result as an error
}

//********************************************************************************************************************
// Disconnects clients that have not completed their handshake within HandshakeTimeout, and rejected clients that have
// not read their response within the same period.  The timer stops when no handshakes are pending.

static ERR handshake_timer(extWebSocketServer *Self, int64_t Elapsed, int64_t CurrentTime)
{
   if (Self->Pending.empty() or (Self->HandshakeTimeout <= 0)) {
      Self->HandshakeTimer = nullptr;
      return ERR::Terminate;
   }

   const auto limit = int64_t(Self->HandshakeTimeout * 1000000.0);
   std::vector<OBJECTID> expired;
   for (auto &[id, pending] : Self->Pending) {
      if (CurrentTime - pending.Since >= limit) expired.push_back(id);
   }

   for (auto id : expired) {
      if (auto it = Self->Pending.find(id); it != Self->Pending.end()) {
         kt::Log(__FUNCTION__).msg("Client socket #%d did not complete its handshake in time.", id);
         auto socket = it->second.Socket;
         Self->Pending.erase(it);
         if (Self->Managed.contains(id)) socket->setState(NTC::DISCONNECTED);
         FreeResource(socket);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Adopt: Handles a complete opening request on an external listener's socket.

Adopt allows an external @NetServer to hand over a connection that is requesting an upgrade to WebSocket.  The server
must be initialised with the `EXTERNAL_LISTENER` flag.  Typically the listener's @NetServer.Incoming callback reads the
socket until the end of the HTTP request header (an empty line) has been received, recognises an upgrade request and
then passes everything read so far to Adopt.  Adopt does not read from the socket, so `RequestData` must include the
complete request.  Any bytes that follow the request are treated as the first WebSocket frames from the client and are
processed after the connection is announced.

The request is validated and answered exactly as for a connection on the server's own listener, including the #Path
filter, protocol selection and the #Accept callback.  If the request is accepted, the `101 Switching Protocols`
response is sent and the new @WebSocket is reported to #Connected before Adopt returns.  The connection is also
returned in `Connection`, unless it was closed or freed by a callback in the meantime.

If the request is rejected, the error response is queued and Adopt returns `ERR::Okay` with a `NULL` `Connection`.
The socket remains under the server's management until the response has been written or #HandshakeTimeout expires.

Once Adopt returns `ERR::Okay` the socket is managed by the server, whether or not the request was accepted.  The
listener must forward all subsequent events for the socket with #Dispatch(), and must not terminate or free the socket
itself.  The server releases the socket when the connection closes, and freeing the server disconnects every socket
that it is managing.  If Adopt returns an error, the socket is not managed and remains the listener's responsibility.

-INPUT-
obj(ClientSocket) Socket: A connected socket accepted by the external listener.
array(char) RequestData: Complete HTTP opening request, optionally followed by WebSocket frames.
&obj(WebSocket) Connection: Accepted connection, or null for a rejection or a connection freed by a callback.

-ERRORS-
Okay
NullArgs
WrongClass
InvalidState: The server is not initialised in external-listener mode, or the socket is disconnected.
InUse: The socket is already managed by a protocol server.
BufferOverflow: The opening response could not be queued completely.
CreateObject
-END-

*********************************************************************************************************************/

static ERR WEBSOCKETSERVER_Adopt(extWebSocketServer *Self, struct wsv::Adopt *Args)
{
   if (not Args) return ERR::NullArgs;

   Args->Connection = nullptr;

   if (not Args->Socket) return ERR::NullArgs;

   if (Args->Socket->classID() != CLASSID::CLIENTSOCKET) return ERR::WrongClass;

   if ((not Self->initialised()) or Self->Terminating or
       ((Self->Flags & WSF::EXTERNAL_LISTENER) IS WSF::NIL)) return ERR::InvalidState;

   auto socket = (objClientSocket *)Args->Socket;

   if (glProtocolSockets.contains(socket->UID)) return ERR::InUse;

   if ((socket->State != NTC::CONNECTED) or socket->collecting()) return ERR::InvalidState;

   // Self is locked by the method call.  Network does not lock the socket during its callbacks.

   socket->pin();
   auto lifetime = kt::Defer([&]() { release_pin(socket); });

   const auto id = socket->UID;
   Self->Managed[id] = socket;
   glProtocolSockets[id] = Self->UID;

   Self->Pending[id] = PendingConnection { socket, {}, PreciseTime(), false };

   auto error = start_handshake_timer(Self);

   if (error IS ERR::Okay) error = complete_request(Self, socket,
      std::string_view((const char *)Args->RequestData.data(), Args->RequestData.size()), &Args->Connection);

   if (error IS ERR::Terminate) {
      // Adopt has no Network callback return channel.  Release after its caller leaves Incoming.
      socket->deactivate();
      free_deferred(id);
      return ERR::Okay;
   }

   if (error != ERR::Okay) {
      Self->Pending.erase(id);
      Self->Managed.erase(id);
      glProtocolSockets.erase(id);
   }

   return error;
}

/*********************************************************************************************************************

-METHOD-
Broadcast: Sends a message to every open connection.

Broadcast queues a copy of the message on every connection whose @WebSocket.State is `OPEN`, as if @WebSocket.Send()
had been called for each of them.  Connections that are not open, and connections whose @WebSocket.SendLimit would be
exceeded by the message, are skipped.  The number of connections that accepted the message is returned in
`Recipients`.

A `TEXT` message must be valid UTF-8 unless the `NO_UTF8_CHECK` flag is set on the server.

-INPUT-
array(char) Data: The message payload.
int(WSM) Type: Either `TEXT` or `BINARY`.
&int Recipients: The number of connections that accepted the message.

-ERRORS-
Okay: The message was queued on every connection that could accept it, which may be none.
NullArgs
InvalidValue: `Type` is not a valid message type.
InvalidData: A `TEXT` message is not valid UTF-8.
-END-

*********************************************************************************************************************/

static ERR WEBSOCKETSERVER_Broadcast(extWebSocketServer *Self, struct wsv::Broadcast *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   Args->Recipients = 0;
   if ((Args->Type != WSM::TEXT) and (Args->Type != WSM::BINARY)) return log.warning(ERR::InvalidValue);

   auto data = std::span<const uint8_t>((const uint8_t *)Args->Data.data(), Args->Data.size());
   if ((Args->Type IS WSM::TEXT) and ((Self->Flags & WSF::NO_UTF8_CHECK) IS WSF::NIL) and (not ws::valid_utf8(data))) {
      return log.warning(ERR::InvalidData);
   }

   auto opcode = (Args->Type IS WSM::TEXT) ? ws::Opcode::TEXT : ws::Opcode::BINARY;

   // Writing can close a connection and modify the connection list, so a pinned snapshot is used.

   std::vector<extWebSocket *> connections;
   connections.reserve(Self->Upgraded.size());
   for (auto &[id, websocket] : Self->Upgraded) {
      websocket->pin();
      connections.push_back(websocket);
   }

   for (auto websocket : connections) {
      if ((not websocket->collecting()) and (websocket->State IS WSS::OPEN) and websocket->Queue) {
         if (websocket->Queue->push_message(opcode, data) IS ws::QueueResult::OKAY) {
            Args->Recipients++;
            flush(websocket);
         }
         else if (not websocket->Queue->idle()) websocket->AwaitingDrain = true; // Report through Outgoing
      }
      release_pin(websocket);
   }

   log.trace("Message queued for %d of %d connections.", Args->Recipients, int(connections.size()));
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DisconnectClient: Starts the closing handshake for one connection.

DisconnectClient closes a connection that was accepted by this server, as for @WebSocket.Close().  Messages already
queued on the connection are sent first.  The connection is reported to #Disconnected once it reaches `CLOSED`.

-INPUT-
obj(WebSocket) WebSocket: A connection accepted by this server.
int Code: The close status code, or zero to send no status.
cstr Reason: Optional.  A short UTF-8 description of the reason for closing.

-ERRORS-
Okay: The closing handshake was started, or the connection is already closing.
NullArgs
WrongClass: `WebSocket` is not a @WebSocket.
NotFound: `WebSocket` is not an open connection of this server.
OutOfRange: `Code` is not a permitted close status.
InvalidValue: `Reason` exceeds 123 bytes, is not valid UTF-8, or was supplied without a `Code`.
-END-

*********************************************************************************************************************/

static ERR WEBSOCKETSERVER_DisconnectClient(extWebSocketServer *Self, struct wsv::DisconnectClient *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->WebSocket)) return log.warning(ERR::NullArgs);
   if (Args->WebSocket->classID() != CLASSID::WEBSOCKET) return log.warning(ERR::WrongClass);

   auto websocket = (extWebSocket *)Args->WebSocket;
   if (websocket->Server != Self) return log.warning(ERR::NotFound);

   wsk::Close close = { Args->Code, Args->Reason };
   return Action(wsk::Close::id, websocket, &close);
}

/*********************************************************************************************************************

-METHOD-
Dispatch: Forwards an external listener event to a managed socket.

In `EXTERNAL_LISTENER` mode, Dispatch is the means by which the server receives events for the sockets that it
manages.  The listener's @NetServer.Incoming, @NetServer.Outgoing and @NetServer.Feedback callbacks must call Dispatch
with `INCOMING`, `OUTGOING` or `DISCONNECTED` respectively for every socket that was handed to #Adopt():

!WSE

Dispatch returns `ERR::NotFound` without touching the socket if the socket is not managed by this server.  A
listener that also serves plain HTTP can therefore call Dispatch first and handle the socket itself if `ERR::NotFound`
is returned.

The result of an `INCOMING` event must be returned from the listener's Incoming callback.  An `ERR::Terminate` result
indicates that the connection has ended and Network is required to terminate the socket.

A `DISCONNECTED` event closes the associated @WebSocket if it is still open, which reports the `CLOSED` state and is
passed to #Disconnected.  The socket is then released and is no longer managed by the server.

-INPUT-
obj(ClientSocket) Socket: The external listener socket.
int(WSE) Event: Incoming, Outgoing or Disconnected notification.

-ERRORS-
Okay
NullArgs
WrongClass
InvalidState: The server is not initialised in external-listener mode.
InvalidValue: Event is not a WSE event.
NotFound: The socket is unmanaged; no socket operation was performed.
Terminate: Incoming requires Network to terminate the socket.
-END-

*********************************************************************************************************************/

static ERR WEBSOCKETSERVER_Dispatch(extWebSocketServer *Self, struct wsv::Dispatch *Args)
{
   if ((not Args) or (not Args->Socket)) return ERR::NullArgs;

   if (Args->Socket->classID() != CLASSID::CLIENTSOCKET) return ERR::WrongClass;

   if ((not Self->initialised()) or Self->Terminating or
       ((Self->Flags & WSF::EXTERNAL_LISTENER) IS WSF::NIL)) return ERR::InvalidState;

   if ((Args->Event != WSE::INCOMING) and (Args->Event != WSE::OUTGOING) and
       (Args->Event != WSE::DISCONNECTED)) return ERR::InvalidValue;

   auto socket = (objClientSocket *)Args->Socket;

   if (not Self->Managed.contains(socket->UID)) return ERR::NotFound;

   if (Args->Event IS WSE::DISCONNECTED) {
      Self->Pending.erase(socket->UID);
      Self->Managed.erase(socket->UID);
      glProtocolSockets.erase(socket->UID);
      if (auto it = Self->Upgraded.find(socket->UID); it != Self->Upgraded.end()) {
         if (owns_socket(it->second, socket)) transport_disconnected(it->second);
      }

      if (not socket->collecting()) free_deferred(socket->UID);
   }
   else if (auto it = Self->Upgraded.find(socket->UID); it != Self->Upgraded.end()) {
      if (owns_socket(it->second, socket)) {
         if (Args->Event IS WSE::INCOMING) return transport_incoming(it->second);
         transport_writable(it->second);
      }
   }
   else if (Args->Event IS WSE::INCOMING) discard_input(socket);
   else if (auto it = Self->Pending.find(socket->UID); it != Self->Pending.end()) {
      if (it->second.Rejected) free_deferred(socket->UID); // Rejection queue drained
   }

   return ERR::Okay;
}

//********************************************************************************************************************

extWebSocketServer::~extWebSocketServer()
{
   Terminating = true;
   cancel_timer(HandshakeTimer);

   if (NetServer) {
      NetServer->setIncoming(FUNCTION{});
      NetServer->setFeedback(FUNCTION{});
      NetServer->setOutgoing(FUNCTION{});
   }

   // Close every connection abortively so that each reports CLOSED to its own StateChanged callback.

   std::vector<extWebSocket *> connections;
   for (auto &[id, websocket] : Upgraded) {
      websocket->pin();
      connections.push_back(websocket);
   }

   for (auto websocket : connections) {
      close_connection(websocket, int(WSC::ABNORMAL), "Server terminated", ERR::Okay);
      websocket->Server = nullptr;
      websocket->unpin();
      FreeResource(websocket);
   }

   Upgraded.clear();
   Pending.clear();
   std::vector<OBJECTID> managed_ids;

   for (auto &[id, socket] : Managed) managed_ids.push_back(id);

   for (auto id : managed_ids) {
      glProtocolSockets.erase(id);
      kt::ScopedObjectLock<objClientSocket> socket(id);
      if (socket.granted()) socket->setState(NTC::DISCONNECTED);
      free_deferred(id);
   }

   Managed.clear();
   std::erase_if(glProtocolSockets, [&](const auto &Entry) { return Entry.second IS UID; });

   if (NetServer) { FreeResource(NetServer); NetServer = nullptr; }

   clear_callback(Accept);
   clear_callback(Connected);
   clear_callback(Disconnected);
}

//********************************************************************************************************************

static ERR WEBSOCKETSERVER_Init(extWebSocketServer *Self)
{
   kt::Log log;

   if ((Self->Flags & WSF::EXTERNAL_LISTENER) != WSF::NIL) {
      if ((Self->Flags & WSF::SSL) != WSF::NIL) return log.warning(ERR::InvalidValue);
      Self->Port = 0;
      return ERR::Okay;
   }

   if ((Self->Port < 0) or (Self->Port > 65535)) return log.warning(ERR::OutOfRange);

   objNetServer *server;
   if (NewObject(CLASSID::NETSERVER, &server) != ERR::Okay) return log.warning(ERR::NewObject);

   auto flags = NSF::MULTI_CONNECT;
   if ((Self->Flags & WSF::SSL) != WSF::NIL) flags |= NSF::SSL;

   auto error = ERR::Okay;
   if (not Self->Address.empty()) error = server->setAddress(Self->Address);

   if (error IS ERR::Okay) error = server->setPort(Self->Port);

   if (error IS ERR::Okay) error = server->setFlags(flags);

   if ((error IS ERR::Okay) and (not Self->SSLCertificate.empty())) {
      error = server->setSSLCertificate(Self->SSLCertificate);
   }

   if ((error IS ERR::Okay) and (not Self->SSLPrivateKey.empty())) {
      OBJECTPTR target;
      if (auto field = FindField(server, kt::fieldhash("SSLPrivateKey"), &target)) {
         error = target->set(field, std::string_view(Self->SSLPrivateKey));
      }
      else error = ERR::UnsupportedField;
   }

   if ((error IS ERR::Okay) and (not Self->SSLKeyPassword.empty())) {
      error = server->setSSLKeyPassword(Self->SSLKeyPassword);
   }

   if (error IS ERR::Okay) error = server->setIncoming(C_FUNCTION(server_incoming));
   if (error IS ERR::Okay) error = server->setFeedback(C_FUNCTION(server_feedback));
   if (error IS ERR::Okay) error = server->setOutgoing(C_FUNCTION(server_outgoing));
   if (error IS ERR::Okay) error = InitObject(server);

   if (error != ERR::Okay) {
      FreeResource(server);
      return log.warning(error);
   }

   Self->NetServer = server;
   Self->Port = server->Port; // Reports the selected port if an ephemeral port was requested

   log.msg("Listening on port %d.", Self->Port);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Accept: Approves or refuses opening handshake requests.

The Accept callback is invoked for each request that satisfies RFC 6455 and the #Path filter, before the
`101 Switching Protocols` response is sent.  The C++ prototype is
`ERR Function(*WebSocketServer, struct WSRequest *Request, APTR Meta)`.  Tiri callbacks receive
`(WebSocketServer, Request)`.  The request is described by a !WSRequest structure, which is valid only for the
duration of the call:

!WSRequest

Tiri callbacks receive the structure as a table with the same fields in lower camel-case, for example
`Request.origin` and `Request.path`.  Fields that are absent from the request are empty strings.

Only an explicit `ERR::Okay` accepts the request.  Any other value, including a Tiri callback that returns nothing,
refuses it with `403 Forbidden`.  If Accept is not defined, every valid request is accepted.

*********************************************************************************************************************/

static ERR SRV_GET_Accept(extWebSocketServer *Self, FUNCTION * &Value)
{
   if (Self->Accept.defined()) {
      Value = &Self->Accept;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SRV_SET_Accept(extWebSocketServer *Self, FUNCTION *Value)
{
   clear_callback(Self->Accept);
   if (Value) {
      Self->Accept = *Value;
      Self->Accept.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Address: The local address to bind.

Set Address to an IPv4 or IPv6 address to accept connections on one interface only, for example `127.0.0.1`.  If
Address is empty, connections are accepted on all interfaces.

-FIELD-
ClientData: Free for user data storage.

-FIELD-
CloseTimeout: The time allowed for the closing handshake, in seconds.

The value is applied to each connection when it is accepted.  Refer to @WebSocket.CloseTimeout for details.  The
default is 5 seconds.  Negative values are invalid.

*********************************************************************************************************************/

static ERR SRV_SET_CloseTimeout(extWebSocketServer *Self, double Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->CloseTimeout = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Connected: Reports connections that have completed the opening handshake.

The C++ prototype is `void Function(*WebSocketServer, *WebSocket, APTR Meta)`.  Tiri callbacks receive
`(WebSocketServer, WebSocket)`.

Connected is invoked after the `101 Switching Protocols` response has been sent, with the connection in the `OPEN`
state.  Install the connection's @WebSocket.Incoming, @WebSocket.StateChanged and other callbacks here; messages that
the client sent immediately after its request are delivered once this callback returns.  The callback may send
messages, close the connection or free it.

*********************************************************************************************************************/

static ERR SRV_GET_Connected(extWebSocketServer *Self, FUNCTION * &Value)
{
   if (Self->Connected.defined()) {
      Value = &Self->Connected;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SRV_SET_Connected(extWebSocketServer *Self, FUNCTION *Value)
{
   clear_callback(Self->Connected);
   if (Value) {
      Self->Connected = *Value;
      Self->Connected.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Disconnected: Reports connections that have closed.

The C++ prototype is `void Function(*WebSocketServer, *WebSocket, APTR Meta)`.  Tiri callbacks receive
`(WebSocketServer, WebSocket)`.

Disconnected is invoked once a connection has reached the `CLOSED` state, after its own @WebSocket.StateChanged
callback.  The connection's @WebSocket.CloseCode, @WebSocket.CloseReason and @WebSocket.Error fields describe how it
ended.  The connection is freed automatically after the callback returns, so references to it must not be retained.

Disconnected is not invoked for a connection that is freed by the application, or for the connections that are closed
when the server is freed.

*********************************************************************************************************************/

static ERR SRV_GET_Disconnected(extWebSocketServer *Self, FUNCTION * &Value)
{
   if (Self->Disconnected.defined()) {
      Value = &Self->Disconnected;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SRV_SET_Disconnected(extWebSocketServer *Self, FUNCTION *Value)
{
   clear_callback(Self->Disconnected);
   if (Value) {
      Self->Disconnected = *Value;
      Self->Disconnected.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags.

Set `SSL` before initialisation to accept `wss://` connections.  The `STREAM_MESSAGES`, `NO_UTF8_CHECK` and
`NO_AUTO_PONG` flags are applied to each connection when it is accepted, and `NO_UTF8_CHECK` also disables validation
of text messages sent with #Broadcast().  `EXTERNAL_LISTENER` creates no listener; use #Adopt() and #Dispatch()
instead.  TLS belongs to the external listener, so `SSL` cannot be combined with `EXTERNAL_LISTENER`.  Listener flags
cannot change after initialisation.

-FIELD-
HandshakeTimeout: The time allowed for a client to send its opening handshake request, in seconds.

A client that has not sent a complete request within this period after connecting is disconnected without a response.
The same period bounds the time that a rejected client is given to read its error response.  The default is 10
seconds.  Zero disables the timeout, which is not recommended for servers exposed to untrusted clients.  Negative
values are invalid.

*********************************************************************************************************************/

static ERR SRV_SET_Flags(extWebSocketServer *Self, WSF Value)
{
   if (Self->initialised()) {
      constexpr auto listener_flags = WSF::EXTERNAL_LISTENER | WSF::SSL;
      if ((Value & listener_flags) != (Self->Flags & listener_flags)) return ERR::InvalidState;
   }
   Self->Flags = Value;
   return ERR::Okay;
}

static ERR SRV_SET_HandshakeTimeout(extWebSocketServer *Self, double Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->HandshakeTimeout = Value;
   if (Value <= 0) {
      cancel_timer(Self->HandshakeTimer);
      return ERR::Okay;
   }
   else if (Self->HandshakeTimer) {
      return UpdateTimer(Self->HandshakeTimer, handshake_timer_interval(Value));
   }
   else return start_handshake_timer(Self);
}

/*********************************************************************************************************************

-FIELD-
MaxFrameSize: The maximum size of a single received frame, in bytes.

The value is applied to each connection when it is accepted.  Refer to @WebSocket.MaxFrameSize for details.  Zero, the
default, applies #MaxMessageSize.  Negative values are invalid.

*********************************************************************************************************************/

static ERR SRV_SET_MaxFrameSize(extWebSocketServer *Self, int64_t Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->MaxFrameSize = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxMessageSize: The maximum size of a received message, in bytes.

The value is applied to each connection when it is accepted.  A client that sends a larger message is disconnected with
close code `MESSAGE_TOO_BIG`.  The default is 16 MiB.  Zero removes the limit, which is not recommended for servers
exposed to untrusted clients.  Negative values are invalid.

*********************************************************************************************************************/

static ERR SRV_SET_MaxMessageSize(extWebSocketServer *Self, int64_t Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->MaxMessageSize = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: Restricts the server to one request path.

If Path is set, requests for any other path are answered with `404 Not Found`.  The comparison is exact and
case-sensitive, and excludes the query string, so a Path of `/chat` accepts `/chat?room=1` but not `/chat/` or
`/Chat`.  If Path is empty, any path is accepted; the #Accept callback can then filter requests by
`WSRequest.Path`.

-FIELD-
PingInterval: Enables keep-alive pings after a period of inactivity, in seconds.

The value is applied to each connection when it is accepted.  Refer to @WebSocket.PingInterval for details.  The
default is zero, which disables keep-alive.  Negative values are invalid.

*********************************************************************************************************************/

static ERR SRV_SET_PingInterval(extWebSocketServer *Self, double Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->PingInterval = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Port: The local port to bind.

The default is zero, which selects an ephemeral port.  After initialisation, Port holds the port in use.
In `EXTERNAL_LISTENER` mode Port remains zero and no address, port or certificate field is used.

-FIELD-
Protocols: Comma-separated subprotocols supported by the server, in order of preference.

Each subprotocol must be an HTTP token, for example `chat, superchat`.  For each request, the first of these that the
client also requested is selected, sent in the `Sec-WebSocket-Protocol` response header and reported in the
connection's @WebSocket.Protocol field.  Names are compared case-sensitively.  If none matches, or Protocols is empty,
the response carries no subprotocol and the connection is still accepted; use #Accept to refuse such requests.

-ERRORS-
Okay
InvalidValue: An element of the list is not a valid token.
-END-

*********************************************************************************************************************/

static ERR SRV_SET_Protocols(extWebSocketServer *Self, const std::string_view &Value)
{
   std::vector<std::string_view> tokens;
   if (not ws::parse_token_list(Value, tokens)) return kt::Log().warning(ERR::InvalidValue);

   Self->SupportedProtocols.assign(tokens.begin(), tokens.end());
   Self->Protocols.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SendLimit: The maximum number of queued bytes awaiting transmission on each connection.

The value is applied to each connection when it is accepted.  Refer to @WebSocket.SendLimit for details.  #Broadcast()
skips connections whose limit would be exceeded.  The default is 16 MiB.  Negative values are invalid.

*********************************************************************************************************************/

static ERR SRV_SET_SendLimit(extWebSocketServer *Self, int64_t Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->SendLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SSLCertificate: The TLS certificate file for `wss://` connections.

Set SSLCertificate to the path of a PEM, CRT or PKCS#12 certificate when the `SSL` flag is set.  If no certificate is
defined, a localhost development certificate is used.  Refer to @NetServer.SSLCertificate for details.

-FIELD-
SSLKeyPassword: The password for an encrypted TLS private key.

Set SSLKeyPassword only if the #SSLPrivateKey, or the key included in the #SSLCertificate, is encrypted.

-FIELD-
SSLPrivateKey: The TLS private key file for `wss://` connections.

Set SSLPrivateKey to the path of a PEM or KEY private key when #SSLCertificate does not include the key.

-FIELD-
TotalConnections: The number of open connections.

TotalConnections counts the connections that have completed the opening handshake and have not yet reached the
`CLOSED` state.  Clients that are still performing the handshake are not included.
-END-

*********************************************************************************************************************/

#include "class_websocket_server_def.c"

static const FieldArray clWebSocketServerFields[] = {
   { "MaxMessageSize",   FDF_INT64|FDF_RW, nullptr, SRV_SET_MaxMessageSize },
   { "MaxFrameSize",     FDF_INT64|FDF_RW, nullptr, SRV_SET_MaxFrameSize },
   { "SendLimit",        FDF_INT64|FDF_RW, nullptr, SRV_SET_SendLimit },
   { "PingInterval",     FDF_DOUBLE|FDF_RW, nullptr, SRV_SET_PingInterval },
   { "CloseTimeout",     FDF_DOUBLE|FDF_RW, nullptr, SRV_SET_CloseTimeout },
   { "HandshakeTimeout", FDF_DOUBLE|FDF_RW, nullptr, SRV_SET_HandshakeTimeout },
   { "ClientData",       FDF_POINTER|FDF_RW },
   { "Address",          FDF_CPPSTRING|FDF_RI },
   { "Path",             FDF_CPPSTRING|FDF_RW },
   { "Protocols",        FDF_CPPSTRING|FDF_RW, nullptr, SRV_SET_Protocols },
   { "SSLCertificate",   FDF_CPPSTRING|FDF_RI },
   { "SSLPrivateKey",    FDF_CPPSTRING|FDF_RI },
   { "SSLKeyPassword",   FDF_CPPSTRING|FDF_RI },
   { "Port",             FDF_INT|FDF_RI },
   { "Flags",            FDF_INTFLAGS|FDF_RW, nullptr, SRV_SET_Flags, &clWebSocketServerFlags },
   { "TotalConnections", FDF_INT|FDF_R },
   // Virtual fields
   { "Accept",           FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, SRV_GET_Accept, SRV_SET_Accept },
   { "Connected",        FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, SRV_GET_Connected, SRV_SET_Connected },
   { "Disconnected",     FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, SRV_GET_Disconnected, SRV_SET_Disconnected },
   END_FIELD
};

//********************************************************************************************************************

static ERR add_websocket_server_class(void)
{
   clWebSocketServer = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::WEBSOCKETSERVER),
      fl::ClassVersion(VER_WEBSOCKETSERVER),
      fl::Name("WebSocketServer"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clWebSocketServerActions),
      fl::Methods(clWebSocketServerMethods),
      fl::Fields(clWebSocketServerFields),
      fl::Size(sizeof(extWebSocketServer)),
      fl::Path(MOD_PATH));

   return clWebSocketServer ? ERR::Okay : ERR::AddClass;
}
