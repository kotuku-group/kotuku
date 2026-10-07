// Backstage application protocol.  RFC 6455 belongs to the official WebSocket module.

static bool websocket_request_targets_streaming(const BackstageHttpRequest &Request)
{
   return Request.line.path IS "/streaming";
}

#ifdef BACKSTAGE_WEBSOCKET
static constexpr size_t MAX_WEBSOCKET_MESSAGE = 64 * 1024;

struct BackstageWebSocketSession {
   uint64_t sequence = 0;
   std::unordered_set<std::string> topics;
};

static std::mutex glWebSocketLock;
static std::unordered_map<OBJECTID, BackstageWebSocketSession> glWebSocketSessions;

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

static ERR backstage_send_text(objWebSocket *Connection, std::string_view Text)
{
   return Connection->send(std::span<const int8_t>((const int8_t *)Text.data(), Text.size()), WSM::TEXT);
}

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

static ERR websocket_send_system_event(objWebSocket *Client, std::string_view Event, std::string_view Data)
{
   return backstage_send_text(Client, websocket_make_event(Client->UID, "system", Event, Data));
}

//********************************************************************************************************************

static ERR websocket_send_log_stub(objWebSocket *Client)
{
   return backstage_send_text(Client, websocket_make_event(Client->UID, "log", "stub",
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

static ERR websocket_handle_subscribe(objWebSocket *Client, std::string_view ID, std::string_view Command,
   const std::vector<std::string> &Topics)
{
   std::vector<std::string> accepted;

   for (auto &topic : Topics) {
      if (not websocket_topic_is_known(topic)) {
         return backstage_send_text(Client, websocket_make_error(ID, "unknown_topic", "Unknown streaming topic."));
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

   auto error = backstage_send_text(Client, websocket_make_ack(ID, Command, accepted));
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

static ERR websocket_handle_text(objWebSocket *Client, std::string_view Message)
{
   auto id = websocket_extract_json_string(Message, "id");
   auto type = websocket_extract_json_string(Message, "type");

   if (kt::iequals(type, "subscribe") or kt::iequals(type, "unsubscribe")) {
      auto topics = websocket_extract_topics(Message);
      if (topics.empty()) {
         return backstage_send_text(Client, websocket_make_error(id, "missing_topics", "No topics were supplied."));
      }

      return websocket_handle_subscribe(Client, id, type, topics);
   }

   return backstage_send_text(Client, websocket_make_error(id, "unknown_command", "Unknown streaming command."));
}

static void backstage_state_changed(objWebSocket *Connection, WSS State, APTR Meta)
{
   if (State IS WSS::CLOSED) {
      std::lock_guard<std::mutex> lock(glWebSocketLock);
      glWebSocketSessions.erase(Connection->UID);
   }
}

static ERR backstage_incoming(objWebSocket *Connection, const void *Data, int Length,
   WSM Type, bool Final, APTR Meta)
{
   if (Type IS WSM::BINARY) {
      Connection->close(int(WSC::UNSUPPORTED_DATA), "Binary messages are not supported");
   }
   else {
      auto error = websocket_handle_text(Connection, std::string_view((const char *)Data, size_t(Length)));
      if (error != ERR::Okay) Connection->close(int(WSC::INTERNAL_ERROR), "Streaming response could not be queued");
   }
   return ERR::Okay;
}

static ERR backstage_accept(objWebSocketServer *Server, WSRequest *Request, APTR Meta)
{
   return websocket_origin_allowed(Request->Origin) ? ERR::Okay : ERR::Failed;
}

static void backstage_connected(objWebSocketServer *Server, objWebSocket *Connection, APTR Meta)
{
   {
      std::lock_guard<std::mutex> lock(glWebSocketLock);
      glWebSocketSessions[Connection->UID] = BackstageWebSocketSession{};
   }
   Connection->setIncoming(C_FUNCTION(backstage_incoming));
   Connection->setStateChanged(C_FUNCTION(backstage_state_changed));
   websocket_send_system_event(Connection, "connected", "{\"endpoint\":\"/streaming\"}");
   websocket_send_system_event(Connection, "heartbeat", "{\"interval\":0}");
}

static void backstage_disconnected(objWebSocketServer *Server, objWebSocket *Connection, APTR Meta)
{
   std::lock_guard<std::mutex> lock(glWebSocketLock);
   glWebSocketSessions.erase(Connection->UID);
}
#endif

static void init_backstage_websockets()
{
#ifdef BACKSTAGE_WEBSOCKET
   if (objModule::load("websocket", &modWebSocket) != ERR::Okay) return;
   glWebSocketServer = objWebSocketServer::create::global({
      fl::Flags(WSF::EXTERNAL_LISTENER),
      fl::Path("/streaming"),
      FieldValue(kt::fieldhash("MaxMessageSize"), int64_t(MAX_WEBSOCKET_MESSAGE)),
      FieldValue(kt::fieldhash("SendLimit"), int64_t(256 * 1024)),
      FieldValue(kt::fieldhash("Accept"), C_FUNCTION(backstage_accept)),
      FieldValue(kt::fieldhash("Connected"), C_FUNCTION(backstage_connected)),
      FieldValue(kt::fieldhash("Disconnected"), C_FUNCTION(backstage_disconnected))
   });
#endif
}

static void release_backstage_websockets()
{
#ifdef BACKSTAGE_WEBSOCKET
   auto server = glWebSocketServer;
   glWebSocketServer = nullptr;
   if (server) FreeResource(server);
   std::lock_guard<std::mutex> lock(glWebSocketLock);
   glWebSocketSessions.clear();
#endif
}

enum class BackstageSocketEvent { INCOMING, OUTGOING, DISCONNECTED };

static ERR dispatch_backstage_websocket(objClientSocket *Client, BackstageSocketEvent Event)
{
#ifdef BACKSTAGE_WEBSOCKET
   if (glWebSocketServer) return glWebSocketServer->dispatch(Client, WSE(int(Event)));
#endif
   return ERR::NotFound;
}

static ERR backstage_websocket_upgrade(objClientSocket *Client, std::string_view Request)
{
#ifdef BACKSTAGE_WEBSOCKET
   if (glWebSocketServer) {
      objWebSocket *connection = nullptr;
      auto error = glWebSocketServer->adopt(Client,
         std::span<const int8_t>((const int8_t *)Request.data(), Request.size()), &connection);
      return (error IS ERR::Okay) ? ERR::Okay : ERR::Terminate;
   }
#endif
   BackstageHttpResponse::plain(501, "WebSocket streaming is unavailable").write(Client);
   return ERR::Okay;
}
