// OpenSSL-specific protocol primitives.  Only OpenSSL backend code may include this header.
#pragma once

#include "protocol_primitives.h"

#ifndef CRYPTO_PROTOCOL_OPENSSL
#error "protocol_openssl.h requires CRYPTO_PROTOCOL_OPENSSL."
#endif

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace crypto_protocol {
inline namespace CRYPTO_PROTOCOL_ABI {

// Generates an ephemeral P-256 key and a self-signed ECDSA-SHA256 certificate valid from one day ago until 30 days
// from now.  The caller owns both objects, including on failure.

bool generate_p256_certificate(std::string_view CommonName, EVP_PKEY *&Key, X509 *&Certificate);

} // inline namespace
} // namespace crypto_protocol
