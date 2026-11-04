#include "crypto_internal.h"

#include <bearssl_aead.h>
#include <bearssl_block.h>

namespace crypto_backend {
namespace {

void secure_clear(void *Data, size_t Size) {
   volatile uint8_t *bytes = (volatile uint8_t *)Data;
   for (size_t i = 0; i < Size; ++i) bytes[i] = 0;
}

bool valid_key(std::span<const uint8_t> Key) {
   return Key.size() IS 16 or Key.size() IS 32;
}

}

ERR aes_ctr(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   uint32_t Counter, std::span<uint8_t> Data) {
   if (not valid_key(Key) or not valid_ctr_range(Counter, Data.size())) return ERR::Args;
   br_aes_ct64_ctr_keys keys{};
   br_aes_ct64_ctr_init(&keys, Key.data(), Key.size());
   br_aes_ct64_ctr_run(&keys, Nonce.data(), Counter, Data.data(), Data.size());
   secure_clear(&keys, sizeof(keys));
   return ERR::Okay;
}

ERR aes_gcm_encrypt(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   std::span<const uint8_t> AAD, std::span<uint8_t> Data, std::span<uint8_t, 16> Tag) {
   if (not valid_key(Key)) return ERR::Args;
   br_aes_ct64_ctr_keys keys{};
   br_gcm_context context{};
   br_aes_ct64_ctr_init(&keys, Key.data(), Key.size());
   br_gcm_init(&context, &keys.vtable, br_ghash_ctmul64);
   br_gcm_reset(&context, Nonce.data(), Nonce.size());
   br_gcm_aad_inject(&context, AAD.data(), AAD.size());
   br_gcm_flip(&context);
   br_gcm_run(&context, 1, Data.data(), Data.size());
   br_gcm_get_tag(&context, Tag.data());
   secure_clear(&context, sizeof(context));
   secure_clear(&keys, sizeof(keys));
   return ERR::Okay;
}

ERR aes_gcm_decrypt(std::span<const uint8_t> Key, std::span<const uint8_t, 12> Nonce,
   std::span<const uint8_t> AAD, std::span<uint8_t> Data, std::span<const uint8_t, 16> Tag) {
   if (not valid_key(Key)) return ERR::Args;
   br_aes_ct64_ctr_keys keys{};
   br_gcm_context context{};
   br_aes_ct64_ctr_init(&keys, Key.data(), Key.size());
   br_gcm_init(&context, &keys.vtable, br_ghash_ctmul64);
   br_gcm_reset(&context, Nonce.data(), Nonce.size());
   br_gcm_aad_inject(&context, AAD.data(), AAD.size());
   br_gcm_flip(&context);
   br_gcm_run(&context, 0, Data.data(), Data.size());
   auto valid = br_gcm_check_tag(&context, Tag.data()) IS 1;
   if (not valid) secure_clear(Data.data(), Data.size());
   secure_clear(&context, sizeof(context));
   secure_clear(&keys, sizeof(keys));
   return valid ? ERR::Okay : ERR::Mismatch;
}

}
