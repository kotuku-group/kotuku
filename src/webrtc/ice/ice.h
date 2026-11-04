// Socket-independent ICE-lite connectivity-check responder (RFC 8445).

#pragma once

#include <kotuku/config.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtc::ice {

struct Endpoint {
   std::array<uint8_t, 16> address{};
   uint16_t port = 0;
   bool ipv6 = false;
   uint64_t route = 0; // Local-only allocation serial; zero denotes a direct socket.  Never encoded on wire.

   bool operator==(const Endpoint &) const = default;
};

struct CheckResult {
   std::vector<uint8_t> response;
   bool authenticated = false;
   bool nominated = false;
   bool role_conflict = false;
};

//********************************************************************************************************************
// Owns ICE credentials and validated source addresses; the caller owns the UDP socket and sends each response
// as one datagram.  A successful check validates a source for early DTLS traffic, even before nomination.
class LiteResponder {
public:
   LiteResponder(std::string_view LocalUfrag, std::string_view LocalPassword, std::string_view RemoteUfrag);

   CheckResult receive(std::span<const uint8_t> Datagram, const Endpoint &Source);
   bool validated(const Endpoint &Source) const;
   const Endpoint * nominated() const;

private:
   std::string local_ufrag;
   std::string local_password;
   std::string remote_ufrag;
   std::vector<Endpoint> valid_sources;
   Endpoint nominated_source;
   bool has_nomination = false;
};

} // namespace rtc::ice
