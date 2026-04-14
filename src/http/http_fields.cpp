
/*********************************************************************************************************************
-FIELD-
AuthCallback: Private.  This field is reserved for future use.

*********************************************************************************************************************/

static ERR GET_AuthCallback(extHTTP *Self, FUNCTION **Value)
{
   if (Self->AuthCallback.defined()) {
      *Value = &Self->AuthCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_AuthCallback(extHTTP *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->AuthCallback.isScript()) UnsubscribeAction(Self->AuthCallback.Context, AC::Free);
      Self->AuthCallback = *Value;
      if (Self->AuthCallback.isScript()) {
         SubscribeAction(Self->AuthCallback.Context, AC::Free, C_FUNCTION(notify_free_auth_callback));
      }
   }
   else Self->AuthCallback.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
BufferSize: Indicates the preferred buffer size for data operations.

The default buffer size for HTTP data operations is indicated here.  It affects the size of the temporary buffer that
is used for storing outgoing data (`PUT` and `POST` operations).

Note that the actual buffer size may not reflect the exact size that you set here.

*********************************************************************************************************************/

static ERR SET_BufferSize(extHTTP *Self, int Value)
{
   if (Value < 2 * 1024) Value = 2 * 1024;
   Self->BufferSize = std::clamp(Value, BUFFER_WRITE_SIZE, 0xffff);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ConnectTimeout: The initial connection timeout value, measured in seconds.

The timeout for connect operations is specified here.  In the event of a timeout, the HTTP object will be deactivated
and the #Error field will be updated to a value of `ERR::TimeOut`.

The timeout value is measured in seconds.

-FIELD-
ContentLength: The byte length of incoming or outgoing content.

HTTP servers will return a `content-length` value in their response headers when retrieving information.  This
value is defined here once the response header is processed.  The ContentLength may be set to `-1` if the content is
being streamed from the server.

Note that if posting data to a server with an #InputFile or #InputObject as the source, the #Size field will have
priority and override any existing value in ContentLength.  In all other cases the ContentLength can be set
directly and a setting of `-1` can be used for streaming.

-FIELD-
ContentType: Defines the content-type for `PUT` and `POST` methods.

The ContentType should be set prior to sending a `PUT` or `POST` request.  If `NULL`, the default content type for
`POST` methods will be set to `application/x-www-form-urlencoded`.  For `PUT` requests the default of
`application/binary` will be applied.

*********************************************************************************************************************/

static ERR GET_ContentType(extHTTP *Self, STRING *Value)
{
   *Value = Self->ContentType.data();
   return ERR::Okay;
}

static ERR SET_ContentType(extHTTP *Self, CSTRING Value)
{
   if (Value) Self->ContentType.assign(Value);
   else Self->ContentType.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CurrentState: Indicates the current state of an HTTP object during its interaction with an HTTP server.

The CurrentState is a readable field that tracks the current state of the client in its relationship with the target HTTP
server.  The default state is `READING_HEADER`.  Changes to the state can be monitored through the #StateChanged field.

On completion of an HTTP request, the state will be changed to either `COMPLETED` or `TERMINATED`.

*********************************************************************************************************************/

static ERR SET_CurrentState(extHTTP *Self, HGS Value)
{
   pf::Log log;

   if ((int(Value) < 0) or (int(Value) >= int(HGS::END))) return log.warning(ERR::OutOfRange);

   log.detail("New State: %s, Currently: %s", clHTTPCurrentState[int(Value)].Name, clHTTPCurrentState[int(Self->CurrentState)].Name);

   if ((Value >= HGS::COMPLETED) and (Self->CurrentState < HGS::COMPLETED)) {
      Self->CurrentState = Value;
      if (Self->Socket) QueueAction(AC::Deactivate, Self->UID);
   }
   else Self->CurrentState = Value;

   if (Self->StateChanged.defined()) {
      ERR error;
      if (Self->StateChanged.isC()) {
         auto routine = (ERR (*)(extHTTP *, HGS, APTR))Self->StateChanged.Routine;
         error = routine(Self, Self->CurrentState, Self->StateChanged.Meta);
      }
      else if (Self->StateChanged.isScript()) {
         if (sc::Call(Self->StateChanged, std::to_array<ScriptArg>({
            { "HTTP", Self->UID, FD_OBJECTID },
            { "State", int(Self->CurrentState) }
         }), error) != ERR::Okay) error = ERR::Terminate;
      }
      else error = ERR::Okay;

      if (error > ERR::ExceptionThreshold) Self->Error = error;

      if (error IS ERR::Terminate) {
         if (Self->CurrentState IS HGS::SENDING_CONTENT) {
            // Stop sending and expect a response from the server.  If the client doesn't care about the response
            // then a subsequent ERR::Terminate code can be returned on notification of this state change.
            SET_CurrentState(Self, HGS::SEND_COMPLETE);
         }
         else if ((Self->CurrentState != HGS::TERMINATED) and (Self->CurrentState != HGS::COMPLETED)) {
            log.branch("State changing to HGS::COMPLETED (ERR::Terminate received).");
            SET_CurrentState(Self, HGS::COMPLETED);
         }
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DataTimeout: The data timeout value, relevant when receiving or sending data.

A timeout for send and receive operations is required to prevent prolonged waiting during data transfer operations.
This is essential when interacting with servers that stream data with indeterminate content lengths.  It should be
noted that a timeout does not necessarily indicate failure if the content is being streamed from the server
(#ContentLength is set to `-1`).

In the event of a timeout, the HTTP object will be deactivated and the #Error field will be updated to a value
of `ERR::TimeOut`.

The timeout value is measured in seconds.

-FIELD-
Datatype: The default datatype format to use when passing data to a target object.

When streaming downloaded content to an object, the default datatype is `RAW` (binary mode).  An alternative is to
send the data as `TEXT` or `XML` by changing the Datatype field value.

The receiving object can identify the data as HTTP information by checking the class ID of the sender.

-FIELD-
Error: The error code received for the most recently executed HTTP command.

On completion of an HTTP request, the most appropriate error code will be stored here.  If the request was successful
then the value will `ERR::Okay`. It should be noted that certain error codes may not necessarily indicate a
comms failure - for instance, an `ERR::TimeOut` error may be received on termination of streamed content; while
`ERR::NotAuthorised` is a credentials issue.

The recommended process for error checking is: 1. Check if the #CurrentState is `COMPLETED` or `TERMINATED`;
2. Check the #Status field for zero; 3. Check the #Error field for the error code.

-FIELD-
Flags: Optional flags.

-FIELD-
Host: The targeted HTTP server is specified here, either by name or IP address.

The HTTP server to target for HTTP requests is defined here.  To change the host post-initialisation, set the
#Location.

*********************************************************************************************************************/

static ERR SET_Host(extHTTP *Self, CSTRING Value)
{
   if (Self->Host) { FreeResource(Self->Host); Self->Host = nullptr; }
   Self->Host = pf::strclone(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Incoming: A callback routine can be defined here for incoming data.

Data can be received from an HTTP request by setting a callback routine in the Incoming field.  The format for the
callback routine is `ERR Function(*HTTP, APTR Data, INT Length)`.  For scripts the format is `Function(HTTP, Array)`.

If an error code of `ERR::Terminate` is returned or raised by the callback routine, the currently executing HTTP
request will be cancelled.

*********************************************************************************************************************/

static ERR GET_Incoming(extHTTP *Self, FUNCTION **Value)
{
   if (Self->Incoming.defined()) {
      *Value = &Self->Incoming;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Incoming(extHTTP *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->Incoming.isScript()) UnsubscribeAction(Self->Incoming.Context, AC::Free);
      Self->Incoming = *Value;
      if (Self->Incoming.isScript()) {
         SubscribeAction(Self->Incoming.Context, AC::Free, C_FUNCTION(notify_free_incoming));
      }
   }
   else Self->Incoming.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Index: Indicates the total bytes received during content transfer.

If an HTTP `GET` request is executed, the Index field will reflect the number of bytes that have been received.
This field is updated continuously until all content is received or the process is cancelled.

The Index value will always start from zero when downloading, even in resume mode.

The Index field can be monitored for changes so that progress during send and receive transmissions can be tracked.

-FIELD-
InputFile: To upload HTTP content from a file, set a file path here.

HTTP content can be streamed from a source file when a `POST` command is executed. To do so, set the InputFile
field to the file path that contains the source data.  The path is not opened or checked for validity until the
`POST` command is executed by the HTTP object.

An alternative is to set the #InputObject for abstracting the data source.

Multiple files can be specified in the InputFile field by separating each file path with a pipe symbol `|`.

*********************************************************************************************************************/

static ERR SET_InputFile(extHTTP *Self, CSTRING Value)
{
   pf::Log log;

   log.trace("InputFile: %.80s", Value);

   if (Self->InputFile) { FreeResource(Self->InputFile);  Self->InputFile = nullptr; }

   Self->MultipleInput = false;
   Self->InputPos = 0;
   if ((Value) and (*Value)) {
      Self->InputFile = pf::strclone(Value);

      // Check if the path contains multiple inputs, separated by the pipe symbol.

      for (int i=0; Self->InputFile[i]; i++) {
         if (Self->InputFile[i] IS '"') {
            i++;
            while ((Self->InputFile[i]) and (Self->InputFile[i] != '"')) i++;
            if (!Self->InputFile[i]) break;
         }
         else if (Self->InputFile[i] IS '|') {
            Self->MultipleInput = true;
            break;
         }
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
InputObject: Allows data to be sent from an object on execution of a `POST` command.

HTTP content can be streamed from a source object when a `POST` command is executed.  To do so, set the InputObject
to an object that supports the #Read() action.  The provided object ID is not checked for validity until the `POST`
command is executed by the HTTP object.

-FIELD-
Location: A valid HTTP URI must be specified here.

The URI of the HTTP source must be specified here.  The string must start with `http://` or `https://`, followed by the
host name, HTTP path and port number if required. The values mentioned will be broken down and stored in the
#Host, #Path and #Port fields respectively.  Note that if the port is not defined in the URI, the #Port field is reset
to the default (`80` for HTTP or `443` for HTTPS).

An alternative to setting the Location is to set the #Host, #Path and #Port separately.
-END-

*********************************************************************************************************************/

static ERR GET_Location(extHTTP *Self, STRING *Value)
{
   Self->AuthRetries = 0; // Reset the retry counter

   std::ostringstream str;

   if (Self->Port IS 80) str << "http://" << Self->Host << '/' << Self->Path; // http
   else if (Self->Port IS 443) {
      str << "https://" << Self->Host << '/' << Self->Path; // https
      Self->Flags |= HTF::SSL;
   }
   else if (Self->Port IS 21) str << "ftp://" << Self->Host << '/' << Self->Path; // ftp
   else str << "http://" << Self->Host << ':' << Self->Port << '/' << Self->Path;

   Self->URI = str.str();
   *Value = Self->URI.data();
   return ERR::Okay;
}

static ERR SET_Location(extHTTP *Self, CSTRING Value)
{
   pf::Log log;

   if (Self->initialised()) {
      if (Self->TimeoutManager) { UpdateTimer(Self->TimeoutManager, 0); Self->TimeoutManager = 0; }

      // Free the current socket if the entire URI changes

      if (Self->Socket) {
         Self->Socket->set(FID_Feedback, (APTR)nullptr);
         FreeResource(Self->Socket);
         Self->Socket = nullptr;
      }

      log.msg("%s", Value);
   }

   CSTRING str = Value;

   Self->Port = 80;

   if (pf::startswith("http://", str)) str += 7;
   else if (pf::startswith("https://", str)) {
      str += 8;
      Self->Port = 443;
      Self->Flags |= HTF::SSL;
   }

   if (Self->Host) { FreeResource(Self->Host); Self->Host = nullptr; }
   if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }

   // Parse host name

   int len;
   for (len=0; (str[len]) and (str[len] != ':') and (str[len] != '/'); len++);

   if (AllocMemory(len+1, MEM::STRING|MEM::NO_CLEAR, &Self->Host) != ERR::Okay) {
      return ERR::AllocMemory;
   }

   pf::copymem(str, Self->Host, len);
   Self->Host[len] = 0;

   str += len;

   // Parse port number

   if (*str IS ':') {
      str++;
      long port_long = strtol(str, nullptr, 0);
      if (port_long > 0 and port_long <= MAX_PORT_NUMBER) {
         Self->Port = int(port_long);
         if (Self->Port IS 443) Self->Flags |= HTF::SSL;
      }
      else {
         pf::Log log;
         log.warning("Invalid port number %ld, using default 80", port_long);
         Self->Port = 80;
      }
   }

   while ((*str) and (*str != '/')) str++;

   if (*str) { // Parse absolute path
      SET_Path(Self, str+1);
      return ERR::Okay;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Method: The HTTP instruction to execute (defaults to `GET`).

*********************************************************************************************************************/

static ERR SET_Method(extHTTP *Self, HTM Value)
{
   Self->Method = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ObjectMode: The transfer mode used when passing data to a targeted object.

ObjectMode defines the data transfer mode when #OutputObject field has been set for receiving incoming data.
The default setting is `DATA::FEED`, which passes data through the data feed system (see also the #Datatype to define
the type of data being sent to the object).  The alternative method is `READ_WRITE`, which uses the Write action to
send data to the targeted object.

-FIELD-
Outgoing: Outgoing data can be sent procedurally using this callback.

Outgoing data can be sent procedurally by setting this field with a callback routine.

In C++ the function prototype is `ERR Function(*HTTP, std::vector&lt;uint8_t&gt; &amp;Buffer, APTR Meta)`.
Write content to the `Buffer` and the final size will determine the amount of data sent to the server.
Alternatively use the Write() action, although this will be less efficient.

For scripting languages the function prototype is `function(HTTP)`.  Use the Write() action to send data
to the server.

If an error code of `ERR::Terminate` is returned or raised by the callback routine, any remaining data will be sent
and the transfer will be treated as having completed successfully.  Use `ERR::TimeOut` if data cannot be returned in
a reasonable time frame.  All other error codes apart from `ERR::Okay` indicate failure.

*********************************************************************************************************************/

static ERR GET_Outgoing(extHTTP *Self, FUNCTION **Value)
{
   if (Self->Outgoing.defined()) {
      *Value = &Self->Outgoing;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Outgoing(extHTTP *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->Outgoing.isScript()) UnsubscribeAction(Self->Outgoing.Context, AC::Free);
      Self->Outgoing = *Value;
      if (Self->Outgoing.isScript()) {
         SubscribeAction(Self->Outgoing.Context, AC::Free, C_FUNCTION(notify_free_outgoing));
      }
   }
   else Self->Outgoing.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputFile: To download HTTP content to a file, set a file path here.

HTTP content can be streamed to a target file during transfer.  To do so, set the OutputFile field to the destination
file name that will receive data.  If the file already exists, it will be overwritten unless the `RESUME` flag has
been set in the #Flags field.

*********************************************************************************************************************/

static ERR SET_OutputFile(extHTTP *Self, CSTRING Value)
{
   if (Self->OutputFile) { FreeResource(Self->OutputFile); Self->OutputFile = nullptr; }
   Self->OutputFile = pf::strclone(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputObject: Incoming data can be sent to the object referenced in this field.

HTTP content can be streamed to a target object during incoming data transfers. To do so, set the OutputObject to an
object that supports data feeds and/or the #Write() action. The type of method used for passing data to the
output object is determined by the setting in the #ObjectMode field.

The provided object ID is not checked for validity until the `POST` command is executed by the HTTP object.

-FIELD-
Password: The password to use when authenticating access to the server.

A password may be preset if authorisation is required against the HTTP server for access to a resource.
Note that if authorisation is required and no username and password has been preset, the HTTP object may
present a dialog box to the user to request the relevant information.

A `401` status code is returned in the event of an authorisation failure.

*********************************************************************************************************************/

static ERR SET_Password(extHTTP *Self, CSTRING Value)
{
   Self->Password.assign(Value);
   Self->PasswordPreset = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: The HTTP path targeted at the host server.

The path to target at the host server is specified here.  If no path is set, the server root will be targeted.  It is
not necessary to set the path if one has been specified in the #Location.

If spaces are discovered in the path, they will be converted to the `%20` HTTP escape code automatically.  No other
automatic conversions are operated when setting the Path field.

*********************************************************************************************************************/

static ERR SET_Path(extHTTP *Self, CSTRING Value)
{
   Self->AuthRetries = 0; // Reset the retry counter

   if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }

   if (!Value) return ERR::Okay;

   while (*Value IS '/') Value++; // Skip '/' prefix

   std::string encoded_path = encode_url_path(Value);

   if (AllocMemory(encoded_path.length() + 1, MEM::STRING|MEM::NO_CLEAR, &Self->Path) IS ERR::Okay) {
      pf::strcopy(encoded_path, Self->Path, encoded_path.length() + 1);

      // Check if this path has been authenticated against the server yet by comparing it to AuthPath.  We need to
      // do this if a PUT instruction is executed against the path and we're not authenticated yet.

      auto pview = std::string_view(Self->Path, encoded_path.length());
      auto folder_len = pview.find_last_of('/');
      if (folder_len IS std::string::npos) folder_len = 0;

      Self->SecurePath = true;
      if (!Self->AuthPath.empty()) {
         if (Self->AuthPath.size() IS folder_len) {
            pview.remove_suffix(pview.size() - folder_len);
            if (pview IS Self->AuthPath) { // No change to the current path
               Self->SecurePath = false;
            }
         }
      }

      Self->AuthPath.assign(Self->Path, folder_len);
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

/*********************************************************************************************************************

-FIELD-
Port: The HTTP port to use when targeting a host.

The Port to target at the HTTP host is defined here and defaults to `80`.

-FIELD-
ProxyPort: The port to use when communicating with a proxy server.

If a #ProxyServer has been set, the ProxyPort must define the proxy communication port number.  The default value
is `8080`.

-FIELD-
ProxyServer: Route the HTTP request through the proxy server defined here.

If a proxy server will receive the HTTP request, set the name or IP address of the server here.  To specify the port
that the proxy server uses to receive requests, see the #ProxyPort field.

*********************************************************************************************************************/

static ERR SET_ProxyServer(extHTTP *Self, CSTRING Value)
{
   if (Self->ProxyServer) { FreeResource(Self->ProxyServer); Self->ProxyServer = nullptr; }
   if ((Value) and (Value[0])) Self->ProxyServer = pf::strclone(Value);
   Self->ProxyDefined = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Realm: Identifies the realm during HTTP authentication.

During the user authentication process, a realm name may be returned by the HTTP server and this will be reflected
here.

*********************************************************************************************************************/

static ERR GET_Realm(extHTTP *Self, CSTRING *Value)
{
   if (Self->Realm.empty()) *Value = nullptr;
   else *Value = Self->Realm.c_str();
   return ERR::Okay;
}

static ERR SET_Realm(extHTTP *Self, CSTRING Value)
{
   if (Value) Self->Realm.assign(Value);
   else Self->Realm.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RecvBuffer: Refers to a data buffer that is used to store all incoming content.

If the `RECV_BUFFER` flag is set, all content received from the HTTP server will be stored in a managed buffer
that is referred to by this field.  This field can be read at any time.  The buffer content is reset whenever the
HTTP object is activated.

*********************************************************************************************************************/

static ERR GET_RecvBuffer(extHTTP *Self, uint8_t **Value, int *Elements)
{
   *Value = (uint8_t *)Self->RecvBuffer.data();
   *Elements = Self->RecvBuffer.size();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Size: Set this field to define the length of a data transfer when issuing a `POST` command.

Prior to the execution of a `POST` command, it is recommended that the Size field is set to the length of the data
transfer.  If this field is not set, the HTTP object will attempt to determine the byte size of
the transfer by reading the size from the source file or object.

-FIELD-
StateChanged: This callback reports changes to the HTTP state.

Define a callback routine in StateChanged in order to receive notifications of any change to the #CurrentState of an
HTTP object.  The format for the routine is `ERR Function(*HTTP, HGS State)`.

If an error code of `ERR::Terminate` is returned by the callback routine, the currently executing HTTP request will be
cancelled.

*********************************************************************************************************************/

static ERR GET_StateChanged(extHTTP *Self, FUNCTION **Value)
{
   if (Self->StateChanged.defined()) {
      *Value = &Self->StateChanged;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_StateChanged(extHTTP *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->StateChanged.isScript()) UnsubscribeAction(Self->StateChanged.Context, AC::Free);
      Self->StateChanged = *Value;
      if (Self->StateChanged.isScript()) {
         SubscribeAction(Self->StateChanged.Context, AC::Free, C_FUNCTION(notify_free_state_changed));
      }
   }
   else Self->StateChanged.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Status: Indicates the HTTP status code returned on completion of an HTTP request.

The Status value is only valid on completion of an HTTP request.  A value of `200` indicates that the
request was successful.  Other values indicate either redirection or an error condition.  For a full list of
HTTP status codes, see https://www.w3.org/Protocols/rfc2616/rfc

If the Status value is `NIL` on completion, the request has failed without a valid response being received from the
server.  Refer to the #Error field for more information.

-FIELD-
UserAgent: Specifies the name of the user-agent string that is sent in HTTP requests.

This field describes the `user-agent` value that will be sent in HTTP requests.  The default value is `Kotuku Client`.

*********************************************************************************************************************/

static ERR SET_UserAgent(extHTTP *Self, CSTRING Value)
{
   if (Self->UserAgent) { FreeResource(Self->UserAgent); Self->UserAgent = nullptr; }
   Self->UserAgent = pf::strclone(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Username: The username to use when authenticating access to the server.

A username can be preset before executing an HTTP method against a secure server zone.  The supplied credentials will
only be passed to the HTTP server if it asks for authorisation.  The username provided should be accompanied by a
#Password.

In the event that a username or password is not supplied, or if the supplied credentials are invalid, the user will be
presented with a dialog box and asked to enter the correct username and password.
-END-

*********************************************************************************************************************/

static ERR SET_Username(extHTTP *Self, CSTRING Value)
{
   Self->Username.assign(Value);
   return ERR::Okay;
}
