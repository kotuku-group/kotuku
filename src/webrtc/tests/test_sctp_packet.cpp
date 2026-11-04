#include "../sctp/sctp_packet.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace rtc::sctp;
namespace {
int glFailures = 0;
#define CHECK(Condition) do { if (not (Condition)) { \
   std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #Condition); ++glFailures; } } while (false)

std::vector<uint8_t> hex(std::string_view Text) {
   std::vector<uint8_t> result;
   auto digit = [](char Value) { return Value <= '9' ? Value - '0' : Value - 'a' + 10; };
   for (size_t i = 0; i < Text.size(); i += 2) result.push_back(uint8_t(digit(Text[i]) * 16 + digit(Text[i + 1])));
   return result;
}
void checksum(std::vector<uint8_t> &Wire) {
   auto crc = packet_crc(Wire);
   for (size_t i = 0; i < 4; ++i) Wire[8 + i] = uint8_t(crc >> (i * 8));
}
void vectors() {
   CHECK(crc32c({}) IS 0);
   CHECK(crc32c(hex("313233343536373839")) IS 0xe3069283u);
   std::array<uint8_t, 32> bytes{};
   CHECK(crc32c(bytes) IS 0x8a9136aau);
   bytes.fill(255);
   CHECK(crc32c(bytes) IS 0x62a8ab43u);
   for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i);
   CHECK(crc32c(bytes) IS 0x46dd794eu);
   // Consume the independently prepared fixture, rather than round-tripping the implementation.
   std::ifstream file(std::string(WEBRTC_FIXTURE_DIR) + "/sctp_wire.json");
   std::string text(std::istreambuf_iterator<char>{file}, {});
   CHECK(not text.empty());
   auto name = text.find("\"name\": \"init\"");
   auto begin = text.find("\"hex\": \"", name);
   CHECK(begin != std::string::npos);
   if (begin IS std::string::npos) return;
   begin += 8;
   auto wire = hex(std::string_view(text).substr(begin, text.find('"', begin) - begin));
   Packet parsed;
   size_t cursor = text.find("\"packets\"");
   while ((cursor = text.find("\"name\": \"", cursor)) != std::string::npos) {
      size_t start = cursor + 9;
      auto fixture_name = text.substr(start, text.find('"', start) - start);
      auto hex_start = text.find("\"hex\": \"", cursor) + 8;
      auto bytes = hex(std::string_view(text).substr(hex_start, text.find('"', hex_start) - hex_start));
      bool valid = fixture_name != "short_chunk_length" and fixture_name != "chunk_length_past_end" and
         fixture_name != "checksum_corruption";
      CHECK(parse(bytes, parsed) IS valid);
      cursor = hex_start;
   }
   CHECK(parse(wire, parsed));
   CHECK(parsed.source_port IS 5000 and parsed.destination_port IS 5001 and parsed.verification_tag IS 0);
   CHECK(parsed.chunks.size() IS 1 and parsed.chunks[0].type IS INIT);
   Encoder encoder(5000, 5001, 0, 1100);
   CHECK(encoder.chunk(INIT, 0, parsed.chunks[0].value));
   CHECK(encoder.finish() IS wire);
   for (size_t i = 0; i < wire.size(); ++i) {
      auto damaged = wire;
      damaged[i] ^= 1;
      CHECK(not parse(damaged, parsed));
      auto truncated = std::vector<uint8_t>(wire.begin(), wire.begin() + i);
      if (i >= HEADER_SIZE) checksum(truncated);
      CHECK(not parse(truncated, parsed));
   }
}
void browser_captures() {
   for (auto role : {"client", "server"}) {
      std::ifstream file(std::string(WEBRTC_FIXTURE_DIR) + "/chromium_154_sctp_" + role + ".json");
      std::string text(std::istreambuf_iterator<char>{file}, {});
      CHECK(not text.empty());
      size_t cursor = 0;
      unsigned count = 0;
      while ((cursor = text.find("\"hex\": \"", cursor)) != std::string::npos) {
         cursor += 8;
         auto bytes = hex(std::string_view(text).substr(cursor, text.find('"', cursor) - cursor));
         Packet parsed;
         CHECK(parse(bytes, parsed));
         CHECK((parsed.source_port IS 5000 and parsed.destination_port IS 6000) or
            (parsed.source_port IS 6000 and parsed.destination_port IS 5000));
         ++count;
      }
      CHECK(count >= 4);
   }
}
void bounds() {
   std::vector<uint8_t> value(7, 42);
   Encoder encoder(1, 2, 3, 24);
   CHECK(encoder.chunk(0x80, 0, value));
   CHECK(not encoder.chunk(COOKIE_ACK, 0, {}));
   auto wire = encoder.finish();
   Packet parsed;
   CHECK(parse(wire, parsed) and parsed.chunks[0].value.size() IS 7);
   for (size_t length = 0; length < wire.size(); ++length) {
      auto truncated = std::vector<uint8_t>(wire.begin(), wire.begin() + length);
      if (length >= HEADER_SIZE) checksum(truncated);
      CHECK(not parse(truncated, parsed));
   }
   wire.back() = 0xff; // Receivers ignore padding contents.
   checksum(wire);
   CHECK(parse(wire, parsed));
   for (uint16_t length : {uint16_t(0), uint16_t(3), uint16_t(13), uint16_t(65535)}) {
      wire[14] = uint8_t(length >> 8);
      wire[15] = uint8_t(length);
      checksum(wire);
      CHECK(not parse(wire, parsed));
   }
   std::vector<Item> params;
   CHECK(parameters(hex("8008000501"), params)); // Final padding belongs to outer chunk.
   CHECK(not parameters(hex("800800050100"), params));
   CHECK(not parameters(hex("80080003"), params));
   CHECK(not parameters(hex("80080006ff"), params));
   std::vector<uint8_t> payload;
   CHECK(append_parameter(payload, 0x8008, value));
   CHECK(payload.size() IS 11); // Final parameter padding is excluded from the containing chunk length.
   CHECK(parameters(payload, params) and params.size() IS 1 and params[0].value.size() IS 7);
   CHECK(append_parameter(payload, 0x8009, {}));
   CHECK(payload.size() IS 16 and parameters(payload, params) and params.size() IS 2);
   std::vector<uint8_t> huge(65532);
   auto before = payload;
   CHECK(not append_parameter(payload, 1, huge) and payload IS before);
   Encoder many(1, 2, 3, 2000);
   for (size_t i = 0; i <= MAX_ITEMS; ++i) CHECK(many.chunk(COOKIE_ACK, 0, {}));
   CHECK(not parse(many.finish(), parsed));
   for (uint16_t action = 0; action < 4; ++action) {
      auto chunk = unknown_chunk(uint8_t((action << 6) | 63));
      auto param = unknown_parameter(uint16_t((action << 14) | 0x123));
      CHECK(chunk.stop IS (action < 2) and chunk.report IS bool(action & 1));
      CHECK(param.stop IS chunk.stop and param.report IS chunk.report);
   }
}
}
int main() {
   vectors();
   bounds();
   browser_captures();
   return glFailures ? 1 : 0;
}
