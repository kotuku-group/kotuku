# Plan: Native WebRTC Support for Kōtuku

Status: Draft for review
Created: 2026-10-07
Owner: Unassigned

## 1. Goal

Add a `webrtc` module that lets Kōtuku programs act as a WebRTC peer, interoperating with browsers and other
standards-compliant endpoints, without linking Google's `libwebrtc`.  "Native" here means the protocol stack is
implemented in-house on top of the existing Network, Crypto and Audio modules, in the same style as the WebSocket
module (RFC-driven, small protocol units, unit-tested in isolation, exercised end to end through Flute).

The deliverable is a Tiri-facing and C++-facing API shaped after the W3C `RTCPeerConnection` model, so that
browser-side signalling code and server-side Kōtuku code share the same vocabulary:

```lua
   include 'webrtc'

local pc = obj.new('peerconnection', {
   iceServers = 'stun:stun.l.google.com:19302',
   onIceCandidate = function(PC, Candidate) signalling.send({ candidate = Candidate }) end,
   onDataChannel  = function(PC, Channel)
      Channel.onMessage = function(Ch, Data, Binary) print('peer says: ' .. Data) end
   end,
   feedback = function(PC, State) print('connection state: ' .. State) end
})

local err, offer = pc.mtCreateOffer()
pc.mtSetLocalDescription('offer', offer)
signalling.send({ sdp = offer })
```

## 2. Non-Goals (first release)

- Simulcast, SVC, FEC/RED, and bandwidth estimation beyond a conservative fixed-rate sender.
- Built-in video codecs.  Video is supported at the transport level (RTP packetisation/depacketisation of
  encoded frames) with the application supplying and consuming encoded frames; a software codec can follow later.
- A hosted signalling service.  Signalling is application-defined; the plan ships a reference signalling server
  written in Tiri using the WebSocket module.
- Perfect negotiation helpers, insertable streams, and WebRTC statistics beyond a basic `GetStats()` snapshot.

## 3. What the Repository Already Provides

| Need | Status | Notes |
| --- | --- | --- |
| Non-blocking UDP sockets | Available | `NetSocket` with `NSF::UDP`, `SendTo`/`RecvFrom`, IPv4/IPv6, multicast.  Linux uses `RegisterFD()`, Windows uses IOCP with datagram queues (`src/network/win32/iocp.cpp`). |
| TCP + TLS | Available | Needed for TURN-over-TCP/TLS and for the WebSocket signalling example. |
| DNS | Available | `NetLookup` for resolving STUN/TURN hostnames. |
| TLS backend | Partial | OpenSSL on Linux (`src/network/openssl.cpp`), Schannel on Windows (`src/network/win32/ssl_*.cpp`).  Both are wired to a socket handle via `BIO_new_socket()` / direct socket I/O.  WebRTC needs DTLS over a *shared, demultiplexed* UDP socket, so a memory-fed transport is required (see §5.3). |
| Hashes and HMAC | Available | Crypto module: MD5, SHA-1, SHA-256, SHA-512, HMAC, `RandomBytes`, base64.  BearSSL subset on Linux, CNG on Windows. |
| Symmetric ciphers | Missing | No AES.  SRTP needs AES-128-CTR + HMAC-SHA1-80 and ideally AES-GCM.  BearSSL provides both (`aes_ct64`, `ghash`), so this is a matter of extending the fetched source list in `src/crypto/CMakeLists.txt` and adding a CNG implementation on Windows. |
| Asymmetric keys / certificates | Missing | Need a self-signed ECDSA P-256 certificate per PeerConnection and its SHA-256 fingerprint.  OpenSSL covers Linux; Windows needs `CertCreateSelfSignCertificate` or BCrypt ECDSA plus a small DER writer. |
| CRC32 | Available | zlib is already a project dependency; STUN FINGERPRINT uses CRC32. |
| Timers | Available | `SubscribeTimer()` for ICE retransmits, SCTP RTO, RTCP intervals, consent freshness. |
| Audio playback | Available | `Audio.AddStream()` can be fed decoded PCM for remote audio tracks. |
| Audio capture | Missing | `Audio.InputRate` exists but there is no capture API in either the ALSA or WASAPI backends.  Required for sending a local microphone track (Phase 4 prerequisite). |
| Protocol-module precedent | Available | WebSocket module: pure protocol files (`ws_frame.cpp`, `ws_handshake.cpp`), `UNIT_TESTS`-gated `unit_protocol.cpp`, Flute loopback and TLS tests, scheduled Autobahn conformance run. |
| Browser for interop tests | Available | Chromium is pre-installed in cloud sessions with Playwright configured, which makes automated browser interop testing feasible. |

## 4. Standards Checklist

The minimum set a browser requires to connect (RFC 8825/8826/8827/8834 "WebRTC overview"):

| Area | RFCs | Required for |
| --- | --- | --- |
| SDP + JSEP offer/answer, BUNDLE, rtcp-mux, trickle ICE | 8866, 8829, 8843, 5761, 8838, 8839, 8840, 8841 | Everything |
| STUN | 8489 | ICE |
| ICE (full agent, not ICE-lite, so Kōtuku can be either side) | 8445, 7675 (consent freshness), 8421 (dual-stack guidelines) | Everything |
| TURN (UDP, TCP, TLS transports) | 8656 | NAT traversal |
| mDNS ICE candidates | 8828 + draft-ietf-mmusic-mdns-ice-candidates | Connecting to browsers on LANs |
| Demultiplexing on a single socket | 7983 (STUN 0–3, DTLS 20–63, RTP/RTCP 128–191) | Everything |
| DTLS 1.2 with fingerprint verification | 6347, 8827, 8122 | Everything |
| DTLS-SRTP key derivation | 5764 (`use_srtp` extension), 5705 (keying material export) | Media only |
| SCTP over DTLS, data channels, DCEP | 8261, 8831, 8832, 4960, 3758 (PR-SCTP), 6525 (stream reset), 7496 (I-DATA, optional) | Data channels |
| RTP/RTCP, SRTP | 3550, 3551, 3711, 7714 (AES-GCM), 4585 (RTCP feedback), 5104 (PLI/FIR), 4588 (RTX) | Media |
| Opus payload | 7587, 6716 | Audio |
| VP8 / H.264 payload | 7741, 6184 | Video transport |

## 5. Architecture

### 5.1 Module placement and dependencies

- New module `src/webrtc/`, TDL `webrtc.tdl`, prefix `rtc`, class names `PeerConnection`, `DataChannel`,
  `MediaTrack` (Phase 4+).  Enabled in the root `CMakeLists.txt` under `if (NOT DISABLE_WEBRTC)` and only when
  `TARGET network AND TARGET crypto`, mirroring the WebSocket block.
- Depends on Network (UDP/TCP/DNS), Crypto (hashes, HMAC, AES, random), Core (timers, FDs).  Audio is an optional
  runtime dependency for PCM playback/capture helpers; the transport layer never requires it.
- Third-party policy: follow the existing pattern of fetching a pinned BearSSL subset rather than adding large
  SDKs.  The only new external code proposed is **libopus** (BSD, fetched via `FetchContent` with a pinned tag,
  Phase 4) behind an option so data-channel-only builds carry no codec.  SCTP, STUN, ICE, SDP, RTP and SRTP are
  implemented in-house (rationale in §7).

### 5.2 Layering

```
  Tiri / C++ API        PeerConnection ── DataChannel ── MediaTrack
                              │
  Session layer          SDP + JSEP state machine, BUNDLE, transceivers, candidate signalling
                              │
  Transport              ICE agent ── DTLS transport ── { SCTP association, SRTP session }
                              │
  Packet demux (RFC 7983) one UDP NetSocket per local candidate base, plus TURN allocations
                              │
  Network module         NetSocket (UDP/TCP/TLS), NetLookup
```

Each layer lives in its own source files with no object-system dependencies so that they compile into the
`UNIT_TESTS` executable and can be fuzzed:

| File(s) | Responsibility |
| --- | --- |
| `stun.cpp/.h` | STUN message encode/decode, MESSAGE-INTEGRITY, FINGERPRINT, XOR-MAPPED-ADDRESS. |
| `ice.cpp/.h` | Candidate gathering, pair formation, connectivity checks, nomination, consent, restart. |
| `turn.cpp/.h` | Allocation, permissions, channel binding, refresh over UDP/TCP/TLS. |
| `sdp.cpp/.h` | SDP parse/serialise, JSEP offer/answer generation and validation, candidate lines. |
| `dtls_transport.cpp/.h` | Platform-neutral DTLS façade: `feed(datagram)`, `drain()`, handshake state, fingerprint check, SRTP key export.  Backends `dtls_openssl.cpp` and `dtls_schannel.cpp`. |
| `sctp_*.cpp/.h` | Association state, chunk codec, reliable/unreliable delivery, congestion control (RFC 4960 §7), stream reset, DCEP. |
| `rtp.cpp/.h`, `rtcp.cpp/.h`, `srtp.cpp/.h` | Packetisation, jitter buffer, SR/RR/NACK/PLI, SRTP protect/unprotect with replay window. |
| `payload_opus.cpp`, `payload_vp8.cpp`, `payload_h264.cpp` | Codec-specific packetisers/depacketisers. |
| `class_peerconnection.cpp`, `class_datachannel.cpp`, `class_mediatrack.cpp` | Object-system classes, field docs, callback dispatch. |

### 5.3 Threading and event model

- All protocol state runs on the owning object's task thread using `RegisterFD()`-driven `NetSocket.Incoming`
  callbacks and `SubscribeTimer()` for retransmission and keep-alive timers.  This matches how NetSocket and
  WebSocket already behave and avoids locking in the hot path.
- Audio and video encode/decode run on worker threads (precedent: `src/audio/audio_worker.h`), handing packets
  back via lock-free queues and `QueueAction()` wake-ups.  Only the media pipeline is multithreaded.
- DTLS must not own the socket.  The ICE layer owns every UDP socket and demultiplexes each datagram by first byte
  (RFC 7983); DTLS and SRTP receive bytes through memory buffers.  On OpenSSL this means `BIO_s_mem()` pairs with
  `DTLS_method()`, `SSL_set_mtu()`, and `DTLSv1_handle_timeout()`; on Schannel it means `ISC_REQ_DATAGRAM` with
  explicit buffer passing.  This is a new transport abstraction, separate from the stream TLS code in the Network
  module, but it should share certificate loading and error mapping where practical.

### 5.4 Public API sketch (TDL)

```lua
module({ name="WebRTC", prefix="rtc", ... }, function()
  c_include("<kotuku/modules/network.h>")

  enums("RTCS", { comment="PeerConnection connection states (W3C RTCPeerConnectionState)." },
    "NEW", "CONNECTING", "CONNECTED", "DISCONNECTED", "FAILED", "CLOSED")
  enums("RTCSIG", { comment="Signalling states (W3C RTCSignalingState)." },
    "STABLE", "HAVE_LOCAL_OFFER", "HAVE_REMOTE_OFFER", "HAVE_LOCAL_PRANSWER", "HAVE_REMOTE_PRANSWER", "CLOSED")
  enums("RTCICE", { comment="ICE connection states." },
    "NEW", "CHECKING", "CONNECTED", "COMPLETED", "FAILED", "DISCONNECTED", "CLOSED")
  enums("RTCGATHER", { comment="ICE gathering states." }, "NEW", "GATHERING", "COMPLETE")
  enums("RTCDC", { comment="DataChannel states." }, "CONNECTING", "OPEN", "CLOSING", "CLOSED")

  flags("RTCF", { comment="PeerConnection options." },
    "RELAY_ONLY: Use TURN relay candidates only.",
    "NO_HOST: Do not gather host candidates.",
    "NO_MDNS: Do not obfuscate host candidates with mDNS names.",
    "DISABLE_IPV6: Gather IPv4 candidates only.",
    "LOG_ALL: Verbose protocol logging.")

  structdef("RTCIceServer", {}, [[ string URL  string Username  string Credential ]])
  structdef("RTCIceCandidate", {}, [[ string Candidate  string Mid  int MLineIndex  string UsernameFragment ]])

  methods("PeerConnection", "rtc", {
    { id=1, name="CreateOffer" },      { id=2, name="CreateAnswer" },
    { id=3, name="SetLocalDescription" }, { id=4, name="SetRemoteDescription" },
    { id=5, name="AddIceCandidate" },  { id=6, name="CreateDataChannel" },
    { id=7, name="AddTrack" },         { id=8, name="RemoveTrack" },
    { id=9, name="RestartIce" },       { id=10, name="GetStats" },
    { id=11, name="Close" }
  })

  klass("PeerConnection", { ... }, [[
    string IceServers        # Comma-separated stun:/turn:/turns: URLs; credentials via RTCIceServer array
    string LocalDescription  # Current local SDP (read-only)
    string RemoteDescription # Current remote SDP (read-only)
    string CertificateFingerprint # sha-256 fingerprint of the local DTLS certificate
    int(RTCS)   State  int(RTCSIG) SignalingState  int(RTCICE) IceState  int(RTCGATHER) GatheringState
    int(RTCF) Flags  error Error  ptr ClientData
    # Callback fields (documented in class_peerconnection.cpp): Feedback, OnIceCandidate, OnDataChannel,
    # OnTrack, OnNegotiationNeeded
  ]])

  methods("DataChannel", "rtcdc", { { id=1, name="Send" }, { id=2, name="Close" } })
  klass("DataChannel", { ... }, [[
    string Label  string Protocol  int ID  int(RTCDC) State
    int Ordered  int MaxRetransmits  int MaxPacketLifeTime  large BufferedAmount  large BufferedAmountLowThreshold
    large MaxMessageSize  ptr ClientData
    # Callbacks: OnMessage(Channel, Data, Binary), OnOpen, OnClose, OnBufferedAmountLow, OnError
  ]])
end)
```

Callbacks follow the NetSocket/WebSocket pattern: `FUNCTION` fields with C++ and Tiri prototypes, `ERR::Terminate`
clears the callback, and `clear_callback_function()`/`pin()` lifetimes.  Asynchronous methods (`CreateOffer`,
`SetRemoteDescription`) are synchronous in the first release because SDP generation does not block; ICE
gathering results arrive through `OnIceCandidate` and the `GatheringState` feedback.

## 6. Phased Delivery

Estimates assume one experienced engineer and include tests and documentation.  Each phase ends with a merged PR,
green CI on Linux and Windows, and the plan file updated.

### Phase 0 — Spikes and decisions (1–2 weeks)

1. **Windows DTLS feasibility.**  Prove a Schannel DTLS 1.2 client and server handshake with an ECDSA
   self-signed certificate using memory buffers, and check whether `SECPKG_ATTR_KEYING_MATERIAL` (RFC 5705) and the
   `use_srtp` extension are obtainable.  Outcome decides whether Windows media uses Schannel, requires OpenSSL on
   Windows for the webrtc module only, or is deferred to data channels until a later release.
2. **OpenSSL DTLS over memory BIOs** with `SSL_CTX_set_tlsext_use_srtp()` and `SSL_export_keying_material()`.
3. **Browser interop harness.**  A Playwright script driving headless Chromium against an `origo` process over a
   WebSocket signalling channel, runnable from CMake as a labelled test (precedent: Autobahn job).
4. **UDP demux throughput** measurement with the existing NetSocket path on both platforms (packets/second at
   1200-byte MTU) to confirm no per-packet allocation surprises in `RecvFrom`.

### Phase 1 — Foundations (4–5 weeks)

- Crypto module: add AES-128/256-CTR, AES-GCM, SHA-1-HMAC fast path, CRC32 wrapper, ECDSA P-256 key generation
  and self-signed certificate creation (OpenSSL + CNG), constant-time compare already exists.  Unit tests against
  RFC test vectors.
- `stun.cpp`: full RFC 8489 codec with short-term and long-term credentials.  Fuzz target.
- `sdp.cpp`: parser/serialiser and JSEP offer/answer with BUNDLE, rtcp-mux, `a=setup`, `a=fingerprint`,
  `a=ice-ufrag/pwd`, `a=sctp-port`, `a=max-message-size`.  Fuzz target.
- `ice.cpp`: host candidates (IPv4/IPv6), server-reflexive via STUN, full agent with aggressive nomination
  disabled (regular nomination), trickle, consent freshness, ICE restart.  Tests with a simulated network in the
  unit executable (packet loss, reordering).
- `dtls_transport`: OpenSSL backend complete; Schannel backend per Phase 0 outcome.
- `PeerConnection` class skeleton: states, feedback callbacks, certificate per object, gathering, offer/answer
  with a data-channel m-line only.
- **Milestone:** DTLS handshake completes with headless Chromium over host candidates; Flute loopback test
  connects two PeerConnections in one process.

### Phase 2 — Data channels (4–6 weeks)

- SCTP association over DTLS: INIT/COOKIE handshake, DATA/SACK, RTO and congestion control (slow start, CWND,
  fast retransmit), ordered/unordered delivery, PR-SCTP (`MaxRetransmits`, `MaxPacketLifeTime`), stream reset
  for channel close, HEARTBEAT, graceful SHUTDOWN.  I-DATA (RFC 8260) is optional and can follow.
- DCEP open/ack, negotiated channels (`negotiated = true, id = n`), `BufferedAmount` back-pressure,
  `MaxMessageSize` negotiation.
- `DataChannel` class, Tiri bindings, string and binary payload handling consistent with `WebSocket.Send`.
- Tests: unit tests for chunk codec and state machine under simulated loss; Flute loopback for ordered,
  unordered, lifetime-limited and large (16 MiB) messages; browser interop test exchanging text and binary;
  throughput and latency benchmark.
- Reference signalling server (`examples/webrtc/signalling.tiri`) built on `WebSocketServer`, plus a static HTML
  demo page.
- Documentation: `docs/wiki/WebRTC.md` quick start and API reference generated from embedded docs.
- **Milestone:** Browser ↔ Kōtuku data channel chat demo on Linux and Windows.

### Phase 3 — NAT traversal and robustness (3–4 weeks)

- TURN client (UDP, TCP, TLS; allocations, permissions, channel data, refresh), `RELAY_ONLY` mode, long-term
  credentials.
- mDNS candidate obfuscation and resolution (RFC 8828).  Requires a small multicast DNS responder/resolver on the
  existing multicast UDP support; evaluate whether this belongs in the Network module as a reusable facility.
- ICE restart end to end, network interface change detection, dual-stack prioritisation (RFC 8421).
- `GetStats()` snapshot: selected pair, RTT, bytes/packets in and out, SCTP RTO/CWND.
- CI: coturn in a container for TURN tests; adversarial tests (malformed STUN/SDP/SCTP, replayed DTLS records).

### Phase 4 — Audio media (5–7 weeks)

- Prerequisite: Audio module capture API (ALSA `snd_pcm` capture stream, WASAPI capture client) delivering PCM
  frames through a callback.  This is a standalone Audio feature and should be planned and merged separately.
- RTP/RTCP: packetiser, sequence/timestamp handling, jitter buffer, SR/RR, NACK and RTX, SRTP with
  AES-CM-128-HMAC-SHA1-80 and AEAD_AES_128_GCM, replay protection, DTLS-SRTP key export, rtcp-mux.
- Opus via libopus (fetched, pinned); `MediaTrack` class for send/receive, `AddTrack` with transceivers,
  `OnTrack` callback; helpers to bridge a receive track to `Audio.AddStream()` and a capture source to a send
  track.
- Tests: SRTP vectors from RFC 3711/7714, RTP loopback, browser interop with echo loop, audio quality check via
  the existing `AudioAnalyser` level meters.

### Phase 5 — Video transport (3–4 weeks)

- VP8 (RFC 7741) and H.264 (RFC 6184) packetisation and depacketisation operating on application-supplied encoded
  frames; PLI/FIR/NACK feedback; frame assembly with loss detection.  No codec implementation is bundled.
- Optional later work: software VP8 via libvpx behind a build option, and decode-to-`Bitmap` helpers for display.

### Phase 6 — Hardening and release (2–3 weeks)

- Fuzzing of all parsers under a `WEBRTC_FUZZ` target, sanitiser runs, soak tests for 24-hour connections.
- Performance pass: zero-copy datagram path, pooled packet buffers, batched SACKs.
- Final documentation, examples, changelog, and plan close-out.

Total: roughly 22–31 engineering weeks for data channels through audio, with video transport and hardening on
top.  Data channels alone (Phases 0–3) are 12–17 weeks and already unlock most server-side use cases such as
low-latency control channels, game state sync and file transfer with browsers.

## 7. Key Decisions and Rationale

**In-house SCTP rather than usrsctp.**  usrsctp is the common choice but is unmaintained, has had memory-safety
CVEs, carries a full BSD networking stack, and uses its own threading model that fights Kōtuku's single-threaded
event loop.  The WebRTC profile needs only a single-homed, single-association subset.  Pion, str0m and werift
all shipped their own SCTP for this reason.  Fallback if Phase 2 overruns badly: wrap usrsctp behind the same
internal interface, since the layering keeps SCTP isolated.

**No libwebrtc.**  It is tens of millions of lines, needs Google's build tooling, and would dominate the binary.
It contradicts the project's existing choice to write HTTP and WebSocket in-house.

**DTLS from the platform TLS stack rather than a third DTLS library.**  OpenSSL already handles DTLS well.  The
Windows path is the main uncertainty and is the first spike.  If Schannel cannot export SRTP keying material,
data channels still work on Windows (they need no key export), and media on Windows becomes a follow-up where
OpenSSL is linked for the webrtc module only.

**Full ICE agent, not ICE-lite.**  ICE-lite would simplify Phase 1 but restricts Kōtuku to public-address
server deployments and cannot connect two Kōtuku peers behind NATs.  The extra cost is moderate and front-loaded.

**Media after data channels.**  Data channels have no codec dependencies, exercise every transport layer, and
give a complete, shippable feature early.  Audio capture work in the Audio module can proceed in parallel.

**API shape mirrors W3C.**  Developers arriving from browser WebRTC should recognise `createOffer`,
`setRemoteDescription`, `addIceCandidate`, `ondatachannel`.  Names are adapted to Kōtuku conventions (methods are
`mtCreateOffer`, callbacks are fields, state is an enum field with `Feedback`).

## 8. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Schannel lacks `use_srtp` / keying export | No Windows media via Schannel | Phase 0 spike; fall back to OpenSSL for webrtc on Windows or defer Windows media. |
| SCTP congestion control subtleties | Poor throughput or stalls with browsers | Simulated-network unit tests, Chromium interop benchmark, compare against RFC 4960 test cases. |
| Browser behaviour drift | Interop breaks silently | Scheduled interop job against current Chromium, like the Autobahn job. |
| Audio capture absent | Blocks send-side audio | Treat as an independent Audio module deliverable started during Phase 2. |
| H.264 licensing | Legal exposure if a codec were bundled | Transport-only video; never bundle an H.264 implementation. |
| Scope creep toward a full media engine | Schedule | Non-goals in §2; simulcast/BWE explicitly later. |

## 9. Testing Strategy

- **Unit tests** (compiled under `UNIT_TESTS`): STUN, SDP, SCTP chunk codec and state machine, RTP/SRTP vectors,
  ICE pair state machine with a scripted fake network.  Each parser gets a fuzz entry point.
- **Flute tests** (`src/webrtc/tests/test_*.tiri`): loopback PeerConnections for data channels, ordered and
  unordered delivery, large messages, close semantics, ICE restart, TURN via coturn, media loopback.
- **Browser interop**: Playwright + headless Chromium against `origo`, exercising data channels and audio; run on
  a schedule and on demand with a label.
- **Benchmarks**: data-channel throughput and round-trip latency recorded alongside the Network module's existing
  performance tests.

## 10. Open Questions

1. Should the mDNS resolver/responder live in the Network module as a general facility?  Leaning yes.
2. Is libopus acceptable as the first fetched codec dependency, or should audio initially be transport-only
   (application-supplied Opus frames) like video?
3. Do we want a Tiri `packages/webrtc` helper that bundles signalling over WebSocket for quick demos, in addition to
   the raw module API?
4. Target of the first release: Linux data channels only, or Linux and Windows together?  The Windows DTLS spike
   answers most of this.

## 11. Progress Log

- 2026-10-07: Plan drafted after surveying the Network, Crypto, WebSocket and Audio modules.  No code yet.
