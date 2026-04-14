
//********************************************************************************************************************

static void free_socket(extNetSocket *Self)
{
   pf::Log log(__FUNCTION__);

   log.branch("Handle: %d", Self->Handle.int_value());

   if (Self->Handle.is_valid()) {
      log.trace("Deregistering socket.");
      DeregisterFD(Self->Handle.hosthandle());

      if (!Self->ExternalSocket) CLOSESOCKET_THREADED(Self->Handle);
      Self->Handle = SocketHandle();
   }

   Self->WriteQueue.Buffer.clear();
   Self->WriteQueue.Index = 0;

   if (!Self->terminating()) {
      if (Self->State != NTC::DISCONNECTED) Self->setState(NTC::DISCONNECTED);
   }

   log.trace("Resetting exception handler.");

   SetResourcePtr(RES::EXCEPTION_HANDLER, nullptr); // Stop winsock from fooling with our exception handler
}

//********************************************************************************************************************
// Store data in the queue

ERR NetQueue::write(CPTR Message, size_t Length)
{
   pf::Log log(__FUNCTION__);

   if (!Message) return log.warning(ERR::NullArgs);
   if (Length <= 0) return ERR::Okay;

   // Security: Check for maximum buffer size to prevent memory exhaustion
   constexpr size_t MAX_QUEUE_SIZE = 16 * 1024 * 1024; // 16MB limit
   if (Length > MAX_QUEUE_SIZE) return log.warning(ERR::DataSize);

   if (!Buffer.empty()) { // Add data to existing queue
      size_t remaining_data = Buffer.size() - Index;

      if (Index > 8192) { // Compact the queue
         if (remaining_data > 0) Buffer.erase(Buffer.begin(), Buffer.begin() + Index);
         else Buffer.clear();
         Index = 0;
      }

      // Security: Check for integer overflow and buffer size limits
      if (Buffer.size() > MAX_QUEUE_SIZE - Length) return log.warning(ERR::BufferOverflow);

      size_t old_size = Buffer.size();
      Buffer.resize(old_size + Length);
      pf::copymem(Message, Buffer.data() + old_size, Length);
   }
   else {
      Buffer.resize(Length);
      Index = 0;
      pf::copymem(Message, Buffer.data(), Length);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// This function is called from winsockwrappers.c whenever a network event occurs on a NetSocket.  Callbacks
// set against the NetSocket object will send/receive data on the socket.
//
// Recursion typically occurs on calls to ProcessMessages() during incoming and outgoing data transmissions.  This is
// not important if the same transmission message is being repeated, but does require careful management if, for
// example, a disconnection were to occur during a read/write operation for example.  Using DataFeeds is a more
// reliable method of managing recursion problems, but burdens the message queue.

#ifdef _WIN32
void win32_netresponse(OBJECTPTR SocketObject, SOCKET_HANDLE Handle, int Message, ERR Error)
{
   pf::Log log(__FUNCTION__);

   extNetSocket *Socket;
   extClientSocket *ClientSocket;

   if (SocketObject->terminating()) { // Sanity check
      log.warning(ERR::MarkedForDeletion);
      return;
   }

   if (SocketObject->classID() IS CLASSID::CLIENTSOCKET) {
      ClientSocket = (extClientSocket *)SocketObject;
      Socket = (extNetSocket *)ClientSocket->Client->Owner;
      if (ClientSocket->Handle != Handle) {
         log.warning(ERR::SanityCheckFailed);
         return;
      }
   }
   else {
      Socket = (extNetSocket *)SocketObject;
      ClientSocket = nullptr;
      if (Socket->Handle != Handle) {
         log.warning(ERR::SanityCheckFailed);
         return;
      }
   }

   #ifndef NDEBUG
   static constexpr const char* const msg[] = { "None", "Write", "Read", "Accept", "Connect", "Close" };
   log.traceBranch("[%d:%d:%p], %s, Error %d, InUse: %d, WinRecursion: %d", Socket->UID, Handle, ClientSocket, msg[Message], int(Error), Socket->InUse, Socket->WinRecursion);
   #endif

   // Safety first

   pf::ScopedObjectLock lock(Socket);
   if (!lock.granted()) return;

   pf::ScopedObjectLock lock_client(ClientSocket); // Not locked if ClientSocket is nullptr
   if ((ClientSocket) and (!lock_client.granted())) return;

   pf::SwitchContext context(Socket);

   Socket->InUse++;

   if (Message IS NTE_READ) {
      if (Error != ERR::Okay) log.warning("Socket failed on incoming data, error %d.", int(Error));

      if (Socket->WinRecursion) log.traceWarning(ERR::Recursion);
      else {
         Socket->WinRecursion++;
         #pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
         if (ClientSocket) server_incoming_from_client((HOSTHANDLE)Handle, ClientSocket);
         else netsocket_incoming(0, Socket);
         #pragma GCC diagnostic warning "-Wint-to-pointer-cast"
         Socket->WinRecursion--;
      }
   }
   else if (Message IS NTE_WRITE) {
      if (Error != ERR::Okay) log.warning("Socket failed on outgoing data, error %d.", int(Error));

      if (Socket->WinRecursion) log.traceWarning(ERR::Recursion);
      else {
         Socket->WinRecursion++;
         #pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
         if (ClientSocket) clientsocket_outgoing((HOSTHANDLE)Handle, ClientSocket);
         else netsocket_outgoing(0, Socket);
         #pragma GCC diagnostic warning "-Wint-to-pointer-cast"
         Socket->WinRecursion--;
      }
   }
   else if (Message IS NTE_CLOSE) {
      if (ClientSocket) {
         log.branch("Client socket closed.");
         FreeResource(ClientSocket);
         // Note: Disconnection feedback is sent to the NetSocket by the ClientSocket destructor.
      }
      else {
         log.branch("Connection closed by host, error %d.", int(Error));

         // Prevent multiple close messages from the same socket
         if (Socket->State IS NTC::DISCONNECTED) {
            log.trace("Ignoring duplicate close message for socket %d", Handle);
            Socket->InUse--;
            return;
         }

         Socket->setState(NTC::DISCONNECTED);
         free_socket(Socket);
      }
   }
   else if (Message IS NTE_ACCEPT) {
      log.traceBranch("Accept message received for new client %d.", Handle);
      server_accept_client(Socket->Handle, Socket);
   }
   else if (Message IS NTE_CONNECT) {
      if (Error IS ERR::Okay) {
         if (ClientSocket) { // Server mode - connect message shouldn't be received for ClientSocket
            log.warning("Unexpected connect message for ClientSocket, ignoring.");
            Socket->InUse--;
            return;
         }
         else {
            log.traceBranch("Connection to host granted.");

            if (Socket->TimerHandle) { UpdateTimer(Socket->TimerHandle, 0); Socket->TimerHandle = 0; }

            #ifndef DISABLE_SSL
               if (Socket->SSLHandle) sslConnect(Socket);
               else Socket->setState(NTC::CONNECTED);
            #else
               Socket->setState(NTC::CONNECTED);
            #endif
         }
      }
      else {
         log.msg("Connection state changed, error: %s", GetErrorMsg(Error));

         if (Socket->TimerHandle) { UpdateTimer(Socket->TimerHandle, 0); Socket->TimerHandle = 0; }

         Socket->Error = Error;
         Socket->setState(NTC::DISCONNECTED);
      }
   }

   Socket->InUse--;
}
#endif

//********************************************************************************************************************
// Called when a server socket handle detects a new client wanting to connect to it.
// Used by Win32 (Windows message loop) & Linux (FD hook)

static void server_accept_client_impl(HOSTHANDLE SocketFD, extNetSocket *Self)
{
   pf::Log log(__FUNCTION__);
   uint8_t ip[8];
   SocketHandle clientfd;

   log.traceBranch("NetSocket: #%d, FD: %" PRId64, Self->UID, int64_t(SocketFD));

   pf::SwitchContext context(Self);

   // Check client limit before accepting to prevent resource exhaustion
   if ((Self->TotalClients >= Self->ClientLimit) or (Self->TotalClients >= glSocketLimit)) {
      log.error(ERR::ArrayFull);
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
         return;
      }
   }

   if (Self->IPV6) {
      #ifdef __linux__
         // For dual-stack sockets, use sockaddr_storage to handle both IPv4 and IPv6
         struct sockaddr_storage addr_storage;
         socklen_t len = sizeof(addr_storage);
         clientfd = accept(SocketFD, (struct sockaddr *)&addr_storage, &len);
         if (clientfd.is_invalid()) return;

         int nodelay = 1;
         setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

         if (addr_storage.ss_family IS AF_INET6) { // IPv6 connection
            struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr_storage;
            ip[0] = addr6->sin6_addr.s6_addr[0];
            ip[1] = addr6->sin6_addr.s6_addr[1];
            ip[2] = addr6->sin6_addr.s6_addr[2];
            ip[3] = addr6->sin6_addr.s6_addr[3];
            ip[4] = addr6->sin6_addr.s6_addr[4];
            ip[5] = addr6->sin6_addr.s6_addr[5];
            ip[6] = addr6->sin6_addr.s6_addr[6];
            ip[7] = addr6->sin6_addr.s6_addr[7];
            log.trace("Accepted IPv6 client connection");
         }
         else if (addr_storage.ss_family IS AF_INET) { // IPv4 connection on dual-stack socket
            struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr_storage;
            uint32_t ipv4_addr = net::LongToHost(addr4->sin_addr.s_addr);
            ip[0] = ipv4_addr & 0xff;
            ip[1] = (ipv4_addr >> 8) & 0xff;
            ip[2] = (ipv4_addr >> 16) & 0xff;
            ip[3] = (ipv4_addr >> 24) & 0xff;
            ip[4] = ip[5] = ip[6] = ip[7] = 0;
            log.trace("Accepted IPv4 client connection on dual-stack socket");
         }
         else {
            log.warning("Unsupported address family: %d", addr_storage.ss_family);
            close(clientfd);
            return;
         }
      #elif _WIN32
         // Windows IPv6 dual-stack accept using wrapper function
         int family;
         struct sockaddr_storage addr_storage;
         int len = sizeof(addr_storage);
         clientfd = win_accept_ipv6(Self, WSW_SOCKET((uintptr_t)SocketFD), (struct sockaddr *)&addr_storage, &len, &family);
         if (clientfd IS NOHANDLE) return;

         if (family IS AF_INET6) { // IPv6 connection
            struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr_storage;
            ip[0] = addr6->sin6_addr.s6_addr[0];
            ip[1] = addr6->sin6_addr.s6_addr[1];
            ip[2] = addr6->sin6_addr.s6_addr[2];
            ip[3] = addr6->sin6_addr.s6_addr[3];
            ip[4] = addr6->sin6_addr.s6_addr[4];
            ip[5] = addr6->sin6_addr.s6_addr[5];
            ip[6] = addr6->sin6_addr.s6_addr[6];
            ip[7] = addr6->sin6_addr.s6_addr[7];
            log.trace("Accepted IPv6 client connection on Windows");
         }
         else if (family IS AF_INET) { // IPv4 connection on dual-stack socket
            struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr_storage;
            uint32_t ipv4_addr = net::LongToHost(addr4->sin_addr.s_addr);
            ip[0] = ipv4_addr & 0xff;
            ip[1] = (ipv4_addr >> 8) & 0xff;
            ip[2] = (ipv4_addr >> 16) & 0xff;
            ip[3] = (ipv4_addr >> 24) & 0xff;
            ip[4] = ip[5] = ip[6] = ip[7] = 0;
            log.trace("Accepted IPv4 client connection on dual-stack socket (Windows)");
         }
         else {
            log.warning("Unsupported address family on Windows: %d", family);
            CLOSESOCKET(clientfd);
            return;
         }
      #else
         #warning Platform requires IPV6 support.
         return;
      #endif
   }
   else {
      struct sockaddr_in addr;

      #ifdef __linux__
         socklen_t len = sizeof(addr);
         clientfd = accept(SocketFD, (struct sockaddr *)&addr, &len);

         if (clientfd.is_valid()) {
            int nodelay = 1;
            setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
         }
      #elif _WIN32
         int len = sizeof(addr);
         clientfd = win_accept(Self, WSW_SOCKET((uintptr_t)SocketFD), (struct sockaddr *)&addr, &len);
      #endif

      if (clientfd.is_invalid()) {
         log.warning("accept() failed to return an FD.");
         return;
      }

      ip[0] = addr.sin_addr.s_addr;
      ip[1] = addr.sin_addr.s_addr>>8;
      ip[2] = addr.sin_addr.s_addr>>16;
      ip[3] = addr.sin_addr.s_addr>>24;
      ip[4] = 0;
      ip[5] = 0;
      ip[6] = 0;
      ip[7] = 0;
   }

   // Check if this IP address already has a client structure from an earlier socket connection.
   // (One NetClient represents a single IP address; Multiple ClientSockets can connect from that IP address)

   objNetClient *client_ip;
   for (client_ip=Self->Clients; client_ip; client_ip=client_ip->Next) {
      if (((int64_t *)&ip)[0] IS ((int64_t *)&client_ip->IP)[0]) break;
   }

   if (!client_ip) {
      if (NewObject(CLASSID::NETCLIENT, &client_ip) IS ERR::Okay) {
         if (InitObject(client_ip) != ERR::Okay) {
            FreeResource(client_ip);
            CLOSESOCKET(clientfd);
            return;
         }
      }
      else {
         CLOSESOCKET(clientfd);
         return;
      }

      ((int64_t *)&client_ip->IP)[0] = ((int64_t *)&ip)[0];
      client_ip->TotalConnections = 0;
      Self->TotalClients++;

      if (!Self->Clients) Self->Clients = client_ip;
      else {
         if (Self->LastClient) Self->LastClient->Next = client_ip;
         if (Self->Clients) Self->Clients->Prev = Self->LastClient;
      }
      Self->LastClient = client_ip;
   }
   else if (client_ip->TotalConnections >= Self->SocketLimit) {
      log.warning("Socket limit of %d reached for IP %d.%d.%d.%d", Self->SocketLimit, client_ip->IP[0], client_ip->IP[1], client_ip->IP[2], client_ip->IP[3]);
      CLOSESOCKET(clientfd);
      return;
   }

   if ((Self->Flags & NSF::MULTI_CONNECT) IS NSF::NIL) { // Check if the IP is already registered and alive
      if (client_ip->Connections) {
         log.msg("Preventing second connection attempt from IP %d.%d.%d.%d", client_ip->IP[0], client_ip->IP[1], client_ip->IP[2], client_ip->IP[3]);
         CLOSESOCKET(clientfd);
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
               pf::SwitchContext context(Self->Feedback.Context);
               auto routine = (void (*)(extNetSocket *, objClientSocket *, NTC, APTR))Self->Feedback.Routine;
               if (routine) routine(Self, client_socket, client_socket->State, Self->Feedback.Meta);
            }
            else if (Self->Feedback.isScript()) {
               sc::Call(Self->Feedback, std::to_array<ScriptArg>({
                  { "NetSocket",    Self, FD_OBJECTPTR },
                  { "ClientSocket", client_socket, FD_OBJECTPTR },
                  { "State",        int(client_socket->State) }
               }));
            }
         }
      }
      else log.warning(ERR::Init);
   }
   else {
      CLOSESOCKET(clientfd);
      if (!client_ip->Connections) free_client(Self, client_ip);
      return;
   }

   log.trace("Total clients: %d", Self->TotalClients);
}

//********************************************************************************************************************
// Terminates all connections for a client IP address and removes associated resources.

static void free_client(extNetSocket *Socket, objNetClient *Client)
{
   pf::Log log(__FUNCTION__);
   static thread_local int8_t recursive = 0;

   if (!Client) return;
   if ((Socket->Flags & NSF::SERVER) IS NSF::NIL) return; // Must be a server

   if (recursive) return;
   recursive++;

   log.branch("%d:%d:%d:%d, Connections: %d", Client->IP[0], Client->IP[1], Client->IP[2], Client->IP[3], Client->TotalConnections);

   // Free all sockets (connections) related to this client IP

   while (Client->Connections) {
      objClientSocket *current_socket = Client->Connections;
      FreeResource(current_socket); // Disconnects & sends a Feedback message
      if (Client->Connections IS current_socket) { // Sanity check
         log.warning("Resource management error detected in Client->Sockets");
         break;
      }
   }

   if (Client->Prev) {
      Client->Prev->Next = Client->Next;
      if (Client->Next) Client->Next->Prev = Client->Prev;
   }
   else {
      Socket->Clients = Client->Next;
      if ((Socket->Clients) and (Socket->Clients->Next)) Socket->Clients->Next->Prev = nullptr;
   }

   FreeResource(Client);

   Socket->TotalClients--;

   recursive--;
}

//********************************************************************************************************************
// See win32_netresponse() for the Windows version.

#ifdef __linux__
static void netsocket_connect_impl(HOSTHANDLE SocketFD, extNetSocket *Self)
{
   pf::Log log(__FUNCTION__);

   pf::SwitchContext context(Self);

   log.trace("Connection from server received.");

   int result = EHOSTUNREACH; // Default error in case getsockopt() fails
   socklen_t optlen = sizeof(result);
   getsockopt(SocketFD, SOL_SOCKET, SO_ERROR, &result, &optlen);

   // Remove the write callback

   RegisterFD(Self->Handle.hosthandle(), RFD::WRITE|RFD::REMOVE, &netsocket_connect, Self);

   #ifndef DISABLE_SSL
   if ((Self->SSLHandle) and (!result)) {
      // Perform the SSL handshake

      log.traceBranch("Attempting SSL handshake.");

      sslConnect(Self);
      if (Self->Error != ERR::Okay) return;

      if (Self->State IS NTC::HANDSHAKING) {
         RegisterFD(Self->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Self);
      }
      return;
   }
   #endif

   if (!result) {
      log.traceBranch("Connection succesful.");

      if (Self->TimerHandle) { UpdateTimer(Self->TimerHandle, 0); Self->TimerHandle = 0; }

      Self->setState(NTC::CONNECTED);
      RegisterFD(Self->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &netsocket_incoming, Self);
      return;
   }
   else {
      log.trace("getsockopt() result %d", result);

      if (Self->TimerHandle) { UpdateTimer(Self->TimerHandle, 0); Self->TimerHandle = 0; }

      if (result IS ECONNREFUSED)      Self->Error = ERR::ConnectionRefused;
      else if (result IS ENETUNREACH)  Self->Error = ERR::NetworkUnreachable;
      else if (result IS EHOSTUNREACH) Self->Error = ERR::HostUnreachable;
      else if (result IS ETIMEDOUT)    Self->Error = ERR::TimeOut;
      else Self->Error = ERR::SystemCall;

      log.error(Self->Error);

      Self->setState(NTC::DISCONNECTED);
   }
}
#endif

//********************************************************************************************************************
// If the socket is the client of a server, messages from the server will come in through here.
//
// Incoming information from the server can be read with either the Incoming callback routine (the developer is
// expected to call the Read action from this).
//
// This function is called from win32_netresponse() and is managed outside of the normal message queue.

static void netsocket_incoming_impl(HOSTHANDLE SocketFD, extNetSocket *Self)
{
   pf::Log log(__FUNCTION__);

   pf::SwitchContext context(Self); // Set context & lock

   if ((Self->Flags & NSF::UDP) IS NSF::NIL) {
      if ((Self->Flags & NSF::SERVER) != NSF::NIL) { // Sanity check
         log.warning("Invalid call from server socket.");
         return;
      }
   }

   if (Self->Terminating) { // Set by FreeWarning()
      log.trace("Socket terminating...", Self->UID);
      if (Self->Handle.is_valid()) free_socket(Self);
      return;
   }

#ifndef DISABLE_SSL
  #ifdef _WIN32
   if ((Self->SSLHandle) and (Self->State IS NTC::HANDSHAKING)) {
      pf::Log log(__FUNCTION__);
      log.traceBranch("Windows SSL handshake in progress, reading raw data.");
      size_t result;
      std::vector<uint8_t> buffer;
      if (ERR error = WIN_APPEND(Self->Handle, buffer, 4096, result); error IS ERR::Okay) {
         sslHandshakeReceived(Self, buffer.data(), int(buffer.size()));

         if ((Self->State != NTC::CONNECTED) or (!ssl_has_decrypted_data(Self->SSLHandle) and !ssl_has_encrypted_data(Self->SSLHandle))) {
            // In most cases we return without further processing unless we're definitely connected and
            // there is data sitting in the queue or SSL has data available (decrypted or encrypted).
            return;
         }
      }
      else {
         log.warning(error);
         return;
      }
   }

  #else
    if ((Self->SSLHandle) and (Self->State IS NTC::HANDSHAKING)) {
      log.traceBranch("Continuing SSL handshake...");
      sslConnect(Self);
      return;
    }

    if (Self->HandshakeStatus != SHS::NIL) { // TODO: Check State is not HANDSHAKING instead
      log.trace("SSL is handshaking.");
      return;
    }
  #endif
#endif

   if (Self->IncomingRecursion) {
      log.trace("[NetSocket:%d] Recursion detected on handle %" PRId64, Self->UID, int64_t(SocketFD));
      if (Self->IncomingRecursion < 2) Self->IncomingRecursion++; // Indicate that there is more data to be received
      return;
   }

   log.traceBranch("[NetSocket:%d] Socket: %" PRId64, Self->UID, int64_t(SocketFD));

   Self->InUse++;
   Self->IncomingRecursion++;

restart:

   // The Incoming callback will normally be defined by the user and is expected to call the Read() action.
   // Otherwise we clear the unprocessed content.

   Self->ReadCalled = false;
   auto error = ERR::Okay;
   if (Self->Incoming.defined()) {
      if (Self->Incoming.isC()) {
         auto routine = (ERR (*)(extNetSocket *, APTR))Self->Incoming.Routine;
         pf::SwitchContext context(Self->Incoming.Context);
         error = routine(Self, Self->Incoming.Meta);
      }
      else if (Self->Incoming.isScript()) {
         if (sc::Call(Self->Incoming, std::to_array<ScriptArg>({ { "NetSocket", Self, FD_OBJECTPTR } }), error) != ERR::Okay) error = ERR::Terminate;
      }

      if (error IS ERR::Terminate) log.trace("Termination of socket requested by channel subscriber.");
      else if (!Self->ReadCalled) log.warning("[NetSocket:%d] Subscriber did not call Read()", Self->UID);
   }

   if (!Self->ReadCalled) {
      log.trace("Clearing unprocessed data from socket %d", Self->UID);

      std::array<char,1024> buffer;
      int total = 0;
      int result;
      do {
         error = acRead(Self, buffer.data(), buffer.size(), &result);
         total += result;
      } while (result > 0);

      if (error != ERR::Okay) error = ERR::Terminate;
   }

   if (error IS ERR::Terminate) {
      log.traceBranch("Socket % " PRId64 " will be terminated.", int64_t(SocketFD));
      if (Self->Handle.is_valid()) free_socket(Self);
   }
   else if (Self->IncomingRecursion > 1) {
      // If netsocket_incoming() was called again during the callback, there is more
      // data available and we should repeat our callback so that the client can receive the rest
      // of the data.

      Self->IncomingRecursion = 1;
      goto restart;
   }
#ifndef DISABLE_SSL
 #ifdef _WIN32
   else if (Self->SSLHandle and (ssl_has_decrypted_data(Self->SSLHandle) or ssl_has_encrypted_data(Self->SSLHandle))) {
      // SSL has buffered data that needs processing - continue without waiting for socket notification
      log.trace("SSL has buffered data, continuing processing");
      Self->IncomingRecursion = 1;
      goto restart;
   }
 #endif
#endif

   Self->InUse--;
   Self->IncomingRecursion = 0;
}

//********************************************************************************************************************
// This function sends data to the client if there is queued data waiting to go out.  Otherwise it does nothing.
//
// Note: This function will prevent the task from going to sleep if it is not managed correctly.  If
// no data is being written to the queue, the program will not be able to sleep until the client stops listening
// to the write queue.
//
// Called from either the Windows messaging logic or a Linux FD subscription.

static void netsocket_outgoing_impl(HOSTHANDLE SocketFD, extNetSocket *Self)
{
   pf::Log log(__FUNCTION__);

   pf::SwitchContext context(Self); // Set context & lock

   if (Self->Terminating) return;

   if (Self->State IS NTC::HANDSHAKING) {
      log.trace("Handshaking...");
      return;
   }

   if (Self->OutgoingRecursion) {
      log.traceWarning(ERR::Recursion);
      return;
   }

   log.traceBranch();

   Self->InUse++;
   Self->OutgoingRecursion++;

   auto error = ERR::Okay;

   // Send out remaining queued data before getting new data to send

   while (!Self->WriteQueue.Buffer.empty()) {
      size_t len = Self->WriteQueue.Buffer.size() - Self->WriteQueue.Index;
      #ifndef DISABLE_SSL
         if ((!Self->SSLHandle) and (len > glMaxWriteLen)) len = glMaxWriteLen;
      #else
         if (len > glMaxWriteLen) len = glMaxWriteLen;
      #endif

      if (len > 0) {
         error = send_data(Self, Self->WriteQueue.Buffer.data() + Self->WriteQueue.Index, &len);
         if ((error != ERR::Okay) or (!len)) break;
         log.trace("Sent %d of %d bytes from the queue.", Self->UID, int(len), int(Self->WriteQueue.Buffer.size() - Self->WriteQueue.Index));
         Self->WriteQueue.Index += len;
      }

      if (Self->WriteQueue.Index >= Self->WriteQueue.Buffer.size()) {
         Self->WriteQueue.Buffer.clear();
         Self->WriteQueue.Index = 0;
         break;
      }
   }

   // Before feeding new data into the queue, the current buffer must be empty.

   if ((Self->WriteQueue.Buffer.empty()) or
       (Self->WriteQueue.Index >= Self->WriteQueue.Buffer.size())) {
      if (Self->Outgoing.defined()) {
         if (Self->Outgoing.isC()) {
            auto routine = (ERR (*)(extNetSocket *, APTR))Self->Outgoing.Routine;
            pf::SwitchContext context(Self->Outgoing.Context);
            error = routine(Self, Self->Outgoing.Meta);
         }
         else if (Self->Outgoing.isScript()) {
            if (sc::Call(Self->Outgoing, std::to_array<ScriptArg>({ { "NetSocket", Self, FD_OBJECTPTR } }), error) != ERR::Okay) error = ERR::Terminate;
         }

         if (error != ERR::Okay) Self->Outgoing.clear();
      }

      // If the write queue is empty and all data has been retrieved, we can remove the FD-Write registration so that
      // we don't tax the system resources.  The WriteSocket function is also dropped because it is intended to
      // be assigned temporarily.

      if ((!Self->Outgoing.defined()) and (Self->WriteQueue.Buffer.empty())) {
         log.trace("Write-queue listening on socket %d will now stop.", Self->UID, Self->Handle.int_value());
         #ifdef __linux__
            RegisterFD(Self->Handle.hosthandle(), RFD::REMOVE|RFD::WRITE|RFD::SOCKET, nullptr, nullptr);
         #elif _WIN32
            if (auto error = win_socketstate(Self->Handle, std::nullopt, false); error != ERR::Okay) log.warning(error);
         #endif
      }

      if (error != ERR::Okay) {
         Self->ErrorCountdown--;
         if (!Self->ErrorCountdown) Self->setState(NTC::DISCONNECTED);
      }
   }

   Self->InUse--;
   Self->OutgoingRecursion--;
}
