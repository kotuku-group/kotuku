#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

struct AudioEndpointFormat {
   unsigned Channels = 0;
   unsigned Bits = 0;
   unsigned ValidBits = 0;
   unsigned Mask = 0;
   bool Floating = false;

   bool valid() const {
      if (!Channels or Channels > 32) return false;
      if (Mask and unsigned(std::popcount(Mask)) != Channels) return false;
      // Mono uses the endpoint's sole speaker; wider layouts require an explicit front pair.
      if (Channels > 2 and (Mask & 3) != 3) return false;
      if (Channels IS 2 and Mask and Mask != 3) return false;
      if (Floating) return Bits IS 32 and ValidBits IS 32;
      return (Bits IS 8 or Bits IS 16 or Bits IS 24 or Bits IS 32) and
         ValidBits > 0 and ValidBits <= Bits;
   }

   void write(void *Destination, const float *Source, unsigned Frames) const {
      auto output = (uint8_t *)Destination;
      const unsigned source_channels = Channels IS 1 ? 1 : 2;
      for (unsigned frame = 0; frame < Frames; ++frame) {
         for (unsigned channel = 0; channel < Channels; ++channel) {
            float value = channel < source_channels ?
               std::clamp(Source[frame * source_channels + channel], -1.0f, 1.0f) : 0.0f;
            if (!std::isfinite(value)) value = 0;
            if (Floating) {
               std::memcpy(output, &value, sizeof(value));
               output += sizeof(value);
            }
            else {
               const int64_t scale = (int64_t(1) << (ValidBits - 1)) - 1;
               uint64_t encoded = uint64_t(int64_t(std::llround(double(value) * double(scale))));
               if (Bits IS 8) encoded += uint64_t(1) << (ValidBits - 1);
               encoded <<= Bits - ValidBits;
               for (unsigned byte = 0; byte < Bits / 8; ++byte) *output++ = uint8_t(encoded >> (byte * 8));
            }
         }
      }
   }
};
