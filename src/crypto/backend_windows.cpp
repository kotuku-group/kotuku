#include "crypto_internal.h"

#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>

namespace crypto_backend {

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

} // namespace crypto_backend
