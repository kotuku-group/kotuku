
//********************************************************************************************************************

static void free_socket(extNetSocket *Self)
{
   kt::Log log(__FUNCTION__);

   log.branch("Handle: %d", Self->Handle.int_value());

   if (Self->Handle.is_valid()) {
      log.trace("Deregistering socket.");
      network_platform().deregister_fd(Self->Handle);

      if (!Self->ExternalSocket) CLOSESOCKET_THREADED(Self->Handle);
      Self->Handle = SocketHandle();
   }

   Self->WriteQueue.Buffer.clear();
   Self->WriteQueue.Index = 0;

   if (!Self->terminating()) {
      if (Self->State != NTC::DISCONNECTED) Self->setState(NTC::DISCONNECTED);
   }
}

//********************************************************************************************************************
// Store data in the queue.

ERR NetQueue::write(CPTR Message, size_t Length)
{
   kt::Log log(__FUNCTION__);

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
      kt::copymem(Message, Buffer.data() + old_size, Length);
   }
   else {
      Buffer.resize(Length);
      Index = 0;
      kt::copymem(Message, Buffer.data(), Length);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static IPAddress client_identity(IPAddress Address)
{
   Address.Port = 0;
   return Address;
}

static bool same_client_ip(const IPAddress &Left, const IPAddress &Right)
{
   if (Left.Type != Right.Type) return false;

   if (Left.Type IS IPADDR::V4) return Left.Data[0] IS Right.Data[0];
   else if (Left.Type IS IPADDR::V6) return std::memcmp(Left.Data, Right.Data, sizeof(Left.Data)) IS 0;
   else return false;
}

static std::string client_ip_label(const IPAddress &Address)
{
   IPAddress printable = client_identity(Address);
   CSTRING value = net::AddressToStr(&printable);
   std::string label = value ? value : "<invalid>";
   if (value) FreeResource((APTR)value);
   return label;
}

//********************************************************************************************************************

static void server_accept_client_impl(HOSTHANDLE SocketFD, extNetSocket *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("NetSocket: #%d, FD: %" PRId64, Self->UID, int64_t(SocketFD));

   kt::SwitchContext context(Self);

   auto accepted = network_platform().accept(Self, Self->Handle, Self->IPV6);
   auto clientfd = accepted.Handle;
   if (clientfd.is_invalid()) {
      log.warning("accept() failed to return an FD.");
      return;
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

   if (!client_ip) {
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

      if (!Self->Clients) Self->Clients = client_ip;
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
      else {
         log.warning(ERR::Init);
         FreeResource(client_socket);
         return;
      }
   }
   else {
      network_platform().close_socket(clientfd);
      if (!client_ip->Connections) free_client(Self, client_ip);
      return;
   }

   log.trace("Total clients: %d", Self->TotalClients);
}

//********************************************************************************************************************
// Terminates all connections for a client IP address and removes associated resources.

static void free_client(extNetSocket *Socket, objNetClient *Client)
{
   kt::Log log(__FUNCTION__);
   static thread_local int8_t recursive = 0;

   if (!Client) return;
   if ((Socket->Flags & NSF::SERVER) IS NSF::NIL) return; // Must be a server

   if (recursive) return;
   recursive++;

   auto label = client_ip_label(Client->IP);
   log.branch("%s, Connections: %d", label.c_str(), Client->TotalConnections);

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
      if (Socket->Clients) Socket->Clients->Prev = nullptr;
   }

   FreeResource(Client);

   Socket->TotalClients--;

   recursive--;
}

//********************************************************************************************************************
static void netsocket_connect_impl(HOSTHANDLE SocketFD, extNetSocket *Self)
{
   kt::Log log(__FUNCTION__);

   kt::SwitchContext context(Self);

   if (Self->Handle.hosthandle() != SocketFD) {
      log.warning(ERR::SanityCheckFailed);
      return;
   }

   log.trace("Connection from server received.");

   auto result = network_platform().complete_connect(Self->Handle);
   complete_socket_connect(Self, result);
}

//********************************************************************************************************************
// If the socket is the client of a server, messages from the server will come in through here.
//
// Incoming information from the server can be read with either the Incoming callback routine (the developer is
// expected to call the Read action from this).
//
// This function is managed outside of the normal message queue.

static void netsocket_incoming_impl(HOSTHANDLE SocketFD, extNetSocket *Self)
{
   kt::Log log(__FUNCTION__);

   kt::SwitchContext context(Self); // Set context & lock

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
   if ((Self->TLS.Handle) and (Self->State IS NTC::HANDSHAKING)) {
      kt::Log log(__FUNCTION__);
      log.traceBranch("Windows SSL handshake in progress, reading raw data.");
      size_t result;
      std::vector<uint8_t> buffer;
      if (ERR error = network_platform().append_receive(Self->Handle, buffer, 32768, result); error IS ERR::Okay) {
         tls_handshake_received(Self, buffer.data(), int(buffer.size()));

         if ((Self->State != NTC::CONNECTED) or (!ssl_has_decrypted_data(Self->TLS.Handle) and !ssl_has_encrypted_data(Self->TLS.Handle))) {
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
   else if ((Self->TLS.Handle) and (Self->State IS NTC::CONNECTED) and (!ssl_has_decrypted_data(Self->TLS.Handle))) {
      if (auto error = tls_receive_encrypted(Self); error IS ERR::Disconnected) {
         free_socket(Self);
         return;
      }
      else if (error != ERR::Okay) {
         log.warning(error);
         return;
      }

      if (!ssl_has_decrypted_data(Self->TLS.Handle)) return;
   }

  #else
    if ((Self->TLS.Handle) and (Self->State IS NTC::HANDSHAKING)) {
      log.traceBranch("Continuing SSL handshake...");
      tls_connect(Self);
      return;
    }

    if (Self->TLS.HandshakeStatus != SHS::NIL) { // TODO: Check State is not HANDSHAKING instead
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
         kt::SwitchContext context(Self->Incoming.Context);
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
      if (Self->Handle.is_valid()) {
         Self->CloseAfterWrite = true;
         Self->Incoming.clear();
         network_platform().register_write(Self->Handle, &netsocket_outgoing, Self);
      }
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
   else if (Self->TLS.Handle and (ssl_has_decrypted_data(Self->TLS.Handle) or ssl_has_encrypted_data(Self->TLS.Handle))) {
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
   kt::Log log(__FUNCTION__);

   kt::SwitchContext context(Self); // Set context & lock

   if (Self->Terminating) return;

   if (Self->State IS NTC::HANDSHAKING) {
      #ifndef DISABLE_SSL
         #ifdef _WIN32
            if (Self->TLS.Handle) {
               if (auto error = tls_flush_output(Self); error != ERR::Okay) log.traceWarning(error);
            }
         #endif
      #endif
      log.trace("Handshaking...");
      return;
   }

#ifndef DISABLE_SSL
#ifndef _WIN32
   if ((Self->TLS.Handle) and (Self->TLS.HandshakeStatus IS SHS::READ)) {
      ssl_suspend_write_queue(Self->Handle.hosthandle());
      return;
   }

   if ((Self->TLS.Handle) and (Self->TLS.HandshakeStatus IS SHS::WRITE)) {
      ssl_resume_write_handshake(Self->Handle.hosthandle(), Self);
      return;
   }
#endif
#endif

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
         if ((!Self->TLS.Handle) and (len > glMaxWriteLen)) len = glMaxWriteLen;
      #else
         if (len > glMaxWriteLen) len = glMaxWriteLen;
      #endif

      if (len > 0) {
         error = send_data(Self, Self->WriteQueue.Buffer.data() + Self->WriteQueue.Index, &len);
         if (len > 0) {
            log.trace("Sent %d of %d bytes from the queue.", Self->UID, int(len),
               int(Self->WriteQueue.Buffer.size() - Self->WriteQueue.Index));
            Self->WriteQueue.Index += len;
         }
         if ((error != ERR::Okay) or (!len)) break;
      }

      if (Self->WriteQueue.Index >= Self->WriteQueue.Buffer.size()) {
         Self->WriteQueue.Buffer.clear();
         Self->WriteQueue.Index = 0;
         break;
      }
   }

   // Before feeding new data into the queue, the current buffer must be empty.

   if ((error IS ERR::Okay) and Self->CloseAfterWrite and Self->WriteQueue.Buffer.empty() and
       (not network_platform().has_pending_write(Self->Handle))) {
      Self->InUse--;
      Self->OutgoingRecursion--;
      free_socket(Self);
      return;
   }

   if ((error IS ERR::Okay) and ((Self->WriteQueue.Buffer.empty()) or
       (Self->WriteQueue.Index >= Self->WriteQueue.Buffer.size()))) {
      if (Self->Outgoing.defined()) {
         if (Self->Outgoing.isC()) {
            auto routine = (ERR (*)(extNetSocket *, APTR))Self->Outgoing.Routine;
            kt::SwitchContext context(Self->Outgoing.Context);
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

      if ((!Self->Outgoing.defined()) and (Self->WriteQueue.Buffer.empty()) and
          (not network_platform().has_pending_write(Self->Handle))) {
         log.trace("Write-queue listening on socket %d will now stop.", Self->UID, Self->Handle.int_value());
         if (auto error = network_platform().remove_write(Self->Handle); error != ERR::Okay) log.warning(error);
      }

      if (error != ERR::Okay) {
         Self->ErrorCountdown--;
         if (!Self->ErrorCountdown) Self->setState(NTC::DISCONNECTED);
      }
   }

   Self->InUse--;
   Self->OutgoingRecursion--;
}
