
//********************************************************************************************************************

static void socket_feedback(objNetSocket *Socket, NTC State, APTR Meta)
{
   kt::Log log("http_feedback");

   log.msg("Socket: %p, State: %d, Context: %d", Socket, int(State), CurrentContext()->UID);

   auto Self = (extHTTP *)CurrentContext();
   if (Self->classID() != CLASSID::HTTP) { log.warning(ERR::SystemCorrupt); return; }
   if (!Self->locked()) { log.warning(ERR::ResourceNotLocked); return; }

   if (State IS NTC::CONNECTING) {
      log.msg("Waiting for connection...");

      if (Self->TimeoutManager) UpdateTimer(Self->TimeoutManager, Self->ConnectTimeout);
      else SubscribeTimer(Self->ConnectTimeout, C_FUNCTION(http_timeout), &Self->TimeoutManager);

      Self->Connecting = true;
   }
   else if (State IS NTC::CONNECTED) {
      // The GET request has been pre-written to the socket on its creation, so we don't need to do anything further
      // here.

      log.msg("Connection confirmed.");
      if (Self->TimeoutManager) { UpdateTimer(Self->TimeoutManager, 0); Self->TimeoutManager = 0; }
      Self->Connecting = false;
   }
   else if (State IS NTC::DISCONNECTED) {
      // Socket disconnected.  The HTTP state must change to either COMPLETED (completed naturally) or TERMINATED
      // (abnormal termination) to correctly inform the user as to what has happened.

      log.msg("Disconnected from socket while in state %s.", clHTTPCurrentState[int(Self->CurrentState)].Name);

      if (Self->TimeoutManager) { UpdateTimer(Self->TimeoutManager, 0); Self->TimeoutManager = 0; }

      if (Self->Connecting) {
         Self->Connecting = false;

         Self->Error = Socket->Error;
         log.branch("Deactivating (connect failure message received).");
         Self->setCurrentState(HGS::TERMINATED);
         return;
      }
      else Self->Connecting = false;

      if (Self->CurrentState >= HGS::COMPLETED) {
         return;
      }
      else if (Self->CurrentState IS HGS::READING_HEADER) {
         auto incoming_error = socket_incoming(Socket);
         if ((incoming_error IS ERR::Okay) or (incoming_error IS ERR::Terminate)) {
            if (Self->CurrentState >= HGS::COMPLETED) return;
         }

         Self->Error = Socket->Error > ERR::ExceptionThreshold ? Socket->Error : ERR::Disconnected;
         log.trace("Received broken header as follows:\n%s", Self->Response.c_str());
         Self->setCurrentState(HGS::TERMINATED);
      }
      else if (Self->CurrentState IS HGS::SEND_COMPLETE) {
         // Disconnection on completion of sending data should be no big deal
         Self->Error = Socket->Error > ERR::ExceptionThreshold ? Socket->Error : ERR::Okay;
         Self->setCurrentState(HGS::COMPLETED);
      }
      else if (Self->CurrentState IS HGS::SENDING_CONTENT) {
         Self->Error = Socket->Error > ERR::ExceptionThreshold ? Socket->Error : ERR::Disconnected;

         // If the socket is not active, then the disconnection is a result of destroying the object (e.g. due to a redirect).

         log.branch("State changing to TERMINATED due to disconnection.");
         Self->setCurrentState(HGS::TERMINATED);
      }
      else if (Self->CurrentState IS HGS::READING_CONTENT) {
         // Unread data can remain on the socket following disconnection, so try to read anything that's been left.

         if (Self->Chunked) {
            log.traceWarning("Support code required to read chunked data following a disconnected socket.");
         }
         else if ((Self->ContentLength IS -1) or (Self->Index < Self->ContentLength)) {
            std::vector<char> buffer(BUFFER_READ_SIZE);
            ERR drain_error = ERR::Okay;

            while (true) {
               auto len = std::ssize(buffer);
               if (Self->ContentLength != -1) {
                  if (len > Self->ContentLength - Self->Index) len = Self->ContentLength - Self->Index;
               }

               int bytes_read;
               if ((Self->Error = acRead(Socket, buffer.data(), len, &bytes_read)) != ERR::Okay) {
                  log.warning("Read() returned error: %s", GetErrorMsg(Self->Error));
               }

               if (!bytes_read) { // No more incoming data
                  log.detail("Received %d bytes of content in this content reading session.", bytes_read);
                  break;
               }

               if (auto output_error = output_incoming_data(Self, buffer.data(), bytes_read);
                     output_error != ERR::Okay) {
                  drain_error = output_error;
                  break;
               }
               if (check_incoming_end(Self) IS ERR::True) break;
            }

            if (drain_error != ERR::Okay) {
               Self->Error = drain_error;
               Self->setCurrentState(HGS::TERMINATED);
               return;
            }
         }

         if (Self->ContentLength IS -1) {
            if (Socket->Error <= ERR::ExceptionThreshold) {
               log.msg("Orderly shutdown while streaming data.");
               Self->setCurrentState(HGS::COMPLETED);
            }
            else {
               Self->Error = Socket->Error;
               Self->setCurrentState(HGS::TERMINATED);
            }
         }
         else if (Self->Index < Self->ContentLength) {
            log.warning("Disconnected before all content was downloaded (%" PRId64 " of %" PRId64 ")", Self->Index, Self->ContentLength);
            Self->Error = Socket->Error > ERR::ExceptionThreshold ? Socket->Error : ERR::Disconnected;
            Self->setCurrentState(HGS::TERMINATED);
         }
         else {
            log.trace("Orderly shutdown, received %" PRId64 " of the expected %" PRId64 " bytes.", Self->Index, Self->ContentLength);
            Self->setCurrentState(HGS::COMPLETED);
         }
      }
      else if (Self->CurrentState IS HGS::AUTHENTICATING) {
         if (Self->DialogWindow) {
            // The HTTP socket was closed because the user is taking too long to authenticate with the dialog
            // window.  We will close the socket and create a new one once the user responds to the dialog.

            Self->Socket->setFeedback(FUNCTION{});
            FreeResource(Socket);
            Self->Socket = nullptr;
            Self->SecurePath = true;
            return;
         }

         Self->setCurrentState(HGS::TERMINATED);
      }
   }
   else if (Self->CurrentState >= HGS::COMPLETED) {
      // If the state is set to HGS::COMPLETED or HGS::TERMINATED, our code should have returned ERR::Terminate to switch
      // off the socket.  This section is entered if we forgot to do that.

      log.warning("Warning - socket channel was not closed correctly (didn't return ERR::Terminate).");
   }
}

//********************************************************************************************************************
// Callback: NetSocket.Outgoing

static ERR socket_outgoing(objNetSocket *Socket)
{
   kt::Log log("http_outgoing");

   constexpr int CHUNK_LENGTH_OFFSET = 10; // Enough space for an 8-digit hex length + CRLF

   auto Self = (extHTTP *)CurrentContext();
   if (Self->classID() != CLASSID::HTTP) return log.warning(ERR::SystemCorrupt);

   log.traceBranch("Socket: %p, Object: %d, State: %d", Socket, CurrentContext()->UID, int(Self->CurrentState));

   ERR error = ERR::Okay;

   if (Self->Chunked) Self->WriteBuffer.resize(CHUNK_LENGTH_OFFSET);
   else Self->WriteBuffer.resize(0);

   if (Self->CurrentState != HGS::SENDING_CONTENT) {
      Self->setCurrentState(HGS::SENDING_CONTENT);
   }

   int client_bytes_written = 0;

   if (Self->Outgoing.defined()) {
      if (Self->Outgoing.isC()) {
         auto routine = (ERR (*)(extHTTP *, std::vector<uint8_t> &, APTR))Self->Outgoing.Routine;
         // NB: Client is expected to append data to the WriteBuffer, not replace it.
         error = routine(Self, Self->WriteBuffer, Self->Outgoing.Meta);
      }
      else if (Self->Outgoing.isScript()) {
         // For a script to write to the buffer, it needs to make a call to the Write() action and this
         // will append to WriteBuffer.
         if (sc::Call(Self->Outgoing, std::to_array<ScriptArg>({
               { "HTTP", Self, FD_OBJECTPTR },
            }), error) != ERR::Okay) error = ERR::Failed;
         if (error > ERR::ExceptionThreshold) {
            log.warning("Procedure %" PRId64 " failed, aborting HTTP call.", Self->Outgoing.ProcedureID);
         }
      }
      else error = ERR::InvalidValue;

      if (error > ERR::ExceptionThreshold) log.warning("Outgoing callback error: %s", GetErrorMsg(error));

      client_bytes_written = std::ssize(Self->WriteBuffer) - (Self->Chunked ? CHUNK_LENGTH_OFFSET : 0);
   }
   else if (Self->flInput) {
      log.detail("Sending content from an Input file.");

      int offset = (Self->Chunked ? CHUNK_LENGTH_OFFSET : 0);
      Self->WriteBuffer.resize(Self->BufferSize + offset);
      error = acRead(Self->flInput, Self->WriteBuffer.data() + offset, std::ssize(Self->WriteBuffer) - offset, &client_bytes_written);

      if (((!error) or (error IS ERR::Terminate)) and (client_bytes_written >= 0)) {
         Self->WriteBuffer.resize(client_bytes_written + offset);
      }
      else {
         if (client_bytes_written < 0) error = ERR::Read;
         client_bytes_written = 0;
         Self->WriteBuffer.resize(offset);
         log.warning("Input file read error: %s", GetErrorMsg(error));
      }

      int64_t size = 0;
      Self->flInput->getSize(size);

      if ((Self->flInput->Position IS size) or (client_bytes_written IS 0)) {
         log.trace("All file content read (%d bytes) - freeing file.", (int)size);
         FreeResource(Self->flInput);
         Self->flInput = nullptr;
         if (!error) error = ERR::Terminate;
      }
   }
   else if (Self->InputObjectID) {
      log.detail("Sending content from InputObject #%d.", Self->InputObjectID);

      kt::ScopedObjectLock object(Self->InputObjectID, 100);
      if (object.granted()) {
         int offset = (Self->Chunked ? CHUNK_LENGTH_OFFSET : 0);
         Self->WriteBuffer.resize(Self->BufferSize + offset);
         error = acRead(*object, Self->WriteBuffer.data() + offset, std::ssize(Self->WriteBuffer) - offset, &client_bytes_written);
         if (((!error) or (error IS ERR::Terminate)) and (client_bytes_written >= 0)) {
            Self->WriteBuffer.resize(client_bytes_written + offset);
         }
         else {
            if (client_bytes_written < 0) error = ERR::Read;
            client_bytes_written = 0;
            Self->WriteBuffer.resize(offset);
         }
      }
      else error = ERR::Lock;

      if (error != ERR::Okay) log.warning("Input object read error: %s", GetErrorMsg(error));
   }
   else {
      if (Self->MultipleInput) error = ERR::NoData;
      else error = ERR::Terminate;

      log.warning("Method %d: No input fields are defined for me to send data to the server.", int(Self->Method));
   }

   if (((!error) or (error IS ERR::Terminate)) and (client_bytes_written > 0)) {
      int bytes_sent;
      ERR write_error;
      auto write_length = std::ssize(Self->WriteBuffer);

      if (Self->Chunked) {
         // Chunked encoding requires the length of each chunk to be sent in hexadecimal format followed by CRLF,
         // then the data, then another CRLF.
         auto len = std::ssize(Self->WriteBuffer) - CHUNK_LENGTH_OFFSET;
         std::format_to(Self->WriteBuffer.begin(), "{:08x}\r\n", len); // Use the full 10 bytes allocated earlier

         // Write the trailing CRLF to signal the end of the chunk;
         // Note that the HTTP packet terminator comes later.

         Self->WriteBuffer.push_back('\r');
         Self->WriteBuffer.push_back('\n');
         write_length = std::ssize(Self->WriteBuffer);

         // Note: If the result were to come back as less than the length we intended to write,
         // it would screw up the entire sending process when using chunks.  However we don't
         // have to worry as the NetSocket has its own buffer - we're safe as long as we're only
         // sending data when the outgoing socket is ready.
      }

      log.trace("Writing %" PRId64 " bytes (of expected %" PRId64 ") to socket.  Chunked: %d",
         write_length, Self->ContentLength, Self->Chunked);

      write_error = acWrite(Self->Socket, Self->WriteBuffer.data(), write_length, &bytes_sent);

      if ((write_error IS ERR::Okay) and (bytes_sent IS write_length)) {
         if (Self->Chunked) bytes_sent -= CHUNK_LENGTH_OFFSET + 2; // Discount chunk information
         Self->setIndex(Self->Index + bytes_sent); // Update the index by the amount of actual data sent, not including chunk headers/footers
         Self->TotalSent += bytes_sent;
      }
      else if (write_error IS ERR::Okay) {
         log.warning("Only sent %" PRId64 " of %" PRId64 " bytes.", int64_t(bytes_sent), int64_t(write_length));
         error = ERR::Write;
      }
      else {
         log.warning("acWrite() failed: %s", GetErrorMsg(write_error));
         error = write_error;
      }

      log.trace("Outgoing index now %" PRId64 " of %" PRId64, Self->Index, Self->ContentLength);
   }
   else log.trace("Finishing (an error occurred (%d), or there is no more content to write to socket).", error);

   if ((error != ERR::Okay) and (error != ERR::Terminate) and (error != ERR::TimeOut)) {
      // In the event of a send error, the connection is immediately dropped and the transmission
      // is considered irrecoverable.
      Self->setCurrentState(HGS::TERMINATED);
      Self->Error = error;
      return ERR::Terminate;
   }
   else {
      // Check for multiple input files, open the next one if necessary

      if ((Self->MultipleInput) and (!Self->flInput)) {
         log.detail("Sequential input stream has uploaded %" PRId64 "/%" PRId64 " bytes.", Self->Index, Self->ContentLength);

         std::string filepath;
         if (!parse_file(Self, filepath)) {
            if ((Self->flInput = objFile::create::local(fl::Path(filepath), fl::Flags(FL::READ)))) {
               goto continue_upload;
            }
         }
      }

      // Check if the upload is complete - either Index >= ContentLength or ERR::Terminate has been given as the return code.
      //
      // Note: On completion of an upload, the HTTP server will normally send back a message to confirm completion of
      // the upload, therefore the state is not changed to HGS::COMPLETED.
      //
      // In the case where the server does not respond to completion of the upload, the timeout would eventually take care of it.

      if (((Self->ContentLength > 0) and (Self->Index >= Self->ContentLength)) or (error IS ERR::Terminate)) {
         int result;

         if (Self->Chunked) {
            auto write_error = acWrite(Self->Socket, (uint8_t *)"0\r\n\r\n", 5, &result);
            if ((write_error != ERR::Okay) or (result != 5)) {
               Self->Error = (write_error != ERR::Okay) ? write_error : ERR::Write;
               Self->setCurrentState(HGS::TERMINATED);
               return ERR::Terminate;
            }
         }

         log.detail("Transfer complete - sent %" PRId64 " bytes.", Self->TotalSent);
         Self->setCurrentState(HGS::SEND_COMPLETE);
         return ERR::Terminate;
      }
      else log.detail("Sent %" PRId64 " bytes of %" PRId64, Self->Index, Self->ContentLength);
   }

   // Data timeout when uploading is high due to content buffering
continue_upload:
   Self->LastReceipt = PreciseTime();

   double time_limit = (Self->DataTimeout > 30) ? Self->DataTimeout : 30;

   if (Self->TimeoutManager) UpdateTimer(Self->TimeoutManager, time_limit);
   else SubscribeTimer(time_limit, C_FUNCTION(http_timeout), &Self->TimeoutManager);

   Self->WriteBuffer.resize(0);

   if (Self->Error != ERR::Okay) return ERR::Terminate;
   return ERR::Okay;
}

//********************************************************************************************************************

static void writehex(HASH Bin, HASHHEX Hex)
{
   for (uint32_t i=0; i < HASHLEN; i++) {
      uint8_t j = (Bin[i] >> 4) & 0xf;

      if (j <= 9) Hex[i<<1] = (j + '0');
      else Hex[i*2] = (j + 'a' - 10);

      j = Bin[i] & 0xf;

      if (j <= 9) Hex[(i<<1)+1] = (j + '0');
      else Hex[(i<<1)+1] = (j + 'a' - 10);
   }
   Hex[HASHHEXLEN] = 0;
}

//********************************************************************************************************************
// Calculate H(A1) as per spec

static void digest_calc_ha1(extHTTP *Self, HASHHEX SessionKey)
{
   MD5Context md5;
   HASH HA1;

   MD5Init(&md5);

   if (!Self->Username.empty()) MD5Update(&md5, (uint8_t *)Self->Username.c_str(), std::ssize(Self->Username));

   MD5Update(&md5, (uint8_t *)":", 1);

   if (!Self->Realm.empty()) MD5Update(&md5, (uint8_t *)Self->Realm.c_str(), std::ssize(Self->Realm));

   MD5Update(&md5, (uint8_t *)":", 1);

   if (!Self->Password.empty()) MD5Update(&md5, (uint8_t *)Self->Password.c_str(), std::ssize(Self->Password));

   MD5Final((uint8_t *)HA1, &md5);

   if (kt::iequals(Self->AuthAlgorithm, "md5-sess")) {
      MD5Init(&md5);
      MD5Update(&md5, (uint8_t *)HA1, HASHLEN);
      MD5Update(&md5, (uint8_t *)":", 1);
      if (!Self->AuthNonce.empty()) MD5Update(&md5, (uint8_t *)Self->AuthNonce.c_str(), std::ssize(Self->AuthNonce));
      MD5Update(&md5, (uint8_t *)":", 1);
      MD5Update(&md5, (uint8_t *)Self->AuthCNonce.c_str(), std::ssize(Self->AuthCNonce));
      MD5Final((uint8_t *)HA1, &md5);
   }

   writehex(HA1, SessionKey);
}

//********************************************************************************************************************
// Calculate request-digest/response-digest as per HTTP Digest spec

static void digest_calc_response(extHTTP *Self, std::string Request, CSTRING NonceCount, HASHHEX HA1, HASHHEX HEntity, HASHHEX Response)
{
   kt::Log log;
   MD5Context md5;
   HASH ha2, response_hash;
   HASHHEX ha2_hex;
   int i;

   // Calculate H(A2)

   MD5Init(&md5);

   auto req = Request.c_str();
   for (i=0; req[i] > 0x20; i++);
   MD5Update(&md5, (uint8_t *)req, i); // Compute MD5 from the name of the HTTP method that we are calling
   while ((req[i]) and (req[i] <= 0x20)) i++; // Skip whitespace

   MD5Update(&md5, (uint8_t *)":", 1);

   req += i;
   for (i=0; req[i] > 0x20; i++);
   MD5Update(&md5, (uint8_t *)req, i); // Compute MD5 from the path of the HTTP method that we are calling

   if (kt::iequals(Self->AuthQOP, "auth-int")) {
      MD5Update(&md5, (uint8_t *)":", 1);
      MD5Update(&md5, (uint8_t *)HEntity, HASHHEXLEN);
   }

   MD5Final((uint8_t *)ha2, &md5);
   writehex(ha2, ha2_hex);

   // Calculate response:  HA1Hex:Nonce:NonceCount:CNonce:auth:HA2Hex

   MD5Init(&md5);
   MD5Update(&md5, (uint8_t *)HA1, HASHHEXLEN);
   MD5Update(&md5, (uint8_t *)":", 1);
   MD5Update(&md5, (uint8_t *)Self->AuthNonce.c_str(), std::ssize(Self->AuthNonce));
   MD5Update(&md5, (uint8_t *)":", 1);

   if (!Self->AuthQOP.empty()) {
      MD5Update(&md5, (uint8_t *)NonceCount, strlen((CSTRING)NonceCount));
      MD5Update(&md5, (uint8_t *)":", 1);
      MD5Update(&md5, (uint8_t *)Self->AuthCNonce.c_str(), std::ssize(Self->AuthCNonce));
      MD5Update(&md5, (uint8_t *)":", 1);
      MD5Update(&md5, (uint8_t *)Self->AuthQOP.c_str(), std::ssize(Self->AuthQOP));
      MD5Update(&md5, (uint8_t *)":", 1);
   }

   MD5Update(&md5, (uint8_t *)ha2_hex, HASHHEXLEN);
   MD5Final((uint8_t *)response_hash, &md5);
   writehex(response_hash, Response);

   log.trace("%s:%s:%s:%s:%s:%s", HA1, Self->AuthNonce.c_str(), NonceCount, Self->AuthCNonce.c_str(), Self->AuthQOP.c_str(), ha2_hex);
}

//********************************************************************************************************************
// The timer is used for managing time-outs on connection to and the receipt of data from the http server.  If the
// timer is activated then we close the current socket.  It should be noted that if the content is streamed, then
// it is not unusual for the client to remain unnotified even in the event of a complete transfer.  Because of this,
// the client should check if the content is streamed in the event of a timeout and not necessarily assume failure.

static ERR http_timeout(extHTTP *Self, int64_t Elapsed, int64_t CurrentTime)
{
   kt::Log log(__FUNCTION__);

   if (!Self->Socket) {
      log.warning("HTTP timeout fired without an active socket.");
      Self->Error = ERR::TimeOut;
      if (Self->CurrentState < HGS::COMPLETED) Self->setCurrentState(HGS::TERMINATED);
      return ERR::Terminate;
   }

   if (Self->Socket->State IS NTC::CONNECTED) {
      log.warning("Data timeout (%gs) - disconnecting from server .", Self->DataTimeout);
   }
   else log.warning("Connect timeout (%gs)", Self->ConnectTimeout);

   Self->TimeoutManager = 0;
   Self->Error = ERR::TimeOut;
   Self->setCurrentState(HGS::TERMINATED);
   return ERR::Terminate;
}

//********************************************************************************************************************
// Returns ERR::True if the transmission is complete and also sets status to HGS::COMPLETED, otherwise ERR::False.

static ERR check_incoming_end(extHTTP *Self)
{
   kt::Log log(__FUNCTION__);

   if (Self->CurrentState IS HGS::AUTHENTICATING) return ERR::False;
   if (Self->CurrentState >= HGS::COMPLETED) return ERR::True;

   if ((Self->ContentLength != -1) and (Self->Index >= Self->ContentLength)) {
      log.trace("Transmission over.");
      if (Self->Index > Self->ContentLength) log.warning("Warning: received too much content.");
      Self->setCurrentState(HGS::COMPLETED);
      return ERR::True;
   }
   else {
      log.trace("Transmission continuing.");
      return ERR::False;
   }
}

//********************************************************************************************************************

static void set_http_method(extHTTP *Self, CSTRING Method, std::ostringstream &Cmd)
{
   if ((not Self->ProxyServer.empty()) and ((Self->Flags & HTF::SSL) IS HTF::NIL)) {
      // Normal proxy request without SSL tunneling
      Cmd << Method << " " << ((Self->Port IS 443) ? "https" : "http") << "://" << Self->Host << ":" <<
         Self->Port << "/" << Self->Path << " HTTP/1.1" << CRLF;
   }
   else Cmd << Method << " /" << Self->Path << " HTTP/1.1" << CRLF;

   Cmd << "Host: " << Self->Host << CRLF;
   Cmd << "User-Agent: " << (Self->UserAgent.empty() ? "Kotuku Client" : Self->UserAgent.c_str()) << CRLF;
}

//********************************************************************************************************************

  static ERR parse_file(extHTTP *Self, std::string &Buffer)
  {
     auto file = Self->InputFile.c_str();
     auto pos = Self->InputPos;
     while (char ch = file[pos]) {
        if (ch IS '"') {
           ++pos;
           auto start = file + pos;
           while (file[pos] and file[pos] != '"') ++pos;
           Buffer.append(start, file + pos);
           if (file[pos] IS '"') ++pos;
        }
        else if (ch IS '|') {
           ++pos;
           while (file[pos] and file[pos] <= 0x20) ++pos;
           break;
        }
        else { // Find end of non-special characters
           auto start = file + pos;
           while (file[pos] and (file[pos] != '"') and (file[pos] != '|')) ++pos;
           Buffer.append(start, file + pos);
        }
     }

     Self->InputPos = pos;
     return ERR::Okay;
  }

//********************************************************************************************************************

static void parse_file(extHTTP *Self, std::ostringstream &Cmd)
{
   auto pos = Self->InputPos;
   while (Self->InputFile[pos]) {
      if (Self->InputFile[pos] IS '"') {
         pos++;
         while ((Self->InputFile[pos]) and (Self->InputFile[pos] != '"')) {
            Cmd.put(Self->InputFile[pos++]);
         }
         if (Self->InputFile[pos] IS '"') pos++;
      }
      else if (Self->InputFile[pos] IS '|') {
         pos++;
         while ((Self->InputFile[pos]) and (Self->InputFile[pos] <= 0x20)) pos++;
         break;
      }
      else Cmd.put(Self->InputFile[pos++]);
   }
   Self->InputPos = pos;
}
