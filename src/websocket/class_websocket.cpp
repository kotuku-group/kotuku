/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
WebSocket: Manages a single RFC 6455 WebSocket connection.

The WebSocket class opens a client connection to a `ws://` or `wss://` URL and exchanges messages with the server.
Set the #Location, optionally the #Protocols to request and an #Incoming callback, then call #Activate() to start the
opening handshake.  The handshake is performed by a privately owned @HTTP object, which transfers its connection to the
WebSocket once the server's `101 Switching Protocols` response has been validated.  The connection's TLS state,
including certificate verification, is preserved through the transfer.

Progress is reported through the #StateChanged callback.  The #State moves from `CONNECTING` to `OPEN` once the
handshake is accepted, to `CLOSING` when the closing handshake starts, and to `CLOSED` when the connection ends or the
handshake fails.  When the handshake fails, the #Error, #Status, #CloseReason and #Redirect fields describe the reason.
Redirects are never followed automatically.

Received messages are passed to #Incoming.  Fragmented messages are reassembled before delivery unless the
`STREAM_MESSAGES` flag is set.  Messages are sent with #Send(), which queues them for transmission.  The queue is
bounded by #SendLimit, its size is reported by #Pending and the #Outgoing callback reports when it has been emptied, so
that a producer can pace large transfers against a slow peer.

Received pings are answered automatically.  Keep-alive pings are sent when #PingInterval is set, and a peer that stops
responding is disconnected.  Call #Close() or #Deactivate() to end the connection with the closing handshake, which is
bounded by #CloseTimeout.  Freeing an open WebSocket drops the connection immediately: queued messages are discarded,
no Close frame is sent and #StateChanged is not called.

The following Tiri example connects to an echo server, sends a message once the connection is open and closes the
connection when the echo arrives:

<pre>
include 'websocket'

ws = obj.new('websocket', {
   location  = 'wss://echo.example.com/chat',
   protocols = 'chat',
   stateChanged = function(WebSocket:obj, State:num)
      if State is WSS_OPEN then
         WebSocket.mtSend('Hello', WSM_TEXT)
      elseif State is WSS_CLOSED then
         print(f'Closed with code {WebSocket.closeCode}')
         processing.signal()
      end
   end,
   incoming = function(WebSocket:obj, Buffer:array&lt;byte&gt;, Type:num, Final:num)
      print('Received: ' .. Buffer.getString())
      WebSocket.mtClose(WSC_NORMAL, 'Done')
   end
})

ws.acActivate()
processing.sleep(10)
</pre>

A @WebSocketServer represents each connection that it accepts as a WebSocket object in the server role.  Such objects
are created by the server and reported to its @WebSocketServer.Connected callback, already in the `OPEN` state.  They
are used in the same way as client connections, except that they cannot be activated, and they are freed by the server
once they have closed.

The WebSocket class is not thread-safe.  Create, use and free each object on the thread that processes its messages.
Callbacks are invoked from the thread's message loop, so a script must process messages, for example with
`processing.sleep()`, for a connection to make progress.

-END-

*********************************************************************************************************************/

constexpr size_t WRITE_CHUNK_SIZE = 64 * 1024; // Largest write offered to the transport in one call
constexpr double SERVER_CLOSE_WAIT = 1.0;      // Seconds a client waits for the server to close TCP after the handshake

class extWebSocketServer;

class extWebSocket : public objWebSocket {
   public:
   FUNCTION Incoming;
   FUNCTION StateChanged;
   FUNCTION Outgoing;
   FUNCTION Heartbeat;
   ws::Location Target;                   // Parsed Location
   std::vector<std::string> RequestedProtocols;
   std::vector<std::pair<std::string, std::string>> Headers; // Custom request headers from SetKey()
   std::string ExpectedAccept;            // Sec-WebSocket-Accept value expected from the server
   std::vector<uint8_t> Message;          // Assembly buffer for the message in progress
   std::vector<uint8_t> Scratch;          // Encoding buffer for writes
   kt::vector<int8_t> Prefix;             // Bytes received with the handshake response, awaiting delivery
   std::unique_ptr<ws::MessageReader> Reader;
   std::unique_ptr<ws::SendQueue> Queue;
   std::unique_ptr<ws::Transport> Transport;
   objHTTP *HTTP = nullptr;               // Handshake in progress
   extWebSocketServer *Server = nullptr;  // Server role: the server that accepted the connection
   OBJECTID ServerSocketID = 0;           // Server role: the ClientSocket's UID, which keys the server's connections
   OBJECTID RetiredSocketID = 0;          // Closed connection, freed on reactivation or destruction
   std::vector<OBJECTID> RetiredHTTP;     // Finished HTTP objects awaiting deferred release
   APTR CloseTimer = nullptr;             // Bounds the closing handshake
   APTR PingTimer = nullptr;              // Keep-alive
   int64_t LastReceived = 0;              // PreciseTime() of the most recent read
   int64_t PingSentAt = 0;                // PreciseTime() of the outstanding keep-alive ping
   ws::Role Role = ws::Role::CLIENT;
   bool HandoverPending = false;          // The connection was transferred but OPEN has not been announced
   bool InSocketCallback = false;         // Running inside the transport's Incoming or Feedback callback
   bool InOutgoing = false;               // Running inside the transport's Outgoing callback
   bool InFeedback = false;               // Running inside a disconnection notification for the transport
   bool TerminateSocket = false;          // Ask Network to close the socket on return from its Incoming callback
   bool Pumping = false;                  // pump() is running
   bool AwaitingDrain = false;            // The queue was not empty when last flushed; report when it empties
   bool CloseReceived = false;            // The peer's Close frame has been received
   bool Failed = false;                   // The connection was failed for a protocol error
   bool AwaitingServerClose = false;      // Client: the handshake completed and the server should close TCP
   bool DisconnectReady = false;          // Disconnect as soon as the transport has drained
   bool PingOutstanding = false;          // A keep-alive ping has not been answered

   extWebSocket(objMetaClass *ClassPtr, OBJECTID ObjectID) : objWebSocket(ClassPtr, ObjectID) {
      MaxMessageSize = 16 * 1024 * 1024;
      MaxFrameSize   = 0;
      SendLimit      = 16 * 1024 * 1024;
      ConnectTimeout = 10.0;
      DataTimeout    = 5.0;
      PingInterval   = 0;
      CloseTimeout   = 5.0;
      State          = WSS::CLOSED;
      Error          = ERR::Okay;
   }

   ~extWebSocket();
};

static void retire_http(extWebSocket *);
static void close_transport(extWebSocket *);
static bool flush(extWebSocket *);
static ERR socket_incoming(objNetSocket *, APTR);
static void socket_feedback(objNetSocket *, NTC, APTR);
static ERR socket_outgoing(objNetSocket *, APTR);
static ERR close_timer(extWebSocket *, int64_t, int64_t);
static ERR ping_timer(extWebSocket *, int64_t, int64_t);

// Implemented in class_websocket_server.cpp

static bool server_announce(extWebSocket *);
static void server_connection_closed(extWebSocket *);
static void server_websocket_freed(extWebSocket *);

//********************************************************************************************************************

static void clear_callback(FUNCTION &Callback)
{
   if (Callback.defined()) {
      Callback.unpin();
      Callback.clear();
   }
}

static bool generate_mask(ws::MaskKey &Key)
{
   return crypto::RandomBytes(std::span<int8_t>((int8_t *)Key.data(), Key.size())) IS ERR::Okay;
}

//********************************************************************************************************************
// Deferred work is posted as a message so that it runs outside of HTTP and Network callbacks.

struct DeferredWork {
   OBJECTID WebSocketID; // The WebSocket that posted the work
   OBJECTID FreeHTTP;    // Free this retired HTTP object
   OBJECTID FreeSocket;  // Free this retired socket
   OBJECTID FreeObject;  // Free this object unconditionally (used by the server role)
   bool Handover;        // Complete a pending handover
};

static void send_deferred(const DeferredWork &Work)
{
   if (SendMessage(glDeferredMsgID, MSF::NIL, std::span((const int8_t *)&Work, sizeof(Work))) != ERR::Okay) {
      kt::Log(__FUNCTION__).warning(ERR::MessageOperation);
   }
}

static void post_deferred(OBJECTID WebSocketID, OBJECTID FreeHTTP, OBJECTID FreeSocket, bool Handover)
{
   send_deferred(DeferredWork { WebSocketID, FreeHTTP, FreeSocket, 0, Handover });
}

// Frees an object once the current callbacks have returned.  Freeing an object that no longer exists is harmless
// because object IDs are not reused.

static void free_deferred(OBJECTID ObjectID)
{
   send_deferred(DeferredWork { 0, 0, 0, ObjectID, false });
}

// Releases a pin taken at a callback entry point.  An object that is freed while pinned, for example by a user
// callback, is only marked for collection, and a plain unpin() does not collect it.  Its destruction is completed by a
// deferred message so that it never runs inside the Network or timer callback that is still on the stack.

static void release_pin(Object *Target)
{
   const bool collect = Target->defined(NF::FREE_ON_UNLOCK) and (not Target->defined(NF::FREE));
   Target->unpin();
   if (collect and (not Target->isPinned())) free_deferred(Target->UID);
}

//********************************************************************************************************************
// Timers are subscribed within the WebSocket's context so that they are removed if the object is freed.

static void set_timer(extWebSocket *Self, APTR &Handle, double Seconds,
   ERR (*Routine)(extWebSocket *, int64_t, int64_t))
{
   if (Seconds <= 0) return;
   if (Handle) {
      UpdateTimer(Handle, Seconds);
      return;
   }

   kt::SwitchContext context(Self);
   if (auto error = SubscribeTimer(Seconds, C_FUNCTION(Routine), &Handle); error != ERR::Okay) {
      Handle = nullptr;
      kt::Log(__FUNCTION__).warning(error);
   }
}

static void cancel_timer(APTR &Handle)
{
   if (Handle) {
      UpdateTimer(Handle, 0);
      Handle = nullptr;
   }
}

//********************************************************************************************************************
// Sets the state and reports it to the StateChanged callback.  Returns false if the object was freed by the callback.

static bool set_state(extWebSocket *Self, WSS State)
{
   if (Self->State IS State) return not Self->collecting();

   kt::Log log(__FUNCTION__);
   log.msg("State: %d -> %d", int(Self->State), int(State));
   Self->State = State;

   if (Self->StateChanged.stale()) clear_callback(Self->StateChanged);
   if (not Self->StateChanged.defined()) return not Self->collecting();

   // Invoke a pinned copy because the callback may replace StateChanged while running.

   auto callback = Self->StateChanged;
   callback.pin();
   Self->pin();
   auto lifetime = kt::Defer([&]() { callback.unpin(); Self->unpin(); });

   if (callback.isC()) {
      auto routine = (void (*)(extWebSocket *, WSS, APTR))callback.Routine;
      kt::SwitchContext context(callback.Context);
      routine(Self, State, callback.Meta);
   }
   else if (callback.isScript()) {
      sc::Call(callback, std::to_array<ScriptArg>({
         { "WebSocket", Self, FD_OBJECTPTR },
         { "State", int(State) }
      }));
   }

   return not Self->collecting();
}

//********************************************************************************************************************
// Moves to CLOSED, recording the close status first so that it is visible to the StateChanged callback.  The status
// is only recorded if a close code has not already been set by a received Close frame or a protocol failure.

static bool close_connection(extWebSocket *Self, int Code, std::string_view Reason, ERR Error)
{
   if (Self->State IS WSS::CLOSED) return not Self->collecting();

   if (not Self->CloseCode) {
      Self->CloseCode = Code;
      Self->CloseReason.assign(Reason);
   }
   if ((Error != ERR::Okay) and (Self->Error IS ERR::Okay)) Self->Error = Error;

   cancel_timer(Self->CloseTimer);
   cancel_timer(Self->PingTimer);
   retire_http(Self);
   close_transport(Self);
   Self->Queue.reset();
   Self->HandoverPending = false;
   Self->Prefix.clear();
   Self->Message.clear();
   if (not set_state(Self, WSS::CLOSED)) return false;
   if (Self->Server) server_connection_closed(Self); // May free Self through a deferred message, never immediately
   return not Self->collecting();
}

//********************************************************************************************************************
// Passes a message, or a message fragment in streaming mode, to the Incoming callback.  Returns false if the object
// was freed by the callback.

static bool deliver_message(extWebSocket *Self, std::span<const uint8_t> Data, WSM Type, bool Final)
{
   if (Self->Incoming.stale()) clear_callback(Self->Incoming);
   if (not Self->Incoming.defined()) return true;

   auto callback = Self->Incoming; // The callback may replace Incoming while running
   callback.pin();
   Self->pin();
   auto lifetime = kt::Defer([&]() { callback.unpin(); Self->unpin(); });

   ERR error = ERR::Okay;
   if (callback.isC()) {
      auto routine = (ERR (*)(extWebSocket *, const void *, int, WSM, bool, APTR))callback.Routine;
      kt::SwitchContext context(callback.Context);
      error = routine(Self, Data.data(), int(Data.size()), Type, Final, callback.Meta);
   }
   else if (callback.isScript()) {
      std::span<std::byte> span((std::byte *)Data.data(), Data.size());
      if (sc::Call(callback, std::to_array<ScriptArg>({
            { "WebSocket", Self, FD_OBJECTPTR },
            { "Buffer",    &span, FDF_SPAN|FD_BYTE },
            { "Type",      int(Type) },
            { "Final",     Final ? 1 : 0 }
         }), error) != ERR::Okay) error = ERR::Function;
   }

   if (error > ERR::ExceptionThreshold) {
      kt::Log(__FUNCTION__).warning("Incoming callback returned: %s", GetErrorMsg(error));
   }

   return not Self->collecting();
}

//********************************************************************************************************************
// Reports a received Ping or Pong to the Heartbeat callback.  Returns false if the object was freed by the callback.

static bool deliver_heartbeat(extWebSocket *Self, std::span<const uint8_t> Data, bool Pong)
{
   if (Self->Heartbeat.stale()) clear_callback(Self->Heartbeat);
   if (not Self->Heartbeat.defined()) return true;

   auto callback = Self->Heartbeat;
   callback.pin();
   Self->pin();
   auto lifetime = kt::Defer([&]() { callback.unpin(); Self->unpin(); });

   if (callback.isC()) {
      auto routine = (void (*)(extWebSocket *, const void *, int, bool, APTR))callback.Routine;
      kt::SwitchContext context(callback.Context);
      routine(Self, Data.data(), int(Data.size()), Pong, callback.Meta);
   }
   else if (callback.isScript()) {
      std::span<std::byte> span((std::byte *)Data.data(), Data.size());
      sc::Call(callback, std::to_array<ScriptArg>({
         { "WebSocket", Self, FD_OBJECTPTR },
         { "Buffer",    &span, FDF_SPAN|FD_BYTE },
         { "Pong",      Pong ? 1 : 0 }
      }));
   }

   return not Self->collecting();
}

//********************************************************************************************************************
// Reports that the send queue has emptied.  Returns false if the object was freed by the callback.

static bool deliver_outgoing(extWebSocket *Self)
{
   if (Self->Outgoing.stale()) clear_callback(Self->Outgoing);
   if (not Self->Outgoing.defined()) return true;

   auto callback = Self->Outgoing;
   callback.pin();
   Self->pin();
   auto lifetime = kt::Defer([&]() { callback.unpin(); Self->unpin(); });

   if (callback.isC()) {
      auto routine = (void (*)(extWebSocket *, APTR))callback.Routine;
      kt::SwitchContext context(callback.Context);
      routine(Self, callback.Meta);
   }
   else if (callback.isScript()) {
      sc::Call(callback, std::to_array<ScriptArg>({ { "WebSocket", Self, FD_OBJECTPTR } }));
   }

   return not Self->collecting();
}

//********************************************************************************************************************
// The connection was lost without completing the closing handshake, or after it completed.

static bool transport_lost(extWebSocket *Self)
{
   kt::Log(__FUNCTION__).msg("Connection lost.");

   if ((Self->State IS WSS::CLOSING) and (Self->CloseReceived or Self->Failed)) {
      return close_connection(Self, int(WSC::ABNORMAL), "Connection lost", ERR::Okay);
   }
   else return close_connection(Self, int(WSC::ABNORMAL), "Connection lost", ERR::Disconnected);
}

//********************************************************************************************************************
// Writes queued frames until the queue is empty or the transport stops accepting data, in which case the transport's
// writability notification is enabled and pump() resumes from socket_outgoing().  Returns false if the object was
// freed.

static bool pump(extWebSocket *Self)
{
   if ((not Self->Transport) or (not Self->Queue) or Self->Pumping) return not Self->collecting();

   Self->Pumping = true;
   auto restore = kt::Defer([&]() { Self->Pumping = false; });

   if (Self->Scratch.empty()) Self->Scratch.resize(WRITE_CHUNK_SIZE);

   while (true) {
      if (Self->Transport->busy()) {
         Self->Transport->watch_writes(true);
         return true;
      }

      auto chunk = Self->Queue->next_chunk(Self->Scratch);
      if (chunk.empty()) {
         if (Self->Queue->has_failed()) {
            Self->Pumping = false;
            return close_connection(Self, int(WSC::ABNORMAL), "Masking key generation failed", ERR::Failed);
         }
         break;
      }

      int accepted = 0;
      auto error = Self->Transport->write(chunk, accepted);
      if (accepted > 0) Self->Queue->consume(size_t(accepted));

      if ((error != ERR::Okay) and (error != ERR::BufferOverflow)) {
         kt::Log(__FUNCTION__).msg("Write failed: %s", GetErrorMsg(error));
         Self->Pumping = false;
         return transport_lost(Self);
      }

      if (size_t(accepted) < chunk.size()) {
         Self->Transport->watch_writes(true);
         return true;
      }
   }

   if (not Self->Transport->busy()) Self->Transport->watch_writes(false);
   return true;
}

//********************************************************************************************************************
// Releases the connection once the transport has drained.  Inside the transport's Incoming callback the release is
// handed to Network, which closes the socket after its own queue has been written.

static bool disconnect_when_drained(extWebSocket *Self)
{
   if (Self->Transport->busy() and (not Self->InSocketCallback)) {
      Self->Transport->watch_writes(true); // socket_outgoing() calls check_close() again
      return true;
   }
   return close_connection(Self, int(WSC::ABNORMAL), "Connection closed", ERR::Okay);
}

//********************************************************************************************************************
// Advances the closing handshake.  Called whenever the queue, the transport or the received frames change while the
// connection is CLOSING.  Returns false if the object was freed.

static bool check_close(extWebSocket *Self)
{
   if ((Self->State != WSS::CLOSING) or (not Self->Transport) or (not Self->Queue)) return not Self->collecting();
   if (not Self->Queue->idle()) return true; // The writability notification drives progress

   if (not Self->DisconnectReady) {
      const bool complete = Self->Failed or (Self->CloseReceived and Self->Queue->close_sent());
      if (not complete) return true; // Waiting for the peer's Close, bounded by CloseTimeout

      if ((Self->Role IS ws::Role::CLIENT) and (not Self->Failed)) {
         // RFC 6455 section 7.1.1: the server closes the TCP connection first.  Wait briefly before closing it.
         if (not Self->AwaitingServerClose) {
            Self->AwaitingServerClose = true;
            auto wait = ((Self->CloseTimeout > 0) and (Self->CloseTimeout < SERVER_CLOSE_WAIT)) ?
               Self->CloseTimeout : SERVER_CLOSE_WAIT;
            set_timer(Self, Self->CloseTimer, wait, close_timer);
         }
         return true;
      }

      Self->DisconnectReady = true;
   }

   return disconnect_when_drained(Self);
}

//********************************************************************************************************************
// Writes queued data, advances the closing handshake and reports an emptied queue.  Returns false if the object was
// freed.

static bool flush(extWebSocket *Self)
{
   if (not pump(Self)) return false;
   if (not Self->Queue) return true;

   if (Self->State IS WSS::CLOSING) return check_close(Self);

   if (Self->Queue->idle()) {
      if (Self->AwaitingDrain and (Self->State IS WSS::OPEN)) {
         Self->AwaitingDrain = false;
         return deliver_outgoing(Self);
      }
   }
   else Self->AwaitingDrain = true;

   return true;
}

//********************************************************************************************************************
// Enters CLOSING after a Close frame has been queued.  Returns false if the object was freed.

static bool enter_closing(extWebSocket *Self)
{
   cancel_timer(Self->PingTimer);
   Self->PingOutstanding = false;
   if (not Self->CloseTimer) set_timer(Self, Self->CloseTimer, Self->CloseTimeout, close_timer);

   if (Self->State IS WSS::OPEN) {
      if (not set_state(Self, WSS::CLOSING)) return false;
      if (Self->State != WSS::CLOSING) return true; // Closed by the callback
   }

   return flush(Self);
}

//********************************************************************************************************************
// Starts the closing handshake with an encoded Close payload.

static bool begin_close(extWebSocket *Self, std::span<const uint8_t> Payload)
{
   if (Self->Queue->push_close(Payload, false) != ws::QueueResult::OKAY) return true;
   kt::Log(__FUNCTION__).msg("Closing handshake started.");
   return enter_closing(Self);
}

//********************************************************************************************************************
// Fails the connection for a protocol violation (RFC 6455 section 7.1.7).  Unsent data is discarded, a Close frame
// carrying Code is sent and the connection is released once it has been written.  Returns false if the object was
// freed.

static bool fail_connection(extWebSocket *Self, int Code, std::string_view Reason)
{
   kt::Log(__FUNCTION__).warning("Protocol failure %d: %.*s", Code, int(Reason.size()), Reason.data());

   if (Self->State IS WSS::CLOSED) return not Self->collecting();

   Self->Failed = true;
   Self->CloseCode = Code;
   Self->CloseReason.assign(Reason);
   if (Self->Error IS ERR::Okay) Self->Error = ERR::InvalidData;

   if (not Self->Queue) return close_connection(Self, Code, Reason, ERR::InvalidData);

   std::array<uint8_t, ws::MAX_CONTROL_PAYLOAD> payload;
   auto length = ws::encode_close_payload(payload, Code, "");
   if (Self->Queue->push_close(std::span(payload.data(), length), true) != ws::QueueResult::OKAY) {
      Self->Queue->discard_unsent(); // A Close is already queued; send nothing else before it
   }

   return enter_closing(Self);
}

//********************************************************************************************************************
// A Close frame was received.  Unsent data is discarded and the Close is echoed unless one has already been queued.

static bool peer_closed(extWebSocket *Self, int Code, std::string_view Reason)
{
   kt::Log(__FUNCTION__).msg("Close received: %d", Code);

   Self->CloseReceived = true;
   if (not Self->CloseCode) {
      Self->CloseCode = Code;
      Self->CloseReason.assign(Reason);
   }

   if (not Self->Queue) return close_connection(Self, Code, Reason, ERR::Okay);

   if (Self->Queue->close_queued()) Self->Queue->discard_unsent();
   else {
      std::array<uint8_t, ws::MAX_CONTROL_PAYLOAD> payload;
      auto length = ws::encode_close_payload(payload, (Code IS ws::CLOSE_NO_STATUS) ? ws::CLOSE_NORMAL : Code, "");
      Self->Queue->push_close(std::span(payload.data(), length), true);
   }

   return enter_closing(Self);
}

//********************************************************************************************************************
// Runs received bytes through the message reader.  Returns false if processing must stop because the connection no
// longer accepts input or the object was freed.

static bool process_input(extWebSocket *Self, std::span<uint8_t> Data)
{
   auto accepting = [Self]() {
      return Self->Reader and (not Self->Failed) and (not Self->CloseReceived) and
         ((Self->State IS WSS::OPEN) or (Self->State IS WSS::CLOSING));
   };

   while (accepting()) {
      ws::ReadEvent event;
      auto consumed = Self->Reader->read(Data, event);
      Data = Data.subspan(consumed);

      if (event.status IS ws::ReadStatus::NEED_MORE) return true;

      if (event.status IS ws::ReadStatus::DATA) {
         auto type = (event.opcode IS ws::Opcode::TEXT) ? WSM::TEXT : WSM::BINARY;

         if ((Self->Flags & WSF::STREAM_MESSAGES) != WSF::NIL) {
            if (not deliver_message(Self, event.data, type, event.message_end)) return false;
         }
         else {
            Self->Message.insert(Self->Message.end(), event.data.begin(), event.data.end());
            if (event.message_end) {
               std::vector<uint8_t> message;
               message.swap(Self->Message);
               if (not deliver_message(Self, message, type, true)) return false;
            }
         }
      }
      else if (event.status IS ws::ReadStatus::CONTROL) {
         if (event.opcode IS ws::Opcode::CLOSE) {
            std::string reason(event.reason);
            peer_closed(Self, event.close_code, reason);
            return false;
         }
         else if (event.opcode IS ws::Opcode::PING) {
            if (((Self->Flags & WSF::NO_AUTO_PONG) IS WSF::NIL) and (Self->State IS WSS::OPEN) and Self->Queue) {
               Self->Queue->push_control(ws::Opcode::PONG, event.data);
            }
            if (not deliver_heartbeat(Self, event.data, false)) return false;
            if (not flush(Self)) return false;
         }
         else { // PONG
            Self->PingOutstanding = false;
            if (not deliver_heartbeat(Self, event.data, true)) return false;
         }
      }
      else { // ERROR
         std::string reason(event.reason);
         fail_connection(Self, event.close_code, reason);
         return false;
      }
   }

   return false;
}

//********************************************************************************************************************
// Announces OPEN and delivers the bytes that arrived with the handshake response.  Runs outside the HTTP notification,
// either from the deferred message or from the first socket callback, whichever occurs first.

static bool complete_handover(extWebSocket *Self)
{
   if (not Self->HandoverPending) return true;
   Self->HandoverPending = false;

   auto prefix = std::move(Self->Prefix);
   Self->Prefix.clear();

   Self->LastReceived = PreciseTime();
   if (not set_state(Self, WSS::OPEN)) return false;
   if (Self->State != WSS::OPEN) return true;

   // The server's Connected callback installs the application's callbacks, so it precedes the prefix.  It may also
   // send, close or free the connection.

   if (Self->Server) {
      if (not server_announce(Self)) return false;
      if (Self->State IS WSS::CLOSED) return true;
   }

   if (Self->State IS WSS::OPEN) set_timer(Self, Self->PingTimer, Self->PingInterval, ping_timer);

   if (not prefix.empty()) return process_input(Self, std::span<uint8_t>((uint8_t *)prefix.data(), prefix.size()));
   return true;
}

//********************************************************************************************************************

static ERR deferred_msg_handler(APTR Meta, int MsgID, MSGID MsgType, std::span<std::byte> Message)
{
   if (Message.size() != sizeof(DeferredWork)) return kt::Log(__FUNCTION__).warning(ERR::Args);

   auto work = (DeferredWork *)Message.data();

   if (work->FreeObject) {
      FreeResource(work->FreeObject);
      return ERR::Okay;
   }

   // If the WebSocket no longer exists, its destructor has already released everything.

   kt::ScopedObjectLock<extWebSocket> websocket(work->WebSocketID);
   if ((not websocket.granted()) or websocket->collecting() or (websocket->classID() != CLASSID::WEBSOCKET)) {
      return ERR::Okay;
   }

   if (work->FreeHTTP) {
      auto &retired = websocket->RetiredHTTP;
      if (auto it = std::find(retired.begin(), retired.end(), work->FreeHTTP); it != retired.end()) {
         retired.erase(it);
         FreeResource(work->FreeHTTP);
      }
   }

   if ((work->FreeSocket) and (work->FreeSocket IS websocket->RetiredSocketID)) {
      websocket->RetiredSocketID = 0;
      FreeResource(work->FreeSocket);
   }

   if (work->Handover) complete_handover(*websocket);
   return ERR::Okay;
}

//********************************************************************************************************************
// The HTTP object is freed outside of its own callbacks.  Its later notifications are ignored because Self->HTTP no
// longer refers to it.

static void retire_http(extWebSocket *Self)
{
   if (not Self->HTTP) return;
   auto id = Self->HTTP->UID;
   Self->HTTP = nullptr;
   Self->RetiredHTTP.push_back(id);
   post_deferred(Self->UID, id, 0, false);
}

//********************************************************************************************************************
// Server role: releases the ClientSocket without taking ownership of it.  Inside the Incoming callback Network is asked
// to free the socket, or to disconnect once its queue has been written if bytes remain.  ERR::Terminate is not used in
// the latter case because Network frees a ClientSocket immediately, discarding its queue.  Inside a disconnection
// notification the socket is already closed and the server frees it.  Inside the Outgoing callback the socket is freed
// by a deferred message because Network uses it after the callback returns.  Freeing the socket otherwise is safe:
// Network defers the destruction of a locked ClientSocket until it is unlocked.

static void release_client_socket(extWebSocket *Self, objClientSocket *Socket, bool Busy)
{
   if (Self->InFeedback) return;
   else if (Self->InSocketCallback) {
      if (Busy) Socket->deactivate();
      else Self->TerminateSocket = true;
   }
   else if (Self->InOutgoing) free_deferred(Socket->UID);
   else FreeResource(Socket);
}

//********************************************************************************************************************
// Releases the connection.  A socket cannot be freed inside its own callbacks: from Incoming, Network is asked to close
// it once its queue has been written; from Outgoing, it is freed by a deferred message.  Callers that need queued
// bytes to reach the peer must wait for the transport to drain first (see disconnect_when_drained()).

static void close_transport(extWebSocket *Self)
{
   if (not Self->Transport) return;

   auto socket = Self->Transport->object();
   auto busy = Self->Transport->busy();
   Self->Transport.reset(); // Detaches the callbacks
   Self->Reader.reset();

   if (Self->Role IS ws::Role::SERVER) {
      release_client_socket(Self, (objClientSocket *)socket, busy);
      return;
   }

   if (Self->RetiredSocketID) { FreeResource(Self->RetiredSocketID); Self->RetiredSocketID = 0; }

   if (Self->InSocketCallback) {
      Self->TerminateSocket = true;
      Self->RetiredSocketID = socket->UID;
   }
   else if (Self->InOutgoing) {
      Self->RetiredSocketID = socket->UID;
      post_deferred(Self->UID, 0, socket->UID, false);
   }
   else FreeResource(socket);
}

//********************************************************************************************************************

// Reads and processes everything that the transport has received.  Called from the transport's Incoming callback, for
// either role.  Returns ERR::Terminate if Network must close the socket on return.

static ERR transport_incoming(extWebSocket *Self)
{
   kt::Log log(__FUNCTION__);

   auto socket = Self->Transport->object();
   Self->pin();
   auto lifetime = kt::Defer([&]() { release_pin(Self); });

   auto in_socket = Self->InSocketCallback;
   Self->InSocketCallback = true;
   Self->TerminateSocket = false;
   auto restore = kt::Defer([&]() { Self->InSocketCallback = in_socket; });

   if (Self->HandoverPending) {
      if (not complete_handover(Self)) return Self->TerminateSocket ? ERR::Terminate : ERR::Okay;
   }

   // Bytes are always read so that Network does not discard them with a warning.  They are decoded only while the
   // connection accepts input.

   std::array<uint8_t, 16 * 1024> buffer;
   while (Self->Transport and (Self->Transport->object() IS socket) and (not Self->collecting())) {
      int length = 0;
      if (auto error = Self->Transport->read(buffer, length); error != ERR::Okay) {
         log.msg("Read failed: %s", GetErrorMsg(error));
         transport_lost(Self);
         break;
      }

      if (length <= 0) break;
      Self->LastReceived = PreciseTime();
      if (not process_input(Self, std::span<uint8_t>(buffer.data(), size_t(length)))) {
         if (Self->collecting() or (Self->State IS WSS::CLOSED)) break;
      }
   }

   if (Self->collecting()) return ERR::Okay;
   return Self->TerminateSocket ? ERR::Terminate : ERR::Okay;
}

//********************************************************************************************************************
// The transport reported that the connection has been lost.

static void transport_disconnected(extWebSocket *Self)
{
   // The notification can arrive while transport_incoming() is running, so the flags are restored rather than cleared.

   Self->pin();
   auto in_socket = Self->InSocketCallback, in_feedback = Self->InFeedback;
   Self->InSocketCallback = true; // Retain the socket rather than freeing it inside its own callback
   Self->InFeedback = true;
   auto lifetime = kt::Defer([&]() {
      Self->InSocketCallback = in_socket;
      Self->InFeedback = in_feedback;
      release_pin(Self);
   });

   if (Self->HandoverPending) {
      if (not complete_handover(Self)) return;
   }
   transport_lost(Self);
}

//********************************************************************************************************************
// The transport's own queue is empty and the connection can accept more data.

static void transport_writable(extWebSocket *Self)
{
   Self->pin();
   auto in_outgoing = Self->InOutgoing;
   Self->InOutgoing = true;
   auto lifetime = kt::Defer([&]() { Self->InOutgoing = in_outgoing; release_pin(Self); });

   flush(Self);
}

//********************************************************************************************************************
// Client role socket callbacks.  The WebSocket is identified by the callback context.

static bool owns_socket(extWebSocket *Self, OBJECTPTR Socket)
{
   return (Self->classID() IS CLASSID::WEBSOCKET) and Self->Transport and (Self->Transport->object() IS Socket);
}

static ERR socket_incoming(objNetSocket *Socket, APTR Meta)
{
   auto Self = (extWebSocket *)CurrentContext();
   if (not owns_socket(Self, Socket)) return ERR::Terminate;
   return transport_incoming(Self);
}

static void socket_feedback(objNetSocket *Socket, NTC State, APTR Meta)
{
   auto Self = (extWebSocket *)CurrentContext();
   if ((State IS NTC::DISCONNECTED) and owns_socket(Self, Socket)) transport_disconnected(Self);
}

static ERR socket_outgoing(objNetSocket *Socket, APTR Meta)
{
   auto Self = (extWebSocket *)CurrentContext();
   if (not owns_socket(Self, Socket)) return ERR::Terminate;
   transport_writable(Self);
   return ERR::Okay; // Network counts any other result as an error
}

//********************************************************************************************************************
// CloseTimeout expired, or a client finished waiting for the server to close the TCP connection.

static ERR close_timer(extWebSocket *Self, int64_t Elapsed, int64_t CurrentTime)
{
   Self->CloseTimer = nullptr; // Removed by ERR::Terminate

   if (Self->State != WSS::CLOSING) return ERR::Terminate;

   Self->pin();
   auto lifetime = kt::Defer([&]() { release_pin(Self); });

   if (Self->AwaitingServerClose and (not Self->DisconnectReady)) {
      // Close the connection from this side.  CloseTimeout bounds the wait for the transport to drain.
      Self->DisconnectReady = true;
      set_timer(Self, Self->CloseTimer, (Self->CloseTimeout > 0) ? Self->CloseTimeout : SERVER_CLOSE_WAIT,
         close_timer);
      check_close(Self);
   }
   else if (Self->CloseReceived or Self->Failed) {
      close_connection(Self, int(WSC::ABNORMAL), "Connection closed", ERR::Okay);
   }
   else {
      kt::Log(__FUNCTION__).msg("The peer did not complete the closing handshake.");
      close_connection(Self, int(WSC::ABNORMAL), "Closing handshake timed out", ERR::TimeOut);
   }

   return ERR::Terminate;
}

//********************************************************************************************************************
// Sends a keep-alive ping after PingInterval seconds without received data, and fails the connection if nothing is
// received within another interval.

static ERR ping_timer(extWebSocket *Self, int64_t Elapsed, int64_t CurrentTime)
{
   if ((Self->State != WSS::OPEN) or (not Self->Queue) or (Self->PingInterval <= 0)) {
      Self->PingTimer = nullptr;
      return ERR::Terminate;
   }

   Self->pin();
   auto lifetime = kt::Defer([&]() { release_pin(Self); });

   if (Self->PingOutstanding) {
      if (Self->LastReceived < Self->PingSentAt) {
         kt::Log(__FUNCTION__).msg("The peer did not answer a keep-alive ping.");
         Self->PingTimer = nullptr;
         close_connection(Self, int(WSC::ABNORMAL), "Keep-alive timeout", ERR::TimeOut);
         return ERR::Terminate;
      }
      Self->PingOutstanding = false;
   }

   const auto interval = int64_t(Self->PingInterval * 1000000.0);
   if (CurrentTime - Self->LastReceived >= interval) {
      if (Self->Queue->push_control(ws::Opcode::PING, {}, true) IS ws::QueueResult::OKAY) {
         Self->PingOutstanding = true;
         Self->PingSentAt = CurrentTime;

         // A running timer cannot be cancelled through its handle, so it is withheld while flush() runs.

         auto handle = Self->PingTimer;
         Self->PingTimer = nullptr;
         if ((not flush(Self)) or (Self->State != WSS::OPEN) or Self->PingTimer) return ERR::Terminate;
         Self->PingTimer = handle;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// HTTP_STATE: UPGRADE_READY.  Validates the WebSocket-specific response fields.  Only ERR::Okay accepts the switch.

static ERR validate_handshake(extWebSocket *Self, objHTTP *HTTP)
{
   kt::Log log(__FUNCTION__);

   auto reject = [&](std::string_view Reason) {
      log.warning("Handshake rejected: %.*s", int(Reason.size()), Reason.data());
      Self->Error = ERR::InvalidHTTPResponse;
      Self->CloseReason.assign(Reason);
      return ERR::Failed;
   };

   std::string headers;
   if (HTTP->getResponseHeaders(headers) != ERR::Okay) return reject("Response headers are unavailable");

   std::vector<ws::HeaderField> fields;
   if (not ws::parse_header_fields(headers, fields)) return reject("Response headers are malformed");

   std::vector<std::string_view> requested(Self->RequestedProtocols.begin(), Self->RequestedProtocols.end());
   std::string_view selected;
   if (auto reason = ws::validate_response(fields, Self->ExpectedAccept, requested, selected); not reason.empty()) {
      return reject(reason);
   }

   Self->Protocol.assign(selected);
   return ERR::Okay;
}

//********************************************************************************************************************
// HTTP state: UPGRADED.  Takes the transferred socket and prefix.  The socket must not be read and user callbacks must
// not run during this notification, so OPEN is announced from the deferred message.

static ERR take_connection(extWebSocket *Self, objHTTP *HTTP)
{
   kt::Log log(__FUNCTION__);

   HTTPUpgrade *upgrade = nullptr;
   if ((HTTP->getUpgrade(upgrade) != ERR::Okay) or (not upgrade) or (not upgrade->Socket)) {
      // Ownership has been committed to Self; the socket is released when Self is freed.
      log.warning("Upgrade descriptor is unavailable.");
      Self->Error = ERR::NoFieldAccess;
      close_connection(Self, int(WSC::ABNORMAL), "Connection handover failed", ERR::NoFieldAccess);
      return ERR::Failed;
   }

   {
      kt::SwitchContext context(Self); // The callbacks identify the WebSocket through their context
      Self->Transport = std::make_unique<ws::NetSocketTransport>(upgrade->Socket, Self,
         C_FUNCTION(socket_incoming), C_FUNCTION(socket_feedback), C_FUNCTION(socket_outgoing));
   }
   Self->Prefix = upgrade->Data;
   Self->Status = int(HTTP->Status);

   auto frame_limit = Self->MaxFrameSize ? Self->MaxFrameSize : Self->MaxMessageSize;
   Self->Reader = std::make_unique<ws::MessageReader>(ws::Role::CLIENT, uint64_t(frame_limit),
      uint64_t(Self->MaxMessageSize), (Self->Flags & WSF::NO_UTF8_CHECK) IS WSF::NIL);
   Self->Queue = std::make_unique<ws::SendQueue>(ws::Role::CLIENT, &generate_mask, uint64_t(frame_limit),
      uint64_t(Self->SendLimit));
   Self->Message.clear();
   Self->HandoverPending = true;

   log.msg("Connection transferred with a %d byte prefix.", int(Self->Prefix.size()));

   retire_http(Self);
   post_deferred(Self->UID, 0, 0, true);
   return ERR::Okay;
}

//********************************************************************************************************************
// HTTP state: COMPLETED or TERMINATED.  The server did not switch protocols, or the switch was rejected.

static void handshake_failed(extWebSocket *Self, objHTTP *HTTP)
{
   std::string reason;

   Self->Status = int(HTTP->Status);
   auto status = Self->Status;

   if (Self->Error IS ERR::Okay) { // Not already rejected by validate_handshake()
      if ((status >= 300) and (status < 400)) {
         Self->Error = ERR::HTTPStatus;
         std::string headers;
         std::vector<ws::HeaderField> fields;
         if ((HTTP->getResponseHeaders(headers) IS ERR::Okay) and ws::parse_header_fields(headers, fields)) {
            for (auto &field : fields) {
               if (kt::iequals(field.name, "Location")) { Self->Redirect.assign(field.value); break; }
            }
         }
      }
      else if (status IS int(HTS::UNAUTHORISED)) Self->Error = ERR::NotAuthorised;
      else if (status and (status != int(HTS::SWITCH_PROTOCOLS))) Self->Error = ERR::HTTPStatus;
      else Self->Error = (HTTP->Error != ERR::Okay) ? HTTP->Error : ERR::Failed;

      if (status and (status != int(HTS::SWITCH_PROTOCOLS))) reason = "HTTP " + std::to_string(status);
      else reason = GetErrorMsg(Self->Error);
   }
   else reason = Self->CloseReason;

   kt::Log(__FUNCTION__).msg("Handshake failed: %s", reason.c_str());
   close_connection(Self, int(WSC::ABNORMAL), reason, ERR::Okay);
}

//********************************************************************************************************************

static ERR http_state_changed(objHTTP *HTTP, HGS State, APTR Meta)
{
   auto Self = (extWebSocket *)Meta;
   if (Self->HTTP != HTTP) return (State IS HGS::UPGRADE_READY) ? ERR::Failed : ERR::Okay;

   switch (State) {
      case HGS::UPGRADE_READY: return validate_handshake(Self, HTTP);
      case HGS::UPGRADED:      return take_connection(Self, HTTP);
      case HGS::COMPLETED:
      case HGS::TERMINATED:    handshake_failed(Self, HTTP); return ERR::Okay;
      default:                 return ERR::Okay;
   }
}

/*********************************************************************************************************************
-ACTION-
Activate: Connects to the server at #Location.

Activation starts the opening handshake and returns immediately.  The #State changes to `CONNECTING`, then to `OPEN`
once the server's response has been validated, or to `CLOSED` if the handshake fails.  Monitor the transition with
#StateChanged.

An object can be activated again once its #State is `CLOSED`.

-ERRORS-
Okay: The handshake was started.
FieldNotSet: The #Location has not been set.
InUse: The object is already connecting or connected.
NoSupport: The object represents a connection accepted by a @WebSocketServer.
CreateObject: The HTTP object for the handshake could not be created.
-END-
*********************************************************************************************************************/

static ERR WEBSOCKET_Activate(extWebSocket *Self)
{
   kt::Log log;

   if (Self->Role IS ws::Role::SERVER) return log.warning(ERR::NoSupport);
   if (Self->State != WSS::CLOSED) return log.warning(ERR::InUse);
   if (Self->Target.host.empty()) return log.warning(ERR::FieldNotSet);

   log.branch("%s", Self->Location.c_str());

   retire_http(Self);
   close_transport(Self);
   if (Self->RetiredSocketID) { FreeResource(Self->RetiredSocketID); Self->RetiredSocketID = 0; }

   Self->Error = ERR::Okay;
   Self->Status = 0;
   Self->CloseCode = 0;
   Self->CloseReason.clear();
   Self->Redirect.clear();
   Self->Protocol.clear();
   Self->Message.clear();
   Self->Prefix.clear();
   Self->Queue.reset();
   Self->HandoverPending     = false;
   Self->AwaitingDrain       = false;
   Self->CloseReceived       = false;
   Self->Failed              = false;
   Self->AwaitingServerClose = false;
   Self->DisconnectReady     = false;
   Self->PingOutstanding     = false;

   std::string key;
   if (auto error = ws::generate_key(key); error != ERR::Okay) return log.warning(error);
   if (auto error = ws::compute_accept_key(key, Self->ExpectedAccept); error != ERR::Okay) return log.warning(error);

   objHTTP *http;
   if (NewLocalObject(CLASSID::HTTP, &http) != ERR::Okay) return log.warning(ERR::CreateObject);

   auto flags = HTF::NO_DIALOG | HTF::REQUEST_WEBSOCKET;
   if (Self->Target.secure) flags |= HTF::SSL;
   if ((Self->Flags & WSF::DISABLE_SERVER_VERIFY) != WSF::NIL) flags |= HTF::DISABLE_SERVER_VERIFY;

   http->setHost(Self->Target.host);
   http->setPort(Self->Target.port);
   http->setPath(Self->Target.target);
   http->setFlags(flags);
   http->setMethod(HTM::GET);
   http->setConnectTimeout(Self->ConnectTimeout);
   http->setDataTimeout(Self->DataTimeout);
   if (not Self->ProxyServer.empty()) {
      http->setProxyServer(Self->ProxyServer);
      http->setProxyPort(Self->ProxyPort);
   }
   http->setStateChanged(C_FUNCTION(http_state_changed, Self));

   auto error = InitObject(http);
   if (error IS ERR::Okay) error = http->setUpgrade(HTTPUpgrade { Self->UID, nullptr, {} });
   if (error IS ERR::Okay) error = http->acSetKey("Sec-WebSocket-Key", key);
   if (error IS ERR::Okay) error = http->acSetKey("Sec-WebSocket-Version", ws::WEBSOCKET_VERSION);

   if ((error IS ERR::Okay) and (not Self->RequestedProtocols.empty())) {
      std::string protocols;
      for (auto &protocol : Self->RequestedProtocols) {
         if (not protocols.empty()) protocols += ", ";
         protocols += protocol;
      }
      error = http->acSetKey("Sec-WebSocket-Protocol", protocols);
   }

   if ((error IS ERR::Okay) and (not Self->Origin.empty())) error = http->acSetKey("Origin", Self->Origin);

   for (auto &[name, value] : Self->Headers) {
      if (error != ERR::Okay) break;
      error = http->acSetKey(name, value);
   }

   if (error != ERR::Okay) {
      FreeResource(http);
      return log.warning(error);
   }

   Self->HTTP = http;
   if (not set_state(Self, WSS::CONNECTING)) return ERR::Okay;
   if ((Self->State != WSS::CONNECTING) or (Self->HTTP != http)) return ERR::Okay;

   if (error = http->activate(); error != ERR::Okay) {
      if (Self->HTTP IS http) { // The failure was not already reported through StateChanged
         Self->Error = error;
         close_connection(Self, int(WSC::ABNORMAL), GetErrorMsg(error), error);
      }
      return error;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Close: Starts the closing handshake.

Close sends a Close frame carrying `Code` and an optional `Reason`, and changes the #State to `CLOSING`.  Messages that
were queued by #Send() beforehand are transmitted first; further calls to #Send() fail.  Messages received before the
server's Close frame are still delivered to #Incoming.

The connection moves to `CLOSED` once the server has answered with its own Close frame and the TCP connection has
ended.  #CloseCode and #CloseReason then hold the status from the server's Close frame.  If the server does not answer
within #CloseTimeout seconds, the connection is dropped and #CloseCode is set to `ABNORMAL`.

`Code` must be `NORMAL` (1000), one of the other codes from 1001 to 1003 and 1007 to 1011 that describe the reason for
closing, or an application code from 3000 to 4999.  A `Code` of zero sends a Close frame without a status, in which
case `Reason` must be empty.  `Reason` is limited to 123 bytes of UTF-8.

Calling Close while the handshake is in progress abandons it, as for #Deactivate().  Calling it while `CLOSING` or
`CLOSED` has no effect.

-INPUT-
int Code: The close status code, or zero to send no status.
cstr Reason: Optional.  A short UTF-8 description of the reason for closing.

-ERRORS-
Okay: The closing handshake was started, or the connection is already closing or closed.
NullArgs
OutOfRange: `Code` is not a permitted close status.
InvalidValue: `Reason` exceeds 123 bytes, is not valid UTF-8, or was supplied without a `Code`.
-END-

*********************************************************************************************************************/

static ERR WEBSOCKET_Close(extWebSocket *Self, struct wsk::Close *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   std::string_view reason = Args->Reason ? std::string_view(Args->Reason) : std::string_view();
   if (Args->Code) {
      if (not ws::valid_close_code(Args->Code)) return log.warning(ERR::OutOfRange);
   }
   else if (not reason.empty()) return log.warning(ERR::InvalidValue);
   if ((reason.size() > ws::MAX_CLOSE_REASON) or (not ws::valid_utf8(reason))) return log.warning(ERR::InvalidValue);

   if (Self->State IS WSS::CONNECTING) {
      close_connection(Self, int(WSC::ABNORMAL), "Handshake cancelled", ERR::Cancelled);
      return ERR::Okay;
   }
   if ((Self->State != WSS::OPEN) or (not Self->Queue)) return ERR::Okay;

   log.branch("Code: %d", Args->Code);

   std::array<uint8_t, ws::MAX_CONTROL_PAYLOAD> payload;
   size_t length = Args->Code ? ws::encode_close_payload(payload, Args->Code, reason) : 0;
   begin_close(Self, std::span(payload.data(), length));
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Deactivate: Abandons the handshake or closes the connection.

Deactivating a connecting WebSocket abandons the handshake and the #State changes to `CLOSED`.  An open connection
starts the closing handshake with status `NORMAL`, as for #Close().  Deactivating a `CLOSING` or `CLOSED` connection has
no effect.
-END-
*********************************************************************************************************************/

static ERR WEBSOCKET_Deactivate(extWebSocket *Self)
{
   if (Self->State IS WSS::CONNECTING) {
      close_connection(Self, int(WSC::ABNORMAL), "Handshake cancelled", ERR::Cancelled);
   }
   else if ((Self->State IS WSS::OPEN) and Self->Queue) {
      std::array<uint8_t, ws::MAX_CONTROL_PAYLOAD> payload;
      auto length = ws::encode_close_payload(payload, int(WSC::NORMAL), "");
      begin_close(Self, std::span(payload.data(), length));
   }
   return ERR::Okay;
}

//********************************************************************************************************************

extWebSocket::~extWebSocket()
{
   cancel_timer(CloseTimer);
   cancel_timer(PingTimer);

   if (HTTP) {
      HTTP->setStateChanged(FUNCTION{});
      FreeResource(HTTP);
      HTTP = nullptr;
   }

   if (Server) server_websocket_freed(this);

   // Freeing an open connection is abortive: queued frames are discarded and no Close frame is sent.  A ClientSocket
   // belongs to the server's NetServer; freeing it here releases the connection, and Network defers its destruction if
   // it is locked by a callback in progress.

   if (Transport) {
      auto socket = Transport->object();
      Transport.reset();
      if ((Role IS ws::Role::SERVER) and InOutgoing) free_deferred(socket->UID);
      else FreeResource(socket);
   }

   for (auto id : RetiredHTTP) FreeResource(id);
   if (RetiredSocketID) FreeResource(RetiredSocketID);

   clear_callback(Incoming);
   clear_callback(StateChanged);
   clear_callback(Outgoing);
   clear_callback(Heartbeat);
}

static ERR WEBSOCKET_Init(extWebSocket *Self)
{
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Ping: Sends a Ping frame.

Ping queues a Ping frame ahead of any queued messages; it is transmitted as soon as the frame in progress has been
written.  The server answers with a Pong frame carrying the same `Data`, which is reported to the #Heartbeat callback.

Automatic keep-alive pings are available through #PingInterval and do not require this method.

-INPUT-
array(char) Data: Optional.  Up to 125 bytes of application data to include in the frame.

-ERRORS-
Okay: The Ping was queued.
NullArgs
InvalidState: The #State is not `OPEN`.
DataSize: `Data` exceeds 125 bytes.
BufferOverflow: Queuing the frame would exceed the #SendLimit.
-END-

*********************************************************************************************************************/

static ERR WEBSOCKET_Ping(extWebSocket *Self, struct wsk::Ping *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if ((Self->State != WSS::OPEN) or (not Self->Queue)) return log.warning(ERR::InvalidState);
   if (Args->Data.size() > ws::MAX_CONTROL_PAYLOAD) return log.warning(ERR::DataSize);

   auto data = std::span<const uint8_t>((const uint8_t *)Args->Data.data(), Args->Data.size());
   auto result = Self->Queue->push_control(ws::Opcode::PING, data);
   if (result IS ws::QueueResult::LIMIT) return ERR::BufferOverflow;
   if (result != ws::QueueResult::OKAY) return log.warning(ERR::InvalidState);

   flush(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Send: Queues a message for transmission.

Send copies a complete message to the send queue and starts transmitting it immediately.  The method never blocks:
bytes that the connection cannot accept are retained and written as the peer reads them, in order and without
duplication.  #Pending reports the number of queued bytes and the #Outgoing callback reports when the queue has been
emptied.

A message that would take the queued total beyond #SendLimit is rejected with `BufferOverflow`, leaving the queue
unchanged, so that a producer can wait for #Outgoing before sending more.  A message larger than #MaxFrameSize (or
#MaxMessageSize if #MaxFrameSize is zero) is divided into fragments of that size; Ping and Pong frames can be sent
between the fragments.

A `TEXT` message must be valid UTF-8 unless the `NO_UTF8_CHECK` flag is set.  An empty message is permitted.

-INPUT-
array(char) Data: The message payload.
int(WSM) Type: Either `TEXT` or `BINARY`.

-ERRORS-
Okay: The message was queued.
NullArgs
InvalidState: The #State is not `OPEN`.
InvalidValue: `Type` is not a valid message type.
InvalidData: A `TEXT` message is not valid UTF-8.
BufferOverflow: Queuing the message would exceed the #SendLimit.
-END-

*********************************************************************************************************************/

static ERR WEBSOCKET_Send(extWebSocket *Self, struct wsk::Send *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if ((Self->State != WSS::OPEN) or (not Self->Queue)) return log.warning(ERR::InvalidState);
   if ((Args->Type != WSM::TEXT) and (Args->Type != WSM::BINARY)) return log.warning(ERR::InvalidValue);

   auto data = std::span<const uint8_t>((const uint8_t *)Args->Data.data(), Args->Data.size());
   if ((Args->Type IS WSM::TEXT) and ((Self->Flags & WSF::NO_UTF8_CHECK) IS WSF::NIL) and (not ws::valid_utf8(data))) {
      return log.warning(ERR::InvalidData);
   }

   auto result = Self->Queue->push_message((Args->Type IS WSM::TEXT) ? ws::Opcode::TEXT : ws::Opcode::BINARY, data);
   if (result IS ws::QueueResult::LIMIT) {
      if (not Self->Queue->idle()) Self->AwaitingDrain = true; // Report through Outgoing when the queue empties
      return ERR::BufferOverflow;
   }
   if (result != ws::QueueResult::OKAY) return log.warning(ERR::InvalidState);

   flush(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SetKey: Adds a custom header to the opening handshake request.

Custom headers are sent with every subsequent activation.  Setting an existing name replaces its value.  Headers that
are managed by the WebSocket and HTTP classes, including `Host`, `Connection`, `Upgrade`, `Origin` and any
`Sec-WebSocket-*` name, cannot be set.  Use the #Origin and #Protocols fields instead.

-ERRORS-
Okay
NullArgs
InvalidValue: The name is not a valid header name, the value contains a control character, or the header is reserved.
-END-
*********************************************************************************************************************/

static ERR WEBSOCKET_SetKey(extWebSocket *Self, struct acSetKey *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if (not ws::is_token(Args->Key)) return log.warning(ERR::InvalidValue);

   for (auto ch : Args->Value) {
      if (((uint8_t(ch) < 0x20) and (ch != '\t')) or (uint8_t(ch) IS 0x7f)) return log.warning(ERR::InvalidValue);
   }

   constexpr std::array<std::string_view, 7> reserved = {
      "Host", "Connection", "Upgrade", "Origin", "Content-Length", "Transfer-Encoding", "Proxy-Authorization"
   };
   for (auto name : reserved) {
      if (kt::iequals(Args->Key, name)) return log.warning(ERR::InvalidValue);
   }
   if ((Args->Key.size() >= 14) and kt::iequals(Args->Key.substr(0, 14), "Sec-WebSocket-")) {
      return log.warning(ERR::InvalidValue);
   }

   for (auto &header : Self->Headers) {
      if (kt::iequals(header.first, Args->Key)) {
         header.second.assign(Args->Value);
         return ERR::Okay;
      }
   }

   Self->Headers.emplace_back(std::string(Args->Key), std::string(Args->Value));
   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_websocket_fields.cpp"
#include "class_websocket_def.c"

static const FieldArray clWebSocketFields[] = {
   { "MaxMessageSize", FDF_INT64|FDF_RW, nullptr, SET_MaxMessageSize },
   { "MaxFrameSize",   FDF_INT64|FDF_RW, nullptr, SET_MaxFrameSize },
   { "SendLimit",      FDF_INT64|FDF_RW, nullptr, SET_SendLimit },
   { "ConnectTimeout", FDF_DOUBLE|FDF_RW },
   { "DataTimeout",    FDF_DOUBLE|FDF_RW },
   { "PingInterval",   FDF_DOUBLE|FDF_RW, nullptr, SET_PingInterval },
   { "CloseTimeout",   FDF_DOUBLE|FDF_RW, nullptr, SET_CloseTimeout },
   { "ClientData",     FDF_POINTER|FDF_RW },
   { "Location",       FDF_CPPSTRING|FDF_RW, nullptr, SET_Location },
   { "Protocols",      FDF_CPPSTRING|FDF_RW, nullptr, SET_Protocols },
   { "Protocol",       FDF_CPPSTRING|FDF_R },
   { "Origin",         FDF_CPPSTRING|FDF_RW, nullptr, SET_Origin },
   { "ProxyServer",    FDF_CPPSTRING|FDF_RW },
   { "CloseReason",    FDF_CPPSTRING|FDF_R },
   { "Redirect",       FDF_CPPSTRING|FDF_R },
   { "ProxyPort",      FDF_INT|FDF_RW },
   { "Flags",          FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clWebSocketFlags },
   { "State",          FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clWebSocketState },
   { "Error",          FDF_ERROR|FDF_R },
   { "CloseCode",      FDF_INT|FDF_R },
   { "Status",         FDF_INT|FDF_R },
   // Virtual fields
   { "Heartbeat",      FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_Heartbeat, SET_Heartbeat },
   { "Incoming",       FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_Incoming, SET_Incoming },
   { "Outgoing",       FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_Outgoing, SET_Outgoing },
   { "Pending",        FDF_VIRTUAL|FDF_INT64|FDF_R, GET_Pending },
   { "StateChanged",   FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_StateChanged, SET_StateChanged },
   END_FIELD
};

//********************************************************************************************************************

static ERR add_websocket_class(void)
{
   clWebSocket = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::WEBSOCKET),
      fl::ClassVersion(VER_WEBSOCKET),
      fl::Name("WebSocket"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clWebSocketActions),
      fl::Methods(clWebSocketMethods),
      fl::Fields(clWebSocketFields),
      fl::Size(sizeof(extWebSocket)),
      fl::Path(MOD_PATH));

   return clWebSocket ? ERR::Okay : ERR::AddClass;
}
