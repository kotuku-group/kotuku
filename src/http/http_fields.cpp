
/*********************************************************************************************************************
-FIELD-
AuthCallback: Private.  This field is reserved for future use.

*********************************************************************************************************************/

static ERR GET_AuthCallback(extHTTP *Self, FUNCTION * &Value)
{
   if (Self->AuthCallback.defined()) {
      Value = &Self->AuthCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_AuthCallback(extHTTP *Self, FUNCTION *Value)
{
   clear_callback_function(Self->AuthCallback);
   if (Value) {
      Self->AuthCallback = *Value;
      Self->AuthCallback.pin();
   }
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

static ERR GET_ContentType(extHTTP *Self, std::string_view &Value)
{
   Value = Self->ContentType;
   return ERR::Okay;
}

static ERR SET_ContentType(extHTTP *Self, const std::string_view &Value)
{
   Self->ContentType.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CurrentState: Indicates the current state of an HTTP object during its interaction with an HTTP server.

The CurrentState is a readable field that tracks the current state of the client in its relationship with the target HTTP
server.  The default state is `READING_HEADER`.  Changes to the state can be monitored through the #StateChanged field.

Ordinary requests end in `COMPLETED` or `TERMINATED`.  A committed protocol handover ends in `UPGRADED`, with no
download completion or HTTP connection ownership.

*********************************************************************************************************************/

static ERR SET_CurrentState(extHTTP *Self, HGS Value)
{
   kt::Log log;

   if ((int(Value) < 0) or (int(Value) >= int(HGS::END))) return log.warning(ERR::OutOfRange);

   log.detail("New State: %s, Currently: %s", clHTTPCurrentState[int(Value)].Name, clHTTPCurrentState[int(Self->CurrentState)].Name);

   if ((Value IS HGS::UPGRADE_READY) or (Value IS HGS::UPGRADED)) return ERR::NoFieldAccess;

   if (Self->inHandover() and ((Value != HGS::TERMINATED) or http_handed_over(Self->CurrentState))) return ERR::InUse;

   if (Self->UpgradeState and Self->UpgradeState->ReadyDispatch and (Value IS HGS::TERMINATED)) {
      Self->CurrentState = Value;
      Self->RequestActive = false;
      return ERR::Okay; // Handover emits the terminal notification after the ready handler unwinds.
   }

   bool finished = http_complete(Value) or http_failed(Value);
   bool was_active = http_active(Self->CurrentState);

   Self->CurrentState = Value;
   if (finished) Self->RequestActive = false;

   const auto generation = Self->RequestGeneration;

   Self->pin();
   auto lifetime = kt::Defer([&]() { Self->unpin(); });

   auto completion = kt::Defer([&]() {
      // Preserve action subscriptions and callback ordering without leaving a stale queued cleanup action.
      if (finished and was_active and (not Self->collecting()) and
          ((not Self->inHandover()) or (Value IS HGS::TERMINATED)) and
          (Self->RequestGeneration IS generation) and (Self->CurrentState IS Value) and Self->Socket) {
         acDeactivate(Self);
      }
   });

   if (Self->StateChanged.stale()) clear_callback_function(Self->StateChanged);

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

      if (Self->collecting()) return ERR::Terminate;
      if (error > ERR::ExceptionThreshold) Self->Error = error; // ERR:Terminate excluded

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

static ERR SET_Host(extHTTP *Self, const std::string_view &Value)
{
   Self->Host.assign(Value);
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

static ERR GET_Incoming(extHTTP *Self, FUNCTION * &Value)
{
   if (Self->Incoming.defined()) {
      Value = &Self->Incoming;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Incoming(extHTTP *Self, FUNCTION *Value)
{
   clear_callback_function(Self->Incoming);
   if (Value) {
      Self->Incoming = *Value;
      Self->Incoming.pin();
   }
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

static ERR SET_InputFile(extHTTP *Self, const std::string_view &Value)
{
   kt::Log log;

   log.trace("InputFile: %.*s", int(std::min<size_t>(Value.size(), 80)), Value.data());

   Self->InputFile.clear();
   Self->MultipleInput = false;
   Self->InputPos = 0;
   if (not Value.empty()) {
      Self->InputFile.assign(Value);

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

static ERR GET_Location(extHTTP *Self, std::string_view &Value)
{
   std::ostringstream str;
   str << (((Self->Flags & HTF::SSL) != HTF::NIL) ? "https://" : "http://") <<
      http_authority(Self) << '/' << Self->Path;

   Self->URI = str.str();
   Value = Self->URI;
   return ERR::Okay;
}

static ERR SET_Location(extHTTP *Self, const std::string_view &Value)
{
   kt::Log log;

   if (Value.empty()) return ERR::InvalidValue;

   auto uri = Value;
   int new_port = 80;
   bool new_ssl = false;

   if (uri.starts_with("http://")) uri.remove_prefix(7);
   else if (uri.starts_with("https://")) {
      uri.remove_prefix(8);
      new_port = 443;
      new_ssl = true;
   }
   else return ERR::InvalidValue;

   bool bracketed = uri.starts_with("[");
   auto host_len = bracketed ? uri.find(']') : uri.find_first_of(":/?#");
   if (bracketed and (host_len IS std::string_view::npos)) return ERR::InvalidValue;
   if (host_len IS std::string_view::npos) host_len = uri.size();
   if (!host_len) return ERR::InvalidValue;

   auto host = bracketed ? uri.substr(1, host_len - 1) : uri.substr(0, host_len);
   auto path = uri.substr(host_len + (bracketed ? 1 : 0));

   if ((!path.empty()) and (path.front() IS ':')) {
      path.remove_prefix(1);

      auto port_end = path.find_first_of("/?#");
      auto port_text = path.substr(0, port_end);
      if (port_text.empty()) return ERR::InvalidValue;

      int port_value = 0;
      auto [ ptr, error ] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port_value);
      if ((error != std::errc()) or (ptr != port_text.data() + port_text.size()) or
            (port_value <= 0) or (port_value > MAX_PORT_NUMBER)) {
         return ERR::InvalidValue;
      }

      new_port = port_value;
      if (new_port IS 443) new_ssl = true;

      path = (port_end IS std::string_view::npos) ? std::string_view() : path.substr(port_end);
   }

   if (Self->initialised()) {
      if (Self->TimeoutManager) { UpdateTimer(Self->TimeoutManager, 0); Self->TimeoutManager = 0; }

      // Free the current socket if the entire URI changes

      if (Self->Socket) {
         Self->Socket->setFeedback(FUNCTION{});
         FreeResource(Self->Socket);
         Self->Socket = nullptr;
      }

      log.msg("%.*s", int(Value.size()), Value.data());
   }

   Self->Port = new_port;
   if (new_ssl) Self->Flags |= HTF::SSL;
   else Self->Flags &= ~HTF::SSL;

   Self->Host.assign(host);
   Self->Path.clear();

   if (not path.empty()) { // Parse absolute path
      if (path.front() IS '/') path.remove_prefix(1);
      return SET_Path(Self, path.substr(0, path.find('#')));
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

ObjectMode defines the data transfer mode when the #OutputObject field has been set for receiving incoming data.
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

static ERR GET_Outgoing(extHTTP *Self, FUNCTION * &Value)
{
   if (Self->Outgoing.defined()) {
      Value = &Self->Outgoing;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Outgoing(extHTTP *Self, FUNCTION *Value)
{
   clear_callback_function(Self->Outgoing);
   if (Value) {
      Self->Outgoing = *Value;
      Self->Outgoing.pin();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputFile: To download HTTP content to a file, set a file path here.

HTTP content can be streamed to a target file during transfer.  To do so, set the OutputFile field to the destination
file name that will receive data.  If the file already exists, it will be overwritten unless the `RESUME` flag has
been set in the #Flags field.

*********************************************************************************************************************/

static ERR SET_OutputFile(extHTTP *Self, const std::string_view &Value)
{
   Self->OutputFile.assign(Value);
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

static ERR SET_Password(extHTTP *Self, const std::string_view &Value)
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

static ERR SET_Path(extHTTP *Self, const std::string_view &Value)
{
   Self->AuthRetries = 0; // Reset the retry counter

   Self->Path.clear();

   if (Value.empty()) return ERR::Okay;

   auto path = Value;
   while ((not path.empty()) and (path.front() IS '/')) path.remove_prefix(1); // Skip '/' prefix

   std::string encoded_path = encode_url_path(path);

   Self->Path.assign(encoded_path);

   // Check if this path has been authenticated against the server yet by comparing it to AuthPath.  We need to
   // do this if a PUT instruction is executed against the path and we're not authenticated yet.

   auto pview = std::string_view(Self->Path.data(), Self->Path.size());
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

   Self->AuthPath.assign(Self->Path, 0, folder_len);
   return ERR::Okay;
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

static ERR SET_ProxyServer(extHTTP *Self, const std::string_view &Value)
{
   Self->ProxyServer.assign(Value);
   Self->ProxyDefined = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Realm: Identifies the realm during HTTP authentication.

During the user authentication process, a realm name may be returned by the HTTP server and this will be reflected
here.

*********************************************************************************************************************/

static ERR GET_Realm(extHTTP *Self, std::string_view &Value)
{
   Value = Self->Realm;
   return ERR::Okay;
}

static ERR SET_Realm(extHTTP *Self,  const std::string_view &Value)
{
   Self->Realm.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RecvBuffer: Refers to a data buffer that is used to store all incoming content.

If the `RECV_BUFFER` flag is set, all content received from the HTTP server will be stored in a managed buffer
that is referred to by this field.  This field can be read at any time.  The buffer content is reset whenever the
HTTP object is activated.

*********************************************************************************************************************/

static ERR GET_RecvBuffer(extHTTP *Self, std::span<int8_t> &Value)
{
   Value = std::span<int8_t>((int8_t *)Self->RecvBuffer.data(), Self->RecvBuffer.size());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ResponseKeys: Returns a string list of received response keys.

This field returns a string list of all the keys found in the HTTP response header.  An empty list will be returned
if no keys are found.

*********************************************************************************************************************/

static ERR GET_ResponseKeys(extHTTP *Self, std::span<std::string> &Value)
{
   Self->ResponseKeys.clear();
   Self->ResponseKeys.reserve(Self->ResponseHeaders.size());

   for (const auto &response_header : Self->ResponseHeaders) {
      Self->ResponseKeys.emplace_back(response_header.first);
   }

   Value = std::span<std::string>(Self->ResponseKeys.data(), Self->ResponseKeys.size());
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

For ordinary state notifications, returning `ERR::Terminate` cancels the executing request using the existing completion
rules.  For an opt-in WebSocket request, `UPGRADE_READY` is synchronous and only an explicit `ERR::Okay` return accepts.
Rejection reports `TERMINATED`.
The ready handler validates protocol-specific response fields with #GetResponseHeaders().  HTTP validates only the
HTTP/1.1 envelope, Connection token and WebSocket selection.

After the ready handler unwinds, HTTP stages and validates the owner change, detaches its socket and notifies
`UPGRADED`.
Read #Upgrade during that notification, install the recipient socket callbacks and consume or copy the binary prefix.
Socket dispatch remains suspended until this notification finishes.  Errors in `UPGRADED` are diagnostic and cannot
reclaim the connection.  Do not pump messages or read further socket bytes inside either upgrade notification.

*********************************************************************************************************************/

static ERR GET_StateChanged(extHTTP *Self, FUNCTION * &Value)
{
   if (Self->StateChanged.defined()) {
      Value = &Self->StateChanged;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_StateChanged(extHTTP *Self, FUNCTION *Value)
{
   clear_callback_function(Self->StateChanged);
   if (Value) {
      Self->StateChanged = *Value;
      Self->StateChanged.pin();
   }
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

static ERR GET_UserAgent(extHTTP *Self, std::string_view &Value)
{
   if (Self->UserAgent.empty()) Value = "Kotuku Client";
   else Value = std::string_view(Self->UserAgent.data(), Self->UserAgent.size());
   return ERR::Okay;
}

static ERR SET_UserAgent(extHTTP *Self,  const std::string_view &Value)
{
   Self->UserAgent.assign(Value);
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

static ERR SET_Username(extHTTP *Self, const std::string_view &Value)
{
   Self->Username.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Upgrade: Configures the recipient and exposes a notification-scoped connection descriptor.

Before activation, write an !HTTPUpgrade with only `Owner` set.  `Owner` must be alive and outside HTTP's
ownership subtree.

This field is readable during the `UPGRADED` #StateChanged notification only.  Each read returns a
descriptor snapshot.  Altering it has no effect on HTTP.  The values are valid only during the notification.

-RESULT-
InUse: HTTP is active or handover is in progress.
InvalidValue: The upgrade descriptor is invalid or the owner is not valid for this HTTP
NoFieldAccess: The field is not readable outside the UPGRADED notification, or the owner is collecting
-END-
*********************************************************************************************************************/

static ERR SET_Upgrade(extHTTP *Self, HTTPUpgrade *Value)
{
   if (Self->RequestActive or Self->inHandover()) return ERR::InUse;
   if ((not Value) or Value->Socket or (not Value->Data.empty())) return ERR::InvalidValue;
   kt::ScopedObjectLock owner(Value->OwnerID);
   if ((not owner.granted()) or (not valid_upgrade_owner(Self, *owner))) return ERR::InvalidValue;
   Self->upgradeState().Owner = Value->OwnerID;
   return ERR::Okay;
}

static ERR GET_Upgrade(extHTTP *Self, HTTPUpgrade **Value)
{
   *Value = nullptr;
   auto upgrade = Self->UpgradeState.get();
   if ((not upgrade) or (not upgrade->Readable) or (not upgrade->Handover)) return ERR::NoFieldAccess;
   kt::ScopedObjectLock owner(upgrade->Result.OwnerID);
   if ((not owner.granted()) or owner->collecting()) return ERR::NoFieldAccess;
   upgrade->Snapshot = upgrade->Result;
   *Value = &upgrade->Snapshot;
   return ERR::Okay;
}
