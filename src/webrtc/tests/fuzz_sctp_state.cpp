#include "../sctp/sctp_association.h"
#include "../sctp/dcep.h"
#include <algorithm>

namespace {
rtc::SctpConfig config(uint16_t Local, uint16_t Remote) {
   rtc::SctpConfig result;
   result.local_port = Local; result.remote_port = Remote;
   result.receive_limit = 4096; result.receive_window = 8192;
   result.initial_rto_us = 10000; result.min_rto_us = 1000; result.max_rto_us = 80000;
   result.random = [Local](std::span<uint8_t> Bytes) {
      std::fill(Bytes.begin(), Bytes.end(), uint8_t(Local)); return true;
   };
   return result;
}
}
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
   if (Size > 4096) return 0;
   rtc::SctpAssociation a(config(5000, 5001)), b(config(5001, 5000));
   uint64_t now = 0;
   (void)a.start(now);
   for (unsigned i = 0; i < 8; ++i) {
      for (auto &wire : a.take_output()) { a.output_accepted(wire); (void)b.feed(wire, now); }
      for (auto &wire : b.take_output()) { b.output_accepted(wire); (void)a.feed(wire, now); }
   }
   rtc::SctpChannels ca(a, true), cb(b, false);
   // At most 128 operations, 4 KiB messages, 8 KiB receive credit and 64 output packets per step.
   for (size_t offset = 0; offset < Size and offset < 128; ++offset) {
      uint8_t operation = Data[offset];
      uint16_t id = uint16_t((operation >> 4) * 2);
      auto payload = std::span<const uint8_t>(Data + offset, std::min(Size - offset, size_t(4096)));
      switch (operation % 8) {
         case 0: {
            rtc::SctpChannelOptions options;
            options.label = "fuzz"; options.negotiated = not bool(operation & 128);
            if (options.negotiated) options.id = id;
            options.ordered = not bool(operation & 8);
            // Policy values are independent of the opcode's three low bits.  Consume an operand only
            // for partial reliability; a truncated operand retains the useful zero-limit case.
            if (operation & 96) {
               uint32_t limit = offset + 1 < Size ? Data[++offset] : 0;
               if (operation & 32) options.reliability.max_retransmits = limit;
               else options.reliability.max_lifetime_ms = limit;
            }
            uint16_t assigned = 0;
            (void)ca.open(options, now, assigned);
            if (options.negotiated) (void)cb.open(options, now, assigned);
            break;
         }
         case 1: (void)ca.send(id, payload, bool(operation & 8), now); break;
         case 2: (void)cb.close(id, now); break;
         case 3: {
            (void)a.feed(payload, now);
            // Repair the envelope so mutations reach chunk handlers behind CRC/tag validation.
            rtc::sctp::Encoder encoder(5001, 5000, a.tag(), 8192);
            uint8_t type = offset + 1 < Size ? Data[offset + 1] : 0;
            (void)encoder.chunk(type, operation >> 4, payload);
            auto wire = encoder.finish();
            (void)a.feed(wire, now);
            break;
         }
         case 4: (void)a.path_changed(128 + operation * 4, now); break;
         case 5: (void)b.reset_stream(id, now); break;
         case 6: (void)a.shutdown(now); break;
         case 7: (void)b.enqueue(id, 50, payload, false, now); break;
      }
      now += 1000 + uint64_t(operation) * 1000;
      (void)a.tick(now); (void)b.tick(now);
      for (auto &wire : a.take_output()) {
         a.output_accepted(wire);
         if (operation & 8) continue; // deterministic packet loss
         if ((operation & 64) and wire.size() > 16) {
            wire[13] ^= operation;
            uint32_t crc = rtc::sctp::packet_crc(wire);
            for (unsigned i = 0; i < 4; ++i) wire[8 + i] = uint8_t(crc >> (8 * i));
         }
         (void)b.feed(wire, now);
         if (operation & 32) (void)b.feed(wire, now);
      }
      for (auto &wire : b.take_output()) {
         b.output_accepted(wire);
         if (not (operation & 16)) (void)a.feed(wire, now);
      }
      (void)ca.pump(now); (void)cb.pump(now);
      if (not (operation & 128)) { (void)ca.take_events(); (void)cb.take_events(); }
      (void)a.take_events(); (void)b.take_events();
   }
   return 0;
}
