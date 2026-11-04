# SCTP and DCEP wire fixtures

`sctp_wire.json` and `dcep_wire.json` hold synthetic SCTP packets and DCEP messages.  They were written by hand before the SCTP codec existed, so they are independent of Kōtuku's encoder.  Do not regenerate them from the codec.

Check fixture integrity from the repository root with:

```bash
build/agents-install/origo src/webrtc/tests/verify_wire_fixtures.tiri --log-warning
```

The verifier checks CRCs, header annotations, positive chunk bounds and DCEP decoding.  It does not test the codec.

## Consumers

- `test_sctp_packet.cpp` checks the CRC known answers and parses every packet.  It also re-encodes INIT
  byte-for-byte and confirms that single-bit damage and truncation are rejected.
- `tests/interop/prepare_fuzz_corpus.tiri` writes each SCTP packet and DCEP message into the `fuzz_sctp_codec` seed
  corpus.  This happens in the build tree when `WEBRTC_FUZZ` is enabled.

## SCTP packets

Integer fields use network byte order, except for the checksum.  All packets share this association profile:

| Field | Value |
| --- | --- |
| Source port | 5000 |
| Destination port | 5001 |
| Verification tag | `0x11223344` (INIT uses zero) |

Different source and destination ports catch code that wrongly assumes they are equal.

| Packet | Contents |
| --- | --- |
| `init` | 32 MiB receive window, 512 outbound and inbound streams, initial TSN `0xfffffffe`.  No address parameters. |
| `text_hi` | DATA on stream 2, SSN 0, TSN `0xfffffffe`, B/E flags.  Payload `hi`, PPID 51, two padding bytes. |
| `empty_binary` | As `text_hi`, but with PPID 57, a one-byte filler and three padding bytes. |
| `cookie_ack` | A four-byte control chunk. |

Payload and chunk lengths exclude padding.

### CRC32C

The checksum covers the whole packet, with bytes 8–11 set to zero during calculation.  It is stored with the least
significant byte first.  The algorithm uses polynomial `0x1edc6f41` (reflected `0x82f63b78`), an initial remainder
of `0xffffffff` and a final complement.

The `crc32c` known answers cover:

- empty input
- `123456789`
- 32 zero bytes
- 32 `0xff` bytes
- the byte sequence 0–31

[RFC 3720 Appendix B.4](https://www.rfc-editor.org/rfc/rfc3720#appendix-B.4) publishes the last three.  Packet
checksums came from a reflected bitwise reference implementation.  They were cross-checked with MSB-first long
division, which was first validated against the known answers.

### Negative packets

Each negative packet breaks exactly one rule.  All of them have correct CRCs, except `checksum_corruption`, so a
test reaches the rule it is aimed at.

| Packet | Rule | Level |
| --- | --- | --- |
| `short_chunk_length` | Chunk length below the 4-byte header | Framing |
| `chunk_length_past_end` | Chunk extends beyond the packet | Framing |
| `checksum_corruption` | Payload changed without updating the CRC | Framing |
| `wrong_source_port` | Source port does not match the association | Association |
| `wrong_tag` | Verification tag does not match the association | Association |

`valid: false` covers association mismatches as well as framing errors.  `parse()` only rejects framing errors, so
`test_sctp_packet.cpp` expects the two association packets to parse successfully.  No test yet uses these two
fixtures to check that an association rejects them without changing state.

## DCEP messages

DCEP messages are SCTP user payloads with no DCEP-level padding.  Use PPID 50 when sending them through the
transport.

`dcep_wire.json` contains one DATA_CHANNEL_OPEN message for each of the six channel types.  Every OPEN uses
priority 256, label `chat` and an empty protocol.  The reliability parameter is 0 for reliable and
retransmission-limited channels, and 1000 ms for timed channels.  `ack` is the single byte `0x02`.

The negative messages cover:

| Message | Defect |
| --- | --- |
| `truncated_open` | Fixed header is too short |
| `label_past_end` | Label length exceeds the message |
| `reserved_channel_type` | Channel type is reserved |
| `invalid_label_utf8` | Label is not valid UTF-8 |

## Related fixtures

- `chromium_154_sctp_client.json` and `chromium_154_sctp_server.json` are live Chromium captures.  Their provenance
  is recorded in each file, and `test_sctp_packet.cpp` parses them.
- `fuzz_sctp_state/` holds operation sequences for the SCTP state fuzzer.

Keep synthetic fixtures, captured packets and acceptance results apart, so that each one's source stays clear.  When
a defect turns up malformed input, add it as a new seed.
