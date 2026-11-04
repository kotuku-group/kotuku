This folder contains the source code for the WebRTC module.

Layout:

* `webrtc.cpp` declares the extended classes (`extPeerConnection`, `extWebChannel`), module globals and init/expunge,
  then includes `webchannel/webchannel.cpp` and `peerconnection/peerconnection.cpp` as one translation unit.
* `peerconnection/` and `webchannel/` contain the public classes.  `peerconnection/peer_engine.cpp` is the
  object-independent `rtc::PeerConnection` state machine that the PeerConnection class drives from a timer.
* Protocol layers are compiled as separate sources: `stun/`, `precis/` (credential preparation), `ice/` (agent,
  candidates, sockets, interface monitoring), `turn/`, `dtls/`, `sctp/` (including DCEP) and `sdp/`.
* `tests/` holds Flute and native unit tests; `tests/interop/` holds browser, coturn and platform fixtures, which are
  registered by `tests/interop/interop.cmake`.

Points to remember:

* Codec and state-machine code (STUN, PRECIS, SDP, SCTP, DTLS, the ICE agent and TURN client) uses framework types
  such as `ERR` but no objects or Core calls, so that unit tests and fuzz targets can compile a single layer without
  the framework.  Only the socket adapters (`ice/ice_socket`, `turn/turn_socket`) and `peer_engine` create Network
  objects; their unit tests are registered as `NATIVE` and link Core.
* CMake defines a source list for each layer (`WEBRTC_STUN_SOURCES`, `WEBRTC_SCTP_SOURCES` and so on).  Add new files
  to the matching list rather than to individual test targets.
* Use relative includes between folders, e.g. `#include "../stun/stun.h"`.
* The module IDL is generated into `module_def.c`, and class definitions into each class folder's `*_def.c`.  Run the
  `build_headers` target after changing `webrtc.tdl` or embedded class documentation.
* All protocol state runs on the thread that owns the PeerConnection.  The module uses OpenSSL for DTLS on every
  platform, including Windows, where the Network module uses Schannel for TLS.
