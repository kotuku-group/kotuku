// Parser for the ICE server configuration, which is a JSON array of objects with "url", "username" and "credential"
// string members.  STUN and TURN URIs follow RFC 7064 and RFC 7065.

#include "ice_servers.h"
#include "ice_candidates.h"
#include "../../link/unicode.h"

#include <array>
#include <charconv>
#include <set>

namespace rtc {
namespace {

//********************************************************************************************************************
// Returns true if Text is well-formed UTF-8.

bool utf8(std::string_view Text)
{
   for (size_t i = 0; i < Text.size();) {
      int count;
      auto value = UTF8ReadValue(Text.substr(i), &count);
      if (value IS 0 and Text[i] != 0) return false;
      i += size_t(count);
   }

   return true;
}

//********************************************************************************************************************
// This deliberately accepts only the contract's array/object/string grammar, with JSON string escapes.

class Reader {
public:
   std::string_view text;

   void space() {
      while (not text.empty() and (text[0] IS ' ' or text[0] IS '\n' or
         text[0] IS '\r' or text[0] IS '\t')) text.remove_prefix(1);
   }

   bool take(char Ch) { // Consumes Ch if it is the next non-space character
      space();
      if (text.empty() or text[0] != Ch) return false;
      text.remove_prefix(1);
      return true;
   }

   bool hex(uint32_t &Value) {
      if (text.size() < 4) return false;
      auto [end, error] = std::from_chars(text.data(), text.data() + 4, Value, 16);
      if (error != std::errc() or end != text.data() + 4) return false;
      text.remove_prefix(4);
      return true;
   }

   // Reads a JSON string.  Escaped U+0000 and unpaired surrogates are rejected.

   bool string(std::string &Value) {
      if (not take('"')) return false;

      Value.clear();
      while (not text.empty()) {
         auto ch = uint8_t(text[0]);
         text.remove_prefix(1);
         if (ch IS '"') return true;
         if (ch < 32) return false;

         if (ch != '\\') {
            Value += char(ch);
            continue;
         }

         if (text.empty()) return false;
         char escape = text[0];
         text.remove_prefix(1);

         switch (escape) {
            case '"': case '\\': case '/': Value += escape; break;
            case 'b': Value += '\b'; break;
            case 'f': Value += '\f'; break;
            case 'n': Value += '\n'; break;
            case 'r': Value += '\r'; break;
            case 't': Value += '\t'; break;

            case 'u': {
               uint32_t code = 0;
               if (not hex(code) or code IS 0) return false;
               if (code >= 0xd800 and code <= 0xdbff) {
                  uint32_t low = 0;
                  if (not text.starts_with("\\u")) return false;
                  text.remove_prefix(2);
                  if (not hex(low) or low < 0xdc00 or low > 0xdfff) return false;
                  code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
               }
               else if (code >= 0xdc00 and code <= 0xdfff) return false;

               std::array<int8_t, 4> encoded;
               auto count = UTF8WriteValue(int(code), encoded);
               if (not count) return false;
               Value.append((const char *)encoded.data(), size_t(count));
               break;
            }

            default: return false;
         }
      }

      return false;
   }
};

//********************************************************************************************************************
// Parses a stun:, turn: or turns: URI.  The only query parameter supported is transport, which selects TCP or UDP for
// TURN.  The host must be a DNS name or a bracketed IPv6 literal.

ERR url(std::string_view Text, IceServer &Server)
{
   if (Text.size() > 2048) return ERR::BufferOverflow;

   if (Text.starts_with("stun:")) {
      Server.transport = ServerTransport::STUN;
      Text.remove_prefix(5);
   }
   else if (Text.starts_with("turn:")) {
      Server.transport = ServerTransport::UDP;
      Text.remove_prefix(5);
   }
   else if (Text.starts_with("turns:")) {
      Server.transport = ServerTransport::TLS;
      Server.port = 5349;
      Text.remove_prefix(6);
   }
   else return ERR::NoSupport;

   auto query = Text.find('?');
   if (query != std::string_view::npos) {
      auto parameter = Text.substr(query + 1);
      Text = Text.substr(0, query);
      if (Server.transport IS ServerTransport::STUN) return ERR::InvalidValue;

      if (parameter IS "transport=tcp") {
         if (Server.transport != ServerTransport::TLS) Server.transport = ServerTransport::TCP;
      }
      else if (parameter IS "transport=udp") {
         if (Server.transport IS ServerTransport::TLS) return ERR::NoSupport;
      }
      else return ERR::NoSupport;
   }

   if (Text.empty() or Text.find_first_of("/@#?\\ \t\r\n") != std::string_view::npos) return ERR::InvalidValue;

   std::string_view host, port;
   if (Text.starts_with('[')) {
      auto close = Text.find(']');
      if (close IS std::string_view::npos) return ERR::InvalidValue;

      host = Text.substr(1, close - 1);
      ice::Endpoint parsed;
      if (not ice::parse_numeric_endpoint("[" + std::string(host) + "]:1", parsed)) return ERR::InvalidValue;

      if (close + 1 < Text.size()) {
         if (Text[close + 1] != ':') return ERR::InvalidValue;
         port = Text.substr(close + 2);
      }
   }
   else {
      auto colon = Text.find(':');
      host = Text.substr(0, colon);
      if (colon != std::string_view::npos) port = Text.substr(colon + 1);
      if (host.find_first_of("[]") != std::string_view::npos) return ERR::InvalidValue;
      for (char ch : host) if (not ((ch >= 'a' and ch <= 'z') or (ch >= 'A' and ch <= 'Z') or
          (ch >= '0' and ch <= '9') or ch IS '.' or ch IS '-')) return ERR::InvalidValue;
   }

   if (host.empty() or host.size() > 253 or (port.empty() and Text.ends_with(':'))) return ERR::InvalidValue;

   if (not port.empty()) {
      uint32_t number = 0;
      auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
      if (error != std::errc() or end != port.data() + port.size() or not number or number > 65535) {
         return ERR::InvalidValue;
      }
      Server.port = uint16_t(number);
   }

   Server.host = host;
   return ERR::Okay;
}

} // namespace

//********************************************************************************************************************
// Parses the ICE server configuration.  At most eight servers are permitted.  TURN servers require credentials and
// STUN servers must not have them.  Result is only updated if the entire configuration is valid.

ERR parse_ice_servers(std::string_view Text, std::vector<IceServer> &Result)
{
   if (Text.size() > 16384) return ERR::BufferOverflow;
   if (not utf8(Text) or Text.find('\0') != std::string_view::npos) return ERR::InvalidValue;

   std::vector<IceServer> parsed;
   if (Text.empty()) {
      Result.clear();
      return ERR::Okay;
   }

   Reader reader{Text};
   if (not reader.take('[')) return ERR::InvalidValue;

   if (not reader.take(']')) for (;;) {
      if (parsed.size() >= 8) return ERR::BufferOverflow;
      if (not reader.take('{')) return ERR::InvalidValue;

      IceServer server;
      std::string address;
      std::set<std::string> keys;
      if (reader.take('}')) return ERR::InvalidValue;

      for (;;) {
         std::string key, value;
         if (not reader.string(key) or not reader.take(':') or not reader.string(value) or
             not keys.insert(key).second) return ERR::InvalidValue;

         if (key IS "url") address = std::move(value);
         else if (key IS "username") server.username = std::move(value);
         else if (key IS "credential") server.credential = std::move(value);
         else return ERR::InvalidValue;

         if (reader.take('}')) break;
         if (not reader.take(',')) return ERR::InvalidValue;
      }

      if (server.username.size() > 512 or server.credential.size() > 1024) return ERR::BufferOverflow;

      auto error = url(address, server);
      if (error != ERR::Okay) return error;

      if (server.transport IS ServerTransport::STUN) {
         if (not server.username.empty() or not server.credential.empty()) return ERR::NoSupport;
      }
      else if (server.username.empty() or server.credential.empty()) return ERR::InvalidValue;

      parsed.push_back(std::move(server));
      if (reader.take(']')) break;
      if (not reader.take(',')) return ERR::InvalidValue;
   }

   reader.space();
   if (not reader.text.empty()) return ERR::InvalidValue;

   Result = std::move(parsed);
   return ERR::Okay;
}

} // namespace rtc
