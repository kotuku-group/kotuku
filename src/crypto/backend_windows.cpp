#include "crypto_internal.h"

#include <windows.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace crypto_backend {

static ERR file_error(DWORD Error) {
   if ((Error IS ERROR_ACCESS_DENIED) or (Error IS ERROR_PRIVILEGE_NOT_HELD) or
       (Error IS ERROR_SHARING_VIOLATION)) return ERR::NoPermission;
   if (Error IS ERROR_DISK_FULL) return ERR::OutOfSpace;
   return ERR::Failed;
}

//********************************************************************************************************************

static std::wstring utf8_to_wide(std::string_view Text) {
   if (Text.empty()) return {};
   int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), int(Text.size()), nullptr, 0);
   if (size <= 0) return {};
   std::wstring result(size_t(size), L'\0');
   if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), int(Text.size()), result.data(), size) != size) {
      return {};
   }
   return result;
}

//********************************************************************************************************************

static HANDLE create_protected_file(const std::wstring &Path, DWORD *Error) {
   HANDLE token = nullptr;
   if (not OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      *Error = GetLastError();
      return INVALID_HANDLE_VALUE;
   }

   DWORD token_size = 0;
   GetTokenInformation(token, TokenUser, nullptr, 0, &token_size);
   if (not (GetLastError() IS ERROR_INSUFFICIENT_BUFFER)) {
      *Error = GetLastError();
      CloseHandle(token);
      return INVALID_HANDLE_VALUE;
   }

   std::vector<uint8_t> token_buffer(token_size);
   if (not GetTokenInformation(token, TokenUser, token_buffer.data(), token_size, &token_size)) {
      *Error = GetLastError();
      CloseHandle(token);
      return INVALID_HANDLE_VALUE;
   }
   CloseHandle(token);

   auto token_user = (TOKEN_USER *)token_buffer.data();
   DWORD sid_size = GetLengthSid(token_user->User.Sid);
   DWORD acl_size = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_size;
   std::vector<uint8_t> acl_buffer(acl_size);
   auto acl = (PACL)acl_buffer.data();
   SECURITY_DESCRIPTOR descriptor;

   if ((not InitializeAcl(acl, acl_size, ACL_REVISION)) or
       (not AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, token_user->User.Sid)) or
       (not InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)) or
       (not SetSecurityDescriptorDacl(&descriptor, true, acl, false)) or
       (not SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))) {
      *Error = GetLastError();
      return INVALID_HANDLE_VALUE;
   }

   SECURITY_ATTRIBUTES security = { sizeof(security), &descriptor, false };
   auto handle = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, &security, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH, nullptr);
   if (handle IS INVALID_HANDLE_VALUE) *Error = GetLastError();
   return handle;
}

ERR random(std::span<int8_t> Output) {
   if (Output.empty()) return ERR::Okay;
   return checked_output(Output, [&]() {
      NTSTATUS status = BCryptGenRandom(nullptr, (PUCHAR)Output.data(), ULONG(Output.size()),
         BCRYPT_USE_SYSTEM_PREFERRED_RNG);
      return BCRYPT_SUCCESS(status);
   }, [](std::span<int8_t> Bytes) { SecureZeroMemory(Bytes.data(), Bytes.size()); });
}

ERR sha256(std::span<const int8_t> Input, std::span<int8_t> Output) {
   return checked_output(Output, [&]() {
      BCRYPT_ALG_HANDLE alg = nullptr;
      BCRYPT_HASH_HANDLE hash = nullptr;
      uint8_t empty = 0;
      PUCHAR input = Input.empty() ? &empty : (PUCHAR)Input.data();
      bool success = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) and
         BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)) and
         BCRYPT_SUCCESS(BCryptHashData(hash, input, ULONG(Input.size()), 0)) and
         BCRYPT_SUCCESS(BCryptFinishHash(hash, (PUCHAR)Output.data(), ULONG(Output.size()), 0));
      if (hash) BCryptDestroyHash(hash);
      if (alg) BCryptCloseAlgorithmProvider(alg, 0);
      return success;
   }, [](std::span<int8_t> Bytes) { SecureZeroMemory(Bytes.data(), Bytes.size()); });
}

ERR verify_rs256(std::span<const uint8_t> Modulus, std::span<const uint8_t> Exponent,
   std::span<const int8_t> Message, std::span<const int8_t> Signature) {
   BCRYPT_ALG_HANDLE alg = nullptr;
   BCRYPT_KEY_HANDLE key = nullptr;
   ERR result = ERR::Failed;
   alignas(BCRYPT_RSAKEY_BLOB) std::array<uint8_t, sizeof(BCRYPT_RSAKEY_BLOB) + 4 + 1024> blob{};
   size_t blob_size = sizeof(BCRYPT_RSAKEY_BLOB) + Exponent.size() + Modulus.size();
   auto header = (BCRYPT_RSAKEY_BLOB *)blob.data();
   header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
   header->BitLength = ULONG(Modulus.size() * 8);
   header->cbPublicExp = ULONG(Exponent.size());
   header->cbModulus = ULONG(Modulus.size());
   header->cbPrime1 = 0;
   header->cbPrime2 = 0;
   memcpy(blob.data() + sizeof(*header), Exponent.data(), Exponent.size());
   memcpy(blob.data() + sizeof(*header) + Exponent.size(), Modulus.data(), Modulus.size());
   std::array<int8_t, 32> digest{};
   if (sha256(Message, digest) != ERR::Okay) return ERR::Failed;
   if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0)) and
       BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(),
          ULONG(blob_size), 0))) {
      BCRYPT_PKCS1_PADDING_INFO padding = { BCRYPT_SHA256_ALGORITHM };
      NTSTATUS status = BCryptVerifySignature(key, &padding, (PUCHAR)digest.data(), ULONG(digest.size()),
         (PUCHAR)Signature.data(), ULONG(Signature.size()), BCRYPT_PAD_PKCS1);
      if (BCRYPT_SUCCESS(status)) result = ERR::Okay;
      else if (status IS NTSTATUS(0xC000A000L)) result = ERR::Mismatch;
   }
   if (key) BCryptDestroyKey(key);
   if (alg) BCryptCloseAlgorithmProvider(alg, 0);
   SecureZeroMemory(digest.data(), digest.size());
   return result;
}

//********************************************************************************************************************

ERR write_protected_file(std::string_view Path, std::span<const int8_t> Data) {
   auto destination = utf8_to_wide(Path);
   if (destination.empty()) return ERR::Args;

   auto separator = destination.find_last_of(L"/\\");
   if ((separator IS std::wstring::npos) or (separator + 1 >= destination.size())) return ERR::Args;
   auto directory = destination.substr(0, separator + 1);

   std::array<int8_t, 16> random_bytes{};
   constexpr wchar_t hex[] = L"0123456789abcdef";
   std::wstring temporary;
   HANDLE handle = INVALID_HANDLE_VALUE;
   DWORD error = ERROR_SUCCESS;

   for (int attempt=0; attempt < 8; attempt++) {
      if (random(random_bytes) != ERR::Okay) return ERR::Failed;

      temporary.assign(directory);
      temporary.append(L".kotuku-protected-");
      for (auto value : random_bytes) {
         temporary.push_back(hex[uint8_t(value) >> 4]);
         temporary.push_back(hex[uint8_t(value) & 15]);
      }
      temporary.append(L".tmp");

      handle = create_protected_file(temporary, &error);
      if (handle != INVALID_HANDLE_VALUE) break;
      if (error != ERROR_FILE_EXISTS) return file_error(error);
   }

   if (handle IS INVALID_HANDLE_VALUE) return ERR::Failed;

   ERR result = ERR::Okay;
   DWORD written = 0;
   if ((not Data.empty()) and
       ((not WriteFile(handle, Data.data(), DWORD(Data.size()), &written, nullptr)) or
        (written != DWORD(Data.size())))) result = file_error(GetLastError());
   if ((result IS ERR::Okay) and (not FlushFileBuffers(handle))) result = file_error(GetLastError());
   if (not CloseHandle(handle) and (result IS ERR::Okay)) result = file_error(GetLastError());

   if (result IS ERR::Okay) {
      if (not MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
         result = file_error(GetLastError());
      }
   }

   if (result != ERR::Okay) DeleteFileW(temporary.c_str());
   return result;
}

} // namespace crypto_backend
