# TURN protocol fixtures

`turn_allocate_401.turn` and `turn_allocate_success.turn` are independent STUN records for a TURN Allocate
exchange with `user` / `pass`, realm `example.org` and nonce `first-nonce`.  These credentials are deterministic
unit-test inputs, not server credentials.  Transaction IDs are little-endian serials 1 and 2 followed by eight
zero bytes, matching the injected test entropy.

The authenticated success grants a 600-second UDP allocation at `192.0.2.2:50000`, with mapped address
`192.0.2.3:40000`.  XOR addresses use the STUN cookie; integrity uses MD5 long-term key derivation and HMAC-SHA1.
The Tiri fixture generator assembles wire records independently of Kōtuku's STUN/TURN codec and uses the Crypto
module for MD5 and HMAC-SHA1.

Verify committed bytes from the repository root with:

```bash
build/agents-install/origo tools/flute.tiri file=src/webrtc/tests/verify_turn_fixtures.tiri --log-warning
```

Regenerate with:

```bash
build/agents-install/origo src/webrtc/tests/verify_turn_fixtures.tiri write=true --log-warning
```

Both commands accept `fixtures=/path/to/fixtures` to use another directory.

The native test also includes hand-transcribed ChannelData and XOR-PEER-ADDRESS arrays, checking stream padding
separately from UDP datagrams.  Wire layouts follow [RFC 8656](https://www.rfc-editor.org/rfc/rfc8656.html).

`fuzz_turn_codec/` contains ChannelData, empty STUN, oversized framing and mixed-record seeds.
`fuzz_turn_state/` contains permission/binding, refresh loss, nonce/duplicate, close-with-pending-work and channel
reuse operation sequences.  The state harness establishes authenticated allocations before interpreting mutations,
then injects authenticated replies, malformed packets, loss, duplicate responses and virtual time.
