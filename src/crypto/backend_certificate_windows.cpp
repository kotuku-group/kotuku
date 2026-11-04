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

#include <array>
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

SYSTEMTIME certificate_time(int Days) {
   FILETIME time;
   GetSystemTimeAsFileTime(&time);
   ULARGE_INTEGER ticks;
   ticks.LowPart = time.dwLowDateTime;
   ticks.HighPart = time.dwHighDateTime;
   ticks.QuadPart += int64_t(Days) * 86400 * 10000000;
   time = { ticks.LowPart, ticks.HighPart };
   SYSTEMTIME result{};
   FileTimeToSystemTime(&time, &result);
   return result;
}

}

ERR generate_p256_certificate(std::string_view CommonName, std::string &CertificatePEM,
   std::string &PrivateKeyPEM, std::string &Fingerprint) {
   auto name_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, CommonName.data(),
      int(CommonName.size()), nullptr, 0);
   if (name_size <= 0) return ERR::Failed;
   std::wstring name(size_t(name_size), L'\0');
   if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, CommonName.data(), int(CommonName.size()),
       name.data(), name_size) != name_size) return ERR::Failed;
   CERT_RDN_ATTR attribute{};
   attribute.pszObjId = (LPSTR)szOID_COMMON_NAME;
   attribute.dwValueType = CERT_RDN_UNICODE_STRING;
   attribute.Value = { DWORD(name.size() * sizeof(wchar_t)), (BYTE *)name.data() };
   CERT_RDN rdn = { 1, &attribute };
   CERT_NAME_INFO name_info = { 1, &rdn };
   DWORD subject_size = 0;
   if (not CryptEncodeObjectEx(X509_ASN_ENCODING, X509_UNICODE_NAME, &name_info, 0,
       nullptr, nullptr, &subject_size)) return ERR::Failed;
   std::vector<uint8_t> subject_bytes(subject_size);
   if (not CryptEncodeObjectEx(X509_ASN_ENCODING, X509_UNICODE_NAME, &name_info, 0,
       nullptr, subject_bytes.data(), &subject_size)) return ERR::Failed;
   CERT_NAME_BLOB subject = { subject_size, subject_bytes.data() };

   NCRYPT_PROV_HANDLE provider = 0;
   NCRYPT_KEY_HANDLE key = 0;
   auto cleanup = kt::Defer([&]() {
      if (key) NCryptFreeObject(key);
      if (provider) NCryptFreeObject(provider);
   });
   DWORD export_policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
   if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS or
       NCryptCreatePersistedKey(provider, &key, NCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0,
          NCRYPT_SILENT_FLAG) != ERROR_SUCCESS or
       NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&export_policy,
          sizeof(export_policy), 0) != ERROR_SUCCESS or
       NCryptFinalizeKey(key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) return ERR::Failed;
   CRYPT_ALGORITHM_IDENTIFIER signature{};
   signature.pszObjId = (LPSTR)szOID_ECDSA_SHA256;
   auto start = certificate_time(-1), end = certificate_time(30);
   auto certificate = CertCreateSelfSignCertificate(key, &subject, CERT_CREATE_SELFSIGN_NO_KEY_INFO,
      nullptr, &signature, &start, &end, nullptr);
   if (not certificate) return ERR::Failed;
   auto free_certificate = kt::Defer([&]() { CertFreeCertificateContext(certificate); });
   DWORD private_size = 0;
   if (NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, nullptr, 0,
       &private_size, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) return ERR::Failed;
   std::vector<uint8_t> private_bytes(private_size);
   auto wipe_private = kt::Defer([&]() { SecureZeroMemory(private_bytes.data(), private_bytes.size()); });
   if (NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, private_bytes.data(), private_size,
       &private_size, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) return ERR::Failed;
   std::array<uint8_t, 32> digest{};
   DWORD digest_size = DWORD(digest.size());
   if (not CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, certificate->pbCertEncoded,
       certificate->cbCertEncoded, digest.data(), &digest_size) or digest_size != digest.size()) return ERR::Failed;
   std::string certificate_pem, private_pem, fingerprint;
   auto wipe_pem = kt::Defer([&]() { SecureZeroMemory(private_pem.data(), private_pem.size()); });
   if (not pem({ certificate->pbCertEncoded, certificate->cbCertEncoded }, "CERTIFICATE", certificate_pem) or
       not pem({ private_bytes.data(), private_size }, "PRIVATE KEY", private_pem)) return ERR::Failed;
   static constexpr char HEX[] = "0123456789ABCDEF";
   for (size_t i = 0; i < digest.size(); ++i) {
      if (i) fingerprint += ':';
      fingerprint += HEX[digest[i] >> 4];
      fingerprint += HEX[digest[i] & 15];
   }
   CertificatePEM = std::move(certificate_pem);
   PrivateKeyPEM = std::move(private_pem);
   Fingerprint = std::move(fingerprint);
   return ERR::Okay;
}

}
