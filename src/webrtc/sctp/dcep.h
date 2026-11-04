// RFC 8832 OPEN/ACK and RFC 8831 application payload encoding.

#pragma once

#include "sctp_streams.h"

namespace rtc::dcep {

// Payload protocol identifiers

constexpr uint32_t PPID_CONTROL      = 50;
constexpr uint32_t PPID_TEXT         = 51;
constexpr uint32_t PPID_BINARY       = 53;
constexpr uint32_t PPID_EMPTY_TEXT   = 56;
constexpr uint32_t PPID_EMPTY_BINARY = 57;

bool utf8(std::span<const uint8_t> Bytes);
ERR validate(const SctpChannelOptions &Options);
ERR encode_open(const SctpChannelOptions &Options, std::vector<uint8_t> &Wire);
ERR decode_open(std::span<const uint8_t> Wire, SctpChannelOptions &Options);
bool ack(std::span<const uint8_t> Wire);
ERR decode_payload(uint32_t PPID, std::vector<uint8_t> &Payload, bool &Text);

} // namespace rtc::dcep
