// WebSocket opening handshake helpers.  See ws_handshake.h.

#include <kotuku/main.h>
#include <kotuku/strings.hpp>
#include <kotuku/modules/crypto.h>

#include <array>

#include "ws_handshake.h"

namespace ws {

//********************************************************************************************************************

ERR generate_key(std::string &Key)
{
   Key.clear();

   std::array<int8_t, KEY_NONCE_SIZE> nonce;
   if (auto error = crypto::RandomBytes(nonce); error != ERR::Okay) return error;

   std::string encoded(KEY_LENGTH, '\0');
   int length = 0;
   if (auto error = crypto::Base64Encode(nonce, CC::STANDARD,
       std::span<int8_t>((int8_t *)encoded.data(), encoded.size()), &length); error != ERR::Okay) return error;
   if (length != int(KEY_LENGTH)) return ERR::Failed;

   Key = std::move(encoded);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR compute_accept_key(std::string_view Key, std::string &Accept)
{
   Accept.clear();

   std::string input;
   input.reserve(Key.size() + HANDSHAKE_GUID.size());
   input.append(Key);
   input.append(HANDSHAKE_GUID);

   std::array<int8_t, 20> digest;
   if (auto error = crypto::Hash(HASH::SHA1, std::span<const int8_t>((const int8_t *)input.data(), input.size()),
       digest); error != ERR::Okay) return error;

   std::string encoded(ACCEPT_LENGTH, '\0');
   int length = 0;
   if (auto error = crypto::Base64Encode(digest, CC::STANDARD,
       std::span<int8_t>((int8_t *)encoded.data(), encoded.size()), &length); error != ERR::Okay) return error;
   if (length != int(ACCEPT_LENGTH)) return ERR::Failed;

   Accept = std::move(encoded);
   return ERR::Okay;
}

//********************************************************************************************************************

bool valid_key(std::string_view Key)
{
   if (Key.size() != KEY_LENGTH) return false;

   std::array<int8_t, KEY_NONCE_SIZE> decoded; // A longer decoding fails with LowCapacity
   int length = 0;
   if (crypto::Base64Decode(std::span<const int8_t>((const int8_t *)Key.data(), Key.size()), CC::STANDARD,
       decoded, &length) != ERR::Okay) return false;
   return length IS int(KEY_NONCE_SIZE);
}

//********************************************************************************************************************

bool valid_version(std::string_view Value)
{
   return trim_ows(Value) IS WEBSOCKET_VERSION;
}

//********************************************************************************************************************

bool is_token(std::string_view Value)
{
   if (Value.empty()) return false;

   for (auto ch : Value) {
      if (((ch >= 'a') and (ch <= 'z')) or ((ch >= 'A') and (ch <= 'Z')) or ((ch >= '0') and (ch <= '9'))) continue;

      switch (ch) {
         case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.':
         case '^': case '_': case '`': case '|': case '~':
            continue;
         default:
            return false;
      }
   }
   return true;
}

//********************************************************************************************************************

std::string_view trim_ows(std::string_view Value)
{
   while ((not Value.empty()) and ((Value.front() IS ' ') or (Value.front() IS '\t'))) Value.remove_prefix(1);
   while ((not Value.empty()) and ((Value.back() IS ' ') or (Value.back() IS '\t'))) Value.remove_suffix(1);
   return Value;
}

//********************************************************************************************************************

bool parse_token_list(std::string_view Value, std::vector<std::string_view> &Tokens)
{
   Tokens.clear();

   while (true) {
      auto comma = Value.find(',');
      auto element = trim_ows(Value.substr(0, comma));
      if (not element.empty()) {
         if (not is_token(element)) {
            Tokens.clear();
            return false;
         }
         Tokens.push_back(element);
      }
      if (comma IS std::string_view::npos) break;
      Value.remove_prefix(comma + 1);
   }
   return true;
}

//********************************************************************************************************************

bool has_token(std::string_view Value, std::string_view Token)
{
   std::vector<std::string_view> tokens;
   if (not parse_token_list(Value, tokens)) return false;

   for (auto token : tokens) {
      if (kt::iequals(token, Token)) return true;
   }
   return false;
}

//********************************************************************************************************************

static bool printable_ascii(std::string_view Value)
{
   for (auto ch : Value) {
      if ((uint8_t(ch) <= 0x20) or (uint8_t(ch) >= 0x7f)) return false;
   }
   return true;
}

bool parse_location(std::string_view URI, Location &Result)
{
   Result = Location();

   if (not printable_ascii(URI)) return false;
   if (URI.find('#') != std::string_view::npos) return false; // RFC 6455 section 3: fragments are not permitted

   bool secure;
   if ((URI.size() >= 5) and kt::iequals(URI.substr(0, 5), "ws://")) {
      secure = false;
      URI.remove_prefix(5);
   }
   else if ((URI.size() >= 6) and kt::iequals(URI.substr(0, 6), "wss://")) {
      secure = true;
      URI.remove_prefix(6);
   }
   else return false;

   auto authority_end = URI.find_first_of("/?");
   auto authority = URI.substr(0, authority_end);
   auto target = (authority_end IS std::string_view::npos) ? std::string_view() : URI.substr(authority_end);

   if (authority.find('@') != std::string_view::npos) return false;

   std::string_view host, port_text;
   bool has_port = false;

   if (authority.starts_with('[')) {
      auto close = authority.find(']');
      if (close IS std::string_view::npos) return false;
      host = authority.substr(1, close - 1);
      if (host.empty()) return false;
      for (auto ch : host) {
         if (not (((ch >= '0') and (ch <= '9')) or ((ch >= 'a') and (ch <= 'f')) or ((ch >= 'A') and (ch <= 'F')) or
               (ch IS ':') or (ch IS '.'))) return false;
      }

      auto rest = authority.substr(close + 1);
      if (not rest.empty()) {
         if (rest.front() != ':') return false;
         port_text = rest.substr(1);
         has_port = true;
      }
   }
   else {
      auto colon = authority.find(':');
      host = authority.substr(0, colon);
      if (colon != std::string_view::npos) {
         port_text = authority.substr(colon + 1);
         has_port = true;
      }
      if (host.empty() or (host.find_first_of("[]") != std::string_view::npos)) return false;
   }

   int port = secure ? 443 : 80;
   if (has_port) {
      if (port_text.empty() or (port_text.size() > 5)) return false;
      port = 0;
      for (auto ch : port_text) {
         if ((ch < '0') or (ch > '9')) return false;
         port = (port * 10) + (ch - '0');
      }
      if ((port < 1) or (port > 65535)) return false;
   }

   Result.host.assign(host);
   Result.port = port;
   Result.secure = secure;
   if (target.empty()) Result.target = "/";
   else if (target.front() IS '?') Result.target = "/" + std::string(target);
   else Result.target.assign(target);
   return true;
}

//********************************************************************************************************************

bool parse_header_fields(std::string_view Headers, std::vector<HeaderField> &Fields)
{
   Fields.clear();

   while (not Headers.empty()) {
      auto eol = Headers.find('\n');
      auto line = Headers.substr(0, eol);
      Headers = (eol IS std::string_view::npos) ? std::string_view() : Headers.substr(eol + 1);
      if ((not line.empty()) and (line.back() IS '\r')) line.remove_suffix(1);
      if (line.empty()) continue;

      auto colon = line.find(':');
      if ((colon IS std::string_view::npos) or (not is_token(line.substr(0, colon)))) {
         Fields.clear();
         return false;
      }
      Fields.push_back({ line.substr(0, colon), trim_ows(line.substr(colon + 1)) });
   }
   return true;
}

//********************************************************************************************************************

std::string_view validate_response(std::span<const HeaderField> Fields, std::string_view ExpectedAccept,
   std::span<const std::string_view> Requested, std::string_view &Selected)
{
   Selected = std::string_view();

   int accept_count = 0, protocol_count = 0;
   std::string_view accept, protocol;

   for (const auto &field : Fields) {
      if (kt::iequals(field.name, "Sec-WebSocket-Accept")) {
         accept_count++;
         accept = field.value;
      }
      else if (kt::iequals(field.name, "Sec-WebSocket-Protocol")) {
         protocol_count++;
         protocol = field.value;
      }
      else if (kt::iequals(field.name, "Sec-WebSocket-Extensions")) {
         return "Server selected an extension that was not requested";
      }
   }

   if (accept_count IS 0) return "Sec-WebSocket-Accept is missing";
   if (accept_count > 1) return "Sec-WebSocket-Accept is repeated";
   if (accept != ExpectedAccept) return "Sec-WebSocket-Accept does not match the key";

   if (protocol_count > 1) return "Sec-WebSocket-Protocol is repeated";
   if (protocol_count IS 1) {
      for (auto requested : Requested) {
         if (requested IS protocol) {
            Selected = protocol;
            return std::string_view();
         }
      }
      return "Server selected a subprotocol that was not requested";
   }

   return std::string_view();
}

//********************************************************************************************************************

size_t find_request_end(std::string_view Data)
{
   for (size_t i = Data.find('\n'); i != std::string_view::npos; i = Data.find('\n', i + 1)) {
      if ((i + 1 < Data.size()) and (Data[i + 1] IS '\n')) return i + 2;
      if ((i + 2 < Data.size()) and (Data[i + 1] IS '\r') and (Data[i + 2] IS '\n')) return i + 3;
   }
   return 0;
}

//********************************************************************************************************************

static bool valid_field_value(std::string_view Value)
{
   for (auto ch : Value) {
      if (((uint8_t(ch) < 0x20) and (ch != '\t')) or (uint8_t(ch) IS 0x7f)) return false;
   }
   return true;
}

int parse_request(std::string_view Head, Request &Result, std::string_view &Reason)
{
   Result = Request();
   Reason = std::string_view();

   auto bad = [&](std::string_view Description) {
      Reason = Description;
      return STATUS_BAD_REQUEST;
   };

   // Request line: GET SP origin-form SP HTTP/1.1

   auto eol = Head.find('\n');
   if (eol IS std::string_view::npos) return bad("Request line is incomplete");
   auto line = Head.substr(0, eol);
   if ((not line.empty()) and (line.back() IS '\r')) line.remove_suffix(1);
   auto headers = Head.substr(eol + 1);

   auto first_space = line.find(' ');
   auto last_space = line.rfind(' ');
   if ((first_space IS std::string_view::npos) or (first_space IS last_space)) return bad("Request line is malformed");

   auto method  = line.substr(0, first_space);
   auto target  = line.substr(first_space + 1, last_space - first_space - 1);
   auto version = line.substr(last_space + 1);

   if (method != "GET") return bad("Request method is not GET");
   if (version != "HTTP/1.1") return bad("Request is not HTTP/1.1");
   if (target.empty() or (target.front() != '/')) return bad("Request target is not an absolute path");
   for (auto ch : target) {
      if ((uint8_t(ch) <= 0x20) or (uint8_t(ch) >= 0x7f) or (ch IS '#')) return bad("Request target is malformed");
   }

   Result.target = target;
   auto question = target.find('?');
   Result.path = target.substr(0, question);
   if (question != std::string_view::npos) Result.query = target.substr(question + 1);

   if (not parse_header_fields(headers, Result.fields)) return bad("Request header fields are malformed");

   int host_count = 0, version_count = 0, key_count = 0;
   bool upgrade = false, connection = false;
   std::string_view version_value;

   for (const auto &field : Result.fields) {
      if (not valid_field_value(field.value)) return bad("Request header field contains a control character");

      if (kt::iequals(field.name, "Host")) {
         host_count++;
         Result.host = field.value;
      }
      else if (kt::iequals(field.name, "Upgrade")) {
         if (has_token(field.value, "websocket")) upgrade = true;
      }
      else if (kt::iequals(field.name, "Connection")) {
         if (has_token(field.value, "Upgrade")) connection = true;
      }
      else if (kt::iequals(field.name, "Sec-WebSocket-Version")) {
         version_count++;
         version_value = field.value;
      }
      else if (kt::iequals(field.name, "Sec-WebSocket-Key")) {
         key_count++;
         Result.key = field.value;
      }
      else if (kt::iequals(field.name, "Sec-WebSocket-Protocol")) {
         std::vector<std::string_view> tokens;
         if (not parse_token_list(field.value, tokens)) return bad("Sec-WebSocket-Protocol is malformed");
         Result.protocols.insert(Result.protocols.end(), tokens.begin(), tokens.end());
      }
      else if (kt::iequals(field.name, "Origin")) {
         if (Result.origin.empty()) Result.origin = field.value;
      }
      else if (kt::iequals(field.name, "Transfer-Encoding")) {
         return bad("Request carries a body");
      }
      else if (kt::iequals(field.name, "Content-Length")) {
         if (field.value != "0") return bad("Request carries a body");
      }
   }

   if ((host_count != 1) or Result.host.empty()) return bad("Host is missing or repeated");
   if (not upgrade) return bad("Upgrade does not name websocket");
   if (not connection) return bad("Connection does not include Upgrade");
   if (version_count != 1) return bad("Sec-WebSocket-Version is missing or repeated");
   if (not valid_version(version_value)) {
      Reason = "Sec-WebSocket-Version is not supported";
      return STATUS_UPGRADE_REQUIRED;
   }
   if ((key_count != 1) or (not valid_key(Result.key))) return bad("Sec-WebSocket-Key is missing or invalid");

   return 0;
}

//********************************************************************************************************************

std::string_view select_protocol(std::span<const std::string> Supported, std::span<const std::string_view> Requested)
{
   for (const auto &supported : Supported) {
      for (auto requested : Requested) {
         if (requested IS supported) return supported;
      }
   }
   return std::string_view();
}

//********************************************************************************************************************

std::string switching_response(std::string_view Accept, std::string_view Protocol)
{
   std::string response("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: ");
   response.append(Accept);
   response.append("\r\n");
   if (not Protocol.empty()) {
      response.append("Sec-WebSocket-Protocol: ");
      response.append(Protocol);
      response.append("\r\n");
   }
   response.append("\r\n");
   return response;
}

//********************************************************************************************************************

std::string rejection_response(int Status)
{
   std::string_view text;
   switch (Status) {
      case STATUS_BAD_REQUEST:      text = "Bad Request"; break;
      case STATUS_FORBIDDEN:        text = "Forbidden"; break;
      case STATUS_NOT_FOUND:        text = "Not Found"; break;
      case STATUS_UPGRADE_REQUIRED: text = "Upgrade Required"; break;
      case STATUS_HEADERS_TOO_BIG:  text = "Request Header Fields Too Large"; break;
      default:                      text = "Error"; break;
   }

   std::string response("HTTP/1.1 ");
   response.append(std::to_string(Status));
   response.append(" ");
   response.append(text);
   response.append("\r\nConnection: close\r\nContent-Length: 0\r\n");
   if (Status IS STATUS_UPGRADE_REQUIRED) {
      response.append("Sec-WebSocket-Version: ");
      response.append(WEBSOCKET_VERSION);
      response.append("\r\n");
   }
   response.append("\r\n");
   return response;
}

} // namespace ws
