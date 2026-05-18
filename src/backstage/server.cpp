
static BackstageHttpResponse process_http_request(objClientSocket *Client, std::string_view RawRequest)
{
   BackstageHttpRequest request;
   auto status = parse_http_request(RawRequest, request);

   if (status IS HttpParseStatus::BAD_REQUEST) return plain_http_response(400, "Bad Request");
   if (status IS HttpParseStatus::PAYLOAD_TOO_LARGE) return plain_http_response(413, "Payload Too Large");

   return dispatch_route_request(Client, request);
}

//********************************************************************************************************************

static void server_feedback(objNetServer *Server, class objClientSocket *Client, NTC State)
{
   kt::Log log(__FUNCTION__);

   if (State IS NTC::CONNECTED) {
      log.msg("Client socket #%d connected.", Client->UID);
   }
   else if (State IS NTC::DISCONNECTED) {
      log.msg("Client socket #%d disconnected.", Client->UID);
      std::lock_guard<std::mutex> lock(glRequestLock);
      glRequestBuffers.erase(Client->UID);
   }
   else log.msg("Unknown state: %d", int(State));
}

//********************************************************************************************************************

static ERR server_incoming(objNetServer *Server, objClientSocket *Client, APTR Meta)
{
   kt::Log log(__FUNCTION__);
   std::array<char, 4096> buffer;
   int len = 0;

   auto error = Client->read(buffer.data(), buffer.size(), &len);
   if (error IS ERR::Disconnected) return ERR::Terminate;
   if (error != ERR::Okay) return error;
   if (not len) return ERR::Okay;

   log.trace("Received %d bytes from client socket #%d", len, Client->UID);

   std::string request;
   HttpBufferState buffer_state = HttpBufferState::INCOMPLETE;

   {
      std::lock_guard<std::mutex> lock(glRequestLock);
      auto &client_buffer = glRequestBuffers[Client->UID];
      client_buffer.append(buffer.data(), size_t(len));
      buffer_state = analyse_http_buffer(client_buffer);

      if (not (buffer_state IS HttpBufferState::INCOMPLETE)) {
         request = client_buffer;
         glRequestBuffers.erase(Client->UID);
      }
   }

   if ((buffer_state IS HttpBufferState::HEADER_TOO_LARGE) or (buffer_state IS HttpBufferState::BAD_REQUEST)) {
      write_response(Client, plain_http_response(400, "Bad Request"));
      return ERR::Okay;
   }

   if (buffer_state IS HttpBufferState::BODY_TOO_LARGE) {
      write_response(Client, plain_http_response(413, "Payload Too Large"));
      return ERR::Okay;
   }

   if (buffer_state IS HttpBufferState::COMPLETE) write_response(Client, process_http_request(Client, request));

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR init_backstage(int Port)
{
   kt::Log log(__FUNCTION__);

   if (objModule::load("network", &modNetwork, &NetworkBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("regex", &modRegex, &RegexBase) != ERR::Okay) return ERR::InitModule;

   if (auto error = compile_backstage_routes(); error != ERR::Okay) return error;

   glServer = objNetServer::create::global({
      FieldValue(FID_Address, "127.0.0.1"),
      fl::Port(Port),
      fl::Flags(int(NSF::MULTI_CONNECT|NSF::KEEP_ALIVE)),
      fl::Feedback((CPTR)server_feedback),
      fl::Incoming((CPTR)server_incoming)
   });

   if (not glServer) {
      log.msg("Failed to initialise backstage server on port %d", Port);
      return ERR::CreateObject;
   }
   else {
      log.msg("Backstage is enabled at http://localhost:%d/", Port);
      return ERR::Okay;
   }
}
