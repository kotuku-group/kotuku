#include "../sctp/sctp_packet.h"
#include "../sctp/dcep.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
   if (Size > rtc::sctp::MAX_PACKET_SIZE) return 0;
   auto bytes = std::span<const uint8_t>(Data, Size);
   rtc::sctp::Packet packet;
   if (rtc::sctp::parse(bytes, packet)) {
      rtc::sctp::Encoder encoder(packet.source_port, packet.destination_port, packet.verification_tag, 65536);
      for (auto &chunk : packet.chunks) {
         (void)encoder.chunk(uint8_t(chunk.type), chunk.flags, chunk.value);
         std::vector<rtc::sctp::Item> parameters;
         (void)rtc::sctp::parameters(chunk.value, parameters);
      }
      auto wire = encoder.finish();
      rtc::sctp::Packet again;
      if (not rtc::sctp::parse(wire, again)) __builtin_trap();
   }
   std::vector<rtc::sctp::Item> parameters;
   (void)rtc::sctp::parameters(bytes, parameters);
   rtc::SctpChannelOptions options;
   if (rtc::dcep::decode_open(bytes, options) IS ERR::Okay) {
      std::vector<uint8_t> wire;
      if (rtc::dcep::encode_open(options, wire) != ERR::Okay) __builtin_trap();
      rtc::SctpChannelOptions again;
      if (rtc::dcep::decode_open(wire, again) != ERR::Okay) __builtin_trap();
   }
   (void)rtc::dcep::ack(bytes);
   (void)rtc::dcep::utf8(bytes);
   for (uint32_t ppid : {50u, 51u, 53u, 56u, 57u, 99u}) {
      std::vector<uint8_t> payload(bytes.begin(), bytes.end());
      bool text = false;
      (void)rtc::dcep::decode_payload(ppid, payload, text);
   }
   return 0;
}
