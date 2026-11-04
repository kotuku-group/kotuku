// Bounded SCTP wire codec (RFC 9260).  All views borrow the input packet.

#pragma once

#include <kotuku/config.h>
#include <cstdint>
#include <span>
#include <vector>

namespace rtc::sctp {

constexpr size_t HEADER_SIZE = 12;
constexpr size_t MAX_PACKET_SIZE = 65536;
constexpr size_t MAX_ITEMS = 256; // Maximum number of chunks in a packet, or parameters in a chunk

enum ChunkType : uint8_t {
   DATA = 0, INIT = 1, INIT_ACK = 2, SACK = 3, HEARTBEAT = 4, HEARTBEAT_ACK = 5,
   ABORT = 6, SHUTDOWN = 7, SHUTDOWN_ACK = 8, ERROR_CHUNK = 9, COOKIE_ECHO = 10,
   COOKIE_ACK = 11, SHUTDOWN_COMPLETE = 14, RE_CONFIG = 130, FORWARD_TSN = 192, PADDING = 132
};

enum ParameterType : uint16_t { STATE_COOKIE = 7, UNRECOGNISED = 8, EXTENSIONS = 0x8008, PR_SUPPORTED = 0xc000 };

struct Item { // A chunk or parameter
   uint16_t type = 0;
   uint8_t flags = 0;
   std::span<const uint8_t> value;
   std::span<const uint8_t> wire; // Header and value, without outer padding
};

struct Packet {
   uint16_t source_port = 0;
   uint16_t destination_port = 0;
   uint32_t verification_tag = 0;
   std::vector<Item> chunks;
};

// The upper two bits of an unknown chunk or parameter type determine whether processing stops and whether the item
// is reported to the peer (RFC 9260 §3.2, §3.2.1).

struct UnknownAction { bool stop; bool report; };

constexpr UnknownAction unknown_chunk(uint8_t Type) { return { not bool(Type & 0x80), bool(Type & 0x40) }; }
constexpr UnknownAction unknown_parameter(uint16_t Type) { return { not bool(Type & 0x8000), bool(Type & 0x4000) }; }

uint16_t read16(std::span<const uint8_t> Data, size_t Offset);
uint32_t read32(std::span<const uint8_t> Data, size_t Offset);
uint64_t read64(std::span<const uint8_t> Data, size_t Offset);
void put16(std::vector<uint8_t> &Data, uint16_t Value);
void put32(std::vector<uint8_t> &Data, uint32_t Value);
void put64(std::vector<uint8_t> &Data, uint64_t Value);
uint32_t crc32c(std::span<const uint8_t> Data);
uint32_t packet_crc(std::span<const uint8_t> Data);

// Counts chunk headers without validating the checksum, returning at least one for budgeting.
size_t chunk_count(std::span<const uint8_t> Data);

bool parse(std::span<const uint8_t> Data, Packet &Result);
bool parameters(std::span<const uint8_t> Data, std::vector<Item> &Result);
bool append_parameter(std::vector<uint8_t> &Data, uint16_t Type, std::span<const uint8_t> Value);

class Encoder {
public:
   Encoder(uint16_t Source, uint16_t Destination, uint32_t Tag, size_t Capacity);
   bool chunk(uint8_t Type, uint8_t Flags, std::span<const uint8_t> Value);
   std::vector<uint8_t> finish();

private:
   std::vector<uint8_t> bytes;
   size_t capacity;
};

} // namespace rtc::sctp
