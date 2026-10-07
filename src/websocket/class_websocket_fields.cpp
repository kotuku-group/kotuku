/*********************************************************************************************************************

-FIELD-
ClientData: Free for user data storage.

-FIELD-
CloseCode: The received or generated close status code.

CloseCode is set before the #State changes to `CLOSED`.  It holds the code from the server's Close frame, the code sent
when the connection was failed for a protocol error, or `ABNORMAL` (1006) if the connection was lost, the handshake
failed, or the closing handshake did not complete.  A Close frame without a status is reported as `NO_STATUS` (1005).
The codes defined by RFC 6455 are available as the `WSC` constants:

!WSC

Codes from 3000 to 4999 are reserved for libraries, frameworks and applications.

-FIELD-
CloseReason: The received or generated close reason.

CloseReason is set alongside #CloseCode.  It holds the reason from the server's Close frame, a description of a
protocol failure, or a description of a failed handshake.  If the server responded with a status other than `101`,
the reason takes the form `HTTP 403`.

-FIELD-
CloseTimeout: The time allowed for the closing handshake, in seconds.

After a Close frame has been sent, the connection waits up to CloseTimeout seconds for the peer's Close frame and for
queued data to be written.  The connection is then dropped and, if the peer's Close frame was not received, #CloseCode
is set to `ABNORMAL` and #Error to `TimeOut`.  The default is 5 seconds.  Zero waits indefinitely, which is not
recommended.  Negative values are invalid.

*********************************************************************************************************************/

static ERR SET_CloseTimeout(extWebSocket *Self, double Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->CloseTimeout = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ConnectTimeout: The time allowed to connect to the server, in seconds.

The timeout applies to the opening handshake only.  The default is 10 seconds.

-FIELD-
DataTimeout: The time allowed for the server's handshake response, in seconds.

The timeout applies to the opening handshake only.  The default is 5 seconds.

-FIELD-
Error: The last error.

When the handshake fails, Error indicates the cause.  `InvalidHTTPResponse` means that the server's `101` response did
not satisfy RFC 6455, `HTTPStatus` means that the server answered with another status (see #Status and #Redirect),
and `NotAuthorised` means that the server answered `401`.  Network failures are reported with the corresponding
network error.  `InvalidData` indicates that the connection was failed because the server broke the protocol,
`Disconnected` that the connection was lost before the closing handshake completed, and `TimeOut` that the peer did not
answer a keep-alive ping or did not complete the closing handshake within #CloseTimeout.

-FIELD-
Flags: Optional flags.

Set `STREAM_MESSAGES` to receive each message fragment through #Incoming as it arrives instead of a reassembled
message.  `NO_UTF8_CHECK` disables UTF-8 validation of `TEXT` messages in both directions, and should only be used
to communicate with peers that are known to send invalid text.  `DISABLE_SERVER_VERIFY` accepts a `wss://` server
whose TLS certificate cannot be verified, which is intended for development only.  `NO_AUTO_PONG` stops received Ping
frames from being answered, which is intended for testing peers' keep-alive handling.  The `SSL` and `EXTERNAL_LISTENER` flags apply to
@WebSocketServer only; `EXTERNAL_LISTENER` is invalid on WebSocket.

Flags that affect the opening handshake must be set before #Activate() is called.

-FIELD-
Heartbeat: Receives Ping and Pong frames from the peer.

The C++ prototype is `void Function(*WebSocket, const void *Data, int Length, bool Pong, APTR Meta)`.  Tiri callbacks
receive `(WebSocket, Buffer, Pong)`, in which `Pong` is `1` for a Pong frame and `0` for a Ping frame.  Test it with
`Pong is 1`, because `0` is a true value in Tiri.  The payload is at most 125 bytes and is valid only for the duration
of the call.

Pings are answered automatically before the callback is invoked, unless the `NO_AUTO_PONG` flag is set.  Pongs answer
#Ping() calls and keep-alive pings, although a peer may also send them unsolicited.

*********************************************************************************************************************/

static ERR GET_Heartbeat(extWebSocket *Self, FUNCTION * &Value)
{
   if (Self->Heartbeat.defined()) {
      Value = &Self->Heartbeat;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Heartbeat(extWebSocket *Self, FUNCTION *Value)
{
   clear_callback(Self->Heartbeat);
   if (Value) {
      Self->Heartbeat = *Value;
      Self->Heartbeat.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Incoming: Receives messages from the peer.

The Incoming callback receives each message from the peer.  The C++ prototype is
`ERR Function(*WebSocket, const void *Data, int Length, WSM Type, bool Final, APTR Meta)`.  Tiri callbacks receive
`(WebSocket, Buffer, Type, Final)`, in which `Buffer` is a byte buffer that can be read with `Buffer.getString()` and
`Final` is `1` or `0`.  Note that `0` is a true value in Tiri, so test it with `Final is 1`.

`Type` is `WSM::TEXT` or `WSM::BINARY`.  Text messages are validated as UTF-8 unless the `NO_UTF8_CHECK` flag is set.
`Final` is always set unless the `STREAM_MESSAGES` flag is enabled, in which case each fragment is delivered as it
arrives and `Final` marks the last fragment of a message.

The buffer is valid only for the duration of the call.  Copy the data, for example with `Buffer.getString()`, if it
is needed afterwards.

The callback's result is logged if it is an error and otherwise ignored.  To stop receiving messages, call #Close() or
free the WebSocket from within the callback.

*********************************************************************************************************************/

static ERR GET_Incoming(extWebSocket *Self, FUNCTION * &Value)
{
   if (Self->Incoming.defined()) {
      Value = &Self->Incoming;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Incoming(extWebSocket *Self, FUNCTION *Value)
{
   clear_callback(Self->Incoming);
   if (Value) {
      Self->Incoming = *Value;
      Self->Incoming.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Location: The `ws://` or `wss://` URL to connect to.

The Location identifies the server and resource for the connection.  The scheme selects plain TCP (`ws://`) or TLS
(`wss://`) with default ports of 80 and 443 respectively.  An explicit port, a bracketed IPv6 address and a query string
are supported, and the path and query are sent verbatim.  Characters outside printable ASCII must be percent-encoded.
Fragments (`#`) and user information are not permitted.

The Location cannot be changed while the connection is open.  For a connection accepted by a @WebSocketServer,
Location reports the URL requested by the client, built from its `Host` header and request target.

-ERRORS-
Okay
InvalidValue: The URL is not a valid WebSocket URL.
InUse: The WebSocket is not closed.
-END-

*********************************************************************************************************************/

static ERR SET_Location(extWebSocket *Self, const std::string_view &Value)
{
   if (Self->State != WSS::CLOSED) return ERR::InUse;

   ws::Location target;
   if (not ws::parse_location(Value, target)) return kt::Log().warning(ERR::InvalidValue);

   Self->Target = std::move(target);
   Self->Location.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxFrameSize: The maximum size of a single received frame, in bytes.

A frame larger than this limit fails the connection with close code `MESSAGE_TOO_BIG` before its payload is buffered.
Zero, the default, applies #MaxMessageSize.  Negative values are invalid.

*********************************************************************************************************************/

static ERR SET_MaxFrameSize(extWebSocket *Self, int64_t Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->MaxFrameSize = Value;

   const auto frame_limit = uint64_t(Value ? Value : Self->MaxMessageSize);
   if (Self->Reader) Self->Reader->set_max_frame_size(frame_limit);
   if (Self->Queue) Self->Queue->set_fragment_size(frame_limit);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxMessageSize: The maximum size of a received message, in bytes.

A message whose frames exceed this limit in total fails the connection with close code `MESSAGE_TOO_BIG`.  The default
is 16 MiB.  Zero removes the limit, which is not recommended for connections to untrusted servers.  Negative values are
invalid.

*********************************************************************************************************************/

static ERR SET_MaxMessageSize(extWebSocket *Self, int64_t Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->MaxMessageSize = Value;

   if (Self->Reader) {
      Self->Reader->set_max_message_size(uint64_t(Value));
      if (not Self->MaxFrameSize) Self->Reader->set_max_frame_size(uint64_t(Value));
   }
   if (Self->Queue and (not Self->MaxFrameSize)) Self->Queue->set_fragment_size(uint64_t(Value));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Outgoing: Reports that the send queue has been emptied.

The C++ prototype is `void Function(*WebSocket, APTR Meta)`.  Tiri callbacks receive `(WebSocket)`.

Outgoing is invoked when #Pending drops to zero after a previous #Send() left data in the queue, or after a #Send()
was rejected with `BufferOverflow`.  A producer that transfers more data than #SendLimit permits can send until
`BufferOverflow` is returned and resume from this callback.  It is not invoked for messages that were written in full
by the #Send() call that queued them, and it is not invoked once the closing handshake has started.

*********************************************************************************************************************/

static ERR GET_Outgoing(extWebSocket *Self, FUNCTION * &Value)
{
   if (Self->Outgoing.defined()) {
      Value = &Self->Outgoing;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Outgoing(extWebSocket *Self, FUNCTION *Value)
{
   clear_callback(Self->Outgoing);
   if (Value) {
      Self->Outgoing = *Value;
      Self->Outgoing.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Pending: The number of queued bytes awaiting transmission.

Pending counts the encoded frames held by the WebSocket's send queue, including frame headers.  Bytes that have been
passed to the network layer are not included.

*********************************************************************************************************************/

static ERR GET_Pending(extWebSocket *Self, int64_t *Value)
{
   *Value = Self->Queue ? int64_t(Self->Queue->pending()) : 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PingInterval: Enables keep-alive pings after a period of inactivity, in seconds.

When PingInterval is greater than zero, a Ping frame is sent once no data has been received from the server for that
many seconds.  If nothing is received within a further interval, the connection is dropped with #CloseCode set to
`ABNORMAL` and #Error set to `TimeOut`.  Any received data counts as activity, so a busy connection is never pinged.

The default is zero, which disables keep-alive.  The interval can be changed while the connection is open.  Negative
values are invalid.

*********************************************************************************************************************/

static ERR SET_PingInterval(extWebSocket *Self, double Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->PingInterval = Value;

   if (Self->State IS WSS::OPEN) {
      if (Value > 0) set_timer(Self, Self->PingTimer, Value, ping_timer);
      else cancel_timer(Self->PingTimer);
      Self->PingOutstanding = false;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Origin: An optional `Origin` request header.

Browsers send the origin of the page that opens a connection, and some servers use it to restrict access.  Leave this
field empty to omit the header.  For a connection accepted by a @WebSocketServer, Origin reports the client's `Origin`
header, if any.

*********************************************************************************************************************/

static ERR SET_Origin(extWebSocket *Self, const std::string_view &Value)
{
   for (auto ch : Value) {
      if (((uint8_t(ch) < 0x20) and (ch != '\t')) or (uint8_t(ch) IS 0x7f)) return ERR::InvalidValue;
   }
   Self->Origin.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Protocol: The subprotocol selected by the server.

Protocol is set when the handshake succeeds.  It is empty if no subprotocol was requested or the server did not select
one.  A server that selects a subprotocol that was not requested fails the handshake.  For a connection accepted by a
@WebSocketServer, Protocol is the subprotocol that the server selected from its @WebSocketServer.Protocols.

-FIELD-
Protocols: Comma-separated subprotocols to request, in order of preference.

Each subprotocol must be an HTTP token, for example `chat, superchat`.  The server may select one of them, which is
then reported in #Protocol.

-ERRORS-
Okay
InvalidValue: An element of the list is not a valid token.
-END-

*********************************************************************************************************************/

static ERR SET_Protocols(extWebSocket *Self, const std::string_view &Value)
{
   std::vector<std::string_view> tokens;
   if (not ws::parse_token_list(Value, tokens)) return kt::Log().warning(ERR::InvalidValue);

   Self->RequestedProtocols.assign(tokens.begin(), tokens.end());
   Self->Protocols.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ProxyPort: The port of the #ProxyServer.

ProxyPort must be set whenever #ProxyServer is set.  It is ignored otherwise.

-FIELD-
ProxyServer: An HTTP proxy server for the connection.

When set, the connection is tunnelled through the proxy with an HTTP `CONNECT` request.  Proxy authentication is not
supported; a proxy that answers `407` fails the handshake.  If this field is empty, the HTTP class's system proxy
settings apply.

-FIELD-
Redirect: The target of a redirecting handshake response.

If the server answers the handshake with a `3xx` status, the handshake fails with `HTTPStatus` and the response's
`Location` header is stored here.  Redirects are never followed automatically, so that the client can decide whether
the new destination is acceptable.

-FIELD-
SendLimit: The maximum number of queued bytes awaiting transmission.

SendLimit bounds the memory used by the send queue when the peer reads more slowly than messages are sent.  A #Send()
or #Ping() that would take #Pending beyond this limit fails with `BufferOverflow`.  Pong and Close frames are exempt.
The default is 16 MiB.  Zero removes the limit, which is not recommended.  Negative values are invalid.  A change made
while connected applies to subsequent calls and does not affect bytes already queued.

*********************************************************************************************************************/

static ERR SET_SendLimit(extWebSocket *Self, int64_t Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->SendLimit = Value;
   if (Self->Queue) Self->Queue->set_limit(uint64_t(Value));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
State: The current connection state.

A new WebSocket is `CLOSED`.  #Activate() changes the State to `CONNECTING`, a successful handshake to `OPEN`, and the
start of the closing handshake to `CLOSING`.  The State returns to `CLOSED` when the connection ends or the handshake
fails, after which the object can be activated again.  Every change is reported to #StateChanged.

-FIELD-
StateChanged: Reports changes to the #State.

The C++ prototype is `void Function(*WebSocket, WSS State, APTR Meta)`.  Tiri callbacks receive `(WebSocket, State)`.
The #CloseCode, #CloseReason and #Error fields are set before a `CLOSED` notification.  The callback may free the
WebSocket.

*********************************************************************************************************************/

static ERR GET_StateChanged(extWebSocket *Self, FUNCTION * &Value)
{
   if (Self->StateChanged.defined()) {
      Value = &Self->StateChanged;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_StateChanged(extWebSocket *Self, FUNCTION *Value)
{
   clear_callback(Self->StateChanged);
   if (Value) {
      Self->StateChanged = *Value;
      Self->StateChanged.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Status: The HTTP status of the handshake response.

Status is `101` after a successful handshake.  If the server rejected the handshake, it holds the response status, for
example `403`.  It is zero if no response was received.
-END-

*********************************************************************************************************************/
