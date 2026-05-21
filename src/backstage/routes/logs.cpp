
struct LogMessage {
   std::string Header;
   std::string Message;
   int ThreadID;
   int Depth;
   int Level;
};

static std::mutex glLogRecordingLock;
static std::vector<LogMessage> glLogMessages;
static int glLogRecordingThread = 0;
static bool glLogRecordingActive = false;
static bool glLogIncludeThreads = false;
static int glLogMaxDepth = 255;
static int glLogMaxLevel = 9;

//********************************************************************************************************************
// Parses a console log level query parameter.  Backstage accepts the same 0-9 range used by the core logging resource.

static bool parse_log_level(std::string_view Value, int &Level)
{
   if (Value.empty()) return false;

   auto result = std::from_chars(Value.data(), Value.data() + Value.size(), Level);
   if ((not (result.ec IS std::errc())) or (not (result.ptr IS Value.data() + Value.size()))) return false;

   return (Level >= 0) and (Level <= 9);
}

//********************************************************************************************************************
// Parses a boolean log-recording query parameter.

static bool parse_log_bool(std::string_view Value, bool &Flag)
{
   if (kt::iequals(Value, "true") or kt::iequals(Value, "1")) {
      Flag = true;
      return true;
   }
   else if (kt::iequals(Value, "false") or kt::iequals(Value, "0")) {
      Flag = false;
      return true;
   }

   return false;
}

//********************************************************************************************************************
// Appends a JSON string literal to a response body while escaping control characters and quote delimiters.

static void append_log_json_string(std::string &Output, std::string_view Value)
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
// Appends an integer as decimal text for JSON numeric fields.

static void append_log_int(std::string &Output, int Value)
{
   char buffer[16];
   auto result = std::to_chars(buffer, buffer + sizeof(buffer), Value);
   if (result.ec IS std::errc()) Output.append(buffer, result.ptr);
}

//********************************************************************************************************************
// Serialises a captured log entry using the JSON field names exposed by the /logs route.

static void append_log_message(std::string &Output, const LogMessage &Message)
{
   Output.append("{\"header\":");
   append_log_json_string(Output, Message.Header);
   Output.append(",\"message\":");
   append_log_json_string(Output, Message.Message);
   Output.append(",\"depth\":");
   append_log_int(Output, Message.Depth);
   Output.append(",\"level\":");
   append_log_int(Output, Message.Level);
   Output.push_back('}');
}

//********************************************************************************************************************
// Parses bounded integer log-recording options such as maxDepth and maxLevel.

static bool parse_log_limit(std::string_view Value, int Min, int Max, int &Limit)
{
   if (Value.empty()) return false;

   auto result = std::from_chars(Value.data(), Value.data() + Value.size(), Limit);
   if ((not (result.ec IS std::errc())) or (not (result.ptr IS Value.data() + Value.size()))) return false;

   return (Limit >= Min) and (Limit <= Max);
}

//********************************************************************************************************************
// Reads an optional query parameter, falling back to a default value when the client omits it.

static bool parse_optional_log_limit(const BackstageRequest &Request, const char *Name, int DefaultValue, int Min,
   int Max, int &Limit)
{
   auto param = Request.queryParams.find(Name);
   if (param IS Request.queryParams.end()) {
      Limit = DefaultValue;
      return true;
   }

   return parse_log_limit(param->second, Min, Max, Limit);
}

//********************************************************************************************************************
// Reads an optional boolean query parameter, falling back to a default value when the client omits it.

static bool parse_optional_log_bool(const BackstageRequest &Request, const char *Name, bool DefaultValue, bool &Flag)
{
   auto param = Request.queryParams.find(Name);
   if (param IS Request.queryParams.end()) {
      Flag = DefaultValue;
      return true;
   }

   return parse_log_bool(param->second, Flag);
}

//********************************************************************************************************************
// Returns a consistent JSON error payload for invalid log-recording query parameters.

static void set_log_recording_error(BackstageResponse &Response, std::string_view Code, std::string_view Message)
{
   Response.status = 400;
   Response.contentType = "application/json";
   Response.body = "{\"error\":";
   append_log_json_string(Response.body, Code);
   Response.body.append(",\"message\":");
   append_log_json_string(Response.body, Message);
   Response.body.push_back('}');
}

//********************************************************************************************************************
// Captures log messages while internal Backstage log recording is active.

static void backstage_log_callback(CSTRING Header, CSTRING Message, int Depth, int MsgLevel, int LogLevel)
{
   auto thread_id = kt::_get_thread_id();

   std::lock_guard<std::mutex> lock(glLogRecordingLock);

   if (not glLogRecordingActive) return;
   if ((not glLogIncludeThreads) and (not (thread_id IS glLogRecordingThread))) return;
   if (Depth > glLogMaxDepth) return;
   if (MsgLevel > glLogMaxLevel) return;

   glLogMessages.emplace_back(LogMessage {
      .Header = Header ? Header : "",
      .Message = Message ? Message : "",
      .ThreadID = thread_id,
      .Depth = Depth,
      .Level = MsgLevel
   });
}

//********************************************************************************************************************
// Stops internal recording and clears the in-memory message buffer before unregistering the callback.

static void release_backstage_logs()
{
   {
      std::lock_guard<std::mutex> lock(glLogRecordingLock);
      glLogRecordingActive = false;
      glLogIncludeThreads = false;
      glLogRecordingThread = 0;
      glLogMessages.clear();
   }

   SetLogCallback((APTR)backstage_log_callback, 0, 0);
}

//********************************************************************************************************************
// @doc POST /logs/start Activates internal log recording.

static ERR post_logs_start(const BackstageRequest &Request, BackstageResponse &Response)
{
   int max_depth;
   int max_level;
   bool include_threads;

   if (not parse_optional_log_limit(Request, "maxDepth", 255, 0, 255, max_depth)) {
      set_log_recording_error(Response, "invalid_max_depth", "Expected maxDepth query parameter from 0 to 255");
      return ERR::Okay;
   }

   if (not parse_optional_log_limit(Request, "maxLevel", 9, 0, 9, max_level)) {
      set_log_recording_error(Response, "invalid_max_level", "Expected maxLevel query parameter from 0 to 9");
      return ERR::Okay;
   }

   if (not parse_optional_log_bool(Request, "includeThreads", false, include_threads)) {
      set_log_recording_error(Response, "invalid_include_threads",
         "Expected includeThreads query parameter to be true, false, 0 or 1");
      return ERR::Okay;
   }

   {
      std::lock_guard<std::mutex> lock(glLogRecordingLock);
      glLogMessages.clear();
      glLogRecordingThread = GetResource(RES::MAIN_THREAD_ID);
      glLogIncludeThreads = include_threads;
      glLogMaxDepth = max_depth;
      glLogMaxLevel = max_level;
      glLogRecordingActive = true;
   }

   SetLogCallback((APTR)backstage_log_callback, max_depth IS 0 ? 1 : max_depth, max_level IS 0 ? 1 : max_level);

   kt::Log("backstage_logs").msg("Internal log recording started.");

   Response.status = 200;
   Response.contentType = "application/json";
   Response.body = "{\"active\":true,\"includeThreads\":";
   Response.body.append(include_threads ? "true" : "false");
   Response.body.append(",\"maxDepth\":");
   Response.body.append(std::to_string(max_depth));
   Response.body.append(",\"maxLevel\":");
   Response.body.append(std::to_string(max_level));
   Response.body.push_back('}');
   return ERR::Okay;
}

//********************************************************************************************************************
// @doc POST /logs/stop Stops internal log recording.

static ERR post_logs_stop(const BackstageRequest &Request, BackstageResponse &Response)
{
   size_t total = 0;

   {
      std::lock_guard<std::mutex> lock(glLogRecordingLock);
      glLogRecordingActive = false;
      glLogIncludeThreads = false;
      glLogRecordingThread = 0;
      total = glLogMessages.size();
   }

   SetLogCallback((APTR)backstage_log_callback, 0, 0);

   Response.status = 200;
   Response.contentType = "application/json";
   Response.body = "{\"active\":false,\"pending\":";
   Response.body.append(std::to_string(total));
   Response.body.push_back('}');
   return ERR::Okay;
}

//********************************************************************************************************************
// @doc GET /logs Returns all logged messages, then clears the log message stack.  The client is responsible for
// maintaining a permanent record.

static ERR get_logs(const BackstageRequest &Request, BackstageResponse &Response)
{
   std::vector<LogMessage> messages;

   {
      std::lock_guard<std::mutex> lock(glLogRecordingLock);
      messages.swap(glLogMessages);
   }

   Response.status = 200;
   Response.contentType = "application/json";
   Response.body.reserve(2 + (messages.size() * 64));
   Response.body.push_back('[');

   for (size_t i = 0; i < messages.size(); i++) {
      if (i > 0) Response.body.push_back(',');
      append_log_message(Response.body, messages[i]);
   }

   Response.body.push_back(']');
   return ERR::Okay;
}

//********************************************************************************************************************
// @doc PUT /logs/level Change the logging level for the program (affects console output).

static ERR put_logs_level(const BackstageRequest &Request, BackstageResponse &Response)
{
   auto level_param = Request.queryParams.find("level");
   int level;

   if ((level_param IS Request.queryParams.end()) or (not parse_log_level(level_param->second, level))) {
      Response.status = 400;
      Response.body = "{\"error\":\"invalid_level\",\"message\":\"Expected level query parameter from 0 to 9\"}";
      return ERR::Okay;
   }

   SetResource(RES::LOG_LEVEL, level);

   Response.status = 200;
   Response.contentType = "application/json";
   Response.body = "{\"level\":";
   Response.body.append(std::to_string(level));
   Response.body.push_back('}');
   return ERR::Okay;
}
