/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
ClientSocket: Represents a single socket connection to a client IP address.

If a @Netsocket is running in server mode then it will create a new ClientSocket object every time that a new connection
is opened by a client.  This is a very simple class that assists in the management of I/O between the client and server.
-END-

*********************************************************************************************************************/

// Forward declaration of template function from network.cpp

template<typename T>
static ERR send_data(T *Self, CPTR Buffer, size_t *Length);

static CSTRING clientsocket_state(NTC Value);

//********************************************************************************************************************
// Disconnect a client socket and report the state change.

static void disconnect(extClientSocket *Self)
{
   pf::Log log(__FUNCTION__);

   log.branch("Disconnecting socket handle %d", Self->Handle.int_value());

   if (Self->Handle.is_valid()) {

#ifdef __linux__
      DeregisterFD(Self->Handle);
#endif
      CLOSESOCKET_THREADED(Self->Handle);
      Self->Handle = NOHANDLE;
   }

   if (Self->State != NTC::DISCONNECTED) Self->setState(NTC::DISCONNECTED);
}

//********************************************************************************************************************
// Read function specifically for ClientSocket connections

static ERR receive_from_client(extClientSocket *Self, APTR Buffer, size_t BufferSize, size_t *Result)
{
   pf::Log log(__FUNCTION__);

   if (!BufferSize) return ERR::Okay;

#ifndef DISABLE_SSL
   if (Self->SSLHandle) {
   #ifdef _WIN32
       // If we're in the middle of SSL handshake, read raw data for handshake processing
       if (Self->State IS NTC::HANDSHAKING) {
          log.trace("Windows SSL handshake in progress, reading raw data.");
          ERR error = WIN_RECEIVE(Self->Handle, Buffer, BufferSize, Result);
          if ((error IS ERR::Okay) and (*Result > 0)) {
             sslHandshakeReceived(Self, Buffer, *Result);
          }
          return error;
      }
      else { // Normal SSL data read for established connections
         int bytes_read = 0;
         auto ssl_error = ssl_read(Self->SSLHandle, Buffer, BufferSize, &bytes_read);
         if ((ssl_error IS SSL_OK) and (bytes_read > 0)) {
            *Result = bytes_read;
            return ERR::Okay;
         }
         else if ((ssl_error IS SSL_OK) and (!bytes_read)) return ERR::Disconnected;
         else if (ssl_error IS SSL_ERROR_WOULD_BLOCK) {
            log.traceWarning("No more data to read from the SSL socket.");
            return ERR::Okay;
         }
         else log.warning(ERR::Failed);
      }
   #else // OpenSSL
      bool read_blocked;
      int pending;

      if (Self->HandshakeStatus IS SHS::WRITE) ssl_handshake_write(Self->Handle, Self);
      else if (Self->HandshakeStatus IS SHS::READ) ssl_handshake_read(Self->Handle, Self);

      if (Self->HandshakeStatus != SHS::NIL) return ERR::Okay;

      log.traceBranch("BufferSize: %d", int(BufferSize));

      do {
         read_blocked = false;

         auto result = SSL_read(Self->SSLHandle, Buffer, BufferSize);

         if (result <= 0) {
            auto ssl_error = SSL_get_error(Self->SSLHandle, result);
            switch (ssl_error) {
               case SSL_ERROR_ZERO_RETURN:
                  return log.traceWarning(ERR::Disconnected);

               case SSL_ERROR_WANT_READ:
                  read_blocked = true;
                  return ERR::Okay; // No data available yet

               case SSL_ERROR_WANT_WRITE:
                  // WANT_WRITE is returned if we're trying to rehandshake and the write operation would block.  We
                  // need to wait on the socket to be writeable, then restart the read when it is.

                  log.msg("SSL socket handshake requested by server.");
                  Self->HandshakeStatus = SHS::WRITE;
                  RegisterFD(Self->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, ssl_handshake_write_clientsocket, Self);
                  return ERR::Okay;

               case SSL_ERROR_SYSCALL:
               default:
                  log.warning("SSL read failed with error %d: %s", ssl_error, ERR_error_string(ssl_error, nullptr));
                  return ERR::Read;
            }
         }
         else {
            *Result += result;
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

         RegisterFD(Self->Handle.hosthandle(), RFD::RECALL|RFD::READ|RFD::SOCKET, &server_incoming_from_client, Self);
      }

      return ERR::Okay;
   #endif
   }
#endif // DISABLE_SSL

#ifdef __linux__
   {
      int result = recv(Self->Handle, Buffer, BufferSize, 0);

      if (result > 0) {
         *Result = result;
         return ERR::Okay;
      }
      else if (result IS 0) { // man recv() says: The return value is 0 when the peer has performed an orderly shutdown.
         return ERR::Disconnected;
      }
      else if ((errno IS EAGAIN) or (errno IS EINTR)) {
         return ERR::Okay;
      }
      else {
         log.warning("recv() failed: %s", strerror(errno));
         return ERR::SystemCall;
      }
   }
#elif _WIN32
   return WIN_RECEIVE(Self->Handle, Buffer, BufferSize, Result);
#else
   #error No support for RECEIVE()
#endif
}

//********************************************************************************************************************
// Data has arrived from a client's socket handle.

static void server_incoming_from_client_impl(HOSTHANDLE SocketFD, extClientSocket *client)
{
   pf::Log log(__FUNCTION__);
   if (!client->Client) return;
   auto Server = (extNetSocket *)(client->Client->Owner);

   if (client->Handle.is_invalid()) {
      log.warning(ERR::InvalidState); // Socket closed but receiving data.
      return;
   }

   pf::ScopedObjectLock lock(client); // Acquire a lock in case a callback tries to free the object.

#ifndef DISABLE_SSL
   #ifdef _WIN32
      if (client->State IS NTC::HANDSHAKING) {
         log.trace("Windows SSL server handshake in progress, reading raw data.");
         std::array<char, 4096> buffer;
         size_t bytes_received;
         ERR error = WIN_RECEIVE(client->Handle, buffer.data(), buffer.size(), &bytes_received);
         if ((error IS ERR::Okay) and (bytes_received > 0)) {
            SSL_ERROR_CODE accept_result = ssl_accept(client->SSLHandle, buffer.data(), bytes_received);

            switch (accept_result) {
               case SSL_OK:
                  log.trace("SSL handshake completed for client %d", client->UID);
                  client->setState(NTC::CONNECTED);
                  return;
               case SSL_ERROR_WOULD_BLOCK:
               case SSL_NEED_DATA: // Server needs to send response data back to client
                  return;

               default:
                  log.warning("Server SSL handshake failed: %d; SecStatus: 0x%08X; WinError: %d", accept_result,
                     ssl_last_security_status(client->SSLHandle),
                     ssl_last_win32_error(client->SSLHandle));
                  client->setState(NTC::DISCONNECTED);
                  return;
            }
         }
         return;
      }
   #else
      if (client->State IS NTC::HANDSHAKING) {
         // Continue SSL handshake for this ClientSocket
         auto result = SSL_accept(client->SSLHandle);
         if (result == 1) {
            log.msg("SSL handshake completed for client %d", client->UID);
            client->setState(NTC::CONNECTED);
         }
         else {
            auto ssl_error = SSL_get_error(client->SSLHandle, result);
            if ((ssl_error == SSL_ERROR_WANT_READ) or (ssl_error == SSL_ERROR_WANT_WRITE)) {
               log.trace("SSL handshake continuing for client %d...", client->UID);
               // Handshake will continue on next data arrival
            }
            else {
               log.warning("SSL handshake failed for client %d: %s", client->UID, ERR_error_string(ssl_error, nullptr));
               client->setState(NTC::DISCONNECTED);
            }
         }
         return;
      }
   #endif
#endif

   if (client->State != NTC::CONNECTED) { // Sanity check
      log.warning(ERR::InvalidState);
      return;
   }

   Server->InUse++;
   client->ReadCalled = false;

   log.traceBranch("Handle: %" PRId64 ", Socket: %d, Client: %d", int64_t(SocketFD), Server->UID, client->UID);

   auto error = ERR::Okay;
   if (Server->Incoming.defined()) {
      if (Server->Incoming.isC()) {
         pf::SwitchContext context(Server->Incoming.Context);
         auto routine = (ERR (*)(extNetSocket *, extClientSocket *, APTR))Server->Incoming.Routine;
         error = routine(Server, client, Server->Incoming.Meta);
      }
      else if (Server->Incoming.isScript()) {
         if (sc::Call(Server->Incoming, std::to_array<ScriptArg>({
               { "NetSocket",    Server, FD_OBJECTPTR },
               { "ClientSocket", client, FD_OBJECTPTR }
            }), error) != ERR::Okay) error = ERR::Terminate;
         if (error IS ERR::Exception) error = ERR::Terminate; // assert() and error() are taken seriously
      }
      else error = ERR::InvalidValue;
   }
   else log.traceWarning("No Incoming callback configured.");

   if (!client->ReadCalled) error = ERR::Terminate;

   if (error IS ERR::Terminate) {
      log.trace("Terminating socket, failed to read incoming data.");
      FreeResource(client); // Disconnect & send Feedback message
   }

   Server->InUse--;
}

//********************************************************************************************************************
// Note that this function will prevent the task from going to sleep if it is not managed correctly.  If
// no data is being written to the queue, the program will not be able to sleep until the client stops listening
// to the write queue.

static void clientsocket_outgoing_impl(HOSTHANDLE SocketFD, extClientSocket *ClientSocket)
{
   pf::Log log(__FUNCTION__);
   auto Server = (extNetSocket *)(ClientSocket->Client->Owner);

   if (Server->Terminating) return;

#ifndef DISABLE_SSL
   if ((ClientSocket->SSLHandle) and (ClientSocket->State IS NTC::HANDSHAKING)) {
      log.trace("Still connecting via SSL...");
      return;
   }
#endif

   if (ClientSocket->OutgoingRecursion) {
      log.trace("Recursion detected.");
      return;
   }

   log.traceBranch();

#ifndef DISABLE_SSL
  #ifndef _WIN32
    if (ClientSocket->HandshakeStatus != SHS::NIL) return;
  #endif
#endif

   ClientSocket->InUse++;
   ClientSocket->OutgoingRecursion++;

   auto error = ERR::Okay;

   // Send out remaining queued data before getting new data to send

   while (!ClientSocket->WriteQueue.Buffer.empty()) {
      size_t len = ClientSocket->WriteQueue.Buffer.size() - ClientSocket->WriteQueue.Index;
      #ifndef DISABLE_SSL
         if ((!ClientSocket->SSLHandle) and (len > glMaxWriteLen)) len = glMaxWriteLen;
      #else
         if (len > glMaxWriteLen) len = glMaxWriteLen;
      #endif

      if (len > 0) {
         error = send_data(ClientSocket, ClientSocket->WriteQueue.Buffer.data() + ClientSocket->WriteQueue.Index, &len);
         if ((error != ERR::Okay) or (!len)) break;
         ClientSocket->WriteQueue.Index += len;
      }

      if (ClientSocket->WriteQueue.Index >= ClientSocket->WriteQueue.Buffer.size()) {
         ClientSocket->WriteQueue.Buffer.clear();
         ClientSocket->WriteQueue.Index = 0;
         break;
      }
   }

   // Before feeding new data into the queue, the current buffer must be empty.

   if ((error IS ERR::Okay) and ((ClientSocket->WriteQueue.Buffer.empty()) or
       (ClientSocket->WriteQueue.Index >= ClientSocket->WriteQueue.Buffer.size()))) {
      // Fetch more data

      if (Server->Outgoing.defined()) {
         if (Server->Outgoing.isC()) {
            auto routine = (ERR (*)(extNetSocket *, extClientSocket *, APTR))(Server->Outgoing.Routine);
            pf::SwitchContext context(Server->Outgoing.Context);
            error = routine(Server, ClientSocket, Server->Outgoing.Meta);
         }
         else if (Server->Outgoing.isScript()) {
            if (sc::Call(Server->Outgoing, std::to_array<ScriptArg>({
                  { "NetSocket", Server, FD_OBJECTPTR },
                  { "ClientSocket", ClientSocket, FD_OBJECTPTR }
               }), error) != ERR::Okay) error = ERR::Terminate;
         }

         if (error != ERR::Okay) Server->Outgoing.clear();
      }

      // If the write queue is empty then we remove the FD-Write registration so that
      // we don't tax system resources.

      if (ClientSocket->WriteQueue.Buffer.empty()) {
         log.trace("[NetSocket:%d] Write-queue listening on FD %d will now stop.", Server->UID, ClientSocket->Handle.int_value());
         #ifdef __linux__
            RegisterFD(ClientSocket->Handle.hosthandle(), RFD::REMOVE|RFD::WRITE|RFD::SOCKET, nullptr, nullptr);
         #elif _WIN32
            win_socketstate(ClientSocket->Handle, std::nullopt, false);
         #endif
      }
   }

   if (error != ERR::Okay) {
      ClientSocket->ErrorCountdown--;
      if (!ClientSocket->ErrorCountdown) disconnect(ClientSocket);
   }

   ClientSocket->InUse--;
   ClientSocket->OutgoingRecursion--;
}

/*********************************************************************************************************************

-ACTION-
Deactivate: Disconnects the socket and changes the #State to `DISCONNECTED`.

*********************************************************************************************************************/

static ERR CLIENTSOCKET_Deactivate(extClientSocket *Self)
{
   pf::Log log;
   log.branch();
   disconnect(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR CLIENTSOCKET_Free(extClientSocket *Self)
{
   pf::Log log;

#ifndef DISABLE_SSL
   sslDisconnect(Self);
#endif

   disconnect(Self);

   if (Self->Client) { // If undefined, ClientSocket was never initialised
      pf::ScopedObjectLock lock(Self->Client);
      if (lock.granted()) {
         if (Self->Prev) {
            Self->Prev->Next = Self->Next;
            if (Self->Next) Self->Next->Prev = Self->Prev;
         }
         else {
            Self->Client->Connections = Self->Next;
            if (Self->Next) Self->Next->Prev = nullptr;
         }

         Self->Client->TotalConnections--;

         if (!Self->Client->Connections) {
            log.msg("No more connections for this IP, removing client.");
            free_client((extNetSocket *)Self->Client->Owner, Self->Client);
         }
      }
   }

   Self->~extClientSocket();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR CLIENTSOCKET_Init(extClientSocket *Self)
{
   pf::Log log;

   if (!Self->Client) return log.warning(ERR::FieldNotSet);

   pf::ScopedObjectLock lock(Self->Client);
   if (!lock.granted()) return ERR::Lock;

#ifdef __linux__
   int non_blocking = 1;
   ioctl(Self->Handle, FIONBIO, &non_blocking);
#endif

   Self->ConnectTime = PreciseTime() / 1000LL;

   if (Self->Client->Connections) {
      Self->Next = Self->Client->Connections;
      Self->Prev = nullptr;
      Self->Client->Connections->Prev = Self;
   }

   Self->Client->Connections = Self;
   Self->Client->TotalConnections++;
   Self->State = NTC::CONNECTING;

#ifndef DISABLE_SSL
   #ifdef _WIN32
      auto server = (extNetSocket *)(Self->Client->Owner);
      if ((server->Flags & NSF::SSL) != NSF::NIL) {
         // Server-side SSL setup - create SSL context and wait for client handshake
         Self->SSLHandle = ssl_create_context(false, true); // No verification, server mode
         if (Self->SSLHandle) {
            ssl_set_server_certificate(server->SSLHandle, Self->SSLHandle);
            ssl_set_socket(Self->SSLHandle, (void*)(size_t)Self->Handle.socket()); // Set socket handle for server-side SSL
            Self->State = NTC::HANDSHAKING;
         }
         else Self->State = NTC::DISCONNECTED;
      }
      else Self->State = NTC::CONNECTED; // Not an SSL socket
   #else
      auto server = (extNetSocket *)(Self->Client->Owner);
      if ((server->Flags & NSF::SSL) != NSF::NIL) {
         if (auto client_ssl = SSL_new(glServerSSL)) { // Use glServerSSL because we represent the server side.
            if (auto client_bio = BIO_new_socket(Self->Handle, BIO_NOCLOSE)) {
               SSL_set_bio(client_ssl, client_bio, client_bio);

               Self->SSLHandle = client_ssl;
               Self->BIOHandle = client_bio;

               if (auto result = SSL_accept(client_ssl); result == 1) {
                  log.trace("SSL handshake successful.");
                  Self->setState(NTC::CONNECTED);
               }
               else {
                  Self->setState(NTC::HANDSHAKING);

                  auto ssl_error = SSL_get_error(client_ssl, result);
                  if ((ssl_error == SSL_ERROR_WANT_READ) or (ssl_error == SSL_ERROR_WANT_WRITE)) {
                     log.msg("SSL handshake in progress...");
                     // Handshake will continue asynchronously
                  }
                  else {
                     log.warning("SSL handshake failed: %s", ERR_error_string(ssl_error, nullptr));
                     Self->SSLHandle = nullptr;
                     Self->BIOHandle = nullptr;
                     SSL_free(client_ssl);
                     return ERR::SystemCall;
                  }
               }
            }
            else {
               SSL_free(client_ssl);
               return log.warning(ERR::SystemCall);
            }
         }
         else return log.warning(ERR::SystemCall);
      }
      else Self->State = NTC::CONNECTED; // Not an SSL socket
   #endif
#else
   Self->State = NTC::CONNECTED;
#endif

#ifdef __linux__
   RegisterFD(Self->Handle.hosthandle(), RFD::READ|RFD::SOCKET, &server_incoming_from_client, Self);
#elif _WIN32
   win_socket_reference(Self->Handle, Self);
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR CLIENTSOCKET_NewPlacement(extClientSocket *Self)
{
   new (Self) extClientSocket;
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Read: Read incoming data from a client socket.

The Read() action will read incoming data from the socket and write it to the provided buffer.  If the socket connection
is safe, success will always be returned by this action regardless of whether or not data was available.  Almost all
other return codes indicate permanent failure, and the socket connection will be closed when the action returns.

-ERRORS-
Okay: Read successful (if no data was on the socket, success is still indicated).
NullArgs
Disconnected: The socket connection is closed.
Failed: A permanent failure has occurred and socket has been closed.

*********************************************************************************************************************/

static ERR CLIENTSOCKET_Read(extClientSocket *Self, struct acRead *Args)
{
   pf::Log log;
   if ((!Args) or (!Args->Buffer)) return log.error(ERR::NullArgs);
   if (Self->Handle.is_invalid()) {
      // Lack of a handle means that disconnection has already been processed, so the client code
      // shouldn't be calling us (client probably needs to be plugged into the feedback mechanisms)
      return log.warning(ERR::Disconnected);
   }
   Self->ReadCalled = true;
   if (!Args->Length) { Args->Result = 0; return ERR::Okay; }

   size_t result = 0;
   auto error = receive_from_client(Self, Args->Buffer, Args->Length, &result);
   Args->Result = result;

   if (error IS ERR::Disconnected) {
      // Detecting a disconnection on read is normal, now handle disconnection gracefully.
      log.branch("Client disconnection detected.");
      disconnect(Self);
   }
   return error;
}

/*********************************************************************************************************************

-ACTION-
Write: Writes data to the socket.

Write raw data to a client socket with this action.  Write connections are buffered, so any data overflow generated
in a call to this action will be buffered into a software queue.  Resource limits placed on the software queue are
governed by the @NetSocket.MsgLimit value.

*********************************************************************************************************************/

static ERR CLIENTSOCKET_Write(extClientSocket *Self, struct acWrite *Args)
{
   pf::Log log;

   // Note that this code is essentially a copy of the NetSocket write code.

   if (!Args) return ERR::NullArgs;
   Args->Result = 0;

   auto server = (extNetSocket *)(Self->Client->Owner);

   if ((Self->Handle.is_invalid()) or (Self->State != NTC::CONNECTED)) { // Queue the write prior to server connection
      log.trace("Saving %d bytes to queue.", Args->Length);
      Self->WriteQueue.write(Args->Buffer, std::min<size_t>(Args->Length, server->MsgLimit));
      return ERR::Okay;
   }

   size_t len;
   ERR error;
   if (Self->WriteQueue.Buffer.empty()) { // No prior buffer to send
      len = Args->Length;
      error = send_data(Self, Args->Buffer, &len);
   }
   else {
      len = 0;
      error = ERR::BufferOverflow;
   }

   if ((error != ERR::Okay) or (len < size_t(Args->Length))) {
      if ((error IS ERR::DataSize) or (error IS ERR::BufferOverflow) or (len > 0))  {
         // Put data into the write queue and register the socket for write events
         log.trace("Error: '%s', queuing %d/%d bytes for transfer...", GetErrorMsg(error), Args->Length - len, Args->Length);
         Self->WriteQueue.write((int8_t *)Args->Buffer + len, std::min<size_t>(Args->Length - len, server->MsgLimit));
         #ifdef __linux__
            RegisterFD(Self->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, &clientsocket_outgoing, Self);
         #elif _WIN32
            win_socketstate(Self->Handle, std::nullopt, true);
         #endif
      }
      else return error;
   }
   else log.trace("Wrote all %d bytes to the server.", Args->Length);

   Args->Result = Args->Length;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Client: Parent client object (IP address).

-FIELD-
ClientData: Available for client data storage.

-FIELD-
ConnectTime: System time for the creation of this socket.

-FIELD-
Next: Next socket in the chain.

-FIELD-
Prev: Previous socket in the chain.

-FIELD-
State: The current connection state of the ClientSocket object.

The State reflects the connection state of the NetSocket.  If the #Feedback field is defined with a function, it will
be called automatically whenever the state is changed.  Note that the ClientSocket parameter will be NULL when the
Feedback function is called.

Note that in server mode this State value should not be used as it cannot reflect the state of all connected
client sockets.  Each @ClientSocket carries its own independent State value for use instead.

*********************************************************************************************************************/

static ERR CS_SET_State(extClientSocket *Self, NTC Value)
{
   pf::Log log;

   if (Value != Self->State) {
      auto server = (extNetSocket *)(Self->Client->Owner);

      log.branch("State changed from %s to %s", clientsocket_state(Self->State), clientsocket_state(Value));

      Self->State = Value;

      if (server->Feedback.defined()) {
         if (server->Feedback.isC()) {
            pf::SwitchContext context(server->Feedback.Context);
            auto routine = (void (*)(extNetSocket *, objClientSocket *, NTC, APTR))server->Feedback.Routine;
            if (routine) routine(server, Self, Self->State, server->Feedback.Meta);
         }
         else if (server->Feedback.isScript()) {
            sc::Call(server->Feedback, std::to_array<ScriptArg>({
               { "NetSocket",    server, FD_OBJECTPTR },
               { "ClientSocket", Self, FD_OBJECTPTR },
               { "State",        int(Self->State) }
            }));
         }
      }

      if ((Self->State IS NTC::CONNECTED) and ((!Self->WriteQueue.Buffer.empty()))) {
         log.msg("Sending queued data to server on connection.");
         #ifdef __linux__
            RegisterFD(Self->Handle.hosthandle(), RFD::WRITE|RFD::SOCKET, &clientsocket_outgoing, Self);
         #elif _WIN32
            win_socketstate(Self->Handle, std::nullopt, true);
         #endif
      }
   }

   SetResourcePtr(RES::EXCEPTION_HANDLER, nullptr); // Stop winsock from fooling with our exception handler

   return ERR::Okay;
}

//********************************************************************************************************************

#include "clientsocket_def.c"

static const FieldArray clClientSocketFields[] = {
   { "ConnectTime", FDF_INT64|FDF_R },
   { "Prev",        FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::CLIENTSOCKET },
   { "Next",        FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::CLIENTSOCKET },
   { "Client",      FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::NETCLIENT },
   { "ClientData",  FDF_POINTER|FDF_R },
   { "State",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, CS_SET_State, &clNetSocketState },
   END_FIELD
};

//********************************************************************************************************************

static CSTRING clientsocket_state(NTC Value) {
   return clClientSocketState[int(Value)].Name;
}

static ERR init_clientsocket(void)
{
   clClientSocket = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::CLIENTSOCKET),
      fl::ClassVersion(1.0),
      fl::Name("ClientSocket"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clClientSocketActions),
      fl::Fields(clClientSocketFields),
      fl::Size(sizeof(extClientSocket)),
      fl::Path(MOD_PATH));

   return clClientSocket ? ERR::Okay : ERR::AddClass;
}
