/*********************************************************************************************************************

-FIELD-
Address: An IP address or domain name to connect to.

For NetSocket clients, if this field is set with an IP address or domain name prior to initialisation, an attempt to
connect to that location will be made when the object is initialised.  Post-initialisation this field cannot be set by
the client, however calls to #Connect() will result in it being updated so that it always reflects the named address of
the current connection.

For @NetServer listeners, this inherited field identifies the local address to bind before initialisation.  Use
`localhost`, `*`, an IPv4 address or an IPv6 address.

*********************************************************************************************************************/

static ERR SET_Address(extNetSocket *Self, CSTRING Value)
{
   if (Self->Address) { FreeResource(Self->Address); Self->Address = nullptr; }
   if (Value) Self->Address = kt::strclone(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ClientData: A client-defined value that can be useful in action notify events.

This is a free-entry field value that can store client data for future reference.

-FIELD-
Error: Information about the last error that occurred during a NetSocket operation

This field describes the last error that occurred during a NetSocket operation:

In the case where a NetSocket object enters the `NTC::DISCONNECTED` state from the `NTC::CONNECTED` state, this field
can be used to determine how a TCP connection was closed.

<types type="Error">
<type name="ERR::Okay">The connection was closed gracefully.  All data sent by the peer has been received.</>
<type name="ERR::Disconnected">The connection was broken in a non-graceful fashion. Data may be lost.</>
<type name="ERR::TimeOut">The connect operation timed out.</>
<type name="ERR::ConnectionRefused">The connection was refused by the remote host.  Note: This error will not occur on Windows, and instead the Error field will be set to `ERR::Failed`.</>
<type name="ERR::NetworkUnreachable">The network was unreachable.  Note: This error will not occur on Windows, and instead the Error field will be set to `ERR::Failed`.</>
<type name="ERR::HostUnreachable">No path to host was found.  Note: This error will not occur on Windows, and instead the Error field will be set to `ERR::Failed`.</>
<type name="ERR::Failed">An unspecified error occurred.</>
</>

-FIELD-
Feedback: A callback trigger for when the state of the NetSocket is changed.

The client can define a function in this field to receive notifications whenever the state of the socket changes -
typically connection messages.

For NetSocket clients, the function must follow the prototype `Function(*NetSocket, NTC State)`.  For @NetServer
listeners, the inherited field uses `Function(*NetServer, *ClientSocket, NTC State)`.

The first parameter refers to the object to which the function is subscribed.  For NetServer listeners, `ClientSocket`
refers to the @ClientSocket on which the state has changed.

*********************************************************************************************************************/

static ERR GET_Feedback(extNetSocket *Self, FUNCTION **Value)
{
   if (Self->Feedback.defined()) {
      *Value = &Self->Feedback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Feedback(extNetSocket *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->Feedback.isScript()) UnsubscribeAction(Self->Feedback.Context, AC::Free);
      Self->Feedback = *Value;
      if (Self->Feedback.isScript()) {
         SubscribeAction(Self->Feedback.Context, AC::Free, C_FUNCTION(notify_free_feedback));
      }
   }
   else Self->Feedback.clear();

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags.

-FIELD-
Incoming: Callback that is triggered when the socket receives data.

The Incoming field can be set with a custom function that will be called whenever the socket receives data.  The
function prototype for C++ is `ERR Incoming(*NetSocket, APTR Meta)`.  For Tiri use `function Incoming(NetSocket)`.

The `NetSocket` parameter refers to the NetSocket object.  `Meta` is optional userdata from the `FUNCTION`.  For
@NetServer listeners, this inherited field receives `Incoming(*NetServer, *ClientSocket, APTR Meta)` in C++ or
`function Incoming(NetServer, ClientSocket)` in Tiri, and data must be read from the supplied @ClientSocket.

Retrieve data from the socket with the #Read() action. Reading at least some of the data from the socket is
compulsory - if the function does not do this then the data will be cleared from the socket when the function returns.
If the callback function returns/raises `ERR::Terminate` then the Incoming field will be cleared and the function
will no longer be called.  All other error codes are ignored.

*********************************************************************************************************************/

static ERR GET_Incoming(extNetSocket *Self, FUNCTION **Value)
{
   if (Self->Incoming.defined()) {
      *Value = &Self->Incoming;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Incoming(extNetSocket *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->Incoming.isScript()) UnsubscribeAction(Self->Incoming.Context, AC::Free);
      Self->Incoming = *Value;
      if (Self->Incoming.isScript()) {
         SubscribeAction(Self->Incoming.Context, AC::Free, C_FUNCTION(notify_free_incoming));
      }
   }
   else Self->Incoming.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxPacketSize: Maximum UDP packet size for sending and receiving data.

This field sets the maximum size in bytes for UDP packets when sending or receiving data.  It only applies to UDP
sockets and is ignored for TCP connections.  The default value is 65507 bytes, which is the maximum payload size
for UDP packets (65535 - 8 bytes UDP header - 20 bytes IP header).

If you attempt to send a packet larger than MaxPacketSize, a warning will be logged and the operation may fail.
When receiving data, packets larger than this size will be truncated.

-FIELD-
MsgLimit: Limits the size of incoming and outgoing data packets.

This field limits the size of incoming and outgoing message queues (each socket connection receives two queues assigned
to both incoming and outgoing messages).  The size is defined in bytes.  Sending or receiving messages that overflow
the queue results in the connection being terminated with an error.

The default setting is 1 megabyte.

-FIELD-
MulticastTTL: Time-to-live (hop limit) for multicast packets.

This field sets the time-to-live (TTL) value for multicast packets sent from UDP sockets.  The TTL determines how
many network hops (routers) a multicast packet can traverse before being discarded.  This helps prevent multicast
traffic from flooding the network indefinitely.

The default TTL is 1, which restricts multicast to the local network segment.  Higher values allow multicast packets
to traverse more network boundaries:

<list type="bullet">
<li>1: Local network segment only</li>
<li>32: Within the local site</li>
<li>64: Within the local region</li>
<li>128: Within the local continent</li>
<li>255: Unrestricted (global)</li>
</list>

-FIELD-
Outgoing: Callback that is triggered when a socket is ready to send data.

The Outgoing field can be set with a custom function that will be called whenever the socket is ready to send data.
For NetSocket clients the function must be in the format `ERR Outgoing(*NetSocket, APTR Meta)`.  For @NetServer
listeners, the inherited field uses `ERR Outgoing(*NetServer, *ClientSocket, APTR Meta)`.

To send data from a NetSocket object, call the #Write() action.  To send data from a NetServer to a connected client,
call @ClientSocket.Write() on the target client socket.  If the callback function returns an error other than
`ERR::Okay` then the Outgoing field will be cleared and the function will no longer be called.

*********************************************************************************************************************/

static ERR GET_Outgoing(extNetSocket *Self, FUNCTION **Value)
{
   if (Self->Outgoing.defined()) {
      *Value = &Self->Outgoing;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Outgoing(extNetSocket *Self, FUNCTION *Value)
{
   kt::Log log;

   if (Self->Outgoing.isScript()) UnsubscribeAction(Self->Outgoing.Context, AC::Free);
   Self->Outgoing = *Value;
   if (Self->Outgoing.isScript()) SubscribeAction(Self->Outgoing.Context, AC::Free, C_FUNCTION(notify_free_outgoing));

   if (Self->initialised()) {
      if ((Self->Handle.is_valid()) and (Self->State IS NTC::CONNECTED)) {
         // Setting the Outgoing field after connectivity is established will put the socket into streamed write mode.

         network_platform().register_write(Self->Handle, &netsocket_outgoing, Self);
      }
      else log.trace("Will not listen for socket-writes (no socket handle, or state %d != NTC::CONNECTED).", Self->State);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutQueueSize: The number of bytes on the socket's outgoing queue.

*********************************************************************************************************************/

static ERR GET_OutQueueSize(extNetSocket *Self, int *Value)
{
   *Value = Self->WriteQueue.Buffer.size();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Port: The port number to use for connections.

For NetSocket clients, this is the remote port used by #Connect().  For @NetServer listeners, this inherited field is
the local port to bind during initialisation.

-FIELD-
Handle: Platform specific reference to the network socket handle.

*********************************************************************************************************************/

static ERR GET_Handle(extNetSocket *Self, APTR *Value)
{
   *Value = (APTR)(MAXINT)Self->Handle.socket();
   return ERR::Okay;
}

static ERR SET_Handle(extNetSocket *Self, APTR Value)
{
   // The user can set Handle prior to initialisation in order to create a NetSocket object that is linked to a
   // socket created from outside the core platform code base.

   Self->Handle = SocketHandle(SOCKET_HANDLE((MAXINT)Value));
   Self->ExternalSocket = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
State: The current connection state of the NetSocket object.

The State reflects the connection state of the NetSocket.  If the #Feedback field is defined with a function, it will
be called automatically whenever the state is changed.  Note that the ClientSocket parameter will be NULL when the
Feedback function is called.

For @NetServer listeners this State value should not be used as it cannot reflect the state of all connected client
sockets.  When read from a NetServer, this inherited field reports `NTC::MULTISTATE`; each @ClientSocket carries its
own independent State value for accepted TCP connections.

*********************************************************************************************************************/

static ERR GET_State(extNetSocket *Self, NTC &Value)
{
   if (Self->classID() IS CLASSID::NETSERVER) {
      kt::Log().warning("Reading the State of a NetServer socket is a probable defect.");
      Value = NTC::MULTISTATE;
   }
   else Value = Self->State;
   return ERR::Okay;
}

static ERR SET_State(extNetSocket *Self, NTC Value)
{
   kt::Log log;

   if (Self->classID() IS CLASSID::NETSERVER) {
      return log.warning(ERR::Immutable);
   }

   if (Value != Self->State) {
      log.branch("State changed from %s to %s", netsocket_state(Self->State), netsocket_state(Value));

      #ifndef DISABLE_SSL
      if ((Self->State IS NTC::HANDSHAKING) and (Value IS NTC::CONNECTED)) {
         // SSL connection has just been established

         bool ssl_valid = true;

         #ifdef _WIN32
         if ((Self->TLS.Handle) and (Self->classID() != CLASSID::NETSERVER)) {
            // Only perform certificate validation if DISABLE_SERVER_VERIFY flag is not set
            if ((Self->Flags & NSF::DISABLE_SERVER_VERIFY) != NSF::NIL) {
               log.trace("SSL certificate validation skipped.");
            }
            else ssl_valid = ssl_get_verify_result(Self->TLS.Handle);
         }
         #else
         if (Self->TLS.Handle) {
            // Only perform certificate validation if DISABLE_SERVER_VERIFY flag is not set
            if ((Self->Flags & NSF::DISABLE_SERVER_VERIFY) != NSF::NIL) {
               log.trace("SSL certificate validation skipped.");
            }
            else {
               if (SSL_get_verify_result(Self->TLS.Handle) != X509_V_OK) ssl_valid = false;
               else log.trace("SSL certificate validation successful.");
            }
         }
         #endif

         if (!ssl_valid) {
            log.warning("SSL certificate validation failed.");
            Self->Error = ERR::Security;
            Self->State = NTC::DISCONNECTED;
            if (Self->Feedback.defined()) {
               if (Self->Feedback.isC()) {
                  kt::SwitchContext context(Self->Feedback.Context);
                  auto routine = (void (*)(extNetSocket *, NTC, APTR))Self->Feedback.Routine;
                  if (routine) routine(Self, Self->State, Self->Feedback.Meta);
               }
               else if (Self->Feedback.isScript()) {
                  sc::Call(Self->Feedback, std::to_array<ScriptArg>({
                     { "NetSocket", Self, FD_OBJECTPTR },
                     { "State", int(Self->State) }
                  }));
               }
            }
            return ERR::Security;
         }
      }
      #endif

      Self->State = Value;

      if (Self->Feedback.defined()) {
         log.traceBranch("Reporting state change to subscriber, operation %d, context %p.", int(Self->State), Self->Feedback.Context);

         if (Self->Feedback.isC()) {
            kt::SwitchContext context(Self->Feedback.Context);
            auto routine = (void (*)(extNetSocket *, NTC, APTR))Self->Feedback.Routine;
            if (routine) routine(Self, Self->State, Self->Feedback.Meta);
         }
         else if (Self->Feedback.isScript()) {
            sc::Call(Self->Feedback, std::to_array<ScriptArg>({
               { "NetSocket", Self, FD_OBJECTPTR },
               { "State",     int(Self->State) }
            }));
         }
      }

      if ((Self->State IS NTC::CONNECTED) and ((!Self->WriteQueue.Buffer.empty()) or (Self->Outgoing.defined()))) {
         log.msg("Sending queued data to server on connection.");
         network_platform().register_write(Self->Handle, &netsocket_outgoing, Self);
      }
   }

   return ERR::Okay;
}
