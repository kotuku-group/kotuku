
enum class HttpBufferState {
   INCOMPLETE,
   COMPLETE,
   BAD_REQUEST,
   HEADER_TOO_LARGE,
   BODY_TOO_LARGE
};

//********************************************************************************************************************

enum class HttpParseStatus {
   OKAY,
   BAD_REQUEST,
   PAYLOAD_TOO_LARGE
};

//********************************************************************************************************************

struct RequestLine {
   std::string_view method;
   std::string_view path;
   std::string_view rawPath;
   std::string_view queryString;
   std::string_view version;

   bool parse(std::string_view Request);
};

//********************************************************************************************************************

struct BackstageHttpRequest {
   RequestLine line;
   std::string_view headers;
   std::string_view body;
   std::string_view content_type;
   bool has_content_length = false;

   static HttpBufferState analyse_buffer(std::string_view Request);
   static std::string_view body_view(std::string_view Request, size_t ContentLength);
   HttpParseStatus parse(std::string_view RawRequest);
};

//********************************************************************************************************************

struct BackstageHttpResponse {
   int status = 200;
   std::string_view status_text = "OK";
   std::string content_type = "application/json";
   std::string body;
   std::string extra_headers;

   static std::string_view status_text_for(int Status);
   static BackstageHttpResponse plain(int Status, std::string_view Body);
   static BackstageHttpResponse from_route(const BackstageResponse &Response);

   void write(objClientSocket *Client) const;
};

//********************************************************************************************************************

struct HttpHeaders {
   std::string_view headers;

   explicit constexpr HttpHeaders(std::string_view Headers) : headers(Headers) {}

   static std::string_view trim_value(std::string_view Value);
   static bool name_is(std::string_view Name, const char *Expected);

   template <class Callback> void for_each(Callback CallbackFn) const;

   std::string_view value(const char *HeaderName) const;
};

//********************************************************************************************************************

struct HttpBodyInfo {
   size_t content_length = 0;
   bool has_content_length = false;
   bool malformed_content_length = false;
   bool unsupported_transfer_encoding = false;

   static bool parse_size_header(std::string_view Value, size_t &Result);
   static HttpBodyInfo parse(std::string_view Headers);
};

//********************************************************************************************************************

bool RequestLine::parse(std::string_view Request)
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
   if (not (version_end IS std::string_view::npos)) return false;

   method = request_line.substr(0, method_end);
   rawPath = request_line.substr(path_start, path_end - path_start);
   path = rawPath;
   queryString = {};
   version = request_line.substr(version_start);

   if (not version.starts_with("HTTP/")) return false;

   auto query = path.find('?');
   if (not (query IS std::string_view::npos)) {
      queryString = path.substr(query + 1);
      path = path.substr(0, query);
   }

   return true;
}

//********************************************************************************************************************

std::string_view HttpHeaders::trim_value(std::string_view Value)
{
   while ((not Value.empty()) and ((Value.front() IS ' ') or (Value.front() IS '\t'))) Value.remove_prefix(1);
   while ((not Value.empty()) and ((Value.back() IS ' ') or (Value.back() IS '\t'))) Value.remove_suffix(1);
   return Value;
}

//********************************************************************************************************************

bool HttpHeaders::name_is(std::string_view Name, const char *Expected)
{
   return kt::iequals(Name, Expected);
}

//********************************************************************************************************************

template <class Callback> void HttpHeaders::for_each(Callback CallbackFn) const
{
   size_t pos = 0;

   while (pos < headers.size()) {
      auto line_end = headers.find("\r\n", pos);
      if (line_end IS std::string_view::npos) line_end = headers.size();

      auto line = headers.substr(pos, line_end - pos);
      auto colon = line.find(':');
      if (not (colon IS std::string_view::npos)) {
         auto name = line.substr(0, colon);
         auto value = trim_value(line.substr(colon + 1));
         if (not CallbackFn(name, value)) return;
      }

      if (line_end IS headers.size()) break;
      pos = line_end + 2;
   }
}

//********************************************************************************************************************

std::string_view HttpHeaders::value(const char *HeaderName) const
{
   std::string_view result;

   for_each([HeaderName, &result](std::string_view Name, std::string_view Value) {
      if (name_is(Name, HeaderName)) {
         result = Value;
         return false;
      }

      return true;
   });

   return result;
}

//********************************************************************************************************************

bool HttpBodyInfo::parse_size_header(std::string_view Value, size_t &Result)
{
   auto value = HttpHeaders::trim_value(Value);
   if (value.empty()) return false;

   size_t length = 0;
   auto value_end = value.data() + value.size();
   auto conversion = std::from_chars(value.data(), value_end, length);
   if (not (conversion.ec IS std::errc())) return false;
   if (not (conversion.ptr IS value_end)) return false;

   Result = length;
   return true;
}

//********************************************************************************************************************

HttpBodyInfo HttpBodyInfo::parse(std::string_view Headers)
{
   HttpBodyInfo info;

   HttpHeaders(Headers).for_each([&info](std::string_view Name, std::string_view Value) {
      if (HttpHeaders::name_is(Name, "Content-Length")) {
         size_t length = 0;
         if (not parse_size_header(Value, length)) info.malformed_content_length = true;
         else if ((not info.has_content_length) or (info.content_length IS length)) {
            info.has_content_length = true;
            info.content_length = length;
         }
         else info.malformed_content_length = true;
      }
      else if (HttpHeaders::name_is(Name, "Transfer-Encoding")) {
         if ((not Value.empty()) and (not kt::iequals(Value, "identity"))) {
            info.unsupported_transfer_encoding = true;
         }
      }

      return true;
   });

   return info;
}

//********************************************************************************************************************

HttpBufferState BackstageHttpRequest::analyse_buffer(std::string_view Request)
{
   auto header_end = Request.find("\r\n\r\n");
   if (header_end IS std::string_view::npos) {
      if (Request.size() > MAX_REQUEST_HEADER) return HttpBufferState::HEADER_TOO_LARGE;
      return HttpBufferState::INCOMPLETE;
   }

   auto body_info = HttpBodyInfo::parse(Request.substr(0, header_end));
   if (body_info.malformed_content_length or body_info.unsupported_transfer_encoding) {
      return HttpBufferState::BAD_REQUEST;
   }

   if (body_info.content_length > MAX_REQUEST_BODY) return HttpBufferState::BODY_TOO_LARGE;
   if (Request.size() >= header_end + 4 + body_info.content_length) return HttpBufferState::COMPLETE;
   return HttpBufferState::INCOMPLETE;
}

//********************************************************************************************************************

std::string_view BackstageHttpRequest::body_view(std::string_view Request, size_t ContentLength)
{
   auto header_end = Request.find("\r\n\r\n");
   if (header_end IS std::string_view::npos) return {};

   auto body = Request.substr(header_end + 4);
   if (body.size() > ContentLength) return body.substr(0, ContentLength);
   return body;
}

//********************************************************************************************************************

HttpParseStatus BackstageHttpRequest::parse(std::string_view RawRequest)
{
   auto header_end = RawRequest.find("\r\n\r\n");
   if (header_end IS std::string_view::npos) return HttpParseStatus::BAD_REQUEST;

   headers = RawRequest.substr(0, header_end);
   auto body_info = HttpBodyInfo::parse(headers);

   if ((body_info.malformed_content_length) or (body_info.unsupported_transfer_encoding)) {
      return HttpParseStatus::BAD_REQUEST;
   }

   if (body_info.content_length > MAX_REQUEST_BODY) return HttpParseStatus::PAYLOAD_TOO_LARGE;
   if (not line.parse(RawRequest)) return HttpParseStatus::BAD_REQUEST;

   has_content_length = body_info.has_content_length;
   content_type = HttpHeaders(headers).value("Content-Type");
   body = body_view(RawRequest, body_info.content_length);
   return HttpParseStatus::OKAY;
}

//********************************************************************************************************************

std::string_view BackstageHttpResponse::status_text_for(int Status)
{
   switch (Status) {
      case 200: return "OK";
      case 201: return "Created";
      case 202: return "Accepted";
      case 204: return "No Content";
      case 400: return "Bad Request";
      case 411: return "Length Required";
      case 413: return "Payload Too Large";
      case 415: return "Unsupported Media Type";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 500: return "Internal Server Error";
      case 501: return "Not Implemented";
      default:  return "OK";
   }
}

//********************************************************************************************************************

BackstageHttpResponse BackstageHttpResponse::plain(int Status, std::string_view Body)
{
   BackstageHttpResponse response;
   response.status = Status;
   response.status_text = status_text_for(Status);
   response.content_type = "text/plain";
   response.body = Body;
   return response;
}

//********************************************************************************************************************

BackstageHttpResponse BackstageHttpResponse::from_route(const BackstageResponse &Response)
{
   BackstageHttpResponse http_response;
   http_response.status = Response.status;
   http_response.status_text = status_text_for(Response.status);
   http_response.content_type = Response.contentType;
   http_response.body = Response.body;
   return http_response;
}

//********************************************************************************************************************

void BackstageHttpResponse::write(objClientSocket *Client) const
{
   if (status <= 0) return;

   std::string response;
   response.reserve(128 + body.size() + extra_headers.size());
   response.append("HTTP/1.1 ");
   response.append(std::to_string(status));
   response.append(" ");
   response.append(status_text);
   response.append("\r\nContent-Type: ");
   response.append(content_type);
   response.append("\r\nContent-Length: ");
   response.append(std::to_string(body.size()));
   response.append("\r\nConnection: close\r\n");
   response.append(extra_headers);
   response.append("\r\n");
   response.append(body);

   int result;
   Client->write(response.c_str(), int(response.size()), &result);
   Client->deactivate();
}
