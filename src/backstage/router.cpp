
struct RouteMatch {
   std::vector<std::string_view> captures;
};

//********************************************************************************************************************

static ERR route_match_callback(int Index, std::vector<std::string_view> &Captures, size_t MatchStart, size_t MatchEnd,
   RouteMatch &Context)
{
   Context.captures = Captures;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR compile_backstage_routes()
{
   kt::Log log(__FUNCTION__);

   for (auto &route : glRoutes) {
      if ((not route.regex) and (not route.pattern.empty())) {
         if (rx::Compile(route.pattern, REGEX::NIL, nullptr, &route.regex) != ERR::Okay) {
            log.warning("Failed to compile route regex: %.*s", int(route.pattern.size()), route.pattern.data());
            return ERR::Failed;
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void release_backstage_routes()
{
   for (auto &route : glRoutes) {
      if (route.regex) {
         FreeResource(route.regex);
         route.regex = nullptr;
      }
   }
}

//********************************************************************************************************************

static void parse_query_params(std::string_view QueryString,
   std::unordered_map<std::string, std::string> &Params)
{
   size_t start = 0;

   while (start <= QueryString.size()) {
      auto end = QueryString.find('&', start);
      if (end IS std::string_view::npos) end = QueryString.size();

      auto pair = QueryString.substr(start, end - start);
      if (not pair.empty()) {
         auto equals = pair.find('=');
         if (equals IS std::string_view::npos) Params.emplace(std::string(pair), std::string());
         else Params.emplace(std::string(pair.substr(0, equals)), std::string(pair.substr(equals + 1)));
      }

      if (end IS QueryString.size()) break;
      start = end + 1;
   }
}

//********************************************************************************************************************

static void extract_path_params(const BackstageRoute &Route, const std::vector<std::string_view> &Captures,
   std::unordered_map<std::string, std::string> &Params)
{
   for (size_t i = 0; (i < Route.path_param_names.size()) and (i + 1 < Captures.size()); i++) {
      Params.emplace(std::string(Route.path_param_names[i]), std::string(Captures[i + 1]));
   }
}

//********************************************************************************************************************

static bool route_matches_path(BackstageRoute &Route, std::string_view Path, RouteMatch &Match)
{
   if (not Route.regex) return false;

   auto callback = C_FUNCTION(&route_match_callback, &Match);
   Match.captures.clear();

   return rx::Match(Route.regex, Path, RMATCH::NIL, &callback) IS ERR::Okay;
}

//********************************************************************************************************************

static void append_allowed_method(std::string &Allow, std::string_view Method)
{
   if (not Allow.empty()) Allow.append(", ");
   Allow.append(Method);
}

//********************************************************************************************************************

static std::string_view media_type(std::string_view ContentType)
{
   auto semicolon = ContentType.find(';');
   if (not (semicolon IS std::string_view::npos)) ContentType = ContentType.substr(0, semicolon);
   return HttpHeaders::trim_value(ContentType);
}

//********************************************************************************************************************

static bool route_body_is_json(const BackstageRoute &Route)
{
   return kt::iequals(Route.metadata.body, "json");
}

//********************************************************************************************************************

static bool route_expects_body(const BackstageRoute &Route)
{
   return not Route.metadata.body.empty();
}

//********************************************************************************************************************

static bool request_content_type_is_json(const BackstageHttpRequest &Request)
{
   return kt::iequals(media_type(Request.content_type), "application/json");
}

//********************************************************************************************************************

static BackstageHttpResponse validate_route_body_policy(const BackstageRoute &Route,
   const BackstageHttpRequest &HttpRequest)
{
   if (route_expects_body(Route)) {
      if (not HttpRequest.has_content_length) return BackstageHttpResponse::plain(411, "Length Required");
      if (HttpRequest.body.empty()) return BackstageHttpResponse::plain(400, "Request body is required");

      if ((route_body_is_json(Route)) and (not request_content_type_is_json(HttpRequest))) {
         return BackstageHttpResponse::plain(415, "Unsupported Media Type");
      }
   }
   else if (not HttpRequest.body.empty()) {
      return BackstageHttpResponse::plain(415, "Unsupported Media Type");
   }

   return {};
}

//********************************************************************************************************************

static BackstageRequest build_route_request(objClientSocket *Client, BackstageRoute &Route,
   const BackstageHttpRequest &HttpRequest, const RouteMatch &Match)
{
   BackstageRequest request = {
      .route       = &Route,
      .server      = (objNetServer *)glServer,
      .client      = Client,
      .method      = HttpRequest.line.method,
      .path        = HttpRequest.line.path,
      .rawPath     = HttpRequest.line.rawPath,
      .queryString = HttpRequest.line.queryString,
      .version     = HttpRequest.line.version,
      .body        = std::span<const uint8_t>((const uint8_t *)HttpRequest.body.data(), HttpRequest.body.size())
   };

   parse_query_params(HttpRequest.line.queryString, request.queryParams);
   extract_path_params(Route, Match.captures, request.pathParams);
   return request;
}

//********************************************************************************************************************

static BackstageHttpResponse dispatch_route_request(objClientSocket *Client, const BackstageHttpRequest &HttpRequest)
{
   std::string allowed_methods;

   for (auto &route : glRoutes) {
      RouteMatch match;

      if (not route_matches_path(route, HttpRequest.line.path, match)) continue;

      if (not (route.method.compare(HttpRequest.line.method) IS 0)) {
         append_allowed_method(allowed_methods, route.method);
         continue;
      }

      if (kt::iequals(route.metadata.transport, "websocket")) {
         return BackstageHttpResponse::plain(400, "Expected WebSocket upgrade");
      }

      if (auto policy_response = validate_route_body_policy(route, HttpRequest);
            not (policy_response.status IS 200)) {
         return policy_response;
      }

      auto request = build_route_request(Client, route, HttpRequest, match);
      BackstageResponse response;
      auto error = route.handler(request, response);

      if (!error) return BackstageHttpResponse::from_route(response);
      if (error IS ERR::NoSupport) return BackstageHttpResponse::plain(501, "Not Implemented");
      return BackstageHttpResponse::plain(500, "Internal Server Error");
   }

   if (not allowed_methods.empty()) {
      auto response = BackstageHttpResponse::plain(405, "Method Not Allowed");
      response.extra_headers = "Allow: ";
      response.extra_headers.append(allowed_methods);
      response.extra_headers.append("\r\n");
      return response;
   }

   return BackstageHttpResponse::plain(404, "Not Found");
}
