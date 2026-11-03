/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
PeerConnection: Manages a WebRTC data connection with a remote peer.

The PeerConnection class negotiates and maintains a WebRTC transport between the local process and one remote peer.
All traffic is carried by a single bundled data section over ICE, DTLS and SCTP.  Application data is exchanged
through @WebChannel objects, which are created with #CreateWebChannel() or announced by the remote peer through
#OnWebChannel.  Audio and video tracks are not supported.

Signalling is the responsibility of the client.  Session descriptions and candidates must be relayed to the remote
peer through an independent channel such as a WebSocket.  A typical exchange is as follows:

<list type="ordered">
<li>The offering peer calls #CreateOffer(), passes the content of #LocalDescription to #SetLocalDescription(), then
sends the description to the remote peer.</li>
<li>The answering peer passes the offer to #SetRemoteDescription() and calls #CreateAnswer().  The content of
#LocalDescription is committed with #SetLocalDescription() and returned to the offering peer.</li>
<li>The offering peer passes the answer to #SetRemoteDescription().</li>
<li>Each peer forwards the candidates reported by #OnIceCandidate, and passes the candidates that it receives from the
remote peer to #AddIceCandidate().</li>
</list>

As an alternative to forwarding candidates individually, a peer can wait for #GatheringState to reach `COMPLETE` and
then send the full #LocalDescription.  The connection is ready for use when #State reaches `CONNECTED`.  Changes to
#State and #IceState are reported through #Feedback.

Network activity is driven by a framework timer on the thread that created the object.  That thread must process
messages for negotiation to progress and for callbacks to be delivered.

By default, host candidates are advertised under random mDNS names so that local IP addresses are not disclosed, and
each candidate is signalled only after its name has been registered.  If multicast is unavailable, host candidates
are withheld.  Set the `NO_MDNS` flag to signal numeric host addresses instead, or configure a TURN server in
#IceServers to provide a relay route.

If the host's network interfaces change after the connection is established, #RestartRequired is set and
#OnRestartRequired is called.  The client recovers the connection by calling #RestartIce() and repeating the
offer and answer exchange.

-END-

*********************************************************************************************************************/

extPeerConnection::~extPeerConnection()
{
   if (TimerHandle) {
      UpdateTimer(TimerHandle, 0);
      TimerHandle = nullptr;
   }

   for (auto &[handle, child] : Channels) child->Parent = nullptr;
   Connection.reset();

   for (auto field : {&Feedback, &OnIceCandidate, &OnWebChannel, &OnRestartRequired}) {
      if (field->defined()) {
         field->unpin();
         field->clear();
      }
   }
}

//********************************************************************************************************************

static uint64_t peer_now_us()
{
   return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

//********************************************************************************************************************
// Copies the engine's state to the object's fields.  Description text is only copied when its revision changes.

static void peer_refresh(extPeerConnection *Self)
{
   if (not Self->Connection) return;

   auto revision = Self->Connection->description_revision();
   if (revision != Self->DescriptionRevision) {
      Self->LocalDescription = Self->Connection->local_description();
      Self->RemoteDescription = Self->Connection->remote_description();
      Self->DescriptionRevision = revision;
   }

   Self->CertificateFingerprint = Self->Connection->certificate_fingerprint();
   Self->State = RTCS(int(Self->Connection->state()));
   Self->SignalingState = RTCSIG(int(Self->Connection->signaling_state()));
   Self->IceState = RTCICE(int(Self->Connection->ice_state()));
   Self->GatheringState = RTCGATHER(int(Self->Connection->gathering_state()));
   if (Self->Connection->gathering_error() != ERR::Okay) Self->Error = Self->Connection->gathering_error();
}

//********************************************************************************************************************
// Calls the Feedback callback if State or IceState has changed since the last report.  The object and callback are
// pinned for the duration of the call, so that the client can safely free the object from within the callback.

static void peer_report(extPeerConnection *Self)
{
   if (Self->State IS Self->ReportedState and Self->IceState IS Self->ReportedIceState) return;

   Self->ReportedState = Self->State;
   Self->ReportedIceState = Self->IceState;

   if (Self->Feedback.stale()) {
      Self->Feedback.unpin();
      Self->Feedback.clear();
   }

   if (not Self->Feedback.defined()) return;

   auto callback = Self->Feedback;
   callback.pin();
   Self->pin();
   auto lifetime = kt::Defer([&]() {
      callback.unpin();
      rtc_release_pin(Self);
   });

   if (callback.isC()) {
      auto routine = (void (*)(extPeerConnection *, RTCS, RTCICE, APTR))callback.Routine;
      kt::SwitchContext context(callback.Context);
      routine(Self, Self->State, Self->IceState, callback.Meta);
   }
   else if (callback.isScript()) {
      sc::Call(callback, std::to_array<ScriptArg>({
         { "PeerConnection", Self, FD_OBJECTPTR },
         { "State", int(Self->State) },
         { "IceState", int(Self->IceState) }
      }));
   }
}

//********************************************************************************************************************
// Delivers queued local candidates to OnIceCandidate.  Candidates are withheld until their routes are ready, and
// candidates from an earlier ICE generation are discarded.

static void peer_report_candidate(extPeerConnection *Self)
{
   if (Self->Connection and not Self->Connection->candidate_routes_ready()) {
      Self->PendingCandidates.clear();
      return;
   }

   if (Self->PendingCandidates.empty()) return;

   if (Self->OnIceCandidate.stale()) {
      Self->OnIceCandidate.unpin();
      Self->OnIceCandidate.clear();
   }

   if (not Self->OnIceCandidate.defined()) {
      Self->PendingCandidates.clear();
      return;
   }

   auto callback = Self->OnIceCandidate;
   callback.pin();
   Self->pin();
   auto lifetime = kt::Defer([&]() {
      callback.unpin();
      rtc_release_pin(Self);
   });

   while (not Self->PendingCandidates.empty() and not Self->collecting()) {
      if (not Self->Connection or Self->Connection->state() IS rtc::PeerState::CLOSED) {
         Self->PendingCandidates.clear();
         return;
      }

      auto event = std::move(Self->PendingCandidates.front());
      Self->PendingCandidates.pop_front();
      if (event.first != Self->Connection->candidate_generation()) continue;

      auto candidate = std::move(event.second);
      if (callback.isC()) {
         auto routine = (void (*)(extPeerConnection *, CSTRING, APTR))callback.Routine;
         kt::SwitchContext context(callback.Context);
         routine(Self, candidate.c_str(), callback.Meta);
      }
      else if (callback.isScript()) {
         sc::Call(callback, std::to_array<ScriptArg>({
            { "PeerConnection", Self, FD_OBJECTPTR }, { "Candidate", candidate }
         }));
      }
   }
}

//********************************************************************************************************************
// Calls OnRestartRequired once for each restart generation.

static void peer_report_restart(extPeerConnection *Self)
{
   if (not Self->Connection or not Self->Connection->restart_required() or
       Self->Connection->state() IS rtc::PeerState::CLOSED or Self->collecting()) return;

   auto generation = Self->Connection->restart_generation();
   if (Self->ReportedRestartGeneration IS generation) return;

   if (Self->OnRestartRequired.stale()) {
      Self->OnRestartRequired.unpin();
      Self->OnRestartRequired.clear();
   }

   if (not Self->OnRestartRequired.defined()) return;

   Self->ReportedRestartGeneration = generation;
   auto callback = Self->OnRestartRequired;
   callback.pin();
   Self->pin();
   auto lifetime = kt::Defer([&]() {
      callback.unpin();
      rtc_release_pin(Self);
   });

   if (callback.isC()) {
      auto routine = (void (*)(extPeerConnection *, int64_t, APTR))callback.Routine;
      kt::SwitchContext context(callback.Context);
      routine(Self, int64_t(generation), callback.Meta);
   }
   else if (callback.isScript()) {
      sc::Call(callback, std::to_array<ScriptArg>({
         { "PeerConnection", Self, FD_OBJECTPTR }, { "Generation", int64_t(generation) }
      }));
   }
}

//********************************************************************************************************************
// Updates the fields of each WebChannel and delivers channel events.  Remote in-band channels are created here and
// reported through OnWebChannel before any of their own events.  Dispatching prevents re-entry from callbacks, and
// the receive credit of the events is released once they have been delivered.

static void peer_report_channels(extPeerConnection *Self)
{
   if (Self->Dispatching) return;

   Self->Dispatching = true;
   auto dispatch = kt::Defer([&]() { Self->Dispatching = false; });

   for (auto &[handle, child] : Self->Channels) {
      if (auto record = Self->Connection->channel(handle)) {
         child->StreamID = record->id;
         child->BufferedAmount = int64_t(record->buffered);
         child->MaxMessageSize = int64_t(Self->Connection->max_message_size());
         if (record->state IS rtc::SctpChannelState::CLOSING) child->State = WCS::CLOSING;
      }
   }

   auto events = Self->Connection->take_channel_events();
   auto credit = kt::Defer([&]() { Self->Connection->release_channel_events(events); });

   for (auto &event : events) {
      if (Self->collecting()) break;

      auto it = Self->Channels.find(event.handle);
      extWebChannel *child = it IS Self->Channels.end() ? nullptr : it->second;

      if (event.type IS rtc::SctpChannelEventType::REMOTE_OPEN) {
         if (make_web_channel(Self, event.handle, child) != ERR::Okay) {
            Self->Connection->detach_channel(event.handle);
            continue;
         }
      }

      if (not child or child->collecting()) continue;

      child->pin();
      auto lifetime = kt::Defer([&]() { rtc_release_pin(child); });

      if (event.type IS rtc::SctpChannelEventType::REMOTE_OPEN) {
         if (Self->OnWebChannel.stale()) {
            Self->OnWebChannel.unpin();
            Self->OnWebChannel.clear();
         }

         auto callback = Self->OnWebChannel;
         if (callback.defined()) {
            callback.pin();
            auto release = kt::Defer([&]() { callback.unpin(); });
            if (callback.isC()) {
               kt::SwitchContext context(callback.Context);
               auto routine = (void (*)(objPeerConnection *, objWebChannel *, APTR))callback.Routine;
               routine(Self, child, callback.Meta);
            }
            else sc::Call(callback, std::to_array<ScriptArg>({
               { "PeerConnection", Self, FD_OBJECTPTR }, { "WebChannel", child, FD_OBJECTPTR }
            }));
         }
      }
      else if (event.type IS rtc::SctpChannelEventType::OPEN) {
         if (child->State != WCS::CONNECTING or child->OpenReported) continue;
         child->State = WCS::OPEN;
         child->OpenReported = true;
         channel_notify(child, child->OnOpen, event);
      }
      else if (event.type IS rtc::SctpChannelEventType::ERROR) {
         if (child->ErrorReported or child->CloseReported) continue;
         child->ErrorReported = true;
         child->Error = event.error;
         child->State = WCS::CLOSED;
         channel_notify(child, child->OnError, event);
      }
      else if (event.type IS rtc::SctpChannelEventType::CLOSE) {
         if (child->CloseReported) continue;
         child->State = WCS::CLOSED;
         child->CloseReported = true;
         channel_notify(child, child->OnClose, event);
      }
      else if (child->State IS WCS::OPEN) {
         channel_notify(child, event.low ? child->OnBufferedAmountLow : child->OnMessage, event);
      }
   }
}

//********************************************************************************************************************
// Drives the engine and reports any changes.  The timer interval follows the engine's next deadline, within the range
// of 1-50 ms.  The timer terminates once the object is being collected, or the connection has closed and no further
// work is due.

static ERR peer_timer(extPeerConnection *Self, int64_t, int64_t)
{
   if (not Self->Connection or Self->Dispatching or Self->collecting()) return ERR::Okay;

   Self->pin();
   auto lifetime = kt::Defer([&]() { rtc_release_pin(Self); });

   auto error = Self->Connection->state() IS rtc::PeerState::CLOSED ? ERR::Okay :
      Self->Connection->tick(peer_now_us());
   if (error != ERR::Okay) Self->Error = error;

   auto due = Self->Connection->timeout_us(peer_now_us());
   auto interval = due < 0 ? 0.05 : std::clamp(double(due) / 1000000.0, 0.001, 0.05);
   UpdateTimer(Self->TimerHandle, interval);

   peer_refresh(Self);
   peer_report_candidate(Self);

   if (Self->collecting()) {
      Self->TimerHandle = nullptr;
      return ERR::Terminate;
   }

   peer_report(Self);
   if (not Self->collecting()) peer_report_restart(Self);
   if (not Self->collecting()) peer_report_channels(Self);

   if (Self->collecting() or (Self->State IS RTCS::CLOSED and
       Self->Connection->timeout_us(peer_now_us()) < 0)) {
      Self->TimerHandle = nullptr;
      return ERR::Terminate;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
AddIceCandidate: Adds a remote ICE candidate, or signals the end of remote candidates.

Call AddIceCandidate() for each candidate that the remote peer signals after its description has been applied with
#SetRemoteDescription().  Only UDP candidates are used.  Other well-formed candidates, such as TCP candidates, are
accepted and ignored.

Candidates that use an mDNS `.local` host name are resolved asynchronously on the local multicast interfaces.  Other
DNS host names are not resolved.  Each resolution is limited to three seconds and is cancelled by #RestartIce() or
#Close().

Passing an empty string or `end-of-candidates` indicates that the remote peer has no further candidates.  Any further
candidates for the current ICE generation are then rejected.

-INPUT-
strview Candidate: An SDP candidate attribute without the `a=` prefix, e.g. `candidate:1 1 UDP ...`.

-ERRORS-
Okay
NullArgs
InvalidState: The remote description has not been set, or the end of candidates has already been signalled.
InvalidData: The candidate is malformed, or its ufrag does not match the remote description.
BufferOverflow: The limit for pending remote candidates has been reached.

-TAGS-
non-blocking, mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_AddIceCandidate(extPeerConnection *Self, struct rtc::AddIceCandidate *Args)
{
   if (not Args) return ERR::NullArgs;
   return Self->Connection->add_ice_candidate(Args->Candidate);
}

/*********************************************************************************************************************

-METHOD-
Close: Closes the connection and releases its network resources.

Close() terminates the connection immediately.  All sockets, mDNS registrations and TURN allocations are released,
open @WebChannel objects are closed and #State changes to `CLOSED`.  A closed connection cannot be reopened.  The
object remains valid so that #GetStats() can still report the final counters.

-ERRORS-
Okay

-TAGS-
non-blocking, mutates-object, callback-inlines
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_Close(extPeerConnection *Self)
{
   Self->Connection->close();

   for (auto &[handle, child] : Self->Channels) {
      if (child->State != WCS::CLOSED) child->State = WCS::CLOSING;
   }

   peer_refresh(Self);
   peer_report(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR PEERCONNECTION_Init(extPeerConnection *Self)
{
   rtc::PeerCallbacks callbacks;
   callbacks.candidate = [Self](std::string_view Value) {
      Self->PendingCandidates.emplace_back(Self->Connection->candidate_generation(), Value);
   };

   // A negative interval only brings the next tick forward; peer_timer() restores the deadline-derived interval.

   callbacks.wake = [Self]() { if (Self->TimerHandle) UpdateTimer(Self->TimerHandle, -0.001); };

   Self->Connection = std::make_unique<rtc::PeerConnection>(std::move(callbacks));
   Self->Connection->enable_channels();

   auto error = Self->Connection->set_ice_servers(Self->ServerConfiguration);
   Self->ServerConfiguration.clear();
   if (error != ERR::Okay) return error;

   error = Self->Connection->set_policy(uint32_t(Self->Flags));
   if (error != ERR::Okay) return error;

   error = Self->Connection->set_receive_limit(Self->ReceiveLimit);
   if (error != ERR::Okay) return error;

   kt::SwitchContext context(Self);
   return SubscribeTimer(0.05, C_FUNCTION(peer_timer), &Self->TimerHandle);
}

/*********************************************************************************************************************

-METHOD-
CreateAnswer: Generates an SDP answer to the remote offer and starts candidate gathering.

CreateAnswer() responds to an offer that has been applied with #SetRemoteDescription().  The answer is written to
#LocalDescription and must be committed by passing it to #SetLocalDescription() before it is sent to the remote
peer.

Candidate gathering begins when the first answer is created, so the STUN or TURN configuration in #IceServers or
#StunServer must be set beforehand.  The method returns without waiting for gathering to finish, so the initial
answer will not include candidates that are gathered later, including mDNS host candidates.  These are reported
through #OnIceCandidate as they become available.  #GatheringState changes to `COMPLETE` once every host, STUN and
TURN job has finished or failed.

-ERRORS-
Okay
InvalidState: The connection is closed, there is no remote offer, or an answer has already been created for it.
InvalidData: #StunServer is not a valid address.
NoSupport: #StunServer uses an unsupported scheme or address family.
CreateObject
Failed: No usable network interface is available, or the DTLS certificate could not be generated.

-TAGS-
non-blocking, mutates-object, callback-inlines
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_CreateAnswer(extPeerConnection *Self)
{
   if (not Self->Connection->has_ice_servers() and not Self->StunServer.empty() and
       Self->Connection->state() IS rtc::PeerState::NEW) {
      auto configured = Self->Connection->set_stun_server(Self->StunServer);
      if (configured != ERR::Okay) return configured;
   }

   auto error = Self->Connection->create_answer();
   if (error != ERR::Okay) return error;

   peer_refresh(Self);
   peer_report_candidate(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
CreateOffer: Generates an SDP offer and starts candidate gathering.

CreateOffer() begins negotiation from the offering side.  The offer is written to #LocalDescription and must be
committed by passing it to #SetLocalDescription() before it is sent to the remote peer.  An offer can only be created
once for each connection.  Use #RestartIce() to generate subsequent offers.

The STUN or TURN configuration in #IceServers or #StunServer must be set before calling this method.  #StunServer
is used only if #IceServers is empty.  It accepts a `stun:` URI, for which the port defaults to 3478, or a host and
an explicit port, e.g. `203.0.113.5:3478`.  A host name is resolved asynchronously.

The method returns without waiting for gathering to finish, so the initial offer will not include candidates that
are gathered later, including mDNS host candidates.  These are reported through #OnIceCandidate as they become
available.  #GatheringState changes to `COMPLETE` once every host, STUN and TURN job has finished or failed.

-ERRORS-
Okay
InvalidState: The connection is closed, negotiation is already in progress, or an offer has already been created.
InvalidData: #StunServer is not a valid address.
NoSupport: #StunServer uses an unsupported scheme or address family.
CreateObject
Failed: No usable network interface is available, or the DTLS certificate could not be generated.

-TAGS-
non-blocking, mutates-object, callback-inlines
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_CreateOffer(extPeerConnection *Self)
{
   if (not Self->Connection->has_ice_servers() and not Self->StunServer.empty()) {
      auto configured = Self->Connection->set_stun_server(Self->StunServer);
      if (configured != ERR::Okay) return configured;
   }

   auto error = Self->Connection->create_offer();
   if (error != ERR::Okay) return error;

   peer_refresh(Self);
   peer_report_candidate(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
CreateWebChannel: Creates a new WebChannel for exchanging messages with the remote peer.

CreateWebChannel() creates a @WebChannel that is owned by the PeerConnection.  It can be called before negotiation,
in which case the channel opens once the connection is established, or on an established connection without
renegotiating the session descriptions.  The channel's @WebChannel.OnOpen callback is called when it is ready for
use.  The options supplied here are copied to the channel's read-only fields.

By default the remote peer is notified of the new channel in-band and receives it through #OnWebChannel.  If
`Negotiated` is true, no notification is sent and both peers must create the channel with the same `ID`.

When `ID` is -1, a stream ID is assigned automatically after the DTLS handshake, with even IDs used by the DTLS
client and odd IDs by the server.  An explicit `ID` is reserved immediately.  If it later conflicts with the
negotiated DTLS role or stream count, the channel reports @WebChannel.OnError followed by @WebChannel.OnClose.

Messages are delivered reliably unless `MaxRetransmits` or `MaxPacketLifeTime` is set.  Only one of these options can
be used.

Freeing the PeerConnection destroys all of its channels.  Freeing a channel explicitly resets its stream and
suppresses any further callbacks.  Releasing a Tiri reference to a channel does not free it.  A maximum of 256
channels can be open at any time.

-INPUT-
strview Label: UTF-8 label for the channel, at most 65535 bytes.  Can be empty.
strview Protocol: UTF-8 name of the sub-protocol, at most 65535 bytes.  Can be empty.
int Ordered: Set to true for ordered delivery, or false to permit out-of-order delivery.
int Negotiated: Set to true if the channel is negotiated by the client rather than announced in-band.
int ID: The stream ID from 0 to 65534, or -1 for automatic assignment.  Required if `Negotiated` is true.
int MaxRetransmits: Maximum retransmissions from 0 to 65535, or -1 for no limit.  Zero sends each message once.
int MaxPacketLifeTime: Maximum lifetime of a queued message from 0 to 65535 milliseconds, or -1 for no limit.
&obj(WebChannel) Channel: The new channel is returned here.  The PeerConnection retains ownership.

-ERRORS-
Okay
NullArgs
InvalidState: The connection is closed or has failed.
InvalidValue: A boolean option is not 0 or 1, both reliability options are set, or `ID` is unavailable.
InvalidData: `Label` or `Protocol` is not valid UTF-8.
OutOfRange: A numeric option is outside of its permitted range.
BufferOverflow: The channel limit has been reached.
NoSupport

-TAGS-
non-blocking, mutates-object, copies-input, object-owns-result
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_CreateWebChannel(extPeerConnection *Self, struct rtc::CreateWebChannel *Args)
{
   if (not Args) return ERR::NullArgs;

   Args->Channel = nullptr;
   if (not Self->Connection or Self->collecting()) return ERR::InvalidState;

   if (Args->ID < -1 or Args->ID > 65534 or Args->MaxRetransmits < -1 or Args->MaxRetransmits > 65535 or
       Args->MaxPacketLifeTime < -1 or Args->MaxPacketLifeTime > 65535) return ERR::OutOfRange;

   if ((Args->Ordered != 0 and Args->Ordered != 1) or (Args->Negotiated != 0 and Args->Negotiated != 1)) {
      return ERR::InvalidValue;
   }

   rtc::SctpChannelOptions options;
   options.label = Args->Label;
   options.protocol = Args->Protocol;
   options.ordered = Args->Ordered;
   options.negotiated = Args->Negotiated;
   if (Args->ID >= 0) options.id = uint16_t(Args->ID);
   if (Args->MaxRetransmits >= 0) options.reliability.max_retransmits = uint32_t(Args->MaxRetransmits);
   if (Args->MaxPacketLifeTime >= 0) options.reliability.max_lifetime_ms = uint32_t(Args->MaxPacketLifeTime);

   uint64_t handle;
   auto error = Self->Connection->create_channel(options, handle);
   if (error != ERR::Okay) return error;

   extWebChannel *channel;
   error = make_web_channel(Self, handle, channel);
   if (error != ERR::Okay) {
      Self->Connection->detach_channel(handle);
      return error;
   }

   Args->Channel = channel;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetStats: Returns a snapshot of the connection's transport statistics in JSON format.

GetStats() returns a UTF-8 JSON document describing the current state of the transport.  The document is a copy and
is not affected by subsequent activity on the connection.  The method can be called at any time after
initialisation, including after #Close().

The document contains a `version` number, which is currently `1`.  Timestamps and durations (`timestampUs`,
`iceRttUs` and the SCTP `rtoUs`) are expressed in microseconds of monotonic time, and sizes (`cwndBytes` and
`pathMtuBytes`) are expressed in bytes.  The timestamp, ICE generation and cumulative counters are encoded as decimal
strings so that 64-bit values are preserved by JSON parsers.  The `selectedRoute`, `route`, `iceRttUs` and `sctp`
values are `null` when unavailable, for instance when no route has been selected or the connection is closed.

The `connection` counters accumulate for the lifetime of the object and are preserved across #RestartIce() and
#Close().  They count every datagram accepted by the transport, including ICE checks, consent freshness, DTLS
records, application data and retransmissions.  Rejected input and failed sends are excluded.  TURN byte counts
include TURN framing and allocation maintenance, but exclude IP, TCP and TLS headers.

When a route is selected, it reports the totals for its local and remote candidate pair, and the totals for its TURN
allocation if one is used.  An allocation may be shared with other candidate pairs.  Route identifiers are opaque and
change when ICE is restarted.

Endpoint addresses are subject to the same privacy rules as signalling, so an address that would not be signalled
under the current #Flags is reported as `null`.  Server credentials are never included.

-INPUT-
!str Stats: The JSON document is returned here.  In C++ it must be released with FreeResource().

-ERRORS-
Okay
NullArgs
NotInitialised
AllocMemory
InUse

-TAGS-
non-blocking, caller-owns-result
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_GetStats(extPeerConnection *Self, struct rtc::GetStats *Args)
{
   if (not Args) return ERR::NullArgs;

   Args->Stats = nullptr;
   if (not Self->Connection) return ERR::NotInitialised;

   auto text = Self->Connection->stats();
   if (AllocResource(text.size() + 1, MEM::NO_CLEAR, (APTR *)&Args->Stats, nullptr) != ERR::Okay) {
      return ERR::AllocMemory;
   }

   // Results belong to the caller, rather than the peer action's current allocation context.

   auto error = TrackResource(GetMemoryID(Args->Stats), nullptr, 0, nullptr);
   if (error != ERR::Okay) {
      FreeResource(Args->Stats);
      Args->Stats = nullptr;
      return error;
   }

   std::memcpy(Args->Stats, text.c_str(), text.size() + 1);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RestartIce: Restarts ICE negotiation on an established connection and generates a new offer.

RestartIce() discards the current network routes and generates a new offer with fresh ICE credentials.  It is
typically called in response to #OnRestartRequired.  The DTLS session, certificate, SCTP association and open
@WebChannel objects are retained, so data channels survive the restart.

The new offer is written to #LocalDescription and the exchange proceeds as for #CreateOffer().  The offer is
committed with #SetLocalDescription() and sent to the remote peer, which applies it with #SetRemoteDescription()
and returns an answer from #CreateAnswer().  All local candidates are gathered again, so new candidates must be
forwarded from #OnIceCandidate, or the exchange can be delayed until #GatheringState reaches `COMPLETE`.  #State
remains `CONNECTING` until ICE selects a new route.

The connection must be established and signalling must be stable when this method is called.

-ERRORS-
Okay
InvalidState: The connection has not been established, or an offer or answer exchange is in progress.
InvalidData
NoSupport
Failed: New credentials could not be generated, or no usable network interface is available.

-TAGS-
non-blocking, mutates-object, callback-inlines
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_RestartIce(extPeerConnection *Self)
{
   auto error = Self->Connection->restart_ice();
   peer_refresh(Self);
   peer_report(Self);
   return error;
}

/*********************************************************************************************************************

-METHOD-
SetLocalDescription: Commits the offer or answer in #LocalDescription.

SetLocalDescription() commits the description generated by #CreateOffer(), #CreateAnswer() or #RestartIce().  The
`Description` must match the content of #LocalDescription exactly.  Modifying the SDP before it is committed is not
supported.

Committing an offer changes #SignalingState to `HAVE_LOCAL_OFFER`.  Committing an answer returns #SignalingState to
`STABLE` and starts connectivity checks once candidate gathering is complete.  After the description is committed,
#LocalDescription is updated with each candidate as it is gathered.

-INPUT-
strview Description: The SDP text read from #LocalDescription.

-ERRORS-
Okay
NullArgs
InvalidState: The description does not match #LocalDescription, is already committed, or the connection is closed.
InvalidData: The remote description contains invalid ICE credentials.
Timeout: No local host or relay candidate is available for connectivity checks.

-TAGS-
non-blocking, mutates-object, callback-inlines
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_SetLocalDescription(extPeerConnection *Self, struct rtc::SetLocalDescription *Args)
{
   if (not Args) return ERR::NullArgs;

   auto error = Self->Connection->set_local_description(Args->Description);
   peer_refresh(Self);
   peer_report(Self);
   return error;
}

/*********************************************************************************************************************

-METHOD-
SetRemoteDescription: Applies an offer or answer received from the remote peer.

SetRemoteDescription() applies a session description that has been received from the remote peer.  If the
connection has sent an offer, the description must be a compatible answer.  Otherwise it must be an offer, which
changes #SignalingState to `HAVE_REMOTE_OFFER` and is followed by a call to #CreateAnswer().

An offer that replaces the ICE credentials of an established connection is treated as an ICE restart initiated by the
remote peer.  The local network routes are gathered again and the existing DTLS session is retained.  Call
#CreateAnswer() to respond to it.

The description can include at most 128 distinct candidates.  Further candidates can be supplied through
#AddIceCandidate().

-INPUT-
strview Description: The SDP text received from the remote peer.

-ERRORS-
Okay
NullArgs
InvalidState: The connection is closed, or the description is not expected in the current #SignalingState.
InvalidData: The description is malformed, is incompatible with the local offer, or has too many candidates.
NoSupport
Failed
Timeout: No local host or relay candidate is available for connectivity checks.

-TAGS-
non-blocking, mutates-object, copies-input, callback-inlines
-END-

*********************************************************************************************************************/

static ERR PEERCONNECTION_SetRemoteDescription(extPeerConnection *Self, struct rtc::SetRemoteDescription *Args)
{
   if (not Args) return ERR::NullArgs;

   auto error = Self->Connection->set_remote_description(Args->Description);
   peer_refresh(Self);
   peer_report(Self);
   return error;
}

/*********************************************************************************************************************

-FIELD-
Feedback: A callback that is notified of changes to #State and #IceState.

The function referenced here is called whenever #State or #IceState changes.  The C++ prototype is
`Function(*PeerConnection, RTCS State, RTCICE IceState, APTR Meta)`.

The Tiri prototype is `function(PeerConnection, State, IceState)`.

Set this field before calling #CreateOffer() or #CreateAnswer() to observe every state change.  The PeerConnection
can be safely freed from within the callback.

-TAGS-
callback-held

*********************************************************************************************************************/

static ERR GET_Feedback(extPeerConnection *Self, FUNCTION * &Value)
{
   if (not Self->Feedback.defined()) return ERR::FieldNotSet;
   Value = &Self->Feedback;
   return ERR::Okay;
}

static ERR SET_Feedback(extPeerConnection *Self, FUNCTION *Value)
{
   if (Self->Feedback.defined()) {
      Self->Feedback.unpin();
      Self->Feedback.clear();
   }

   if (Value) {
      Self->Feedback = *Value;
      Self->Feedback.pin();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags that control how candidates are gathered and signalled.

-FIELD-
IceServers: A JSON array that configures the STUN and TURN servers.

IceServers defines the STUN and TURN servers that are used for candidate gathering.  It can only be set prior to
initialisation and cannot be read.  The value is a JSON array of up to eight server objects, with a total length of
no more than 16384 bytes.  Each object contains a `url` string, and TURN servers additionally require non-empty
`username` and `credential` strings.  For example:

<pre>[
   { "url": "stun:stun.example.com:3478" },
   { "url": "turn:turn.example.com?transport=udp", "username": "user", "credential": "secret" }
]</pre>

The following URL formats are supported.  IPv6 addresses must be enclosed in brackets.  The default port is 3478 for
`stun:` and `turn:`, and 5349 for `turns:`.

<list type="bullet">
<li>`stun:host:port`</li>
<li>`turn:host:port?transport=udp`</li>
<li>`turn:host:port?transport=tcp`</li>
<li>`turns:host:port?transport=tcp`</li>
</list>

STUN servers do not accept credentials.  Unknown or duplicate keys, values of the wrong type and invalid URLs are
rejected.

If IceServers is not empty, #StunServer is ignored.  Credentials are not included in session descriptions,
candidates or statistics.  Each server is contacted independently during gathering, so the failure of one server
does not affect the candidates obtained from the others.

*********************************************************************************************************************/

static ERR SET_IceServers(extPeerConnection *Self, const std::string_view &Value)
{
   if (Self->Connection) return ERR::InvalidState;

   // The configuration is validated here and parsed again by the engine during initialisation.

   std::vector<rtc::IceServer> parsed;
   auto error = rtc::parse_ice_servers(Value, parsed);
   if (error IS ERR::Okay) Self->ServerConfiguration.assign(Value);
   return error;
}

/*********************************************************************************************************************

-FIELD-
OnIceCandidate: A callback that receives each local ICE candidate as it is gathered.

The function referenced here is called for each local candidate that is gathered.  The candidate is provided as an
SDP candidate attribute without the `a=` prefix, and should be forwarded to the remote peer for use with
#AddIceCandidate().  The C++ prototype is `Function(*PeerConnection, CSTRING Candidate, APTR Meta)`.

The Tiri prototype is `function(PeerConnection, Candidate)`.

Candidates are reported asynchronously as gathering progresses.  Host candidates are reported once their mDNS names
have been registered, unless the `NO_MDNS` flag is set.  Server-reflexive and relay candidates follow as responses
are received from the servers in #IceServers.  Set this field before calling #CreateOffer() or #CreateAnswer() to
receive every candidate.  Candidates from an earlier ICE generation are discarded after #RestartIce().

-TAGS-
callback-held
-END-

*********************************************************************************************************************/

static ERR GET_OnIceCandidate(extPeerConnection *Self, FUNCTION * &Value)
{
   if (not Self->OnIceCandidate.defined()) return ERR::FieldNotSet;
   Value = &Self->OnIceCandidate;
   return ERR::Okay;
}

static ERR SET_OnIceCandidate(extPeerConnection *Self, FUNCTION *Value)
{
   if (Self->OnIceCandidate.defined()) {
      Self->OnIceCandidate.unpin();
      Self->OnIceCandidate.clear();
   }

   if (Value) {
      Self->OnIceCandidate = *Value;
      Self->OnIceCandidate.pin();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OnRestartRequired: A callback that is notified when the connection requires an ICE restart.

The function referenced here is called when a change to the host's network interfaces requires the client to restart
ICE.  The client should respond by calling #RestartIce() and completing the offer and answer exchange.  No offer is
created automatically.  The C++ prototype is `Function(*PeerConnection, INT64 Generation, APTR Meta)`.

The Tiri prototype is `function(PeerConnection, Generation)`.

`Generation` identifies the local candidate generation that has been retired.  #RestartRequired is set before the
callback is made.  Multiple interface changes in quick succession are combined into a single notification, and the
callback is called once per restart request.  If the callback is set while a request is pending, it receives that
request.  The request is cancelled by #Close().

If signalling is not stable when the notification is received, the client should defer #RestartIce() until the
current exchange has completed.  Resolution of simultaneous offers from both peers is the client's responsibility.

-TAGS-
callback-held

*********************************************************************************************************************/

static ERR GET_OnRestartRequired(extPeerConnection *Self, FUNCTION * &Value)
{
   if (not Self->OnRestartRequired.defined()) return ERR::FieldNotSet;
   Value = &Self->OnRestartRequired;
   return ERR::Okay;
}

static ERR SET_OnRestartRequired(extPeerConnection *Self, FUNCTION *Value)
{
   if (Self->OnRestartRequired.defined()) {
      Self->OnRestartRequired.unpin();
      Self->OnRestartRequired.clear();
   }

   if (Value) {
      Self->OnRestartRequired = *Value;
      Self->OnRestartRequired.pin();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OnWebChannel: A callback that receives channels created by the remote peer.

The function referenced here is called when the remote peer creates a @WebChannel and announces it in-band.  The
C++ prototype is `Function(*PeerConnection, *WebChannel Channel, APTR Meta)`.

The Tiri prototype is `function(PeerConnection, WebChannel)`.

The callback is made before any other event for the new channel, so the channel's @WebChannel.OnOpen and
@WebChannel.OnMessage callbacks should be set here.  The channel is owned by the PeerConnection.  Channels created
with the `Negotiated` option of #CreateWebChannel() are not announced and do not trigger this callback.

-TAGS-
callback-held

*********************************************************************************************************************/

static ERR GET_OnWebChannel(extPeerConnection *Self, FUNCTION * &Value)
{
   if (not Self->OnWebChannel.defined()) return ERR::FieldNotSet;
   Value = &Self->OnWebChannel;
   return ERR::Okay;
}

static ERR SET_OnWebChannel(extPeerConnection *Self, FUNCTION *Value)
{
   if (Self->OnWebChannel.defined()) {
      Self->OnWebChannel.unpin();
      Self->OnWebChannel.clear();
   }

   if (Value) {
      Self->OnWebChannel = *Value;
      Self->OnWebChannel.pin();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ReceiveLimit: The largest message, in bytes, that the connection will accept from the remote peer.

ReceiveLimit is advertised to the remote peer as the maximum message size in the local session description.  The
default is 262144 bytes, and the value can be set from 1 to 16777216 prior to initialisation.  The limit for
outgoing messages is set independently by the remote peer and is reported by @WebChannel.MaxMessageSize.

-FIELD-
RestartRequired: Non-zero if the connection requires an ICE restart to recover from a network change.

RestartRequired is set when a change to the host's network interfaces invalidates the connection's routes.  The
client is notified through #OnRestartRequired, and should respond by calling #RestartIce() once signalling is
stable.  An ICE restart initiated by the remote peer also satisfies the request.

Interface changes are detected from operating system notifications where available, and by checking the interfaces
every two seconds.  A burst of changes is combined into a single request once the interfaces have been
stable for 250 milliseconds, with a maximum delay of one second.  All local routes are discarded, but the DTLS
session, SCTP association, open channels and queued reliable messages are retained.

The field is cleared when a newer ICE generation selects a working route.  If a new route is not established within
30 seconds of the request, #State changes to `FAILED`.
-END-

*********************************************************************************************************************/

static ERR GET_RestartRequired(extPeerConnection *Self, int *Value)
{
   *Value = Self->Connection and Self->Connection->restart_required();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CertificateFingerprint: The SHA-256 fingerprint of the local DTLS certificate.

The certificate is generated when the first offer or answer is created, and its fingerprint is advertised in the
local session description.  The fingerprint is formatted as 32 colon-separated pairs of upper-case hexadecimal digits.
The field is empty until #CreateOffer() or #CreateAnswer() has been called.

-FIELD-
ClientData: A custom pointer available for userspace.

-FIELD-
Error: The most recent error reported by the transport.

Error records the most recent failure that was detected by the transport while it was running in the background, such
as a failed TURN allocation or a timeout during network recovery.  It is not cleared when the connection recovers.
Errors returned by methods are not recorded here.  Check this field when #State changes to `FAILED`.

-FIELD-
GatheringState: The state of local candidate gathering.

GatheringState changes to `GATHERING` when the first offer or answer is created, and to `COMPLETE` when every host,
STUN and TURN job has finished or failed.  Gathering is limited to 30 seconds.  It starts again after an ICE restart.

-FIELD-
IceState: The state of the ICE agent.

IceState tracks the progress of the ICE connectivity checks.  Changes are reported through #Feedback.  The overall
state of the connection is reported by #State.

-FIELD-
LocalDescription: The local session description.

LocalDescription holds the SDP generated by the last call to #CreateOffer(), #CreateAnswer() or #RestartIce().  It
must be passed unmodified to #SetLocalDescription() before it is sent to the remote peer.  Once committed, the
description is updated with each local candidate as it is gathered, and includes all candidates when #GatheringState
is `COMPLETE`.

-FIELD-
RemoteDescription: The session description received from the remote peer.

RemoteDescription holds the SDP text most recently applied with #SetRemoteDescription().  Candidates supplied through
#AddIceCandidate() are not added to it.

-FIELD-
SignalingState: The state of the offer and answer exchange.

SignalingState is `HAVE_LOCAL_OFFER` after a local offer is committed and `HAVE_REMOTE_OFFER` after a remote offer is
applied.  It returns to `STABLE` when the exchange is complete, and changes to `CLOSED` when the connection is closed.

-FIELD-
State: The overall state of the connection.

State combines the ICE, DTLS and SCTP layers into a single value.  The connection is `CONNECTING` while connectivity
checks or the DTLS handshake are in progress, and `CONNECTED` once ICE has selected a route and the DTLS handshake has
succeeded.  `DISCONNECTED` indicates that the selected route has stopped responding, and `FAILED` indicates that the
connection cannot recover.  The reason for a failure is recorded in #Error.  Changes are reported through #Feedback.

-FIELD-
StunServer: The address of a STUN server for discovering the public address of the host.

StunServer provides a simple alternative to #IceServers when only one STUN server is required.  It is ignored if
#IceServers is not empty.  The value can be a `stun:` URI, for which the port defaults to 3478, or a host and an
explicit port, e.g. `203.0.113.5:3478`.  Host names are resolved asynchronously during gathering.  `stuns:` URIs are
not supported.

The value is applied by the first call to #CreateOffer() or #CreateAnswer(), and later changes have no effect.

*********************************************************************************************************************/

#include "peerconnection_def.c"

static const FieldArray clPeerConnectionFields[] = {
   { "LocalDescription",       FDF_CPPSTRING|FDF_R },
   { "RemoteDescription",      FDF_CPPSTRING|FDF_R },
   { "CertificateFingerprint", FDF_CPPSTRING|FDF_R },
   { "StunServer",             FDF_CPPSTRING|FDF_RW },
   { "IceServers",             FDF_VIRTUAL|FDF_CPPSTRING|FDF_I, nullptr, SET_IceServers },
   { "ClientData",             FDF_POINTER|FDF_RW },
   { "State",                  FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clPeerConnectionState },
   { "SignalingState",         FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clPeerConnectionSignalingState },
   { "IceState",               FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clPeerConnectionIceState },
   { "GatheringState",         FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clPeerConnectionGatheringState },
   { "Error",                  FDF_ERROR|FDF_R },
   { "ReceiveLimit",           FDF_INT|FDF_RI },
   { "Flags",                  FDF_INT|FDF_FLAGS|FDF_RI, nullptr, nullptr, &clPeerConnectionFlags },
   // Virtual fields
   { "Feedback",               FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_Feedback, SET_Feedback },
   { "OnIceCandidate",         FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_OnIceCandidate, SET_OnIceCandidate },
   { "RestartRequired",        FDF_VIRTUAL|FDF_INT|FDF_R, GET_RestartRequired },
   { "OnRestartRequired",      FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_OnRestartRequired, SET_OnRestartRequired },
   { "OnWebChannel",           FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_OnWebChannel, SET_OnWebChannel },
   END_FIELD
};

//********************************************************************************************************************

static ERR add_peer_connection_class()
{
   clPeerConnection = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::PEERCONNECTION),
      fl::ClassVersion(VER_PEERCONNECTION),
      fl::Name("PeerConnection"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clPeerConnectionActions),
      fl::Methods(clPeerConnectionMethods),
      fl::Fields(clPeerConnectionFields),
      fl::Size(sizeof(extPeerConnection)),
      fl::Path(MOD_PATH));

   return clPeerConnection ? ERR::Okay : ERR::AddClass;
}
