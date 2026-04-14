/*********************************************************************************************************************

-FIELD-
Address: An IP address or domain name to connect to.

If this field is set with an IP address or domain name prior to initialisation, an attempt to connect to that location
will be made when the NetSocket is initialised.  Post-initialisation this field cannot be set by the client, however
calls to #Connect() will result in it being updated so that it always reflects the named address of the current
connection.

*********************************************************************************************************************/

static ERR SET_Address(extNetSocket *Self, CSTRING Value)
{
   if (Self->Address) { FreeResource(Self->Address); Self->Address = nullptr; }
   if (Value) Self->Address = pf::strclone(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Backlog: The maximum number of connections that can be queued against the socket.

Incoming connections to NetSocket objects are queued until they are answered by the object.  Setting the Backlog
adjusts the maximum number of connections on the queue, which otherwise defaults to 10.

If the backlog is exceeded, subsequent connections to the socket should expect a connection refused error.

-FIELD-
ClientData: A client-defined value that can be useful in action notify events.

This is a free-entry field value that can store client data for future reference.

-FIELD-
ClientLimit: The maximum number of clients (unique IP addresses) that can be connected to a server socket.

The ClientLimit value limits the maximum number of IP addresses that can be connected to the socket at any one time.
For socket limits per client, see the #SocketLimit field.

-FIELD-
Clients: For server sockets, lists all clients connected to the server.

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

In server mode, the function must follow the prototype `Function(*NetSocket, *ClientSocket, NTC State)`.  Otherwise
`Function(*NetSocket, NTC State)`.

The `NetSocket` parameter refers to the NetSocket object to which the function is subscribed.  In server mode,
`ClientSocket` refers to the @ClientSocket on which the state has changed.

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

The `NetSocket` parameter refers to the NetSocket object.  `Meta` is optional userdata from the `FUNCTION`.

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
In client mode the function must be in the format `ERR Outgoing(*NetSocket, APTR Meta)`.  In server mode the function
format is `ERR Outgoing(*NetSocket, *ClientSocket, APTR Meta)`.

To send data to the NetSocket object, call the #Write() action.  If the callback function returns an error other
than `ERR::Okay` then the Outgoing field will be cleared and the function will no longer be called.

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
   pf::Log log;

   if (Self->Outgoing.isScript()) UnsubscribeAction(Self->Outgoing.Context, AC::Free);
   Self->Outgoing = *Value;
   if (Self->Outgoing.isScript()) SubscribeAction(Self->Outgoing.Context, AC::Free, C_FUNCTION(notify_free_outgoing));

   if (Self->initialised()) {
      if ((Self->Handle.is_valid()) and (Self->State IS NTC::CONNECTED)) {
         // Setting the Outgoing field after connectivity is established will put the socket into streamed write mode.

         #ifdef __linux__
            RegisterFD(Self->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, &netsocket_outgoing, Self);
         #elif _WIN32
            win_socketstate(Self->Handle, std::nullopt, true);
         #endif
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

   Self->Handle = SocketHandle(int((MAXINT)Value));
   Self->ExternalSocket = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SSLCertificate: SSL certificate file to use if in server mode.

Set SSLCertificate to the path of an SSL certificate file to use when the NetSocket is in server mode.  The
certificate file must be in a supported format such as PEM, CRT, or P12.  If no certificate is defined, the
NetSocket will either self-sign or use a localhost certificate, if available.

*********************************************************************************************************************/

static ERR SET_SSLCertificate(extNetSocket *Self, CSTRING Value)
{
   if (Self->SSLCertificate) { FreeResource(Self->SSLCertificate); Self->SSLCertificate = nullptr; }

   if ((Value) and (*Value)) {
      pf::Log log(__FUNCTION__);

      LOC type;
      if ((AnalysePath(Value, &type) IS ERR::Okay) and (type IS LOC::FILE)) {
         // Check file extension for supported formats
         std::string path(Value);
         std::string ext = path.substr(path.find_last_of(".") + 1);
         std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

         if ((ext IS "pem") or (ext IS "crt") or (ext IS "cert") or (ext IS "p12") or (ext IS "pfx")) {
            Self->SSLCertificate = pf::strclone(Value);
         }
         else return log.warning(ERR::InvalidData);
      }
      else return log.warning(ERR::FileNotFound);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SSLPrivateKey: Private key file to use if in server mode.

Set SSLPrivateKey to the path of an SSL private key file to use when the NetSocket is in server mode.  The
private key file must be in a supported format such as PEM or KEY.  If no private key is defined, the NetSocket
will either self-sign or use a localhost private key, if available.

*********************************************************************************************************************/

static ERR SET_SSLPrivateKey(extNetSocket *Self, CSTRING Value)
{
   if (Self->SSLPrivateKey) { FreeResource(Self->SSLPrivateKey); Self->SSLPrivateKey = nullptr; }

   if ((Value) and (*Value)) {
      pf::Log log(__FUNCTION__);

      LOC type;
      if ((AnalysePath(Value, &type) IS ERR::Okay) and (type IS LOC::FILE)) {
         // Check file extension for supported formats
         std::string path(Value);
         std::string ext = path.substr(path.find_last_of(".") + 1);
         std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
         if ((ext IS "pem") or (ext IS "key")) {
            Self->SSLPrivateKey = pf::strclone(Value);
         }
         else return log.warning(ERR::InvalidData);
      }
      else return log.warning(ERR::FileNotFound);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SSLKeyPassword: SSL private key password.

If the SSL private key is encrypted, set this field to the password required to decrypt it.  If the private key is
not encrypted, this field can be left empty.

*********************************************************************************************************************/

static ERR SET_SSLKeyPassword(extNetSocket *Self, CSTRING Value)
{
   if (Self->SSLKeyPassword) { FreeResource(Self->SSLKeyPassword); Self->SSLKeyPassword = nullptr; }
   if ((Value) and (*Value)) Self->SSLKeyPassword = pf::strclone(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
State: The current connection state of the NetSocket object.

The State reflects the connection state of the NetSocket.  If the #Feedback field is defined with a function, it will
be called automatically whenever the state is changed.  Note that the ClientSocket parameter will be NULL when the
Feedback function is called.

Note that in server mode this State value should not be used as it cannot reflect the state of all connected
client sockets.  Each @ClientSocket carries its own independent State value for use instead.

*********************************************************************************************************************/

static ERR GET_State(extNetSocket *Self, NTC &Value)
{
   if ((Self->Flags & NSF::SERVER) != NSF::NIL) {
      pf::Log().warning("Reading the State of a server socket is a probable defect.");
      Value = NTC::MULTISTATE;
   }
   else Value = Self->State;
   return ERR::Okay;
}

static ERR SET_State(extNetSocket *Self, NTC Value)
{
   pf::Log log;

   if ((Self->Flags & NSF::SERVER) != NSF::NIL) {
      return log.warning(ERR::Immutable);
   }

   if (Value != Self->State) {
      log.branch("State changed from %s to %s", netsocket_state(Self->State), netsocket_state(Value));

      #ifndef DISABLE_SSL
      if ((Self->State IS NTC::HANDSHAKING) and (Value IS NTC::CONNECTED)) {
         // SSL connection has just been established

         bool ssl_valid = true;

         #ifdef _WIN32
         if ((Self->SSLHandle) and ((Self->Flags & NSF::SERVER) IS NSF::NIL)) {
            // Only perform certificate validation if DISABLE_SERVER_VERIFY flag is not set
            if ((Self->Flags & NSF::DISABLE_SERVER_VERIFY) != NSF::NIL) {
               log.trace("SSL certificate validation skipped.");
            }
            else ssl_valid = ssl_get_verify_result(Self->SSLHandle);
         }
         #else
         if (Self->SSLHandle) {
            // Only perform certificate validation if DISABLE_SERVER_VERIFY flag is not set
            if ((Self->Flags & NSF::DISABLE_SERVER_VERIFY) != NSF::NIL) {
               log.trace("SSL certificate validation skipped.");
            }
            else {
               if (SSL_get_verify_result(Self->SSLHandle) != X509_V_OK) ssl_valid = false;
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
                  pf::SwitchContext context(Self->Feedback.Context);
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
            pf::SwitchContext context(Self->Feedback.Context);
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
         #ifdef __linux__
            RegisterFD(Self->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, &netsocket_outgoing, Self);
         #elif _WIN32
            win_socketstate(Self->Handle, std::nullopt, true);
         #endif
      }
   }

   SetResourcePtr(RES::EXCEPTION_HANDLER, nullptr); // Stop winsock from fooling with the Core exception handler

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TotalClients: Indicates the total number of clients currently connected to the socket (if in server mode).

In server mode, the NetSocket will maintain a count of the total number of clients currently connected to the socket.
You can read the total number of connections from this field.

In client mode, this field is always set to zero.

*********************************************************************************************************************/
