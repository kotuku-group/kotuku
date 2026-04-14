#pragma once

#include <kotuku/system/errors.h>
#include <optional>

#ifndef _WINSOCK2API_
typedef void * SOCKET;
#endif

typedef struct ssl_context * SSL_HANDLE;

// Error codes
typedef enum {
    SSL_OK = 0,
    SSL_ERROR_ARGS = -1,
    SSL_ERROR_FAILED = -2,
    SSL_ERROR_MEMORY = -3,
    SSL_ERROR_WOULD_BLOCK = -4,
    SSL_ERROR_DISCONNECTED = -5,
    SSL_NEED_DATA = -6, // Indicates more data needed to complete operation
    SSL_ERROR_CONNECTING = -7
} SSL_ERROR_CODE;

ERR ssl_wrapper_init();
void ssl_cleanup();
void ssl_enable_logging();
SSL_HANDLE ssl_create_context(bool ValidateCredentials = true, bool ServerMode = false);
void ssl_shutdown(SSL_HANDLE);
void ssl_free_context(SSL_HANDLE);
SSL_ERROR_CODE ssl_connect(SSL_HANDLE, void *, const std::string &);
SSL_ERROR_CODE ssl_continue_handshake(SSL_HANDLE, const void *, int, int &);
SSL_ERROR_CODE ssl_accept(SSL_HANDLE, const void *, int);
void ssl_set_socket(SSL_HANDLE, void* socket_handle);
bool ssl_has_decrypted_data(SSL_HANDLE);
bool ssl_has_encrypted_data(SSL_HANDLE);
int ssl_read_internal(SSL_HANDLE, void* buffer, int buffer_size, int &);
SSL_ERROR_CODE ssl_read(SSL_HANDLE, void* buffer, int buffer_size, int* bytes_read);
SSL_ERROR_CODE ssl_write(SSL_HANDLE, const void* buffer, size_t buffer_size, size_t* bytes_sent);
uint32_t ssl_last_win32_error(SSL_HANDLE);
int ssl_last_security_status(SSL_HANDLE);
bool ssl_get_verify_result(SSL_HANDLE);
SSL_ERROR_CODE ssl_load_server_certificate(SSL_HANDLE, const std::string &, std::optional<const std::string> &, std::optional<const std::string> &);
void ssl_set_server_certificate(SSL_HANDLE Server, SSL_HANDLE Client);
bool load_pem_certificate(SSL_HANDLE SSL, const std::string &Path);
bool load_pkcs12_certificate(SSL_HANDLE SSL, const std::string &Path);

// Connection information structure
struct SSL_CONNECTION_INFO {
    const char* protocol_version;     // e.g., "TLS 1.3", "TLS 1.2"
    const char* cipher_suite;         // e.g., "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384"
    const char* key_exchange;         // e.g., "ECDHE"
    const char* signature_algorithm;  // e.g., "RSA"
    const char* encryption_algorithm; // e.g., "AES_256_GCM"
    int key_size_bits;               // e.g., 256
    bool certificate_chain_valid;    // true if cert chain validation passed
    int certificate_chain_length;    // number of certificates in chain
};

// Connection information queries
bool ssl_get_connection_info(SSL_HANDLE, SSL_CONNECTION_INFO* info);
const char* ssl_get_protocol_version(SSL_HANDLE);
const char* ssl_get_cipher_suite(SSL_HANDLE);
int ssl_get_key_size_bits(SSL_HANDLE);

// SSL Debug callback function type
typedef void (*SSL_DEBUG_CALLBACK)(const char* message, int level);

// Debug levels
enum SSL_DEBUG_LEVEL {
   SSL_DEBUG_ERROR = 0,
   SSL_DEBUG_WARNING = 1,
   SSL_DEBUG_INFO = 2,
   SSL_DEBUG_TRACE = 3
};

// Debug functions
void ssl_set_debug_callback(SSL_DEBUG_CALLBACK callback);
void ssl_debug_handshake(SSL_HANDLE, const char* operation);
