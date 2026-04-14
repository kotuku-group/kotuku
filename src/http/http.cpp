/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
HTTP: Provides a complete working implementation of HTTP.

The HTTP class provides a way of interacting with servers that support the HTTP protocol.  Supported HTTP methods
include `GET`, `POST`, `PUT`, `DELETE`, `COPY`, `MOVE`, `MKCOL` and more.  The following features are included:

<list type="bullet">
<li>Handling of errors and HTTP status codes.</li>
<li>Monitoring of the server communication process.</li>
<li>Data transfer monitoring.</li>
<li>Sending and receiving in chunks of data.</li>
<li>Background processing of all HTTP instructions.</li>
<li>Data streaming.</li>
<li>User authentication, either automated or with user login dialogs.</li>
</list>

For information on command execution and a technical overview of HTTP processing, please refer to the #Activate()
action.

<header>Sending Content</>

There are a variety of ways to send content to a server when using methods such as `PUT` and `POST`.  Content can be
sent from objects by setting the #InputObject field.  To send content from files, set the #InputFile field.  To send
string content, use an #InputFile location that starts with `string:` followed by the text to send.

<header>Receiving Content</>

There are three possible methods for content download.  This first example downloads content to a temporary file for
further processing:

<pre>
http = obj.new('http', {
   src        = 'http://www.kotuku.dev/index.html',
   method     = 'get',
   outputFile = 'temp:index.html',
   stateChanged = function(HTTP, State)
      if (State == HGS::COMPLETED) then print(content) end
   end
})

http.acActivate()
</pre>

This example uses data feeds to push the downloaded data to another object in text format:

<pre>
doc = obj.new('scintilla')
http = obj.new('http', {
   src        = 'http://www.kotuku.dev/index.html',
   method     = 'get',
   datatype   = 'text',
   objectMode = 'DATA_FEED',
   outputObject = doc
})
http.acActivate()
</pre>

Note that the target object needs to support the datatype that you specify, or it will ignore the incoming data.  The
default datatype is `RAW` (binary format), but the most commonly supported datatype is `TEXT`.

The third method is to use function callbacks.  Refer to the #Incoming field for further information on receiving
data through callbacks.

<header>Progress Monitoring</>

Progress of a data transfer can be monitored through the #Index field.  If the callback features are not being used for
a data transfer, consider using a timer to read from the #Index periodically.

<header>SSL Support (HTTPS)</>

Secure sockets are supported and can be enabled by setting the #Port to 443 prior to connection, or by using `https://`
in URI strings.  Methods of communication remain unchanged when using SSL, as encrypted communication is handled
transparently.

-END-

For information about the HTTP protocol, please refer to the official protocol web page:

   http://www.w3.org/Protocols/rfc2616/rfc2616.html

*********************************************************************************************************************/

//#define DEBUG
#define PRV_HTTP

#include <stdio.h>
#include <unordered_map>
#include <map>
#include <sstream>
#include <charconv>
#include <limits>
#include <algorithm>
#include <format>

#include <kotuku/main.h>
#include <kotuku/modules/http.h>
//#include <kotuku/modules/display.h>
#include <kotuku/modules/network.h>
#include <kotuku/strings.hpp>
#include "../link/base64.h"

#include "md5.c"

#define CRLF "\r\n"
constexpr int MAX_AUTH_RETRIES = 5;
constexpr int HASHLEN = 16;
constexpr int HASHHEXLEN = 32;
using HASH = char[HASHLEN];
using HASHHEX = char[HASHHEXLEN+1];

// Security limits
constexpr int64_t MAX_CONTENT_LENGTH = 10LL * 1024 * 1024 * 1024; // 10GB max
constexpr int64_t MAX_CHUNK_LENGTH = 100 * 1024 * 1024; // 100MB max chunk
constexpr int MAX_HEADER_SIZE = 8 * 1024 * 1024; // 8MB max headers
constexpr int MAX_PORT_NUMBER = 65535;

constexpr int BUFFER_READ_SIZE = 16384;  // Dictates how many bytes are read from the network socket at a time.  Do not make this greater than 64k
constexpr int BUFFER_WRITE_SIZE = 16384; // Dictates how many bytes are written to the network socket at a time.  Do not make this greater than 64k

static void secure_clear_memory(void* Ptr, size_t Len) {
   auto p = (volatile char *)Ptr; // Use volatile to prevent compiler optimization
   for (size_t i = 0; i < Len; i++) p[i] = 0;
   for (size_t i = 0; i < Len; i++) p[i] = 0xff;
   for (size_t i = 0; i < Len; i++) p[i] = 0;
}

//********************************************************************************************************************
// Fast URL validation (RFC 3986)

static uint32_t glUnreservedTable[4];
static uint32_t glReservedTable[4];
static bool glURLTablesInitialised = false;

static void init_url_tables() {
   if (glURLTablesInitialised) return;

   // Initialize unreserved characters table
   // A-Z (0x41-0x5A), a-z (0x61-0x7A), 0-9 (0x30-0x39), -, ., _, ~

   for (char c = 'A'; c <= 'Z'; c++) {
      auto bit = c & 31;
      glUnreservedTable[c >> 5] |= (1U << bit);
   }
   for (char c = 'a'; c <= 'z'; c++) {
      auto bit = c & 31;
      glUnreservedTable[c >> 5] |= (1U << bit);
   }
   for (char c = '0'; c <= '9'; c++) {
      auto bit = c & 31;
      glUnreservedTable[c >> 5] |= (1U << bit);
   }

   // Special unreserved characters

   const char unreserved_special[] = {'-', '.', '_', '~'};
   for (char c : unreserved_special) {
      auto bit = c & 31;
      glUnreservedTable[c >> 5] |= (1U << bit);
   }

   // Initialize reserved characters table

   const char reserved_chars[] = {':', '/', '?', '#', '[', ']', '@', '!', '$', '&', '\'', '(', ')', '*', '+', ',', ';', '='};
   for (char c : reserved_chars) {
      auto bit = c & 31;
      glReservedTable[c >> 5] |= (1U << bit);
   }

   glURLTablesInitialised = true;
}

static bool is_valid_url_char(char Char, bool AllowReserved = false) {
   init_url_tables();

   const auto index = uint8_t(Char);
   if (index >= 128) return false; // Non-ASCII characters

   const auto table_index = index >> 5;  // Divide by 32
   const auto bit_index = index & 31;    // Modulo 32

   if (glUnreservedTable[table_index] & (1U << bit_index)) return true;
   if (AllowReserved and (glReservedTable[table_index] & (1U << bit_index))) return true;
   return false;
}

//********************************************************************************************************************
// Enhanced URL encoding with validation

static std::string encode_url_path(const char* Input)
{
   if (!Input) return std::string();

   std::string result;
   result.reserve(strlen(Input) * 3); // Worst case: every char becomes %XX

   for (const char* p = Input; *p; p++) {
      if (is_valid_url_char(*p, true)) {
         result += *p;
      }
      else if (*p IS ' ') {
         result += "%20";
      }
      else if ((unsigned char)(*p) < 32 or (unsigned char)(*p) > 126) {
         // Encode control characters and non-ASCII
         char encoded[4];
         snprintf(encoded, sizeof(encoded), "%%%02X", (unsigned char)(*p));
         result += encoded;
      }
      else { // Other characters that need encoding
         char encoded[4];
         snprintf(encoded, sizeof(encoded), "%%%02X", (unsigned char)(*p));
         result += encoded;
      }
   }

   return result;
}

//********************************************************************************************************************

static ERR create_http_class(void);

JUMPTABLE_CORE
JUMPTABLE_NETWORK
static OBJECTPTR modNetwork = nullptr;
static OBJECTPTR clHTTP = nullptr;
static objProxy *glProxy = nullptr;
#ifdef DEBUG_SOCKET
static objFile *glDebugFile = nullptr; // For debugging of traffic
#endif

extern "C" uint8_t glAuthScript[];
static int glAuthScriptLength;

class extHTTP : public objHTTP {
   public:
   FUNCTION Incoming;
   FUNCTION Outgoing;
   FUNCTION AuthCallback;
   FUNCTION StateChanged;
   ankerl::unordered_dense::map<std::string, std::string> ResponseKeys;
   ankerl::unordered_dense::map<std::string, std::string> Headers;
   std::string Response;   // Response header buffer
   std::string URI;        // Temporary string, used only when the user reads the URI
   std::string Username;
   std::string Password;
   std::string Realm;
   std::string AuthNonce;
   std::string AuthOpaque;
   std::string AuthPath;    // Equivalent to Path, sans file name
   std::string ContentType;
   std::string AuthQOP;
   std::string AuthAlgorithm;
   std::string AuthCNonce;
   std::string RecvBuffer; // Receive buffer - aids downloading if HTF::RECVBUFFER is defined
   std::vector<uint8_t> WriteBuffer;
   std::vector<uint8_t> Chunk;
   objFile  *flOutput;
   objFile  *flInput;
   objNetSocket *Socket;      // Socket over which the communication is taking place
   int      ChunkBuffered;    // Number of bytes buffered, cannot exceed ChunkSize
   int      ChunkRemaining;   // Unprocessed bytes in the current chunk
   int      ChunkIndex;
   TIMER    TimeoutManager;
   int64_t  LastReceipt;      // Last time (microseconds) at which data was received
   int64_t  TotalSent;        // Total number of bytes sent - exists for assisting debugging only
   OBJECTID DialogWindow;
   int      ResponseIndex;    // Next element to write to in 'Buffer'
   int      SearchIndex;      // Current position of the CRLFCRLF search.
   int16_t  InputPos;         // File name parsing position in InputFile
   uint8_t  RedirectCount;
   uint8_t  AuthRetries;
   uint8_t  ResponseVersion;  // 0x10=HTTP/1.0, 0x11=HTTP/1.1
   uint16_t Connecting:1;
   uint16_t AuthAttempt:1;
   uint16_t PasswordPreset:1;
   uint16_t AuthDigest:1;
   uint16_t SecurePath:1;
   uint16_t Tunneling:1;
   uint16_t Chunked:1;
   uint16_t MultipleInput:1;
   uint16_t KeepAlive:1;
   uint16_t ProxyDefined:1;   // TRUE if the ProxyServer has been manually set by the user
};

static ERR HTTP_Activate(extHTTP *);
static ERR HTTP_Deactivate(extHTTP *);
static ERR HTTP_Free(extHTTP *);
static ERR HTTP_GetKey(extHTTP *, struct acGetKey *);
static ERR HTTP_Init(extHTTP *);
static ERR HTTP_NewPlacement(extHTTP *);
static ERR HTTP_SetKey(extHTTP *, struct acSetKey *);
static ERR HTTP_Write(extHTTP *, struct acWrite *);

#include "http_def.c"

//********************************************************************************************************************

static const FieldDef clStatus[] = {
   { "Continue",                 HTS::CONTINUE },
   { "Switching Protocols",      HTS::SWITCH_PROTOCOLS },
   { "Okay",                     HTS::OKAY },
   { "Created",                  HTS::CREATED },
   { "Accepted",                 HTS::ACCEPTED },
   { "Unverified Content",       HTS::UNVERIFIED_CONTENT },
   { "No Content",               HTS::NO_CONTENT },
   { "Reset Content",            HTS::RESET_CONTENT },
   { "Partial Content",          HTS::PARTIAL_CONTENT },
   { "Multiple Choices",         HTS::MULTIPLE_CHOICES },
   { "Moved Permanently",        HTS::MOVED_PERMANENTLY },
   { "Found",                    HTS::FOUND },
   { "See Other",                HTS::SEE_OTHER },
   { "Not Modified",             HTS::NOT_MODIFIED },
   { "Use Proxy",                HTS::USE_PROXY },
   { "Temporary Redirect",       HTS::TEMP_REDIRECT },
   { "Bad Request",              HTS::BAD_REQUEST },
   { "Unauthorised",             HTS::UNAUTHORISED },
   { "Payment Required",         HTS::PAYMENT_REQUIRED },
   { "Forbidden",                HTS::FORBIDDEN },
   { "Not Found",                HTS::NOT_FOUND },
   { "Method Not Allowed",       HTS::METHOD_NOT_ALLOWED },
   { "Not Acceptable",           HTS::NOT_ACCEPTABLE },
   { "Proxy Authentication Required", HTS::PROXY_AUTHENTICATION },
   { "Request Timeout",          HTS::REQUEST_TIMEOUT },
   { "Conflict",                 HTS::CONFLICT },
   { "Gone",                     HTS::GONE },
   { "Length Required",          HTS::LENGTH_REQUIRED },
   { "Precondition Failed",      HTS::PRECONDITION_FAILED },
   { "Request Entity Too Large", HTS::ENTITY_TOO_LARGE },
   { "Request-URI Too Long",     HTS::URI_TOO_LONG },
   { "Unsupported Media Type",   HTS::UNSUPPORTED_MEDIA },
   { "Out of Range",             HTS::OUT_OF_RANGE },
   { "Expectation Failed",       HTS::EXPECTATION_FAILED },
   { "Internal Server Error",    HTS::SERVER_ERROR },
   { "Not Implemented",          HTS::NOT_IMPLEMENTED },
   { "Bad Gateway",              HTS::BAD_GATEWAY },
   { "Service Unavailable",      HTS::SERVICE_UNAVAILABLE },
   { "Gateway Timeout",          HTS::GATEWAY_TIMEOUT },
   { "HTTP Version Unsupported", HTS::VERSION_UNSUPPORTED },
   { nullptr, 0 }
};

//********************************************************************************************************************

static ERR  check_incoming_end(extHTTP *);
static ERR  parse_file(extHTTP *, std::string &);
static void parse_file(extHTTP *, std::ostringstream &);
static ERR  parse_response(extHTTP *, std::string_view);
static ERR  output_incoming_data(extHTTP *, APTR, int);
static void writehex(HASH, HASHHEX);
static void digest_calc_ha1(extHTTP *, HASHHEX);
static void digest_calc_response(extHTTP *, std::string, CSTRING, HASHHEX, HASHHEX, HASHHEX);
static void set_http_method(extHTTP *, CSTRING, std::ostringstream &);
static ERR  SET_Path(extHTTP *, CSTRING);
static ERR  SET_Location(extHTTP *, CSTRING);
static ERR  http_timeout(extHTTP *, int64_t, int64_t);
static void socket_feedback(objNetSocket *, NTC, APTR);
static ERR  socket_incoming(objNetSocket *);
static ERR  socket_outgoing(objNetSocket *);

//********************************************************************************************************************

[[maybe_unused]] inline CSTRING GETSTATUS(int Code)
{
   for (int16_t i=0; clStatus[i].Name; i++) {
      if (clStatus[i].Value IS Code) return clStatus[i].Name;
   }
   return "Unrecognised Status Code";
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("network", &modNetwork, &NetworkBase) != ERR::Okay) return ERR::InitModule;

   glProxy = objProxy::create::global();

   return create_http_class();
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   if (clHTTP)     { FreeResource(clHTTP);     clHTTP     = nullptr; }
   if (glProxy)    { FreeResource(glProxy);    glProxy    = nullptr; }
   if (modNetwork) { FreeResource(modNetwork); modNetwork = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static void notify_free_outgoing(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extHTTP *)CurrentContext())->Outgoing.clear();
}

static void notify_free_state_changed(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extHTTP *)CurrentContext())->StateChanged.clear();
}

static void notify_free_incoming(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extHTTP *)CurrentContext())->Incoming.clear();
}

static void notify_free_auth_callback(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extHTTP *)CurrentContext())->AuthCallback.clear();
}

/*********************************************************************************************************************

-ACTION-
Activate: Sends an HTTP request to the host.

This action sends an HTTP request to the targeted #Host.  Based on the desired #Method, an HTTP request
will be sent to the host and the action will return whilst the HTTP object waits for a
response from the server.  If the host fails to respond within the time period indicated by the #ConnectTimeout,
the HTTP object will be deactivated and the #CurrentState set to `TERMINATED`.

Successful parsing of the HTTP request at the host will result in a response being received, followed by content
data (if applicable). The HTTP response code will be stored in the #Status field.  The response will be parsed
automatically with the header strings stored as key-values.  To receive notice of the parsed response, hook into the
#StateChanged callback.

Incoming HTTP content can be managed in the following ways: It may be streamed to an object
referenced by the #OutputObject field through data feeds.  It can be written to the target object if the #ObjectMode
is set to `READ_WRITE`.  Or it can be received through the #Incoming callback.

On completion of an HTTP request, the #Deactivate() action is called, regardless of the level of success.

-ERRORS-
Okay:   The HTTP get operation was successfully started.
Failed: The HTTP get operation failed immediately for an unspecified reason.
File:   Failed to create a target file if the OutputFile field was set.
Write:  Failed to write data to the HTTP @NetSocket.
CreateObject: Failed to create a @NetSocket object.
HostNotFound: DNS resolution of the domain name in the URI failed.
Recursion: Function was called recursively.
NotInitialised: The HTTP object has not been initialised.
FieldNotSet: Required fields are not set (e.g. destination for COPY/MOVE methods, or data source for POST/PUT methods).
NoData: Input file is empty or has no data for POST/PUT operations.
ConnectionRefused: The connection to the server was refused.
-END-

*********************************************************************************************************************/

static ERR HTTP_Activate(extHTTP *Self)
{
   pf::Log log;
   int i;
   static thread_local uint8_t recursion = 0;

   if (recursion) return log.warning(ERR::Recursion);

   recursion++;
   auto __cleanup = pf::Defer([&]() { recursion--; });

   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   log.branch("Host: %s, Port: %d, Path: %s, Proxy: %s, SSL: %d", Self->Host, Self->Port, Self->Path, Self->ProxyServer, ((Self->Flags & HTF::SSL) != HTF::NIL) ? 1 : 0);

   if (Self->TimeoutManager) { UpdateTimer(Self->TimeoutManager, 0); Self->TimeoutManager = 0; }

   Self->Error         = ERR::Okay;
   Self->ResponseIndex = 0;
   Self->SearchIndex   = 0;
   Self->Index         = 0;
   Self->CurrentState  = HGS::NIL;
   Self->Status        = HTS::NIL;
   Self->TotalSent     = 0;
   Self->Tunneling     = false;
   Self->Flags        &= ~(HTF::MOVED|HTF::REDIRECTED);

   if ((Self->Socket) and (Self->Socket->State IS NTC::DISCONNECTED)) {
      Self->Socket->set(FID_Feedback, (APTR)nullptr);
      FreeResource(Self->Socket);
      Self->Socket = nullptr;
      Self->SecurePath = true;
   }

   Self->Response.clear();
   if (Self->flInput)  { FreeResource(Self->flInput); Self->flInput = nullptr; }
   if (Self->flOutput) { FreeResource(Self->flOutput); Self->flOutput = nullptr; }

   Self->RecvBuffer.resize(0);

   std::ostringstream cmd;

   if ((Self->ProxyServer) and ((Self->Flags & HTF::SSL) != HTF::NIL) and (!Self->Socket)) {
      // SSL tunnelling is required.  Send a CONNECT request to the proxy and
      // then we will follow this up with the actual HTTP requests.

      log.trace("SSL tunneling is required.");

      cmd << "CONNECT " << Self->Host << ":" << Self->Port << " HTTP/1.1" << CRLF;
      cmd << "Host: " << Self->Host << CRLF;
      cmd << "User-Agent: " << Self->UserAgent << CRLF;
      cmd << "Proxy-Connection: keep-alive" << CRLF;
      cmd << "Connection: keep-alive" << CRLF;

      Self->Tunneling = true;

      //set auth "Proxy-Authorization: Basic [base64::encode $opts(proxyUser):$opts(proxyPass)]"
   }
   else {
      if (Self->Method IS HTM::COPY) {
         // Copies a source (indicated by Path) to a Destination.  The Destination is referenced as an variable field.

         if (Self->Headers.contains("Destination")) {
            set_http_method(Self, "COPY", cmd);
            cmd << "Destination: http://" << Self->Host << "/" << Self->Headers["Destination"] << CRLF;

            auto & overwrite = Self->Headers["Overwrite"];
            if (!overwrite.empty()) {
               // If the overwrite is 'F' then copy will fail if the destination exists
               cmd << "Overwrite: " << overwrite << CRLF;
            }
         }
         else {
            log.warning("HTTP COPY request requires a destination path.");
            Self->Error = ERR::FieldNotSet;
            return Self->Error;
         }
      }
      else if (Self->Method IS HTM::DELETE) {
         set_http_method(Self, "DELETE", cmd);
      }
      else if (Self->Method IS HTM::GET) {
         set_http_method(Self, "GET", cmd);
         if (Self->Index) cmd << "Range: bytes=" << Self->Index << "-" << CRLF;
      }
      else if (Self->Method IS HTM::LOCK) {

      }
      else if (Self->Method IS HTM::MKCOL) {
         set_http_method(Self, "MKCOL", cmd);
      }
      else if (Self->Method IS HTM::MOVE) {
         // Moves a source (indicated by Path) to a Destination.  The Destination is referenced as a variable field.

         if (Self->Headers.contains("Destination")) {
            set_http_method(Self, "MOVE", cmd);
            cmd << "Destination: http://" << Self->Host << "/" << Self->Headers["Destination"] << CRLF;
         }
         else {
            log.warning("HTTP MOVE request requires a destination path.");
            Self->Error = ERR::FieldNotSet;
            return Self->Error;
         }
      }
      else if (Self->Method IS HTM::OPTIONS) {
         if ((!Self->Path) or ((Self->Path[0] IS '*') and (!Self->Path[1]))) {
            cmd << "OPTIONS * HTTP/1.1" << CRLF;
            cmd << "Host: " << Self->Host << CRLF;
            cmd << "User-Agent: " << Self->UserAgent << CRLF;
         }
         else set_http_method(Self, "OPTIONS", cmd);
      }
      else if ((Self->Method IS HTM::POST) or (Self->Method IS HTM::PUT)) {
         log.trace("POST/PUT request being processed.");

         Self->Chunked = false;

         if (((Self->Flags & HTF::NO_HEAD) IS HTF::NIL) and ((Self->SecurePath) or (Self->CurrentState IS HGS::AUTHENTICATING))) {
            log.trace("Executing HEAD statement for authentication.");
            set_http_method(Self, "HEAD", cmd);
            Self->setCurrentState(HGS::AUTHENTICATING);
         }
         else {
            // You can post data from a file source or an object.  In the case of an object it is possible to preset
            // the content-length, although we will attempt to read the amount to transfer from the object's Size
            // field, if supported.  An Outgoing routine can be specified for customised output.
            //
            // To post data from a string, use an InputFile setting as follows:  string:data=to&send

            if (Self->Outgoing.defined()) {
               // User has specified an Outgoing function.  No preparation is necessary.  It is recommended that
               // ContentLength is set beforehand if the amount of data to be sent is known, otherwise
               // the developer should set ContentLength to -1.

            }
            else if (Self->InputFile) {
               if (Self->MultipleInput) {
                  log.trace("Multiple input files detected.");
                  Self->InputPos = 0;
                  parse_file(Self, cmd);
                  Self->flInput = objFile::create::local(fl::Path(cmd.str().c_str()), fl::Flags(FL::READ));
               }
               else Self->flInput = objFile::create::local(fl::Path(Self->InputFile), fl::Flags(FL::READ));

               if (Self->flInput) {
                  Self->Index = 0;
                  if (!Self->Size) {
                     Self->flInput->get(FID_Size, Self->ContentLength); // Use the file's size as ContentLength
                     if (!Self->ContentLength) { // If the file is empty or size is indeterminate then assume nothing is being posted
                        Self->Error = ERR::NoData;
                        return Self->Error;
                     }
                  }
                  else Self->ContentLength = Self->Size; // Allow the developer to define the ContentLength
               }
               else {
                  Self->Error = ERR::File;
                  return log.warning(Self->Error);
               }
            }
            else if (Self->InputObjectID) {
               if (!Self->Size) {
                  pf::ScopedObjectLock<Object> input(Self->InputObjectID, 3000);
                  if (input.granted()) {
                     input->get(FID_Size, Self->ContentLength);
                  }
               }
               else Self->ContentLength = Self->Size;
            }
            else {
               log.warning("No data source specified for POST/PUT method.");
               Self->Error = ERR::FieldNotSet;
               return Self->Error;
            }

            set_http_method(Self, (Self->Method IS HTM::POST) ? "POST" : "PUT", cmd);

            if (Self->ContentLength >= 0) {
               cmd << "Content-length: " << Self->ContentLength << CRLF;
            }
            else {
               log.msg("Content-length not defined for POST/PUT (transfer will be streamed).");

               // Using chunked encoding for post/put will help the server manage streaming
               // uploads, and may even be of help when the content length is known.

               if ((Self->Flags & HTF::RAW) IS HTF::NIL) {
                  cmd << "Transfer-Encoding: chunked" << CRLF;
                  Self->Chunked = true;
               }
            }

            if (!Self->ContentType.empty()) {
               log.trace("User content type: %s", Self->ContentType.c_str());
               cmd << "Content-type: " << Self->ContentType << CRLF;
            }
            else if (Self->Method IS HTM::POST) {
               cmd << "Content-type: application/x-www-form-urlencoded" << CRLF;
            }
            else cmd << "Content-type: application/binary" << CRLF;
         }
      }
      else if (Self->Method IS HTM::UNLOCK) {

      }
      else {
         log.warning("HTTP method no. %d not understood.", int(Self->Method));
         Self->Error = ERR::Failed;
         return Self->Error;
      }

      // Authentication support.  At least one attempt to get the resource (Retries > 0) is required before we can pass the
      // username and password, as it is necessary to be told the method of authentication required (in the case of digest
      // authentication, the nonce value is also required from the server).

      if ((Self->AuthRetries > 0) and (!Self->Username.empty()) and (!Self->Password.empty())) {
         if (Self->AuthDigest) {
            char nonce_count[9] = "00000001";
            HASHHEX HA1, HA2 = "", response;

            Self->AuthCNonce.resize(8);
            for (i=0; i < 8; i++) Self->AuthCNonce[i] = '0' + (rand() % 10);

            digest_calc_ha1(Self, HA1);
            digest_calc_response(Self, cmd.str(), nonce_count, HA1, HA2, response);

            cmd << "Authorization: Digest ";
            cmd << "username=\"" << Self->Username << "\", realm=\"" << Self->Realm << "\", ";
            cmd << "nonce=\"" << Self->AuthNonce << "\", uri=\"/" << Self->Path << "\", ";
            cmd << "qop=" << Self->AuthQOP << ", nc=" << nonce_count << ", ";
            cmd << "cnonce=\"" << Self->AuthCNonce << "\", response=\"" << response << "\"";

            if (!Self->AuthOpaque.empty()) cmd << ", opaque=\"" << Self->AuthOpaque << "\"";

            cmd << CRLF;
         }
         else {
            std::string buffer = Self->Username + ':' + Self->Password;
            std::vector<char> output(buffer.size() * 2);

            pf::BASE64ENCODE state;

            cmd << "Authorization: Basic ";
            auto len = pf::Base64Encode(&state, buffer.c_str(), buffer.size(), output.data(), buffer.length() * 2);
            cmd.write(output.data(), len);
            cmd << CRLF;
         }

         // Clear the password.  This has the effect of resetting the authentication attempt in case the credentials are wrong.

   /*
         for (i=0; Self->Password[i]; i++) Self->Password[i] = 0;
         Self->Password.clear();
   */
      }

      // Add any custom headers

      if (Self->CurrentState != HGS::AUTHENTICATING) {
         for (const auto& [k, v] : Self->Headers) {
            log.trace("Custom header: %s: %s", k.c_str(), v.c_str());
            cmd << k << ": " << v << CRLF;
         }
      }
   }

   cmd << CRLF; // Terminating line feed
   auto cstr = cmd.str();
   log.detail("HTTP REQUEST HEADER\n%s", cstr.c_str());

   if ((Self->Socket) and (not Self->KeepAlive)) {
      // Termination of the existing connection is pending and we need to honour it.
      FreeResource(Self->Socket);
      Self->Socket = nullptr;
   }

   if (!Self->Socket) {
      // If we're using straight SSL without tunnelling, set the SSL flag now so that SSL is automatically engaged on connection.

      auto flags = NSF::NIL;
      if (((Self->Flags & HTF::SSL) != HTF::NIL) and (!Self->Tunneling)) {
         flags |= NSF::SSL;
         if ((Self->Flags & HTF::DISABLE_SERVER_VERIFY) != HTF::NIL) {
            flags |= NSF::DISABLE_SERVER_VERIFY;
         }
      }

      if (!(Self->Socket = objNetSocket::create::local(
            fl::ClientData(Self),
            fl::Incoming(C_FUNCTION(socket_incoming)),
            fl::Feedback(C_FUNCTION(socket_feedback)),
            fl::Flags(flags)))) {
         Self->Error = ERR::CreateObject;
         return log.warning(Self->Error);
      }
   }
   else {
      log.trace("Re-using existing socket/server connection.");

      Self->Socket->setIncoming(C_FUNCTION(socket_incoming));
      Self->Socket->setFeedback(C_FUNCTION(socket_feedback));
   }

   if (!Self->Tunneling) {
      if (Self->CurrentState != HGS::AUTHENTICATING) {
         if ((Self->Method IS HTM::PUT) or (Self->Method IS HTM::POST)) {
            Self->Socket->setOutgoing(C_FUNCTION(socket_outgoing));
         }
         else Self->Socket->set(FID_Outgoing, (APTR)nullptr);
      }
      else Self->Socket->set(FID_Outgoing, (APTR)nullptr);
   }

   // Buffer the HTTP command string to the socket (will write on connect if we're not connected already).

   if (acWrite(Self->Socket, cstr.c_str(), cstr.length()) IS ERR::Okay) {
      if (Self->Socket->State IS NTC::DISCONNECTED) {
         if (auto result = Self->Socket->connect(Self->ProxyServer ? Self->ProxyServer : Self->Host, Self->ProxyServer ? Self->ProxyPort : Self->Port, 5.0); result IS ERR::Okay) {
            Self->Connecting = true;

            if (Self->TimeoutManager) UpdateTimer(Self->TimeoutManager, Self->ConnectTimeout);
            else SubscribeTimer(Self->ConnectTimeout, C_FUNCTION(http_timeout), &Self->TimeoutManager);

            return ERR::Okay;
         }
         else if (result IS ERR::HostNotFound) {
            Self->Error = ERR::HostNotFound;
            return log.warning(Self->Error);
         }
         else {
            Self->Error = ERR::ConnectionRefused;
            return log.warning(Self->Error);
         }
      }
      else return ERR::Okay;
   }
   else {
      Self->Error = ERR::Write;
      return log.warning(Self->Error);
   }
}

/*********************************************************************************************************************
-ACTION-
Deactivate: Ends the current HTTP request.

Following the completion of an HTTP request, the Deactivate() action will be called internally to signal an end to the
process.  Subscribing to Deactivate() is an effective means of responding to the end of a single HTTP request.

Active HTTP requests can be manually cancelled by calling Deactivate() at any time.  Deactivation does not necessarily
result in closure of the socket.
-END-
*********************************************************************************************************************/

static ERR HTTP_Deactivate(extHTTP *Self)
{
   pf::Log log;

   log.branch("Halting HTTP request.");

   if (Self->CurrentState < HGS::COMPLETED) Self->setCurrentState(HGS::TERMINATED);

   // Closing files is important for dropping the file locks

   if (Self->flInput) { FreeResource(Self->flInput); Self->flInput = nullptr; }
   if (Self->flOutput) { FreeResource(Self->flOutput); Self->flOutput = nullptr; }

   Self->WriteBuffer.resize(0);
   if (Self->TimeoutManager) { UpdateTimer(Self->TimeoutManager, 0); Self->TimeoutManager = 0; }

   if (Self->Socket) {
      // The socket object is removed if it has been closed at the server, or if our HTTP object is closing prematurely
      // (for example due to a timeout, or an early call to Deactivate).  This prevents any more incoming data from the
      // server being processed when we don't want it.

      if ((Self->Socket->State IS NTC::DISCONNECTED) or (Self->CurrentState IS HGS::TERMINATED)) {
         log.msg("Terminating socket (disconnected).");
         Self->Socket->set(FID_Feedback, (APTR)nullptr);
         FreeResource(Self->Socket);
         Self->Socket = nullptr;
         Self->SecurePath = true;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR HTTP_Free(extHTTP *Self)
{
   if (Self->Socket) {
      Self->Socket->set(FID_Feedback, (APTR)nullptr);
      FreeResource(Self->Socket);
      Self->Socket = nullptr;
   }

   if (Self->AuthCallback.isScript()) UnsubscribeAction(Self->AuthCallback.Context, AC::Free);
   if (Self->Incoming.isScript())     UnsubscribeAction(Self->Incoming.Context, AC::Free);
   if (Self->StateChanged.isScript()) UnsubscribeAction(Self->StateChanged.Context, AC::Free);
   if (Self->Outgoing.isScript())     UnsubscribeAction(Self->Outgoing.Context, AC::Free);

   if (Self->TimeoutManager) { UpdateTimer(Self->TimeoutManager, 0); Self->TimeoutManager = 0; }

   if (Self->flInput)     { FreeResource(Self->flInput);     Self->flInput = nullptr; }
   if (Self->flOutput)    { FreeResource(Self->flOutput);    Self->flOutput = nullptr; }
   if (Self->Path)        { FreeResource(Self->Path);        Self->Path = nullptr; }
   if (Self->InputFile)   { FreeResource(Self->InputFile);   Self->InputFile = nullptr; }
   if (Self->OutputFile)  { FreeResource(Self->OutputFile);  Self->OutputFile = nullptr; }
   if (Self->Host)        { FreeResource(Self->Host);        Self->Host = nullptr; }
   if (Self->UserAgent)   { FreeResource(Self->UserAgent);   Self->UserAgent = nullptr; }
   if (Self->ProxyServer) { FreeResource(Self->ProxyServer); Self->ProxyServer = nullptr; }

   if (!Self->Password.empty()) {
      secure_clear_memory(const_cast<char*>(Self->Password.data()), Self->Password.size());
   }

   Self->~extHTTP();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
GetKey: Entries in the HTTP response header can be read as key-values.
-END-
*********************************************************************************************************************/

static ERR HTTP_GetKey(extHTTP *Self, struct acGetKey *Args)
{
   if (!Args) return ERR::NullArgs;

   if (Self->ResponseKeys.contains(Args->Key)) {
      pf::strcopy(Self->ResponseKeys[Args->Key], Args->Value, Args->Size);
      return ERR::Okay;
   }

   if (Self->Headers.contains(Args->Key)) {
      pf::strcopy(Self->Headers[Args->Key], Args->Value, Args->Size);
      return ERR::Okay;
   }

   return ERR::UnsupportedField;
}

//********************************************************************************************************************

static ERR HTTP_Init(extHTTP *Self)
{
   pf::Log log;

   if (!Self->ProxyDefined) {
      if ((glProxy) and (glProxy->find(Self->Port, true) IS ERR::Okay)) {
         if (Self->ProxyServer) FreeResource(Self->ProxyServer);
         Self->ProxyServer = pf::strclone(glProxy->Server);
         Self->ProxyPort   = glProxy->ServerPort; // NB: Default is usually 8080

         log.msg("Using preset proxy server '%s:%d'", Self->ProxyServer, Self->ProxyPort);
      }
   }
   else log.msg("Proxy pre-defined by user.");

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR HTTP_NewPlacement(extHTTP *Self)
{
   new (Self) extHTTP;
   Self->Error          = ERR::Okay;
   Self->UserAgent      = pf::strclone("Kotuku Client");
   Self->DataTimeout    = 5.0;
   Self->ConnectTimeout = 10.0;
   Self->Datatype       = DATA::RAW;
   Self->BufferSize     = 16 * 1024;
   Self->AuthQOP        = "auth";
   Self->AuthAlgorithm  = "md5";
   Self->KeepAlive      = true;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SetKey: Options for the HTTP header can be set as key-values.
-END-
*********************************************************************************************************************/

static ERR HTTP_SetKey(extHTTP *Self, struct acSetKey *Args)
{
   if (!Args) return ERR::NullArgs;

   Self->Headers[Args->Key] = Args->Value;
   return ERR::Okay;
}

//********************************************************************************************************************
// Writing to an HTTP object's outgoing buffer is possible if the Outgoing callback function is active.

static ERR HTTP_Write(extHTTP *Self, struct acWrite *Args)
{
   if ((!Args) or (!Args->Buffer)) return ERR::NullArgs;

   if (auto len = Args->Length; len > 0) {
      auto offset = Self->WriteBuffer.size();
      Self->WriteBuffer.resize(Self->WriteBuffer.size() + len);
      pf::copymem(Args->Buffer, Self->WriteBuffer.data() + offset, len);
      Args->Result = len;
      return ERR::Okay;
   }
   else {
      Args->Result = 0;
      return ERR::Args;
   }
}

//********************************************************************************************************************

#include "http_fields.cpp"
#include "http_functions.cpp"
#include "http_incoming.cpp"

static const FieldArray clFields[] = {
   { "DataTimeout",    FDF_DOUBLE|FDF_RW },
   { "ConnectTimeout", FDF_DOUBLE|FDF_RW },
   { "Index",          FDF_INT64|FDF_RW }, // Writeable only because we update it using SetField()
   { "ContentLength",  FDF_INT64|FDF_RW },
   { "Size",           FDF_INT64|FDF_RW },
   { "Host",           FDF_STRING|FDF_RI, nullptr, SET_Host },
   { "Path",           FDF_STRING|FDF_RW, nullptr, SET_Path },
   { "OutputFile",     FDF_STRING|FDF_RW, nullptr, SET_OutputFile },
   { "InputFile",      FDF_STRING|FDF_RW, nullptr, SET_InputFile },
   { "UserAgent",      FDF_STRING|FDF_RW, nullptr, SET_UserAgent },
   { "InputObject",    FDF_OBJECTID|FDF_RW },
   { "OutputObject",   FDF_OBJECTID|FDF_RW },
   { "Method",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_Method, &clHTTPMethod },
   { "Port",           FDF_INT|FDF_RW },
   { "ObjectMode",     FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clHTTPObjectMode },
   { "Flags",          FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clHTTPFlags },
   { "Status",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clStatus },
   { "Error",          FDF_INT|FDF_RW },
   { "Datatype",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clHTTPDatatype },
   { "CurrentState",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_CurrentState, &clHTTPCurrentState },
   { "ProxyServer",    FDF_STRING|FDF_RW, nullptr, SET_ProxyServer },
   { "ProxyPort",      FDF_INT|FDF_RW },
   { "BufferSize",     FDF_INT|FDF_RW, nullptr, SET_BufferSize },
   // Virtual fields
   { "AuthCallback",   FDF_FUNCTIONPTR|FDF_RW,   GET_AuthCallback, SET_AuthCallback },
   { "ContentType",    FDF_STRING|FDF_RW,        GET_ContentType, SET_ContentType },
   { "Incoming",       FDF_FUNCTIONPTR|FDF_RW,   GET_Incoming, SET_Incoming },
   { "Location",       FDF_STRING|FDF_RW,        GET_Location, SET_Location },
   { "Outgoing",       FDF_FUNCTIONPTR|FDF_RW,   GET_Outgoing, SET_Outgoing },
   { "Realm",          FDF_STRING|FDF_RW,        GET_Realm, SET_Realm },
   { "RecvBuffer",     FDF_ARRAY|FDF_BYTE|FDF_R, GET_RecvBuffer },
   { "Src",            FDF_STRING|FDF_SYNONYM|FD_PRIVATE|FDF_RW, GET_Location, SET_Location }, // Deprecated by URL
   { "URL",            FDF_STRING|FDF_SYNONYM|FDF_RW, GET_Location, SET_Location },
   { "StateChanged",   FDF_FUNCTIONPTR|FDF_RW,   GET_StateChanged, SET_StateChanged },
   { "Username",       FDF_STRING|FDF_W,         nullptr, SET_Username },
   { "Password",       FDF_STRING|FDF_W,         nullptr, SET_Password },
   END_FIELD
};

//********************************************************************************************************************

static ERR create_http_class(void)
{
   clHTTP = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::HTTP),
      fl::ClassVersion(VER_HTTP),
      fl::Name("HTTP"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clHTTPActions),
      fl::Fields(clFields),
      fl::Size(sizeof(extHTTP)),
      fl::Path(MOD_PATH));

   return clHTTP ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_http_module() { return &ModHeader; }
