/*********************************************************************************************************************

Windows SSL Wrapper Implementation
Pure Windows implementation that avoids all Kotuku headers to prevent conflicts.

*********************************************************************************************************************/

#ifdef _WIN32

// Define required constants to avoid header conflicts
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#define NOMINMAX

#include <winsock2.h>
#include <windows.h>
#include <schannel.h>
#include <sspi.h>
#include <security.h>
#include <wincrypt.h>
#include <prsht.h>
#include <cryptuiapi.h>
#include <cstring>
#include <vector>
#include <string>
#include <sstream>
#include <array>
#include <algorithm>
#include <span>

#include "ssl_wrapper.h"

static void ssl_debug_log(int level, const char* format, ...);
extern "C" void ssl_debug_to_kotuku_log(const char* message, int level);

// Forward declarations
static void cache_connection_info(ssl_context* SSL);

// Define provider names if not available
#ifndef MS_ENH_RSA_AES_PROV
#define MS_ENH_RSA_AES_PROV "Microsoft Enhanced RSA and AES Cryptographic Provider"
#endif

// Buffer size for SSL operations - optimized for SSL record sizes
constexpr size_t SSL_IO_BUFFER_SIZE = 0x8000;  // 32KB - 2x max SSL record size (16KB)
constexpr size_t SSL_INITIAL_BUFFER_SIZE = 0x4000;  // 16KB initial size
constexpr size_t SSL_MAX_RECORD_SIZE = 0x4000;  // 16KB max SSL record size
constexpr size_t SSL_RECORD_HEADER_SIZE = 5;  // SSL record header size
constexpr size_t MIN_SSL_RECORD_SIZE = 32;  // Conservative minimum for a valid SSL record
constexpr int MAX_INVALID_TOKEN_RETRIES = 3;  // Maximum retries for invalid token errors

class SSLBuffer {
private:
   std::vector<unsigned char> data_;
   size_t used_ = 0;

public:
   explicit SSLBuffer(size_t initial_size = SSL_INITIAL_BUFFER_SIZE)
      : data_(initial_size) {}

   std::span<unsigned char> available_space() {
      return std::span<unsigned char>(data_.data() + used_, data_.size() - used_);
   }

   std::span<const unsigned char> used_data() const {
      return std::span<const unsigned char>(data_.data(), used_);
   }

   std::span<unsigned char> used_data_mutable() {
      return std::span<unsigned char>(data_.data(), used_);
   }

   void advance_used(size_t bytes) {
      if (used_ + bytes <= data_.size()) used_ += bytes;
   }

   void ensure_capacity(size_t min_size) {
      if (data_.size() < min_size) {
         data_.resize(std::min(min_size, SSL_IO_BUFFER_SIZE));
      }
   }

   void reset() { used_ = 0; }

   void clear() {
      used_ = 0;
      data_.clear();
      data_.resize(SSL_INITIAL_BUFFER_SIZE);
   }

   bool append(std::span<const unsigned char> data) {
      size_t total_needed = used_ + data.size();
      if (total_needed > SSL_IO_BUFFER_SIZE) return false; // Would exceed maximum buffer size
      if (data_.size() < total_needed) data_.resize(total_needed);
      std::memcpy(data_.data() + used_, data.data(), data.size());
      used_ += data.size();
      return true;
   }

   void consume_front(size_t bytes) {
      if (bytes >= used_) reset();
      else {
         std::memmove(data_.data(), data_.data() + bytes, used_ - bytes);
         used_ -= bytes;
      }
   }

   void compact(size_t bytes_consumed) {
      if (bytes_consumed >= used_) reset();
      else {
         size_t remaining = used_ - bytes_consumed;
         std::memmove(data_.data(), data_.data() + bytes_consumed, remaining);
         used_ = remaining;
      }
   }

   unsigned char * data() { return data_.data(); }
   const unsigned char * data() const { return data_.data(); }
   size_t size() const { return used_; }
   size_t capacity() const { return data_.size(); }
   bool empty() const { return used_ == 0; }
   size_t available() const { return data_.size() - used_; }

   void reserve(size_t new_cap) {
      if (new_cap <= SSL_IO_BUFFER_SIZE and new_cap > data_.size()) {
         data_.reserve(new_cap);
      }
   }

   void resize(size_t new_size) {
      if (new_size <= SSL_IO_BUFFER_SIZE) {
         data_.resize(new_size);
         if (used_ > new_size) used_ = new_size;
      }
   }
};

struct ssl_context {
   CredHandle credentials;
   CtxtHandle context;
   SecPkgContext_StreamSizes stream_sizes;
   SOCKET socket_handle;
   SSLBuffer io_buffer;
   SSLBuffer recv_buffer;                      // Persistent buffer for incomplete SSL messages
   SSLBuffer send_buffer;                      // Buffer for SSL encryption
   SSLBuffer decrypted_buffer;                 // Buffer for leftover decrypted data
   size_t decrypted_buffer_offset;             // Bytes already returned to user
   SECURITY_STATUS last_security_status;
   DWORD last_win32_error;
   std::string hostname;
   bool validate_credentials;
   bool credentials_acquired;
   bool context_initialised;
   bool is_server_mode;                        // True for server-side SSL, false for client-side
   PCCERT_CONTEXT server_certificate;          // Server certificate for server-side SSL
   PCCERT_CONTEXT peer_certificate;            // Peer certificate for validation
   PCCERT_CHAIN_CONTEXT certificate_chain;     // Certificate chain context for validation

   // Connection information cache
   std::string protocol_version_str;
   std::string cipher_suite_str;
   std::string key_exchange_str;
   std::string signature_algorithm_str;
   std::string encryption_algorithm_str;
   int key_size_bits;
   bool certificate_chain_valid;
   int certificate_chain_length;
   bool connection_info_cached;

   ssl_context()
      : socket_handle(INVALID_SOCKET)
      , io_buffer(SSL_INITIAL_BUFFER_SIZE)
      , recv_buffer(SSL_INITIAL_BUFFER_SIZE)
      , send_buffer(SSL_INITIAL_BUFFER_SIZE)
      , decrypted_buffer(SSL_MAX_RECORD_SIZE)
      , decrypted_buffer_offset(0)
      , last_security_status(SEC_E_OK)
      , last_win32_error(0)
      , validate_credentials(true)
      , credentials_acquired(false)
      , context_initialised(false)
      , is_server_mode(false)
      , server_certificate(nullptr)
      , peer_certificate(nullptr)
      , certificate_chain(nullptr)
      , key_size_bits(0)
      , certificate_chain_valid(false)
      , certificate_chain_length(0)
      , connection_info_cached(false)
   {
      io_buffer.reserve(SSL_IO_BUFFER_SIZE);
      recv_buffer.reserve(SSL_IO_BUFFER_SIZE);
      send_buffer.reserve(SSL_IO_BUFFER_SIZE);
      decrypted_buffer.reserve(SSL_MAX_RECORD_SIZE);
   }

   ~ssl_context() {
      if (context_initialised) {
         DeleteSecurityContext(&context);
         context_initialised = false;
      }
      if (credentials_acquired) {
         FreeCredentialsHandle(&credentials);
         credentials_acquired = false;
      }
      if (server_certificate) {
         CertFreeCertificateContext(server_certificate);
         server_certificate = nullptr;
      }
      if (peer_certificate) {
         CertFreeCertificateContext(peer_certificate);
         peer_certificate = nullptr;
      }
      if (certificate_chain) {
         CertFreeCertificateChain(certificate_chain);
         certificate_chain = nullptr;
      }
   }

   SSL_ERROR_CODE process_recv_error(int Result, std::string Process)
   {
      if (!Result) {
         return SSL_ERROR_DISCONNECTED;
      }
      else if (Result == -1) {
         last_win32_error = WSAGetLastError();
         if (last_win32_error == WSAEWOULDBLOCK) return SSL_ERROR_WOULD_BLOCK;
         else return SSL_ERROR_FAILED;
      }

      return SSL_OK;
   }
};

//********************************************************************************************************************

static bool glSSLInitialised = false;
static bool glLoggingEnabled = false;

#include "ssl_certs.cpp"

//********************************************************************************************************************

static void set_error_status(ssl_context* Ctx, SECURITY_STATUS Status)
{
   Ctx->last_security_status = Status;
   Ctx->last_win32_error = GetLastError();
}

//********************************************************************************************************************

static void ssl_debug_log(int level, const char* format, ...)
{
   if (!glLoggingEnabled) return;

   char buffer[1024];
   va_list args;
   va_start(args, format);
   vsnprintf(buffer, sizeof(buffer), format, args);
   va_end(args);

   ssl_debug_to_kotuku_log(buffer, level);
}

//********************************************************************************************************************

static void debug_ssl_handshake_state(ssl_context* SSL, const char* operation)
{
   if (!glLoggingEnabled or !SSL->context_initialised) return;

   SecPkgContext_ConnectionInfo conn_info;
   QueryContextAttributes(&SSL->context, SECPKG_ATTR_CONNECTION_INFO, &conn_info);

   SecPkgContext_CipherInfo cipher_info;
   if (QueryContextAttributes(&SSL->context, SECPKG_ATTR_CIPHER_INFO, &cipher_info) == SEC_E_OK) {
      ssl_debug_log(SSL_DEBUG_INFO, "SSL Debug [%s] - Cipher Suite: %S", operation, cipher_info.szCipherSuite);
   }
   SecPkgContext_KeyInfo key_info;
   if (QueryContextAttributes(&SSL->context, SECPKG_ATTR_KEY_INFO, &key_info) == SEC_E_OK) {
      ssl_debug_log(SSL_DEBUG_INFO, "SSL Debug [%s] - Signature: %S, Encryption: %S",
                   operation, key_info.sSignatureAlgorithmName, key_info.sEncryptAlgorithmName);
   }
}

//********************************************************************************************************************

void ssl_cleanup(void)
{
   if (!glSSLInitialised) return;
   glSSLInitialised = false;
}

//********************************************************************************************************************

SSL_HANDLE ssl_create_context(bool ValidateCredentials, bool ServerMode)
{
   if (!glSSLInitialised) {
      // The certificate store would be needed if you want to:
      // 1. Client Certificate Authentication - Select client certificates for mutual TLS
      // 2. Custom Certificate Validation - Manual certificate chain validation
      // 3. Certificate Enumeration - Browse available certificates
      // 4. Certificate Installation - Add new certificates programmatically

      //g_cert_store = CertOpenSystemStore(0, "MY");
      //if (!g_cert_store) return ERR::Failed;

      glSSLInitialised = true;
   }

   ssl_context *ctx = new (std::nothrow) ssl_context;
   if (!ctx) return nullptr;

   ctx->validate_credentials = ValidateCredentials;
   ctx->is_server_mode = ServerMode;
   return ctx;
}

//********************************************************************************************************************

void ssl_shutdown(SSL_HANDLE SSL)
{
   if ((!SSL) or (!SSL->context_initialised)) return;

   // Step 1: Apply shutdown control token
   DWORD shutdown_type = SCHANNEL_SHUTDOWN;
   SecBuffer shutdown_buf;
   shutdown_buf.cbBuffer = sizeof(shutdown_type);
   shutdown_buf.BufferType = SECBUFFER_TOKEN;
   shutdown_buf.pvBuffer = &shutdown_type;

   SecBufferDesc shutdown_desc;
   shutdown_desc.cBuffers = 1;
   shutdown_desc.pBuffers = &shutdown_buf;
   shutdown_desc.ulVersion = SECBUFFER_VERSION;

   auto status = ApplyControlToken(&SSL->context, &shutdown_desc);
   if (status != SEC_E_OK) return;

   SecBuffer out_buffer;
   out_buffer.pvBuffer = nullptr;
   out_buffer.cbBuffer = 0;
   out_buffer.BufferType = SECBUFFER_TOKEN;

   SecBufferDesc out_desc;
   out_desc.cBuffers = 1;
   out_desc.pBuffers = &out_buffer;
   out_desc.ulVersion = SECBUFFER_VERSION;

   DWORD ctx_attrs = 0;
   TimeStamp expiry;
   status = InitializeSecurityContext(
      &SSL->credentials, &SSL->context, nullptr,
      ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY,
      0, SECURITY_NATIVE_DREP, &shutdown_desc, 0, nullptr, &out_desc, &ctx_attrs, &expiry);

   if (out_buffer.pvBuffer and out_buffer.cbBuffer > 0) {
      send(SSL->socket_handle, (const char*)out_buffer.pvBuffer, out_buffer.cbBuffer, 0);
      FreeContextBuffer(out_buffer.pvBuffer);
   }
}

void ssl_free_context(SSL_HANDLE SSL)
{
   if (SSL) {
      ssl_shutdown(SSL);
      delete SSL;
   }
}

//********************************************************************************************************************

uint32_t ssl_last_win32_error(SSL_HANDLE SSL)
{
   return ((ssl_context*)SSL)->last_win32_error;
}

//********************************************************************************************************************
// Debug handshake public function

void ssl_debug_handshake(SSL_HANDLE SSL, const char* operation)
{
   debug_ssl_handshake_state(SSL, operation);
}

//********************************************************************************************************************
// Set socket handle for server-side SSL contexts

void ssl_set_socket(SSL_HANDLE SSL, void* socket_handle)
{
   if (!socket_handle) return;
   SSL->socket_handle = (SOCKET)(size_t)socket_handle;
}

//********************************************************************************************************************
// Check if SSL context has decrypted application data ready

bool ssl_has_decrypted_data(SSL_HANDLE SSL)
{
   return SSL->decrypted_buffer.size() > SSL->decrypted_buffer_offset;
}

//********************************************************************************************************************
// Check if SSL context has encrypted data ready for decryption

bool ssl_has_encrypted_data(SSL_HANDLE SSL)
{
   return !SSL->recv_buffer.empty();
}

//********************************************************************************************************************
// Get last security status

int ssl_last_security_status(SSL_HANDLE SSL)
{
   return int(((ssl_context*)SSL)->last_security_status);
}

//********************************************************************************************************************

void ssl_enable_logging()
{
   glLoggingEnabled = true;
}

//********************************************************************************************************************

static bool validate_certificate_chain(ssl_context* SSL)
{
   if (!SSL->peer_certificate) return false;

   CERT_CHAIN_PARA chain_para = {};
   chain_para.cbSize = sizeof(chain_para);

   // Build the certificate chain
   BOOL result = CertGetCertificateChain(
      nullptr,                           // Use default chain engine
      SSL->peer_certificate,             // End certificate
      nullptr,                           // Use current system time
      SSL->peer_certificate->hCertStore, // Additional store
      &chain_para,                       // Chain building parameters
      CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY, // Check revocation from cache only
      nullptr,                           // Reserved
      &SSL->certificate_chain            // Chain context output
   );

   if (!result or !SSL->certificate_chain) return false;

   PCERT_SIMPLE_CHAIN simple_chain = SSL->certificate_chain->rgpChain[0];
   SSL->certificate_chain_length = int(simple_chain->cElement);

   DWORD chain_error_status = SSL->certificate_chain->TrustStatus.dwErrorStatus;
   bool chain_valid = (chain_error_status == CERT_TRUST_NO_ERROR);

   SSL->certificate_chain_valid = chain_valid;
   return chain_valid;
}

//********************************************************************************************************************

static void cache_connection_info(ssl_context* SSL)
{
   if (SSL->connection_info_cached or !SSL->context_initialised) return;

   SecPkgContext_ConnectionInfo conn_info = {};
   if (QueryContextAttributes(&SSL->context, SECPKG_ATTR_CONNECTION_INFO, &conn_info) == SEC_E_OK) {
      SSL->key_size_bits = int(conn_info.dwCipherStrength);
      switch (conn_info.dwProtocol) {
         case SP_PROT_TLS1_3_CLIENT:
         case SP_PROT_TLS1_3_SERVER: SSL->protocol_version_str = "TLS 1.3"; break;
         case SP_PROT_TLS1_2_CLIENT:
         case SP_PROT_TLS1_2_SERVER: SSL->protocol_version_str = "TLS 1.2"; break;
         case SP_PROT_TLS1_1_CLIENT:
         case SP_PROT_TLS1_1_SERVER: SSL->protocol_version_str = "TLS 1.1"; break;
         case SP_PROT_TLS1_CLIENT:
         case SP_PROT_TLS1_SERVER: SSL->protocol_version_str = "TLS 1.0"; break;
         default: SSL->protocol_version_str = "Unknown"; break;
      }
   }

   SecPkgContext_CipherInfo cipher_info = {};
   if (QueryContextAttributes(&SSL->context, SECPKG_ATTR_CIPHER_INFO, &cipher_info) == SEC_E_OK) {
      size_t converted = 0;
      char cipher_buf[256];
      wcstombs_s(&converted, cipher_buf, sizeof(cipher_buf), cipher_info.szCipherSuite, _TRUNCATE);
      SSL->cipher_suite_str = cipher_buf;
   }

   SecPkgContext_KeyInfo key_info = {};
   if (QueryContextAttributes(&SSL->context, SECPKG_ATTR_KEY_INFO, &key_info) == SEC_E_OK) {
      size_t converted = 0;
      char sig_buf[128], enc_buf[128];

      if (key_info.sSignatureAlgorithmName) {
         wcstombs_s(&converted, sig_buf, sizeof(sig_buf), (const wchar_t*)key_info.sSignatureAlgorithmName, _TRUNCATE);
         SSL->signature_algorithm_str = sig_buf;
      }

      if (key_info.sEncryptAlgorithmName) {
         wcstombs_s(&converted, enc_buf, sizeof(enc_buf), (const wchar_t*)key_info.sEncryptAlgorithmName, _TRUNCATE);
         SSL->encryption_algorithm_str = enc_buf;
      }
   }

   if (QueryContextAttributes(&SSL->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, (PVOID)&SSL->peer_certificate) == SEC_E_OK) {
      if (SSL->peer_certificate and SSL->validate_credentials) {
         validate_certificate_chain(SSL);
      }
      else {
         SSL->certificate_chain_valid = !SSL->validate_credentials;
         SSL->certificate_chain_length = 1;
      }
   }

   SSL->connection_info_cached = true;
}

//********************************************************************************************************************

bool ssl_get_connection_info(SSL_HANDLE SSL, SSL_CONNECTION_INFO *Info)
{
   if (!SSL or !Info) return false;

   cache_connection_info(SSL);

   Info->protocol_version         = SSL->protocol_version_str.c_str();
   Info->cipher_suite             = SSL->cipher_suite_str.c_str();
   Info->key_exchange             = SSL->key_exchange_str.c_str();
   Info->signature_algorithm      = SSL->signature_algorithm_str.c_str();
   Info->encryption_algorithm     = SSL->encryption_algorithm_str.c_str();
   Info->key_size_bits            = SSL->key_size_bits;
   Info->certificate_chain_valid  = SSL->certificate_chain_valid;
   Info->certificate_chain_length = SSL->certificate_chain_length;
   return true;
}

const char* ssl_get_protocol_version(SSL_HANDLE SSL)
{
   cache_connection_info(SSL);
   return SSL->protocol_version_str.c_str();
}

const char* ssl_get_cipher_suite(SSL_HANDLE SSL)
{
   cache_connection_info(SSL);
   return SSL->cipher_suite_str.c_str();
}

int ssl_get_key_size_bits(SSL_HANDLE SSL)
{
   cache_connection_info(SSL);
   return SSL->key_size_bits;
}

//********************************************************************************************************************
// Load server certificate from file

// TODO: Key path and password handling for PKCS#12 files

SSL_ERROR_CODE ssl_load_server_certificate(SSL_HANDLE SSL, const std::string &CertPath, std::optional<const std::string> &KeyPath, std::optional<const std::string> &Password)
{
   if (!SSL->is_server_mode) return SSL_ERROR_FAILED;

   std::string ext = CertPath.substr(CertPath.find_last_of(".") + 1);
   std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

   bool success = false;
   if ((ext == "p12") or (ext == "pfx")) { // Load PKCS#12 certificate
      success = load_pkcs12_certificate(SSL, CertPath);
   }
   else if ((ext == "pem") or (ext == "crt") or (ext == "cert")) { // Load PEM certificate
      success = load_pem_certificate(SSL, CertPath);
   }

   return success ? SSL_OK : SSL_ERROR_FAILED;
}

//********************************************************************************************************************

void ssl_set_server_certificate(SSL_HANDLE Server, SSL_HANDLE Client)
{
   Client->server_certificate = Server->server_certificate;
}

#include "ssl_handshake.cpp"
#include "ssl_io.cpp"
#include "ssl_connect.cpp"

#endif // _WIN32