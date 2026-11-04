// Software accounting at accepted datagram and TURN adapter boundaries.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace rtc {

// Adds Amount to Value, saturating at the maximum.

inline void add_counter(uint64_t &Value, uint64_t Amount) {
   Value += std::min(Amount, std::numeric_limits<uint64_t>::max() - Value);
}

struct TransportCounters {
   uint64_t sent_bytes = 0, sent_packets = 0, received_bytes = 0, received_packets = 0;
   uint64_t turn_sent_bytes = 0, turn_received_bytes = 0;

   void sent(size_t Bytes) {
      add_counter(sent_bytes, Bytes);
      add_counter(sent_packets, 1);
   }

   void received(size_t Bytes) {
      add_counter(received_bytes, Bytes);
      add_counter(received_packets, 1);
   }

   // Counters are serialised as strings so that 64-bit values survive JSON number precision.

   std::string json() const {
      return "{\"sentBytes\":\"" + std::to_string(sent_bytes) + "\",\"sentPackets\":\"" +
         std::to_string(sent_packets) + "\",\"receivedBytes\":\"" + std::to_string(received_bytes) +
         "\",\"receivedPackets\":\"" + std::to_string(received_packets) + "\",\"turnSentBytes\":\"" +
         std::to_string(turn_sent_bytes) + "\",\"turnReceivedBytes\":\"" +
         std::to_string(turn_received_bytes) + "\"}";
   }
};

} // namespace rtc
