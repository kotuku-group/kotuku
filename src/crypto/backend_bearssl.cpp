#include "crypto_internal.h"

#include <bearssl_hash.h>
#include <bearssl_rsa.h>

#include <sys/random.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>

namespace crypto_backend {

static void secure_clear(std::span<int8_t> Bytes) {
   volatile int8_t *output = Bytes.data();
   for (size_t i=0; i < Bytes.size(); i++) output[i] = 0;
}

//********************************************************************************************************************

static ERR file_error(int Error) {
   if ((Error IS EACCES) or (Error IS EPERM) or (Error IS EROFS)) return ERR::NoPermission;
   if (Error IS ENOSPC) return ERR::OutOfSpace;
   return ERR::Failed;
}

//********************************************************************************************************************

static bool sync_descriptor(int Descriptor) {
   while (fsync(Descriptor) != 0) {
      if (errno != EINTR) return false;
   }
   return true;
}

ERR random(std::span<int8_t> Output) {
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

ERR sha256(std::span<const int8_t> Input, std::span<int8_t> Output) {
   br_sha256_context context;
   br_sha256_init(&context);
   br_sha256_update(&context, Input.data(), Input.size());
   br_sha256_out(&context, Output.data());
   return ERR::Okay;
}

ERR verify_rs256(std::span<const uint8_t> Modulus, std::span<const uint8_t> Exponent,
   std::span<const int8_t> Message, std::span<const int8_t> Signature) {
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

//********************************************************************************************************************

ERR write_protected_file(std::string_view Path, std::span<const int8_t> Data) {
   auto separator = Path.find_last_of('/');
   std::string directory = (separator IS std::string_view::npos) ? "." : std::string(Path.substr(0, separator));
   std::string destination = (separator IS std::string_view::npos) ? std::string(Path) :
      std::string(Path.substr(separator + 1));
   if (directory.empty()) directory = "/";
   if (destination.empty() or (destination IS ".") or (destination IS "..")) return ERR::Args;

   int directory_fd = open(directory.c_str(), O_RDONLY|O_DIRECTORY|O_CLOEXEC);
   if (directory_fd < 0) return file_error(errno);

   int temporary_fd = -1;
   std::string temporary;
   std::array<int8_t, 16> random_bytes{};
   constexpr char hex[] = "0123456789abcdef";

   for (int attempt=0; attempt < 8; attempt++) {
      if (random(random_bytes) != ERR::Okay) {
         close(directory_fd);
         return ERR::Failed;
      }

      temporary.assign(".kotuku-protected-");
      for (auto value : random_bytes) {
         temporary.push_back(hex[uint8_t(value) >> 4]);
         temporary.push_back(hex[uint8_t(value) & 15]);
      }
      temporary.append(".tmp");

      temporary_fd = openat(directory_fd, temporary.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600);
      if (temporary_fd >= 0) break;
      if (errno != EEXIST) {
         auto error = file_error(errno);
         close(directory_fd);
         return error;
      }
   }

   if (temporary_fd < 0) {
      close(directory_fd);
      return ERR::Failed;
   }

   ERR result = ERR::Okay;
   size_t offset = 0;
   while (offset < Data.size()) {
      auto written = write(temporary_fd, Data.data() + offset, Data.size() - offset);
      if (written > 0) offset += size_t(written);
      else if ((written < 0) and (errno IS EINTR)) continue;
      else {
         result = file_error(errno);
         break;
      }
   }

   if ((result IS ERR::Okay) and (fchmod(temporary_fd, S_IRUSR|S_IWUSR) != 0)) result = file_error(errno);
   if ((result IS ERR::Okay) and (not sync_descriptor(temporary_fd))) result = file_error(errno);
   if (close(temporary_fd) != 0 and (result IS ERR::Okay)) result = file_error(errno);

   if (result IS ERR::Okay) {
      if (renameat(directory_fd, temporary.c_str(), directory_fd, destination.c_str()) != 0) result = file_error(errno);
      else if (not sync_descriptor(directory_fd)) result = file_error(errno);
   }

   if (result != ERR::Okay) unlinkat(directory_fd, temporary.c_str(), 0);
   close(directory_fd);
   return result;
}

} // namespace crypto_backend
