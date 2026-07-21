// Functions for processing incoming HTTP data.

#include <ranges>
#include <algorithm>
#include <span>
#include <optional>
#include <charconv>
#include <concepts>
#include <string_view>
#include <kotuku/modules/tiri.h>

constexpr int CHUNK_BUFFER_SIZE = 32 * 1024;
constexpr int MAX_CHUNK_HEADER_SIZE = 128;

namespace ranges = std::ranges;
namespace views = std::views;

template<typename T>
concept StringLike = requires(T t) {
    { t.data() } -> std::convertible_to<const char*>;
    { t.size() } -> std::convertible_to<size_t>;
    { t.empty() } -> std::convertible_to<bool>;
};

//********************************************************************************************************************
// CRLF detection with concepts

template<StringLike T>
constexpr auto find_crlf_x2(T&& data) -> decltype(data.begin())
{
   constexpr std::array<char, 4> pattern = {'\r', '\n', '\r', '\n'};
   auto result = ranges::search(data, pattern);
   return result.begin();
}

//********************************************************************************************************************
// Header splitting

template<StringLike T>
constexpr std::vector<std::string_view> split_http_headers(T && Headers)
{
   std::vector<std::string_view> result;

   for (auto line_range : Headers | std::views::split(std::string_view{"\r\n"})) {
      std::string_view line(line_range.begin(), line_range.end());
      if (!line.empty()) result.push_back(line);
   }

   return result;
}

//********************************************************************************************************************
// Field parsing for HTTP headers

template<StringLike T>
constexpr std::pair<std::string_view, std::string_view> parse_header_field(T && Line)
   requires requires(T t) { ranges::find(t, ':'); }
{
   auto colon_pos = ranges::find(Line, ':');
   if (colon_pos IS Line.end()) return {std::string_view{}, std::string_view{}};

   auto field_name = std::string_view(Line.begin(), colon_pos);
   auto value_start = colon_pos + 1;

   // Skip whitespace
   while ((value_start != Line.end()) and (uint8_t(*value_start) <= 0x20)) ++value_start;
   auto field_value = std::string_view(value_start, Line.end());
   return {field_name, field_value};
}

//********************************************************************************************************************
// Comma-separated header token matching

static bool header_contains_token(std::string_view Value, std::string_view Token)
{
   for (auto token_range : Value | std::views::split(',')) {
      std::string token(token_range.begin(), token_range.end());
      kt::trim(token);
      if (kt::iequals(token, Token)) return true;
   }

   return false;
}

//********************************************************************************************************************
// Chunk header parsing

static inline std::optional<std::pair<int64_t, size_t>> parse_chunk_header(std::span<const uint8_t> Buffer, size_t Start)
{
   if (Start >= Buffer.size()) return std::nullopt;

   auto chunk_view = std::span(Buffer.begin() + Start, Buffer.end());
   size_t max_scan = std::min(chunk_view.size(), size_t(MAX_CHUNK_HEADER_SIZE));
   auto chunk_str = std::string_view((char *)chunk_view.data(), max_scan);

   // Find CRLF using ranges
   constexpr std::array<char, 2> crlf_pattern = {'\r', '\n'};
   auto crlf_result = ranges::search(chunk_str, crlf_pattern);

   if (crlf_result.begin() IS chunk_str.end()) return std::nullopt; // No CRLF found

   // Extract hex string before CRLF
   auto hex_str = std::string_view(chunk_str.begin(), crlf_result.begin());

   // Parse hex value
   int64_t chunk_length = 0;
   auto [ptr, ec] = std::from_chars(hex_str.data(), hex_str.data() + hex_str.size(), chunk_length, 16);

   if (ec != std::errc{}) return std::nullopt; // Parse error
   while ((ptr != hex_str.data() + hex_str.size()) and ((*ptr IS ' ') or (*ptr IS '\t'))) ptr++;
   if ((ptr != hex_str.data() + hex_str.size()) and (*ptr != ';')) return std::nullopt;

   size_t header_end = Start + std::distance(chunk_str.begin(), crlf_result.end());
   return std::make_pair(chunk_length, header_end);
}

//********************************************************************************************************************
// WWW-Authenticate parsing

template<StringLike T>
constexpr std::vector<std::pair<std::string_view, std::string_view>> parse_auth_fields(T && AuthHeader)
{
   std::vector<std::pair<std::string_view, std::string_view>> result;

   // Remove "Digest " prefix
   std::string_view auth_data = AuthHeader;
   if (auth_data.starts_with("Digest ")) auth_data.remove_prefix(7);

   // Split by commas
   for (auto field_range : auth_data | std::views::split(',')) {
      std::string_view field(field_range.begin(), field_range.end());

      // Trim whitespace
      while (!field.empty() and uint8_t(field.front()) <= 0x20) field.remove_prefix(1);
      while (!field.empty() and uint8_t(field.back()) <= 0x20) field.remove_suffix(1);

      if (field.empty()) continue;

      auto eq_pos = ranges::find(field, '=');
      if (eq_pos IS field.end()) continue;

      auto key = std::string_view(field.begin(), eq_pos);
      auto value = std::string_view(eq_pos + 1, field.end());

      // Remove quotes if present
      if (value.size() >= 2 and value.front() IS '"' and value.back() IS '"') {
         value = value.substr(1, value.size() - 2);
      }

      result.emplace_back(key, value);
   }

   return result;
}

//********************************************************************************************************************

static ERR read_incoming_header(extHTTP *Self, objNetSocket *Socket)
{
   kt::Log log(__FUNCTION__);

   if (not ((Self->CurrentState IS HGS::READING_HEADER) or (Self->CurrentState IS HGS::AUTHENTICATING))) {
      return log.warning(ERR::SanityCheckFailed);
   }

   while (true) {
      if (Self->Response.empty()) Self->Response.resize(512);

      if (Self->ResponseIndex >= std::ssize(Self->Response)) {
         if (Self->Response.size() >= MAX_HEADER_SIZE) {
            log.warning("HTTP response header exceeds maximum size of %d bytes", MAX_HEADER_SIZE);
            Self->Error = ERR::InvalidHTTPResponse;
            return ERR::Terminate;
         }
         Self->Response.resize(std::min(Self->Response.size() + 1024, size_t(MAX_HEADER_SIZE)));
      }

      int len;
      Self->Error = acRead(Socket, std::span<int8_t>((int8_t *)Self->Response.data() + Self->ResponseIndex,
         Self->Response.size() - Self->ResponseIndex), &len);

      if (Self->Error != ERR::Okay) {
         log.warning(Self->Error);
         return ERR::Terminate;
      }

      if (!len) break; // No more incoming data

      #ifdef DEBUG_SOCKET
         write_debug_socket_data(Self->Response.data() + Self->ResponseIndex, len);
      #endif

      Self->ResponseIndex += len;

      std::string_view response_view(Self->Response.c_str(), Self->ResponseIndex);
      auto crlf_iter = find_crlf_x2(response_view);

      if (crlf_iter != response_view.end()) {
         int i = int(std::distance(response_view.begin(), crlf_iter)); // Position of the CRLF pattern
         auto response_header = std::string_view(Self->Response.c_str(), i);
         if (parse_response(Self, response_header) != ERR::Okay) {
            Self->Error = log.warning(ERR::InvalidHTTPResponse);
            Self->setCurrentState(HGS::TERMINATED);
            return ERR::Terminate;
         }

         if (Self->Tunneling) { // Proxy tunneling in progress
            if (Self->Status IS HTS::OKAY) {
               // Proxy tunnel established.  Convert the socket to an SSL connection, then send the HTTP command.

               // Set SSL verification flags before enabling SSL
               if ((Self->Flags & HTF::DISABLE_SERVER_VERIFY) != HTF::NIL) {
                  Socket->Flags |= NSF::DISABLE_SERVER_VERIFY;
               }

               if (!net::SetSSL(Socket, "EnableSSL", "")) {
                  Self->setCurrentState(HGS::COMPLETED);
                  return acActivate(Self);
               }
               else {
                  Self->Error = log.warning(ERR::ConnectionAborted);
                  Self->setCurrentState(HGS::TERMINATED);
                  return ERR::Terminate;
               }
            }
            else {
               Self->Error = log.warning(ERR::ProxySSLTunnel);
               Self->setCurrentState(HGS::TERMINATED);
               return ERR::Terminate;
            }
         }

         if ((Self->CurrentState IS HGS::AUTHENTICATING) and (Self->Status != HTS::UNAUTHORISED)) {
            log.msg("Authentication successful, reactivating...");
            Self->SecurePath = false;
            Self->setCurrentState(HGS::AUTHENTICATED);
            QueueAction(AC::Activate, Self->UID);
            return ERR::Okay;
         }

         if ((Self->Status IS HTS::MOVED_PERMANENTLY) and ((Self->Flags & HTF::NO_AUTO_REDIRECT) IS HTF::NIL)) {
            if ((Self->Flags & HTF::MOVED) != HTF::NIL) {
               // Chaining of MovedPermanently messages is disallowed (could cause circular referencing).

               log.warning("Sequential MovedPermanently messages are not supported.");
            }
            else {
               if (auto it = Self->ResponseHeaders.find("location"); it != Self->ResponseHeaders.end()) {
                  auto &location = it->second;
                  ERR redirect_error;

                  log.msg("MovedPermanently to %s", location.c_str());

                  if (auto active_socket = Self->Socket) {
                     Self->Socket->setFeedback(FUNCTION{});
                     Self->Socket = nullptr;
                     FreeResource(active_socket);
                  }

                  if (location.starts_with("http:")) redirect_error = Self->setLocation(location);
                  else if (location.starts_with("https:")) redirect_error = Self->setLocation(location);
                  else redirect_error = Self->setPath(location);

                  if (redirect_error != ERR::Okay) {
                     Self->Error = redirect_error;
                     Self->setCurrentState(HGS::TERMINATED);
                     return ERR::Terminate;
                  }

                  if (auto activate_error = acActivate(Self); activate_error != ERR::Okay) {
                     Self->Error = activate_error;
                     Self->setCurrentState(HGS::TERMINATED);
                  }

                  Self->Flags |= HTF::MOVED;
                  return ERR::Terminate;
               }
               else {
                  Self->Flags |= HTF::MOVED;
                  log.warning("Invalid MovedPermanently HTTP response received (no location specified).");
               }
            }
         }
         else if (Self->Status IS HTS::TEMP_REDIRECT) {
            if ((Self->Flags & HTF::REDIRECTED) != HTF::NIL) {
               // Chaining of TempRedirect messages is disallowed (could cause circular referencing).

               log.warning("Sequential TempRedirect messages are not supported.");
            }
            else Self->Flags |= HTF::REDIRECTED;
         }

         if ((Self->ContentLength IS 0) and (!Self->Chunked)) {
            log.msg("Response header received, no content imminent.");
            Self->setCurrentState(HGS::COMPLETED);
            return ERR::Terminate;
         }

         log.msg("Complete response header has been received.  Incoming Content: %" PRId64, Self->ContentLength);

         Self->AuthDigest = false;
         if ((Self->Status IS HTS::UNAUTHORISED) and (Self->AuthRetries < MAX_AUTH_RETRIES)) {
            Self->AuthRetries++;

            if (not Self->Password.empty()) {
               // Destroy the current password if it was entered by the user (therefore is invalid) or if it was
               // preset and second authorisation attempt failed (in the case of preset passwords, two
               // authorisation attempts are required in order to receive the 401 from the server first).

               if ((not Self->PasswordPreset) or (Self->AuthRetries >= 2)) {
                  if (not Self->Password.empty()) {
                     secure_clear_memory(const_cast<char*>(Self->Password.data()), Self->Password.size());
                     Self->Password.clear();
                  }
               }
            }

            if (auto auth_it = Self->ResponseHeaders.find("www-authenticate"); auth_it != Self->ResponseHeaders.end()) {
               auto &authenticate = auth_it->second;
               if (kt::startswith("Digest", authenticate)) {
                  log.trace("Digest authentication mode.");

                  Self->Realm.clear();
                  Self->AuthNonce.clear();
                  Self->AuthOpaque.clear();
                  Self->AuthAlgorithm.clear();
                  Self->AuthDigest = true;

                  for (auto [key, value] : parse_auth_fields(authenticate)) {
                     if (key IS "realm") Self->Realm = std::string(value);
                     else if (key IS "nonce") Self->AuthNonce = std::string(value);
                     else if (key IS "opaque") Self->AuthOpaque = std::string(value);
                     else if (key IS "algorithm") Self->AuthAlgorithm = std::string(value);
                     else if (key IS "qop") {
                        Self->AuthQOP = (value.find("auth-int") != std::string_view::npos) ? "auth-int" : "auth";
                     }
                  }
               }
               else log.trace("Basic authentication mode.");
            }
            else log.msg("Authenticate method unknown.");

            if ((Self->Password.empty()) and ((Self->Flags & HTF::NO_DIALOG) IS HTF::NIL)) {
               // Pop up a dialog requesting the user to authorise himself with the http server.  The user will
               // need to respond to the dialog before we can repost the HTTP request.

               Self->setCurrentState(HGS::AUTHENTICATING);

               // TODO: Needs a rewrite using the dialog script
               std::string scriptfile((const char *)glAuthScript, glAuthScriptLength);

               objTiri::create script = { fl::String(scriptfile) };
               if (script.ok()) {
                  AdjustLogLevel(1);
                  auto error = script->activate();
                  AdjustLogLevel(-1);
                  if (error != ERR::Okay) {
                     Self->Error = ERR::Activate;
                     Self->setCurrentState(HGS::TERMINATED);
                  }
                  return error;
               }
               else {
                  Self->Error = log.warning(ERR::CreateObject);
                  Self->setCurrentState(HGS::TERMINATED);
                  return ERR::CreateObject;
               }
            }
            else if (not Self->Password.empty()) {
               kt::Log log(__FUNCTION__);
               log.branch("Reattempting request with preset password.");
               Self->setCurrentState(HGS::AUTHENTICATING);
               // NB: This cancels the reading of any content that followed the header.
               return acActivate(Self);
            }
            else {
               Self->Error = log.warning(ERR::NotAuthorised);
               Self->setCurrentState(HGS::COMPLETED);
               return ERR::NotAuthorised;
            }
         }

         if (Self->CurrentState != HGS::READING_CONTENT) {
            Self->setCurrentState(HGS::READING_CONTENT);
         }

         int header_end = i + 4;
         if (header_end > Self->ResponseIndex) {
            log.warning("Invalid header boundary calculation");
            return ERR::InvalidHTTPResponse;
         }
         len = Self->ResponseIndex - header_end;

         if (Self->Chunked) {
            log.trace("Content to be received in chunks.");
            Self->ChunkIndex     = 0; // Number of bytes processed for the current chunk
            Self->ChunkRemaining = 0;  // Length of the first chunk is unknown at this stage
            Self->ChunkBuffered  = len;
            Self->Chunk.resize(len > CHUNK_BUFFER_SIZE ? len : CHUNK_BUFFER_SIZE);
            if (len > 0) {
               if (header_end + len <= std::ssize(Self->Response)) {
                  kt::copymem(Self->Response.data() + header_end, Self->Chunk.data(), len);
               }
               else return log.warning(ERR::BufferOverflow);
            }
            Self->SearchIndex = 0;
         }
         else {
            log.trace("%" PRId64 " bytes of content is incoming.  Bytes Buffered: %d, Index: %" PRId64, Self->ContentLength, len, Self->Index);

            if (len > 0) {
               if (header_end + len <= std::ssize(Self->Response)) {
                  if (auto output_error = output_incoming_data(Self, Self->Response.data() + header_end, len);
                        output_error != ERR::Okay) {
                     return ERR::Terminate;
                  }
               }
               else return log.warning(ERR::BufferOverflow);
            }
         }

         check_incoming_end(Self);

         Self->Response.clear(); // Buffer no longer required, response key-values are in the Args table.

         // Note that status check comes after processing of content, as it is legal for content to be attached
         // with bad status codes (e.g. SOAP does this).

         if ((int(Self->Status) < 200) or (int(Self->Status) >= 300)) {
            if (Self->CurrentState != HGS::READING_CONTENT) {
               if (Self->Status IS HTS::UNAUTHORISED) log.warning("Exhausted maximum number of retries.");
               else log.warning("Status code %d != 2xx", int(Self->Status));

               Self->Error = ERR::Failed;
               return ERR::Terminate;
            }
            else log.warning("Status code %d != 2xx.  Receiving content...", int(Self->Status));
         }

         return ERR::Okay; // Response header has been read, process any remaining data
      }

      // Update SearchIndex to avoid re-scanning, but preserve boundary detection capability
      Self->SearchIndex = (Self->ResponseIndex >= 3) ? Self->ResponseIndex - 3 : 0;
      log.trace("Partial HTTP header received, awaiting full header...");
   }

   return ERR::Continue; // More data needed
}

//********************************************************************************************************************
// Data chunk mode.  Store received data in a chunk buffer.  As long as we know the entire size of the chunk, all
// data can be immediately passed onto our subscribers.
//
// Chunked data is passed as follows:
//
// ChunkSize\r\n
// Data....
// ChunkSize\r\n
// Data...
// \r\n (indicates end) or 0\r\n (indicates end of chunks with further HTTP tags following)
//
// ChunkIndex:     Current read position within the buffer.
// ChunkBuffered:  Number of bytes currently buffered.
// ChunkRemaining: Unprocessed bytes in this chunk (decreases as bytes are processed).

static ERR read_incoming_chunks(extHTTP *Self, objNetSocket *Socket)
{
   // Adaptive pass limit based on chunk buffer utilization
   int max_passes = 2;
   if (Self->ChunkBuffered > (int(Self->Chunk.size()) / 2)) {
      max_passes = 3; // Allow more passes when buffer is fuller
   }

   for (int count = max_passes; count > 0; count--) { // Make multiple passes in case there's more data than fits in the buffer
      kt::Log log("http_incoming");
      log.traceBranch("Receiving content (chunk mode) Index: %d/%d/%d, Remaining: %d", Self->ChunkIndex, Self->ChunkBuffered, int(Self->Chunk.size()), Self->ChunkRemaining);

      // Compress or clear the buffer

      if (Self->ChunkIndex > 0) {
         if (Self->ChunkBuffered > Self->ChunkIndex) {
            log.trace("Compressing the chunk buffer.");
            kt::copymem(Self->Chunk.data() + Self->ChunkIndex, Self->Chunk.data(), Self->ChunkBuffered - Self->ChunkIndex);
            Self->ChunkBuffered -= Self->ChunkIndex;
         }
         else Self->ChunkBuffered = 0;
         Self->ChunkIndex = 0;
      }

      // Fill the chunk buffer

      if (Self->ChunkBuffered < int(Self->Chunk.size())) {
         int read_bytes;
         Self->Error = acRead(Socket, std::span<int8_t>((int8_t *)Self->Chunk.data() + Self->ChunkBuffered,
            Self->Chunk.size() - Self->ChunkBuffered), &read_bytes);

         #ifdef DEBUG_SOCKET
            write_debug_socket_data(Self->Chunk.data() + Self->ChunkBuffered, read_bytes);
         #endif

         log.trace("Filling the chunk buffer: Read %d bytes.", read_bytes);

         if (Self->Error IS ERR::Disconnected) {
            log.detail("Received all chunked content (disconnected by peer).");
            Self->setCurrentState(HGS::COMPLETED);
            return ERR::Terminate;
         }
         else if (Self->Error != ERR::Okay) {
            log.warning("Read() returned error %d whilst reading content.", int(Self->Error));
            Self->setCurrentState(HGS::COMPLETED);
            return ERR::Terminate;
         }
         else if ((!read_bytes) and (Self->ChunkIndex >= Self->ChunkBuffered)) {
            log.detail("Nothing left to read.");
            return ERR::Okay;
         }
         else Self->ChunkBuffered += read_bytes;
      }

      while (Self->ChunkIndex < Self->ChunkBuffered) {
         log.trace("Status: Index: %d/%d, CurrentChunk: %d", Self->ChunkIndex, Self->ChunkBuffered, Self->ChunkRemaining);

         if (!Self->ChunkRemaining) {
            // Read the next chunk header.  It is assumed that the format is:
            //
            // ChunkSize\r\n
            // Data...

            log.trace("Examining chunk header (%d bytes buffered).", Self->ChunkBuffered - Self->ChunkIndex);

            std::span<const uint8_t> chunk_buffer(Self->Chunk.data(), Self->ChunkBuffered);
            auto chunk_result = parse_chunk_header(chunk_buffer, Self->ChunkIndex);

            if (chunk_result.has_value()) {
               auto [temp_chunk_len, header_end] = chunk_result.value();

               // Validate chunk length
               if ((temp_chunk_len < 0) or (temp_chunk_len > MAX_CHUNK_LENGTH)) {
                  if (temp_chunk_len > 0) {
                     log.warning("Chunk length %d exceeds maximum %d terminating", int(temp_chunk_len), int(MAX_CHUNK_LENGTH));
                     Self->setCurrentState(HGS::TERMINATED);
                     return ERR::Terminate;
                  }
               }
               Self->ChunkRemaining = temp_chunk_len;

               if (Self->ChunkRemaining <= 0) {
                  if (Self->Chunk[Self->ChunkIndex] IS '0') {
                     // A line of "0\r\n" indicates an end to the chunks, followed by optional data for
                     // interpretation.

                     log.detail("End of chunks reached, optional data follows.");
                     Self->setCurrentState(HGS::COMPLETED);
                     return ERR::Terminate;
                  }
                  else {
                     // We have reached the terminating line (CRLF on an empty line)
                     log.trace("Received all chunked content.");
                     Self->setCurrentState(HGS::COMPLETED);
                     return ERR::Terminate;
                  }
               }

               log.trace("Next chunk length is %d bytes.", Self->ChunkRemaining);
               Self->ChunkIndex = int(header_end); // Move past the header
            }
            else {
               // Check if we've searched too far without finding \r\n (prevent DoS)
               if ((Self->ChunkBuffered - Self->ChunkIndex) > MAX_CHUNK_HEADER_SIZE) {
                  log.warning("Chunk header exceeds maximum size of %d bytes", MAX_CHUNK_HEADER_SIZE);
                  Self->setCurrentState(HGS::TERMINATED);
                  return ERR::Terminate;
               }
               // No CRLF found, need more data
            }

            // Quit the main loop if we still don't have a chunk length (more data needs to be read from the HTTP socket).

            if (!Self->ChunkRemaining) break;
         }

         if (Self->ChunkRemaining > 0) {
            auto len = Self->ChunkBuffered - Self->ChunkIndex;
            if (len > Self->ChunkRemaining) len = Self->ChunkRemaining; // Cannot process more bytes than the expected chunk length

            log.trace("%d bytes yet to process, outputting %d bytes", Self->ChunkRemaining, len);

            Self->ChunkRemaining -= len;
            if (auto output_error = output_incoming_data(Self, Self->Chunk.data() + Self->ChunkIndex, len);
                  output_error != ERR::Okay) {
               return ERR::Terminate;
            }

            Self->ChunkIndex += len;

            if (!Self->ChunkRemaining) { // The end of the chunk binary is followed with a CRLF
               log.trace("A complete chunk has been processed.");
               Self->ChunkRemaining = -2;
            }
         }

         if (Self->ChunkRemaining < 0) {
            log.trace("Skipping %d bytes.", -Self->ChunkRemaining);

            while ((Self->ChunkRemaining < 0) and (Self->ChunkIndex < Self->ChunkBuffered)) {
               auto expected = (Self->ChunkRemaining IS -2) ? '\r' : '\n';
               if (Self->Chunk[Self->ChunkIndex] != expected) {
                  log.warning("Invalid chunk delimiter.");
                  Self->setCurrentState(HGS::TERMINATED);
                  return ERR::Terminate;
               }

               Self->ChunkIndex++;
               Self->ChunkRemaining++;
            }

            if (Self->ChunkRemaining < 0) break; // If we did not receive all the bytes, break to continue processing until more bytes are ready
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR read_incoming_content(extHTTP *Self, objNetSocket *Socket)
{
   kt::Log log(__FUNCTION__);

   std::vector<char> buffer(BUFFER_READ_SIZE);

   // Adaptive loop limits based on content size and network conditions
   // For small content, use fewer iterations; for large content or streaming, use more

   int base_loop_limit = (64 * 1024) / BUFFER_READ_SIZE;
   int adaptive_loop_limit;

   if (Self->ContentLength > 0) {
      // Known content length - calculate optimal iterations
      int64_t expected_iterations = (Self->ContentLength - Self->Index + BUFFER_READ_SIZE - 1) / BUFFER_READ_SIZE;
      adaptive_loop_limit = std::min(int64_t(base_loop_limit * 2), std::max(int64_t(4), expected_iterations));
   } else {
      // Unknown content length (streaming) - use adaptive approach
      adaptive_loop_limit = base_loop_limit;
   }

   // Adaptive time limit based on expected data volume
   int64_t base_time_limit = 5000000LL; // 5ms
   int64_t adaptive_time_limit = (Self->ContentLength > 1024 * 1024) ? base_time_limit * 2 : base_time_limit;
   int64_t timelimit = PreciseTime() + adaptive_time_limit;

   while (true) {
      int len = BUFFER_READ_SIZE;
      if (Self->ContentLength != -1) {
         if (len > Self->ContentLength - Self->Index) len = Self->ContentLength - Self->Index;
      }

      if ((Self->Error = acRead(Socket, std::span<int8_t>((int8_t *)buffer.data(), len), &len)) != ERR::Okay) {
         #ifdef DEBUG_SOCKET
            write_debug_socket_data(buffer.data(), len);
         #endif

         if ((Self->Error IS ERR::Disconnected) and (Self->ContentLength IS -1)) {
            log.trace("Received all streamed content (disconnected by peer).");
            Self->setCurrentState(HGS::COMPLETED);
            return ERR::Terminate;
         }
         else {
            log.warning("Read() returned error %d whilst reading content.", int(Self->Error));
            return ERR::Terminate;
         }
      }

      if (!len) break; // No more incoming data right now

      if (auto output_error = output_incoming_data(Self, buffer.data(), len); output_error != ERR::Okay) {
         return ERR::Terminate;
      }
      if (check_incoming_end(Self) IS ERR::True) {
         return ERR::Terminate;
      }

      if (--adaptive_loop_limit <= 0) break;
      if (PreciseTime() > timelimit) break;
   }

   Self->LastReceipt = PreciseTime();

   if (Self->TimeoutManager) UpdateTimer(Self->TimeoutManager, Self->DataTimeout);
   else SubscribeTimer(Self->DataTimeout, C_FUNCTION(http_timeout), &Self->TimeoutManager);

   if (Self->Error != ERR::Okay) return ERR::Terminate;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR parse_response(extHTTP *Self, std::string_view Response)
{
   kt::Log log(__FUNCTION__);

   Self->ResponseHeaders.clear();

   log.detail("HTTP RESPONSE HEADER\n%.*s", int(Response.size()), Response.data());

   // First line: HTTP/1.1 200 OK

   if (!Response.starts_with("HTTP/")) {
      log.warning("Unsupported HTTP header.");
      return ERR::InvalidHTTPResponse;
   }

   Response.remove_prefix(5);
   if (Response.starts_with("1.1"))      Self->ResponseVersion = 0x11;
   else if (Response.starts_with("1.0")) Self->ResponseVersion = 0x10;
   else if (Response.starts_with("2.0")) Self->ResponseVersion = 0x20;
   else if (Response.starts_with("3.0")) Self->ResponseVersion = 0x30;
   else return log.warning(ERR::InvalidHTTPResponse);

   if (auto pos = Response.find(' '); pos != std::string_view::npos) {
      Response.remove_prefix(pos + 1); // skip the ' '
   }
   else return log.warning(ERR::InvalidHTTPResponse);

   int code = 0;
   auto [ ptr, error ] = std::from_chars(Response.data(), Response.data() + Response.size(), code);
   if (error IS std::errc()) Self->Status = HTS(code);
   else Self->Status = HTS::NIL;

   if (not Self->ProxyServer.empty()) Self->ContentLength = -1; // Some proxy servers (Squid) strip out information like 'transfer-encoding' yet pass all the requested content anyway :-/
   else Self->ContentLength = 0;
   Self->Chunked = false;

   // Parse response fields

   auto end_line = Response.find("\r\n");
   if (end_line IS std::string::npos) return ERR::Okay;
   Response.remove_prefix(end_line + 2);

   log.msg("HTTP response header received, status code %d", code);

   for (auto header_line : split_http_headers(Response)) {
      auto [field_name, field_value] = parse_header_field(header_line);

      if (field_name.empty()) continue;

      // Convert field name to lowercase
      std::string field_key;
      field_key.reserve(field_name.size());
      ranges::transform(field_name, std::back_inserter(field_key), [](char c) { return char(std::tolower(c)); });

      Self->ResponseHeaders[field_key] = std::string(field_value);
   }

   if (auto it = Self->ResponseHeaders.find("content-length"); it != Self->ResponseHeaders.end()) {
      auto &value = it->second;
      Self->ContentLength = 0;
      int64_t temp_length = 0;
      auto [ ptr, error ] = std::from_chars(value.data(), value.data() + value.size(), temp_length);
      while ((ptr != value.data() + value.size()) and (uint8_t(*ptr) <= 0x20)) ptr++;
      if ((error IS std::errc()) and (ptr IS value.data() + value.size()) and
            (temp_length >= 0) and (temp_length <= MAX_CONTENT_LENGTH)) {
         Self->ContentLength = temp_length;
      }
      else {
         log.warning("Invalid or excessive Content-Length: %.*s", int(value.size()), value.data());
         Self->ContentLength = -1; // Treat as streaming
      }
   }

   if (auto it = Self->ResponseHeaders.find("transfer-encoding"); it != Self->ResponseHeaders.end()) {
      auto &value = it->second;
      if (header_contains_token(value, "chunked")) {
         if ((Self->Flags & HTF::RAW) IS HTF::NIL) Self->Chunked = true;
         Self->ContentLength = -1;
      }
   }

   // Determine the keep-alive status according the default HTTP protocol rules and then consider any connection value.

   if (Self->ResponseVersion >= 0x11) Self->KeepAlive = true;
   else Self->KeepAlive = false;

   if (auto it = Self->ResponseHeaders.find("connection"); it != Self->ResponseHeaders.end()) {
      // HTTP/1.0 if keep-alive is not specified then the connection is closed by default.
      // HTTP/1.1 if keep-alive is not specified then the connection is persistent
      //
      // If close is specified then the server should disconnect its end first, but
      // being pro-active and disconnecting our side early will keep things predictable.

      auto &value = it->second;
      if (kt::iequals(value, "close")) {
         Self->KeepAlive = false;
      }
      else if (kt::iequals(value, "keep-alive")) {
         Self->KeepAlive = true;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Sends buffered data to the listener

static ERR output_incoming_data(extHTTP *Self, APTR Buffer, int Length)
{
   kt::Log log(__FUNCTION__);

   log.trace("Buffer: %p, Length: %d", Buffer, Length);

   if (!Length) return ERR::Okay;

   Self->setIndex(Self->Index + Length); // Use Set() so that field subscribers can track progress with field monitoring

   if ((!Self->flOutput) and (not Self->OutputFile.empty())) {
      FL flags;
      LOC type;

      if ((Self->Flags & HTF::RESUME) != HTF::NIL) {
         if ((!AnalysePath(Self->OutputFile, &type)) and (type IS LOC::FILE)) {
            flags = FL::NIL;
         }
         else flags = FL::NEW;
      }
      else flags = FL::NEW;

      if ((Self->flOutput = objFile::create::local(fl::Path(Self->OutputFile), fl::Flags(flags|FL::WRITE)))) {
         if ((Self->Flags & HTF::RESUME) != HTF::NIL) {
            acSeekEnd(Self->flOutput, 0);
            Self->setIndex(0);
         }
      }
      else {
         Self->Error = ERR::CreateFile;
         return Self->Error;
      }
   }

   if (Self->flOutput) {
      int result = 0;
      if (auto error = Self->flOutput->write(std::span<const int8_t>((const int8_t *)Buffer, Length), &result);
          error != ERR::Okay) {
         Self->Error = error;
         return Self->Error;
      }
      else if (result != Length) {
         Self->Error = ERR::Write;
         return Self->Error;
      }
   }

   if ((Self->Flags & HTF::RECV_BUFFER) != HTF::NIL) {
      Self->RecvBuffer.append(std::string_view((char *)Buffer, Length));
   }

   if (Self->Incoming.stale()) clear_callback_function(Self->Incoming);

   if (Self->Incoming.defined()) {
      log.trace("Incoming callback is set.");

      ERR error;
      if (Self->Incoming.isC()) {
         auto routine = (ERR (*)(extHTTP *, APTR, int, APTR))Self->Incoming.Routine;
         error = routine(Self, Buffer, Length, Self->Incoming.Meta);
      }
      else if (Self->Incoming.isScript()) {
         // For speed, the client will receive a direct pointer to the buffer memory via the 'mem' interface.

         log.trace("Calling script procedure %" PRId64, Self->Incoming.ProcedureID);

         if (sc::Call(Self->Incoming, std::to_array<ScriptArg>({
               { "HTTP",       Self,   FD_OBJECTPTR },
               { "Buffer",     Buffer, FD_PTRBUFFER },
               { "BufferSize", Length, FD_INT|FD_BUFSIZE }
            }), error) != ERR::Okay) error = ERR::Terminate;
      }
      else error = ERR::InvalidValue;

      if (error > ERR::ExceptionThreshold) Self->Error = error;

      if (error IS ERR::Terminate) {
         kt::Log log(__FUNCTION__);
         log.branch("Client changing state to HGS::TERMINATED.");
         Self->setCurrentState(HGS::TERMINATED);
      }
   }

   if (Self->OutputObjectID) {
      if (Self->ObjectMode IS HOM::DATA_FEED) {
         kt::ScopedObjectLock output(Self->OutputObjectID);
         if (output.granted()) {
            if (auto error = acDataFeed(*output, Self, Self->Datatype,
                  std::span<const int8_t>((const int8_t *)Buffer, size_t(Length))); error != ERR::Okay) {
               Self->Error = error;
               return Self->Error;
            }
         }
         else {
            Self->Error = ERR::Lock;
            return Self->Error;
         }
      }
      else if (Self->ObjectMode IS HOM::READ_WRITE) {
         kt::ScopedObjectLock output(Self->OutputObjectID);
         if (output.granted()) {
            int result = 0;
            if (auto error = acWrite(*output, std::span<const int8_t>((const int8_t *)Buffer, Length), &result);
                error != ERR::Okay) {
               Self->Error = error;
               return Self->Error;
            }
            else if (result != Length) {
               Self->Error = ERR::Write;
               return Self->Error;
            }
         }
         else {
            Self->Error = ERR::Lock;
            return Self->Error;
         }
      }
   }

   return Self->Error;
}

//********************************************************************************************************************
// Callback: NetSocket.Incoming

static ERR socket_incoming(objNetSocket *Socket)
{
   kt::Log log("http_incoming");

   auto Self = (extHTTP *)Socket->ClientData;

   if (Self->classID() != CLASSID::HTTP) return log.warning(ERR::SystemCorrupt);

restart:

   if (Self->CurrentState >= HGS::COMPLETED) {
      // Erroneous data received from server while we are in a completion/resting state.  Returning a terminate message
      // will cause the socket object to close the connection to the server so that we stop receiving erroneous data.
      // NB: This isn't considered a problem; changing to a completed state is permitted mid transfer.
      log.detail("Unwanted data remains on socket - terminating.");
      return ERR::Terminate;
   }

   if (Self->CurrentState IS HGS::SENDING_CONTENT) {
      // Sanity check failed - we should not be receiving data while we are sending content to the server.
      if (Self->ContentLength IS -1) {
         log.warning("Incoming data while streaming content - %" PRId64 " bytes already written.", Self->Index);
      }
      else if (Self->Index < Self->ContentLength) {
         log.warning("Incoming data while sending content - only %" PRId64 "/%" PRId64 " bytes written!", Self->Index, Self->ContentLength);
      }
   }

   if ((Self->CurrentState IS HGS::SENDING_CONTENT) or (Self->CurrentState IS HGS::SEND_COMPLETE)) {
      log.trace("Transition SENDING_CONTENT -> READING_HEADER.");
      Self->setCurrentState(HGS::READING_HEADER);
      Self->Index = 0;
   }

   if ((Self->CurrentState IS HGS::READING_HEADER) or (Self->CurrentState IS HGS::AUTHENTICATING)) {
      auto error = read_incoming_header(Self, Socket);
      if (!error) goto restart; // Header read, process any remaining data
      else return error;
   }
   else if (Self->CurrentState IS HGS::READING_CONTENT) {
      if (Self->Chunked) return read_incoming_chunks(Self, Socket);
      else return read_incoming_content(Self, Socket);
   }
   else { // Unexpected data received from HTTP server
      std::string buffer;
      buffer.resize(512);
      int len;
      if ((!acRead(Socket, std::span<int8_t>((int8_t *)buffer.data(), buffer.size()), &len)) and (len > 0)) {
         log.warning("WARNING: Received data whilst in state %d.", int(Self->CurrentState));
         log.warning("Content (%d bytes) Follows:\n%.80s", len, buffer.c_str());
      }
      return ERR::Terminate;
   }

   return ERR::Okay;
}
