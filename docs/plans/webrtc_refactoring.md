# WebRTC Refactoring Plan

Created: 2026-10-15
Status: Implementation order steps 1 and 2 complete (recommendations 1, 6 and 7); steps 3-5 not started.
Related plan: [Native WebRTC support](webrtc_native_support.md).

## Scope and evidence

Review the module for efficiency, readability and maintainability following its phased implementation and directory reorganisation. Preserve the existing separation between object-independent protocols and framework adapters.

The review covered the current working tree, including staged reorganisation, public class bindings, protocol interfaces, CMake registration and relevant tests. No source changes, builds or benchmarks were performed. Performance opportunities are inferred from code; measured improvements remain to be established. Source line numbers below refer to the reviewed tree and may move.

## Recommendations

### 1. Regenerate SDP only when it changes

Priority: High. Confidence: High.

Evidence: `peerconnection/peer_engine.cpp:1147` serialises the committed local description during timer processing even after gathering completes. The default mDNS service requests ticks every 10 ms. `peerconnection/peerconnection.cpp:52` also assigns description strings to public fields on every refresh.

- [x] Track description changes or revisions and serialise only after candidates, credentials, negotiation state or end-of-candidates change.
- [x] Use the revision to avoid redundant public-field copies where appropriate.
- [x] Preserve immediate description visibility at existing API and callback boundaries.

Validation: Compare SDP before and after gathering, candidate withdrawal, offer/answer and restart. Measure serialisation counts and idle work after connection establishment.

### 2. Reduce repeated buffered-byte scans

Priority: High for investigation. Confidence: High that repeated work exists; benefit requires measurement.

Evidence: `peerconnection/peer_engine.cpp:1437` refreshes all buffered totals after every successful channel send. `sctp/sctp_transfer.cpp:1165` allocates a map and scans queued messages and sent chunks; the caller then visits all public channels. Bursts into a growing queue can accumulate quadratic scanning work.

- [ ] Benchmark bursts with growing queues and multiple channels.
- [ ] Evaluate maintained per-stream totals or updating only the affected stream on enqueue.
- [ ] Preserve the distinction between transport acceptance and SCTP acknowledgement.
- [ ] Preserve buffered-amount-low threshold crossings, expiry, abandonment and stream reuse semantics.

Validation: Native SCTP transfer and stream tests, public channel buffering and reentrant low-water callbacks, partial reliability, close and stream reuse. Compare accounting against the existing scan during development.

### 3. Retain metadata for encoded SCTP output

Priority: Medium. Confidence: High that redundant decoding exists; design and payoff require validation.

Evidence: `sctp/sctp_transfer.cpp:1188` reparses locally generated packets, including CRC verification, to recover DATA TSNs when output is accepted. `sctp/sctp_streams.cpp:48` reparses and rebuilds output during message abandonment and searches sent descriptors for DATA chunks.

- [ ] Profile these paths before changing the output contract.
- [ ] Evaluate an internal output record containing wire bytes and associated DATA identifiers.
- [ ] Use retained metadata for acceptance and abandonment without weakening validation of incoming packets.
- [ ] Update metadata consistently whenever output is rebuilt or discarded.

Validation: Packet acceptance, retransmission, expiry, abandonment, mixed control/DATA output and TSN wraparound. Run SCTP unit tests, fuzz regressions and browser interoperability checks.

### 4. Share legacy STUN and IceServers gathering machinery

Priority: Medium. Confidence: High.

Evidence: `peerconnection/peer_engine.cpp:129` onwards implements legacy URI parsing, DNS callbacks, lookup ownership, flags, endpoint selection and timeout handling. The `ServerJob` path starting around line 277 implements overlapping responsibilities for IceServers.

- [ ] Preserve `StunServer` as a compatibility interface.
- [ ] Progressively share parsing, lookup ownership and gathering jobs.
- [ ] Preserve uppercase-scheme acceptance, bare-address syntax, address-family selection and IceServers fallback precedence.
- [ ] Make cancellation, completion and restart ownership explicit.

Validation: Legacy parser cases, DNS cancellation and late completion, empty/nonempty IceServers precedence, IPv4/IPv6 selection, restart and STUN/TURN gathering on Linux and Windows.

### 5. Centralise terminal failure transitions

Priority: Medium. Confidence: High as a maintainability finding; no defect established.

Evidence: `peerconnection/peer_engine.cpp:737`, `:1017` and `:1062` contain overlapping failure sequences for restart, recovery timeout, mDNS, relay and transport failures. They variously update state, abort SCTP, terminate channels, clear input, notify callbacks and wake the owner.

- [ ] Document required cleanup and notification ordering for each failure class.
- [ ] Introduce a small failure-transition helper with explicit rules for intentional differences.
- [ ] Extract gathering progression from `tick()` after failure handling is clear.
- [ ] Preserve callback reentrancy checks and terminal notification semantics.

Validation: Fingerprint failure, failed gathering, relay-only failure, recovery timeout, restart failure and pending/open channel termination. Check error-before-close ordering and callbacks that close or free objects.

### 6. Share callback lifetime bookkeeping

Priority: High-value consolidation. Confidence: High.

Evidence: `peerconnection/peerconnection.cpp:70`, `webchannel/webchannel.cpp:86` and callback setters such as `webchannel/webchannel.cpp:292` repeat stale-callback removal, callback snapshots, pin/unpin and replacement bookkeeping.

- [x] Introduce small helpers for callback replacement and scoped callback retention.
- [x] Keep typed invocation, event ordering and object pin ownership explicit.
- [x] Preserve `rtc_release_pin()` and deferred object destruction exactly.
- [x] Avoid replacing deferred release with immediate destruction.

Validation: Existing lifecycle coverage for freeing a peer or channel inside callbacks, freeing from error and low-water callbacks, callback replacement and reentrant buffered-amount notifications. Exercise both C++ and Tiri bindings.

### 7. Share address conversion and ICE wire helpers

Priority: High-value consolidation. Confidence: High.

Evidence: `ice/ice_socket.cpp:18` and `turn/turn_socket.cpp:28` duplicate Network/endpoint conversion and IPv4-mapped IPv6 handling. ICE-lite and full ICE repeat attribute cardinality validation and XOR-address encoding; `turn/turn.cpp:97` contains another XOR-address encoder.

- [x] Share Network conversions within adapter support.
- [x] Share wire helpers within the object-independent protocol layer.
- [x] Preserve host/network byte order and IPv6 transaction-ID masking.
- [x] Keep ICE-lite and full ICE role-specific behaviour separate.

Validation: IPv4, IPv6 and mapped IPv6 conversions, malformed/duplicate attributes, XOR-address wire fixtures, native ICE/TURN tests and relevant fuzz regressions.

### 8. Name and relate resource-budget constants

Priority: Medium. Confidence: High.

Evidence: `peerconnection/peer_engine.cpp:1173`, `sctp/sctp_streams.cpp:610` and `sctp/sctp_association.h:232` use related limits across layers: 256 channels, 256 KiB of labels, 4096 events, 4094 admission thresholds, 3072 backpressure threshold and a 64-packet output allowance.

- [ ] Give limits descriptive names at their owning layer.
- [ ] Express actual dependencies, including terminal-event reserves and the relationship to `MAX_OUTPUT`.
- [ ] Keep unrelated limits distinct even when numeric values coincide.
- [ ] Preserve existing budget values in the initial cleanup.

Validation: Admission boundaries, reserved error/close events, backpressure, queue exhaustion and existing compile-time memory-budget assertions.

## Implementation order

1. Establish focused baseline behaviour and performance measurements where relevant.
2. Apply SDP change tracking and small helper consolidations as separate changes.
3. Name resource budgets without changing their values.
4. Optimise buffered accounting and output metadata only with measurements and accounting regressions.
5. Consolidate gathering and failure transitions incrementally, retaining compatibility tests.

## Validation requirements

- Use the repository C++ programming and code-quality-reviewer skills for implementation and review; use flute-testing and tiri-programming before changing or running Flute tests.
- Build affected Debug targets. For static builds, rebuild `origo_cmd` as well.
- Install the latest build before running tests.
- Select focused native and public API tests per recommendation, then browser/TURN interoperability and Linux/Windows coverage for affected transport paths.
- Use sanitiser and fuzz regressions for ownership, packet metadata and parser changes.
- Regenerate headers and XML only if TDL, blueprints or embedded documentation change.
- Record actual commands, outcomes and performance evidence here as work completes.

## Boundaries and non-findings

ICE-lite has real test callers and is not a dead-code removal target. Generated class hooks and the single translation unit for public class bindings are intentional integration mechanisms. Preserve protocol independence and public API contracts. No confirmed bug is claimed by this refactoring review.

## Progress

- 2026-10-15: Completed source review and recorded eight recommendations. Saved implementation order, compatibility constraints and validation requirements. No refactoring implemented or runtime validation performed.
- 2026-11-04: Completed implementation order steps 1 and 2.
  - Baseline (unmodified tree, Linux Debug, `ctest --preset agents -R webrtc`): 36/39 passed.  `webrtc_network_change_recover`, `webrtc_network_change_free` and `webrtc_network_change_fail` fail with "Timed out waiting for channel progress" in the cloud container before any change, and fail identically after each change.
  - Baseline SDP measurement: a temporary counter in `peer_engine.cpp` recorded 500 local SDP serialisations over 500 idle ticks after gathering completed (one per tick).
  - Recommendation 1: `peer_engine` tracks a local description revision (`local_changed()`), re-serialises committed SDP only through `refresh_local()` when the revision has advanced, and exposes `description_revision()` so `peer_refresh()` copies `LocalDescription`/`RemoteDescription` only on change.  Uncommitted offers/answers are never re-serialised.  Measured result: 0 serialisations over 500 idle ticks; `test_peer_connection` now asserts this and that the idle SDP contains the last candidate and end-of-candidates.
  - Recommendation 6: `webrtc.cpp` adds `rtc_clear_callback()`, `rtc_set_callback()`, `rtc_live_callback()` and `RetainedCallback`.  All PeerConnection and WebChannel setters, destructors and notification paths use them; object pins and `rtc_release_pin()` remain explicit, and the callback copy is released before the object pin as before.  Added Flute test `ReplaceCallbacksFromWithin` (clearing Feedback and replacing OnMessage from inside the callback).
  - Recommendation 7: `ice/network_endpoint.h` provides `from_network()`, `to_network()` and `unmap_ipv4()` for both socket adapters.  TURN UDP source validation now compares normalised endpoints; the only behavioural difference is that a plain IPv4 source would now match a server configured as an IPv4-mapped IPv6 literal, which cannot occur on the dual-stack socket used for such servers.  `stun::valid_attribute()` and `stun::encode_xor_address()` replace the ICE-lite, full ICE and TURN copies.  Added conversion checks to `test_ice_socket` and RFC 5769 section 2.2/2.3 XOR-address vectors plus cardinality checks to `test_stun`.
  - Validation: `cmake --build build/agents` with no new warnings; `ctest --preset agents -R webrtc` 36/39 after every change (same three pre-existing failures).  Fuzz targets `turn_codec`, `turn_state` and `stun` were built manually with clang 18 (`-fsanitize=fuzzer,address,undefined`; `-Wno-invalid-constexpr` was needed for an unrelated `core.h` diagnostic).  Their regression corpora replayed cleanly, and 60-second campaigns found no issues.  Not run: full-module ASan build, browser/TURN interop beyond the registered ctest fixtures, and Windows.
