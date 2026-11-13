#define OBJECTID WindowsObjectID
#include <windows.h>
#undef OBJECTID
#undef __OBJECTID_DEFINED
#undef RELATIVE
#undef DELETE
#undef FD_READ
#undef FD_WRITE
#include <bcrypt.h>
#include <ncrypt.h>
#include <wincrypt.h>

#include "crypto_internal.h"
#include "protocol_cng.h"

#include <string>
#include <vector>

namespace crypto_backend {
namespace {

bool pem(std::span<const uint8_t> DER, const char *Label, std::string &Output) {
   DWORD size = 0;
   if (not CryptBinaryToStringA(DER.data(), DWORD(DER.size()), CRYPT_STRING_BASE64, nullptr, &size)) return false;
   std::string encoded(size, '\0');
   auto wipe = kt::Defer([&]() { SecureZeroMemory(encoded.data(), encoded.size()); });
   if (not CryptBinaryToStringA(DER.data(), DWORD(DER.size()), CRYPT_STRING_BASE64, encoded.data(), &size)) {
      return false;
   }
   Output = std::string("-----BEGIN ") + Label + "-----\n";
   for (char ch : encoded) if (ch != '\r' and ch != '\0') Output += ch;
   Output += std::string("-----END ") + Label + "-----\n";
   return true;
}

}

// The certificate and key come from the protocol-level CNG generator in protocol_cng.cpp.  The key is created
// exportable so that it can be returned as PKCS #8.

ERR generate_p256_certificate(std::string_view CommonName, std::string &CertificatePEM,
   std::string &PrivateKeyPEM, std::string &Fingerprint) {
   crypto_protocol::CngCertificate certificate;
   if (not crypto_protocol::generate_p256_certificate_cng(CommonName, true, certificate)) return ERR::Failed;
   auto key = NCRYPT_KEY_HANDLE(certificate.key);
   DWORD private_size = 0;
   if (NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, nullptr, 0,
       &private_size, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) return ERR::Failed;
   std::vector<uint8_t> private_bytes(private_size);
   auto wipe_private = kt::Defer([&]() { SecureZeroMemory(private_bytes.data(), private_bytes.size()); });
   if (NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, private_bytes.data(), private_size,
       &private_size, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) return ERR::Failed;
   std::string certificate_pem, private_pem, fingerprint;
   auto wipe_pem = kt::Defer([&]() { SecureZeroMemory(private_pem.data(), private_pem.size()); });
   if (not pem(certificate.der(), "CERTIFICATE", certificate_pem) or
       not pem({ private_bytes.data(), private_size }, "PRIVATE KEY", private_pem)) return ERR::Failed;
   static constexpr char HEX[] = "0123456789ABCDEF";
   for (size_t i = 0; i < certificate.fingerprint.size(); ++i) {
      if (i) fingerprint += ':';
      fingerprint += HEX[certificate.fingerprint[i] >> 4];
      fingerprint += HEX[certificate.fingerprint[i] & 15];
   }
   CertificatePEM = std::move(certificate_pem);
   PrivateKeyPEM = std::move(private_pem);
   Fingerprint = std::move(fingerprint);
   return ERR::Okay;
}

}
