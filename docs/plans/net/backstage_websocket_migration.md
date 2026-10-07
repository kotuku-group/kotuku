# Backstage WebSocket Migration Plan

Status: Implemented; Linux validation complete; Windows validation pending.  Prepared on 6 October 2026.

## Objective

Replace the private RFC 6455 implementation in `src/backstage/websocket.cpp` with the official `WebSocket` protocol
engine while retaining Backstage's REST API and `/streaming` endpoint on the same TCP port.

The migration must preserve Backstage's application protocol and loopback-origin policy.  The official module will own
opening-handshake validation, frame decoding and encoding, message assembly, UTF-8 validation, ping/pong handling,
send backpressure, the closing handshake and connection state.

## Current constraint

Backstage accepts REST requests and WebSocket upgrades through one `NetServer`.  Its Incoming callback initially parses
HTTP, then switches upgraded client sockets to a private frame parser.  In contrast, `WebSocketServer` always creates
and owns a dedicated `NetServer`, and server-role `WebSocket` initialisation is private.  The exported API has no way to
adopt a `ClientSocket` accepted by another listener or receive that listener's Incoming, Outgoing and Disconnected
events.

Running `WebSocketServer` on a second port would change the Backstage interface and complicate access control.  Copying
the official protocol classes into Backstage would retain duplicate protocol ownership.  The recommended solution is a
supported external-listener mode in `WebSocketServer`.

## Scope

This work includes:

- an official API for adopting a complete server-side opening request on an existing `ClientSocket`;
- event forwarding from an external `NetServer` to the adopted connection;
- explicit socket and `WebSocket` lifetime rules;
- migration of `/streaming` to `WebSocket.Incoming`, `WebSocket.Send()` and `WebSocket.Close()`;
- removal of Backstage's handshake, SHA-1, Base64, frame and close implementations;
- optional-build behaviour when the WebSocket module is unavailable;
- focused protocol-adoption and Backstage integration coverage;
- generated headers and API documentation for the new public contract.

This work does not include per-message compression, TLS for the loopback Backstage listener, changes to the streaming
JSON schema, real log-event production, or integration with `packages/httpserver`.  The adoption API should be suitable
for a later `httpserver` integration, but that migration is separate.

## Proposed public API

### External-listener mode

Add `WSF::EXTERNAL_LISTENER`, valid only on `WebSocketServer`.  With this flag set, `Init()` validates the configured
limits, protocols and callbacks but does not create or bind a `NetServer`.  `Address`, `Port`, `SSL` and certificate
fields are not used in this mode.  `Port` remains zero.

Using the flag on `WebSocket` is invalid.  Combining it with `WSF::SSL` is invalid because TLS belongs to the external
listener.

### Adopt method

Add `WebSocketServer.Adopt()` with this logical signature:

```text
Adopt(ClientSocket Socket, array(byte) RequestData, &WebSocket Connection)
```

`RequestData` contains the complete HTTP opening request and may contain WebSocket bytes after the header terminator.
The method must:

1. Require an initialised external-listener server and a connected, unmanaged `ClientSocket`.
2. Find and validate the opening request with the existing `ws::find_request_end()` and `ws::parse_request()` helpers.
3. Apply `WebSocketServer.Path`, subprotocol selection and the existing Accept callback.
4. Generate either the existing standards-compliant rejection response or the 101 response.
5. Create the server-role `WebSocket` through the existing connection factory.
6. Preserve every byte after the HTTP header as the connection prefix.
7. Announce the connection through Connected before delivering the prefix, matching dedicated-server behaviour.
8. Return the accepted `WebSocket` in `Connection`; leave it null when the request was rejected.

A protocol rejection is a handled request and returns `ERR::Okay`; the null result distinguishes it from acceptance.
An operational failure that prevents a complete response returns its specific error, and the external listener should
terminate the socket.  A second adoption of the same socket returns `ERR::InUse` without modifying the existing
connection.

### Dispatch method

Add a `WSE` event enum with `INCOMING`, `OUTGOING` and `DISCONNECTED`, and add this method:

```text
Dispatch(ClientSocket Socket, WSE Event)
```

The method routes by `ClientSocket.UID` through the server's pending/rejected and upgraded maps:

- `INCOMING` reads and processes all available bytes through `transport_incoming()`; its `ERR::Terminate` result must
  be propagated by the external listener.
- `OUTGOING` calls `transport_writable()`.  It returns `ERR::Okay` because Network clears a listener's global Outgoing
  callback after any error.
- `DISCONNECTED` calls `transport_disconnected()`, removes pending state and completes normal server-side cleanup.
- An unmanaged socket returns `ERR::NotFound` without reading, writing or changing the socket.

Backstage can therefore call `Dispatch(INCOMING)` before reading a socket itself.  `ERR::NotFound` selects ordinary HTTP
processing; every other result belongs to the WebSocket path.

### Ownership contract

The external `NetServer` remains the framework owner of each `ClientSocket`.  After `Adopt()` accepts or starts a
rejection response, the WebSocket server becomes responsible for protocol use and closure of that socket.  The caller
must continue forwarding all three event types and must not read from or write to an adopted socket directly.

The existing deferred-destruction rules remain authoritative:

- do not free a socket from its Incoming or Outgoing callback;
- drain a queued Close or rejection response before disconnecting;
- report `CLOSED` before the server's Disconnected callback;
- remove the connection from `TotalConnections` exactly once;
- freeing the external-listener `WebSocketServer` closes its adopted connections abortively without freeing the
  external `NetServer`;
- freeing the external `NetServer` first is supported when its Disconnected events are forwarded before the
  `WebSocketServer` is released.

Document that both objects and their callbacks must run on the same message-processing thread.  Do not add locks around
the protocol engine as a substitute for this requirement.

## Backstage integration

### Module and build availability

Keep the Backstage REST service available in builds without WebSocket support:

1. Move the Backstage CMake block after the WebSocket target decision so `TARGET websocket` is known.
2. When the target exists, define a private Backstage feature macro and compile the official integration.
3. Load the `websocket` module during Backstage initialisation and create one global external-listener
   `WebSocketServer` configured with:
   - `Path = "/streaming"`;
   - `MaxMessageSize = 64 * 1024`;
   - a bounded `SendLimit` no smaller than the largest permitted message;
   - Accept, Connected and Disconnected callbacks.
4. If the module is unavailable, continue starting the REST listener and answer `/streaming` with
   `501 Not Implemented`.  Do not retain the private protocol implementation as a fallback.
5. Release the adopted WebSocket server before the Backstage `NetServer`, while callback targets are still valid, then
   release the loaded module during expunge.

Confirm modular and static linkage.  A static build must include `websocket` whenever Backstage streaming support is
compiled, while a `DISABLE_WEBSOCKET=ON` build must not acquire an unresolved class or module dependency.

### Listener dispatch

Update `src/backstage/server.cpp` as follows:

- Incoming: call `WebSocketServer.Dispatch(Socket, WSE::INCOMING)` before reading.  Continue with HTTP only for
  `ERR::NotFound`.
- Outgoing: add a listener callback that forwards `WSE::OUTGOING` for managed sockets and returns `ERR::Okay` for both
  managed and unmanaged sockets.
- Feedback: forward `WSE::DISCONNECTED` before clearing ordinary request state.  Treat `ERR::NotFound` as a normal HTTP
  connection.
- Upgrade: pass the exact buffered request, including any post-header bytes, to `Adopt()`.  Once `Adopt()` handles the
  request, suppress `BackstageHttpResponse::write()`.

Do not parse or copy the WebSocket prefix into a second Backstage buffer.  The adoption call consumes the complete
buffer synchronously and copies the prefix into the new connection before the request string leaves scope.

### Application callbacks

Retain the application-specific parts of `src/backstage/websocket.cpp`:

- loopback-origin parsing and policy;
- JSON escaping, command parsing and response generation;
- subscription state and sequence numbers;
- system and log-stub event creation.

Change session keys from `ClientSocket.UID` to `WebSocket.UID`.  The Connected callback creates the session, installs
Incoming and StateChanged callbacks, then sends the existing `connected` and `heartbeat` events.  The Disconnected
callback removes the session.

Incoming receives assembled messages because `STREAM_MESSAGES` remains disabled:

- `WSM::TEXT`: decode the buffer as the existing JSON command and send responses with `WebSocket.Send()`;
- `WSM::BINARY`: start a Close with `WSC::UNSUPPORTED_DATA` (1003);
- invalid UTF-8, malformed control frames, masking errors and oversized messages are handled by the official parser.

Never hold `glWebSocketLock` while invoking a `WebSocket` method or application callback.  Build the outgoing message
and update session state under the lock, release it, then call `Send()`.  Future event producers on other threads must
post work to the WebSocket's message-processing thread rather than sending directly.

### Intentional HTTP response changes

Adopt the official server's RFC-oriented handshake responses:

| Condition | Current Backstage response | Migrated response |
|---|---:|---:|
| Unsupported `Sec-WebSocket-Version` | 400 | 426 with `Sec-WebSocket-Version: 13` |
| Origin rejected by the Accept callback | 400 with text body | 403 |
| Malformed key or upgrade request | 400 | 400 |

The WebSocket endpoint, accepted message schema, acknowledgements, events and close code for binary messages remain
unchanged.

## Implementation phases

### Phase 1: Public adoption contract

- [x] Add `WSF::EXTERNAL_LISTENER` and `WSE` to `src/websocket/websocket.tdl`.
- [x] Add documented `Adopt` and `Dispatch` methods to `WebSocketServer`.
- [x] Refactor handshake completion so dedicated listeners and adopted requests use one validation and response path.
- [x] Reuse `create_connection()`, prefix delivery, callback ordering and deferred cleanup in both modes.
- [x] Reject invalid flag combinations and listener-only field use consistently.
- [x] Regenerate `class_websocket_server_def.c`, the public WebSocket header and XML documentation with
  `build_headers`.

### Phase 2: Adoption tests

- [x] Add `src/websocket/tests/test_adoption.tiri` with an ordinary `NetServer` that delegates events to an
  external-listener `WebSocketServer`.
- [x] Register it as `websocket_adoption` in `src/websocket/CMakeLists.txt`.
- [x] Cover acceptance, rejection, subprotocol selection, Accept policy, a frame coalesced with the request,
  fragmented text, interleaved control frames and normal Close.
- [x] Cover foreign sockets, duplicate adoption, disconnect before and after acceptance, freeing from callbacks,
  freeing the adoption server with live connections and external-listener shutdown order.
- [x] Exercise a slow reader until `SendLimit` is reached, then verify ordered resumption through forwarded Outgoing
  events.
- [x] Keep the existing dedicated `websocket_server`, loopback, client, TLS and compiled protocol suites unchanged as
  regression coverage.

### Phase 3: Backstage migration

- [x] Add conditional WebSocket module loading and the external-listener server object.
- [x] Add Incoming, Outgoing and Disconnected forwarding to `src/backstage/server.cpp`.
- [x] Replace the local upgrade routine with `Adopt()` using the complete buffered request.
- [x] Convert event sends and application replies to `WebSocket.Send()`.
- [x] Convert binary-message rejection to `WebSocket.Close(1003, ...)`.
- [x] Move lifecycle cleanup to official Connected, StateChanged and Disconnected callbacks.
- [x] Delete local SHA-1, Base64, handshake, frame parsing/writing, ping/pong and close code.
- [x] Retain and rename only the Backstage-specific policy, session, JSON and topic helpers.

### Phase 4: Backstage tests and documentation

- [x] Preserve handshake success, loopback-origin acceptance, lookalike-origin rejection, subscriptions, unknown
  commands/topics, ping/pong, binary rejection and clean Close coverage in `test_backstage.tiri`.
- [x] Update expected origin and unsupported-version status codes.
- [x] Add a handshake-plus-frame test proving that post-header bytes are delivered exactly once.
- [x] Add fragmented subscription input, invalid UTF-8, malformed Close, unmasked frame, oversized message and abrupt
  disconnect cases with expected close codes.
- [x] Add a burst-send/backpressure case that verifies complete, ordered JSON events without exceeding `SendLimit`.
- [x] Add build-variant coverage showing REST remains available and `/streaming` returns 501 when WebSocket support is
  disabled.
- [x] Update `src/backstage/AGENTS.md`, generated WebSocket API documentation and the WebSocket sections of
  `docs/plans/net/network_features.md` and `docs/plans/net/httpserver_lib_features.md`.

## Validation matrix

Always build and install before running CTest.

### Linux Debug modular build

```bash
cmake --build build/agents --config Debug --target websocket backstage --parallel
cmake --install build/agents --config Debug
ctest --build-config Debug --test-dir build/agents --output-on-failure \
  -R '^(websocket_unit_tests|websocket_client|websocket_server|websocket_adoption|websocket_loopback|websocket_tls|backstage_ping)$'
```

Run the focused Backstage suite directly while iterating:

```bash
build/agents-install/origo tools/flute.tiri file=src/backstage/tests/test_backstage.tiri --log-warning
```

### Static build

Configure a Debug tree with `KOTUKU_STATIC=ON`, build `websocket backstage origo_cmd`, install it, then run
`websocket_adoption` and `backstage_ping`.  Confirm `origo --version` reports the expected Debug static build.

### WebSocket-disabled build

Configure a separate Debug tree with `DISABLE_WEBSOCKET=ON`.  Build and install Backstage, run its REST tests, and
verify `/streaming` returns 501 without module-load errors or unresolved static symbols.

### Windows

Build and install the modular Debug targets with native Network TLS, then run `websocket_adoption`,
`websocket_server` and `backstage_ping`.  Include `--log-threads` when diagnosing callback ordering or IOCP lifetime
failures.

## Acceptance criteria

- Backstage REST and `/streaming` continue to share the configured Backstage port.
- No WebSocket handshake, hashing, frame parser, frame writer, ping/pong or closing-state implementation remains in
  Backstage.
- Backstage retains its loopback-origin policy and streaming JSON contract.
- Frames received with the opening request are delivered once and in order.
- Fragmented text works; binary input closes with 1003; invalid UTF-8 closes with 1007; protocol violations close with
  1002; oversized messages close with 1009.
- Partial writes and slow readers do not duplicate, truncate or reorder messages, and queued bytes remain bounded by
  `SendLimit`.
- Every accepted, rejected, timed-out and abruptly disconnected socket is removed exactly once without leaks,
  use-after-free or duplicate Disconnected notifications.
- Dedicated `WebSocketServer` behaviour and all existing WebSocket client behaviour remain unchanged.
- Modular, static, WebSocket-disabled and Windows Debug validation completes as described above.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| A forwarded event reads a normal HTTP socket | `Dispatch()` returns `ERR::NotFound` without touching unmanaged sockets. |
| Network clears Backstage's global Outgoing callback | Map unmanaged sockets and protocol errors to `ERR::Okay` in the Outgoing callback. |
| A request-prefix frame is lost or processed twice | Pass the original complete buffer once; reuse `HandoverPending` and existing prefix delivery. |
| Socket destruction occurs inside a Network callback | Preserve `InSocketCallback`, `InOutgoing`, `InFeedback` and deferred-free paths. |
| Shutdown calls into a freed adoption server | Release the adoption server while the external listener and callback context are still valid, then clear its pointer before listener teardown. |
| Optional builds silently retain the unsafe implementation | Remove the local protocol fallback and make disabled streaming return an explicit 501. |
| Backstage locks deadlock through callbacks | Never hold `glWebSocketLock` while calling WebSocket methods or user callbacks. |

## Completion record

Implemented on 7 October 2026 in the working tree; no commit was created.

- Added external-listener flags, event enum, shared opening-request validation, Adopt and Dispatch.  External sockets
  remain tracked until disconnection, including rejection and late closure events.  Duplicate adoption is prevented
  across protocol servers on the processing thread.  Operational failures relinquish pending adoption state.
- Backstage now loads the optional official module, adopts complete request buffers, forwards listener events and
  exchanges assembled messages through Send and Close.  Its local RFC 6455, SHA-1 and Base64 code and dependency were
  removed.  REST and streaming retain one loopback port.  Disabled streaming returns 501.
- Added 20 adoption tests and expanded Backstage to 37 tests.  Coverage includes prefix delivery, fragmented text and
  interleaved control frames, protocol failures, duplicate and foreign sockets, abrupt disconnect, callback destruction,
  both shutdown orders, close timeout and bounded slow-reader resumption.  The Backstage burst case decodes all 202
  JSON frames and verifies event sequences and acknowledgement order.
- Generated the public header, method metadata, runtime enum definitions and WebSocket class XML with build_headers.

### Completed Linux Debug validation

| Configuration | CTest result | Focused totals |
|---|---|---|
| Modular `build/agents` | 7/7 suites passed | 28 embedded protocol tests; client 23, dedicated server 11, adoption 20, loopback 6, TLS 5, Backstage 37 |
| Static `build/agents-static` | 2/2 suites passed | Adoption 20; Backstage 37 |
| Static, WebSocket disabled `build/agents-no-websocket` | 1/1 suite passed | Backstage REST and explicit streaming 501: 21 tests |

Each build was installed before CTest.  Both static executables reported Debug and `GetSystemState().staticBuild == 1`.
Static configurations disabled unrelated graphics/audio features and used `CMAKE_CXX_STANDARD_LIBRARIES=-lz` to resolve
an existing static OpenSSL/zlib link-order issue in this environment.  This workaround is confined to build caches;
the Network build configuration was not changed.  The test variants ran sequentially because Backstage's existing
fixture uses a fixed port.  An initial client timing failure passed in isolation and in the subsequent complete runs.

### Clarifications and remaining validation

- External-mode closure reports socket disconnection before deferred destruction so Tiri can forward a live object.
  NetServer suppresses Feedback during destruction.  For listener-first shutdown, callers must explicitly forward
  DISCONNECTED for their adopted sockets before freeing the listener; this is documented and tested.
- The requested Backstage AGENTS file and two network feature-plan files were absent from this checkout.  They were
  created with current integration and future httpserver guidance.
- Windows native Network TLS and IOCP validation could not run in this Linux environment and remains outstanding.
  No protocol or HTTP status deviation from the proposed migration was introduced.
