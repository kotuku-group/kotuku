// Incremental UTF-8 validator for WebSocket text messages (RFC 6455 section 8.1, RFC 3629).
//
// The validator keeps carry-over state so that a code point split across frames or reads is checked as one sequence.
// Overlong forms, UTF-16 surrogates (U+D800 to U+DFFF) and code points above U+10FFFF are rejected.  This file has no
// object-system dependencies.

#pragma once

#include <kotuku/config.h>
#include <cstdint>
#include <span>
#include <string_view>

namespace ws {

class Utf8Validator {
public:
   // Validates the next run of bytes.  Returns false as soon as an invalid byte is found, after which the validator
   // remains in the failed state until reset().

   bool feed(std::span<const uint8_t> Data) noexcept;

   bool feed(std::string_view Data) noexcept {
      return feed(std::span<const uint8_t>((const uint8_t *)Data.data(), Data.size()));
   }

   // True if the input so far ends on a code point boundary and no invalid byte has been seen.

   [[nodiscard]] bool complete() const noexcept { return (not failed) and (remaining IS 0); }

   [[nodiscard]] bool has_failed() const noexcept { return failed; }

   void reset() noexcept {
      remaining = 0;
      lower = 0x80;
      upper = 0xbf;
      failed = false;
   }

private:
   uint8_t remaining = 0;  // Continuation bytes still expected for the current sequence
   uint8_t lower = 0x80;   // Permitted range of the next continuation byte
   uint8_t upper = 0xbf;
   bool failed = false;
};

// Validates a complete buffer in one call.

[[nodiscard]] inline bool valid_utf8(std::span<const uint8_t> Data) noexcept
{
   Utf8Validator validator;
   return validator.feed(Data) and validator.complete();
}

[[nodiscard]] inline bool valid_utf8(std::string_view Data) noexcept
{
   return valid_utf8(std::span<const uint8_t>((const uint8_t *)Data.data(), Data.size()));
}

} // namespace ws
