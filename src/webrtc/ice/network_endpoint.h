// Conversions between Network module addresses and ICE endpoints, shared by the socket adapters.  Only adapters that
// link the Network module may include this header; protocol layers use ice::Endpoint alone.

#pragma once

#include "ice.h"

#include <kotuku/modules/network.h>

#include <algorithm>
#include <cstring>

namespace rtc {

//********************************************************************************************************************
// Converts an IPv4-mapped IPv6 endpoint (::ffff:a.b.c.d) to IPv4.  Other endpoints are returned unchanged.

inline ice::Endpoint unmap_ipv4(ice::Endpoint Value)
{
   if (not Value.ipv6 or not std::all_of(Value.address.begin(), Value.address.begin() + 10,
       [](uint8_t Byte) { return Byte IS 0; }) or Value.address[10] != 0xff or Value.address[11] != 0xff) {
      return Value;
   }

   std::array<uint8_t, 4> ipv4{};
   std::copy_n(Value.address.begin() + 12, 4, ipv4.begin());
   Value.address = {};
   std::copy(ipv4.begin(), ipv4.end(), Value.address.begin());
   Value.ipv6 = false;
   return Value;
}

//********************************************************************************************************************
// Converts a Network module address to an ICE endpoint.  IPv4-mapped IPv6 addresses are converted to IPv4, so that
// dual-stack sockets report the same endpoint as the candidate that was signalled.  The Network module stores IPv4
// addresses in host byte order, while ICE endpoints hold network byte order.

inline ice::Endpoint from_network(const IPAddress &Source)
{
   ice::Endpoint result;
   result.port = uint16_t(Source.Port);
   result.ipv6 = Source.Type IS IPADDR::V6;

   if (result.ipv6) {
      std::memcpy(result.address.data(), Source.Data, 16);
      return unmap_ipv4(result);
   }

   result.address[0] = uint8_t(Source.Data[0] >> 24);
   result.address[1] = uint8_t(Source.Data[0] >> 16);
   result.address[2] = uint8_t(Source.Data[0] >> 8);
   result.address[3] = uint8_t(Source.Data[0]);
   return result;
}

//********************************************************************************************************************
// Converts an ICE endpoint to a Network module address.  The route identifier is local-only and is not converted.

inline IPAddress to_network(const ice::Endpoint &Destination)
{
   IPAddress result{};
   result.Type = Destination.ipv6 ? IPADDR::V6 : IPADDR::V4;
   result.Port = Destination.port;

   if (Destination.ipv6) std::memcpy(result.Data, Destination.address.data(), 16);
   else result.Data[0] = (uint32_t(Destination.address[0]) << 24) | (uint32_t(Destination.address[1]) << 16) |
      (uint32_t(Destination.address[2]) << 8) | Destination.address[3];

   return result;
}

} // namespace rtc
