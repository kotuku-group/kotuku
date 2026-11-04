#include "../sdp/sdp.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
   rtc::sdp::Description parsed;
   if (rtc::sdp::parse(std::string_view((const char *)Data, Size), parsed)) {
      auto serialised = rtc::sdp::serialize(parsed);
      rtc::sdp::Description again;
      if (serialised.empty() or not rtc::sdp::parse(serialised, again)) __builtin_trap();
   }
   return 0;
}
