// Incremental UTF-8 validator.  See ws_utf8.h.

#include "ws_utf8.h"

namespace ws {

bool Utf8Validator::feed(std::span<const uint8_t> Data) noexcept
{
   if (failed) return false;

   size_t i = 0;
   while (i < Data.size()) {
      if (remaining) {
         auto byte = Data[i++];
         if ((byte < lower) or (byte > upper)) {
            failed = true;
            return false;
         }
         remaining--;
         lower = 0x80; // Only the first continuation byte has a restricted range
         upper = 0xbf;
         continue;
      }

      // ASCII fast path

      while ((i < Data.size()) and (Data[i] < 0x80)) i++;
      if (i >= Data.size()) break;

      auto lead = Data[i++];
      if ((lead >= 0xc2) and (lead <= 0xdf)) remaining = 1;
      else if (lead IS 0xe0) { remaining = 2; lower = 0xa0; }      // Excludes overlong 3-byte forms
      else if (lead IS 0xed) { remaining = 2; upper = 0x9f; }      // Excludes surrogates
      else if ((lead >= 0xe1) and (lead <= 0xef)) remaining = 2;
      else if (lead IS 0xf0) { remaining = 3; lower = 0x90; }      // Excludes overlong 4-byte forms
      else if (lead IS 0xf4) { remaining = 3; upper = 0x8f; }      // Excludes code points above U+10FFFF
      else if ((lead >= 0xf1) and (lead <= 0xf3)) remaining = 3;
      else { // Stray continuation byte, overlong 2-byte lead (C0, C1) or F5 to FF
         failed = true;
         return false;
      }
   }
   return true;
}

} // namespace ws
