/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
NetSocket: Manages network connections via TCP/IP sockets.

The NetSocket class provides a simple way of managing TCP/IP socket communications.  Connections from a single client
to the server and from the server to multiple clients are supported.  SSL functionality is also integrated.

The design of the NetSocket class caters to asynchronous (non-blocking) communication.  This is achieved primarily
through callback fields - connection alerts are managed by #Feedback, incoming data is received through #Incoming
and readiness for outgoing data is supported by #Outgoing.

<header>Client-Server Connections</>

After a connection has been established, data may be written using any of the following methods:

<list type="bullet">
<li>Write directly to the socket with the #Write() action.</li>
<li>Subscribe to the socket by referring to a routine in the #Outgoing field.  The routine will be called to
initially fill the internal write buffer, thereafter it will be called whenever the buffer is empty.</li>
</ul>

It is possible to write to a NetSocket object before the connection to a server is established.  Doing so will buffer
the data in the socket until the connection with the server has been initiated, at which point the data will be
immediately sent.

<header>Server-Client Connections</>

To accept incoming client connections, create a NetSocket object with the `SERVER` flag set and define the #Port value
on which to listen for new clients.  If multiple connections from a single client IP address are allowed, set the
`MULTI_CONNECT` flag.

When a new connection is detected, the #Feedback function will be called as `Feedback(*NetSocket, *ClientSocket, NTC State)`

The NetSocket parameter refers to the original NetSocket object, @ClientSocket applies if a client connection is
involved and the State value will be set to `NTC::CONNECTED`.  If a client disconnects, the #Feedback function will be
called in the same manner but with a State value of `NTC::DISCONNECTED`.

Information on all active connections can be read from the #Clients field.  This contains a linked list of IP
addresses and their connections to the server port.

To send data to a client, write it to the target @ClientSocket.

All data that is received from client sockets will be passed to the #Incoming feedback routine with a reference to a @ClientSocket.

<header>SSL Server Certificates</>

For SSL server sockets, custom certificates can be specified using the #SSLCertificate field. Both PEM and PKCS#12
formats are supported across all platforms.

Example with PKCS#12 certificate:

```
netsocket = obj.new('netsocket', {
   flags = 'SERVER|SSL',
   port = 8443,
   sslCertificate = 'config:ssl/server.p12',
   sslKeyPassword = 'password123'
})
```

Example with PEM certificate and separate private key:

```
netsocket = obj.new('netsocket', {
   flags = 'SERVER|SSL',
   port = 8443,
   sslCertificate = 'config:ssl/server.crt',
   sslPrivateKey = 'config:ssl/server.key'
})
```

If no custom certificate is specified, the framework will automatically use a localhost self-signed certificate
for development purposes.  For production use, always specify a proper certificate signed by a trusted CA.

-END-

*********************************************************************************************************************/

// The MaxWriteLen cannot exceed the size of the network queue on the host platform, otherwise all send attempts will
// return 'could block' error codes.  Note that when using SSL, the write length is an SSL library imposition.

static size_t glMaxWriteLen = 16 * 1024;

//********************************************************************************************************************
// Prototypes for internal methods

static void free_client(extNetSocket *, objNetClient *);
static void free_socket(extNetSocket *);
static CSTRING netsocket_state(NTC Value);

// Implementation functions that take HOSTHANDLE
static void netsocket_incoming_impl(HOSTHANDLE, extNetSocket *);
static void netsocket_outgoing_impl(HOSTHANDLE, extNetSocket *);
#ifdef __linux__
static void netsocket_connect_impl(HOSTHANDLE, extNetSocket *);
#endif
static void server_accept_client_impl(HOSTHANDLE, extNetSocket *);
static void server_incoming_from_client_impl(HOSTHANDLE, extClientSocket *);
static void clientsocket_outgoing_impl(HOSTHANDLE, extClientSocket *);

// Wrappers for RegisterFD
static void netsocket_incoming(HOSTHANDLE FD, APTR Data) {
   netsocket_incoming_impl(FD, (extNetSocket *)Data);
}

static void netsocket_outgoing(HOSTHANDLE FD, APTR Data) {
   netsocket_outgoing_impl(FD, (extNetSocket *)Data);
}
#ifdef __linux__
static void netsocket_connect(HOSTHANDLE FD, APTR Data) {
   netsocket_connect_impl(FD, (extNetSocket *)Data);
}
#endif
static void server_accept_client(HOSTHANDLE FD, APTR Data) {
   server_accept_client_impl(FD, (extNetSocket *)Data);
}

static void server_incoming_from_client(HOSTHANDLE FD, APTR Data) {
   server_incoming_from_client_impl(FD, (extClientSocket *)Data);
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
cstr Address: String containing either a domain name (e.g. `www.google.com`) or an IP address (e.g. `123.123.123.123`)
int Port: Remote port to connect to.
double Timeout: Connection timeout in seconds (0 = no timeout).

-ERRORS-
Okay: The NetSocket connecting process was successfully started.
Args: Address was NULL, or Port was not in the required range.
InvalidState: The NetSocket was not in the state `NTC::DISCONNECTED` or the object is in server mode.
HostNotFound: Host name resolution failed.
TimeOut: Connection attempt timed out.
Failed: The connect failed for some other reason.
-END-

*********************************************************************************************************************/

static void connect_name_resolved_nl(objNetLookup *, ERR, const std::string &, const std::vector<IPAddress> &);
static void connect_name_resolved(extNetSocket *, ERR, const std::string &, const std::vector<IPAddress> &);
static ERR connect_timeout_handler(OBJECTPTR, int64_t, int64_t);

static ERR NETSOCKET_Connect(extNetSocket *Self, struct ns::Connect *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Address) or (Args->Port <= 0) or (Args->Port >= 65536)) return log.warning(ERR::Args);

   if ((Self->Flags & NSF::SERVER) != NSF::NIL) return ERR::InvalidState;

   if ((Self->Flags & NSF::UDP) != NSF::NIL) return log.warning(ERR::NoSupport); // UDP is connectionless

   if (!Self->Handle) return log.warning(ERR::NotInitialised);

   if (Self->State != NTC::DISCONNECTED) {
      log.warning("Attempt to connect when socket is in state %s", netsocket_state(Self->State));
      return ERR::InvalidState;
   }

   log.branch("Address: %s, Port: %d", Args->Address, Args->Port);

   if (Args->Address != Self->Address) {
      if (Self->Address) FreeResource(Self->Address);
      Self->Address = pf::strclone(Args->Address);
   }
   Self->Port = Args->Port;

   Self->setState(NTC::RESOLVING);

   // Set up timeout timer if specified.  Failure is not critical.

   if (Args->Timeout > 0) {
      SubscribeTimer(Args->Timeout, C_FUNCTION(connect_timeout_handler, Self), &Self->TimerHandle);
   }

   IPAddress server_ip;
   if (net::StrToAddress(Self->Address, &server_ip) IS ERR::Okay) { // The address is an IP string, no resolution is necessary
      std::vector<IPAddress> list;
      list.emplace_back(server_ip);
      connect_name_resolved(Self, ERR::Okay, "", list);
   }
   else { // Assume address is a domain name, perform name resolution
      log.msg("Attempting to resolve domain name '%s'...", Self->Address);

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

static void connect_name_resolved_nl(objNetLookup *NetLookup, ERR Error, const std::string &HostName, const std::vector<IPAddress> &IPs)
{
   connect_name_resolved((extNetSocket *)CurrentContext(), Error, HostName, IPs);
}

static void connect_name_resolved(extNetSocket *Socket, ERR Error, const std::string &HostName, const std::vector<IPAddress> &IPs)
{
   pf::Log log(__FUNCTION__);
   struct sockaddr_in server_address;

   if (Error != ERR::Okay) {
      log.warning("DNS resolution failed: %s", GetErrorMsg(Error));
      Socket->Error = ERR::HostNotFound;
      Socket->setState(NTC::DISCONNECTED);
      return;
   }

   log.msg("Received callback on DNS resolution.  Handle: %d", Socket->Handle.int_value());

   // Start connect()

   if (IPs.empty()) {
      log.warning("No IP addresses resolved for %s", HostName.c_str());
      Socket->Error = ERR::HostNotFound;
      Socket->setState(NTC::DISCONNECTED);
      return;
   }

   // Find an appropriate address for our socket type
   const IPAddress *addr = nullptr;

   // If we have an IPv4 socket, prefer IPv4 addresses
   if (!Socket->IPV6) {
      for (const auto &ip : IPs) {
         if (ip.Type IS IPADDR::V4) {
            addr = &ip;
            break;
         }
      }
   }
   else { // For IPv6 sockets, use the first address (could be IPv4 or IPv6)
      addr = &IPs[0];
      if ((!addr->Data[0]) and (!addr->Data[1]) and (!addr->Data[2]) and (!addr->Data[3])) {
         log.traceWarning("Failed sanity check, incoming IP address is empty.");
         Socket->Error = log.warning(ERR::InvalidData);
         return;
      }
   }

   if (!addr) {
      log.warning("Of %d addresses, no compatible IP address found for socket type (IPv6: %s)", int(IPs.size()), Socket->IPV6 ? "true" : "false");
      Socket->Error = ERR::HostNotFound;
      Socket->setState(NTC::DISCONNECTED);
      return;
   }

   if (addr->Type IS IPADDR::V6) { // Pure IPv6 connection
      #ifdef _WIN32
         struct sockaddr_in6 server_address6;
         pf::clearmem(&server_address6, sizeof(server_address6));
         server_address6.sin6_family = AF_INET6;
         server_address6.sin6_port = htons(Socket->Port);
         pf::copymem((void *)addr->Data, &server_address6.sin6_addr.s6_addr, 16);

         if ((Socket->Error = win_connect(Socket->Handle, (struct sockaddr *)&server_address6, sizeof(server_address6))) != ERR::Okay) {
            if (Socket->Error IS ERR::BufferOverflow) {
               log.trace("IPv6 connection in progress...");
               Socket->setState(NTC::CONNECTING);
            }
            else {
               log.warning("IPv6 connect() failed: %s", GetErrorMsg(Socket->Error));
               Socket->setState(NTC::DISCONNECTED);
               return;
            }
         }
         else {
            log.trace("IPv6 connect() successful.");
            Socket->setState(NTC::CONNECTED);
         }
         return;
      #else
         struct sockaddr_in6 server_address6;
         pf::clearmem(&server_address6, sizeof(server_address6));
         server_address6.sin6_family = AF_INET6;
         server_address6.sin6_port = htons(Socket->Port);
         pf::copymem((void *)addr->Data, &server_address6.sin6_addr.s6_addr, 16);

         int result = connect(Socket->Handle, (struct sockaddr *)&server_address6, sizeof(server_address6));

         if (result IS -1) {
            if (errno IS EINPROGRESS) {
               log.trace("IPv6 connection in progress...");
            }
            else if ((errno IS EWOULDBLOCK) or (errno IS EAGAIN)) {
               log.trace("IPv6 connect() attempt would block or need to try again.");
            }
            else {
               log.warning("IPv6 Connect() failed: %s", strerror(errno));
               Socket->Error = ERR::SystemCall;
               Socket->setState(NTC::DISCONNECTED);
               return;
            }

            Socket->setState(NTC::CONNECTING);
            RegisterFD(Socket->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Socket);
            RegisterFD(Socket->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, &netsocket_connect, Socket);
         }
         else {
            log.trace("IPv6 connect() successful.");
            Socket->setState(NTC::CONNECTED);
            RegisterFD(Socket->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Socket);
         }
      #endif
      return;
   }

   // IPv4 connection to dual-stack socket - use IPv4-mapped IPv6 address
   if ((Socket->IPV6) and (addr->Type IS IPADDR::V4)) {
      // Use IPv4-mapped IPv6 address for dual-stack socket
      #ifdef __linux__
         struct sockaddr_in6 server_address6;
         pf::clearmem(&server_address6, sizeof(server_address6));
         server_address6.sin6_family = AF_INET6;
         server_address6.sin6_port = htons(Socket->Port);

         // Create IPv4-mapped IPv6 address (::ffff:x.x.x.x)
         server_address6.sin6_addr.s6_addr[10] = 0xff;
         server_address6.sin6_addr.s6_addr[11] = 0xff;
         *((uint32_t*)&server_address6.sin6_addr.s6_addr[12]) = htonl(addr->Data[0]);

         int result = connect(Socket->Handle, (struct sockaddr *)&server_address6, sizeof(server_address6));

         if (result IS -1) {
            if (errno IS EINPROGRESS) {
               log.trace("IPv4-mapped IPv6 connection in progress...");
            }
            else if ((errno IS EWOULDBLOCK) or (errno IS EAGAIN)) {
               log.trace("IPv4-mapped IPv6 connect() attempt would block or need to try again.");
            }
            else {
               log.warning("IPv4-mapped IPv6 Connect() failed: %s", strerror(errno));
               Socket->Error = ERR::SystemCall;
               Socket->setState(NTC::DISCONNECTED);
               return;
            }

            Socket->setState(NTC::CONNECTING);
            RegisterFD(Socket->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Socket);
            RegisterFD(Socket->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, &netsocket_connect, Socket);
         }
         else {
            log.trace("IPv4-mapped IPv6 connect() successful.");
            Socket->setState(NTC::CONNECTED);
            RegisterFD(Socket->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Socket);
         }
         return;
      #elif _WIN32
         // Windows IPv6 dual-stack socket connecting to IPv4 address
         struct sockaddr_in6 server_address6;
         pf::clearmem(&server_address6, sizeof(server_address6));
         server_address6.sin6_family = AF_INET6;
         server_address6.sin6_port = htons(Socket->Port);

         // Create IPv4-mapped IPv6 address (::ffff:x.x.x.x)
         server_address6.sin6_addr.s6_addr[10] = 0xff;
         server_address6.sin6_addr.s6_addr[11] = 0xff;
         *((uint32_t*)&server_address6.sin6_addr.s6_addr[12]) = htonl(addr->Data[0]);

         if (win_connect(Socket->Handle, (struct sockaddr *)&server_address6, sizeof(server_address6)) IS ERR::Okay) {
            log.trace("IPv4-mapped IPv6 connection initiated successfully");
            Socket->setState(NTC::CONNECTING);
         }
         else {
            log.trace("IPv4-mapped IPv6 connect() failed");
            Socket->Error = ERR::SystemCall;
            Socket->setState(NTC::DISCONNECTED);
         }
         return;
      #endif
   }

   // Pure IPv4 connection
   pf::clearmem(&server_address, sizeof(struct sockaddr_in));
   server_address.sin_family = AF_INET;
   server_address.sin_port = htons(Socket->Port);
   server_address.sin_addr.s_addr = htonl(addr->Data[0]);

#ifdef __linux__
   int result = connect(Socket->Handle, (struct sockaddr *)&server_address, sizeof(server_address));

   if (result IS -1) {
      if (errno IS EINPROGRESS) {
         log.trace("Connection in progress...");
      }
      else if ((errno IS EWOULDBLOCK) or (errno IS EAGAIN)) {
         log.trace("connect() attempt would block or need to try again.");
      }
      else {
         log.warning("Connect() failed: %s", strerror(errno));
         Socket->Error = ERR::SystemCall;
         Socket->setState(NTC::DISCONNECTED);
         return;
      }

      Socket->setState(NTC::CONNECTING);
      RegisterFD(Socket->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Socket);

      // The write queue will be signalled once the connection process is completed.

      RegisterFD(Socket->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, &netsocket_connect, Socket);
   }
   else {
      log.trace("connect() successful.");

      Socket->setState(NTC::CONNECTED);
      RegisterFD(Socket->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Socket);
   }

#elif _WIN32
   if ((Socket->Error = win_connect(Socket->Handle, (struct sockaddr *)&server_address, sizeof(server_address))) != ERR::Okay) {
      log.warning("connect() failed: %s", GetErrorMsg(Socket->Error));
      return;
   }

   Socket->setState(NTC::CONNECTING); // Connection isn't complete yet - see win32_netresponse() code for NTE_CONNECT
#endif
}

//********************************************************************************************************************
// Connection timeout handler - called when the connection timeout expires

static ERR connect_timeout_handler(OBJECTPTR Subscriber, int64_t TimeElapsed, int64_t CurrentTime)
{
   pf::Log log(__FUNCTION__);
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

//********************************************************************************************************************
// TODO: Accept data-feed content for writing to the socket.

static ERR NETSOCKET_DataFeed(extNetSocket *Self, struct acDataFeed *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Disable: Disables sending and receiving on the socket.

This method will stop all sending and receiving of data over the socket.  This is irreversible.

-ERRORS-
Okay
Failed: Shutdown operation failed.

*********************************************************************************************************************/

static ERR NETSOCKET_Disable(extNetSocket *Self)
{
   pf::Log log;

   log.trace("");

#ifdef __linux__
   int result = shutdown(Self->Handle, 2);
#elif _WIN32
   int result = win_shutdown(Self->Handle, 2);
#endif

   if (result) { // Zero is success on both platforms
      log.warning("shutdown() failed.");
      return ERR::SystemCall;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DisconnectClient: Disconnects all sockets connected to a specific client IP.

For server sockets with client IP connections, this method will terminate all socket connections made to a specific
client IP and free the resources allocated to it.  If #Feedback is defined, a `DISCONNECTED` state message will also
be issued for each socket connection.

If only one socket connection needs to be disconnected, please use #DisconnectSocket().

-INPUT-
obj(NetClient) Client: The client to be disconnected.

-ERRORS-
Okay
NullArgs
WrongClass: The Client object is not of type `NetClient`.
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_DisconnectClient(extNetSocket *Self, struct ns::DisconnectClient *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Client)) return ERR::NullArgs;

   if (Args->Client->classID() != CLASSID::NETCLIENT) return log.warning(ERR::WrongClass);

   log.branch("Disconnecting client #%d", Args->Client->UID);
   free_client(Self, Args->Client);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DisconnectSocket: Disconnects a single socket that is connected to a client IP address.

This method will disconnect a socket connection for a given client.  If #Feedback is defined, a `DISCONNECTED`
state message will also be issued.

NOTE: To terminate the connection of a socket acting as the client, either free the object or return/raise
`ERR::Terminate` during #Incoming feedback.

-INPUT-
obj(ClientSocket) Socket: The client socket to be disconnected.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_DisconnectSocket(extNetSocket *Self, struct ns::DisconnectSocket *Args)
{
   pf::Log log;
   if ((!Args) or (!Args->Socket)) return log.warning(ERR::NullArgs);
   if (Args->Socket->classID() != CLASSID::CLIENTSOCKET) return log.warning(ERR::WrongClass);
   FreeResource(Args->Socket); // Disconnects & sends a Feedback message
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETSOCKET_Free(extNetSocket *Self)
{
#ifndef DISABLE_SSL
   sslDisconnect(Self);
#endif

   if (Self->TimerHandle)    { UpdateTimer(Self->TimerHandle, 0); Self->TimerHandle = 0; }
   if (Self->Address)        { FreeResource(Self->Address); Self->Address = nullptr; }
   if (Self->NetLookup)      { FreeResource(Self->NetLookup); Self->NetLookup = nullptr; }
   if (Self->SSLCertificate) { FreeResource(Self->SSLCertificate); Self->SSLCertificate = nullptr; }
   if (Self->SSLKeyPassword) { FreeResource(Self->SSLKeyPassword); Self->SSLKeyPassword = nullptr; }
   if (Self->SSLPrivateKey)  { FreeResource(Self->SSLPrivateKey); Self->SSLPrivateKey = nullptr; }

   if (Self->Feedback.isScript()) UnsubscribeAction(Self->Feedback.Context, AC::Free);
   if (Self->Incoming.isScript()) UnsubscribeAction(Self->Incoming.Context, AC::Free);
   if (Self->Outgoing.isScript()) UnsubscribeAction(Self->Outgoing.Context, AC::Free);

   while (Self->Clients) free_client(Self, Self->Clients);

   free_socket(Self);

   Self->~extNetSocket();
   return ERR::Okay;
}

//********************************************************************************************************************
// If a netsocket object is about to be freed, ensure that we are not using the netsocket object in one of our message
// handlers.  We can still delay the free request in any case.

static ERR NETSOCKET_FreeWarning(extNetSocket *Self)
{
   if (Self->InUse) {
      if (!Self->Terminating) { // Check terminating state to prevent flooding of the message queue
         pf::Log().msg("NetSocket in use, cannot free yet (request delayed).");
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
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_GetLocalIPAddress(extNetSocket *Self, struct ns::GetLocalIPAddress *Args)
{
   pf::Log log;

   log.traceBranch();

   if ((!Args) or (!Args->Address)) return log.warning(ERR::NullArgs);

   struct sockaddr_storage addr_storage;
   int result;

#ifdef __linux__
   socklen_t addr_length = sizeof(addr_storage);
   result = getsockname(Self->Handle, (struct sockaddr *)&addr_storage, &addr_length);
#elif _WIN32
   int addr_length = sizeof(addr_storage);
   result = win_getsockname(Self->Handle, (struct sockaddr *)&addr_storage, &addr_length);
#endif

   if (!result) {
      if (addr_storage.ss_family IS AF_INET6) {
         auto addr6 = (struct sockaddr_in6 *)&addr_storage;
         pf::copymem(Args->Address->Data, &addr6->sin6_addr.s6_addr, 16);
         Args->Address->Type = IPADDR::V6;
      }
      else if (addr_storage.ss_family IS AF_INET) {
         auto addr4 = (struct sockaddr_in *)&addr_storage;
         Args->Address->Data[0] = net::LongToHost(addr4->sin_addr.s_addr);
         Args->Address->Data[1] = 0;
         Args->Address->Data[2] = 0;
         Args->Address->Data[3] = 0;
         Args->Address->Type = IPADDR::V4;
      }
      else {
         log.warning("Unsupported address family: %d", addr_storage.ss_family);
         return ERR::Failed;
      }
      return ERR::Okay;
   }
   else return log.warning(ERR::SystemCall);
}

//********************************************************************************************************************
// Parse a bind address string and convert it to the appropriate sockaddr structure

static ERR parse_bind_address(CSTRING Address, bool IPv6, void *AddrOut)
{
   pf::Log log(__FUNCTION__);

   if ((!Address) or (!AddrOut)) return log.warning(ERR::NullArgs);

   IPAddress ip;
   if (net::StrToAddress(Address, &ip) IS ERR::Okay) {
      if (ip.Type IS IPADDR::V4) {
         if (IPv6) { // IPv4 -> IPv6-mapped
            auto out = (sockaddr_in6 *)AddrOut;
            pf::clearmem(out, sizeof(sockaddr_in6));
            out->sin6_family = AF_INET6;
            out->sin6_addr.s6_addr[10] = 0xff;
            out->sin6_addr.s6_addr[11] = 0xff;
            *((uint32_t*)&out->sin6_addr.s6_addr[12]) = htonl(ip.Data[0]); // Convert to network byte order
         }
         else { // IPv4 -> IPv4
            auto addr = (sockaddr_in *)AddrOut;
            pf::clearmem(addr, sizeof(struct sockaddr_in));
            addr->sin_family = AF_INET;
            addr->sin_addr.s_addr = htonl(ip.Data[0]); // Convert from host to network byte order
         }
      }
      else if (ip.Type IS IPADDR::V6) {
         if (IPv6) { // IPv6 -> IPv6
            auto out = (sockaddr_in6 *)AddrOut;
            pf::clearmem(out, sizeof(struct sockaddr_in6));
            out->sin6_family = AF_INET6;
            pf::copymem(ip.Data, &out->sin6_addr, 16);
         }
         else {
            log.warning("Address is IPv6 but socket is IPv4");
            return ERR::InvalidValue;
         }
      }
      else return log.warning(ERR::SanityCheckFailed); // Should never happen

      return ERR::Okay;
   }
   else {
      // If not an IP address, could be a hostname - however we leave it up to the user to perform the
      // name resolution in that case.
      log.warning("Bind address '%s' is not an IP address.", Address);
      return ERR::InvalidValue;
   }
}

//********************************************************************************************************************

static ERR NETSOCKET_Init(extNetSocket *Self)
{
   pf::Log log;
   ERR error;

   if (Self->Handle.is_valid()) return ERR::Okay; // The socket has been pre-configured by the developer

   if ((Self->Flags & NSF::UDP) != NSF::NIL) { // Set UDP-specific defaults
      if (!Self->MaxPacketSize) Self->MaxPacketSize = 65507; // Standard UDP max packet size
      Self->State = NTC::CONNECTED; // UDP sockets are always "connected" (ready to send/receive)
   }

#ifndef DISABLE_SSL
   // Initialise SSL ahead of any connections being made.

   if ((Self->Flags & NSF::SSL) != NSF::NIL) {
      if ((error = sslSetup(Self)) != ERR::Okay) return error;
   }
#endif

#ifdef __linux__

   // Create socket - UDP or TCP, IPv6 dual-stack if available, otherwise IPv4
   int socket_type = ((Self->Flags & NSF::UDP) != NSF::NIL) ? SOCK_DGRAM : SOCK_STREAM;
   int protocol = ((Self->Flags & NSF::UDP) != NSF::NIL) ? IPPROTO_UDP : IPPROTO_TCP;

   if (auto sock = socket(PF_INET6, socket_type, protocol); sock != -1) {
      Self->Handle = SocketHandle(sock);
      Self->IPV6 = true;

      // Enable dual-stack mode (accept both IPv4 and IPv6)
      int v6only = 0;
      if (setsockopt(Self->Handle, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0) {
         log.warning("Failed to set dual-stack mode: %s", strerror(errno));
      }

      if ((Self->Flags & NSF::UDP) IS NSF::NIL) { // TCP-specific options
         int nodelay = 1;
         setsockopt(Self->Handle, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
      }
   }
   else if (auto sock = socket(PF_INET, socket_type, protocol); sock != -1) {
      Self->Handle = SocketHandle(sock);
      Self->IPV6 = false;

      if ((Self->Flags & NSF::UDP) IS NSF::NIL) { // TCP-specific options
         int nodelay = 1;
         setsockopt(Self->Handle, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
      }
   }
   else {
      log.warning("Failed to create socket: %s", strerror(errno));
      return ERR::SystemCall;
   }

   // Put the socket into non-blocking mode, this is required when registering it as an FD and also prevents connect()
   // calls from going to sleep.

   if (fcntl(Self->Handle, F_SETFL, fcntl(Self->Handle, F_GETFL) | O_NONBLOCK)) return log.warning(ERR::SystemCall);

   // Set the send timeout so that connect() will timeout after a reasonable time

   //int timeout = 30000;
   //result = setsockopt(Self->Handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
   //assert(result IS 0);

#elif _WIN32

   bool is_ipv6;
   bool is_udp = ((Self->Flags & NSF::UDP) != NSF::NIL);
   Self->Handle = win_socket_ipv6(Self, true, false, is_ipv6, is_udp);
   if (Self->Handle.is_invalid()) return ERR::SystemCall;
   Self->IPV6 = is_ipv6;

#endif

   // Configure UDP-specific socket options

   if ((Self->Flags & NSF::UDP) != NSF::NIL) {
      if ((Self->Flags & NSF::BROADCAST) != NSF::NIL) {
         #ifdef __linux__
            int broadcast = 1;
            if (setsockopt(Self->Handle, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) != 0) {
               log.warning("Failed to enable broadcast: %s", strerror(errno));
            }
         #elif _WIN32
            // Use winsock wrapper - will be added to winsockwrappers.cpp
            if (win_enable_broadcast(Self->Handle) != ERR::Okay) {
               log.warning("Failed to enable broadcast");
            }
         #endif
      }

      if (Self->MulticastTTL > 0) {
         #ifdef __linux__
            int ttl = Self->MulticastTTL;
            if (Self->IPV6) {
               if (setsockopt(Self->Handle, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(ttl)) != 0) {
                  log.warning("Failed to set multicast TTL: %s", strerror(errno));
               }
            }
            else {
               if (setsockopt(Self->Handle, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0) {
                  log.warning("Failed to set multicast TTL: %s", strerror(errno));
               }
            }
         #elif _WIN32
            if (win_set_multicast_ttl(Self->Handle, Self->MulticastTTL, Self->IPV6) != ERR::Okay) {
               log.warning("Failed to set multicast TTL");
            }
         #endif
      }
   }

   if ((Self->Flags & NSF::SERVER) != NSF::NIL) {
      if (!Self->Port) return log.warning(ERR::FieldNotSet);
      Self->State = NTC::MULTISTATE; // Permanent value to indicate that the socket serves multiple clients.

      if (Self->IPV6) {
         #ifdef __linux__
            struct sockaddr_in6 addr;

            // Use parse_bind_address if Address is specified, otherwise bind to all interfaces
            if (Self->Address) {
               if (auto error = parse_bind_address(Self->Address, true, &addr); error != ERR::Okay) {
                  return error;
               }
               addr.sin6_port = net::HostToShort(Self->Port);
            }
            else {
               pf::clearmem(&addr, sizeof(addr));
               addr.sin6_family = AF_INET6;
               addr.sin6_port   = net::HostToShort(Self->Port); // Must be passed in in network byte order
               addr.sin6_addr   = in6addr_any;   // Must be passed in in network byte order
            }

            int result;
            int value = 1;
            setsockopt(Self->Handle, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));

            if ((result = bind(Self->Handle, (struct sockaddr *)&addr, sizeof(addr))) != -1) {
               if ((Self->Flags & NSF::UDP) IS NSF::NIL) { // TCP server - needs listen()
                  listen(Self->Handle, Self->Backlog);
                  RegisterFD(Self->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &server_accept_client, Self);
               }
               else { // UDP server - just register for data reception
                  RegisterFD(Self->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Self);
               }
               return ERR::Okay;
            }
            else if (result IS EADDRINUSE) return log.warning(ERR::InUse);
            else return log.warning(ERR::SystemCall);
         #elif _WIN32
            // Windows IPv6 dual-stack server binding
            struct sockaddr_in6 addr;

            // Use parse_bind_address if Address is specified, otherwise bind to all interfaces
            if (Self->Address) {
               if (auto error = parse_bind_address(Self->Address, true, &addr); error != ERR::Okay) {
                  return error;
               }
               addr.sin6_port = net::HostToShort(Self->Port);
            }
            else {
               pf::clearmem(&addr, sizeof(addr));
               addr.sin6_family = AF_INET6;
               addr.sin6_port   = net::HostToShort(Self->Port);
               addr.sin6_addr   = in6addr_any;
            }

            if ((error = win_bind(Self->Handle, (struct sockaddr *)&addr, sizeof(addr))) IS ERR::Okay) {
               if ((Self->Flags & NSF::UDP) IS NSF::NIL) { // TCP server - needs listen()
                  if ((error = win_listen(Self->Handle, Self->Backlog)) IS ERR::Okay) {
                     return ERR::Okay;
                  }
                  else {
                     log.warning("Listen failed on port %d, error: %s", Self->Port, GetErrorMsg(error));
                     return error;
                  }
               }
               else { // UDP server - just return success after bind
                  return ERR::Okay;
               }
            }
            else {
               log.warning("Bind failed on port %d, error: %s", Self->Port, GetErrorMsg(error));
               return error;
            }
         #else
            return ERR::NoSupport;
         #endif
      }
      else {
         // IPV4
         struct sockaddr_in addr;

         // Use parse_bind_address if Address is specified, otherwise bind to all interfaces
         if (Self->Address) {
            if (auto error = parse_bind_address(Self->Address, false, &addr); error != ERR::Okay) {
               return error;
            }
            addr.sin_port = net::HostToShort(Self->Port);
         }
         else {
            pf::clearmem(&addr, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port   = net::HostToShort(Self->Port); // Must be passed in in network byte order
            addr.sin_addr.s_addr   = INADDR_ANY;   // Must be passed in in network byte order
         }

         #ifdef __linux__
            int result;
            int value = 1;
            setsockopt(Self->Handle, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));

            if ((result = bind(Self->Handle, (struct sockaddr *)&addr, sizeof(addr))) != -1) {
               if ((Self->Flags & NSF::UDP) IS NSF::NIL) { // TCP server - needs listen()
                  listen(Self->Handle, Self->Backlog);
                  RegisterFD(Self->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &server_accept_client, Self);
               }
               else { // UDP server - just register for data reception
                  RegisterFD(Self->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Self);
               }
               return ERR::Okay;
            }
            else if (result IS EADDRINUSE) return log.warning(ERR::InUse);
            else {
               log.warning("bind() failed with error: %s", strerror(errno));
               return ERR::SystemCall;
            }
         #elif _WIN32
            if ((error = win_bind(Self->Handle, (struct sockaddr *)&addr, sizeof(addr))) IS ERR::Okay) {
               if ((Self->Flags & NSF::UDP) IS NSF::NIL) { // TCP server - needs listen()
                  if ((error = win_listen(Self->Handle, Self->Backlog)) IS ERR::Okay) {
                     return ERR::Okay;
                  }
                  else {
                     log.warning("Listen failed on port %d, error: %s", Self->Port, GetErrorMsg(error));
                     return error;
                  }
               }
               else { // UDP server - just return success after bind
                  return ERR::Okay;
               }
            }
            else {
               log.warning("Bind failed on port %d, error: %s", Self->Port, GetErrorMsg(error));
               return error;
            }
         #endif
      }
   }
   else if ((Self->Address) and (Self->Port > 0)) {
      if ((error = Self->connect(Self->Address, Self->Port, 0)) != ERR::Okay) {
         return error;
      }
      else return ERR::Okay;
   }
   else if ((Self->Flags & NSF::UDP) != NSF::NIL) {
      // UDP client sockets need to be registered for incoming data on Linux
      #ifdef __linux__
      RegisterFD(Self->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Self);
      #endif
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
cstr Group: The multicast group address to join (e.g. `224.1.1.1`).

-ERRORS-
Okay: Successfully joined the multicast group.
Args: Invalid multicast address.
NoSupport: Socket is not configured for UDP mode.
Failed: Failed to join multicast group.
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_JoinMulticastGroup(extNetSocket *Self, struct ns::JoinMulticastGroup *Args)
{
   pf::Log log;

   if ((Self->Flags & NSF::UDP) IS NSF::NIL) return ERR::NoSupport;
   if (!Args->Group) return ERR::Args;

   log.branch("%s", Args->Group);

   // Parse the multicast address
   struct sockaddr_storage mcast_addr;
   bool is_ipv6 = false;

   auto addr4 = (struct sockaddr_in *)&mcast_addr;
   auto addr6 = (struct sockaddr_in6 *)&mcast_addr;

   if (unified_inet_pton(AF_INET6, Args->Group, &addr6->sin6_addr) IS 1) is_ipv6 = true;
   else if (unified_inet_pton(AF_INET, Args->Group, &addr4->sin_addr) IS 1) is_ipv6 = false;
   else {
      log.warning("Invalid multicast address: %s", Args->Group);
      return ERR::Args;
   }

   // Join the multicast group
#ifdef __linux__
   if (is_ipv6) {
      struct ipv6_mreq mreq6;
      mreq6.ipv6mr_multiaddr = addr6->sin6_addr;
      mreq6.ipv6mr_interface = 0;
      if (setsockopt(Self->Handle, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char*)&mreq6, sizeof(mreq6)) != 0) {
         log.warning("Failed to join IPv6 multicast group: %s", strerror(errno));
         return ERR::Failed;
      }
   }
   else {
      struct ip_mreq mreq;
      mreq.imr_multiaddr = addr4->sin_addr;
      mreq.imr_interface.s_addr = INADDR_ANY;
      if (setsockopt(Self->Handle, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) != 0) {
         log.warning("Failed to join IPv4 multicast group: %s", strerror(errno));
         return ERR::Failed;
      }
   }
#elif _WIN32
   if (win_join_multicast_group(Self->Handle, Args->Group, is_ipv6) != ERR::Okay) {
      return log.warning(ERR::Failed);
   }
#endif

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
LeaveMulticastGroup: Leave a multicast group (UDP only).

This method leaves a previously joined multicast group, stopping the reception of packets sent to the specified
multicast address.

-INPUT-
cstr Group: The multicast group address to leave.

-ERRORS-
Okay: Successfully left the multicast group.
Args: Invalid multicast address.
NoSupport: Socket is not configured for UDP mode.
Failed: Failed to leave multicast group.
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_LeaveMulticastGroup(extNetSocket *Self, struct ns::LeaveMulticastGroup *Args)
{
   pf::Log log;

   if ((Self->Flags & NSF::UDP) IS NSF::NIL) return ERR::NoSupport;
   if (!Args->Group) return ERR::Args;

   log.branch("%s", Args->Group);

   // Parse the multicast address
   struct sockaddr_storage mcast_addr;
   bool is_ipv6 = false;

   auto addr4 = (struct sockaddr_in *)&mcast_addr;
   auto addr6 = (struct sockaddr_in6 *)&mcast_addr;

   if (unified_inet_pton(AF_INET6, Args->Group, &addr6->sin6_addr) IS 1) is_ipv6 = true;
   else if (unified_inet_pton(AF_INET, Args->Group, &addr4->sin_addr) IS 1) is_ipv6 = false;
   else {
      log.warning("Invalid multicast address: %s", Args->Group);
      return ERR::Args;
   }

   // Leave the multicast group
#ifdef __linux__
   if (is_ipv6) {
      struct ipv6_mreq mreq6;
      mreq6.ipv6mr_multiaddr = addr6->sin6_addr;
      mreq6.ipv6mr_interface = 0; // Use default interface

      if (setsockopt(Self->Handle, IPPROTO_IPV6, IPV6_LEAVE_GROUP, (char*)&mreq6, sizeof(mreq6)) != 0) {
         log.warning("Failed to leave IPv6 multicast group: %s", strerror(errno));
         return ERR::Failed;
      }
   }
   else {
      struct ip_mreq mreq;
      mreq.imr_multiaddr = addr4->sin_addr;
      mreq.imr_interface.s_addr = INADDR_ANY; // Use default interface

      if (setsockopt(Self->Handle, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) != 0) {
         log.warning("Failed to leave IPv4 multicast group: %s", strerror(errno));
         return ERR::Failed;
      }
   }
#elif _WIN32
   // Use winsock wrapper - will be added to winsockwrappers.cpp
   if (win_leave_multicast_group(Self->Handle, Args->Group, is_ipv6) != ERR::Okay) {
      log.warning("Failed to leave multicast group: %s", Args->Group);
      return ERR::Failed;
   }
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETSOCKET_NewPlacement(extNetSocket * Self)
{
   new (Self) extNetSocket;
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

*********************************************************************************************************************/

static ERR NETSOCKET_Read(extNetSocket *Self, struct acRead *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Buffer)) return log.warning(ERR::NullArgs);

   if ((Self->Flags & NSF::SERVER) != NSF::NIL) { // Not allowed - client must read from the ClientSocket.
      return ERR::NoSupport;
   }

   if (Self->Handle.is_invalid()) return log.warning(ERR::Disconnected);

   Self->ReadCalled = true;
   Args->Result = 0;

   if (!Args->Length) return ERR::Okay;

#ifndef DISABLE_SSL
   if (Self->SSLHandle) {
      #ifdef _WIN32
         // If we're in the middle of SSL handshake, return nothing.  The automated incoming data handler is managing the object state.
         if (Self->State IS NTC::HANDSHAKING) return log.traceWarning(ERR::InvalidState);
         else if (Self->State != NTC::CONNECTED) return log.warning(ERR::Disconnected);

         int bytes_read = 0;
         if (auto error = ssl_read(Self->SSLHandle, Args->Buffer, Args->Length, &bytes_read); error IS SSL_OK) {
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

         if (Self->HandshakeStatus IS SHS::WRITE) ssl_handshake_write(Self->Handle, Self);
         else if (Self->HandshakeStatus IS SHS::READ) ssl_handshake_read(Self->Handle, Self);

         if (Self->HandshakeStatus != SHS::NIL) { // Still handshaking
            log.trace("SSL handshake still in progress.");
            return ERR::Okay;
         }

         auto Buffer = Args->Buffer;
         auto BufferSize = Args->Length;
         do {
            read_blocked = false;
            if (auto result = SSL_read(Self->SSLHandle, Buffer, BufferSize); result <= 0) {
               auto ssl_error = SSL_get_error(Self->SSLHandle, result);
               switch (ssl_error) {
                  case SSL_ERROR_ZERO_RETURN: return log.traceWarning(ERR::Disconnected);

                  case SSL_ERROR_WANT_READ: read_blocked = true; break;

                   case SSL_ERROR_WANT_WRITE:
                     // WANT_WRITE is returned if we're trying to rehandshake and the write operation would block.  We
                     // need to wait on the socket to be writeable, then restart the read when it is.

                      log.msg("SSL socket handshake requested by server.");
                      Self->HandshakeStatus = SHS::WRITE;
                      RegisterFD(Self->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, ssl_handshake_write_netsocket, Self);
                      return ERR::Okay;

                   case SSL_ERROR_SYSCALL:
                   default:
                      log.warning("SSL read failed with error %d: %s", ssl_error, ERR_error_string(ssl_error, nullptr));
                      return ERR::Read;
               }
            }
            else {
               Args->Result += result;
               Buffer = (APTR)((char *)Buffer + result);
               BufferSize -= result;
            }
         } while ((pending = SSL_pending(Self->SSLHandle)) and (!read_blocked) and (BufferSize > 0));

         log.trace("Pending: %d, BufSize: %d, Blocked: %d", pending, BufferSize, read_blocked);

         if (pending) {
            // With regards to non-blocking SSL sockets, be aware that a socket can be empty in terms of incoming data,
            // yet SSL can keep data that has already arrived in an internal buffer.  This means that we can get stuck
            // select()ing on the socket because you aren't told that there is internal data waiting to be processed by
            // SSL_read().
            //
            // For this reason we set the RECALL flag so that we can be called again manually when we know that there is
            // data pending.

            RegisterFD(Self->Handle.hosthandle(), RFD::RECALL|RFD::READ|RFD::SOCKET, &netsocket_incoming, Self);
         }

         return ERR::Okay;
      #endif
   }
#endif

#ifdef __linux__
   auto bytes_received = recv(Self->Handle, Args->Buffer, Args->Length, 0);

   if (bytes_received > 0) {
      Args->Result = bytes_received;
      return ERR::Okay;
   }

   if (bytes_received IS 0) { // man recv() says: The return value is 0 when the peer has performed an orderly shutdown.
      return ERR::Disconnected;
   }
   else if ((errno IS EAGAIN) or (errno IS EINTR)) return ERR::Okay;
   else {
      log.warning("recv() failed: %s", strerror(errno));
      return ERR::SystemCall;
   }
#elif _WIN32
   size_t result;
   auto error = WIN_RECEIVE(Self->Handle, (char *)Args->Buffer, Args->Length, &result);
   Args->Result = result;
   return error;
#else
   #error No support for RECEIVE()
#endif
}

/*********************************************************************************************************************

-ACTION-
Write: Writes data to the socket.

Writing data to a socket will send raw data to the remote client or server.  Write connections are buffered, so any
data overflow generated in a call to this action will be buffered into a software queue.  Resource limits placed on
the software queue are governed by the #MsgLimit field setting.

Do not use this action if in server mode.  Instead, write to the @ClientSocket object that will receive the data.

It is possible to write to a socket in advance of any connection being made. The netsocket will queue the data
and automatically send it once the first connection has been made.

*********************************************************************************************************************/

static ERR NETSOCKET_Write(extNetSocket *Self, struct acWrite *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   Args->Result = 0;

   if ((Self->Flags & NSF::SERVER) != NSF::NIL) {
      log.warning("Write to the ClientSocket objects of this server.");
      return ERR::NoSupport;
   }

   if ((Self->Handle.is_invalid()) or (Self->State != NTC::CONNECTED)) { // Queue the write prior to server connection
      log.trace("Saving %d bytes to queue.", Args->Length);
      Self->WriteQueue.write(Args->Buffer, std::min<size_t>(Args->Length, Self->MsgLimit));
      return ERR::Okay;
   }

   // Note that if a write queue has been setup, there is no way that we can write to the server until the queue has
   // been exhausted.  Thus we have add more data to the queue if it already exists.

   size_t len;
   ERR error;
   if (Self->WriteQueue.Buffer.empty()) { // No prior buffer to send
      len = Args->Length;
      error = send_data(Self, Args->Buffer, &len);
      // len now reflects the total bytes that were sent to the server.
   }
   else { // Content remains in the write queue
      len = 0;
      error = ERR::BufferOverflow;
   }

   if ((error != ERR::Okay) or (len < size_t(Args->Length))) {
      if ((error IS ERR::DataSize) or (error IS ERR::BufferOverflow) or (len > 0))  {
         // Put data into the write queue and register the socket for write events
         log.trace("Error: '%s', queuing %d/%d bytes for transfer...", GetErrorMsg(error), Args->Length - len, Args->Length);
         Self->WriteQueue.write((int8_t *)Args->Buffer + len, std::min<size_t>(Args->Length - len, Self->MsgLimit));
         #ifdef __linux__
            RegisterFD(Self->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, &netsocket_outgoing, Self);
         #elif _WIN32
            win_socketstate(Self->Handle, std::nullopt, true);
         #endif
      }
      else {
         Self->ErrorCountdown--;
         if (!Self->ErrorCountdown) Self->setState(NTC::DISCONNECTED);
         return error;
      }
   }
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
buf(ptr) Buffer:    Pointer to the buffer where received data will be stored.
bufsize BufferSize: Size of the receive buffer in bytes.
&int BytesRead:     Number of bytes actually received.

-ERRORS-
Okay: Data was received successfully, or no data available.
Args: Invalid arguments provided.
NoSupport: Socket is not configured for UDP mode.
BufferOverflow: Receive buffer is too small for the incoming packet.
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_RecvFrom(extNetSocket *Self, struct ns::RecvFrom *Args)
{
   pf::Log log;

   if ((Self->Flags & NSF::UDP) IS NSF::NIL) return log.warning(ERR::NoSupport);
   if ((!Args->Buffer) or (!Args->BufferSize) or (!Args->Source)) return log.warning(ERR::NullArgs);

   Self->ReadCalled = true;

   Args->BytesRead = 0;

   struct sockaddr_storage source_addr;

#ifdef __linux__
   socklen_t addr_len = sizeof(source_addr);
   auto result = recvfrom(Self->Handle, Args->Buffer, Args->BufferSize, MSG_DONTWAIT, (struct sockaddr *)&source_addr, &addr_len);
   if (result > 0) {
      Args->BytesRead = result;
   }
   else if (result IS 0) {
      return ERR::Okay; // No data available
   }
   else {
      switch (errno) {
#if EAGAIN == EWOULDBLOCK
         case EAGAIN:
            return ERR::Okay; // No data available
#else
         case EAGAIN:
         case EWOULDBLOCK:
            return ERR::Okay; // No data available
#endif
         case EMSGSIZE:
            return ERR::BufferOverflow;
         default:
            log.warning("recvfrom() failed: %s", strerror(errno));
            return ERR::SystemCall;
      }
   }
#elif _WIN32
   int addr_len = sizeof(source_addr);
   size_t bytes_read_temp = Args->BytesRead;
   auto error = WIN_RECVFROM(Self->Handle, Args->Buffer, Args->BufferSize, &bytes_read_temp, (struct sockaddr *)&source_addr, &addr_len);
   Args->BytesRead = int(bytes_read_temp);
   if (error != ERR::Okay) return error;
#endif

   if (Args->BytesRead > 0) {
      // Populate IPAddress structure with source address and port
      if (source_addr.ss_family IS AF_INET) {
         auto addr4 = (sockaddr_in *)&source_addr;
         setIPV4(*Args->Source, ntohl(addr4->sin_addr.s_addr), ntohs(addr4->sin_port));
      }
      else if (source_addr.ss_family IS AF_INET6) {
         auto addr6 = (sockaddr_in6 *)&source_addr;
         setIPV6(*Args->Source, addr6->sin6_addr.s6_addr, ntohs(addr6->sin6_port));
      }
   }

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
-END-

*********************************************************************************************************************/

static ERR NETSOCKET_SendTo(extNetSocket *Self, struct ns::SendTo *Args)
{
   pf::Log log;

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

   struct sockaddr_storage dest_addr;
   memset(&dest_addr, 0, sizeof(dest_addr));
   int addr_len = 0;

   if (Args->Dest->Type IS IPADDR::V4) {
      if (Self->IPV6) { // IPv4-mapped IPv6
         auto addr6 = (sockaddr_in6 *)&dest_addr;
         addr6->sin6_family = AF_INET6;
         addr6->sin6_port   = htons(Args->Dest->Port);
         memset(&addr6->sin6_addr, 0, sizeof(addr6->sin6_addr));
         addr6->sin6_addr.s6_addr[10] = 0xff;
         addr6->sin6_addr.s6_addr[11] = 0xff;
         uint32_t v4_net = htonl(Args->Dest->Data[0]);
         memcpy(&addr6->sin6_addr.s6_addr[12], &v4_net, 4);
         addr_len = sizeof(sockaddr_in6);

      }
      else { // Regular IPv4
         auto addr4 = (sockaddr_in *)&dest_addr;
         addr4->sin_family = AF_INET;
         addr4->sin_port   = htons(Args->Dest->Port);
         addr4->sin_addr.s_addr = htonl(Args->Dest->Data[0]);
         memset(addr4->sin_zero, 0, sizeof(addr4->sin_zero));
         addr_len = sizeof(sockaddr_in);
      }
   }
   else if (Args->Dest->Type IS IPADDR::V6) { // Standard IPv6
      auto addr6 = (sockaddr_in6 *)&dest_addr;
      addr6->sin6_family = AF_INET6;
      addr6->sin6_port   = htons(Args->Dest->Port);
      addr6->sin6_flowinfo = 0;
      addr6->sin6_scope_id = 0;
      memcpy(&addr6->sin6_addr, Args->Dest->Data, 16);
      addr_len = sizeof(sockaddr_in6);
   }
   else return log.warning(ERR::Args);

   size_t bytes_to_send = Args->Length;

#if defined(__linux__)
   auto result = sendto(Self->Handle, Args->Data, bytes_to_send, MSG_DONTWAIT, (sockaddr *)&dest_addr, addr_len);
   if (result >= 0) {
      Args->BytesSent = (int)result;
      return ERR::Okay;
   }
   switch (errno) {
#if EAGAIN == EWOULDBLOCK
      case EAGAIN: return ERR::BufferOverflow;
#else
      case EAGAIN:
      case EWOULDBLOCK: return ERR::BufferOverflow;
#endif
      case ENETUNREACH: return ERR::NetworkUnreachable;
      case EINVAL:      return ERR::Args;
      default:
         log.warning("sendto() failed: %s", strerror(errno));
         return ERR::SystemCall;
   }
#elif defined(_WIN32)
   auto error = WIN_SENDTO(Self->Handle, Args->Data, &bytes_to_send, (sockaddr *)&dest_addr, addr_len);
   if (error IS ERR::Okay) Args->BytesSent = (int)bytes_to_send;
   return error;
#endif
}

//********************************************************************************************************************

#include "netsocket_fields.cpp"
#include "netsocket_functions.cpp"
#include "netsocket_def.c"

//********************************************************************************************************************

static const FieldArray clSocketFields[] = {
   { "Clients",          FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::NETCLIENT },
   { "ClientData",       FDF_POINTER|FDF_RW },
   { "Address",          FDF_STRING|FDF_RI, nullptr, SET_Address },
   { "SSLCertificate",   FDF_STRING|FDF_RI, nullptr, SET_SSLCertificate },
   { "SSLPrivateKey",    FDF_STRING|FDF_RI, nullptr, SET_SSLPrivateKey },
   { "SSLKeyPassword",   FDF_STRING|FDF_RI, nullptr, SET_SSLKeyPassword },
   { "State",            FDF_INT|FDF_LOOKUP|FDF_RW, GET_State, SET_State, &clNetSocketState },
   { "Error",            FDF_ERROR|FDF_R },
   { "Port",             FDF_INT|FDF_RI },
   { "Flags",            FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clNetSocketFlags },
   { "TotalClients",     FDF_INT|FDF_R },
   { "Backlog",          FDF_INT|FDF_RI },
   { "ClientLimit",      FDF_INT|FDF_RW },
   { "SocketLimit",      FDF_INT|FDF_RW },
   { "MsgLimit",         FDF_INT|FDF_RI },
   { "MaxPacketSize",    FDF_INT|FDF_RI },
   { "MulticastTTL",     FDF_INT|FDF_RI },
   // Virtual fields
   { "Handle",         FDF_POINTER|FDF_RI,     GET_Handle, SET_Handle },
   { "Feedback",       FDF_FUNCTIONPTR|FDF_RW, GET_Feedback, SET_Feedback },
   { "Incoming",       FDF_FUNCTIONPTR|FDF_RW, GET_Incoming, SET_Incoming },
   { "Outgoing",       FDF_FUNCTIONPTR|FDF_W,  GET_Outgoing, SET_Outgoing },
   { "OutQueueSize",   FDF_INT|FDF_R,          GET_OutQueueSize },
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
