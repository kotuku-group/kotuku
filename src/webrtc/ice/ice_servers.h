// Copied, bounded public ICE-server configuration; parsing never logs credentials.

#pragma once

#include <kotuku/main.h>

#include <string>
#include <string_view>
#include <vector>

namespace rtc {

enum class ServerTransport { STUN, UDP, TCP, TLS }; // UDP, TCP and TLS denote TURN servers

struct IceServer {
   std::string host, username, credential;
   uint16_t port = 3478;
   ServerTransport transport = ServerTransport::STUN;
};

// ICE gathering policy flags

constexpr uint32_t POLICY_RELAY_ONLY   = 1;
constexpr uint32_t POLICY_NO_HOST      = 2;
constexpr uint32_t POLICY_NO_MDNS      = 4;
constexpr uint32_t POLICY_DISABLE_IPV6 = 8;

ERR parse_ice_servers(std::string_view Text, std::vector<IceServer> &Result);

} // namespace rtc
