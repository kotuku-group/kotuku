//********************************************************************************************************************
// @doc GET /docs Return Backstage API documentation meta data.

static void append_json_string(std::string &Output, std::string_view Value)
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

static void append_json_field(std::string &Output, std::string_view Name, std::string_view Value)
{
   append_json_string(Output, Name);
   Output.push_back(':');
   append_json_string(Output, Value);
}

//********************************************************************************************************************

static void append_route_param(std::string &Output, const BackstageParam &Param)
{
   Output.push_back('{');
   append_json_field(Output, "name", Param.name);
   Output.push_back(',');
   append_json_field(Output, "type", Param.type);
   Output.push_back(',');
   append_json_field(Output, "description", Param.description);
   Output.push_back(',');
   append_json_field(Output, "default", Param.default_value);
   Output.append(",\"required\":");
   Output.append(Param.required ? "true" : "false");
   Output.push_back('}');
}

//********************************************************************************************************************

static void append_route_params(std::string &Output, std::string_view Name, std::span<const BackstageParam> Params)
{
   append_json_string(Output, Name);
   Output.append(":[");

   for (size_t i = 0; i < Params.size(); i++) {
      if (i > 0) Output.push_back(',');
      append_route_param(Output, Params[i]);
   }

   Output.push_back(']');
}

//********************************************************************************************************************

static void append_route_index_entry(std::string &Output, const BackstageRoute &Route)
{
   Output.push_back('{');
   append_json_field(Output, "method", Route.method);
   Output.push_back(',');
   append_json_field(Output, "path", Route.path);
   if (not Route.metadata.transport.empty()) {
      Output.push_back(',');
      append_json_field(Output, "transport", Route.metadata.transport);
   }
   Output.push_back('}');
}

//********************************************************************************************************************

static void append_route_details(std::string &Output, const BackstageRoute &Route)
{
   Output.push_back('{');
   append_json_field(Output, "method", Route.method);
   Output.push_back(',');
   append_json_field(Output, "path", Route.path);
   Output.push_back(',');
   append_json_field(Output, "description", Route.metadata.description);
   Output.push_back(',');
   append_json_field(Output, "body", Route.metadata.body);
   Output.push_back(',');
   append_json_field(Output, "returns", Route.metadata.returns);
   Output.push_back(',');
   append_json_field(Output, "transport", Route.metadata.transport);
   Output.push_back(',');
   append_route_params(Output, "pathParams", Route.metadata.path_params);
   Output.push_back(',');
   append_route_params(Output, "queryParams", Route.metadata.query_params);
   Output.push_back('}');
}

//********************************************************************************************************************

static bool compile_docs_filter(const BackstageRequest &Request, const char *Name, Regex **Result,
   BackstageResponse &Response)
{
   *Result = nullptr;

   auto filter = Request.queryParams.find(Name);
   if ((filter IS Request.queryParams.end()) or filter->second.empty()) return true;

   std::string error_message;
   if (not (rx::Compile(filter->second, REGEX::NIL, &error_message, Result) IS ERR::Okay)) {
      Response.status = 400;
      Response.contentType = "text/plain";
      Response.body = "Invalid ";
      Response.body.append(Name);
      Response.body.append(" regex");
      if (not error_message.empty()) {
         Response.body.append(": ");
         Response.body.append(error_message);
      }
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool route_matches_filter(Regex *Filter, std::string_view Value)
{
   if (not Filter) return true;
   return rx::Match(Filter, Value, RMATCH::NIL, nullptr) IS ERR::Okay;
}

//********************************************************************************************************************

static ERR get_docs(const BackstageRequest &Request, BackstageResponse &Response)
{
   auto routes = backstage_routes();

   Response.status = 200;
   Response.contentType = "application/json";
   Response.body.reserve(256 + (routes.size() * 48));
   Response.body.append("{\"name\":\"Backstage\",");
   append_json_field(Response.body, "description", "Embedded REST service for interacting with running processes.");
   Response.body.append(",\"routeCount\":");
   Response.body.append(std::to_string(routes.size()));
   Response.body.append(",\"routes\":[");

   for (size_t i = 0; i < routes.size(); i++) {
      if (i > 0) Response.body.push_back(',');
      append_route_index_entry(Response.body, routes[i]);
   }

   Response.body.append("]}");
   return ERR::Okay;
}

//********************************************************************************************************************
// @doc GET /docs/routes Return the declared Backstage routes and their meta data.

static ERR get_docs_routes(const BackstageRequest &Request, BackstageResponse &Response)
{
   Regex *method_filter = nullptr;
   Regex *path_filter = nullptr;

   if (not compile_docs_filter(Request, "methodFilter", &method_filter, Response)) return ERR::Okay;

   if (not compile_docs_filter(Request, "pathFilter", &path_filter, Response)) {
      if (method_filter) FreeResource(method_filter);
      return ERR::Okay;
   }

   auto routes = backstage_routes();

   Response.status = 200;
   Response.contentType = "application/json";
   Response.body.reserve(routes.size() * 256);
   Response.body.push_back('[');

   bool first = true;

   for (auto &route : routes) {
      if (not route_matches_filter(method_filter, route.method)) continue;
      if (not route_matches_filter(path_filter, route.path)) continue;

      if (not first) Response.body.push_back(',');
      append_route_details(Response.body, route);
      first = false;
   }

   Response.body.push_back(']');

   if (path_filter) FreeResource(path_filter);
   if (method_filter) FreeResource(method_filter);

   return ERR::Okay;
}
