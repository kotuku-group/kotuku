
//********************************************************************************************************************
// Perform SSL connect and handshake

SSL_ERROR_CODE ssl_connect(SSL_HANDLE SSL, void *SocketHandle, const std::string &HostName)
{
   if ((!SSL) or ((SOCKET)SocketHandle == INVALID_SOCKET)) return SSL_ERROR_ARGS;

   SSL->socket_handle = (SOCKET)SocketHandle;
   SSL->hostname = HostName;

   if (auto error = flush_handshake_buffer(SSL); error != SSL_OK) return error;

   if (SSL->context_initialised) return SSL_ERROR_CONNECTING; // Already in handshake process

   // Acquire credentials
   SCHANNEL_CRED cred_data{};
   cred_data.dwVersion = SCHANNEL_CRED_VERSION;

   if (SSL->validate_credentials) cred_data.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION;
   else cred_data.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION;

   cred_data.grbitEnabledProtocols = 0; // Use system defaults

   TimeStamp expiry;
   auto status = AcquireCredentialsHandle(nullptr, const_cast<char*>(UNISP_NAME), SECPKG_CRED_OUTBOUND,
      nullptr, &cred_data, nullptr, nullptr, &SSL->credentials, &expiry);

   if (status != SEC_E_OK) {
      set_error_status(SSL, status);
      return SSL_ERROR_FAILED;
   }

   SSL->credentials_acquired = true;

   DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                 ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

   SecBuffer out_buffer;
   out_buffer.pvBuffer = nullptr;
   out_buffer.BufferType = SECBUFFER_TOKEN;
   out_buffer.cbBuffer = 0;

   SecBufferDesc out_buffer_desc;
   out_buffer_desc.cBuffers = 1;
   out_buffer_desc.pBuffers = &out_buffer;
   out_buffer_desc.ulVersion = SECBUFFER_VERSION;

   DWORD out_flags;
   status = InitializeSecurityContext(
      &SSL->credentials, nullptr, const_cast<char*>(HostName.c_str()), flags,
      0, SECURITY_NATIVE_DREP, nullptr, 0,
      &SSL->context, &out_buffer_desc, &out_flags, &expiry);

   if (status != SEC_I_CONTINUE_NEEDED) {
      set_error_status(SSL, status);
      return SSL_ERROR_FAILED;
   }

   SSL->context_initialised = true;

   // Send initial handshake data
   if ((out_buffer.cbBuffer > 0) and (out_buffer.pvBuffer != nullptr)) {
      if (auto error = queue_handshake_token(SSL, out_buffer.pvBuffer, out_buffer.cbBuffer); error != SSL_OK) return error;
   }

   // For simplicity, full handshake would need more rounds
   return SSL_NEED_DATA;
}

//********************************************************************************************************************
// Server-side handshake handling using AcceptSecurityContext
// Called by sslServerHandshakeReceived()

SSL_ERROR_CODE ssl_accept(SSL_HANDLE SSL, const void* ClientData, int DataLength)
{
   if ((!SSL) or (!ClientData) or (!SSL->server_certificate) or (DataLength <= 0)) {
      return SSL_ERROR_ARGS;
   }

   // Acquire server credentials if not already done
   if (!SSL->credentials_acquired) {
      SCHANNEL_CRED cred_data;
      memset(&cred_data, 0, sizeof(cred_data));
      cred_data.dwVersion = SCHANNEL_CRED_VERSION;
      cred_data.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION |
                        SCH_CRED_IGNORE_NO_REVOCATION_CHECK | SCH_CRED_IGNORE_REVOCATION_OFFLINE;
      cred_data.cCreds = 1;
      cred_data.paCred = &SSL->server_certificate;

      TimeStamp expiry;
      auto status = AcquireCredentialsHandle(
         nullptr, const_cast<char*>(UNISP_NAME), SECPKG_CRED_INBOUND,  // INBOUND for server
         nullptr, &cred_data, nullptr, nullptr,
         &SSL->credentials, &expiry);

      if (status != SEC_E_OK) {
         set_error_status(SSL, status);
         return SSL_ERROR_FAILED;
      }

      SSL->credentials_acquired = true;
   }

   // Setup input buffer with client data
   SecBuffer in_buffers[2];
   in_buffers[0].pvBuffer   = const_cast<void*>(ClientData);
   in_buffers[0].cbBuffer   = DataLength;
   in_buffers[0].BufferType = SECBUFFER_TOKEN;
   in_buffers[1].pvBuffer   = nullptr;
   in_buffers[1].cbBuffer   = 0;
   in_buffers[1].BufferType = SECBUFFER_EMPTY;

   SecBufferDesc in_buffer_desc;
   in_buffer_desc.cBuffers  = 2;
   in_buffer_desc.pBuffers  = in_buffers;
   in_buffer_desc.ulVersion = SECBUFFER_VERSION;

   // Setup output buffer for response
   SecBuffer out_buffer;
   out_buffer.pvBuffer   = nullptr;
   out_buffer.BufferType = SECBUFFER_TOKEN;
   out_buffer.cbBuffer   = 0;

   SecBufferDesc out_buffer_desc;
   out_buffer_desc.cBuffers  = 1;
   out_buffer_desc.pBuffers  = &out_buffer;
   out_buffer_desc.ulVersion = SECBUFFER_VERSION;

   DWORD context_attr;
   TimeStamp expiry;

   SECURITY_STATUS status;
   if (!SSL->context_initialised) {
      status = AcceptSecurityContext(
         &SSL->credentials,
         nullptr,
         &in_buffer_desc,
         ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
         ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM |
         ASC_REQ_MUTUAL_AUTH,
         SECURITY_NATIVE_DREP,
         &SSL->context,
         &out_buffer_desc,
         &context_attr,
         &expiry);

      if (status == SEC_E_OK or status == SEC_I_CONTINUE_NEEDED) {
         SSL->context_initialised = true;
      }
   }
   else {
      status = AcceptSecurityContext(
         &SSL->credentials,
         &SSL->context,
         &in_buffer_desc,
         ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
         ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM |
         ASC_REQ_MUTUAL_AUTH,
         SECURITY_NATIVE_DREP,
         &SSL->context,
         &out_buffer_desc,
         &context_attr,
         &expiry);
   }

   if ((status == SEC_E_OK) or (status == SEC_I_CONTINUE_NEEDED)) {
      if (status == SEC_E_OK) {
         auto stream_status = QueryContextAttributes(&SSL->context, SECPKG_ATTR_STREAM_SIZES, &SSL->stream_sizes);
         if (stream_status != SEC_E_OK) {
            SSL->last_security_status = stream_status;
            return SSL_ERROR_FAILED;
         }
      }

      if (out_buffer.pvBuffer and out_buffer.cbBuffer > 0) {
         auto bytes_sent = send(SSL->socket_handle, (const char *)out_buffer.pvBuffer, out_buffer.cbBuffer, 0);
         if (bytes_sent < 0) return SSL_ERROR_FAILED;
         else if (unsigned(bytes_sent) != out_buffer.cbBuffer) return SSL_ERROR_FAILED;

         FreeContextBuffer(out_buffer.pvBuffer);
      }

      if (status == SEC_E_OK) return SSL_OK;
      else return SSL_NEED_DATA;
   }
   else {
      set_error_status(SSL, status);
      if (out_buffer.pvBuffer) FreeContextBuffer(out_buffer.pvBuffer);
      return SSL_ERROR_FAILED;
   }
}
