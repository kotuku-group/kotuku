#include "crypto_internal.h"
#include "backend_windows_native.h"

#include <new>

namespace crypto_backend {

static crypto_native::Algorithm native_algorithm(HASH Algorithm) {
   switch (Algorithm) {
      case HASH::MD5:    return crypto_native::Algorithm::MD5;
      case HASH::SHA1:   return crypto_native::Algorithm::SHA1;
      case HASH::SHA256: return crypto_native::Algorithm::SHA256;
      case HASH::SHA512: return crypto_native::Algorithm::SHA512;
      default:           return crypto_native::Algorithm::Invalid;
   }
}

static ERR native_result(crypto_native::Result Result) {
   switch (Result) {
      case crypto_native::Result::Okay:         return ERR::Okay;
      case crypto_native::Result::Mismatch:     return ERR::Mismatch;
      case crypto_native::Result::Args:         return ERR::Args;
      case crypto_native::Result::NoPermission: return ERR::NoPermission;
      case crypto_native::Result::OutOfSpace:   return ERR::OutOfSpace;
      default:                                  return ERR::Failed;
   }
}

size_t digest_size(HASH Algorithm) {
   return crypto_native::digest_size(native_algorithm(Algorithm));
}

class WindowsHashContext final : public HashContext {
   crypto_native::HashContext *context;

public:
   WindowsHashContext(HASH Algorithm, std::span<const int8_t> Key, bool UseHMAC) :
      context(crypto_native::create_hash_context(native_algorithm(Algorithm), Key, UseHMAC)) { }

   ~WindowsHashContext() override {
      crypto_native::destroy_hash_context(context);
   }

   bool valid() const { return context != nullptr; }

   ERR update(std::span<const int8_t> Input) override {
      return native_result(crypto_native::update_hash(context, Input));
   }

   ERR digest(std::span<int8_t> Output) override {
      return native_result(crypto_native::digest_hash(context, Output));
   }

   ERR reset() override {
      return native_result(crypto_native::reset_hash(context));
   }
};

std::unique_ptr<HashContext> make_hash_context(HASH Algorithm, std::span<const int8_t> Key, bool UseHMAC) {
   if (native_algorithm(Algorithm) IS crypto_native::Algorithm::Invalid) return {};
   auto context = std::unique_ptr<WindowsHashContext>(
      new (std::nothrow) WindowsHashContext(Algorithm, Key, UseHMAC));
   if ((not context) or (not context->valid())) return {};
   return context;
}

ERR hash(HASH Algorithm, std::span<const int8_t> Input, std::span<int8_t> Output) {
   return native_result(crypto_native::hash(native_algorithm(Algorithm), Input, Output));
}

ERR hmac(HASH Algorithm, std::span<const int8_t> Key, std::span<const int8_t> Input,
   std::span<int8_t> Output) {
   return native_result(crypto_native::hmac(native_algorithm(Algorithm), Key, Input, Output));
}

ERR random(std::span<int8_t> Output) {
   return native_result(crypto_native::random(Output));
}

ERR verify_rs256(std::span<const uint8_t> Modulus, std::span<const uint8_t> Exponent,
   std::span<const int8_t> Message, std::span<const int8_t> Signature) {
   return native_result(crypto_native::verify_rs256(Modulus, Exponent, Message, Signature));
}

ERR write_protected_file(std::string_view Path, std::span<const int8_t> Data) {
   return native_result(crypto_native::write_protected_file(Path, Data));
}

} // namespace crypto_backend
