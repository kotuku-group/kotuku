# Internal mDNS Service

This folder is a small, self-contained multicast DNS (RFC 6762) implementation. It is built as the `network_mdns` static library and is **internal**: it adds nothing to the public Network module API.

Its only consumer today is the WebRTC module. Browsers hide a peer's private LAN addresses by publishing ICE host candidates under random `.local` names (see [draft-ietf-mmusic-mdns-ice-candidates](https://datatracker.ietf.org/doc/draft-ietf-mmusic-mdns-ice-candidates/)). `PeerConnection` uses this service to:

- register a random 32-hex-digit `.local` name for each local host address, so the numeric address never appears in the SDP, and
- resolve `.local` names received in remote candidates back to addresses on the local link.

See `src/webrtc/peerconnection/peer_engine.cpp` and `src/webrtc/ice/ice_candidates.cpp` for the call sites.

## Scope

The implementation covers only what WebRTC needs:

- A and AAAA records for single-label host names (`<label>.local`, label of `a-z`, `0-9` and `-`, at most 63 characters).
- No DNS-SD/service discovery, no PTR/SRV/TXT records, and no unicast DNS fallback.
- Unsolicited answers are not cached. A record is cached only if it answers an active query or refreshes an existing cache entry.
- Fixed limits: 32 registered names, 64 pending lookups, 8 addresses per name, 256 cache entries, 128 queued outgoing packets, 128 records per packet and 9000 bytes per packet. A lookup gives up after 3 seconds.

## Source files

| File | Purpose |
|------|---------|
| `mdns.h` | Declarations for the codec, `Engine` and `Service` in namespace `net::mdns`. |
| `mdns.cpp` | Wire codec (`decode`, `encode`), name and address validation, and the `Engine` state machine. It has no sockets and no clock: the caller passes the time in microseconds, so it is deterministic and easy to test. |
| `mdns_socket.cpp` | `Service`: opens non-blocking UDP 5353 sockets for IPv4 (`224.0.0.251`) and optionally IPv6 (`ff02::fb`), joins the group on each multicast-capable interface, and moves packets between the sockets and the `Engine`. Supports Linux/POSIX and Windows. |
| `test_mdns.cpp` | Unit test for the codec and `Engine` (see [Tests](#tests)). |
| `fuzz_mdns.cpp` | libFuzzer harness for the codec and `Engine` (see [Fuzzing and the fixtures folder](#fuzzing-and-the-fixtures-folder)). |
| `unavailable_fixture.cpp` | Linux `LD_PRELOAD` shim that makes multicast unavailable (see [unavailable_fixture.cpp](#unavailable_fixturecpp)). |
| `fixtures/` | Seed packets for the fuzzer, also replayed as a regression test. |

### Engine behaviour

The `Engine` does no I/O. It accepts packets through `receive()`, advances timers through `tick()`, and queues outgoing packets for `take_output()`. Finished lookups are collected with `take_results()`.

- **Registration**: the engine waits 0–250 ms (derived from the name), sends three probes 250 ms apart with the proposed record in the authority section, and then announces. It announces again 1 second later and then every 60 seconds. Calling `withdraw()` sends goodbye packets (TTL 0) for every name that has reached `READY`.
- **Conflicts**: a conflicting record received during probing moves the name to `FAILED`. Once a name is `READY`, the engine answers the first conflict by re-announcing. A second conflict within 1 second sends a goodbye and moves the name to `FAILED`.
- **Responding**: the engine answers queries for its names at most once per second per name. It suppresses the answer when the query already lists the same record with a TTL of at least 60 (known-answer suppression).
- **Resolution**: queries go out immediately, again after 0.5 s, and then every second. A lookup completes 100 ms after its first answer, so that answers from other interfaces can arrive, or after 3 seconds with an empty result. Results are scoped: an answer counts only if it arrives on an interface and address family the lookup asked for. Cached TTLs are capped at 120 s, and the cache-flush bit expires other cached addresses for the same name and scope after 1 s.
- **Input filtering**: `receive()` drops packets that do not come from source port 5353 with a hop limit/TTL of 255, which shows they came from the local link. `Service::tick()` also drops packets that were not sent to the multicast group or that arrived on an interface it did not join. `decode()` rejects packets with a non-zero ID, a non-zero opcode, the truncation bit set, a non-zero RCODE, trailing bytes, or compression pointers that loop.

### Service usage

`Service` runs on the caller's thread. The owner polls it; it never blocks:

```cpp
net::mdns::Service mdns;
if (mdns.open(true)) {                       // true = IPv6 as well as IPv4
   mdns.register_address("0123abcd.local", address, now_us);
   mdns.resolve("peer.local", now_us);
}
// Later, from the owner's timer or event loop:
mdns.tick(now_us);                            // Read at most 64 packets per socket, run timers, send output
for (auto &result : mdns.engine.take_results()) { ... }
for (auto &reg : mdns.engine.registrations()) { ... }   // PROBING, READY or FAILED
```

`register_address()` only accepts an address that belongs to one of the interfaces found by `open()`. The destructor withdraws all names, sending goodbye packets.

## Tests

| CTest name | Built when | What it runs |
|------------|------------|--------------|
| `network_mdns` | `UNIT_TESTS=ON` | `test_mdns.cpp`: name validation, codec round trips (including compressed names, truncated packets and pointer loops), probing, known-answer suppression, goodbyes, scoped resolution and cache expiry, the answer and queue limits, conflict handling, and cancellation through `withdraw()`. |
| `network_mdns_fuzz_regression` | `NETWORK_MDNS_FUZZ=ON` and `BUILD_TESTS=ON`, Clang only | Replays each file in `fixtures/` through the fuzz harness once, with ASan and UBSan enabled. |
| `webrtc_mdns_unavailable` | `UNIT_TESTS=ON` on Linux | Flute test `src/webrtc/tests/test_mdns_unavailable.tiri`, run with `unavailable_fixture.cpp` preloaded. |

The WebRTC module has further mDNS coverage: the `webrtc_mdns_privacy` test, and the `webrtc_mdns_interop` and `webrtc_mdns_channels_interop` tests, which run against Chromium.

Run the unit test with:

```bash
ctest --preset agents -R '^network_mdns$'
```

When the test fails, it prints `mDNS failed at N`. `N` is the step number passed to `fail()` in `test_mdns.cpp`.

## Fuzzing and the fixtures folder

`fuzz_mdns.cpp` is a libFuzzer entry point. For each input it:

1. decodes the input. If decoding succeeds, it re-encodes the message and decodes the result again (a round trip);
2. creates an `Engine`, registers `fuzz.local` at `192.0.2.1` on interface 2, and starts a lookup for `fuzz.local` on that interface;
3. feeds the input to `receive()` eight times, 0.5 s apart. Each time it alternates the receiving interface between 2 and 3 and the hop limit between 255 and 254, so the harness reaches both the accepted and rejected paths. It calls `tick()` and drains the output after each one;
4. calls `withdraw()`, then feeds the input once more and ticks again, to check that a cancelled engine stays inert.

Each file in `fixtures/` is one raw DNS message: the UDP payload exactly as it appears on port 5353, with no framing or header. They serve two purposes:

- a **seed corpus**, so the fuzzer starts from valid mDNS packets that match the names and addresses the harness uses, instead of random bytes; and
- a **regression suite**: the `network_mdns_fuzz_regression` test runs every file once (`-runs=1`), so a past crash fixed by adding its input here stays fixed.

All fixtures use the name `fuzz.local` and the documentation address `192.0.2.2`. This deliberately differs from the harness's own registration (`192.0.2.1`), so packets carrying `192.0.2.2` also reach the conflict-detection code.

| File | Bytes | Message | Code it exercises |
|------|-------|---------|-------------------|
| `query.dns` | 28 | Query: one question, `fuzz.local` type ANY (255), class IN. | The responder path for a registered name, plus the per-name reply rate limit. |
| `probe.dns` | 54 | Probe: the same question, plus an authority-section A record `fuzz.local → 192.0.2.2`, TTL 120. | Conflict detection while probing: a competing probe for our name moves the registration to `FAILED`. |
| `address.dns` | 38 | Response (QR+AA): one answer, A `fuzz.local → 192.0.2.2`, cache-flush set, TTL 120. | Delivers an answer to the pending lookup and fills the cache. Also conflicts with the registration. |
| `goodbye.dns` | 38 | Same as `address.dns`, but with TTL 0. | Goodbye handling: removes an address from the lookup's answers and cache. |
| `pointer_loop.dns` | 28 | Response whose answer owner name is a compression pointer `c0 0c` to offset 12, i.e. to itself. | The decoder's loop detection. The packet must be rejected without hanging or reading out of bounds. |

To inspect a fixture:

```bash
od -An -tx1 -v src/network/mdns/fixtures/probe.dns
```

### Running the fuzzer

The fuzz target needs Clang:

```bash
cmake -S . -B build/fuzz -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
   -DNETWORK_MDNS_FUZZ=ON
cmake --build build/fuzz --target fuzz_network_mdns --parallel
mkdir -p build/fuzz/mdns_corpus
build/fuzz/src/network/fuzz_network_mdns build/fuzz/mdns_corpus src/network/mdns/fixtures
```

Give the fuzzer a scratch corpus directory as its first argument, as above. libFuzzer writes new inputs to the first directory it is given, and the fixtures folder should only hold hand-picked cases.

### Adding a fixture

When the fuzzer finds a crash (`crash-<sha1>`), or you want to lock in a specific edge case:

1. Shrink the input with `-minimize_crash=1` where possible.
2. Copy it to `fixtures/` with a short, descriptive lower snake-case name and the `.dns` extension.
3. Add a row for it to the table above.
4. Prefer hand-built packets based on `fuzz.local` and `192.0.2.x`, so that the harness's registration and lookup actually see them. For example, Python's `bytes.fromhex(...)` written to a file is enough.

## unavailable_fixture.cpp

This file is a test fixture, not part of the library. It builds as the shared library `webrtc_mdns_unavailable` (Linux only). It overrides `setsockopt()` so that `IP_PKTINFO` and `IPV6_RECVPKTINFO` fail with `EOPNOTSUPP`, and passes every other option to the real implementation. Without packet info, `Service::open()` cannot create a scoped multicast socket, so mDNS is unavailable while ordinary UDP sockets still work.

The `webrtc_mdns_unavailable` Flute test runs with this library preloaded (`LD_PRELOAD`). It checks that when mDNS cannot be used, a `PeerConnection` fails explicitly, and does not fall back to exposing numeric host candidates. A connection created with `NO_MDNS` must still produce numeric host candidates.
