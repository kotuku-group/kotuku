
struct RequestLine {
   std::string_view method;
   std::string_view path;
   std::string_view rawPath;
   std::string_view queryString;
   std::string_view version;
};

//********************************************************************************************************************

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

static bool parse_request_line(std::string_view Request, RequestLine &Line)
{
   auto line_end = Request.find("\r\n");
   if (line_end IS std::string_view::npos) return false;

   auto request_line = Request.substr(0, line_end);
   auto method_end = request_line.find(' ');
   if ((method_end IS std::string_view::npos) or (method_end <= 0)) return false;

   auto path_start = request_line.find_first_not_of(' ', method_end);
   if (path_start IS std::string_view::npos) return false;

   auto path_end = request_line.find(' ', path_start);
   if ((path_end IS std::string_view::npos) or (path_end <= path_start)) return false;

   auto version_start = request_line.find_first_not_of(' ', path_end);
   if (version_start IS std::string_view::npos) return false;

   auto version_end = request_line.find(' ', version_start);
   if (version_end != std::string_view::npos) return false;

   Line.method = request_line.substr(0, method_end);
   Line.rawPath = request_line.substr(path_start, path_end - path_start);
   Line.path = Line.rawPath;
   Line.version = request_line.substr(version_start);

   if (!Line.version.starts_with("HTTP/")) return false;

   auto query = Line.path.find('?');
   if (query != std::string_view::npos) {
      Line.queryString = Line.path.substr(query + 1);
      Line.path = Line.path.substr(0, query);
   }

   return true;
}

//********************************************************************************************************************

static void write_response(objClientSocket *Client, int Status, std::string_view StatusText, std::string_view Body,
   std::string_view ContentType = "text/plain", std::string_view ExtraHeaders = {})
{
   std::string response;
   response.reserve(128 + Body.size() + ExtraHeaders.size());
   response.append("HTTP/1.1 ");
   response.append(std::to_string(Status));
   response.append(" ");
   response.append(StatusText);
   response.append("\r\nContent-Type: ");
   response.append(ContentType);
   response.append("\r\nContent-Length: ");
   response.append(std::to_string(Body.size()));
   response.append("\r\nConnection: close\r\n");
   response.append(ExtraHeaders);
   response.append("\r\n");
   response.append(Body);

   int result;
   Client->write(response.c_str(), int(response.size()), &result);
   Client->deactivate();
}

//********************************************************************************************************************

static std::string_view trim_header_value(std::string_view Value)
{
   while ((not Value.empty()) and ((Value.front() IS ' ') or (Value.front() IS '\t'))) Value.remove_prefix(1);
   while ((not Value.empty()) and ((Value.back() IS ' ') or (Value.back() IS '\t'))) Value.remove_suffix(1);
   return Value;
}

//********************************************************************************************************************

static size_t parse_content_length(std::string_view Headers)
{
   size_t pos = 0;

   while (pos < Headers.size()) {
      auto line_end = Headers.find("\r\n", pos);
      if (line_end IS std::string_view::npos) line_end = Headers.size();

      auto line = Headers.substr(pos, line_end - pos);
      auto colon = line.find(':');
      if (colon != std::string_view::npos) {
         auto name = line.substr(0, colon);
         if (kt::iequals(name, "Content-Length")) {
            auto value = trim_header_value(line.substr(colon + 1));
            size_t length = 0;

            for (auto ch : value) {
               if ((ch < '0') or (ch > '9')) break;
               length = (length * 10) + size_t(ch - '0');
            }

            return length;
         }
      }

      if (line_end IS Headers.size()) break;
      pos = line_end + 2;
   }

   return 0;
}

//********************************************************************************************************************

static bool request_is_complete(std::string_view Request)
{
   auto header_end = Request.find("\r\n\r\n");
   if (header_end IS std::string_view::npos) return false;

   auto content_length = parse_content_length(Request.substr(0, header_end));
   return Request.size() >= header_end + 4 + content_length;
}

//********************************************************************************************************************

static std::string_view get_status_text(int Status)
{
   switch (Status) {
      case 200: return "OK";
      case 201: return "Created";
      case 202: return "Accepted";
      case 204: return "No Content";
      case 400: return "Bad Request";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 500: return "Internal Server Error";
      case 501: return "Not Implemented";
      default:  return "OK";
   }
}

//********************************************************************************************************************

static std::string_view get_request_body(std::string_view Request)
{
   auto header_end = Request.find("\r\n\r\n");
   if (header_end IS std::string_view::npos) return {};

   auto content_length = parse_content_length(Request.substr(0, header_end));
   auto body = Request.substr(header_end + 4);
   if (body.size() > content_length) return body.substr(0, content_length);
   return body;
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
   size_t capture_index = 1;
   size_t search = 0;

   while (capture_index < Captures.size()) {
      auto name_start = Route.path.find('{', search);
      if (name_start IS std::string_view::npos) break;

      auto name_end = Route.path.find('}', name_start + 1);
      if (name_end IS std::string_view::npos) break;

      Params.emplace(std::string(Route.path.substr(name_start + 1, name_end - name_start - 1)),
         std::string(Captures[capture_index]));
      capture_index++;
      search = name_end + 1;
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

static void send_route_response(objClientSocket *Client, const BackstageResponse &Response)
{
   write_response(Client, Response.status, get_status_text(Response.status), Response.body, Response.contentType);
}

//********************************************************************************************************************

static void process_request(objClientSocket *Client, std::string_view Request)
{
   RequestLine line;

   if (not parse_request_line(Request, line)) {
      write_response(Client, 400, "Bad Request", "Bad Request");
      return;
   }

   auto body = get_request_body(Request);
   std::string allowed_methods;

   for (auto &route : glRoutes) {
      RouteMatch match;

      if (not route_matches_path(route, line.path, match)) continue;

      if (route.method.compare(line.method) != 0) {
         append_allowed_method(allowed_methods, route.method);
         continue;
      }

      BackstageRequest request = {
         .route       = &route,
         .server      = (objNetServer *)glServer,
         .client      = Client,
         .method      = std::string(line.method),
         .path        = std::string(line.path),
         .rawPath     = std::string(line.rawPath),
         .queryString = std::string(line.queryString),
         .version     = std::string(line.version)
      };

      if (not body.empty()) {
         auto body_start = (const uint8_t *)body.data();
         request.body.assign(body_start, body_start + body.size());
      }

      parse_query_params(line.queryString, request.queryParams);
      extract_path_params(route, match.captures, request.pathParams);

      BackstageResponse response;
      auto error = route.handler(request, response);

      if (error IS ERR::Okay) send_route_response(Client, response);
      else write_response(Client, 500, "Internal Server Error", "Internal Server Error");
      return;
   }

   if (not allowed_methods.empty()) {
      std::string headers = "Allow: ";
      headers.append(allowed_methods);
      headers.append("\r\n");
      write_response(Client, 405, "Method Not Allowed", "Method Not Allowed", "text/plain", headers);
   }
   else write_response(Client, 404, "Not Found", "Not Found");
}

//********************************************************************************************************************

static void server_feedback(objNetServer *Server, class objClientSocket *Client, NTC State)
{
   kt::Log log(__FUNCTION__);

   if (State IS NTC::CONNECTED) {
      log.msg("Client socket #%d connected.", Client->UID);
   }
   else if (State IS NTC::DISCONNECTED) {
      log.msg("Client socket #%d disconnected.", Client->UID);
      std::lock_guard<std::mutex> lock(glRequestLock);
      glRequestBuffers.erase(Client->UID);
   }
   else log.msg("Unknown state: %d", int(State));
}

//********************************************************************************************************************

static ERR server_incoming(objNetServer *Server, objClientSocket *Client, APTR Meta)
{
   kt::Log log(__FUNCTION__);
   std::array<char, 4096> buffer;
   int len = 0;

   auto error = Client->read(buffer.data(), buffer.size(), &len);
   if (error IS ERR::Disconnected) return ERR::Terminate;
   if (error != ERR::Okay) return error;
   if (not len) return ERR::Okay;

   log.trace("Received %d bytes from client socket #%d", len, Client->UID);

   std::string request;
   bool complete = false;
   bool overflow = false;

   {
      std::lock_guard<std::mutex> lock(glRequestLock);
      auto &client_buffer = glRequestBuffers[Client->UID];
      client_buffer.append(buffer.data(), size_t(len));
      complete = request_is_complete(client_buffer);
      overflow = (not complete) and (client_buffer.find("\r\n\r\n") IS std::string::npos) and
         (client_buffer.size() > MAX_REQUEST_HEADER);
      if (complete or overflow) {
         request = client_buffer;
         glRequestBuffers.erase(Client->UID);
      }
   }

   if (overflow) {
      write_response(Client, 400, "Bad Request", "Bad Request");
      return ERR::Okay;
   }

   if (complete) process_request(Client, request);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR init_backstage(int Port)
{
   kt::Log log(__FUNCTION__);

   if (auto error = compile_backstage_routes(); error != ERR::Okay) return error;

   glServer = objNetServer::create::global({
      FieldValue(FID_Address, "127.0.0.1"),
      fl::Port(Port),
      fl::Flags(int(NSF::MULTI_CONNECT|NSF::KEEP_ALIVE)),
      fl::Feedback((CPTR)server_feedback),
      fl::Incoming((CPTR)server_incoming)
   });

   if (not glServer) {
      log.msg("Failed to initialise backstage server on port %d", Port);
      return ERR::CreateObject;
   }
   else {
      log.msg("Backstage is enabled at http://localhost:%d/", Port);
      return ERR::Okay;
   }
}
