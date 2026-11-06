#include "../ice/ice_servers.h"

#include <cstdio>

int main() {
   struct Valid {
      const char *url, *host;
      uint16_t port;
      rtc::ServerTransport transport;
   };
   for (const auto &value : {
        Valid{"stun:localhost", "localhost", 3478, rtc::ServerTransport::STUN},
        Valid{"stun:[::1]:65535", "::1", 65535, rtc::ServerTransport::STUN},
        Valid{"turn:127.0.0.1:1?transport=tcp", "127.0.0.1", 1, rtc::ServerTransport::TCP},
        Valid{"turns:[::1]", "::1", 5349, rtc::ServerTransport::TLS}}) {
      auto credentials = value.transport IS rtc::ServerTransport::STUN ? "" :
         ",\"username\":\"user\",\"credential\":\"pass\"";
      std::vector<rtc::IceServer> parsed;
      if (rtc::parse_ice_servers(std::string("[{\"url\":\"") + value.url + "\"" + credentials + "}]", parsed)
          != ERR::Okay or parsed.size() != 1 or parsed[0].host != value.host or parsed[0].port != value.port or
          parsed[0].transport != value.transport) return 2;
   }
   for (auto url : {"STUN:localhost", "localhost:3478", "stun:[]", "stun:[localhost]", "stun:[::1]extra",
        "stun:localhost:", "stun:localhost:0", "stun:localhost:65536", "stun:localhost:12x",
        "stun:localhost:1:2", "stun:user@localhost", "stun:local_host", "stun:localhost?transport=udp"}) {
      std::vector<rtc::IceServer> parsed;
      if (rtc::parse_ice_servers(std::string("[{\"url\":\"") + url + "\"}]", parsed) IS ERR::Okay) return 3;
   }

   const std::string prefix = R"([{"url":"turn:localhost","username":")";
   const std::string suffix = R"(","credential":"pass"}])";
   const std::string expected = "A\xc2\xa3\xe2\x82\xac\xf0\x9f\x98\x80";
   std::vector<rtc::IceServer> servers;
   for (const auto &username : {expected, std::string(R"(\u0041\u00a3\u20ac\ud83d\ude00)")}) {
      if (rtc::parse_ice_servers(prefix + username + suffix, servers) != ERR::Okay or
          servers.size() != 1 or servers[0].username != expected) {
         std::fprintf(stderr, "ICE server UTF-8 or escaped Unicode decoding differs\n");
         return 1;
      }
   }
   for (const auto &username : {std::string("\0", 1), std::string("\x80"), std::string("\xc0\x80"),
        std::string("\xe2\x82"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
        std::string(R"(\u0000)"), std::string(R"(\ud800)"), std::string(R"(\udc00)"),
        std::string(R"(\ud800\u0041)")}) {
      if (rtc::parse_ice_servers(prefix + username + suffix, servers) IS ERR::Okay or
          servers.size() != 1 or servers[0].username != expected) {
         std::fprintf(stderr, "Invalid ICE server Unicode accepted or changed previous output\n");
         return 1;
      }
   }
   return 0;
}
