// Backstage-local WebSocket support for the /streaming endpoint.

static constexpr size_t MAX_WEBSOCKET_MESSAGE = 64 * 1024;
static constexpr std::string_view WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

enum class WebSocketFrameState {
   INCOMPLETE,
   COMPLETE,
   PROTOCOL_ERROR,
   TOO_LARGE
};

struct BackstageWebSocketFrame {
   bool fin = false;
   uint8_t opcode = 0;
   std::vector<uint8_t> payload;
   size_t consumed = 0;
};

struct BackstageWebSocketSession {
   OBJECTID client_uid = 0;
   uint64_t sequence = 0;
   std::string buffer;
   std::unordered_set<std::string> topics;
};

static std::mutex glWebSocketLock;
static std::unordered_map<OBJECTID, BackstageWebSocketSession> glWebSocketSessions;

//********************************************************************************************************************

static uint32_t websocket_rotate_left(uint32_t Value, int Bits)
{
   return (Value << Bits) | (Value >> (32 - Bits));
}

//********************************************************************************************************************

static std::array<uint8_t, 20> websocket_sha1(std::string_view Input)
{
   std::vector<uint8_t> message(Input.begin(), Input.end());
   uint64_t bit_length = uint64_t(Input.size()) * 8;

   message.push_back(0x80);
   while ((message.size() % 64) != 56) message.push_back(0);

   for (int i=7; i >= 0; i--) message.push_back(uint8_t((bit_length >> (i * 8)) & 0xff));

   uint32_t h0 = 0x67452301;
   uint32_t h1 = 0xefcdab89;
   uint32_t h2 = 0x98badcfe;
   uint32_t h3 = 0x10325476;
   uint32_t h4 = 0xc3d2e1f0;

   for (size_t offset=0; offset < message.size(); offset += 64) {
      std::array<uint32_t, 80> words = {};

      for (int i=0; i < 16; i++) {
         size_t pos = offset + (size_t(i) * 4);
         words[i] = (uint32_t(message[pos]) << 24) | (uint32_t(message[pos + 1]) << 16) |
            (uint32_t(message[pos + 2]) << 8) | uint32_t(message[pos + 3]);
      }

      for (int i=16; i < 80; i++) {
         words[i] = websocket_rotate_left(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
      }

      uint32_t a = h0;
      uint32_t b = h1;
      uint32_t c = h2;
      uint32_t d = h3;
      uint32_t e = h4;

      for (int i=0; i < 80; i++) {
         uint32_t f = 0;
         uint32_t k = 0;

         if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999;
         }
         else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
         }
         else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
         }
         else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
         }

         uint32_t temp = websocket_rotate_left(a, 5) + f + e + k + words[i];
         e = d;
         d = c;
         c = websocket_rotate_left(b, 30);
         b = a;
         a = temp;
      }

      h0 += a;
      h1 += b;
      h2 += c;
      h3 += d;
      h4 += e;
   }

   std::array<uint8_t, 20> digest = {};
   std::array<uint32_t, 5> state = { h0, h1, h2, h3, h4 };

   for (size_t i=0; i < state.size(); i++) {
      digest[(i * 4)]     = uint8_t((state[i] >> 24) & 0xff);
      digest[(i * 4) + 1] = uint8_t((state[i] >> 16) & 0xff);
      digest[(i * 4) + 2] = uint8_t((state[i] >> 8) & 0xff);
      digest[(i * 4) + 3] = uint8_t(state[i] & 0xff);
   }

   return digest;
}

//********************************************************************************************************************

static std::string websocket_base64(std::span<const uint8_t> Input)
{
   std::array<char, 128> output = {};
   kt::BASE64ENCODE state;

   int written = kt::Base64Encode(&state, Input.data(), int(Input.size()), output.data(), int(output.size()));
   if (written <= 0) return {};

   int final = kt::Base64Encode(&state, nullptr, 0, output.data() + written, int(output.size()) - written);
   if (final <= 0) return {};

   std::string result(output.data(), size_t(written + final));
   while ((not result.empty()) and ((result.back() IS '\0') or (result.back() IS '\n') or (result.back() IS '\r'))) {
      result.pop_back();
   }

   return result;
}

//********************************************************************************************************************

static std::string websocket_accept_key(std::string_view Key)
{
   std::string material;
   material.reserve(Key.size() + WEBSOCKET_GUID.size());
   material.append(Key);
   material.append(WEBSOCKET_GUID);

   auto digest = websocket_sha1(material);
   return websocket_base64(digest);
}

//********************************************************************************************************************

static bool websocket_header_contains_token(std::string_view Value, std::string_view Token)
{
   size_t start = 0;

   while (start <= Value.size()) {
      auto end = Value.find(',', start);
      if (end IS std::string_view::npos) end = Value.size();

      auto item = HttpHeaders::trim_value(Value.substr(start, end - start));
      if (kt::iequals(item, Token)) return true;

      if (end IS Value.size()) break;
      start = end + 1;
   }

   return false;
}

//********************************************************************************************************************

static bool websocket_key_is_valid(std::string_view Key)
{
   if (Key.empty()) return false;

   std::array<uint8_t, 32> decoded = {};
   kt::BASE64DECODE state;
   int written = 0;

   std::string key_copy(Key);
   if (kt::Base64Decode(&state, key_copy.c_str(), int(key_copy.size()), decoded.data(), &written) != ERR::Okay) {
      return false;
   }

   return written IS 16;
}

//********************************************************************************************************************

static bool websocket_origin_allowed(std::string_view Origin)
{
   if (Origin.empty()) return true;

   if (not kt::startswith("http://", Origin)) return false;

   std::string_view authority = Origin.substr(7);
   if (authority.empty()) return false;
   if (not (authority.find_first_of("/?#") IS std::string_view::npos)) return false;

   std::string_view host;
   std::string_view port;

   if (authority.front() IS '[') {
      size_t end = authority.find(']');
      if (end IS std::string_view::npos) return false;

      host = authority.substr(0, end + 1);
      if (end + 1 < authority.size()) {
         if (not (authority[end + 1] IS ':')) return false;
         port = authority.substr(end + 2);
      }
   }
   else {
      size_t colon = authority.find(':');
      if (colon IS std::string_view::npos) host = authority;
      else {
         host = authority.substr(0, colon);
         port = authority.substr(colon + 1);
      }
   }

   if ((not (host.compare("127.0.0.1") IS 0)) and (not kt::iequals(host, "localhost")) and
      (not (host.compare("[::1]") IS 0))) {
      return false;
   }

   if (port.empty()) return true;

   int port_value = 0;
   for (char ch : port) {
      if (ch < '0' or ch > '9') return false;
      port_value = (port_value * 10) + int(ch - '0');
      if (port_value > 65535) return false;
   }

   return port_value > 0;
}

//********************************************************************************************************************

static bool websocket_request_targets_streaming(const BackstageHttpRequest &Request)
{
   return (Request.line.method.compare("GET") IS 0) and (Request.line.path.compare("/streaming") IS 0);
}

//********************************************************************************************************************

static bool websocket_request_is_upgrade(const BackstageHttpRequest &Request)
{
   HttpHeaders headers(Request.headers);
   return kt::iequals(headers.value("Upgrade"), "websocket") and
      websocket_header_contains_token(headers.value("Connection"), "Upgrade");
}

//********************************************************************************************************************

static bool backstage_websocket_has_session(OBJECTID ClientID)
{
   std::lock_guard<std::mutex> lock(glWebSocketLock);
   return glWebSocketSessions.find(ClientID) != glWebSocketSessions.end();
}

//********************************************************************************************************************

static void release_backstage_websocket(OBJECTID ClientID)
{
   std::lock_guard<std::mutex> lock(glWebSocketLock);
   glWebSocketSessions.erase(ClientID);
}

//********************************************************************************************************************

static void release_backstage_websockets()
{
   std::lock_guard<std::mutex> lock(glWebSocketLock);
   glWebSocketSessions.clear();
}

//********************************************************************************************************************

static ERR websocket_write_raw(objClientSocket *Client, const std::string &Data)
{
   int result = 0;
   auto error = Client->write(Data.data(), int(Data.size()), &result);
   if (error != ERR::Okay) return error;
   if (result != int(Data.size())) return ERR::BufferOverflow;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR websocket_write_frame(objClientSocket *Client, uint8_t Opcode, std::string_view Payload)
{
   std::string frame;
   frame.reserve(Payload.size() + 16);
   frame.push_back(char(0x80 | Opcode));

   if (Payload.size() < 126) {
      frame.push_back(char(Payload.size()));
   }
   else if (Payload.size() <= 0xffff) {
      frame.push_back(char(126));
      frame.push_back(char((Payload.size() >> 8) & 0xff));
      frame.push_back(char(Payload.size() & 0xff));
   }
   else {
      frame.push_back(char(127));
      uint64_t length = uint64_t(Payload.size());
      for (int i=7; i >= 0; i--) frame.push_back(char((length >> (i * 8)) & 0xff));
   }

   frame.append(Payload);
   return websocket_write_raw(Client, frame);
}

//********************************************************************************************************************

static ERR websocket_write_text(objClientSocket *Client, std::string_view Text)
{
   return websocket_write_frame(Client, 0x01, Text);
}

//********************************************************************************************************************

static ERR websocket_write_close(objClientSocket *Client, uint16_t Code, std::string_view Reason = {})
{
   std::string payload;
   payload.reserve(2 + Reason.size());
   payload.push_back(char((Code >> 8) & 0xff));
   payload.push_back(char(Code & 0xff));
   payload.append(Reason);

   auto error = websocket_write_frame(Client, 0x08, payload);
   Client->deactivate();
   return error;
}

//********************************************************************************************************************

static WebSocketFrameState websocket_parse_frame(std::string_view Buffer, BackstageWebSocketFrame &Frame)
{
   if (Buffer.size() < 2) return WebSocketFrameState::INCOMPLETE;

   uint8_t first = uint8_t(Buffer[0]);
   uint8_t second = uint8_t(Buffer[1]);
   bool masked = (second & 0x80) != 0;
   uint64_t payload_length = second & 0x7f;
   size_t offset = 2;

   Frame.fin = (first & 0x80) != 0;
   Frame.opcode = first & 0x0f;

   if ((first & 0x70) != 0) return WebSocketFrameState::PROTOCOL_ERROR;
   if (not masked) return WebSocketFrameState::PROTOCOL_ERROR;

   if (payload_length IS 126) {
      if (Buffer.size() < 4) return WebSocketFrameState::INCOMPLETE;
      payload_length = (uint64_t(uint8_t(Buffer[2])) << 8) | uint64_t(uint8_t(Buffer[3]));
      offset = 4;
   }
   else if (payload_length IS 127) {
      if (Buffer.size() < 10) return WebSocketFrameState::INCOMPLETE;
      payload_length = 0;
      for (int i=0; i < 8; i++) payload_length = (payload_length << 8) | uint64_t(uint8_t(Buffer[2 + i]));
      offset = 10;
   }

   if (payload_length > MAX_WEBSOCKET_MESSAGE) return WebSocketFrameState::TOO_LARGE;
   if (Buffer.size() < offset + 4 + payload_length) return WebSocketFrameState::INCOMPLETE;

   if ((Frame.opcode >= 0x08) and ((not Frame.fin) or (payload_length > 125))) {
      return WebSocketFrameState::PROTOCOL_ERROR;
   }

   const uint8_t *mask = (const uint8_t *)Buffer.data() + offset;
   offset += 4;

   Frame.payload.resize(size_t(payload_length));
   for (size_t i=0; i < Frame.payload.size(); i++) {
      Frame.payload[i] = uint8_t(Buffer[offset + i]) ^ mask[i % 4];
   }

   Frame.consumed = offset + size_t(payload_length);
   return WebSocketFrameState::COMPLETE;
}

//********************************************************************************************************************

static void websocket_append_json_string(std::string &Output, std::string_view Value)
{
   static constexpr char hex[] = "0123456789abcdef";

   Output.push_back('"');

   for (char c : Value) {
      auto ch = (unsigned char)c;

      if (c IS '"') Output.append("\\\"");
      else if (c IS '\\') Output.append("\\\\");
      else if (c IS '\b') Output.append("\\b");
      else if (c IS '\f') Output.append("\\f");
      else if (c IS '\n') Output.append("\\n");
      else if (c IS '\r') Output.append("\\r");
      else if (c IS '\t') Output.append("\\t");
      else if (ch < 0x20) {
         Output.append("\\u00");
         Output.push_back(hex[ch >> 4]);
         Output.push_back(hex[ch & 0x0f]);
      }
      else Output.push_back(c);
   }

   Output.push_back('"');
}

//********************************************************************************************************************

static std::string websocket_json_field(std::string_view Name, std::string_view Value)
{
   std::string result;
   websocket_append_json_string(result, Name);
   result.push_back(':');
   websocket_append_json_string(result, Value);
   return result;
}

//********************************************************************************************************************

static uint64_t websocket_next_sequence(OBJECTID ClientID)
{
   std::lock_guard<std::mutex> lock(glWebSocketLock);

   auto it = glWebSocketSessions.find(ClientID);
   if (it IS glWebSocketSessions.end()) return 0;

   it->second.sequence++;
   return it->second.sequence;
}

//********************************************************************************************************************

static std::string websocket_make_event(OBJECTID ClientID, std::string_view Topic, std::string_view Event,
   std::string_view Data)
{
   std::string msg;
   msg.reserve(96 + Topic.size() + Event.size() + Data.size());
   msg.append("{\"type\":\"event\",");
   msg.append(websocket_json_field("topic", Topic));
   msg.append(",");
   msg.append(websocket_json_field("event", Event));
   msg.append(",\"seq\":");
   msg.append(std::to_string(websocket_next_sequence(ClientID)));
   msg.append(",\"data\":");
   msg.append(Data);
   msg.push_back('}');
   return msg;
}

//********************************************************************************************************************

static ERR websocket_send_system_event(objClientSocket *Client, std::string_view Event, std::string_view Data)
{
   return websocket_write_text(Client, websocket_make_event(Client->UID, "system", Event, Data));
}

//********************************************************************************************************************

static ERR websocket_send_log_stub(objClientSocket *Client)
{
   return websocket_write_text(Client, websocket_make_event(Client->UID, "log", "stub",
      "{\"message\":\"Log streaming is not connected to the core log yet.\"}"));
}

//********************************************************************************************************************

static std::string websocket_extract_json_string(std::string_view Message, std::string_view Name)
{
   std::string pattern;
   pattern.reserve(Name.size() + 4);
   pattern.append("\"");
   pattern.append(Name);
   pattern.append("\"");

   auto name_pos = Message.find(pattern);
   if (name_pos IS std::string_view::npos) return {};

   auto colon = Message.find(':', name_pos + pattern.size());
   if (colon IS std::string_view::npos) return {};

   auto quote = Message.find('"', colon + 1);
   if (quote IS std::string_view::npos) return {};

   std::string value;

   for (size_t i=quote + 1; i < Message.size(); i++) {
      char c = Message[i];
      if (c IS '\\') {
         if (i + 1 >= Message.size()) return {};
         value.push_back(Message[++i]);
      }
      else if (c IS '"') return value;
      else value.push_back(c);
   }

   return {};
}

//********************************************************************************************************************

static std::vector<std::string> websocket_extract_topics(std::string_view Message)
{
   std::vector<std::string> topics;
   auto topics_pos = Message.find("\"topics\"");
   if (topics_pos IS std::string_view::npos) return topics;

   auto open = Message.find('[', topics_pos);
   auto close = Message.find(']', topics_pos);
   if ((open IS std::string_view::npos) or (close IS std::string_view::npos) or (close <= open)) return topics;

   auto list = Message.substr(open + 1, close - open - 1);
   size_t pos = 0;

   while (pos < list.size()) {
      auto quote = list.find('"', pos);
      if (quote IS std::string_view::npos) break;

      std::string value;

      for (size_t i=quote + 1; i < list.size(); i++) {
         char c = list[i];
         if (c IS '\\') {
            if (i + 1 >= list.size()) break;
            value.push_back(list[++i]);
         }
         else if (c IS '"') {
            topics.push_back(value);
            pos = i + 1;
            break;
         }
         else value.push_back(c);

         if (i + 1 >= list.size()) pos = list.size();
      }
   }

   return topics;
}

//********************************************************************************************************************

static bool websocket_topic_is_known(const std::string &Topic)
{
   return kt::iequals(Topic, "system") or kt::iequals(Topic, "log");
}

//********************************************************************************************************************

static std::string websocket_make_ack(std::string_view ID, std::string_view Command,
   const std::vector<std::string> &Topics)
{
   std::string message;
   message.append("{\"type\":\"ack\"");

   if (not ID.empty()) {
      message.push_back(',');
      message.append(websocket_json_field("id", ID));
   }

   message.push_back(',');
   message.append(websocket_json_field("command", Command));
   message.append(",\"topics\":[");

   for (size_t i=0; i < Topics.size(); i++) {
      if (i > 0) message.push_back(',');
      websocket_append_json_string(message, Topics[i]);
   }

   message.append("]}");
   return message;
}

//********************************************************************************************************************

static std::string websocket_make_error(std::string_view ID, std::string_view Code, std::string_view Message)
{
   std::string response;
   response.append("{\"type\":\"error\"");

   if (not ID.empty()) {
      response.push_back(',');
      response.append(websocket_json_field("id", ID));
   }

   response.push_back(',');
   response.append(websocket_json_field("code", Code));
   response.push_back(',');
   response.append(websocket_json_field("message", Message));
   response.push_back('}');
   return response;
}

//********************************************************************************************************************

static ERR websocket_handle_subscribe(objClientSocket *Client, std::string_view ID, std::string_view Command,
   const std::vector<std::string> &Topics)
{
   std::vector<std::string> accepted;

   for (auto &topic : Topics) {
      if (not websocket_topic_is_known(topic)) {
         return websocket_write_text(Client, websocket_make_error(ID, "unknown_topic", "Unknown streaming topic."));
      }
   }

   {
      std::lock_guard<std::mutex> lock(glWebSocketLock);
      auto it = glWebSocketSessions.find(Client->UID);
      if (it IS glWebSocketSessions.end()) return ERR::Disconnected;

      for (auto &topic : Topics) {
         if (kt::iequals(Command, "subscribe")) {
            it->second.topics.insert(topic);
            accepted.push_back(topic);
         }
         else if (kt::iequals(Command, "unsubscribe")) {
            it->second.topics.erase(topic);
            accepted.push_back(topic);
         }
      }
   }

   auto error = websocket_write_text(Client, websocket_make_ack(ID, Command, accepted));
   if (error != ERR::Okay) return error;

   if (kt::iequals(Command, "subscribe")) {
      for (auto &topic : accepted) {
         if (kt::iequals(topic, "system")) {
            if (auto system_error = websocket_send_system_event(Client, "subscribed", "{\"topic\":\"system\"}");
                  system_error != ERR::Okay) return system_error;
         }
         else if (kt::iequals(topic, "log")) {
            if (auto log_error = websocket_send_log_stub(Client); log_error != ERR::Okay) return log_error;
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR websocket_handle_text(objClientSocket *Client, std::string_view Message)
{
   auto id = websocket_extract_json_string(Message, "id");
   auto type = websocket_extract_json_string(Message, "type");

   if (kt::iequals(type, "subscribe") or kt::iequals(type, "unsubscribe")) {
      auto topics = websocket_extract_topics(Message);
      if (topics.empty()) {
         return websocket_write_text(Client, websocket_make_error(id, "missing_topics", "No topics were supplied."));
      }

      return websocket_handle_subscribe(Client, id, type, topics);
   }

   return websocket_write_text(Client, websocket_make_error(id, "unknown_command", "Unknown streaming command."));
}

//********************************************************************************************************************

static BackstageHttpResponse backstage_websocket_upgrade(objClientSocket *Client, const BackstageHttpRequest &Request)
{
   HttpHeaders headers(Request.headers);

   if (not websocket_request_is_upgrade(Request)) return BackstageHttpResponse::plain(400, "Expected WebSocket upgrade");
   if (not kt::iequals(headers.value("Sec-WebSocket-Version"), "13")) {
      return BackstageHttpResponse::plain(400, "Unsupported WebSocket version");
   }
   if (not websocket_key_is_valid(headers.value("Sec-WebSocket-Key"))) {
      return BackstageHttpResponse::plain(400, "Invalid WebSocket key");
   }
   if (not websocket_origin_allowed(headers.value("Origin"))) {
      return BackstageHttpResponse::plain(400, "Origin not allowed");
   }

   auto accept = websocket_accept_key(headers.value("Sec-WebSocket-Key"));
   if (accept.empty()) return BackstageHttpResponse::plain(500, "WebSocket handshake failed");

   std::string response;
   response.append("HTTP/1.1 101 Switching Protocols\r\n");
   response.append("Upgrade: websocket\r\n");
   response.append("Connection: Upgrade\r\n");
   response.append("Sec-WebSocket-Accept: ");
   response.append(accept);
   response.append("\r\n\r\n");

   int result = 0;
   if (Client->write(response.data(), int(response.size()), &result) != ERR::Okay) {
      return BackstageHttpResponse::plain(500, "WebSocket handshake failed");
   }

   if (result != int(response.size())) return BackstageHttpResponse::plain(500, "WebSocket handshake failed");

   {
      std::lock_guard<std::mutex> lock(glWebSocketLock);
      glWebSocketSessions[Client->UID] = BackstageWebSocketSession {
         .client_uid = Client->UID
      };
   }

   websocket_send_system_event(Client, "connected", "{\"endpoint\":\"/streaming\"}");
   websocket_send_system_event(Client, "heartbeat", "{\"interval\":0}");

   BackstageHttpResponse handled;
   handled.status = 0;
   return handled;
}

//********************************************************************************************************************

static ERR backstage_websocket_process(objClientSocket *Client, std::string_view Data)
{
   {
      std::lock_guard<std::mutex> lock(glWebSocketLock);
      auto it = glWebSocketSessions.find(Client->UID);
      if (it IS glWebSocketSessions.end()) return ERR::Disconnected;
      it->second.buffer.append(Data);
   }

   while (true) {
      BackstageWebSocketFrame frame;
      std::string buffer_copy;

      {
         std::lock_guard<std::mutex> lock(glWebSocketLock);
         auto it = glWebSocketSessions.find(Client->UID);
         if (it IS glWebSocketSessions.end()) return ERR::Disconnected;
         buffer_copy = it->second.buffer;
      }

      auto state = websocket_parse_frame(buffer_copy, frame);

      if (state IS WebSocketFrameState::INCOMPLETE) return ERR::Okay;
      if (state IS WebSocketFrameState::TOO_LARGE) return websocket_write_close(Client, 1009, "Message too large");
      if (state IS WebSocketFrameState::PROTOCOL_ERROR) return websocket_write_close(Client, 1002, "Protocol error");

      {
         std::lock_guard<std::mutex> lock(glWebSocketLock);
         auto it = glWebSocketSessions.find(Client->UID);
         if (it IS glWebSocketSessions.end()) return ERR::Disconnected;
         it->second.buffer.erase(0, frame.consumed);
      }

      if (not frame.fin) return websocket_write_close(Client, 1002, "Fragmented messages are not supported");

      if (frame.opcode IS 0x01) {
         std::string message((const char *)frame.payload.data(), frame.payload.size());
         if (auto error = websocket_handle_text(Client, message); error != ERR::Okay) return error;
      }
      else if (frame.opcode IS 0x02) {
         return websocket_write_close(Client, 1003, "Binary messages are not supported");
      }
      else if (frame.opcode IS 0x08) {
         websocket_write_frame(Client, 0x08, std::string_view((const char *)frame.payload.data(), frame.payload.size()));
         Client->deactivate();
         return ERR::Okay;
      }
      else if (frame.opcode IS 0x09) {
         if (auto error = websocket_write_frame(Client, 0x0a,
               std::string_view((const char *)frame.payload.data(), frame.payload.size())); error != ERR::Okay) {
            return error;
         }
      }
      else if (frame.opcode IS 0x0a) {
      }
      else return websocket_write_close(Client, 1002, "Unsupported frame opcode");
   }
}
