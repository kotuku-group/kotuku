/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
NetSocket: Manages network connections via TCP/IP sockets.

The NetSocket class provides a simple way of managing outbound TCP/IP socket communications.  It can connect to
remote TCP servers, exchange UDP datagrams when the `UDP` flag is set, and optionally use SSL.  Inbound listening and
accepted client management are provided by the @NetServer subclass, with each accepted TCP connection represented by a
@ClientSocket.

The design of the NetSocket class caters to asynchronous (non-blocking) communication.  This is achieved primarily
through callback fields - connection alerts are managed by #Feedback, incoming data is received through #Incoming
and readiness for outgoing data is supported by #Outgoing.

<header>Outbound Connections</>

After a connection has been established, data may be written using any of the following methods:

<list type="bullet">
<li>Write directly to the socket with the #Write() action.</li>
<li>Subscribe to the socket by referring to a routine in the #Outgoing field.  The routine will be called to
initially fill the internal write buffer, thereafter it will be called whenever the buffer is empty.</li>
</list>

It is possible to write to a NetSocket object before the connection to a server is established.  Doing so will buffer
the data in the socket until the connection with the server has been initiated, at which point the data will be
immediately sent.

<header>Inbound Listeners</>

Use @NetServer when a program needs to bind a local port and accept inbound clients.  NetServer inherits common socket
fields from NetSocket, reports client state changes through #Feedback with a @ClientSocket parameter, and exposes
active clients through its `Clients` field.  Data for accepted TCP clients is read from and written to @ClientSocket
objects.

-END-

*********************************************************************************************************************/

// The MaxWriteLen cannot exceed the size of the network queue on the host platform, otherwise all send attempts will
// return 'could block' error codes.  Note that when using SSL, the write length is an SSL library imposition.

static size_t glMaxWriteLen = 16 * 1024;

//********************************************************************************************************************
// Prototypes for internal methods

static void free_socket(extNetSocket *);
static void free_client(extNetServer *, objNetClient *);
static CSTRING netsocket_state(NTC Value);
static bool unlink_client_socket(objNetClient *, objClientSocket *);

// Implementation functions that take HOSTHANDLE
static void netsocket_incoming_impl(HOSTHANDLE, extNetSocket *);
static void netsocket_outgoing_impl(HOSTHANDLE, extNetSocket *);
static void netsocket_connect_impl(HOSTHANDLE, extNetSocket *);
static void server_incoming_from_client_impl(HOSTHANDLE, extClientSocket *);
static void clientsocket_outgoing_impl(HOSTHANDLE, extClientSocket *);
static void server_incoming_from_client(HOSTHANDLE, APTR);

// Wrappers for RegisterFD
static void netsocket_incoming(HOSTHANDLE FD, APTR Data) {
   netsocket_incoming_impl(FD, (extNetSocket *)Data);
}

static void netsocket_outgoing(HOSTHANDLE FD, APTR Data) {
   netsocket_outgoing_impl(FD, (extNetSocket *)Data);
}

static void netsocket_connect(HOSTHANDLE FD, APTR Data) {
   netsocket_connect_impl(FD, (extNetSocket *)Data);
}

static void clientsocket_outgoing(HOSTHANDLE FD, APTR Data) {
   clientsocket_outgoing_impl(FD, (extClientSocket *)Data);
}

//********************************************************************************************************************

static void notify_free_feedback(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extNetSocket *)CurrentContext())->Feedback.clear();
}

static void notify_free_incoming(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extNetSocket *)CurrentContext())->Incoming.clear();
}

static void notify_free_outgoing(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extNetSocket *)CurrentContext())->Outgoing.clear();
}

/*********************************************************************************************************************

-METHOD-
Connect: Connects a NetSocket to an address.

This method initiates the connection process with a target IP address.  The address to connect to can be specified either
as a domain name, in which case the domain name is first resolved to an IP address, or the address can be specified in
standard IP notation.

This method is non-blocking.  It will return immediately and the connection will be resolved once the server responds
to the connection request or an error occurs.  Client code should subscribe to the #State field to respond to changes to
the connection state.

Pre-Condition: Must be in a connection state of `NTC::DISCONNECTED`

Post-Condition: If this method returns `ERR::Okay`, will be in state `NTC::CONNECTING`.

-INPUT-
strview Address: String containing either a domain name (e.g. `www.google.com`) or an IP address (e.g. `123.123.123.123`)
int Port: Remote port to connect to.
double Timeout: Connection timeout in seconds (0 = no timeout).

-ERRORS-
Okay: The NetSocket connecting process was successfully started.
Args: Address was NULL, or Port was not in the required range.
InvalidState: The NetSocket was not in the state `NTC::DISCONNECTED` or the object is a @NetServer.
HostNotFound: Host name resolution failed.
TimeOut: Connection attempt timed out.
Failed: The connect failed for some other reason.

-TAGS-
non-blocking, mutates-object, copies-input
-END-

*********************************************************************************************************************/

static void connect_name_resolved_nl(objNetLookup *, ERR, std::string_view, const std::vector<IPAddress> &);
static void connect_name_resolved(extNetSocket *, ERR, std::string_view, const std::vector<IPAddress> &);
static ERR connect_timeout_handler(OBJECTPTR, int64_t, int64_t);

static void clear_connect_timer(extNetSocket *Socket)
{
   if (Socket->TimerHandle) { UpdateTimer(Socket->TimerHandle, 0); Socket->TimerHandle = 0; }
}

static void complete_socket_connect(extNetSocket *Socket, ERR Result)
{
   kt::Log log(__FUNCTION__);

   if ((Socket->State != NTC::RESOLVING) and (Socket->State != NTC::CONNECTING)) {
      log.trace("Ignoring duplicate connect completion while socket is in state %s.", netsocket_state(Socket->State));
      return;
   }

   network_platform().remove_write(Socket->Handle);

   if (Result != ERR::Okay) {
      log.trace("Connect completion result %d", int(Result));
      clear_connect_timer(Socket);
      Socket->Error = Result;
      Socket->setState(NTC::DISCONNECTED);
      return;
   }

   Socket->Error = ERR::Okay;
   clear_connect_timer(Socket);

   #ifndef DISABLE_SSL
      if (Socket->TLS.Handle) {
         tls_connect(Socket);
         if (Socket->Error != ERR::Okay) return;
         if (Socket->State IS NTC::HANDSHAKING) network_platform().register_read(Socket->Handle, &netsocket_incoming,
            Socket);
         return;
      }
      else {
         Socket->setState(NTC::CONNECTED);
         network_platform().register_read(Socket->Handle, &netsocket_incoming, Socket);
      }
   #else
      Socket->setState(NTC::CONNECTED);
      network_platform().register_read(Socket->Handle, &netsocket_incoming, Socket);
   #endif
}

static ERR NETSOCKET_Connect(extNetSocket *Self, struct ns::Connect *Args)
{
   kt::Log log;

   if ((!Args) or Args->Address.empty() or (Args->Port <= 0) or (Args->Port >= 65536)) return log.warning(ERR::Args);

   if (Self->isDerived()) return ERR::InvalidState; // Server cannot use Connect()

   if ((Self->Flags & NSF::UDP) != NSF::NIL) return log.warning(ERR::NoSupport); // UDP is connectionless

   if (!Self->Handle) return log.warning(ERR::NotInitialised);

   if (Self->State != NTC::DISCONNECTED) {
      log.warning("Attempt to connect when socket is in state %s", netsocket_state(Self->State));
      return ERR::InvalidState;
   }

#ifdef DISABLE_SSL
   if ((Self->Flags & NSF::SSL) != NSF::NIL) {
      Self->Error = ERR::NoSecureSockets;
      return log.warning(Self->Error);
   }
#endif

   log.branch("Address: %.*s, Port: %d", int(Args->Address.size()), Args->Address.data(), Args->Port);

   if (Self->Address != Args->Address) {
      Self->Address.assign(Args->Address);
   }
   Self->Port = Args->Port;

   Self->setState(NTC::RESOLVING);

   // Set up timeout timer if specified.  Failure is not critical.

   if (Args->Timeout > 0) {
      SubscribeTimer(Args->Timeout, C_FUNCTION(connect_timeout_handler, Self), &Self->TimerHandle);
   }

   IPAddress server_ip;
   if (!net::StrToAddress(Self->Address, &server_ip)) { // The address is an IP string, no resolution is necessary
      std::vector<IPAddress> list;
      list.emplace_back(server_ip);
      connect_name_resolved(Self, ERR::Okay, "", list);
   }
   else { // Assume address is a domain name, perform name resolution
      log.msg("Attempting to resolve domain name '%s'...", Self->Address.c_str());

      if (!Self->NetLookup) {
         if (!(Self->NetLookup = extNetLookup::create::local())) return ERR::CreateObject;
      }

      ((extNetLookup *)Self->NetLookup)->Callback = C_FUNCTION(connect_name_resolved_nl);

      if (Self->NetLookup->resolveName(Self->Address) != ERR::Okay) {
         // Cancel timer on DNS failure
         if (Self->TimerHandle) { UpdateTimer(Self->TimerHandle, 0); Self->TimerHandle = 0; }
         return log.warning(Self->Error = ERR::HostNotFound);
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// This function is called on completion of nlResolveName().

static void connect_name_resolved_nl(objNetLookup *NetLookup, ERR Error, std::string_view HostName,
   const std::vector<IPAddress> &IPs)
{
   connect_name_resolved((extNetSocket *)CurrentContext(), Error, HostName, IPs);
}

static const IPAddress *select_connect_address(extNetSocket *Socket, const std::vector<IPAddress> &IPs)
{
   if (!Socket->IPV6) {
      for (const auto &ip : IPs) {
         if (ip.Type IS IPADDR::V4) return &ip;
      }
      return nullptr;
   }
   else return &IPs[0];
}

static ERR start_platform_connect(extNetSocket *Socket, const NetworkEndpoint &Endpoint)
{
   kt::Log log(__FUNCTION__);

   auto connect_error = network_platform().connect(Socket->Handle, Endpoint);

   if (connect_error IS ERR::Busy) {
      log.trace("%s connection in progress...", Endpoint.Label);
      Socket->setState(NTC::CONNECTING);
      network_platform().begin_connect_wait(Socket->Handle, &netsocket_connect, Socket);
   }
   else if (!connect_error) {
      log.trace("%s connect() successful.", Endpoint.Label);
      complete_socket_connect(Socket, network_platform().complete_connect(Socket->Handle));
   }
   else {
      Socket->Error = connect_error;
      log.warning("%s connect() failed: %s", Endpoint.Label, GetErrorMsg(Socket->Error));
      clear_connect_timer(Socket);
      Socket->setState(NTC::DISCONNECTED);
      return Socket->Error;
   }

   return ERR::Okay;
}

static void connect_name_resolved(extNetSocket *Socket, ERR Error, std::string_view HostName,
   const std::vector<IPAddress> &IPs)
{
   kt::Log log(__FUNCTION__);

   if (Error != ERR::Okay) {
      log.warning("DNS resolution failed: %s", GetErrorMsg(Error));
      Socket->Error = ERR::HostNotFound;
      Socket->setState(NTC::DISCONNECTED);
      return;
   }

   log.msg("Received callback on DNS resolution.  Handle: %d", Socket->Handle.int_value());

   if (IPs.empty()) {
      log.warning("No IP addresses resolved for %.*s", int(HostName.size()), HostName.data());
      Socket->Error = ERR::HostNotFound;
      Socket->setState(NTC::DISCONNECTED);
      return;
   }

   const IPAddress *addr = select_connect_address(Socket, IPs);
   if (addr and Socket->IPV6) {
      if ((!addr->Data[0]) and (!addr->Data[1]) and (!addr->Data[2]) and (!addr->Data[3])) {
         log.traceWarning("Failed sanity check, incoming IP address is empty.");
         Socket->Error = log.warning(ERR::InvalidData);
         return;
      }
   }

   if (!addr) {
      auto socket_type = Socket->IPV6 ? "true" : "false";
      log.warning("Of %d addresses, no compatible IP address found for socket type (IPv6: %s)", int(IPs.size()),
         socket_type);
      Socket->Error = ERR::HostNotFound;
      Socket->setState(NTC::DISCONNECTED);
      return;
   }

   NetworkEndpoint endpoint;
   if (auto error = network_platform().build_address(*addr, Socket->Port, Socket->IPV6, endpoint); error != ERR::Okay) {
      Socket->Error = log.warning(error);
      Socket->setState(NTC::DISCONNECTED);
      return;
   }

   start_platform_connect(Socket, endpoint);
}

//********************************************************************************************************************
// Connection timeout handler - called when the connection timeout expires

static ERR connect_timeout_handler(OBJECTPTR Subscriber, int64_t TimeElapsed, int64_t CurrentTime)
{
   kt::Log log(__FUNCTION__);
   auto socket = (extNetSocket *)Subscriber;

   log.msg("Connection timeout triggered.");

   socket->TimerHandle = 0;
   socket->Error = ERR::TimeOut;

   if ((socket->State != NTC::CONNECTING) and (socket->State != NTC::RESOLVING) and (socket->State != NTC::HANDSHAKING)) {
      log.trace("Socket is no longer connecting, ignoring timeout.");
      return ERR::Terminate;
   }

   if (socket->Handle.is_valid()) free_socket(socket);

   // Cancel DNS resolution if in progress
   if (socket->NetLookup) { FreeResource(socket->NetLookup); socket->NetLookup = nullptr; }

   return ERR::Terminate; // Unsubscribe
}

/*********************************************************************************************************************
-ACTION-
DataFeed: Streams raw data to the socket for writing.

Data sent to a NetSocket through this action is written using the same buffered behaviour as #Write().  If the feed
size is zero, the buffer is treated as null-terminated text.

*********************************************************************************************************************/

static ERR NETSOCKET_DataFeed(extNetSocket *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Buffer)) return log.warning(ERR::NullArgs);

   if (not Args->Size) return ERR::Okay;
   if (Args->Size < 0) return log.warning(ERR::Args);

   switch(Args->Datatype) {
      case DATA::TEXT:
      case DATA::RAW:
      case DATA::XML:
      case DATA::AUDIO:
      case DATA::IMAGE:
         return acWrite(Self, Args->Buffer, Args->Size, nullptr);

      case DATA::FILE: { // File path
         auto file = objFile::create({ fl::Path(CSTRING(Args->Buffer)), fl::Flags(FL::READ) });
         if (file.ok()) {
            auto size = file->get<size_t>(FID_Size);
            std::vector<int8_t> buf(size);
            if (!file->read(buf.data(), size)) {
               return acWrite(Self, buf.data(), size, nullptr);
            }
            else return log.warning(ERR::Read);
         }
         else return log.warning(ERR::File);
      }

      default:
         return log.warning(ERR::NoSupport);
   }
}

/*********************************************************************************************************************

-ACTION-
Disable: Disables sending and receiving on the socket.

This method will stop all sending and receiving of data over the socket.  This is irreversible.

-ERRORS-
Okay
Failed: Shutdown operation failed.
SystemCall


*********************************************************************************************************************/

static ERR NETSOCKET_Disable(extNetSocket *Self)
{
   kt::Log log;

   log.trace("");

   int result = network_platform().shutdown_socket(Self->Handle, 2);

   if (result) { // Zero is success on both platforms
      log.warning("shutdown() failed.");
      return ERR::SystemCall;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

extNetSocket::~extNetSocket()
{
   if (TimerHandle)    UpdateTimer(TimerHandle, 0);
   if (NetLookup)      FreeResource(NetLookup);

   if (Feedback.isScript()) UnsubscribeAction(Feedback.Context, AC::Free);
   if (Incoming.isScript()) UnsubscribeAction(Incoming.Context, AC::Free);
   if (Outgoing.isScript()) UnsubscribeAction(Outgoing.Context, AC::Free);

#ifndef DISABLE_SSL
   tls_disconnect(this);
#endif

   free_socket(this);
}

//********************************************************************************************************************
// If a netsocket object is about to be freed, ensure that we are not using the netsocket object in one of our message
// handlers.  We can still delay the free request in any case.

static ERR NETSOCKET_FreeWarning(extNetSocket *Self)
{
   if (Self->InUse) {
      if (!Self->Terminating) { // Check terminating state to prevent flooding of the message queue
         kt::Log().msg("NetSocket in use, cannot free yet (request delayed).");
         Self->Terminating = true;
         SendMessage(MSGID::FREE, MSF::NIL, &Self->UID, sizeof(OBJECTID));
      }
      return ERR::InUse;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetLocalIPAddress: Returns the IP address that the socket is locally bound to.

This method performs the POSIX equivalent of `getsockname()`.  It returns the current address to which the NetSocket
is bound.

-INPUT-
struct(*IPAddress) Address:  Pointer to an IPAddress structure which will be set to the result of the query if successful.

-ERRORS-
Okay
NullArgs
Failed

-TAGS-
pure-query, mutates-input
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_GetLocalIPAddress(extNetSocket *Self, struct ns::GetLocalIPAddress *Args)
{
   kt::Log log;

   log.traceBranch();

   if ((!Args) or (!Args->Address)) return log.warning(ERR::NullArgs);

   auto result = network_platform().get_local_ip(Self->Handle, *Args->Address);
   if (!result) return ERR::Okay;
   else return log.warning(result);
}

//********************************************************************************************************************

static ERR NETSOCKET_Init(extNetSocket *Self)
{
   kt::Log log;
   ERR error;

   if (Self->Handle.is_valid()) return ERR::Okay; // The socket has been pre-configured by the developer

   if ((Self->Flags & NSF::UDP) != NSF::NIL) { // Set UDP-specific defaults
      if (!Self->MaxPacketSize) Self->MaxPacketSize = 65507; // Standard UDP max packet size
      Self->State = NTC::CONNECTED; // UDP sockets are always "connected" (ready to send/receive)
   }

#ifndef DISABLE_SSL
   // Initialise SSL ahead of any connections being made.

   if ((Self->Flags & NSF::SSL) != NSF::NIL) {
      error = (Self->classID() IS CLASSID::NETSERVER) ? tls_setup_server((extNetServer *)Self) : tls_setup_client((extNetSocket *)Self);
      if (error != ERR::Okay) return error;
   }
#endif

   bool is_ipv6 = false;
   bool is_udp = ((Self->Flags & NSF::UDP) != NSF::NIL);
   Self->Handle = network_platform().create_socket(Self, true, false, is_udp, is_ipv6);
   if (Self->Handle.is_invalid()) return ERR::SystemCall;
   Self->IPV6 = is_ipv6;

   if ((!Self->isDerived()) and (!is_udp) and ((Self->Flags & NSF::KEEP_ALIVE) != NSF::NIL)) {
      if (auto error = network_platform().enable_keep_alive(Self->Handle); error != ERR::Okay) {
         free_socket(Self);
         return error;
      }
   }

   // Configure UDP-specific socket options

   if ((Self->Flags & NSF::UDP) != NSF::NIL) {
      if ((Self->Flags & NSF::BROADCAST) != NSF::NIL) {
         if (network_platform().enable_broadcast(Self->Handle) != ERR::Okay) {
            log.warning("Failed to enable broadcast");
         }
      }

      if (Self->MulticastTTL > 0) {
         if (network_platform().set_multicast_ttl(Self->Handle, Self->MulticastTTL, Self->IPV6) != ERR::Okay) {
            log.warning("Failed to set multicast TTL");
         }
      }
   }

   if (Self->isDerived()) return ERR::Okay; // Will hand-off to the derived class

   if ((not Self->Address.empty()) and (Self->Port > 0)) {
      if ((error = Self->connect(Self->Address, Self->Port, 0)) != ERR::Okay) {
         free_socket(Self);
         return error;
      }
      else return ERR::Okay;
   }
   else if ((Self->Flags & NSF::UDP) != NSF::NIL) {
      network_platform().register_read(Self->Handle, &netsocket_incoming, Self);
      return ERR::Okay;
   }
   else return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
JoinMulticastGroup: Join a multicast group for receiving multicast packets (UDP only).

This method joins a multicast group, allowing the socket to receive packets sent to the specified multicast address.
This is only available for UDP sockets.

The socket must be bound to a local address before joining a multicast group.

-INPUT-
strview Group: The multicast group address to join (e.g. `224.1.1.1`).

-ERRORS-
Okay: Successfully joined the multicast group.
Args: Invalid multicast address.
NoSupport: Socket is not configured for UDP mode.
Failed: Failed to join multicast group.
NullArgs

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_JoinMulticastGroup(extNetSocket *Self, struct ns::JoinMulticastGroup *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & NSF::UDP) IS NSF::NIL) return ERR::NoSupport;
   if (Args->Group.empty()) return ERR::Args;

   log.branch("%.*s", int(Args->Group.size()), Args->Group.data());

   bool group_ipv6 = false;
   if (auto error = network_platform().join_multicast_group(Self->Handle, Args->Group, group_ipv6);
         error != ERR::Okay) {
      if (error IS ERR::Args) {
         log.warning("Invalid multicast address: %.*s", int(Args->Group.size()), Args->Group.data());
         return ERR::Args;
      }

      if (group_ipv6) {
         log.warning("Failed to join IPv6 multicast group");
      }
      else {
         log.warning("Failed to join IPv4 multicast group");
      }
      return ERR::Failed;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
LeaveMulticastGroup: Leave a multicast group (UDP only).

This method leaves a previously joined multicast group, stopping the reception of packets sent to the specified
multicast address.

-INPUT-
strview Group: The multicast group address to leave.

-ERRORS-
Okay: Successfully left the multicast group.
Args: Invalid multicast address.
NoSupport: Socket is not configured for UDP mode.
Failed: Failed to leave multicast group.
NullArgs

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_LeaveMulticastGroup(extNetSocket *Self, struct ns::LeaveMulticastGroup *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & NSF::UDP) IS NSF::NIL) return ERR::NoSupport;
   if (Args->Group.empty()) return ERR::Args;

   log.branch("%.*s", int(Args->Group.size()), Args->Group.data());

   bool group_ipv6 = false;
   if (auto error = network_platform().leave_multicast_group(Self->Handle, Args->Group, group_ipv6);
         error != ERR::Okay) {
      if (error IS ERR::Args) {
         log.warning("Invalid multicast address: %.*s", int(Args->Group.size()), Args->Group.data());
         return ERR::Args;
      }

      if (group_ipv6) {
         log.warning("Failed to leave IPv6 multicast group");
      }
      else {
         log.warning("Failed to leave IPv4 multicast group");
      }
      return ERR::Failed;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Read: Read information from the socket.

The Read() action will read incoming data from the socket and write it to the provided buffer.  If the socket connection
is safe, success will always be returned by this action regardless of whether or not data was available.  Almost all
other return codes indicate permanent failure and the socket connection will be closed when the action returns.

Because NetSocket objects are non-blocking, reading from the socket is normally performed in the #Incoming
callback.  Reading from the socket when no data is available will result in an immediate return with no output.

-ERRORS-
Okay: Read successful (if no data was on the socket, success is still indicated).
NullArgs
Disconnected: The socket connection is closed.
InvalidState: The socket is not in a state that allows reading (e.g. during SSL handshake).
Failed: A permanent failure has occurred and socket has been closed.
Args
Read


*********************************************************************************************************************/

static ERR NETSOCKET_Read(extNetSocket *Self, struct acRead *Args)
{
   kt::Log log;

   if ((!Args) or (!Args->Buffer)) return log.warning(ERR::NullArgs);
   Args->Result = 0;
   if (Args->Length < 0) return log.warning(ERR::Args);

   if (Self->Handle.is_invalid()) return log.warning(ERR::Disconnected);

   Self->ReadCalled = true;

   if (!Args->Length) return ERR::Okay;

#ifndef DISABLE_SSL
   if (Self->TLS.Handle) {
      #ifdef _WIN32
         // If we're in the middle of SSL handshake, return nothing.  The automated incoming data handler is managing the object state.
         if (Self->State IS NTC::HANDSHAKING) return log.traceWarning(ERR::InvalidState);
         else if (Self->State != NTC::CONNECTED) return log.warning(ERR::Disconnected);

         if (!ssl_has_decrypted_data(Self->TLS.Handle)) {
            if (auto receive_error = tls_receive_encrypted(Self); receive_error IS ERR::Disconnected) {
               free_socket(Self);
               return ERR::Disconnected;
            }
            else if (receive_error != ERR::Okay) return receive_error;
         }

         int bytes_read = 0;
         if (auto error = ssl_read(Self->TLS.Handle, Args->Buffer, Args->Length, &bytes_read); error IS SSL_OK) {
            Args->Result = bytes_read;
            return ERR::Okay;
         }
         else if (error IS SSL_ERROR_DISCONNECTED) return log.traceWarning(ERR::Disconnected);
         else if (error IS SSL_ERROR_WOULD_BLOCK) return ERR::Okay; // Not considered an error.
         else {
            log.warning("Windows SSL read error (code %d)", error);
            return ERR::Failed;
         }
      #else // OpenSSL
         bool read_blocked;
         int pending;

         if (Self->TLS.HandshakeStatus IS SHS::WRITE) ssl_handshake_write(Self->Handle, Self);
         else if (Self->TLS.HandshakeStatus IS SHS::READ) ssl_handshake_read(Self->Handle, Self);

         if (Self->TLS.HandshakeStatus != SHS::NIL) { // Still handshaking
            log.trace("SSL handshake still in progress.");
            return ERR::Okay;
         }

         auto Buffer = Args->Buffer;
         auto BufferSize = Args->Length;
         do {
            read_blocked = false;
            ssl_clear_error_queue();
            if (auto result = SSL_read(Self->TLS.Handle, Buffer, BufferSize); result <= 0) {
               auto ssl_error = SSL_get_error(Self->TLS.Handle, result);
               switch (ssl_error) {
                  case SSL_ERROR_ZERO_RETURN:
                     free_socket(Self);
                     return log.traceWarning(ERR::Disconnected);

                  case SSL_ERROR_WANT_READ: read_blocked = true; break;

                   case SSL_ERROR_WANT_WRITE:
                     // WANT_WRITE is returned if we're trying to rehandshake and the write operation would block.  We
                     // need to wait on the socket to be writeable, then restart the read when it is.

                      log.msg("SSL socket handshake requested by server.");
                      Self->TLS.HandshakeStatus = SHS::WRITE;
                     network_platform().register_write(Self->Handle, ssl_handshake_write_netsocket, Self);
                      return ERR::Okay;

                  case SSL_ERROR_SYSCALL:
                     log.warning("SSL read failed with %s: %s", ssl_error_name(ssl_error), strerror(errno));
                     return ERR::Read;

                  case SSL_ERROR_SSL:
                     if (ssl_unexpected_eof()) {
                        ssl_clear_error_queue();
                        free_socket(Self);
                        return log.traceWarning(ERR::Disconnected);
                     }
                     log.warning("SSL read failed with %s.", ssl_error_name(ssl_error));
                     ssl_log_error_queue(log, "SSL_read");
                     return ERR::Read;

                  default:
                     log.warning("SSL read failed with %s.", ssl_error_name(ssl_error));
                     return ERR::Read;
               }
            }
            else {
               Args->Result += result;
               Buffer = (APTR)((char *)Buffer + result);
               BufferSize -= result;
            }
         } while ((pending = SSL_pending(Self->TLS.Handle)) and (!read_blocked) and (BufferSize > 0));

         log.trace("Pending: %d, BufSize: %d, Blocked: %d", pending, BufferSize, read_blocked);

         if (pending) {
            // With regards to non-blocking SSL sockets, be aware that a socket can be empty in terms of incoming data,
            // yet SSL can keep data that has already arrived in an internal buffer.  This means that we can get stuck
            // select()ing on the socket because you aren't told that there is internal data waiting to be processed by
            // SSL_read().
            //
            // For this reason we set the RECALL flag so that we can be called again manually when we know that there is
            // data pending.

            network_platform().register_recall_read(Self->Handle, &netsocket_incoming, Self);
         }

         return ERR::Okay;
      #endif
   }
#endif

   size_t bytes_received = 0;
   auto receive_error = network_platform().receive(Self->Handle, Args->Buffer, Args->Length, bytes_received);
   Args->Result = int(bytes_received);

   if (receive_error IS ERR::Disconnected) {
      free_socket(Self);
      return ERR::Disconnected;
   }
   else return receive_error;
}

/*********************************************************************************************************************

-ACTION-
Write: Writes data to the socket.

Writing data to a socket will send raw data to the remote client or server.  Write connections are buffered, so any
data overflow generated in a call to this action will be buffered into a software queue.  Resource limits placed on
the software queue are governed by the #MsgLimit field setting.

Do not use this action on a @NetServer listener.  Instead, write to the @ClientSocket object that will receive the data.

It is possible to write to a socket in advance of any connection being made. The netsocket will queue the data
and automatically send it once the first connection has been made.

*********************************************************************************************************************/

template <class T>
static ERR queue_socket_write(T *Self, struct acWrite *Args, size_t MsgLimit)
{
   kt::Log log(__FUNCTION__);

   log.trace("Saving %d bytes to queue.", Args->Length);

   auto len = std::min<size_t>(Args->Length, MsgLimit);
   if (auto error = Self->WriteQueue.write(Args->Buffer, len); error != ERR::Okay) return error;

   Args->Result = int(len);
   return (len < size_t(Args->Length)) ? ERR::BufferOverflow : ERR::Okay;
}

template <class T>
static void register_socket_write(T *Self, void (*WriteCallback)(HOSTHANDLE, APTR))
{
   network_platform().register_write(Self->Handle, WriteCallback, Self);
}

template <class T>
static ERR write_connected_socket_data(T *Self, struct acWrite *Args, size_t MsgLimit,
   void (*WriteCallback)(HOSTHANDLE, APTR), bool &FatalError)
{
   kt::Log log(__FUNCTION__);

   size_t len;
   ERR error;

   if (Self->WriteQueue.Buffer.empty()) {
      len = Args->Length;
      error = send_data(Self, Args->Buffer, &len);
   }
   else {
      len = 0;
      error = ERR::BufferOverflow;
   }

   if ((!error) and (len >= size_t(Args->Length))) return ERR::Okay;

   bool ssl_read_blocked = false;
   bool ssl_write_blocked = false;

   #if !defined(DISABLE_SSL) and !defined(_WIN32)
      ssl_read_blocked = (error IS ERR::Busy) and (Self->TLS.Handle) and (Self->TLS.HandshakeStatus IS SHS::READ);
      ssl_write_blocked = (error IS ERR::BufferOverflow) and (Self->TLS.Handle) and
         (Self->TLS.HandshakeStatus IS SHS::WRITE);
   #endif

   if ((error IS ERR::DataSize) or (error IS ERR::BufferOverflow) or (ssl_read_blocked) or (len > 0)) {
      auto remaining = size_t(Args->Length) - len;

      log.trace("Error: '%s', queuing %d/%d bytes for transfer...", GetErrorMsg(error), int(remaining),
         Args->Length);

      auto queue_len = std::min<size_t>(remaining, MsgLimit);
      if (auto queue_error = Self->WriteQueue.write((int8_t *)Args->Buffer + len, queue_len);
          queue_error != ERR::Okay) {
         Args->Result = int(len);
         return queue_error;
      }

      auto queue_overflow = queue_len < remaining;
      if (queue_overflow) Args->Result = int(len + queue_len);

      if (ssl_write_blocked) {
         #if !defined(DISABLE_SSL) and !defined(_WIN32)
            ssl_resume_write_handshake(Self->Handle.hosthandle(), Self);
         #endif
      }
      else if (!ssl_read_blocked) register_socket_write(Self, WriteCallback);

      return queue_overflow ? ERR::BufferOverflow : ERR::Okay;
   }

   FatalError = true;
   return error;
}

static ERR NETSOCKET_Write(extNetSocket *Self, struct acWrite *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   Args->Result = 0;
   if (Args->Length < 0) return log.warning(ERR::Args);
   if ((!Args->Buffer) and (Args->Length > 0)) return log.warning(ERR::NullArgs);


   if ((Self->Handle.is_invalid()) or (Self->State != NTC::CONNECTED)) { // Queue the write prior to server connection
      return queue_socket_write(Self, Args, Self->MsgLimit);
   }

   // Note that if a write queue has been setup, there is no way that we can write to the server until the queue has
   // been exhausted.  Thus we have add more data to the queue if it already exists.

   bool fatal_error = false;
   auto error = write_connected_socket_data(Self, Args, Self->MsgLimit, &netsocket_outgoing, fatal_error);
   if (fatal_error) {
      Self->ErrorCountdown--;
      if (!Self->ErrorCountdown) Self->setState(NTC::DISCONNECTED);
      return error;
   }
   else if (error != ERR::Okay) return error;
   else log.trace("Successfully wrote all %d bytes to the server.", Args->Length);

   Args->Result = Args->Length;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RecvFrom: Receive a datagram packet from any address (UDP only).

This method receives a datagram packet from any source address.  It is only available for sockets configured with
the UDP flag.  Unlike TCP connections, UDP is connectionless so packets can be received from any source without
establishing a connection first.

The method is non-blocking and will return immediately.  If no data is available, `ERR::Okay` will be returned
with `BytesRead` set to zero.

The source address and port of the received packet will be provided in the output parameters.

For TCP sockets, use the standard Read action instead.

-INPUT-
ptr(struct(IPAddress)) Source: Source IP address of the received packet.
^buf(ptr) Buffer:   Output buffer for received data.
bufsize BufferSize: Size of the receive buffer in bytes.
&int BytesRead:     Number of bytes actually received.

-ERRORS-
Okay: Data was received successfully, or no data available.
Args: Invalid arguments provided.
NoSupport: Socket is not configured for UDP mode.
BufferOverflow: Receive buffer is too small for the incoming packet.
NullArgs

-TAGS-
non-blocking, mutates-input, mutates-object
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_RecvFrom(extNetSocket *Self, struct ns::RecvFrom *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & NSF::UDP) IS NSF::NIL) return log.warning(ERR::NoSupport);
   if ((!Args->Buffer) or (!Args->Source)) return log.warning(ERR::NullArgs);
   if (Args->BufferSize <= 0) return log.warning(ERR::Args);

   Self->ReadCalled = true;

   Args->BytesRead = 0;

   size_t bytes_read = 0;
   IPAddress source_address;

   auto error = network_platform().receive_from(Self->Handle, Args->Buffer, Args->BufferSize, bytes_read,
      source_address);
   Args->BytesRead = int(bytes_read);
   if (error != ERR::Okay) return error;

   if (Args->BytesRead > 0) *Args->Source = source_address;

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SendTo: Send a datagram packet to a specific address (UDP only).

This method sends a datagram packet to a specified IP address and port.  It is only available for sockets configured
with the UDP flag.  Unlike TCP connections, UDP is connectionless so packets can be sent to any address without
establishing a connection first.

The method is non-blocking and will return immediately.  If the network buffer is full, an `ERR::BufferOverflow`
error will be returned and the client should retry the operation later.

For TCP sockets, use the standard Write action instead.

-INPUT-
ptr(struct(IPAddress)) Dest: The destination IP address (IPv4 or IPv6) and port number.
buf(ptr) Data:  Pointer to the data buffer to send.
bufsize Length: Number of bytes to send from Data.
&int BytesSent: Number of bytes actually sent.

-ERRORS-
Okay: The packet was sent successfully.
BufferOverflow: The network buffer is full, retry later.
NullArgs: Invalid arguments provided.
OutOfRange: Invalid port number specified.
InvalidState: Socket is not configured for UDP mode.
NetworkUnreachable: The destination network is unreachable.
Args
DataSize

-TAGS-
non-blocking, consumes-input
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_SendTo(extNetSocket *Self, struct ns::SendTo *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & NSF::UDP) IS NSF::NIL) return log.warning(ERR::InvalidState);
   if ((!Args->Dest) or (!Args->Data) or (!Args->Length)) return log.warning(ERR::NullArgs);
   if (Args->Length <= 0) return log.warning(ERR::Args);
   if (Args->Dest->Port <= 0 or Args->Dest->Port > 65535) return log.warning(ERR::OutOfRange);

   // Enforce max packet size (optional safety)
   if (Self->MaxPacketSize and Args->Length > Self->MaxPacketSize) {
      log.warning("Packet length %d exceeds MaxPacketSize %d", Args->Length, Self->MaxPacketSize);
      return ERR::DataSize;
   }

   log.branch("%d bytes", Args->Length);

   Args->BytesSent = 0;

   NetworkEndpoint dest_addr;
   if (auto error = network_platform().build_address(*Args->Dest, Args->Dest->Port, Self->IPV6, dest_addr);
      error != ERR::Okay) return log.warning(error);

   size_t bytes_to_send = Args->Length;

   auto error = network_platform().send_to(Self->Handle, Args->Data, bytes_to_send, dest_addr);
   if (!error) Args->BytesSent = int(bytes_to_send);
   return error;
}

//********************************************************************************************************************

#include "netsocket_fields.cpp"
#include "netsocket_functions.cpp"
#include "netsocket_def.c"

//********************************************************************************************************************

static const FieldArray clSocketFields[] = {
   { "ClientData",     FDF_POINTER|FDF_RW },
   { "Address",        FDF_CPPSTRING|FDF_RI },
   { "State",          FDF_INT|FDF_LOOKUP|FDF_RW, GET_State, SET_State, &clNetSocketState },
   { "Error",          FDF_ERROR|FDF_R },
   { "Port",           FDF_INT|FDF_RI },
   { "Flags",          FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clNetSocketFlags },
   { "MsgLimit",       FDF_INT|FDF_RI },
   { "MaxPacketSize",  FDF_INT|FDF_RI },
   { "MulticastTTL",   FDF_INT|FDF_RI },
   // Virtual fields
   { "Handle",         FDF_VIRTUAL|FDF_POINTER|FDF_RI,     GET_Handle, SET_Handle },
   { "Feedback",       FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_Feedback, SET_Feedback },
   { "Incoming",       FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_Incoming, SET_Incoming },
   { "Outgoing",       FDF_VIRTUAL|FDF_FUNCTION|FDF_W,  GET_Outgoing, SET_Outgoing },
   { "OutQueueSize",   FDF_VIRTUAL|FDF_INT|FDF_R,          GET_OutQueueSize },
   END_FIELD
};

//********************************************************************************************************************

static CSTRING netsocket_state(NTC Value) {
   return clNetSocketState[int(Value)].Name;
}

static ERR init_netsocket(void)
{
   clNetSocket = objMetaClass::create::global(
      fl::ClassVersion(VER_NETSOCKET),
      fl::Name("NetSocket"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clNetSocketActions),
      fl::Methods(clNetSocketMethods),
      fl::Fields(clSocketFields),
      fl::Size(sizeof(extNetSocket)),
      fl::Path(MOD_PATH));

   return clNetSocket ? ERR::Okay : ERR::AddClass;
}
