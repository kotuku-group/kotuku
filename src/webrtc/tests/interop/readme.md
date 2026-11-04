# WebRTC interoperability fixtures

These helpers drive browser interoperability, temporary TURN servers, and platform network tests.  Test registration and prerequisites are defined in [`interop.cmake`](interop.cmake), which is included by [`../../CMakeLists.txt`](../../CMakeLists.txt).  Run registered tests from a configured Debug build after building and installing it.

## Browser tests

Install the pinned Playwright dependency with `npm ci --prefix src/webrtc/tests/interop`.  Configure CMake with `WEBRTC_INTEROP_NODE_PATH` pointing to this directory's absolute `node_modules` path and `WEBRTC_CHROME_EXECUTABLE` pointing to Chromium.  `WEBRTC_REQUIRE_INTEROP=ON` makes missing browser prerequisites a configure error.

| Driver | Peer or signalling implementation | Coverage |
| --- | --- | --- |
| `native_sctp.cjs` | `webrtc_native_sctp_probe` | Native SCTP channels with both DTLS roles |
| `public_channels.cjs` | [`examples/webrtc/signalling.tiri`](../../../../examples/webrtc/signalling.tiri) and its adjacent `index.html` | Public channels, buffering, closure and browser demo; numeric and mDNS candidates |
| `public_mdns.cjs` | `mdns_signalling.tiri` | Obfuscated host candidates and ICE restart |
| `public_turn.cjs` | `turn_signalling.tiri` | Relay-only channels and ICE restart; launched by the TURN fixture |

```sh
ctest --preset agents -L webrtc_interop
ctest --preset agents -L webrtc_mdns_interop
```

## TURN and stream tests

`run_turn_fixture.py` creates an ephemeral coturn server, certificates and credentials.  It exercises UDP, TCP and TLS, with optional native and browser relay checks.  Configure `WEBRTC_TURN_SERVER` and `WEBRTC_TURN_UCLIENT` with the coturn executable paths; the registered fixture also requires `BUILD_TESTS`, `UNIT_TESTS`, and the OpenSSL CLI.  `WEBRTC_REQUIRE_TURN=ON` additionally requires the browser prerequisites.

```sh
ctest --preset agents -L webrtc_turn_integration
python3 src/webrtc/tests/interop/run_turn_fixture.py --help
```

`test_network_stream.tiri` is a Flute test supplied with a listening server by `run_turn_fixture.py` or `test_linux_tls_shutdown.py`.  It checks split writes, STUN responses, TLS rejection and remote shutdown.  The TLS shutdown helper repeats the exchange against its own loopback TLS server.

## Platform fixtures

- `run_isolated_relay.py`: blocked-direct relay checks with nftables in disposable Linux user/network namespaces (`WEBRTC_TEST_RELAY_NAMESPACE`).
- `test_interface_notifications.tiri` and `test_linux_path_removal.py`: Linux interface notifications and IPv6 path removal (`WEBRTC_TEST_INTERFACE_NAMESPACE`).  The notification test runs through Flute and launches itself in a disposable user/network namespace.
- `test_linux_dns_close.py`: stalled DNS teardown in private Linux namespaces (`WEBRTC_TEST_DNS_NAMESPACE`).
- `test_windows_notifications.py`, `test_windows_path_removal.py` and `test_windows_dns_close.py`: opt-in Windows topology and DNS tests (`WEBRTC_TEST_WINDOWS_NOTIFICATIONS`).  Read each helper's prerequisites before running; these fixtures require elevation and temporarily change host network configuration.
- `probe_linux_multicast.py`: standalone Linux diagnostic for shared multicast sockets, packet metadata and netlink enumeration.  It takes `--interface` and `--ipv4`; it is not registered with CTest.

The Linux namespace fixtures need the corresponding host utilities and permission to create user/network namespaces.  CMake leaves these topology fixtures disabled by default.

The notification test can also be run directly through Flute:

```sh
build/agents-install/origo tools/flute.tiri file=src/webrtc/tests/interop/test_interface_notifications.tiri monitor=/absolute/path/to/test_webrtc_interface_monitor --log-warning --gfx-driver=headless
```

The remaining Python fixtures retain their native socket, TLS-server and platform coordination implementations.  The multicast probe needs ancillary packet metadata and raw netlink access; the TURN, DNS and relay fixtures provide independent socket peers and negative controls.  The path-removal and Windows fixtures remain Python pending equivalent validation of their address checks, process coordination and host-network cleanup.

## Fuzzing helpers

`prepare_fuzz_corpus.tiri` prepares SCTP and TURN codec corpora from the canonical fixtures in `../fixtures`; CMake invokes it for fuzz builds.  Fuzz configuration now requires an installed Origo executable, located through `ORIGO_CMD_PATH`.  `run_fuzz_campaigns.tiri` copies seeds into the build tree and runs bounded STUN, SDP, SCTP, TURN and Network mDNS campaigns.  Build the corresponding fuzz targets first.  Logs, writable corpora and `campaigns.json` are stored under `<build-tree>/evidence`.  A failed campaign stops the runner with a nonzero status; the exact child exit code is recorded in `campaigns.json`.

```sh
build/agents-install/origo src/webrtc/tests/interop/prepare_fuzz_corpus.tiri name=sctp_codec destination=build/corpus_sctp --log-warning
build/agents-install/origo src/webrtc/tests/interop/run_fuzz_campaigns.tiri build=build/fuzz runs=20000 seed=8656 --log-warning
```

Browser and topology reports belong in the build tree.  Python bytecode and local npm dependencies are ignored in this directory.
