#include "crypto_internal.h"

#include <bearssl_hash.h>
#include <bearssl_rsa.h>

#include <sys/random.h>

#include <array>
#include <cerrno>

namespace crypto_backend {

static void secure_clear(std::span<int8_t> Bytes)
{
   volatile int8_t *output = Bytes.data();
   for (size_t i=0; i < Bytes.size(); i++) output[i] = 0;
}

ERR random(std::span<int8_t> Output)
{
   if (Output.empty()) return ERR::Okay;

   return checked_output(Output, [&]() {
      size_t offset = 0;
      while (offset < Output.size()) {
         ssize_t received = getrandom(Output.data() + offset, Output.size() - offset, 0);
         if (received > 0) offset += size_t(received);
         else if ((received < 0) and (errno IS EINTR)) continue;
         else return false;
      }
      return true;
   }, secure_clear);
}

ERR sha256(std::span<const int8_t> Input, std::span<int8_t> Output)
{
   br_sha256_context context;
   br_sha256_init(&context);
   br_sha256_update(&context, Input.data(), Input.size());
   br_sha256_out(&context, Output.data());
   return ERR::Okay;
}

ERR verify_rs256(std::span<const uint8_t> Modulus, std::span<const uint8_t> Exponent,
   std::span<const int8_t> Message, std::span<const int8_t> Signature)
{
   std::array<int8_t, 32> digest{};
   std::array<int8_t, 32> signed_digest{};
   if (sha256(Message, digest) != ERR::Okay) return ERR::Failed;

   br_rsa_public_key key = {
      (unsigned char *)Modulus.data(), Modulus.size(),
      (unsigned char *)Exponent.data(), Exponent.size()
   };

   if (br_rsa_i31_pkcs1_vrfy((const unsigned char *)Signature.data(), Signature.size(), BR_HASH_OID_SHA256,
       signed_digest.size(), &key, (unsigned char *)signed_digest.data()) IS 0) return ERR::Mismatch;

   volatile uint32_t difference = 0;
   for (size_t i=0; i < digest.size(); i++) {
      difference = difference | uint32_t(uint8_t(digest[i]) ^ uint8_t(signed_digest[i]));
   }
   return (difference IS 0) ? ERR::Okay : ERR::Mismatch;
}

} // namespace crypto_backend
