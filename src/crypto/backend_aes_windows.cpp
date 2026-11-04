#define OBJECTID WindowsObjectID
#include <windows.h>
#undef OBJECTID
#undef __OBJECTID_DEFINED
#undef RELATIVE
#undef DELETE
#undef FD_READ
#undef FD_WRITE
#include <bcrypt.h>

#include "crypto_internal.h"

#include <algorithm>
#include <array>
#include <limits>

namespace crypto_backend {
namespace {

struct AesKey {
   BCRYPT_ALG_HANDLE Algorithm = nullptr;
   BCRYPT_KEY_HANDLE Key = nullptr;

   ~AesKey() {
      if (Key) BCryptDestroyKey(Key);
      if (Algorithm) BCryptCloseAlgorithmProvider(Algorithm, 0);
   }

   bool initialise(std::span<const uint8_t> Bytes, LPCWSTR Mode) {
      return BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)) and
         BCRYPT_SUCCESS(BCryptSetProperty(Algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)Mode,
            ULONG((wcslen(Mode) + 1) * sizeof(wchar_t)), 0)) and
         BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(Algorithm, &Key, nullptr, 0,
            (PUCHAR)Bytes.data(), ULONG(Bytes.size()), 0));
   }
};

bool valid_key(std::span<const uint8_t> Key) {
   return Key.size() IS 16 or Key.size() IS 32;
}

ERR gcm(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   std::span<const uint8_t> AAD, std::span<uint8_t> Data, std::span<uint8_t, 16> Tag, bool Encrypt) {
   if (not valid_key(Key) or Data.size() > std::numeric_limits<ULONG>::max() or
       AAD.size() > std::numeric_limits<ULONG>::max()) return ERR::Args;
   AesKey key;
   if (not key.initialise(Key, BCRYPT_CHAIN_MODE_GCM)) {
      SecureZeroMemory(Data.data(), Data.size());
      if (Encrypt) SecureZeroMemory(Tag.data(), Tag.size());
      return ERR::Failed;
   }
   BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
   BCRYPT_INIT_AUTH_MODE_INFO(info);
   info.pbNonce = (PUCHAR)Nonce.data();
   info.cbNonce = ULONG(Nonce.size());
   info.pbAuthData = (PUCHAR)AAD.data();
   info.cbAuthData = ULONG(AAD.size());
   info.pbTag = Tag.data();
   info.cbTag = ULONG(Tag.size());
   uint8_t empty = 0;
   auto data = Data.empty() ? &empty : Data.data();
   ULONG written = 0;
   auto status = Encrypt ?
      BCryptEncrypt(key.Key, data, ULONG(Data.size()), &info, nullptr, 0,
         data, ULONG(Data.size()), &written, 0) :
      BCryptDecrypt(key.Key, data, ULONG(Data.size()), &info, nullptr, 0,
         data, ULONG(Data.size()), &written, 0);
   if (BCRYPT_SUCCESS(status) and written IS Data.size()) return ERR::Okay;
   SecureZeroMemory(Data.data(), Data.size());
   if (Encrypt) SecureZeroMemory(Tag.data(), Tag.size());
   return status IS NTSTATUS(0xc000a002) ? ERR::Mismatch : ERR::Failed; // STATUS_AUTH_TAG_MISMATCH
}

}

ERR aes_ctr(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   uint32_t Counter, std::span<uint8_t> Data) {
   if (not valid_key(Key) or not valid_ctr_range(Counter, Data.size())) return ERR::Args;
   AesKey key;
   if (not key.initialise(Key, BCRYPT_CHAIN_MODE_ECB)) {
      SecureZeroMemory(Data.data(), Data.size());
      return ERR::Failed;
   }
   std::array<uint8_t, 16> block{}, stream{};
   std::copy(Nonce.begin(), Nonce.end(), block.begin());
   auto wipe = kt::Defer([&]() { SecureZeroMemory(stream.data(), stream.size()); });
   for (size_t offset = 0; offset < Data.size(); offset += 16, ++Counter) {
      for (int i = 0; i < 4; ++i) block[12 + i] = uint8_t(Counter >> (24 - i * 8));
      ULONG written = 0;
      if (not BCRYPT_SUCCESS(BCryptEncrypt(key.Key, block.data(), ULONG(block.size()), nullptr, nullptr, 0,
          stream.data(), ULONG(stream.size()), &written, 0)) or written != stream.size()) {
         SecureZeroMemory(Data.data(), Data.size());
         return ERR::Failed;
      }
      auto count = std::min<size_t>(16, Data.size() - offset);
      for (size_t i = 0; i < count; ++i) Data[offset + i] ^= stream[i];
   }
   return ERR::Okay;
}

ERR aes_gcm_encrypt(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   std::span<const uint8_t> AAD, std::span<uint8_t> Data, std::span<uint8_t, 16> Tag) {
   return gcm(Key, Nonce, AAD, Data, Tag, true);
}

ERR aes_gcm_decrypt(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   std::span<const uint8_t> AAD, std::span<uint8_t> Data, std::span<const uint8_t, 16> Tag) {
   std::array<uint8_t, 16> tag;
   std::copy(Tag.begin(), Tag.end(), tag.begin());
   return gcm(Key, Nonce, AAD, Data, tag, false);
}

}
