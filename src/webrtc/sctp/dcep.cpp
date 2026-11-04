// Data Channel Establishment Protocol (RFC 8832) and application payload handling (RFC 8831).

#include "dcep.h"
#include "sctp_packet.h"
#include "../../link/unicode.h"

namespace rtc {

//********************************************************************************************************************
// Partial reliability is expressed as either a retransmission limit or a lifetime, never both.  DCEP encodes either
// value in a 32-bit field, but the module restricts them to 16 bits for consistency with the browser APIs.

ERR SctpReliability::validate() const
{
   if (max_retransmits and max_lifetime_ms) return ERR::InvalidValue;

   if ((max_retransmits and *max_retransmits > 65535) or
       (max_lifetime_ms and *max_lifetime_ms > 65535)) return ERR::OutOfRange;

   return ERR::Okay;
}

namespace dcep {

//********************************************************************************************************************
// Returns true if Bytes is well-formed UTF-8.  Embedded U+0000 characters are permitted.

bool utf8(std::span<const uint8_t> Bytes)
{
   for (size_t i = 0; i < Bytes.size();) {
      int count;
      auto value = UTF8ReadValue(std::string_view((const char *)Bytes.data() + i, Bytes.size() - i), &count);

      // The shared decoder returns zero for both U+0000 and invalid sequences.

      if (value IS 0 and Bytes[i] != 0) return false;
      i += size_t(count);
   }

   return true;
}

//********************************************************************************************************************
// Validates channel options before they are encoded or after they have been decoded from a DATA_CHANNEL_OPEN.

ERR validate(const SctpChannelOptions &Options)
{
   auto error = Options.reliability.validate();
   if (error != ERR::Okay) return error;

   if (Options.label.size() > 65535 or Options.protocol.size() > 65535 or
       (Options.id and *Options.id IS 65535)) return ERR::OutOfRange;

   if (Options.negotiated and not Options.id) return ERR::InvalidValue;

   if (not utf8({(const uint8_t *)Options.label.data(), Options.label.size()}) or
       not utf8({(const uint8_t *)Options.protocol.data(), Options.protocol.size()})) return ERR::InvalidData;

   return ERR::Okay;
}

//********************************************************************************************************************
// Encodes a DATA_CHANNEL_OPEN message.  The channel type combines the unordered flag (0x80) with the reliability
// mode: 0 for reliable, 1 for a retransmission limit and 2 for a lifetime limit.

ERR encode_open(const SctpChannelOptions &Options, std::vector<uint8_t> &Wire)
{
   auto error = validate(Options);
   if (error != ERR::Okay) return error;

   std::vector<uint8_t> result{3, uint8_t((Options.ordered ? 0 : 0x80) |
      (Options.reliability.max_retransmits ? 1 : Options.reliability.max_lifetime_ms ? 2 : 0))};

   sctp::put16(result, Options.priority);
   sctp::put32(result, Options.reliability.max_retransmits.value_or(
      Options.reliability.max_lifetime_ms.value_or(0)));
   sctp::put16(result, uint16_t(Options.label.size()));
   sctp::put16(result, uint16_t(Options.protocol.size()));
   result.insert(result.end(), Options.label.begin(), Options.label.end());
   result.insert(result.end(), Options.protocol.begin(), Options.protocol.end());

   Wire = std::move(result);
   return ERR::Okay;
}

//********************************************************************************************************************
// Decodes a DATA_CHANNEL_OPEN message.  Options are only updated if the message is entirely valid.

ERR decode_open(std::span<const uint8_t> Wire, SctpChannelOptions &Options)
{
   if (Wire.size() < 12 or Wire[0] != 3 or (Wire[1] & 0x7f) > 2) return ERR::InvalidData;

   auto label = sctp::read16(Wire, 8);
   auto protocol = sctp::read16(Wire, 10);
   if (Wire.size() != 12u + size_t(label) + protocol) return ERR::InvalidData;

   SctpChannelOptions result;
   result.ordered = not (Wire[1] & 0x80);
   result.priority = sctp::read16(Wire, 2);

   auto reliability = sctp::read32(Wire, 4);
   if ((Wire[1] & 0x7f) IS 1) result.reliability.max_retransmits = reliability;
   if ((Wire[1] & 0x7f) IS 2) result.reliability.max_lifetime_ms = reliability;

   result.label.assign((const char *)Wire.data() + 12, label);
   result.protocol.assign((const char *)Wire.data() + 12 + label, protocol);

   auto error = validate(result);
   if (error != ERR::Okay) return error;

   Options = std::move(result);
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns true if Wire is a DATA_CHANNEL_ACK message.

bool ack(std::span<const uint8_t> Wire)
{
   return Wire.size() IS 1 and Wire[0] IS 2;
}

//********************************************************************************************************************
// Converts a received payload to its application form according to the PPID.  Empty messages are transmitted as a
// single filler byte with a dedicated PPID, which is removed here.

ERR decode_payload(uint32_t PPID, std::vector<uint8_t> &Payload, bool &Text)
{
   Text = PPID IS PPID_TEXT or PPID IS PPID_EMPTY_TEXT;

   if (PPID IS PPID_EMPTY_TEXT or PPID IS PPID_EMPTY_BINARY) {
      // The filler byte carries no application data; its value is intentionally ignored.
      if (Payload.size() != 1) return ERR::InvalidData;
      Payload.clear();
   }
   else if (PPID != PPID_TEXT and PPID != PPID_BINARY) return ERR::NoSupport;

   if (Text and not utf8(Payload)) return ERR::InvalidData;
   return ERR::Okay;
}

} // namespace dcep
} // namespace rtc
