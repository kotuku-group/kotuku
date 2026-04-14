#pragma once

// Name:      http.h
// Copyright: Paul Manias © 2005-2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_HTTP (1)

#include <kotuku/modules/network.h>

class objHTTP;

// Output mode.

enum class HOM : int {
   NIL = 0,
   DATA_FEED = 0,
   READ_WRITE = 1,
   READ = 1,
   WRITE = 1,
};

// Options for defining an HTTP object's state.

enum class HGS : int {
   NIL = 0,
   READING_HEADER = 0,
   AUTHENTICATING = 1,
   AUTHENTICATED = 2,
   SENDING_CONTENT = 3,
   SEND_COMPLETE = 4,
   READING_CONTENT = 5,
   COMPLETED = 6,
   TERMINATED = 7,
   END = 8,
};

// The HTTP Method to use when the object is activated.

enum class HTM : int {
   NIL = 0,
   GET = 0,
   POST = 1,
   PUT = 2,
   HEAD = 3,
   DELETE = 4,
   OPTIONS = 5,
   TRACE = 6,
   MKCOL = 7,
   BCOPY = 8,
   BDELETE = 9,
   BMOVE = 10,
   BPROPFIND = 11,
   BPROPPATCH = 12,
   COPY = 13,
   LOCK = 14,
   MOVE = 15,
   NOTIFY = 16,
   POLL = 17,
   PROPFIND = 18,
   PROPPATCH = 19,
   SEARCH = 20,
   SUBSCRIBE = 21,
   UNLOCK = 22,
   UNSUBSCRIBE = 23,
};

// HTTP status codes

enum class HTS : int {
   NIL = 0,
   CONTINUE = 100,
   SWITCH_PROTOCOLS = 101,
   OKAY = 200,
   CREATED = 201,
   ACCEPTED = 202,
   UNVERIFIED_CONTENT = 203,
   NO_CONTENT = 204,
   RESET_CONTENT = 205,
   PARTIAL_CONTENT = 206,
   MULTIPLE_CHOICES = 300,
   MOVED_PERMANENTLY = 301,
   FOUND = 302,
   SEE_OTHER = 303,
   NOT_MODIFIED = 304,
   USE_PROXY = 305,
   TEMP_REDIRECT = 307,
   BAD_REQUEST = 400,
   UNAUTHORISED = 401,
   PAYMENT_REQUIRED = 402,
   FORBIDDEN = 403,
   NOT_FOUND = 404,
   METHOD_NOT_ALLOWED = 405,
   NOT_ACCEPTABLE = 406,
   PROXY_AUTHENTICATION = 407,
   REQUEST_TIMEOUT = 408,
   CONFLICT = 409,
   GONE = 410,
   LENGTH_REQUIRED = 411,
   PRECONDITION_FAILED = 412,
   ENTITY_TOO_LARGE = 413,
   URI_TOO_LONG = 414,
   UNSUPPORTED_MEDIA = 415,
   OUT_OF_RANGE = 416,
   EXPECTATION_FAILED = 417,
   SERVER_ERROR = 500,
   NOT_IMPLEMENTED = 501,
   BAD_GATEWAY = 502,
   SERVICE_UNAVAILABLE = 503,
   GATEWAY_TIMEOUT = 504,
   VERSION_UNSUPPORTED = 505,
};

// HTTP flags

enum class HTF : uint32_t {
   NIL = 0,
   RESUME = 0x00000001,
   MESSAGE = 0x00000002,
   MOVED = 0x00000004,
   REDIRECTED = 0x00000008,
   NO_HEAD = 0x00000010,
   NO_DIALOG = 0x00000020,
   RAW = 0x00000040,
   DEBUG_SOCKET = 0x00000080,
   RECV_BUFFER = 0x00000100,
   SSL = 0x00000200,
   DISABLE_SERVER_VERIFY = 0x00000400,
};

DEFINE_ENUM_FLAG_OPERATORS(HTF)

// HTTP class definition

#define VER_HTTP (1.000000)

class objHTTP : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::HTTP;
   static constexpr CSTRING CLASS_NAME = "HTTP";

   using create = pf::Create<objHTTP>;

   double   DataTimeout;     // The data timeout value, relevant when receiving or sending data.
   double   ConnectTimeout;  // The initial connection timeout value, measured in seconds.
   int64_t  Index;           // Indicates the total bytes received during content transfer.
   int64_t  ContentLength;   // The byte length of incoming or outgoing content.
   int64_t  Size;            // Set this field to define the length of a data transfer when issuing a POST command.
   STRING   Host;            // The targeted HTTP server is specified here, either by name or IP address.
   STRING   Path;            // The HTTP path targeted at the host server.
   STRING   OutputFile;      // To download HTTP content to a file, set a file path here.
   STRING   InputFile;       // To upload HTTP content from a file, set a file path here.
   STRING   UserAgent;       // Specifies the name of the user-agent string that is sent in HTTP requests.
   OBJECTID InputObjectID;   // Allows data to be sent from an object on execution of a POST command.
   OBJECTID OutputObjectID;  // Incoming data can be sent to the object referenced in this field.
   HTM      Method;          // The HTTP instruction to execute (defaults to GET).
   int      Port;            // The HTTP port to use when targeting a host.
   HOM      ObjectMode;      // The transfer mode used when passing data to a targeted object.
   HTF      Flags;           // Optional flags.
   HTS      Status;          // Indicates the HTTP status code returned on completion of an HTTP request.
   ERR      Error;           // The error code received for the most recently executed HTTP command.
   DATA     Datatype;        // The default datatype format to use when passing data to a target object.
   HGS      CurrentState;    // Indicates the current state of an HTTP object during its interaction with an HTTP server.
   STRING   ProxyServer;     // Route the HTTP request through the proxy server defined here.
   int      ProxyPort;       // The port to use when communicating with a proxy server.
   int      BufferSize;      // Indicates the preferred buffer size for data operations.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR getKey(CSTRING Key, STRING Value, int Size) noexcept {
      struct acGetKey args = { Key, Value, Size };
      auto error = Action(AC::GetKey, this, &args);
      if ((error != ERR::Okay) and (Value)) Value[0] = 0;
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR acSetKey(CSTRING FieldName, CSTRING Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR write(CPTR Buffer, int Size, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { (int8_t *)Buffer.c_str(), int(Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline int writeResult(CPTR Buffer, int Size) noexcept {
      struct acWrite write = { (int8_t *)Buffer, Size };
      if (Action(AC::Write, this, &write) IS ERR::Okay) return write.Result;
      else return 0;
   }

   // Customised field setting

   inline ERR setDataTimeout(const double Value) noexcept {
      this->DataTimeout = Value;
      return ERR::Okay;
   }

   inline ERR setConnectTimeout(const double Value) noexcept {
      this->ConnectTimeout = Value;
      return ERR::Okay;
   }

   inline ERR setIndex(const int64_t Value) noexcept {
      this->Index = Value;
      return ERR::Okay;
   }

   inline ERR setContentLength(const int64_t Value) noexcept {
      this->ContentLength = Value;
      return ERR::Okay;
   }

   inline ERR setSize(const int64_t Value) noexcept {
      this->Size = Value;
      return ERR::Okay;
   }

   template <class T> inline ERR setHost(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[23];
      return field->WriteValue(target, field, 0x08800500, to_cstring(Value), 1);
   }

   template <class T> inline ERR setPath(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setOutputFile(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setInputFile(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   template <class T> inline ERR setUserAgent(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[33];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setInputObject(OBJECTID Value) noexcept {
      this->InputObjectID = Value;
      return ERR::Okay;
   }

   inline ERR setOutputObject(OBJECTID Value) noexcept {
      this->OutputObjectID = Value;
      return ERR::Okay;
   }

   inline ERR setMethod(const HTM Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setPort(const int Value) noexcept {
      this->Port = Value;
      return ERR::Okay;
   }

   inline ERR setObjectMode(const HOM Value) noexcept {
      this->ObjectMode = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const HTF Value) noexcept {
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setStatus(const HTS Value) noexcept {
      this->Status = Value;
      return ERR::Okay;
   }

   inline ERR setError(const ERR Value) noexcept {
      this->Error = Value;
      return ERR::Okay;
   }

   inline ERR setDatatype(const DATA Value) noexcept {
      this->Datatype = Value;
      return ERR::Okay;
   }

   inline ERR setCurrentState(const HGS Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[18];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   template <class T> inline ERR setProxyServer(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[28];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setProxyPort(const int Value) noexcept {
      this->ProxyPort = Value;
      return ERR::Okay;
   }

   inline ERR setBufferSize(const int Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[34];
      return field->WriteValue(target, field, FD_INT, &Value, 1);
   }

   inline ERR setAuthCallback(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setContentType(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[35];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setIncoming(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setLocation(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[22];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setOutgoing(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setRealm(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(target, field, 0x08800300, to_cstring(Value), 1);
   }

   inline ERR setStateChanged(FUNCTION Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(target, field, FD_FUNCTION, &Value, 1);
   }

   template <class T> inline ERR setUsername(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[39];
      return field->WriteValue(target, field, 0x08800200, to_cstring(Value), 1);
   }

   template <class T> inline ERR setPassword(T && Value) noexcept {
      auto target = this;
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(target, field, 0x08800200, to_cstring(Value), 1);
   }

};

