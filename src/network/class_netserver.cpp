/*********************************************************************************************************************

-CLASS-
NetServer: Listens for inbound TCP or UDP communication.

The NetServer class extends NetSocket with local bind, listen and accepted-client management.  For TCP listeners,
each accepted connection is represented by a ClientSocket and grouped by client IP address in NetClient records.
For UDP listeners, incoming datagrams are received through the inherited `Incoming` callback and can be read with
`RecvFrom()`.

<header>SSL Server Certificates</>

For SSL NetServer listeners, custom certificates can be specified using the #SSLCertificate field.  Both PEM and PKCS#12
formats are supported across all platforms.

Example with PKCS#12 certificate:

<pre>
netserver = obj.new('netserver', {
   flags = 'SSL',
   port = 8443,
   sslCertificate = 'config:ssl/server.p12',
   sslKeyPassword = 'password123'
})
</pre>

Example with PEM certificate and separate private key:

<pre>
netserver = obj.new('netserver', {
   flags = 'SSL',
   port = 8443,
   sslCertificate = 'config:ssl/server.crt',
   sslPrivateKey = 'config:ssl/server.key'
})
</pre>

If no custom certificate is specified, the framework will automatically use a localhost self-signed certificate
for development purposes.  For production use, always specify a proper certificate signed by a trusted CA.

*********************************************************************************************************************/

static void free_client(extNetServer *, objNetClient *);
static void server_accept_client_impl(HOSTHANDLE, extNetServer *);
static ERR setup_server_socket(extNetServer *);

static void server_accept_client(HOSTHANDLE FD, APTR Data) {
   server_accept_client_impl(FD, (extNetServer *)Data);
}

static void server_incoming_from_client(HOSTHANDLE FD, APTR Data) {
   server_incoming_from_client_impl(FD, (extClientSocket *)Data);
}

/*********************************************************************************************************************

-METHOD-
DisconnectClient: Disconnects all sockets connected to a specific client IP.

For NetServer listeners with client IP connections, this method will terminate all socket connections made to a
specific client IP and free the resources allocated to it.  If #Feedback is defined, a `DISCONNECTED` state message
will also be issued for each socket connection.

If only one socket connection needs to be disconnected, please use #DisconnectSocket().

-INPUT-
obj(NetClient) Client: The client to be disconnected.

-ERRORS-
Okay
NullArgs
WrongClass: The Client object is not of type `NetClient`.
-END-

*********************************************************************************************************************/

static ERR NETSERVER_DisconnectClient(extNetServer *Self, struct ns::DisconnectClient *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Client)) return ERR::NullArgs;

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
`ERR::Terminate` during `Incoming` feedback.

-INPUT-
obj(ClientSocket) Socket: The client socket to be disconnected.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

static ERR NETSERVER_DisconnectSocket(extNetServer *Self, struct ns::DisconnectSocket *Args)
{
   kt::Log log;
   if ((not Args) or (not Args->Socket)) return log.warning(ERR::NullArgs);
   if (Args->Socket->classID() != CLASSID::CLIENTSOCKET) return log.warning(ERR::WrongClass);
   FreeResource(Args->Socket); // Disconnects & sends a Feedback message
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETSERVER_Free(extNetServer *Self)
{
   if (Self->SSLCertificate) { FreeResource(Self->SSLCertificate); Self->SSLCertificate = nullptr; }
   if (Self->SSLKeyPassword) { FreeResource(Self->SSLKeyPassword); Self->SSLKeyPassword = nullptr; }
   if (Self->SSLPrivateKey)  { FreeResource(Self->SSLPrivateKey); Self->SSLPrivateKey = nullptr; }

   #ifndef DISABLE_SSL
      #ifndef _WIN32
         if (Self->ServerSSLContext) {
            SSL_CTX_free(Self->ServerSSLContext);
            Self->ServerSSLContext = nullptr;
         }
      #endif
   #endif

   while (Self->Clients) free_client(Self, Self->Clients);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETSERVER_Init(extNetServer *Self)
{
   ERR error;

   if ((error = setup_server_socket(Self)) != ERR::Okay) {
      free_socket(Self);
      return error;
   }
   else return error;
}

//********************************************************************************************************************

static ERR NETSERVER_NewObject(extNetServer *Self)
{
   Self->ClientLimit    = 1024;
   Self->SocketLimit    = 256;
   Self->Backlog        = 10;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETSERVER_Read(extNetServer *Self, struct acRead *Args)
{
   // Not allowed - client must read from the ClientSocket.
   return ERR::NoSupport;
}

//********************************************************************************************************************

static ERR NETSERVER_Write(extNetServer *Self, struct acWrite *Args)
{
   kt::Log().warning("Write to the ClientSocket objects of this server.");
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
SSLCertificate: SSL certificate file to use for SSL NetServer listeners.

Set SSLCertificate to the path of an SSL certificate file to use when the NetServer is initialised with SSL enabled.
The certificate file must be in a supported format such as PEM, CRT, or P12.  If no certificate is defined, the
NetServer will either self-sign or use a localhost certificate, if available.

*********************************************************************************************************************/

static ERR NETSERVER_SET_SSLCertificate(extNetServer *Self, CSTRING Value)
{
   if (Self->SSLCertificate) { FreeResource(Self->SSLCertificate); Self->SSLCertificate = nullptr; }

   if ((Value) and (*Value)) {
      kt::Log log;

      LOC type;
      if ((AnalysePath(Value, &type) IS ERR::Okay) and (type IS LOC::FILE)) {
         if (ssl_certificate_format(Value) != SSLCERTFORMAT::NIL) {
            Self->SSLCertificate = kt::strclone(Value);
         }
         else return log.warning(ERR::InvalidData);
      }
      else return log.warning(ERR::FileNotFound);
   }

   return ERR::Okay;
}

static ERR NETSERVER_GET_SSLCertificate(extNetServer *Self, CSTRING *Value)
{
   *Value = Self->SSLCertificate;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SSLPrivateKey: Private key file to use for SSL NetServer listeners.

Set SSLPrivateKey to the path of an SSL private key file to use when the NetServer is initialised with SSL enabled.
The private key file must be in a supported format such as PEM or KEY.  If no private key is defined, the NetServer
will either self-sign or use a localhost private key, if available.

*********************************************************************************************************************/

static ERR NETSERVER_SET_SSLPrivateKey(extNetServer *Self, CSTRING Value)
{
   if (Self->SSLPrivateKey) { FreeResource(Self->SSLPrivateKey); Self->SSLPrivateKey = nullptr; }

   if ((Value) and (*Value)) {
      kt::Log log;

      LOC type;
      if ((AnalysePath(Value, &type) IS ERR::Okay) and (type IS LOC::FILE)) {
         if (ssl_private_key_format(Value) != SSLCERTFORMAT::NIL) {
            Self->SSLPrivateKey = kt::strclone(Value);
         }
         else return log.warning(ERR::InvalidData);
      }
      else return log.warning(ERR::FileNotFound);
   }
   return ERR::Okay;
}

static ERR NETSERVER_GET_SSLPrivateKey(extNetServer *Self, CSTRING *Value)
{
   *Value = Self->SSLPrivateKey;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SSLKeyPassword: SSL private key password.

If the SSL private key is encrypted, set this field to the password required to decrypt it.  If the private key is
not encrypted, this field can be left empty.

*********************************************************************************************************************/

static ERR NETSERVER_SET_SSLKeyPassword(extNetServer *Self, CSTRING Value)
{
   if (Self->SSLKeyPassword) { FreeResource(Self->SSLKeyPassword); Self->SSLKeyPassword = nullptr; }
   if ((Value) and (*Value)) Self->SSLKeyPassword = kt::strclone(Value);
   return ERR::Okay;
}

static ERR NETSERVER_GET_SSLKeyPassword(extNetServer *Self, CSTRING *Value)
{
   *Value = Self->SSLKeyPassword;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Clients: Lists all NetClient records connected to the NetServer.

*********************************************************************************************************************/

static ERR NETSERVER_GET_Clients(extNetServer *Self, objNetClient **Value)
{
   *Value = Self->Clients;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TotalClients: Indicates the total number of clients currently connected to the NetServer.

NetServer maintains a count of the total number of currently connected TCP client sockets.  You can read the total
number of connections from this field.

*********************************************************************************************************************/

static ERR NETSERVER_GET_TotalClients(extNetServer *Self, int *Value)
{
   *Value = Self->TotalClients;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Backlog: The maximum number of connections that can be queued against the socket.

Incoming TCP connections to NetServer objects are queued until they are accepted by the object.  Setting the Backlog
adjusts the maximum number of connections on the queue, which otherwise defaults to 10.

If the backlog is exceeded, subsequent connections to the socket should expect a connection refused error.

*********************************************************************************************************************/

static ERR NETSERVER_GET_Backlog(extNetServer *Self, int *Value)
{
   *Value = Self->Backlog;
   return ERR::Okay;
}

static ERR NETSERVER_SET_Backlog(extNetServer *Self, int Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->Backlog = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ClientLimit: The maximum number of clients (unique IP addresses) that can be connected to a NetServer.

The ClientLimit value limits the maximum number of IP addresses that can be connected to the socket at any one time.
For socket limits per client, see the #SocketLimit field.

*********************************************************************************************************************/

static ERR NETSERVER_GET_ClientLimit(extNetServer *Self, int *Value)
{
   *Value = Self->ClientLimit;
   return ERR::Okay;
}

static ERR NETSERVER_SET_ClientLimit(extNetServer *Self, int Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->ClientLimit = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SocketLimit: The maximum number of sockets that can be connected from a single client IP address.

The SocketLimit value limits how many simultaneous ClientSocket connections may be opened by one NetClient record.

*********************************************************************************************************************/

static ERR NETSERVER_GET_SocketLimit(extNetServer *Self, int *Value)
{
   *Value = Self->SocketLimit;
   return ERR::Okay;
}

static ERR NETSERVER_SET_SocketLimit(extNetServer *Self, int Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->SocketLimit = Value;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR activate_server_socket(extNetServer *Self, const NetworkEndpoint &Endpoint)
{
   kt::Log log(__FUNCTION__);

   if (auto error = network_platform().bind(Self->Handle, Endpoint); error != ERR::Okay) {
      log.warning("Bind failed on port %d, error: %s", Self->Port, GetErrorMsg(error));
      return error;
   }

   if ((Self->Flags & NSF::UDP) IS NSF::NIL) {
      if (auto error = network_platform().listen(Self->Handle, Self->Backlog); error != ERR::Okay) {
         log.warning("Listen failed on port %d, error: %s", Self->Port, GetErrorMsg(error));
         return error;
      }

      network_platform().register_accept(Self->Handle, &server_accept_client, Self);
   }
   else network_platform().register_read(Self->Handle, &netsocket_incoming, Self);

   return ERR::Okay;
}

static ERR setup_server_socket(extNetServer *Self)
{
   kt::Log log(__FUNCTION__);

   if (not Self->Port) return log.warning(ERR::FieldNotSet);

   Self->State = NTC::MULTISTATE; // Permanent value to indicate that the socket serves multiple clients.

   NetworkEndpoint endpoint;
   if (auto error = network_platform().prepare_bind_address(Self->Address, Self->Port, Self->IPV6, endpoint);
      error != ERR::Okay) return error;

   return activate_server_socket(Self, endpoint);
}

//********************************************************************************************************************

static void server_accept_client_impl(HOSTHANDLE SocketFD, extNetServer *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("NetServer: #%d, FD: %" PRId64, Self->UID, int64_t(SocketFD));

   kt::SwitchContext context(Self);

   auto accepted = network_platform().accept(Self, Self->Handle, Self->IPV6);
   auto clientfd = accepted.Handle;
   if (clientfd.is_invalid()) {
      log.warning("accept() failed to return an FD.");
      return;
   }

   if ((Self->Flags & NSF::KEEP_ALIVE) != NSF::NIL) {
      if (auto error = network_platform().enable_keep_alive(clientfd); error != ERR::Okay) {
         log.warning("Failed to enable TCP keep-alive on accepted socket: %s", GetErrorMsg(error));
         network_platform().close_socket(clientfd);
         return;
      }
   }

   // Accept before enforcing admission limits so IOCP-backed servers drain and close completed AcceptEx sockets.
   if ((Self->TotalClients >= Self->ClientLimit) or (Self->TotalClients >= glSocketLimit)) {
      log.error(ERR::ArrayFull);
      network_platform().close_socket(clientfd);
      return;
   }

   // Basic rate limiting - prevent connection floods

   time_t current_time = time(nullptr);
   static time_t last_accept = 0;
   static int accept_count = 0;

   if (current_time != last_accept) {
      accept_count = 1;
      last_accept = current_time;
   }
   else {
      accept_count++;
      if (accept_count > 100) { // Maximum 100 accepts per second
         log.warning("Connection rate limit exceeded, rejecting connection");
         network_platform().close_socket(clientfd);
         return;
      }
   }

   if ((accepted.Address.Type != IPADDR::V4) and (accepted.Address.Type != IPADDR::V6)) {
      log.warning("Unsupported address type: %d", int(accepted.Address.Type));
      network_platform().close_socket(clientfd);
      return;
   }

   if (accepted.Address.Type IS IPADDR::V6) log.trace("Accepted IPv6 client connection");
   else if (Self->IPV6) log.trace("Accepted IPv4 client connection on dual-stack socket");

   // Check if this IP address already has a client structure from an earlier socket connection.
   // (One NetClient represents a single IP address; Multiple ClientSockets can connect from that IP address)

   objNetClient *client_ip;
   auto accepted_ip = client_identity(accepted.Address);
   for (client_ip=Self->Clients; client_ip; client_ip=client_ip->Next) {
      if (same_client_ip(accepted_ip, client_ip->IP)) break;
   }

   if (not client_ip) {
      if (NewObject(CLASSID::NETCLIENT, &client_ip) IS ERR::Okay) {
         if (InitObject(client_ip) != ERR::Okay) {
            FreeResource(client_ip);
            network_platform().close_socket(clientfd);
            return;
         }
      }
      else {
         network_platform().close_socket(clientfd);
         return;
      }

      client_ip->IP = accepted_ip;
      client_ip->TotalConnections = 0;
      Self->TotalClients++;

      if (not Self->Clients) Self->Clients = client_ip;
      else {
         if (Self->LastClient) Self->LastClient->Next = client_ip;
         client_ip->Prev = Self->LastClient;
      }
      Self->LastClient = client_ip;
   }
   else if (client_ip->TotalConnections >= Self->SocketLimit) {
      auto label = client_ip_label(client_ip->IP);
      log.warning("Socket limit of %d reached for IP %s", Self->SocketLimit, label.c_str());
      network_platform().close_socket(clientfd);
      return;
   }

   if ((Self->Flags & NSF::MULTI_CONNECT) IS NSF::NIL) { // Check if the IP is already registered and alive
      if (client_ip->Connections) {
         auto label = client_ip_label(client_ip->IP);
         log.msg("Preventing second connection attempt from IP %s", label.c_str());
         network_platform().close_socket(clientfd);
         return;
      }
   }

   // Socket Management

   extClientSocket *client_socket;
   if (NewObject(CLASSID::CLIENTSOCKET, &client_socket) IS ERR::Okay) {
      client_socket->Handle = clientfd;
      client_socket->Client = client_ip;
      if (InitObject(client_socket) IS ERR::Okay) {
         // Note that if the connection is over SSL then handshaking won't have
         // completed yet, in which case the connection feedback will be sent in a later state change.

         if (client_socket->State IS NTC::CONNECTED) {
            if (Self->Feedback.isC()) {
               kt::SwitchContext context(Self->Feedback.Context);
               auto routine = (void (*)(extNetServer *, objClientSocket *, NTC, APTR))Self->Feedback.Routine;
               if (routine) routine(Self, client_socket, client_socket->State, Self->Feedback.Meta);
            }
            else if (Self->Feedback.isScript()) {
               sc::Call(Self->Feedback, std::to_array<ScriptArg>({
                  { "NetServer",    Self, FD_OBJECTPTR },
                  { "ClientSocket", client_socket, FD_OBJECTPTR },
                  { "State",        int(client_socket->State) }
               }));
            }
         }
      }
      else {
         log.warning(ERR::Init);
         FreeResource(client_socket);
         return;
      }
   }
   else {
      network_platform().close_socket(clientfd);
      if (not client_ip->Connections) free_client(Self, client_ip);
      return;
   }

   log.trace("Total clients: %d", Self->TotalClients);
}

//********************************************************************************************************************
// Terminates all connections for a client IP address and removes associated resources.

static void free_client(extNetServer *Server, objNetClient *Client)
{
   kt::Log log(__FUNCTION__);
   static thread_local int8_t recursive = 0;

   if (not Client) return;
   if (recursive) return;
   recursive++;

   auto label = client_ip_label(Client->IP);
   log.branch("%s, Connections: %d", label.c_str(), Client->TotalConnections);

   // Free all sockets (connections) related to this client IP

   while (Client->Connections) {
      objClientSocket *current_socket = Client->Connections;
      FreeResource(current_socket); // Disconnects & sends a Feedback message
      if (Client->Connections IS current_socket) { // Sanity check
         log.warning(ERR::SanityCheckFailed);
         break;
      }
   }

   if (Client->Prev) {
      Client->Prev->Next = Client->Next;
      if (Client->Next) Client->Next->Prev = Client->Prev;
   }
   else {
      Server->Clients = Client->Next;
      if (Server->Clients) Server->Clients->Prev = nullptr;
   }

   FreeResource(Client);

   Server->TotalClients--;

   recursive--;
}

//********************************************************************************************************************

#include "netserver_def.c"

static const FieldArray clNetServerFields[] = {
   { "TotalClients",   FDF_VIRTUAL|FDF_INT|FDF_R,     NETSERVER_GET_TotalClients },
   { "Backlog",        FDF_VIRTUAL|FDF_INT|FDF_RI,    NETSERVER_GET_Backlog, NETSERVER_SET_Backlog },
   { "ClientLimit",    FDF_VIRTUAL|FDF_INT|FDF_RW,    NETSERVER_GET_ClientLimit, NETSERVER_SET_ClientLimit },
   { "SocketLimit",    FDF_VIRTUAL|FDF_INT|FDF_RW,    NETSERVER_GET_SocketLimit, NETSERVER_SET_SocketLimit },
   { "SSLCertificate", FDF_VIRTUAL|FDF_STRING|FDF_RI, NETSERVER_GET_SSLCertificate, NETSERVER_SET_SSLCertificate },
   { "SSLPrivateKey",  FDF_VIRTUAL|FDF_STRING|FDF_RI, NETSERVER_GET_SSLPrivateKey, NETSERVER_SET_SSLPrivateKey },
   { "SSLKeyPassword", FDF_VIRTUAL|FDF_STRING|FDF_RI, NETSERVER_GET_SSLKeyPassword, NETSERVER_SET_SSLKeyPassword },
   { "Clients",        FDF_VIRTUAL|FDF_OBJECT|FDF_R,  NETSERVER_GET_Clients, nullptr, CLASSID::NETCLIENT },
   END_FIELD
};

static ERR init_netserver(void)
{
   clNetServer = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::NETSOCKET),
      fl::ClassID(CLASSID::NETSERVER),
      fl::Name("NetServer"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clNetServerActions),
      fl::Methods(clNetServerMethods),
      fl::Fields(clNetServerFields),
      fl::Size(sizeof(extNetServer)),
      fl::Path(MOD_PATH));

   return clNetServer ? ERR::Okay : ERR::AddClass;
}
