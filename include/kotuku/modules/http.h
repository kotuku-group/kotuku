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
   PATCH = 24,
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
   NO_AUTO_REDIRECT = 0x00000800,
};

DEFINE_ENUM_FLAG_OPERATORS(HTF)

// HTTP class definition

#define VER_HTTP (1.000000)

class objHTTP : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::HTTP;
   static constexpr CSTRING CLASS_NAME = "HTTP";

   using create = kt::Create<objHTTP>;
   objHTTP(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   double   DataTimeout;     // The data timeout value, relevant when receiving or sending data.
   double   ConnectTimeout;  // The initial connection timeout value, measured in seconds.
   int64_t  Index;           // Indicates the total bytes received during content transfer.
   int64_t  ContentLength;   // The byte length of incoming or outgoing content.
   int64_t  Size;            // Set this field to define the length of a data transfer when issuing a POST command.
   std::string Host;         // The targeted HTTP server is specified here, either by name or IP address.
   std::string Path;         // The HTTP path targeted at the host server.
   std::string OutputFile;   // To download HTTP content to a file, set a file path here.
   std::string InputFile;    // To upload HTTP content from a file, set a file path here.
   std::string UserAgent;    // Specifies the name of the user-agent string that is sent in HTTP requests.
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
   std::string ProxyServer;  // Route the HTTP request through the proxy server defined here.
   int      ProxyPort;       // The port to use when communicating with a proxy server.
   int      BufferSize;      // Indicates the preferred buffer size for data operations.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR getKey(std::string_view Key, std::string &Value) noexcept {
      struct acGetKey args = { Key, &Value };
      auto error = Action(AC::GetKey, this, &args);
      if (error != ERR::Okay) Value.clear();
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
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

   // Customised field getting

   inline ERR getDataTimeout(double &Value) noexcept {
      Value = this->DataTimeout;
      return ERR::Okay;
   }

   inline ERR getConnectTimeout(double &Value) noexcept {
      Value = this->ConnectTimeout;
      return ERR::Okay;
   }

   inline ERR getIndex(int64_t &Value) noexcept {
      Value = this->Index;
      return ERR::Okay;
   }

   inline ERR getContentLength(int64_t &Value) noexcept {
      Value = this->ContentLength;
      return ERR::Okay;
   }

   inline ERR getSize(int64_t &Value) noexcept {
      Value = this->Size;
      return ERR::Okay;
   }

   inline ERR getHost(std::string_view &Value) noexcept {
      Value = this->Host;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getOutputFile(std::string_view &Value) noexcept {
      Value = this->OutputFile;
      return ERR::Okay;
   }

   inline ERR getInputFile(std::string_view &Value) noexcept {
      Value = this->InputFile;
      return ERR::Okay;
   }

   inline ERR getUserAgent(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getInputObject(OBJECTID &Value) noexcept {
      Value = this->InputObjectID;
      return ERR::Okay;
   }

   inline ERR getOutputObject(OBJECTID &Value) noexcept {
      Value = this->OutputObjectID;
      return ERR::Okay;
   }

   inline ERR getMethod(HTM &Value) noexcept {
      Value = this->Method;
      return ERR::Okay;
   }

   inline ERR getPort(int &Value) noexcept {
      Value = this->Port;
      return ERR::Okay;
   }

   inline ERR getObjectMode(HOM &Value) noexcept {
      Value = this->ObjectMode;
      return ERR::Okay;
   }

   inline ERR getFlags(HTF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getStatus(HTS &Value) noexcept {
      Value = this->Status;
      return ERR::Okay;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getDatatype(DATA &Value) noexcept {
      Value = this->Datatype;
      return ERR::Okay;
   }

   inline ERR getCurrentState(HGS &Value) noexcept {
      Value = this->CurrentState;
      return ERR::Okay;
   }

   inline ERR getProxyServer(std::string_view &Value) noexcept {
      Value = this->ProxyServer;
      return ERR::Okay;
   }

   inline ERR getProxyPort(int &Value) noexcept {
      Value = this->ProxyPort;
      return ERR::Okay;
   }

   inline ERR getBufferSize(int &Value) noexcept {
      Value = this->BufferSize;
      return ERR::Okay;
   }

   inline ERR getContentType(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getIncoming(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getLocation(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[34];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getOutgoing(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[29];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getRealm(std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::string_view &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getRecvBuffer(std::span<int8_t> &Value) noexcept {
      auto field = &this->Class->Dictionary[0];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::span<int8_t> &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getResponseKeys(std::span<std::string> &Value) noexcept {
      auto field = &this->Class->Dictionary[28];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::span<std::string> &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getStateChanged(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
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

   inline ERR setHost(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[25];
      return field->WriteValue(this, field, 0x00804500, &Value);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setOutputFile(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[15];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setInputFile(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[16];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setUserAgent(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, 0x00804300, &Value);
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
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_INT, &Value);
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
      auto field = &this->Class->Dictionary[4];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setProxyServer(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setProxyPort(const int Value) noexcept {
      this->ProxyPort = Value;
      return ERR::Okay;
   }

   inline ERR setBufferSize(const int Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setContentType(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[21];
      return field->WriteValue(this, field, 0x00804308, &Value);
   }

   inline ERR setIncoming(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[10];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setLocation(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[34];
      return field->WriteValue(this, field, 0x00804308, &Value);
   }

   inline ERR setOutgoing(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[29];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setRealm(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, 0x00804308, &Value);
   }

   inline ERR setStateChanged(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[31];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setUsername(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[37];
      return field->WriteValue(this, field, 0x00804208, &Value);
   }

   inline ERR setPassword(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, 0x00804208, &Value);
   }

};