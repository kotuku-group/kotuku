//********************************************************************************************************************

struct BackstageErrorEntry {
   int code = 0;
   std::vector<std::string_view> names;
};

//********************************************************************************************************************

static void append_error_json_string(std::string &Output, std::string_view Value)
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

static void append_error_json_field(std::string &Output, std::string_view Name, std::string_view Value)
{
   append_error_json_string(Output, Name);
   Output.push_back(':');
   append_error_json_string(Output, Value);
}

//********************************************************************************************************************

static bool parse_error_code(std::string_view Text, int &Code)
{
   if (Text.starts_with("ERR_")) Text.remove_prefix(4);
   if (Text.empty()) return false;

   int base = 10;
   if ((Text.size() > 2) and (Text[0] IS '0') and ((Text[1] IS 'x') or (Text[1] IS 'X'))) {
      Text.remove_prefix(2);
      base = 16;
   }

   auto result = std::from_chars(Text.data(), Text.data() + Text.size(), Code, base);
   return (result.ec IS std::errc()) and (result.ptr IS Text.data() + Text.size());
}

//********************************************************************************************************************

static bool error_name_matches(std::string_view Name, std::string_view Token)
{
   if (Token.starts_with("ERR_")) Token.remove_prefix(4);
   return kt::iequals(Name, Token);
}

//********************************************************************************************************************

static BackstageErrorEntry *find_error_by_code(std::vector<BackstageErrorEntry> &Entries, int Code)
{
   for (auto &entry : Entries) {
      if (entry.code IS Code) return &entry;
   }

   return nullptr;
}

//********************************************************************************************************************

static const BackstageErrorEntry *find_error_by_code(const std::vector<BackstageErrorEntry> &Entries, int Code)
{
   for (auto &entry : Entries) {
      if (entry.code IS Code) return &entry;
   }

   return nullptr;
}

//********************************************************************************************************************

static const BackstageErrorEntry *find_error_by_name(const std::vector<BackstageErrorEntry> &Entries,
   std::string_view Name)
{
   for (auto &entry : Entries) {
      for (auto &entry_name : entry.names) {
         if (error_name_matches(entry_name, Name)) return &entry;
      }
   }

   return nullptr;
}

//********************************************************************************************************************

static std::span<const std::string_view> priority_error_names()
{
   static constexpr std::array<std::string_view, 14> names = {
      "Okay", "Terminate", "Search", "FileNotFound", "LockFailed", "Timeout", "NoPermission", "UndefinedField",
      "ExclusiveDenied", "Finished", "Syntax", "IntegrityViolation", "InvalidObject", "AlreadyLocked"
   };

   return names;
}

//********************************************************************************************************************

static size_t error_name_priority(std::string_view Name)
{
   auto names = priority_error_names();
   for (size_t i = 0; i < names.size(); i++) {
      if (names[i] IS Name) return i;
   }

   return names.size();
}

//********************************************************************************************************************

static void prioritise_error_names(std::vector<BackstageErrorEntry> &Entries)
{
   for (auto &entry : Entries) {
      if (entry.names.size() <= 1) continue;

      auto canonical = std::min_element(entry.names.begin(), entry.names.end(),
         [](std::string_view A, std::string_view B) {
            return error_name_priority(A) < error_name_priority(B);
         });

      if (not (canonical IS entry.names.end())) std::rotate(entry.names.begin(), canonical, canonical + 1);
   }
}

//********************************************************************************************************************

static bool parse_error_idl_value(std::string_view Value, int &Code)
{
   if ((Value.size() > 2) and (Value[0] IS '0') and ((Value[1] IS 'x') or (Value[1] IS 'X'))) {
      Value.remove_prefix(2);
   }

   auto result = std::from_chars(Value.data(), Value.data() + Value.size(), Code, 16);
   return (result.ec IS std::errc()) and (result.ptr IS Value.data() + Value.size());
}

//********************************************************************************************************************

static void parse_error_idl_constant(std::vector<BackstageErrorEntry> &Entries, std::string_view Token)
{
   auto equals = Token.find('=');
   if (equals IS std::string_view::npos) return;

   auto name = Token.substr(0, equals);
   if (name.empty()) return;
   if (name IS "END") return;

   int code = 0;
   if (not parse_error_idl_value(Token.substr(equals + 1), code)) return;
   if (code < 0) return;
   if (code >= int(ERR::END)) return;

   auto entry = find_error_by_code(Entries, code);
   if (entry) entry->names.push_back(name);
   else {
      BackstageErrorEntry new_entry;
      new_entry.code = code;
      new_entry.names.push_back(name);
      Entries.push_back(new_entry);
   }
}

//********************************************************************************************************************

static void parse_error_idl(std::vector<BackstageErrorEntry> &Entries)
{
   auto state = GetSystemState();
   if ((not state) or (not state->IDL)) return;

   std::string_view idl(state->IDL);
   auto start = idl.find("c.ERR:");
   if (start IS std::string_view::npos) return;

   start += 6;
   auto end = idl.find('\n', start);
   if (end IS std::string_view::npos) end = idl.size();

   auto line = idl.substr(start, end - start);
   size_t pos = 0;

   while (pos <= line.size()) {
      auto next = line.find(',', pos);
      if (next IS std::string_view::npos) next = line.size();

      parse_error_idl_constant(Entries, line.substr(pos, next - pos));

      if (next IS line.size()) break;
      pos = next + 1;
   }

   prioritise_error_names(Entries);

   std::sort(Entries.begin(), Entries.end(), [](const BackstageErrorEntry &A, const BackstageErrorEntry &B) {
      return A.code < B.code;
   });
}

//********************************************************************************************************************

static const std::vector<BackstageErrorEntry> & backstage_error_entries()
{
   static std::once_flag once;
   static std::vector<BackstageErrorEntry> entries;

   std::call_once(once, [] {
      parse_error_idl(entries);
   });

   return entries;
}

//********************************************************************************************************************

static void append_error_aliases(std::string &Output, const BackstageErrorEntry &Entry)
{
   if (Entry.names.size() <= 1) return;

   Output.append(",\"aliases\":[");

   for (size_t i = 1; i < Entry.names.size(); i++) {
      if (i > 1) Output.push_back(',');
      append_error_json_string(Output, Entry.names[i]);
   }

   Output.push_back(']');
}

//********************************************************************************************************************

static void append_error_entry(std::string &Output, const BackstageErrorEntry &Entry)
{
   auto message = GetErrorMsg(ERR(Entry.code));

   Output.push_back('{');
   Output.append("\"code\":");
   Output.append(std::to_string(Entry.code));
   Output.push_back(',');
   append_error_json_field(Output, "name", Entry.names.empty() ? std::string_view() : Entry.names[0]);
   append_error_aliases(Output, Entry);
   Output.push_back(',');
   append_error_json_field(Output, "message", message ? std::string_view(message) : std::string_view());
   Output.append(",\"exception\":");
   Output.append(Entry.code > int(ERR::ExceptionThreshold) ? "true" : "false");
   Output.push_back('}');
}

//********************************************************************************************************************

static bool error_entry_matches_filter(const BackstageErrorEntry &Entry, std::string_view Token)
{
   int code = 0;
   if ((parse_error_code(Token, code)) and (Entry.code IS code)) return true;

   for (auto &name : Entry.names) {
      if (error_name_matches(name, Token)) return true;
   }

   return false;
}

//********************************************************************************************************************

static bool error_entry_matches_filters(const BackstageErrorEntry &Entry, std::string_view Filters)
{
   if (Filters.empty()) return true;

   size_t pos = 0;

   while (pos <= Filters.size()) {
      auto next = Filters.find(',', pos);
      if (next IS std::string_view::npos) next = Filters.size();

      auto token = Filters.substr(pos, next - pos);
      if ((not token.empty()) and (error_entry_matches_filter(Entry, token))) return true;

      if (next IS Filters.size()) break;
      pos = next + 1;
   }

   return false;
}

//********************************************************************************************************************

static void set_error_not_found(BackstageResponse &Response, std::string_view Code)
{
   Response.status = 404;
   Response.contentType = "application/json";
   Response.body = "{\"error\":\"not_found\",\"code\":";
   append_error_json_string(Response.body, Code);
   Response.body.push_back('}');
}

//********************************************************************************************************************
// @doc GET /errors Return a JSON list of known error codes and their symbolic names.

static ERR get_errors(const BackstageRequest &Request, BackstageResponse &Response)
{
   const auto &entries = backstage_error_entries();

   std::string_view filter;
   auto filter_iter = Request.queryParams.find("filter");
   if (not (filter_iter IS Request.queryParams.end())) filter = filter_iter->second;

   Response.status = 200;
   Response.contentType = "application/json";
   Response.body.reserve(entries.size() * 96);
   Response.body.push_back('[');

   bool first = true;

   for (auto &entry : entries) {
      if (not error_entry_matches_filters(entry, filter)) continue;

      if (not first) Response.body.push_back(',');
      append_error_entry(Response.body, entry);
      first = false;
   }

   Response.body.push_back(']');
   return ERR::Okay;
}

//********************************************************************************************************************
// @doc GET /errors/{code} Return meta data for a single error code.

static ERR get_errors_code(const BackstageRequest &Request, BackstageResponse &Response)
{
   const auto &entries = backstage_error_entries();
   auto code_iter = Request.pathParams.find("code");
   if (code_iter IS Request.pathParams.end()) {
      set_error_not_found(Response, {});
      return ERR::Okay;
   }

   const BackstageErrorEntry *entry = nullptr;
   int code = 0;

   if (parse_error_code(code_iter->second, code)) entry = find_error_by_code(entries, code);
   if (not entry) entry = find_error_by_name(entries, code_iter->second);

   if (not entry) {
      set_error_not_found(Response, code_iter->second);
      return ERR::Okay;
   }

   Response.status = 200;
   Response.contentType = "application/json";
   Response.body.reserve(160);
   append_error_entry(Response.body, *entry);
   return ERR::Okay;
}
