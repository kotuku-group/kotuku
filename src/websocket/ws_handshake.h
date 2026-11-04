// WebSocket opening handshake helpers (RFC 6455 section 4).
//
// Header value parsing is plain C++.  Key generation and accept-key computation use the Crypto module, which the
// WebSocket module loads in MODInit().

#pragma once

#include <kotuku/main.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ws {

constexpr std::string_view HANDSHAKE_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::string_view WEBSOCKET_VERSION = "13";
constexpr size_t KEY_NONCE_SIZE = 16;
constexpr size_t KEY_LENGTH     = 24; // Base64 length of a 16 byte nonce
constexpr size_t ACCEPT_LENGTH  = 28; // Base64 length of a 20 byte SHA-1 digest

// Generates a Sec-WebSocket-Key value: 16 random bytes in standard base64.

ERR generate_key(std::string &Key);

// Computes the Sec-WebSocket-Accept value for Key: base64(SHA1(Key + HANDSHAKE_GUID)).  Key is used verbatim.

ERR compute_accept_key(std::string_view Key, std::string &Accept);

// True if Key is a canonical standard base64 encoding of exactly 16 bytes.

[[nodiscard]] bool valid_key(std::string_view Key);

// True if a Sec-WebSocket-Version value is exactly 13, ignoring surrounding whitespace.

[[nodiscard]] bool valid_version(std::string_view Value);

// True if Value is a non-empty HTTP token (RFC 9110 section 5.6.2).

[[nodiscard]] bool is_token(std::string_view Value);

// Removes leading and trailing spaces and horizontal tabs.

[[nodiscard]] std::string_view trim_ows(std::string_view Value);

// Splits a comma-separated header value into its tokens, trimming whitespace and skipping empty elements.  Tokens
// refer to Value.  Returns false if any element is not a valid token, in which case Tokens is cleared.

bool parse_token_list(std::string_view Value, std::vector<std::string_view> &Tokens);

// True if the comma-separated token list in Value contains Token, compared case-insensitively.  Intended for the
// Connection and Upgrade headers.  An invalid list never matches.

[[nodiscard]] bool has_token(std::string_view Value, std::string_view Token);

//********************************************************************************************************************
// Client handshake support

// The components of a ws:// or wss:// URI (RFC 6455 section 3).

struct Location {
   std::string host;    // Host name or IP literal, without IPv6 brackets
   std::string target;  // Request target: the path and query, always starting with '/'
   int port = 0;        // Explicit port, otherwise 80 for ws:// and 443 for wss://
   bool secure = false; // True for wss://
};

// Parses a ws:// or wss:// URI.  The scheme is case-insensitive and the path and query are preserved verbatim.  Returns
// false for other schemes, fragments, user information, an empty host, malformed IPv6 brackets, an invalid port, or
// any byte outside printable ASCII.

bool parse_location(std::string_view URI, Location &Result);

// A response header field.  Both members refer to the parsed string.

struct HeaderField {
   std::string_view name;
   std::string_view value; // Surrounding whitespace removed
};

// Parses the output of HTTP's GetResponseHeaders(): one field per LF-terminated line, split at the first colon.  Field
// order and repeated fields are preserved.  Returns false if a line has no colon or its name is not a token, in which
// case Fields is cleared.

bool parse_header_fields(std::string_view Headers, std::vector<HeaderField> &Fields);

// Validates the WebSocket-specific fields of a 101 response (RFC 6455 section 4.1).  Requires exactly one
// Sec-WebSocket-Accept matching ExpectedAccept, at most one Sec-WebSocket-Protocol naming one of the Requested
// subprotocols, and no Sec-WebSocket-Extensions.  Returns an empty view on success, setting Selected to the chosen
// subprotocol (empty if none), otherwise a static description of the failure.

std::string_view validate_response(std::span<const HeaderField> Fields, std::string_view ExpectedAccept,
   std::span<const std::string_view> Requested, std::string_view &Selected);

//********************************************************************************************************************
// Server handshake support

constexpr int STATUS_BAD_REQUEST      = 400;
constexpr int STATUS_FORBIDDEN        = 403;
constexpr int STATUS_NOT_FOUND        = 404;
constexpr int STATUS_UPGRADE_REQUIRED = 426;
constexpr int STATUS_HEADERS_TOO_BIG  = 431;

// Returns the size of the request head in Data, including the blank line that terminates it, or zero if the blank line
// has not been received.  Lines may end with CRLF or a bare LF.

[[nodiscard]] size_t find_request_end(std::string_view Data);

// The components of an opening handshake request.  Every view refers to the parsed request head.

struct Request {
   std::string_view target;                // Request target as received, for example /chat?room=1
   std::string_view path;                  // Target up to the first '?'
   std::string_view query;                 // Target after the first '?', excluding it
   std::string_view host;
   std::string_view key;                   // Sec-WebSocket-Key
   std::string_view origin;                // Origin, empty if absent
   std::vector<std::string_view> protocols; // Requested subprotocols, in the client's order of preference
   std::vector<HeaderField> fields;        // Every header field, in the order received
};

// Parses and validates an opening handshake request (RFC 6455 section 4.2.1).  Head is the request through its blank
// line, as measured by find_request_end().  The request must be `GET <origin-form> HTTP/1.1` with exactly one Host,
// an Upgrade field containing `websocket`, a Connection field containing `Upgrade`, exactly one Sec-WebSocket-Version
// and exactly one valid Sec-WebSocket-Key, and must not carry a body.  Returns zero if the request is acceptable,
// otherwise STATUS_UPGRADE_REQUIRED for an unsupported Sec-WebSocket-Version or STATUS_BAD_REQUEST for anything else.
// Reason receives a static description of a failure.

int parse_request(std::string_view Head, Request &Result, std::string_view &Reason);

// Returns the first of the server's Supported subprotocols, in the server's order of preference, that appears in
// Requested.  Names are compared case-sensitively.  Returns an empty view if there is no match.

[[nodiscard]] std::string_view select_protocol(std::span<const std::string> Supported,
   std::span<const std::string_view> Requested);

// Builds a 101 Switching Protocols response.  Protocol is omitted if empty.

[[nodiscard]] std::string switching_response(std::string_view Accept, std::string_view Protocol);

// Builds an empty error response that closes the connection.  A 426 response carries Sec-WebSocket-Version: 13.

[[nodiscard]] std::string rejection_response(int Status);

} // namespace ws
