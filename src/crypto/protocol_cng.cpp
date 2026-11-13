// CNG (BCrypt/NCrypt) implementations of the protocol primitives.  See protocol_primitives.h.

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

#include "protocol_cng.h"

#include <kotuku/config.h>

#include <cstdio>
#include <cwchar>
#include <limits>
#include <mutex>
#include <string>

namespace crypto_protocol {
namespace {

// Computes a digest, or an HMAC when Key is non-null.  The algorithm provider is opened per call; CNG caches
// providers internally and the protocol layers hash at signalling rates, not per media packet.

bool bcrypt_digest(LPCWSTR Algorithm, const std::span<const uint8_t> *Key, std::span<const uint8_t> Data,
   std::span<uint8_t> Result)
{
   constexpr size_t LIMIT = std::numeric_limits<ULONG>::max();
   if (Data.size() > LIMIT or (Key and Key->size() > LIMIT)) return false;

   BCRYPT_ALG_HANDLE algorithm = nullptr;
   BCRYPT_HASH_HANDLE hash = nullptr;
   ULONG flags = Key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
   if (not BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, Algorithm, nullptr, flags))) return false;

   // An empty HMAC key is valid (RFC 2104 pads it to the block size); CNG only needs a non-null pointer.

   static const UCHAR EMPTY_KEY = 0;
   auto key_data = Key ? (Key->empty() ? (PUCHAR)&EMPTY_KEY : (PUCHAR)Key->data()) : nullptr;
   auto key_size = Key ? ULONG(Key->size()) : 0;
   bool success = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, key_data, key_size, 0)) and
      BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)Data.data(), ULONG(Data.size()), 0)) and
      BCRYPT_SUCCESS(BCryptFinishHash(hash, Result.data(), ULONG(Result.size()), 0));

   if (hash) BCryptDestroyHash(hash);
   BCryptCloseAlgorithmProvider(algorithm, 0);
   if (not success) SecureZeroMemory(Result.data(), Result.size());
   return success;
}

SYSTEMTIME certificate_time(int Days)
{
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

// The Common Name is encoded as a single UTF-8 converted CN RDN rather than through CertStrToName(), so that commas,
// quotes and other X.500 string syntax in the name are not interpreted.

bool encode_subject(std::string_view CommonName, std::vector<uint8_t> &Output)
{
   if (CommonName.size() > size_t(std::numeric_limits<int>::max())) return false;
   auto name_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, CommonName.data(),
      int(CommonName.size()), nullptr, 0);
   if (name_size <= 0) return false;
   std::wstring name(size_t(name_size), L'\0');
   if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, CommonName.data(), int(CommonName.size()),
       name.data(), name_size) != name_size) return false;

   CERT_RDN_ATTR attribute{};
   attribute.pszObjId = (LPSTR)szOID_COMMON_NAME;
   attribute.dwValueType = CERT_RDN_UNICODE_STRING;
   attribute.Value = { DWORD(name.size() * sizeof(wchar_t)), (BYTE *)name.data() };
   CERT_RDN rdn = { 1, &attribute };
   CERT_NAME_INFO name_info = { 1, &rdn };
   DWORD subject_size = 0;
   if (not CryptEncodeObjectEx(X509_ASN_ENCODING, X509_UNICODE_NAME, &name_info, 0,
       nullptr, nullptr, &subject_size)) return false;
   Output.resize(subject_size);
   if (not CryptEncodeObjectEx(X509_ASN_ENCODING, X509_UNICODE_NAME, &name_info, 0,
       nullptr, Output.data(), &subject_size)) return false;
   Output.resize(subject_size);
   return true;
}

uint64_t process_creation_time(HANDLE Process)
{
   FILETIME created, exited, kernel, user;
   if (not GetProcessTimes(Process, &created, &exited, &kernel, &user)) return 0;
   return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// Remove persisted DTLS keys abandoned by exited processes.  Key names include both the PID and the process creation
// time so that a recycled PID cannot keep an abandoned key alive.  Keys belonging to live processes (including other
// Kotuku instances), or to processes that cannot be inspected, are never removed.

void sweep_dtls_keys(NCRYPT_PROV_HANDLE Provider)
{
   static constexpr std::wstring_view PREFIX = L"kotuku-webrtc-";

   void *enumeration = nullptr;
   NCryptKeyName *name = nullptr;
   while (NCryptEnumKeys(Provider, nullptr, &name, &enumeration, NCRYPT_SILENT_FLAG) IS ERROR_SUCCESS) {
      std::wstring_view text(name->pszName);
      unsigned long pid = 0;
      uint64_t created = 0;
      bool recognised = false;

      if (text.starts_with(PREFIX)) {
         wchar_t *end = nullptr;
         auto start = name->pszName + PREFIX.size();
         pid = std::wcstoul(start, &end, 10);
         if (end != start and *end IS '-') {
            start = end + 1;
            created = std::wcstoull(start, &end, 16);
            if (end != start and *end IS '-') {
               std::wstring_view suffix(end + 1);
               recognised = pid and created and suffix.size() IS 32 and
                  suffix.find_first_not_of(L"0123456789abcdef") IS std::wstring_view::npos;
            }
         }
      }

      bool stale = false;
      if (recognised) {
         auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
         if (process) {
            auto actual = process_creation_time(process);
            stale = WaitForSingleObject(process, 0) IS WAIT_OBJECT_0 or (actual and actual != created);
            CloseHandle(process);
         }
         else stale = GetLastError() IS ERROR_INVALID_PARAMETER;
      }

      if (stale) {
         NCRYPT_KEY_HANDLE key = 0;
         if (NCryptOpenKey(Provider, &key, name->pszName, 0, NCRYPT_SILENT_FLAG) IS ERROR_SUCCESS) {
            if (NCryptDeleteKey(key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) NCryptFreeObject(key);
         }
      }

      NCryptFreeBuffer(name);
   }

   if (enumeration) NCryptFreeBuffer(enumeration);
}

}

inline namespace CRYPTO_PROTOCOL_ABI {

bool hmac_sha1(std::span<const uint8_t> Key, std::span<const uint8_t> Data, std::span<uint8_t, 20> Result) {
   return bcrypt_digest(BCRYPT_SHA1_ALGORITHM, &Key, Data, Result);
}

bool hmac_sha256(std::span<const uint8_t> Key, std::span<const uint8_t> Data, std::span<uint8_t, 32> Result) {
   return bcrypt_digest(BCRYPT_SHA256_ALGORITHM, &Key, Data, Result);
}

bool sha256(std::span<const uint8_t> Data, std::span<uint8_t, 32> Result) {
   return bcrypt_digest(BCRYPT_SHA256_ALGORITHM, nullptr, Data, Result);
}

bool long_term_key(std::string_view Username, std::string_view Realm, std::string_view Password,
   std::vector<uint8_t> &Result, bool Sha256) {
   auto text = std::string(Username) + ":" + std::string(Realm) + ":" + std::string(Password);
   Result.resize(Sha256 ? 32 : 16);
   bool success = bcrypt_digest(Sha256 ? BCRYPT_SHA256_ALGORITHM : BCRYPT_MD5_ALGORITHM, nullptr,
      std::span<const uint8_t>((const uint8_t *)text.data(), text.size()), Result);
   SecureZeroMemory(text.data(), text.size());
   if (not success) {
      SecureZeroMemory(Result.data(), Result.size());
      Result.clear();
      return false;
   }
   return true;
}

bool random_bytes(std::span<uint8_t> Output) {
   if (Output.size() > size_t(std::numeric_limits<ULONG>::max())) return false;
   if (Output.empty()) return true;
   return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, Output.data(), ULONG(Output.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

void secure_wipe(void *Data, size_t Size) {
   if (Data and Size) SecureZeroMemory(Data, Size);
}

//********************************************************************************************************************

std::span<const uint8_t> CngCertificate::der() const noexcept {
   if (not context) return {};
   return { context->pbCertEncoded, context->cbCertEncoded };
}

void CngCertificate::reset() noexcept {
   if (context) CertFreeCertificateContext(context);
   if (key) {
      if (not persisted or NCryptDeleteKey(NCRYPT_KEY_HANDLE(key), NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) {
         NCryptFreeObject(NCRYPT_KEY_HANDLE(key));
      }
   }

   context = nullptr;
   key = 0;
   persisted = false;
   fingerprint.fill(0);
}

bool generate_p256_certificate_cng(std::string_view CommonName, bool Exportable, CngCertificate &Result,
   bool Persisted) {
   Result.reset();

   std::vector<uint8_t> subject_bytes;
   if (not encode_subject(CommonName, subject_bytes)) return false;
   CERT_NAME_BLOB subject = { DWORD(subject_bytes.size()), subject_bytes.data() };

   NCRYPT_PROV_HANDLE provider = 0;
   if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return false;

   std::wstring key_name;
   if (Persisted) {
      static std::once_flag sweep_once;
      std::call_once(sweep_once, [provider]() { sweep_dtls_keys(provider); });

      std::array<uint8_t, 16> random;
      auto created = process_creation_time(GetCurrentProcess());
      if (not created or not random_bytes(random)) {
         NCryptFreeObject(provider);
         return false;
      }

      // Key names take the form kotuku-webrtc-<pid>-<creation time>-<random>, as parsed by sweep_dtls_keys().

      wchar_t prefix[80];
      std::swprintf(prefix, std::size(prefix), L"kotuku-webrtc-%lu-%llx-", GetCurrentProcessId(),
         (unsigned long long)created);
      key_name = prefix;

      static constexpr wchar_t HEX[] = L"0123456789abcdef";
      for (auto value : random) {
         key_name += HEX[value >> 4];
         key_name += HEX[value & 15];
      }
   }

   NCRYPT_KEY_HANDLE key = 0;
   auto status = NCryptCreatePersistedKey(provider, &key, NCRYPT_ECDSA_P256_ALGORITHM,
      Persisted ? key_name.c_str() : nullptr, 0, NCRYPT_SILENT_FLAG);
   NCryptFreeObject(provider);
   if (status != ERROR_SUCCESS) return false;

   Result.key = uintptr_t(key);
   Result.persisted = Persisted;

   DWORD export_policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
   if ((Exportable and NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&export_policy,
          sizeof(export_policy), 0) != ERROR_SUCCESS) or
       NCryptFinalizeKey(key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS) {
      Result.reset();
      return false;
   }

   CRYPT_ALGORITHM_IDENTIFIER signature{};
   signature.pszObjId = (LPSTR)szOID_ECDSA_SHA256;
   auto start = certificate_time(-1), end = certificate_time(30);

   CRYPT_KEY_PROV_INFO key_info{};
   key_info.pwszContainerName = key_name.data();
   key_info.pwszProvName = (LPWSTR)MS_KEY_STORAGE_PROVIDER;
   key_info.dwFlags = NCRYPT_SILENT_FLAG;

   auto certificate = CertCreateSelfSignCertificate(key, &subject, Persisted ? 0 : CERT_CREATE_SELFSIGN_NO_KEY_INFO,
      Persisted ? &key_info : nullptr, &signature, &start, &end, nullptr);
   if (not certificate) {
      Result.reset();
      return false;
   }
   Result.context = certificate;

   // INHIBIT_PERSIST keeps ownership of the key handle with Result rather than the certificate context.

   DWORD digest_size = DWORD(Result.fingerprint.size());
   if (not CertSetCertificateContextProperty(certificate, CERT_NCRYPT_KEY_HANDLE_PROP_ID,
          CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG, &key) or
       not CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, certificate->pbCertEncoded,
          certificate->cbCertEncoded, Result.fingerprint.data(), &digest_size) or
       digest_size != Result.fingerprint.size()) {
      Result.reset();
      return false;
   }
   return true;
}

} // inline namespace
} // namespace crypto_protocol
