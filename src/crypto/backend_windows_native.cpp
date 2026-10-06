#include <windows.h>
#include <bcrypt.h>
#include <aclapi.h>

#include "backend_windows_native.h"

#include <array>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#define IS ==

namespace crypto_native {

static LPCWSTR algorithm_name(Algorithm Algorithm) {
   switch (Algorithm) {
      case Algorithm::MD5:    return BCRYPT_MD5_ALGORITHM;
      case Algorithm::SHA1:   return BCRYPT_SHA1_ALGORITHM;
      case Algorithm::SHA256: return BCRYPT_SHA256_ALGORITHM;
      case Algorithm::SHA512: return BCRYPT_SHA512_ALGORITHM;
      default:                return nullptr;
   }
}

size_t digest_size(Algorithm Algorithm) {
   switch (Algorithm) {
      case Algorithm::MD5:    return 16;
      case Algorithm::SHA1:   return 20;
      case Algorithm::SHA256: return 32;
      case Algorithm::SHA512: return 64;
      default:                return 0;
   }
}

struct HashContext {
   BCRYPT_ALG_HANDLE Algorithm = nullptr;
   BCRYPT_HASH_HANDLE Hash = nullptr;
   std::vector<uint8_t> Key;
};

static bool create_hash(HashContext *Context) {
   PUCHAR key_data = Context->Key.empty() ? nullptr : Context->Key.data();
   return BCRYPT_SUCCESS(BCryptCreateHash(Context->Algorithm, &Context->Hash, nullptr, 0, key_data,
      ULONG(Context->Key.size()), 0));
}

HashContext * create_hash_context(Algorithm Algorithm, std::span<const int8_t> Key, bool UseHMAC) {
   auto name = algorithm_name(Algorithm);
   if (not name) return nullptr;

   auto context = new (std::nothrow) HashContext;
   if (not context) return nullptr;
   if (not Key.empty()) context->Key.assign((const uint8_t *)Key.data(), (const uint8_t *)Key.data() + Key.size());
   ULONG flags = UseHMAC ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
   if ((not BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&context->Algorithm, name, nullptr, flags))) or
       (not create_hash(context))) {
      destroy_hash_context(context);
      return nullptr;
   }
   return context;
}

void destroy_hash_context(HashContext *Context) {
   if (not Context) return;
   if (Context->Hash) BCryptDestroyHash(Context->Hash);
   if (Context->Algorithm) BCryptCloseAlgorithmProvider(Context->Algorithm, 0);
   if (not Context->Key.empty()) SecureZeroMemory(Context->Key.data(), Context->Key.size());
   delete Context;
}

Result update_hash(HashContext *Context, std::span<const int8_t> Input) {
   uint8_t empty = 0;
   PUCHAR input = Input.empty() ? &empty : (PUCHAR)Input.data();
   return BCRYPT_SUCCESS(BCryptHashData(Context->Hash, input, ULONG(Input.size()), 0)) ?
      Result::Okay : Result::Failed;
}

Result digest_hash(HashContext *Context, std::span<int8_t> Output) {
   BCRYPT_HASH_HANDLE copy = nullptr;
   if (not BCRYPT_SUCCESS(BCryptDuplicateHash(Context->Hash, &copy, nullptr, 0, 0))) {
      SecureZeroMemory(Output.data(), Output.size());
      return Result::Failed;
   }
   auto result = BCRYPT_SUCCESS(BCryptFinishHash(copy, (PUCHAR)Output.data(), ULONG(Output.size()), 0)) ?
      Result::Okay : Result::Failed;
   BCryptDestroyHash(copy);
   if (result != Result::Okay) SecureZeroMemory(Output.data(), Output.size());
   return result;
}

Result reset_hash(HashContext *Context) {
   if (Context->Hash) BCryptDestroyHash(Context->Hash);
   Context->Hash = nullptr;
   return create_hash(Context) ? Result::Okay : Result::Failed;
}

Result hash(Algorithm Algorithm, std::span<const int8_t> Input, std::span<int8_t> Output) {
   auto context = create_hash_context(Algorithm, {}, false);
   if (not context) {
      SecureZeroMemory(Output.data(), Output.size());
      return Result::Failed;
   }
   auto result = update_hash(context, Input);
   if (result != Result::Okay) SecureZeroMemory(Output.data(), Output.size());
   else result = digest_hash(context, Output);
   destroy_hash_context(context);
   return result;
}

Result hmac(Algorithm Algorithm, std::span<const int8_t> Key, std::span<const int8_t> Input,
   std::span<int8_t> Output) {
   auto context = create_hash_context(Algorithm, Key, true);
   if (not context) {
      SecureZeroMemory(Output.data(), Output.size());
      return Result::Failed;
   }
   auto result = update_hash(context, Input);
   if (result != Result::Okay) SecureZeroMemory(Output.data(), Output.size());
   else result = digest_hash(context, Output);
   destroy_hash_context(context);
   return result;
}

static Result file_error(DWORD Error) {
   switch (Error) {
      case ERROR_ACCESS_DENIED:
      case ERROR_PRIVILEGE_NOT_HELD:
      case ERROR_SHARING_VIOLATION:
         return Result::NoPermission;
      case ERROR_DISK_FULL:
         return Result::OutOfSpace;
      default:
         return Result::Failed;
   }
}

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

static HANDLE create_protected_file(const std::wstring &Path, DWORD *Error) {
   HANDLE token = nullptr;
   if (not OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      *Error = GetLastError();
      return INVALID_HANDLE_VALUE;
   }

   DWORD token_size = 0;
   GetTokenInformation(token, TokenUser, nullptr, 0, &token_size);
   if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
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

Result random(std::span<int8_t> Output) {
   if (Output.empty()) return Result::Okay;
   NTSTATUS status = BCryptGenRandom(nullptr, (PUCHAR)Output.data(), ULONG(Output.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
   if (BCRYPT_SUCCESS(status)) return Result::Okay;
   SecureZeroMemory(Output.data(), Output.size());
   return Result::Failed;
}

Result verify_rs256(std::span<const uint8_t> Modulus, std::span<const uint8_t> Exponent,
   std::span<const int8_t> Message, std::span<const int8_t> Signature) {
   BCRYPT_ALG_HANDLE algorithm = nullptr;
   BCRYPT_KEY_HANDLE key = nullptr;
   Result result = Result::Failed;
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
   if (hash(Algorithm::SHA256, Message, digest) != Result::Okay) return Result::Failed;
   if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0)) and
       BCRYPT_SUCCESS(BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(),
          ULONG(blob_size), 0))) {
      BCRYPT_PKCS1_PADDING_INFO padding = { BCRYPT_SHA256_ALGORITHM };
      NTSTATUS status = BCryptVerifySignature(key, &padding, (PUCHAR)digest.data(), ULONG(digest.size()),
         (PUCHAR)Signature.data(), ULONG(Signature.size()), BCRYPT_PAD_PKCS1);
      if (BCRYPT_SUCCESS(status)) result = Result::Okay;
      else if (status IS NTSTATUS(0xC000A000L)) result = Result::Mismatch;
   }
   if (key) BCryptDestroyKey(key);
   if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
   SecureZeroMemory(digest.data(), digest.size());
   return result;
}

Result write_protected_file(std::string_view Path, std::span<const int8_t> Data) {
   auto destination = utf8_to_wide(Path);
   if (destination.empty()) return Result::Args;

   auto separator = destination.find_last_of(L"/\\");
   if ((separator IS std::wstring::npos) or (separator + 1 >= destination.size())) return Result::Args;
   auto directory = destination.substr(0, separator + 1);

   std::array<int8_t, 16> random_bytes{};
   constexpr wchar_t hex[] = L"0123456789abcdef";
   std::wstring temporary;
   HANDLE handle = INVALID_HANDLE_VALUE;
   DWORD error = ERROR_SUCCESS;

   for (int attempt=0; attempt < 8; attempt++) {
      if (random(random_bytes) != Result::Okay) return Result::Failed;

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

   if (handle IS INVALID_HANDLE_VALUE) return Result::Failed;

   Result result = Result::Okay;
   DWORD written = 0;
   if ((not Data.empty()) and
       ((not WriteFile(handle, Data.data(), DWORD(Data.size()), &written, nullptr)) or
        (written != DWORD(Data.size())))) result = file_error(GetLastError());
   if ((result IS Result::Okay) and (not FlushFileBuffers(handle))) result = file_error(GetLastError());
   if ((not CloseHandle(handle)) and (result IS Result::Okay)) result = file_error(GetLastError());

   if (result IS Result::Okay) {
      if (not MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
         result = file_error(GetLastError());
      }
   }

   if (result != Result::Okay) DeleteFileW(temporary.c_str());
   return result;
}

} // namespace crypto_native
