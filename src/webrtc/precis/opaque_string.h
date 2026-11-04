// RFC 8265 OpaqueString preparation and enforcement for STUN credentials.

#pragma once

#include <kotuku/config.h>

#include <string>
#include <string_view>

namespace rtc::precis {

// Input and output are UTF-8.  Rejects empty, malformed and disallowed strings.
// Output is replaced only after all FreeformClass and contextual rules pass.
bool prepare_opaque_string(std::string_view Input, std::string &Output);

} // namespace rtc::precis
