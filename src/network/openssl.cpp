
static SSL_CTX *glClientSSL = nullptr; // Thread-safe unless you call a SET function
static SSL_CTX *glClientSSLNV = nullptr; // No-verify version
static std::mutex glSSLSetupMutex;
static bool glSSLInitialised = false;

static ERR loadPKCS12Certificate(const std::string &, std::optional<const std::string> &, SSL_CTX *);
static ERR loadPEMCertificate(const std::string &, std::optional<const std::string> &, std::optional<const std::string> &, SSL_CTX *);

//********************************************************************************************************************

// Forward declarations for template functions
template <class T> void ssl_handshake_write_impl(HOSTHANDLE SocketFD, T *Self);
template <class T> void ssl_handshake_read_impl(HOSTHANDLE SocketFD, T *Self);

// Non-template wrappers for RegisterFD callbacks
static void ssl_handshake_write_netsocket(HOSTHANDLE SocketFD, APTR Data) {
   ssl_handshake_write_impl(SocketFD, (extNetSocket *)Data);
}

static void ssl_handshake_read_netsocket(HOSTHANDLE SocketFD, APTR Data) {
   ssl_handshake_read_impl(SocketFD, (extNetSocket *)Data);
}

static void ssl_handshake_write_clientsocket(HOSTHANDLE SocketFD, APTR Data) {
   ssl_handshake_write_impl(SocketFD, (extClientSocket *)Data);
}

static void ssl_handshake_read_clientsocket(HOSTHANDLE SocketFD, APTR Data) {
   ssl_handshake_read_impl(SocketFD, (extClientSocket *)Data);
}

// Template wrappers for direct calls with SocketHandle
template <class T> void ssl_handshake_write(SocketHandle Socket, T *Self) {
   ssl_handshake_write_impl(Socket.hosthandle(), Self);
}

template <class T> void ssl_handshake_read(SocketHandle Socket, T *Self) {
   ssl_handshake_read_impl(Socket.hosthandle(), Self);
}

static void ssl_suspend_write_queue(HOSTHANDLE SocketFD)
{
   network_platform().remove_write(network_platform().socket_from_hosthandle(SocketFD));
}

template <class T> void ssl_resume_write_handshake(HOSTHANDLE SocketFD, T *Self)
{
   auto write_callback = std::is_same<T, extNetSocket>::value ?
      ssl_handshake_write_netsocket : ssl_handshake_write_clientsocket;
   network_platform().register_write(network_platform().socket_from_hosthandle(SocketFD), write_callback, Self);
}

template <class T> void ssl_resume_write_queue(HOSTHANDLE SocketFD, T *Self)
{
   if (Self->WriteQueue.Buffer.empty()) {
      if constexpr (std::is_same<T, extNetSocket>::value) {
         if (!Self->Outgoing.defined()) return;
      }
      else {
         auto server = (extNetSocket *)(Self->Client->Owner);
         if (!server->Outgoing.defined()) return;
      }
   }

   auto outgoing_callback = std::is_same<T, extNetSocket>::value ? netsocket_outgoing : clientsocket_outgoing;
   network_platform().register_write(network_platform().socket_from_hosthandle(SocketFD), outgoing_callback, Self);
}

static bool ssl_has_buffered_read_data(SSL *Handle)
{
   if (!Handle) return false;
   if (SSL_pending(Handle) > 0) return true;

   #if OPENSSL_VERSION_NUMBER >= 0x10100000L
      return SSL_has_pending(Handle) != 0;
   #else
      return false;
   #endif
}

static CSTRING ssl_error_name(int Error)
{
   switch(Error) {
      case SSL_ERROR_NONE:             return "SSL_ERROR_NONE";
      case SSL_ERROR_ZERO_RETURN:      return "SSL_ERROR_ZERO_RETURN";
      case SSL_ERROR_WANT_READ:        return "SSL_ERROR_WANT_READ";
      case SSL_ERROR_WANT_WRITE:       return "SSL_ERROR_WANT_WRITE";
      case SSL_ERROR_WANT_CONNECT:     return "SSL_ERROR_WANT_CONNECT";
      case SSL_ERROR_WANT_ACCEPT:      return "SSL_ERROR_WANT_ACCEPT";
      case SSL_ERROR_WANT_X509_LOOKUP: return "SSL_ERROR_WANT_X509_LOOKUP";
      case SSL_ERROR_SYSCALL:          return "SSL_ERROR_SYSCALL";
      case SSL_ERROR_SSL:              return "SSL_ERROR_SSL";
      default:                         return "SSL_ERROR_UNKNOWN";
   }
}

static void ssl_log_error_queue(kt::Log &Log, CSTRING Context)
{
   unsigned long ssl_error;
   bool logged = false;

   while ((ssl_error = ERR_get_error()) != 0) {
      char buffer[256];
      ERR_error_string_n(ssl_error, buffer, sizeof(buffer));
      Log.warning("%s: %s", Context, buffer);
      logged = true;
   }

   if (!logged) Log.warning("%s: SSL_ERROR_SSL reported with an empty OpenSSL error queue.", Context);
}

static void ssl_clear_error_queue()
{
   ERR_clear_error();
}

static bool ssl_unexpected_eof()
{
   #ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
      auto ssl_error = ERR_peek_error();
      return ssl_error and (ERR_GET_REASON(ssl_error) IS SSL_R_UNEXPECTED_EOF_WHILE_READING);
   #else
      return false;
   #endif
}

template <class T> void tls_disconnect(T *Self)
{
   if (Self->TLS.Handle) {
      kt::Log log(__FUNCTION__);

      log.traceBranch("Closing SSL connection.");

      SSL_set_info_callback(Self->TLS.Handle, nullptr);

      // Perform proper bidirectional SSL shutdown

      ssl_clear_error_queue();
      if (auto shutdown_result = SSL_shutdown(Self->TLS.Handle); shutdown_result IS 0) {
         // First shutdown call completed, perform second shutdown for bidirectional close
         ssl_clear_error_queue();
         shutdown_result = SSL_shutdown(Self->TLS.Handle);
         if (shutdown_result < 0) {
            int ssl_error = SSL_get_error(Self->TLS.Handle, shutdown_result);
            if ((ssl_error != SSL_ERROR_WANT_READ) and (ssl_error != SSL_ERROR_WANT_WRITE)) {
               log.warning("SSL_shutdown failed: %s", ssl_error_name(ssl_error));
               if (ssl_error IS SSL_ERROR_SSL) ssl_log_error_queue(log, "SSL_shutdown");
            }
         }
      }

      SSL_free(Self->TLS.Handle);
      Self->TLS.Handle = nullptr;
      Self->TLS.BIOHandle = nullptr; // BIO is terminated by SSL_free()
   }
}

//********************************************************************************************************************

static void sslMsgCallback(const ssl_st *s, int where, int ret)
{
   const char *state;
   kt::Log log(__FUNCTION__);

   int w = where & (~SSL_ST_MASK);

   if (w & SSL_ST_CONNECT) state = "SSL_Connect";
   else if (w & SSL_ST_ACCEPT) state = "SSL_Accept";
   else if (w & TLS_ST_BEFORE) state = "TLS_Before";
   else if (w & TLS_ST_OK) state = "TLS_OK";
   else if (w IS SSL_F_SSL_RENEGOTIATE) state = "SSL_Renegotiate";
   else state = "SSL_Undefined";

   if (where & SSL_CB_LOOP) {
      log.msg("%s: Loop: %s", state, SSL_state_string_long(s));
   }
   else if (where & SSL_CB_ALERT) {
      log.msg("%s: %s Alert: %s : %s", state, (where & SSL_CB_READ) ? "Read" : "Write", SSL_alert_type_string_long(ret), SSL_alert_desc_string_long(ret));
   }
   else if (where & SSL_CB_EXIT) {
      if (ret == 0) log.msg("%s: Failed in %s", state, SSL_state_string_long(s));
      else if (ret < 0) log.msg("%s: Error in %s", state, SSL_state_string_long(s));
   }
   else if (where & SSL_CB_HANDSHAKE_START) {
      log.msg("%s: Handshake Start: %s", state, SSL_state_string_long(s));
   }
   else if (where & SSL_CB_HANDSHAKE_DONE) {
      log.msg("%s: Handshake Done: %s", state, SSL_state_string_long(s));
   }
   else log.msg("%s: Unknown: %s", state, SSL_state_string_long(s));
}

//********************************************************************************************************************

static void sslCtxMsgCallback(const SSL *s, int where, int ret) __attribute__ ((unused));
static void sslCtxMsgCallback(const SSL *s, int where, int ret)
{
   sslMsgCallback(s, where, ret);
}

//********************************************************************************************************************
// Load custom certificate from file for OpenSSL server context

static ERR loadCustomCertificateOpenSSL(extNetServer *Self, SSL_CTX *ctx)
{
   kt::Log log(__FUNCTION__);

   ssl_certificate_paths paths;
   if (auto error = resolve_ssl_certificate_paths(Self, paths); error != ERR::Okay) return log.warning(error);

   if (paths.Format IS SSLCERTFORMAT::PKCS12) {
      return loadPKCS12Certificate(paths.Certificate, paths.Password, ctx);
   }
   else if (paths.Format IS SSLCERTFORMAT::PEM) {
      return loadPEMCertificate(paths.Certificate, paths.PrivateKey, paths.Password, ctx);
   }
   else return log.warning(ERR::InvalidData);
}

//********************************************************************************************************************
// Load PEM format certificate and private key

static ERR loadPEMCertificate(const std::string &certPath, std::optional<const std::string> &keyPath, std::optional<const std::string> &password, SSL_CTX *ctx)
{
   kt::Log log(__FUNCTION__);

   FILE *cert_file = fopen(certPath.c_str(), "r");
   if (!cert_file) return log.warning(ERR::File);

   X509 *cert = PEM_read_X509(cert_file, nullptr, nullptr, nullptr);
   fclose(cert_file);

   if (!cert) return log.warning(ERR::InvalidData);

   if (SSL_CTX_use_certificate(ctx, cert) != 1) {
      X509_free(cert);
      return log.warning(ERR::Failed);
   }

   X509_free(cert);

   // Load private key
   std::string key_file_path = keyPath.has_value() ? keyPath.value() : certPath;

   auto key_file = fopen(key_file_path.c_str(), "r");
   if (!key_file) return log.warning(ERR::File);

   EVP_PKEY *pkey = PEM_read_PrivateKey(key_file, nullptr, nullptr, (void*)(password.has_value() ? password.value().c_str() : nullptr));
   fclose(key_file);

   if (!pkey) return log.warning(ERR::InvalidData);

   if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
      EVP_PKEY_free(pkey);
      return log.warning(ERR::Failed);
   }

   EVP_PKEY_free(pkey);

   // Verify certificate and key match
   if (SSL_CTX_check_private_key(ctx) != 1) return log.warning(ERR::Mismatch);

   return ERR::Okay;
}

//********************************************************************************************************************
// Load PKCS#12 format certificate bundle

static ERR loadPKCS12Certificate(const std::string &p12Path, std::optional<const std::string> &password, SSL_CTX *ctx)
{
   kt::Log log(__FUNCTION__);

   auto p12_file = fopen(p12Path.c_str(), "rb");
   if (!p12_file) {
      log.warning("Cannot open PKCS#12 file: %s", p12Path.c_str());
      return ERR::File;
   }

   PKCS12 *p12 = d2i_PKCS12_fp(p12_file, nullptr);
   fclose(p12_file);

   if (!p12) {
      log.warning("Failed to read PKCS#12 file: %s", p12Path.c_str());
      return ERR::InvalidData;
   }

   EVP_PKEY *pkey = nullptr;
   X509 *cert = nullptr;
   STACK_OF(X509) *ca_certs = nullptr;

   if (PKCS12_parse(p12, password.has_value() ? password.value().c_str() : "", &pkey, &cert, &ca_certs) != 1) {
      PKCS12_free(p12);
      log.warning("Failed to parse PKCS#12 file: %s (check password)", p12Path.c_str());
      return ERR::InvalidData;
   }

   PKCS12_free(p12);

   auto result = ERR::Okay;

   if (SSL_CTX_use_certificate(ctx, cert) != 1) {
      log.warning("Failed to use certificate from PKCS#12 file: %s", p12Path.c_str());
      result = ERR::Failed;
   }
   else if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
      log.warning("Failed to use private key from PKCS#12 file: %s", p12Path.c_str());
      result = ERR::Failed;
   }
   else if (SSL_CTX_check_private_key(ctx) != 1) {
      log.warning("Certificate and private key do not match in PKCS#12 file: %s", p12Path.c_str());
      result = ERR::Mismatch;
   }
   else { // Load certificate chain if available
      if (ca_certs) {
         for (int i = 0; i < sk_X509_num(ca_certs); i++) {
            X509 *ca_cert = sk_X509_value(ca_certs, i);
            if (SSL_CTX_add_extra_chain_cert(ctx, ca_cert) != 1) {
               log.warning("Failed to add certificate chain cert %d", i);
            }
            else X509_up_ref(ca_cert); // Increment reference count since SSL_CTX takes ownership
         }
      }
      log.msg("Successfully loaded PKCS#12 certificate and key");
   }

   if (pkey) EVP_PKEY_free(pkey);
   if (cert) X509_free(cert);
   if (ca_certs) sk_X509_pop_free(ca_certs, X509_free);

   return result;
}

//********************************************************************************************************************
// This only needs to be called once to setup OpenSSL's global state.

static ERR tls_initialise(kt::Log &Log)
{
   if (not glSSLInitialised) {
      if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr) != 1) {
         return Log.warning(ERR::SystemCall);
      }
      glSSLInitialised = true;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// This only needs to be called once to setup the unique SSL context for NetServer listeners and certificate locations.

static ERR tls_setup_server(extNetServer *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch();

   ERR error = ERR::Okay;

   std::lock_guard<std::mutex> lock(glSSLSetupMutex);

   if ((error = tls_initialise(log)) != ERR::Okay) return error;

   bool setup_success = false;

   if (Self->ServerSSLContext) return ERR::Okay;

   if (auto server_ctx = SSL_CTX_new(TLS_server_method())) {
      // Check if custom certificate is specified
      if (not Self->SSLCertificate.empty()) {
         log.msg("Loading custom SSL server certificate: %s", Self->SSLCertificate.c_str());
         if ((error = loadCustomCertificateOpenSSL(Self, server_ctx)) IS ERR::Okay) {
            setup_success = true;
            log.msg("Custom SSL server certificate loaded successfully.");
         }
         else {
            log.warning("Failed to load custom SSL certificate: %s", GetErrorMsg(error));
            SSL_CTX_free(server_ctx);
            return error;
         }
      }

      if (!setup_success) {
         log.msg("Generating self-signed certificate for localhost...");
         // Generate a simple self-signed certificate using modern OpenSSL APIs
         EVP_PKEY *pkey = nullptr;
         X509 *cert = nullptr;

         // Generate key pair using EVP interface

         if (EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr); ctx) {
            if (EVP_PKEY_keygen_init(ctx) > 0) {
               if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0) {
                  if (EVP_PKEY_keygen(ctx, &pkey) > 0) {
                     // Create certificate
                     if ((cert = X509_new())) {
                        X509_set_version(cert, 2);
                        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
                        X509_gmtime_adj(X509_get_notBefore(cert), 0);
                        X509_gmtime_adj(X509_get_notAfter(cert), 365 * 24 * 3600);
                        X509_set_pubkey(cert, pkey);

                        auto name = X509_get_subject_name(cert);
                        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)"localhost", -1, -1, 0);
                        X509_set_issuer_name(cert, name);

                        if (X509_sign(cert, pkey, EVP_sha256()) > 0) {
                           if (SSL_CTX_use_certificate(server_ctx, cert) and SSL_CTX_use_PrivateKey(server_ctx, pkey)) {
                              setup_success = true;
                           }
                           else log.warning("Failed to set SSL server certificate and key.");
                        }
                        else log.warning("Failed to sign SSL certificate.");
                     }
                  }
               }
            }
            EVP_PKEY_CTX_free(ctx);
         }

         if (pkey) EVP_PKEY_free(pkey);
         if (cert) X509_free(cert);
      }

      if (!setup_success) {
         log.warning("SSL server certificate setup failed, trying with no certificate verification.");
         // For testing, allow servers without proper certificates
         SSL_CTX_set_verify(server_ctx, SSL_VERIFY_NONE, nullptr);
      }

      Self->ServerSSLContext = server_ctx;
      return ERR::Okay;
   }
   else return log.warning(ERR::SystemCall);
}

//********************************************************************************************************************
// This only needs to be called once to setup the unique SSL context for client sockets.

static ERR tls_setup_client(extNetSocket *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch();

   ERR error = ERR::Okay;

   std::lock_guard<std::mutex> lock(glSSLSetupMutex);

   if ((error = tls_initialise(log)) != ERR::Okay) return error;

   // Client mode - no CA verification

   if ((Self->Flags & NSF::DISABLE_SERVER_VERIFY) != NSF::NIL) {
      if (!glClientSSLNV) {
         if ((glClientSSLNV = SSL_CTX_new(TLS_client_method()))) {
            if (GetResource(RES::LOG_LEVEL) > 7) SSL_CTX_set_info_callback(glClientSSLNV, &sslCtxMsgCallback);
            // Disable certificate verification for client sockets
            SSL_CTX_set_verify(glClientSSLNV, SSL_VERIFY_NONE, nullptr);

            // Additional settings to ensure verification is completely disabled
            SSL_CTX_set_verify_depth(glClientSSLNV, 0);
            SSL_CTX_set_options(glClientSSLNV, SSL_OP_NO_COMPRESSION | SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
         }
         else return log.warning(ERR::SystemCall);
      }

      if ((Self->TLS.Handle = SSL_new(glClientSSLNV))) {
         if (GetResource(RES::LOG_LEVEL) > 7) SSL_set_info_callback(Self->TLS.Handle, &sslMsgCallback);
         return ERR::Okay;
      }
      else return log.warning(ERR::SystemCall);
   }

   // Client mode - full verification

   if (!glClientSSL) {
      if ((glClientSSL = SSL_CTX_new(TLS_client_method()))) {
         if (GetResource(RES::LOG_LEVEL) > 7) SSL_CTX_set_info_callback(glClientSSL, &sslCtxMsgCallback);

         // Try system certificates first (most up-to-date), then bundled certificates as fallback
         bool cert_loaded = false;
         if (SSL_CTX_set_default_verify_paths(glClientSSL)) cert_loaded = true;
         else {
            auto ssl_error = ERR_get_error();
            log.warning("Failed to load system certificate paths - SSL Error: %s", ERR_error_string(ssl_error, nullptr));
         }

         // If system certificates failed, try bundled certificate as fallback

         std::string path;
         if (!cert_loaded) {
            if (!ResolvePath("config:ssl/ca-bundle.crt", RSF::NO_FILE_CHECK, &path)) {
               if (SSL_CTX_load_verify_locations(glClientSSL, path.c_str(), nullptr)) {
                  cert_loaded = true;
               }
               else {
                  auto ssl_error = ERR_get_error();
                  log.warning("Failed to load certificates: %s - SSL Error: %s", path.c_str(), ERR_error_string(ssl_error, nullptr));
               }
            }
            else log.warning(ERR::ResolvePath);
         }

         if (cert_loaded) {
            // Set up certificate verification with detailed callback
            SSL_CTX_set_verify(glClientSSL, SSL_VERIFY_PEER, nullptr);

            // Configure additional SSL context options for better compatibility
            SSL_CTX_set_verify_depth(glClientSSL, 10);  // Allow longer certificate chains

            // Set security level to 1 for broader compatibility (default might be too strict)
            SSL_CTX_set_security_level(glClientSSL, 1);

            // Enable certificate chain checking
            SSL_CTX_set_mode(glClientSSL, SSL_MODE_AUTO_RETRY);

            // Set certificate verification flags for better compatibility
            X509_VERIFY_PARAM *param = SSL_CTX_get0_param(glClientSSL);
            if (param) X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_TRUSTED_FIRST);
         }
         else {
            SSL_CTX_free(glClientSSL);
            glClientSSL = nullptr;
            return ERR::Failed;
         }
      }
   }

   if (glClientSSL) {
      if ((Self->TLS.Handle = SSL_new(glClientSSL))) {
         if (GetResource(RES::LOG_LEVEL) > 7) SSL_set_info_callback(Self->TLS.Handle, &sslMsgCallback);
         return ERR::Okay;
      }
      else log.warning(ERR::SystemCall);
   }

   return error;
}

//********************************************************************************************************************

static ERR sslLinkSocket(extNetSocket *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch();

   if ((Self->TLS.BIOHandle = BIO_new_socket(Self->Handle, BIO_NOCLOSE))) {
      SSL_set_bio(Self->TLS.Handle, Self->TLS.BIOHandle, Self->TLS.BIOHandle);
//      SSL_ctrl(Self->TLS.Handle, SSL_CTRL_MODE,(SSL_MODE_AUTO_RETRY), nullptr); // SSL library will process 'non-application' data automatically [good]
      SSL_ctrl(Self->TLS.Handle, SSL_CTRL_MODE,(SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER), nullptr);
      SSL_ctrl(Self->TLS.Handle, SSL_CTRL_MODE,(SSL_MODE_ENABLE_PARTIAL_WRITE), nullptr);
      return ERR::Okay;
   }
   else return ERR::SystemCall;
}

//********************************************************************************************************************
// Setup SSL state for a newly accepted server-side client socket.

static ERR tls_accept_client(extClientSocket *Self, extNetServer *Server)
{
   kt::Log log(__FUNCTION__);

   if (!Server->ServerSSLContext) {
      if (auto error = tls_setup_server(Server); error != ERR::Okay) return error;
   }

   if (auto client_ssl = SSL_new(Server->ServerSSLContext)) {
      if (auto client_bio = BIO_new_socket(Self->Handle, BIO_NOCLOSE)) {
         SSL_set_bio(client_ssl, client_bio, client_bio);

         Self->TLS.Handle = client_ssl;
         Self->TLS.BIOHandle = client_bio;

         ssl_clear_error_queue();
         if (auto result = SSL_accept(client_ssl); result IS 1) {
            log.trace("SSL handshake successful.");
            Self->setState(NTC::CONNECTED);
         }
         else {
            Self->setState(NTC::HANDSHAKING);

            auto ssl_error = SSL_get_error(client_ssl, result);
            if ((ssl_error IS SSL_ERROR_WANT_READ) or (ssl_error IS SSL_ERROR_WANT_WRITE)) {
               log.msg("SSL handshake in progress...");
            }
            else {
               log.warning("SSL handshake failed: %s", ssl_error_name(ssl_error));
               if (ssl_error IS SSL_ERROR_SSL) ssl_log_error_queue(log, "SSL_accept");
               Self->TLS.Handle = nullptr;
               Self->TLS.BIOHandle = nullptr;
               SSL_free(client_ssl);
               return ERR::SystemCall;
            }
         }

         return ERR::Okay;
      }
      else {
         SSL_free(client_ssl);
         return log.warning(ERR::SystemCall);
      }
   }
   else return log.warning(ERR::SystemCall);
}

//********************************************************************************************************************
// To establish an SSL connection, this function must be called after the initial connect() has succeeded.  If a
// NetSocket has the NSF_SSL flag set, then the connection is handled automatically.  Otherwise a plain text socket
// connection can be converted to SSL at any time (if the server is ready for it) by calling this function.
//
// The state will be changed to NTC::CONNECTED if the SSL connection is established immediately, otherwise
// NTC::HANDSHAKING may be used to indicate that the connection is ongoing.  If a failure occurs, the state is set to
// NTC::DISCONNECTED and the Error field is set appropriately.

static ERR tls_connect(extNetSocket *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch();

   if (!Self->TLS.Handle) return ERR::FieldNotSet;

   // Ensure the SSL BIO is linked to the socket before attempting connection

   if (!Self->TLS.BIOHandle) {
      if (auto error = sslLinkSocket(Self); error != ERR::Okay) {
         log.warning("Failed to link SSL socket to BIO.");
         return error;
      }
   }

   // Set SNI (Server Name Indication) if we have a hostname
   // This is critical for modern HTTPS servers that serve multiple domains

   if (!Self->Address.empty()) {
      auto address = Self->Address.c_str();

      // Only set SNI for client connections, and only if Address is a hostname (not IP)
      struct in_addr addr;
      if (inet_aton(address, &addr) IS 0) {
         // Address is not an IP, so it's likely a hostname - set SNI
         if (SSL_set_tlsext_host_name(Self->TLS.Handle, address)) {
            log.msg("SNI set to: %s", address);
         }
         else log.warning("Failed to set SNI hostname: %s", address);
      }
   }

   ssl_clear_error_queue();
   if (auto result = SSL_connect(Self->TLS.Handle); result <= 0) {
      result = SSL_get_error(Self->TLS.Handle, result);

      // The SSL routine may respond with WANT_READ or WANT_WRITE when
      // non-blocking sockets are used.  This is technically not an error.

      switch(result) {
         case SSL_ERROR_NONE:             Self->Error = ERR::Okay;
                                          return ERR::Okay;

         case SSL_ERROR_ZERO_RETURN:      Self->Error = ERR::Disconnected; break;

         case SSL_ERROR_WANT_READ:        Self->setState(NTC::HANDSHAKING);
                                          return ERR::Okay;

         case SSL_ERROR_WANT_WRITE:       Self->setState(NTC::HANDSHAKING);
                                          return ERR::Okay;

         case SSL_ERROR_WANT_CONNECT:     Self->Error = ERR::WouldBlock; break;
         case SSL_ERROR_WANT_ACCEPT:      Self->Error = ERR::WouldBlock; break;
         case SSL_ERROR_WANT_X509_LOOKUP: Self->Error = ERR::Retry; break;
         case SSL_ERROR_SYSCALL:          Self->Error = ERR::InputOutput; break;

         case SSL_ERROR_SSL:              Self->Error = ERR::SystemCall;
                                          ssl_log_error_queue(log, "SSL_connect");
                                          break;

         default:                         Self->Error = ERR::Failed;
      }

      log.warning("SSL_connect: %s (%s)", ssl_error_name(result), GetErrorMsg(Self->Error));
      Self->setState(NTC::DISCONNECTED);
      return Self->Error;
   }
   else {
      Self->setState(NTC::CONNECTED);
      return ERR::Okay;
   }
}

//********************************************************************************************************************
// Handshaking may be required during normal read/write operations.  This routine simply tells SSL to continue with its
// handshake and then ceases monitoring of the FD.  If SSL then needs to continue its handshake then it will tell us in
// the RECEIVE() and SEND() functions.

template <class T> void ssl_handshake_write_impl(HOSTHANDLE SocketFD, T *Self)
{
   kt::Log log(__FUNCTION__);
   auto Socket = network_platform().socket_from_hosthandle(SocketFD);

   log.trace("Socket: %" PF64, (MAXINT)SocketFD);

   auto read_callback = std::is_same<T, extNetSocket>::value ?
      ssl_handshake_read_netsocket : ssl_handshake_read_clientsocket;

   ssl_clear_error_queue();
   if (auto result = SSL_do_handshake(Self->TLS.Handle); result IS 1) { // Handshake successful, connection established
      network_platform().remove_write(Socket);
      Self->TLS.HandshakeStatus = SHS::NIL;
      ssl_resume_write_queue(Socket.hosthandle(), Self);
   }
   else switch (auto ssl_error = SSL_get_error(Self->TLS.Handle, result)) {
      case SSL_ERROR_WANT_READ:
         network_platform().remove_write(Socket);
         Self->TLS.HandshakeStatus = SHS::READ;
         network_platform().register_read(Socket, read_callback, Self);
         break;

      case SSL_ERROR_WANT_WRITE:
         // Continue monitoring for write readiness - no action needed
         break;

      default:
         log.warning("SSL_do_handshake failed: %s", ssl_error_name(ssl_error));
         if (ssl_error IS SSL_ERROR_SSL) ssl_log_error_queue(log, "SSL_do_handshake");
         Self->TLS.HandshakeStatus = SHS::NIL;
   }
}

template <class T> void ssl_handshake_read_impl(HOSTHANDLE SocketFD, T *Self)
{
   kt::Log log(__FUNCTION__);
   auto Socket = network_platform().socket_from_hosthandle(SocketFD);

   log.trace("Socket: %" PF64, (MAXINT)SocketFD);

   auto write_callback = std::is_same<T, extNetSocket>::value ?
      ssl_handshake_write_netsocket : ssl_handshake_write_clientsocket;

   ssl_clear_error_queue();
   if (auto result = SSL_do_handshake(Self->TLS.Handle); result IS 1) { // Handshake successful, connection established
      network_platform().remove_read(Socket);
      Self->TLS.HandshakeStatus = SHS::NIL;
      ssl_resume_write_queue(Socket.hosthandle(), Self);
   }
   else switch (auto ssl_error = SSL_get_error(Self->TLS.Handle, result)) {
      case SSL_ERROR_WANT_READ:
         // Continue monitoring for read readiness - no action needed
         break;

      case SSL_ERROR_WANT_WRITE:
         network_platform().remove_read(Socket);
         Self->TLS.HandshakeStatus = SHS::WRITE;
         network_platform().register_write(Socket, write_callback, Self);
         break;

      default:
         log.warning("SSL_do_handshake failed: %s", ssl_error_name(ssl_error));
         if (ssl_error IS SSL_ERROR_SSL) ssl_log_error_queue(log, "SSL_do_handshake");
         Self->TLS.HandshakeStatus = SHS::NIL;
   }
}
