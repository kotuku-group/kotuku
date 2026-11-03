/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
WebChannel: Exchanges text and binary messages with a remote peer over a PeerConnection.

A WebChannel is a bidirectional message channel that is carried by a @PeerConnection.  Messages are always
delivered whole, and each is flagged as either text or binary.  A connection can carry many channels, each with its
own label, ordering and reliability settings.

WebChannel objects cannot be created directly.  A local channel is created with @PeerConnection.CreateWebChannel(),
and a channel that is announced by the remote peer is delivered through @PeerConnection.OnWebChannel.  In both cases
the channel is owned by the PeerConnection and is destroyed with it.  The #Label, #Protocol, #Negotiated, #Ordered,
#MaxRetransmits and #MaxPacketLifeTime fields are set on creation and cannot be changed.

A new channel starts in the `CONNECTING` state and calls #OnOpen when it is ready for use.  A local channel opens
once the remote peer acknowledges it, or once the connection is established if the channel is negotiated.  A remote
channel is reported through @PeerConnection.OnWebChannel before any of its other events.  Messages are then sent with
#Send() and received through #OnMessage.

When a channel closes normally, #OnClose is called once.  If the channel fails, #OnError is called first, followed by
#OnClose.  Freeing a channel closes it without making any further callbacks.

Channel events are delivered by the parent's framework timer, so the thread that created the PeerConnection must
process messages, and the channel must only be used from that thread.  Callbacks are never made from within a call to
#Send() or #Close().  A callback can safely send messages, or close or free the channel or its parent.
-END-

*********************************************************************************************************************/

extWebChannel::~extWebChannel()
{
   if (Parent) {
      Parent->Channels.erase(Handle);
      if (Parent->Connection) Parent->Connection->detach_channel(Handle);
   }

   for (auto field : {&OnOpen, &OnMessage, &OnClose, &OnError, &OnBufferedAmountLow}) rtc_clear_callback(*field);
}

//********************************************************************************************************************
// WebChannel objects can only be created by their parent PeerConnection.

static ERR WEBCHANNEL_Init(extWebChannel *Self)
{
   return Self->Parent ? ERR::Okay : ERR::InvalidState;
}

//********************************************************************************************************************
// Creates a WebChannel object for an engine channel record, owned by its parent PeerConnection.

static ERR make_web_channel(extPeerConnection *Parent, uint64_t Handle, extWebChannel * &Result)
{
   Result = nullptr;

   auto record = Parent->Connection->channel(Handle);
   if (not record) return ERR::InvalidState;

   OBJECTPTR object;
   auto error = NewObject(CLASSID::WEBCHANNEL, NF::NIL, &object);
   if (error != ERR::Okay) return error;

   auto child = (extWebChannel *)object;
   child->Parent = Parent;
   child->Handle = Handle;
   child->Label = record->options.label;
   child->Protocol = record->options.protocol;
   child->StreamID = record->id;
   child->Negotiated = record->options.negotiated;
   child->Ordered = record->options.ordered;
   child->MaxRetransmits = int(record->options.reliability.max_retransmits.value_or(uint32_t(-1)));
   child->MaxPacketLifeTime = int(record->options.reliability.max_lifetime_ms.value_or(uint32_t(-1)));

   error = SetOwner(child, Parent);
   if (error IS ERR::Okay) error = InitObject(child);
   if (error != ERR::Okay) {
      FreeResource(child);
      return error;
   }

   Parent->Channels[Handle] = child;
   Result = child;
   return ERR::Okay;
}

//********************************************************************************************************************
// Calls a WebChannel callback field for a channel event.  The callback is pinned for the duration of the call, and
// errors returned by message handlers are logged.

static void channel_notify(extWebChannel *Self, FUNCTION &Field, const rtc::PublicChannelEvent &Event)
{
   if (Self->collecting() or not Self->Parent or Self->Parent->collecting()) return;

   if (not rtc_live_callback(Field)) return;

   RetainedCallback retained(Field);
   auto &callback = retained.function;

   ERR error = ERR::Okay;
   if (callback.isC()) {
      kt::SwitchContext context(callback.Context);
      if (Event.type IS rtc::SctpChannelEventType::MESSAGE and not Event.low) {
         auto routine = (ERR (*)(objWebChannel *, const void *, int, WCM, APTR))callback.Routine;
         error = routine(Self, Event.payload.data(), int(Event.payload.size()),
            Event.text ? WCM::TEXT : WCM::BINARY, callback.Meta);
      }
      else if (Event.type IS rtc::SctpChannelEventType::ERROR) {
         auto routine = (void (*)(objWebChannel *, ERR, APTR))callback.Routine;
         routine(Self, Event.error, callback.Meta);
      }
      else {
         auto routine = (void (*)(objWebChannel *, APTR))callback.Routine;
         routine(Self, callback.Meta);
      }
   }
   else if (callback.isScript()) {
      if (Event.type IS rtc::SctpChannelEventType::MESSAGE and not Event.low) {
         std::span<std::byte> span((std::byte *)Event.payload.data(), Event.payload.size());
         sc::Call(callback, std::to_array<ScriptArg>({
            { "WebChannel", Self, FD_OBJECTPTR }, { "Buffer", &span, FDF_SPAN|FD_BYTE },
            { "Type", int(Event.text ? WCM::TEXT : WCM::BINARY) }
         }), error);
      }
      else if (Event.type IS rtc::SctpChannelEventType::ERROR) {
         sc::Call(callback, std::to_array<ScriptArg>({
            { "WebChannel", Self, FD_OBJECTPTR }, { "Error", int(Event.error) }
         }), error);
      }
      else sc::Call(callback, std::to_array<ScriptArg>({ { "WebChannel", Self, FD_OBJECTPTR } }), error);
   }

   if (error > ERR::ExceptionThreshold) kt::Log(__FUNCTION__).warning(error);
}

/*********************************************************************************************************************

-METHOD-
Close: Closes the channel in both directions.

Close() changes #State to `CLOSING` and rejects any further calls to #Send().  Reliable messages that have already
been queued are delivered before the channel's stream is reset.  Partially reliable messages remain subject to their
#MaxRetransmits or #MaxPacketLifeTime limits.  #OnClose is called once the remote peer has completed the reset.

If the remote peer does not complete the reset within five seconds, the connection fails with `ERR::Timeout`.  A
channel that has not yet been assigned a stream is closed immediately.

Calling Close() on a channel that is closing or closed has no effect.  Freeing the channel also closes it, but
without calling #OnClose.

-ERRORS-
Okay
InvalidState
OutOfRange
BufferOverflow: Too many stream resets are pending on the connection.

-TAGS-
non-blocking, mutates-object, idempotent
-END-

*********************************************************************************************************************/

static ERR WEBCHANNEL_Close(extWebChannel *Self)
{
   if (Self->State IS WCS::CLOSED or Self->State IS WCS::CLOSING) return ERR::Okay;

   if (not Self->Parent) {
      Self->State = WCS::CLOSED;
      return ERR::Okay;
   }

   auto error = Self->Parent->Connection->close_channel(Self->Handle);
   if (error IS ERR::Okay) Self->State = WCS::CLOSING;
   return error;
}

/*********************************************************************************************************************

-METHOD-
Send: Queues a message for delivery to the remote peer.

Send() copies a complete message to the channel's send queue and returns immediately.  The message is either queued
in full or rejected, and is delivered to the remote peer as a single message.  A successful result does not indicate
that the message has been sent or received.  The number of bytes awaiting transmission is reported by
#BufferedAmount.

The `Type` is delivered with the message so that the receiver can tell text from binary data; the content itself is
not altered.  Use `TEXT` for strings, which browsers receive as a JavaScript string, and `BINARY` for arbitrary
bytes, which browsers receive as an `ArrayBuffer` or `Blob`.  Both types can be mixed on the same channel.  Refer to
RFC 8831 for further information.

The channel must be in the `OPEN` state.  Messages of type `TEXT` must be valid UTF-8, and no message can exceed
#MaxMessageSize.  Empty messages are permitted.

The send queue is limited to 16 MiB for each channel and 32 MiB across the connection.  If a message would exceed
these limits it is rejected with `ERR::BufferOverflow`, and the client should wait for #OnBufferedAmountLow before
sending more data.

In Tiri, `Data` can be a string, which may contain embedded null bytes, an `array<byte>`, or `nil` for an empty
message.

-INPUT-
array(char) Data: The message content.
int(WCM) Type: Declares the message as `TEXT` or `BINARY`.

-ERRORS-
Okay
NullArgs
InvalidState: The channel is not open.
InvalidValue: `Type` is not a recognised value.
InvalidData: A `TEXT` message is not valid UTF-8.
OutOfRange: The message exceeds #MaxMessageSize.
BufferOverflow: The send queue is full.
NoSupport: The remote peer does not support partially reliable delivery.

-TAGS-
non-blocking, mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR WEBCHANNEL_Send(extWebChannel *Self, struct wch::Send *Args)
{
   if (not Args) return ERR::NullArgs;

   if (not Self->Parent or Self->Parent->collecting() or Self->collecting() or Self->State != WCS::OPEN) {
      return ERR::InvalidState;
   }

   if (Args->Type != WCM::TEXT and Args->Type != WCM::BINARY) return ERR::InvalidValue;

   auto error = Self->Parent->Connection->send_channel(Self->Handle,
      {(const uint8_t *)Args->Data.data(), Args->Data.size()}, Args->Type IS WCM::TEXT);
   if (auto record = Self->Parent->Connection->channel(Self->Handle)) Self->BufferedAmount = int64_t(record->buffered);
   return error;
}

/*********************************************************************************************************************

-FIELD-
StreamID: The channel's SCTP stream ID.

The stream ID identifies the channel on the connection.  If the ID was not specified when the channel was created,
it is -1 until it is assigned automatically after the DTLS handshake.
-END-

*********************************************************************************************************************/

static ERR GET_StreamID(extWebChannel *Self, int &Value)
{
   auto record = Self->Parent ? Self->Parent->Connection->channel(Self->Handle) : nullptr;
   Value = record ? record->id : Self->StreamID;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxMessageSize: The largest message, in bytes, that can be sent to the remote peer.

MaxMessageSize reflects the limit advertised in the remote peer's session description, and is zero until the remote
description has been applied.  If the remote peer does not advertise a limit, the default is 65536 bytes.  If it
advertises no limit, or a limit above 16 MiB, the value is capped at 16 MiB.  Messages sent with #Send() cannot exceed
this size.
-END-

*********************************************************************************************************************/

static ERR GET_MaxMessageSize(extWebChannel *Self, int64_t &Value)
{
   Value = Self->Parent ? int64_t(Self->Parent->Connection->max_message_size()) : 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
BufferedAmount: The number of message bytes that are queued and have not yet been transmitted.

BufferedAmount counts the message content that has been queued with #Send() but not yet passed to the transport.  It
decreases as data is transmitted, or when partially reliable messages are abandoned.  Protocol overhead and
retransmissions are not counted.

Transmitted data continues to occupy the send queue until the remote peer acknowledges it, so #Send() can reject a
message with `ERR::BufferOverflow` even when BufferedAmount is low.  Use #BufferedAmountLowThreshold and
#OnBufferedAmountLow to regulate the rate of sending.
-END-

*********************************************************************************************************************/

static ERR GET_BufferedAmount(extWebChannel *Self, int64_t &Value)
{
   auto record = Self->Parent ? Self->Parent->Connection->channel(Self->Handle) : nullptr;
   Value = record ? int64_t(record->buffered) : 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
BufferedAmountLowThreshold: The #BufferedAmount level, in bytes, at which #OnBufferedAmountLow is called.

#OnBufferedAmountLow is called when #BufferedAmount falls from above this threshold to a value at or below it.  The
default is zero, and the value can be set from 0 to 16777216.  Changing the threshold does not trigger a callback.
-END-

*********************************************************************************************************************/

static ERR SET_BufferedAmountLowThreshold(extWebChannel *Self, int64_t Value)
{
   if (Value < 0 or Value > 16777216) return ERR::OutOfRange;
   if (not Self->Parent) return ERR::InvalidState;

   auto error = Self->Parent->Connection->set_threshold(Self->Handle, size_t(Value));
   if (error IS ERR::Okay) Self->BufferedAmountLowThreshold = Value;
   return error;
}

/*********************************************************************************************************************

-FIELD-
OnClose: A callback that is notified when the channel has closed.

The function referenced here is called once when the channel closes, whether it was closed by either peer or failed.
#State is `CLOSED` when the callback is made.  It is not called if the channel is freed.  The C++ prototype is
`Function(*WebChannel, APTR Meta)`.

The Tiri prototype is `function(WebChannel)`.

-TAGS-
callback-held
-END-

*********************************************************************************************************************/

static ERR GET_OnClose(extWebChannel *Self, FUNCTION * &Value)
{
   if (not Self->OnClose.defined()) return ERR::FieldNotSet;
   Value = &Self->OnClose;
   return ERR::Okay;
}

static ERR SET_OnClose(extWebChannel *Self, FUNCTION *Value)
{
   return rtc_set_callback(Self->OnClose, Value);
}

/*********************************************************************************************************************

-FIELD-
OnOpen: A callback that is notified when the channel is ready for use.

The function referenced here is called once when #State changes to `OPEN`.  Messages can be sent with #Send() from
this point.  The C++ prototype is `Function(*WebChannel, APTR Meta)`.

The Tiri prototype is `function(WebChannel)`.

-TAGS-
callback-held
-END-

*********************************************************************************************************************/

static ERR GET_OnOpen(extWebChannel *Self, FUNCTION * &Value)
{
   if (not Self->OnOpen.defined()) return ERR::FieldNotSet;
   Value = &Self->OnOpen;
   return ERR::Okay;
}

static ERR SET_OnOpen(extWebChannel *Self, FUNCTION *Value)
{
   return rtc_set_callback(Self->OnOpen, Value);
}

/*********************************************************************************************************************

-FIELD-
OnError: A callback that is notified when the channel fails.

The function referenced here is called once if the channel fails, and is followed by a call to #OnClose.  #State and
#Error are updated before the callback is made.  The C++ prototype is `Function(*WebChannel, ERR Error, APTR Meta)`.

The Tiri prototype is `function(WebChannel, Error)`.

-TAGS-
callback-held
-END-

*********************************************************************************************************************/

static ERR GET_OnError(extWebChannel *Self, FUNCTION * &Value)
{
   if (not Self->OnError.defined()) return ERR::FieldNotSet;
   Value = &Self->OnError;
   return ERR::Okay;
}

static ERR SET_OnError(extWebChannel *Self, FUNCTION *Value)
{
   return rtc_set_callback(Self->OnError, Value);
}

/*********************************************************************************************************************

-FIELD-
OnMessage: A callback that receives each message from the remote peer.

The function referenced here is called for each complete message that is received while the channel is open.  The
`Type` is `TEXT` or `BINARY`, as declared by the sender.  Empty messages are delivered with a length of zero.  The
C++ prototype is `ERR Function(*WebChannel, const void *Data, INT Length, WCM Type, APTR Meta)`.

The Tiri prototype is `function(WebChannel, Buffer, Type)`, where `Buffer` is an `array<byte>`.

The message data is only valid for the duration of the callback, and must be copied if it is to be retained.  Errors
returned by the callback are logged and otherwise ignored.

-TAGS-
callback-held, volatile-result
-END-

*********************************************************************************************************************/

static ERR GET_OnMessage(extWebChannel *Self, FUNCTION * &Value)
{
   if (not Self->OnMessage.defined()) return ERR::FieldNotSet;
   Value = &Self->OnMessage;
   return ERR::Okay;
}

static ERR SET_OnMessage(extWebChannel *Self, FUNCTION *Value)
{
   return rtc_set_callback(Self->OnMessage, Value);
}

/*********************************************************************************************************************

-FIELD-
OnBufferedAmountLow: A callback that is notified when the send queue falls to #BufferedAmountLowThreshold.

The function referenced here is called when #BufferedAmount falls from above #BufferedAmountLowThreshold to a value at
or below it.  It is typically used to resume sending after the queue has filled.  The callback is only made while the
channel is open.  The C++ prototype is `Function(*WebChannel, APTR Meta)`.

The Tiri prototype is `function(WebChannel)`.

The callback can send further messages.  If these cause the threshold to be crossed again, the next notification is
made in a later cycle rather than from within the current callback.

-TAGS-
callback-held
-END-

*********************************************************************************************************************/

static ERR GET_OnBufferedAmountLow(extWebChannel *Self, FUNCTION * &Value)
{
   if (not Self->OnBufferedAmountLow.defined()) return ERR::FieldNotSet;
   Value = &Self->OnBufferedAmountLow;
   return ERR::Okay;
}

static ERR SET_OnBufferedAmountLow(extWebChannel *Self, FUNCTION *Value)
{
   return rtc_set_callback(Self->OnBufferedAmountLow, Value);
}

/*********************************************************************************************************************

-FIELD-
Error: The error that caused the channel to fail.

Error is set when the channel fails, before #OnError is called.  It remains `ERR::Okay` if the channel opens and
closes normally.

-FIELD-
Label: The name of the channel.

Label is a UTF-8 string that is set when the channel is created.  For a channel created by the remote peer, it is the
label chosen by that peer.  Labels do not need to be unique.

-FIELD-
MaxPacketLifeTime: The maximum lifetime of an outgoing message, in milliseconds.

If MaxPacketLifeTime is set, a message that has not been delivered within this time of being queued is abandoned
rather than retransmitted.  It is -1 if no lifetime limit applies.  It cannot be set in conjunction with
#MaxRetransmits.

-FIELD-
MaxRetransmits: The maximum number of times an outgoing message is retransmitted.

If MaxRetransmits is set, a message that is not delivered after this many retransmissions is abandoned.  A value of
zero means that each message is sent only once.  It is -1 if no retransmission limit applies.  It cannot be set in
conjunction with #MaxPacketLifeTime.

-FIELD-
Negotiated: True if the channel was negotiated by the client rather than announced in-band.

A negotiated channel is created by both peers with the same #StreamID, and the remote peer is not notified of its
creation.  Channels that are announced in-band are delivered to the remote peer through @PeerConnection.OnWebChannel.

-FIELD-
Ordered: True if messages are delivered in the order that they were sent.

If Ordered is false, messages may be delivered to the remote peer in a different order to that in which they were
sent.

-FIELD-
Protocol: The name of the sub-protocol used by the channel.

Protocol is a UTF-8 string that is set when the channel is created.  It is not interpreted by the channel and is
available for the client to identify the format of the messages.  It may be empty.

-FIELD-
State: The current state of the channel.

The channel starts in the `CONNECTING` state and changes to `OPEN` when #OnOpen is called.  It changes to `CLOSING`
when either peer starts to close it, and to `CLOSED` when the closure is complete or the channel fails.

*********************************************************************************************************************/

#include "webchannel_def.c"

static const FieldArray clWebChannelFields[] = {
   { "Label",                      FDF_CPPSTRING|FDF_R },
   { "Protocol",                   FDF_CPPSTRING|FDF_R },
   { "BufferedAmountLowThreshold", FDF_INT64|FDF_RW, nullptr, SET_BufferedAmountLowThreshold },
   { "StreamID",                   FDF_VIRTUAL|FDF_INT|FDF_R, GET_StreamID },
   { "Negotiated",                 FDF_INT|FDF_R },
   { "Ordered",                    FDF_INT|FDF_R },
   { "MaxRetransmits",             FDF_INT|FDF_R },
   { "MaxPacketLifeTime",          FDF_INT|FDF_R },
   { "State",                      FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clWebChannelState },
   { "Error",                      FDF_ERROR|FDF_R },
   { "MaxMessageSize",             FDF_VIRTUAL|FDF_INT64|FDF_R, GET_MaxMessageSize },
   { "BufferedAmount",             FDF_VIRTUAL|FDF_INT64|FDF_R, GET_BufferedAmount },
   { "OnOpen",                     FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_OnOpen, SET_OnOpen },
   { "OnClose",                    FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_OnClose, SET_OnClose },
   { "OnError",                    FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_OnError, SET_OnError },
   { "OnMessage",                  FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_OnMessage, SET_OnMessage },
   { "OnBufferedAmountLow",        FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_OnBufferedAmountLow, SET_OnBufferedAmountLow },
   END_FIELD
};

//********************************************************************************************************************

static ERR add_web_channel_class()
{
   clWebChannel = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::WEBCHANNEL),
      fl::ClassVersion(VER_WEBCHANNEL),
      fl::Name("WebChannel"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clWebChannelActions),
      fl::Methods(clWebChannelMethods),
      fl::Fields(clWebChannelFields),
      fl::Size(sizeof(extWebChannel)),
      fl::Path(MOD_PATH));

   return clWebChannel ? ERR::Okay : ERR::AddClass;
}
