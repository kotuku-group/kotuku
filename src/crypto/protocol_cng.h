// CNG-specific protocol primitives.  Only Windows backend code may include this header.  Windows types are
// forward-declared so that callers do not need <windows.h>.
#pragma once

#include "protocol_primitives.h"

#ifndef CRYPTO_PROTOCOL_CNG
#error "protocol_cng.h requires CRYPTO_PROTOCOL_CNG."
#endif

#include <array>
#include <span>

struct _CERT_CONTEXT;

namespace crypto_protocol {
inline namespace CRYPTO_PROTOCOL_ABI {

// A self-signed certificate and its private key.  Both are owned and released on destruction.  The key is attached to
// the certificate context through CERT_NCRYPT_KEY_HANDLE_PROP_ID without transferring ownership.

struct CngCertificate {
   const _CERT_CONTEXT *context = nullptr; // PCCERT_CONTEXT
   uintptr_t key = 0;                      // NCRYPT_KEY_HANDLE
   bool persisted = false;                 // Delete a named key as well as releasing its handle
   std::array<uint8_t, 32> fingerprint{};  // SHA-256 of the DER encoding

   CngCertificate() = default;
   ~CngCertificate() { reset(); }
   CngCertificate(const CngCertificate &) = delete;
   CngCertificate & operator=(const CngCertificate &) = delete;

   std::span<const uint8_t> der() const noexcept;
   void reset() noexcept;
};

// Generates an ephemeral (unnamed) P-256 key in the Microsoft Software Key Storage Provider and a self-signed
// ECDSA-SHA256 certificate valid from one day ago until 30 days from now.  Exportable allows the private key to be
// exported in plaintext, e.g. as PKCS #8.  Persisted creates a uniquely named key usable by Schannel, deleted when
// Result is reset.  The first persisted generation also removes keys left by exited Kotuku processes.  Result is
// reset on failure.

bool generate_p256_certificate_cng(std::string_view CommonName, bool Exportable, CngCertificate &Result,
   bool Persisted = false);

} // inline namespace
} // namespace crypto_protocol
