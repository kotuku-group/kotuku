#include "../turn/turn.h"
#include <algorithm>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
   if (Size > rtc::turn::MAX_FRAME * 2) return 0;
   auto wire = std::span<const uint8_t>(Data, Size);
   rtc::stun::Message parsed;
   if (rtc::stun::parse(wire, parsed)) {
      (void)rtc::turn::valid_attributes(parsed);
      for (const auto &attribute : parsed.attributes) {
         rtc::turn::Endpoint endpoint;
         (void)rtc::turn::decode_xor_address(attribute, parsed, endpoint);
      }
   }
   uint16_t channel = 0; std::span<const uint8_t> payload;
   (void)rtc::turn::decode_channel_data(wire, false, channel, payload);
   (void)rtc::turn::decode_channel_data(wire, true, channel, payload);
   rtc::turn::StreamDecoder stream;
   size_t offset = 0;
   // Bound work even when input emits many empty records.  Split points are independent of record framing.
   for (unsigned i = 0; i < 256 and offset < Size; ++i) {
      size_t count = std::min(Size - offset, size_t(1 + Data[offset]));
      size_t consumed = 0; std::vector<std::vector<uint8_t>> records;
      if (not stream.feed(wire.subspan(offset, count), consumed, records) or not consumed) break;
      offset += consumed;
   }
   return 0;
}
