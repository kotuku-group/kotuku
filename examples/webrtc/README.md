# Local native WebRTC chat (Linux)

Run these commands from the repository root after the modular Debug build and install described in
`docs/plans/webrtc_phase_2_linux.md`.  The example uses the installed WebRTC module; configure with
`LINKED_PACKAGES=ON` for its JSON package import.

In one terminal:

```bash
build/agents-install/origo examples/webrtc/signalling.tiri port=9320 --log-warning
```

In another:

```bash
python3 -m http.server 9323 --bind 127.0.0.1 --directory examples/webrtc
```

Launch a separate local Chromium profile:

```bash
google-chrome --user-data-dir=/tmp/kotuku-webrtc-demo \
  http://127.0.0.1:9323/
```

Click **Connect**, wait for **Data channel: open**, then send a message.  The native peer sends a greeting and
returns every complete message unchanged.  Select **Native peer creates the offer** to exercise the other SDP
direction.  The signalling status describes the WebSocket only; it does not prove ICE, DTLS or a data channel has
opened.  Disconnect closes both transports.  Use a dedicated disposable browser profile for local testing.

Each WebSocket connection owns exactly one native PeerConnection.  SDP and candidate messages are routed only to
that peer; a second join is rejected.  Disconnect explicitly frees the native parent and its channels.  The server
binds localhost, limits concurrent sessions to eight and caps each signalling message at 128 KiB.  The chat channel
is created by the native peer; additional echo channels demonstrate reliable/unordered/partial-reliability and
negotiated channel creation for the automated harness.  The browser's negotiated ID 100 must match the native ID.

Host candidates use random `.local` names by default.  Registration is asynchronous, so the server forwards
trickle candidates after the initial description; the browser queues candidates until it has a remote description.
Native names are withdrawn on close and replaced on ICE restart.  Multicast must be available on the local network;
unavailable registration suppresses that host without exposing a numeric fallback.  For an explicit numeric-host
test, pass `policy=NO_MDNS` to the signalling script.

TURN is optional; media, I-DATA interleaving and general renegotiation remain unsupported.  A local relay test does not establish connectivity through every NAT/firewall topology.  Signalling is an
unauthenticated localhost development service.  The message field limits the interactive demo to 4096 characters;
the installed native receive limit defaults to 262144 bytes per message.  A successful Send queues a copied payload;
it does not confirm delivery.  The browser pauses interactive sends while more than 64 KiB is buffered.

Automated public API checks:

```bash
ctest --preset agents -R '^(webrtc_public_channels_interop|webrtc_mdns_interop|webrtc_mdns_channels_interop)$'
```

The browser harness requires the pinned Playwright package and an actual Chromium executable configured through
`WEBRTC_INTEROP_NODE_PATH`, `WEBRTC_CHROME_EXECUTABLE` and `WEBRTC_REQUIRE_INTEROP=ON`.

## TURN and network recovery

Supply native ICE servers in a private JSON file (maximum 16 KiB), then run:

```bash
build/agents-install/origo examples/webrtc/signalling.tiri config=/private/ice_servers.json policy=RELAY_ONLY --log-warning
```

The file contains an array such as `[{"url":"turns:relay.example:5349?transport=tcp","username":"temporary-user","credential":"temporary-secret"}]`.  Obtain expiring credentials from your application's authenticated service; do not commit real credentials or embed them in a public webpage.  Supported client transports are `turn:...?transport=udp`, `turn:...?transport=tcp` and `turns:...?transport=tcp`; all use UDP between relay and peer.  TLS uses the system trust store and checks the server hostname.  A nonempty IceServers array overrides legacy StunServer.  One failed server does not discard a usable allocation from another.

RELAY_ONLY restricts actual routes and advertised candidates; allocation failure cannot fall back to direct traffic.  NO_HOST suppresses host candidates, NO_MDNS explicitly permits numeric host signalling, and DISABLE_IPV6 restricts gathering to IPv4.  The browser in this demo has no ICE servers by default; native-only relay configuration tests a relay-to-direct path.  For relay on both ends, configure the browser's RTCPeerConnection iceServers independently using the browser's `urls` key and `iceTransportPolicy: 'relay'`.  Native configuration uses `url`.

Ordinary Chromium needs no mDNS-disabling flag.  Allow scoped UDP multicast on port 5353; when unavailable, native host registration and remote name resolution finish within bounded deadlines.  Configure TURN for a usable alternative.  Default SDP, candidate callbacks and statistics retain random names, including for resolved remote candidates; private numeric mappings remain internal.

On an interface change the native onRestartRequired callback sends `restart-required` to the browser.  The browser creates a fresh ICE offer and the native application answers it through the existing offer/answer messages.  This exchange preserves compatible DTLS/SCTP state and public channel handles; reliable accepted messages remain queued while recovery is pending.  Recovery requires working signalling and compatible restart descriptions.  Partial-reliability deadlines continue to elapse, permanent loss fails within the recovery deadline, and simultaneous offers require application arbitration beyond this demo.

## Statistics

`local _, text = check peer.mtGetStats()` returns an immutable owned JSON string; decode with `json.decode(text)`.  C++ uses `peer->getStats(&text)` and releases each result with `FreeResource(text)`.  Snapshots remain valid after restart, close and parent destruction.  Version 1 uses microseconds for timestampUs/iceRttUs/sctp.rtoUs and bytes for sctp.cwndBytes/pathMtuBytes.  Timestamp, generation and byte/packet totals are decimal strings to preserve 64-bit precision; convert only when the consumer can tolerate its numeric limits.  Null RTT means no valid authenticated response sample for the selected route, and differs from a zero sample.

Connection counters include accepted inner ICE/consent/DTLS datagrams and retransmissions and persist across restart/close.  Route totals belong to the local/peer pair; TURN byte totals include allocation framing and maintenance at the adapter boundary, excluding IP/TCP/TLS headers.  TURN allocation totals may include checks to other candidate peers.  These are software observations rather than NIC wire totals.  Route IDs change on restart; endpoints obey signalling privacy policy.  Closed peers report null live route, RTT and SCTP values.  This signalling example also accepts a `stats` message and returns the decoded snapshot.
