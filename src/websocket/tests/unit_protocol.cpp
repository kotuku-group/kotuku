/*********************************************************************************************************************

Unit tests for the WebSocket protocol core: frame encoding and decoding, message reading, UTF-8 validation, close
payloads, opening handshake helpers for both roles and the send queue.  Run through MODTest() by test_unit_tests.tiri.

*********************************************************************************************************************/

#include <kotuku/main.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "../ws_frame.h"
#include "../ws_handshake.h"
#include "../ws_send_queue.h"
#include "../ws_utf8.h"

#ifdef UNIT_TESTS

namespace {

using namespace ws;

#define CHECK(Condition, Message) \
   if (not (Condition)) { Log.error("Line %d: %s", __LINE__, Message); return false; }

constexpr MaskKey RFC_MASK = { 0x37, 0xfa, 0x21, 0x3d }; // RFC 6455 section 5.7

std::vector<uint8_t> to_bytes(std::string_view Text)
{
   return std::vector<uint8_t>(Text.begin(), Text.end());
}

void append_frame(std::vector<uint8_t> &Output, bool Fin, Opcode Op, std::span<const uint8_t> Payload,
   const MaskKey *Mask = nullptr)
{
   std::vector<uint8_t> buffer(MAX_FRAME_HEADER + Payload.size());
   auto size = encode_frame(buffer, Fin, Op, Payload, Mask);
   Output.insert(Output.end(), buffer.begin(), buffer.begin() + size);
}

void append_frame(std::vector<uint8_t> &Output, bool Fin, Opcode Op, std::string_view Payload,
   const MaskKey *Mask = nullptr)
{
   auto bytes = to_bytes(Payload);
   append_frame(Output, Fin, Op, bytes, Mask);
}

struct Message {
   Opcode opcode;
   std::vector<uint8_t> data;
};

struct ReadResult {
   std::vector<Message> messages;
   std::vector<Message> controls;
   std::vector<int> close_codes;
   std::string close_reason;
   std::string_view error_reason;
   int error_code = 0;
   size_t consumed = 0;
   bool consistent = true; // False if NEED_MORE was reported before a chunk was fully consumed
};

// Feeds Stream to a MessageReader in chunks of ChunkSize bytes and collects the results.  Stream is copied because
// payloads are unmasked in place.

ReadResult read_stream(Role LocalRole, std::vector<uint8_t> Stream, size_t ChunkSize, uint64_t MaxFrame = 0,
   uint64_t MaxMessage = 0)
{
   ReadResult result;
   MessageReader reader(LocalRole, MaxFrame, MaxMessage);
   Message current { Opcode::CONTINUATION, { } };

   size_t position = 0;
   while (position < Stream.size()) {
      auto chunk = std::span<uint8_t>(Stream).subspan(position, std::min(ChunkSize, Stream.size() - position));
      size_t offset = 0;
      while (true) {
         ReadEvent event;
         offset += reader.read(chunk.subspan(offset), event);

         if (event.status IS ReadStatus::NEED_MORE) {
            if (offset != chunk.size()) result.consistent = false;
            break;
         }
         else if (event.status IS ReadStatus::ERROR) {
            result.error_code   = event.close_code;
            result.error_reason = event.reason;
            result.consumed     = position + offset;
            return result;
         }
         else if (event.status IS ReadStatus::DATA) {
            current.opcode = event.opcode;
            current.data.insert(current.data.end(), event.data.begin(), event.data.end());
            if (event.message_end) {
               result.messages.push_back(std::move(current));
               current = Message { Opcode::CONTINUATION, { } };
            }
         }
         else {
            result.controls.push_back({ event.opcode, std::vector<uint8_t>(event.data.begin(), event.data.end()) });
            if (event.opcode IS Opcode::CLOSE) {
               result.close_codes.push_back(event.close_code);
               result.close_reason.assign(event.reason);
            }
         }
      }
      position += chunk.size();
   }
   result.consumed = position;
   return result;
}

// Decodes a single frame header with a FrameDecoder and returns the event.

DecodeEvent decode_header(Role LocalRole, std::vector<uint8_t> Bytes, uint64_t MaxFrame = 0, uint64_t MaxMessage = 0)
{
   FrameDecoder decoder(LocalRole, MaxFrame, MaxMessage);
   DecodeEvent event;
   decoder.decode(Bytes, event);
   return event;
}

//********************************************************************************************************************
// UTF-8

bool test_utf8_valid(kt::Log &Log)
{
   const std::array<std::string_view, 12> valid = {
      "", "Hello", "\x7f", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbf",
      "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf", "\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5"
   };
   for (auto text : valid) CHECK(valid_utf8(text), "Valid UTF-8 was rejected");
   return true;
}

bool test_utf8_invalid(kt::Log &Log)
{
   const std::array<std::string_view, 16> invalid = {
      "\x80",                 // Stray continuation byte
      "\xbf",
      "\xc0\x80",             // Overlong 2-byte forms
      "\xc1\xbf",
      "\xe0\x80\x80",         // Overlong 3-byte form
      "\xe0\x9f\xbf",
      "\xf0\x80\x80\x80",     // Overlong 4-byte form
      "\xf0\x8f\xbf\xbf",
      "\xed\xa0\x80",         // High surrogate
      "\xed\xbf\xbf",         // Low surrogate
      "\xf4\x90\x80\x80",     // Above U+10FFFF
      "\xf5\x80\x80\x80",
      "\xff",
      "\xc2\x41",             // Missing continuation
      "\xe2\x82",             // Truncated sequences
      "\xf0\x9f\x98"
   };
   for (auto text : invalid) CHECK(not valid_utf8(text), "Invalid UTF-8 was accepted");

   Utf8Validator validator;
   CHECK(validator.feed(std::string_view("\xe2\x82")) and (not validator.complete()), "Partial sequence state");
   CHECK(not validator.feed(std::string_view("A")), "Interrupted sequence was accepted");
   CHECK(not validator.feed(std::string_view("B")), "Validator did not remain failed");
   validator.reset();
   CHECK(validator.feed(std::string_view("B")) and validator.complete(), "Validator did not reset");
   return true;
}

bool test_utf8_split(kt::Log &Log)
{
   const std::string_view text = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z";

   for (size_t split = 0; split <= text.size(); split++) {
      Utf8Validator validator;
      CHECK(validator.feed(text.substr(0, split)), "First half of a split sequence was rejected");
      CHECK(validator.feed(text.substr(split)) and validator.complete(), "Split sequence was rejected");
   }

   Utf8Validator validator;
   for (auto ch : text) CHECK(validator.feed(std::string_view(&ch, 1)), "Byte-at-a-time feed was rejected");
   CHECK(validator.complete(), "Byte-at-a-time feed did not complete");
   return true;
}

//********************************************************************************************************************
// Frame encoding

bool test_encode_rfc_examples(kt::Log &Log)
{
   std::vector<uint8_t> output;

   append_frame(output, true, Opcode::TEXT, "Hello");
   CHECK((output IS std::vector<uint8_t> { 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f }), "Unmasked text frame");

   output.clear();
   append_frame(output, true, Opcode::TEXT, "Hello", &RFC_MASK);
   CHECK((output IS std::vector<uint8_t> { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 }),
      "Masked text frame");

   output.clear();
   append_frame(output, false, Opcode::TEXT, "Hel");
   append_frame(output, true, Opcode::CONTINUATION, "lo");
   CHECK((output IS std::vector<uint8_t> { 0x01, 0x03, 0x48, 0x65, 0x6c, 0x80, 0x02, 0x6c, 0x6f }),
      "Fragmented text frames");

   output.clear();
   append_frame(output, true, Opcode::PING, "Hello");
   CHECK((output IS std::vector<uint8_t> { 0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f }), "Unmasked ping frame");

   std::array<uint8_t, MAX_FRAME_HEADER> header;
   CHECK(encode_frame_header(header, true, Opcode::BINARY, 256) IS 4, "256 byte header size");
   CHECK((header[0] IS 0x82) and (header[1] IS 0x7e) and (header[2] IS 0x01) and (header[3] IS 0x00),
      "256 byte header bytes");

   CHECK(encode_frame_header(header, true, Opcode::BINARY, 65536) IS 10, "64 KiB header size");
   const std::array<uint8_t, 10> expected = { 0x82, 0x7f, 0, 0, 0, 0, 0, 0x01, 0x00, 0x00 };
   CHECK(std::equal(expected.begin(), expected.end(), header.begin()), "64 KiB header bytes");
   return true;
}

bool test_header_boundaries(kt::Log &Log)
{
   const std::array<uint64_t, 9> lengths = { 0, 1, 125, 126, 127, 65535, 65536, 0x100000000ull,
      0x7fffffffffffffffull };

   for (auto length : lengths) {
      for (bool masked : { false, true }) {
         std::array<uint8_t, MAX_FRAME_HEADER> header;
         const size_t expected_size = 2 + ((length <= 125) ? 0 : (length <= 65535) ? 2 : 8) + (masked ? 4 : 0);
         CHECK(frame_header_size(length, masked) IS expected_size, "frame_header_size() is incorrect");

         auto size = encode_frame_header(header, true, Opcode::BINARY, length, masked ? &RFC_MASK : nullptr);
         CHECK(size IS expected_size, "encode_frame_header() returned the wrong size");

         auto event = decode_header(masked ? Role::SERVER : Role::CLIENT,
            std::vector<uint8_t>(header.begin(), header.begin() + size));
         CHECK(event.status IS DecodeStatus::HEADER, "Encoded header did not decode");
         CHECK(event.header.length IS length, "Decoded length differs from the encoded length");
         CHECK(event.header.masked IS masked, "Decoded mask bit differs");
         if (masked) CHECK(event.header.mask IS RFC_MASK, "Decoded mask key differs");
      }
   }

   std::array<uint8_t, 4> small;
   CHECK(not encode_frame_header(small, true, Opcode::BINARY, 126, &RFC_MASK), "Undersized output was accepted");
   CHECK(not encode_frame_header(small, true, Opcode(3), 0), "Reserved opcode was encoded");
   CHECK(not encode_frame_header(small, true, Opcode::BINARY, 0x8000000000000000ull),
      "Length with the top bit set was encoded");
   return true;
}

bool test_apply_mask_offsets(kt::Log &Log)
{
   std::vector<uint8_t> whole(1031);
   for (size_t i = 0; i < whole.size(); i++) whole[i] = uint8_t(i * 7);
   auto original = whole;

   apply_mask(whole, RFC_MASK);
   for (size_t i = 0; i < whole.size(); i++) {
      CHECK(whole[i] IS uint8_t(original[i] ^ RFC_MASK[i & 3]), "Whole-buffer mask is incorrect");
   }

   for (size_t split : { size_t(1), size_t(3), size_t(9), size_t(500), size_t(1030) }) {
      auto pieces = original;
      apply_mask(std::span(pieces).first(split), RFC_MASK, 0);
      apply_mask(std::span(pieces).subspan(split), RFC_MASK, split);
      CHECK(pieces IS whole, "Masking in pieces differs from masking the whole buffer");
   }
   return true;
}

//********************************************************************************************************************
// Frame decoding

bool test_non_minimal_lengths(kt::Log &Log)
{
   auto event = decode_header(Role::CLIENT, { 0x82, 126, 0x00, 125 });
   CHECK((event.status IS DecodeStatus::ERROR) and (event.close_code IS CLOSE_PROTOCOL_ERROR),
      "16-bit encoding of 125 was accepted");

   event = decode_header(Role::CLIENT, { 0x82, 126, 0x00, 126 });
   CHECK((event.status IS DecodeStatus::HEADER) and (event.header.length IS 126), "16-bit encoding of 126");

   event = decode_header(Role::CLIENT, { 0x82, 127, 0, 0, 0, 0, 0, 0, 0xff, 0xff });
   CHECK((event.status IS DecodeStatus::ERROR) and (event.close_code IS CLOSE_PROTOCOL_ERROR),
      "64-bit encoding of 65535 was accepted");

   event = decode_header(Role::CLIENT, { 0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0 });
   CHECK((event.status IS DecodeStatus::HEADER) and (event.header.length IS 65536), "64-bit encoding of 65536");

   event = decode_header(Role::CLIENT, { 0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0 });
   CHECK((event.status IS DecodeStatus::ERROR) and (event.close_code IS CLOSE_PROTOCOL_ERROR),
      "64-bit length with the top bit set was accepted");
   return true;
}

bool test_reserved_bits_and_opcodes(kt::Log &Log)
{
   for (uint8_t rsv : { uint8_t(0x40), uint8_t(0x20), uint8_t(0x10) }) {
      auto event = decode_header(Role::CLIENT, { uint8_t(0x82 | rsv) });
      CHECK((event.status IS DecodeStatus::ERROR) and (event.close_code IS CLOSE_PROTOCOL_ERROR),
         "RSV bit was accepted");
   }

   for (uint8_t opcode = 0; opcode < 16; opcode++) {
      auto event = decode_header(Role::CLIENT, { uint8_t(0x80 | opcode), 0x00 });
      const bool reserved = ((opcode >= 3) and (opcode <= 7)) or (opcode >= 0xb);
      if ((opcode IS 0) or reserved) {
         CHECK((event.status IS DecodeStatus::ERROR) and (event.close_code IS CLOSE_PROTOCOL_ERROR),
            "Reserved opcode or unexpected continuation was accepted");
      }
      else CHECK(event.status IS DecodeStatus::HEADER, "Valid opcode was rejected");
   }
   return true;
}

bool test_control_frames(kt::Log &Log)
{
   auto event = decode_header(Role::CLIENT, { 0x89, 126, 0x00, 126 });
   CHECK((event.status IS DecodeStatus::ERROR) and (event.close_code IS CLOSE_PROTOCOL_ERROR),
      "Ping over 125 bytes was accepted");

   event = decode_header(Role::CLIENT, { 0x09, 0x00 });
   CHECK((event.status IS DecodeStatus::ERROR) and (event.close_code IS CLOSE_PROTOCOL_ERROR),
      "Fragmented ping was accepted");

   std::vector<uint8_t> stream;
   std::vector<uint8_t> payload(125, 'p');
   append_frame(stream, true, Opcode::PING, payload);
   auto result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK((result.error_code IS 0) and (result.controls.size() IS 1), "125 byte ping was rejected");
   CHECK(result.controls[0].data IS payload, "Ping payload differs");

   std::array<uint8_t, 256> output;
   std::vector<uint8_t> large(126, 'x');
   CHECK(not encode_frame(output, true, Opcode::PING, large), "Ping over 125 bytes was encoded");
   CHECK(not encode_frame(output, false, Opcode::CLOSE, {}), "Fragmented close frame was encoded");
   return true;
}

bool test_masking_by_role(kt::Log &Log)
{
   std::vector<uint8_t> masked, unmasked;
   append_frame(masked, true, Opcode::TEXT, "Hello", &RFC_MASK);
   append_frame(unmasked, true, Opcode::TEXT, "Hello");

   auto result = read_stream(Role::SERVER, unmasked, unmasked.size());
   CHECK(result.error_code IS CLOSE_PROTOCOL_ERROR, "Server accepted an unmasked frame");
   CHECK(result.consumed IS 2, "Unmasked frame was not rejected at the second byte");

   result = read_stream(Role::CLIENT, masked, masked.size());
   CHECK(result.error_code IS CLOSE_PROTOCOL_ERROR, "Client accepted a masked frame");

   result = read_stream(Role::SERVER, masked, masked.size());
   CHECK((result.error_code IS 0) and (result.messages.size() IS 1), "Server rejected a masked frame");
   CHECK(result.messages[0].data IS to_bytes("Hello"), "Server did not unmask the payload");

   result = read_stream(Role::CLIENT, unmasked, unmasked.size());
   CHECK((result.error_code IS 0) and (result.messages.size() IS 1), "Client rejected an unmasked frame");
   CHECK(result.messages[0].data IS to_bytes("Hello"), "Client payload differs");
   return true;
}

bool test_fragment_sequencing(kt::Log &Log)
{
   std::vector<uint8_t> stream;
   append_frame(stream, true, Opcode::CONTINUATION, "orphan");
   auto result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK(result.error_code IS CLOSE_PROTOCOL_ERROR, "Continuation without a message was accepted");

   stream.clear();
   append_frame(stream, false, Opcode::TEXT, "first");
   append_frame(stream, true, Opcode::BINARY, "second");
   result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK(result.error_code IS CLOSE_PROTOCOL_ERROR, "New data frame inside a fragmented message was accepted");

   stream.clear();
   append_frame(stream, false, Opcode::TEXT, "Hel");
   append_frame(stream, true, Opcode::PING, "ping");
   append_frame(stream, false, Opcode::CONTINUATION, "");
   append_frame(stream, true, Opcode::PONG, "");
   append_frame(stream, true, Opcode::CONTINUATION, "lo");
   append_frame(stream, true, Opcode::BINARY, "after");
   result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK(result.error_code IS 0, "Interleaved control frames were rejected");
   CHECK(result.messages.size() IS 2, "Expected two messages");
   CHECK((result.messages[0].opcode IS Opcode::TEXT) and (result.messages[0].data IS to_bytes("Hello")),
      "Fragmented text message differs");
   CHECK((result.messages[1].opcode IS Opcode::BINARY) and (result.messages[1].data IS to_bytes("after")),
      "Message after fragments differs");
   CHECK((result.controls.size() IS 2) and (result.controls[0].opcode IS Opcode::PING) and
      (result.controls[0].data IS to_bytes("ping")) and (result.controls[1].opcode IS Opcode::PONG),
      "Interleaved control frames differ");
   return true;
}

// A multi-frame client-to-server stream fed at many chunk sizes, including one byte at a time.

bool test_split_feeding(kt::Log &Log)
{
   std::vector<uint8_t> medium(200), large(70000);
   for (size_t i = 0; i < medium.size(); i++) medium[i] = uint8_t(i);
   for (size_t i = 0; i < large.size(); i++) large[i] = uint8_t(i * 31 + 7);

   std::array<uint8_t, MAX_CONTROL_PAYLOAD> close_payload;
   auto close_size = encode_close_payload(close_payload, CLOSE_NORMAL, "bye");
   CHECK(close_size IS 5, "Close payload size");

   std::vector<uint8_t> stream;
   append_frame(stream, true, Opcode::TEXT, "Hello", &RFC_MASK);
   append_frame(stream, false, Opcode::BINARY, std::vector<uint8_t> { 0, 1, 2 }, &RFC_MASK);
   append_frame(stream, true, Opcode::PING, "p", &RFC_MASK);
   append_frame(stream, false, Opcode::CONTINUATION, std::vector<uint8_t> { 3, 4 }, &RFC_MASK);
   append_frame(stream, true, Opcode::CONTINUATION, std::vector<uint8_t> { 5 }, &RFC_MASK);
   append_frame(stream, true, Opcode::BINARY, medium, &RFC_MASK);
   append_frame(stream, true, Opcode::BINARY, large, &RFC_MASK);
   append_frame(stream, true, Opcode::TEXT, "", &RFC_MASK);
   append_frame(stream, true, Opcode::CLOSE, std::span<const uint8_t>(close_payload.data(), close_size), &RFC_MASK);

   for (size_t chunk : { size_t(1), size_t(2), size_t(3), size_t(7), size_t(13), size_t(125), size_t(4096),
                         stream.size() }) {
      auto result = read_stream(Role::SERVER, stream, chunk);
      CHECK(result.consistent, "NEED_MORE was reported before the input was consumed");
      CHECK(result.error_code IS 0, "Valid stream was rejected");
      CHECK(result.consumed IS stream.size(), "Stream was not fully consumed");
      CHECK(result.messages.size() IS 5, "Expected five messages");
      CHECK((result.messages[0].opcode IS Opcode::TEXT) and (result.messages[0].data IS to_bytes("Hello")),
         "First message differs");
      CHECK((result.messages[1].opcode IS Opcode::BINARY) and
         (result.messages[1].data IS std::vector<uint8_t> { 0, 1, 2, 3, 4, 5 }), "Fragmented message differs");
      CHECK(result.messages[2].data IS medium, "200 byte message differs");
      CHECK(result.messages[3].data IS large, "70000 byte message differs");
      CHECK((result.messages[4].opcode IS Opcode::TEXT) and result.messages[4].data.empty(),
         "Empty text message differs");
      CHECK(result.controls.size() IS 2, "Expected two control frames");
      CHECK((result.controls[0].opcode IS Opcode::PING) and (result.controls[0].data IS to_bytes("p")),
         "Ping differs");
      CHECK((result.close_codes.size() IS 1) and (result.close_codes[0] IS CLOSE_NORMAL) and
         (result.close_reason IS "bye"), "Close frame differs");
   }
   return true;
}

//********************************************************************************************************************
// Text validation across frames

bool test_fragmented_text_utf8(kt::Log &Log)
{
   const std::string_view text = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z";

   // A code point split at every possible frame boundary is accepted.

   for (size_t split = 0; split <= text.size(); split++) {
      std::vector<uint8_t> stream;
      append_frame(stream, false, Opcode::TEXT, text.substr(0, split));
      append_frame(stream, true, Opcode::CONTINUATION, text.substr(split));
      auto result = read_stream(Role::CLIENT, stream, 1);
      CHECK((result.error_code IS 0) and (result.messages.size() IS 1), "Split code point was rejected");
      CHECK(result.messages[0].data IS to_bytes(text), "Split text message differs");
   }

   // An invalid byte at any position fails with 1007, wherever the frame boundary falls.

   for (size_t position = 0; position < text.size(); position++) {
      std::string bad(text);
      bad[position] = '\xff';
      for (size_t split : { size_t(0), position, text.size() / 2, text.size() }) {
         std::vector<uint8_t> stream;
         append_frame(stream, false, Opcode::TEXT, std::string_view(bad).substr(0, split));
         append_frame(stream, true, Opcode::CONTINUATION, std::string_view(bad).substr(split));
         auto result = read_stream(Role::CLIENT, stream, stream.size());
         CHECK(result.error_code IS CLOSE_INVALID_PAYLOAD, "Invalid UTF-8 was not rejected with 1007");
         CHECK(result.messages.empty(), "A message was delivered despite invalid UTF-8");
      }
   }

   // Invalid UTF-8 is detected in the first fragment, before the message is complete.

   std::vector<uint8_t> stream;
   append_frame(stream, false, Opcode::TEXT, "ok\xc0\xaf");
   auto result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK(result.error_code IS CLOSE_INVALID_PAYLOAD, "Invalid UTF-8 was not detected in the first fragment");

   // A message that ends inside a sequence fails with 1007.

   stream.clear();
   append_frame(stream, false, Opcode::TEXT, "abc\xf0\x9f");
   append_frame(stream, true, Opcode::CONTINUATION, "\x98");
   result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK(result.error_code IS CLOSE_INVALID_PAYLOAD, "Truncated final sequence was not rejected with 1007");

   // Binary messages are not validated.

   stream.clear();
   append_frame(stream, true, Opcode::BINARY, "\xff\xfe");
   result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK((result.error_code IS 0) and (result.messages.size() IS 1), "Binary message was validated as text");

   // UTF-8 checks can be disabled.

   stream.clear();
   append_frame(stream, true, Opcode::TEXT, "\xff");
   MessageReader reader(Role::CLIENT, 0, 0, false);
   ReadEvent event;
   reader.read(stream, event);
   CHECK((event.status IS ReadStatus::DATA) and event.message_end, "Disabled UTF-8 check still rejected text");
   return true;
}

//********************************************************************************************************************
// Size limits

bool test_size_limits(kt::Log &Log)
{
   // The frame limit is enforced on the header alone, before any payload arrives.

   std::array<uint8_t, MAX_FRAME_HEADER> header;
   auto size = encode_frame_header(header, true, Opcode::BINARY, 101, &RFC_MASK);
   auto event = decode_header(Role::SERVER, std::vector<uint8_t>(header.begin(), header.begin() + size), 100, 0);
   CHECK((event.status IS DecodeStatus::ERROR) and (event.close_code IS CLOSE_MESSAGE_TOO_BIG),
      "Oversized frame was not rejected with 1009");

   size = encode_frame_header(header, true, Opcode::BINARY, 100, &RFC_MASK);
   event = decode_header(Role::SERVER, std::vector<uint8_t>(header.begin(), header.begin() + size), 100, 0);
   CHECK(event.status IS DecodeStatus::HEADER, "Frame at the limit was rejected");

   // The message limit applies to the sum of the fragments.

   std::vector<uint8_t> six(6, 'x'), four(4, 'y'), five(5, 'z');
   std::vector<uint8_t> stream;
   append_frame(stream, false, Opcode::BINARY, six);
   append_frame(stream, true, Opcode::CONTINUATION, four);
   auto result = read_stream(Role::CLIENT, stream, stream.size(), 0, 10);
   CHECK((result.error_code IS 0) and (result.messages.size() IS 1), "Message at the limit was rejected");

   stream.clear();
   append_frame(stream, false, Opcode::BINARY, six);
   append_frame(stream, true, Opcode::CONTINUATION, five);
   result = read_stream(Role::CLIENT, stream, stream.size(), 0, 10);
   CHECK(result.error_code IS CLOSE_MESSAGE_TOO_BIG, "Oversized message was not rejected with 1009");
   CHECK(result.consumed IS 8 + 2, "Oversized message was not rejected at the continuation header");

   // Control frames do not count towards the message limit.

   stream.clear();
   append_frame(stream, false, Opcode::BINARY, six);
   append_frame(stream, true, Opcode::PING, std::vector<uint8_t>(50, 'p'));
   append_frame(stream, true, Opcode::CONTINUATION, four);
   result = read_stream(Role::CLIENT, stream, stream.size(), 0, 10);
   CHECK((result.error_code IS 0) and (result.messages.size() IS 1), "Ping counted towards the message limit");
   return true;
}

//********************************************************************************************************************
// Close payloads

bool test_close_payloads(kt::Log &Log)
{
   int code;
   std::string_view reason;

   CHECK((parse_close_payload({}, code, reason) IS 0) and (code IS CLOSE_NO_STATUS), "Empty close payload");

   const std::array<uint8_t, 1> one = { 0x03 };
   CHECK(parse_close_payload(one, code, reason) IS CLOSE_PROTOCOL_ERROR, "1-byte close payload was accepted");

   for (int valid : { 1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 3000, 3999, 4000, 4999 }) {
      std::array<uint8_t, MAX_CONTROL_PAYLOAD> payload;
      auto size = encode_close_payload(payload, valid, "ok");
      CHECK(size IS 4, "Valid close code was not encoded");
      CHECK(parse_close_payload(std::span<const uint8_t>(payload.data(), size), code, reason) IS 0,
         "Valid close code was rejected");
      CHECK((code IS valid) and (reason IS "ok"), "Parsed close payload differs");
   }

   for (int invalid : { 0, 999, 1004, 1005, 1006, 1012, 1013, 1014, 1015, 1016, 2000, 2999, 5000, 65535 }) {
      const std::array<uint8_t, 2> payload = { uint8_t(invalid >> 8), uint8_t(invalid) };
      CHECK(parse_close_payload(payload, code, reason) IS CLOSE_PROTOCOL_ERROR, "Invalid close code was accepted");

      std::array<uint8_t, MAX_CONTROL_PAYLOAD> output;
      CHECK(not encode_close_payload(output, invalid, ""), "Invalid close code was encoded");
   }

   const std::array<uint8_t, 4> bad_reason = { 0x03, 0xe8, 0xc0, 0x80 };
   CHECK(parse_close_payload(bad_reason, code, reason) IS CLOSE_INVALID_PAYLOAD,
      "Close reason with invalid UTF-8 was accepted");

   std::array<uint8_t, MAX_CONTROL_PAYLOAD> output;
   CHECK(encode_close_payload(output, 1000, std::string(MAX_CLOSE_REASON, 'r')) IS MAX_CONTROL_PAYLOAD,
      "123 byte reason was rejected");
   CHECK(not encode_close_payload(output, 1000, std::string(MAX_CLOSE_REASON + 1, 'r')),
      "124 byte reason was accepted");
   CHECK(not encode_close_payload(output, 1000, "\xff"), "Invalid UTF-8 reason was encoded");

   // The reader validates close frames and reports their status.

   std::vector<uint8_t> stream;
   append_frame(stream, true, Opcode::CLOSE, std::vector<uint8_t> { 0x03 });
   auto result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK(result.error_code IS CLOSE_PROTOCOL_ERROR, "Reader accepted a 1-byte close payload");

   stream.clear();
   append_frame(stream, true, Opcode::CLOSE, std::vector<uint8_t> { 0x03, 0xee }); // 1006
   result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK(result.error_code IS CLOSE_PROTOCOL_ERROR, "Reader accepted close code 1006");

   stream.clear();
   append_frame(stream, true, Opcode::CLOSE, std::vector<uint8_t> { });
   result = read_stream(Role::CLIENT, stream, stream.size());
   CHECK((result.close_codes.size() IS 1) and (result.close_codes[0] IS CLOSE_NO_STATUS),
      "Empty close frame did not report 1005");
   return true;
}

//********************************************************************************************************************
// Handshake helpers

bool test_accept_key(kt::Log &Log)
{
   std::string accept;
   CHECK(compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept) IS ERR::Okay, "compute_accept_key() failed");
   CHECK(accept IS "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "RFC 6455 section 4.2.2 accept key differs");
   return true;
}

bool test_keys(kt::Log &Log)
{
   std::string first, second;
   CHECK(generate_key(first) IS ERR::Okay, "generate_key() failed");
   CHECK(generate_key(second) IS ERR::Okay, "generate_key() failed");
   CHECK((first.size() IS KEY_LENGTH) and valid_key(first), "Generated key is not valid");
   CHECK(first != second, "Two generated keys are identical");

   CHECK(valid_key("dGhlIHNhbXBsZSBub25jZQ=="), "RFC sample key was rejected");
   CHECK(not valid_key(""), "Empty key was accepted");
   CHECK(not valid_key("dGhlIHNhbXBsZSBub25jZQ"), "Unpadded key was accepted");
   CHECK(not valid_key("dGhlIHNhbXBsZSBub25jZR=="), "Non-canonical key was accepted");
   CHECK(not valid_key("dGhlIHNhbXBsZSBub25j"), "Key decoding to 15 bytes was accepted");
   CHECK(not valid_key("dGhlIHNhbXBsZSBub25jZQ==AAAA"), "Key decoding to 19 bytes was accepted");
   CHECK(not valid_key("dGhlIHNhbXBsZSBub25jZ!=="), "Key with an invalid character was accepted");
   CHECK(not valid_key(" dGhlIHNhbXBsZSBub25jZQ="), "Key with whitespace was accepted");
   return true;
}

bool test_header_tokens(kt::Log &Log)
{
   CHECK(has_token("Upgrade", "upgrade"), "Single token was not matched");
   CHECK(has_token("keep-alive, Upgrade", "upgrade"), "Token in a list was not matched");
   CHECK(has_token(" keep-alive ,\tUPGRADE\t", "Upgrade"), "Token with whitespace was not matched");
   CHECK(not has_token("keep-alive", "upgrade"), "Absent token was matched");
   CHECK(not has_token("Upgraded", "upgrade"), "Token prefix was matched");
   CHECK(not has_token("Up grade", "upgrade"), "Invalid list was matched");

   std::vector<std::string_view> tokens;
   CHECK(parse_token_list(", chat ,,superchat, ", tokens), "Valid token list was rejected");
   CHECK((tokens.size() IS 2) and (tokens[0] IS "chat") and (tokens[1] IS "superchat"), "Token list differs");
   CHECK(parse_token_list("", tokens) and tokens.empty(), "Empty token list");
   CHECK(not parse_token_list("chat, super chat", tokens) and tokens.empty(), "Token with a space was accepted");
   CHECK(not parse_token_list("chat;v=1", tokens), "Token with a separator was accepted");
   CHECK(not parse_token_list("\"chat\"", tokens), "Quoted string was accepted as a token");

   CHECK(is_token("v1.chat_x-y~z!#$%&'*+^`|"), "Token characters were rejected");
   CHECK(not is_token(""), "Empty token was accepted");

   CHECK(valid_version("13") and valid_version(" 13\t"), "Version 13 was rejected");
   CHECK(not valid_version("8") and not valid_version("13, 8") and not valid_version("") and
      not valid_version("130"), "Invalid version was accepted");
   return true;
}

//********************************************************************************************************************

bool test_parse_location(kt::Log &Log)
{
   Location loc;
   CHECK(parse_location("ws://example.com", loc), "Minimal URL was rejected");
   CHECK((loc.host IS "example.com") and (loc.port IS 80) and (not loc.secure) and (loc.target IS "/"),
      "Minimal URL components differ");

   CHECK(parse_location("WSS://Example.com/chat", loc), "Upper-case scheme was rejected");
   CHECK((loc.port IS 443) and loc.secure and (loc.target IS "/chat"), "wss:// defaults differ");

   CHECK(parse_location("ws://h:8080/a/b?x=1&y=%20z", loc), "Port and query were rejected");
   CHECK((loc.port IS 8080) and (loc.target IS "/a/b?x=1&y=%20z"), "Path and query were not preserved verbatim");

   CHECK(parse_location("ws://h?q", loc) and (loc.target IS "/?q"), "Query without a path was not normalised");
   CHECK(parse_location("wss://[::1]:9443/x", loc), "Bracketed IPv6 was rejected");
   CHECK((loc.host IS "::1") and (loc.port IS 9443) and loc.secure, "IPv6 components differ");
   CHECK(parse_location("ws://[2001:db8::7]", loc) and (loc.host IS "2001:db8::7") and (loc.port IS 80),
      "IPv6 without a port differs");
   CHECK(parse_location("ws://h:443/", loc) and (not loc.secure), "Port 443 changed the scheme's security");
   CHECK(parse_location("ws://h:1/", loc) and parse_location("ws://h:65535/", loc), "Port range limits");

   constexpr std::array<std::string_view, 16> invalid = {
      "", "http://h/", "ws:/h", "ws://", "ws:///path", "ws://h/#frag", "ws://h#frag", "ws://user@h/",
      "ws://h:0/", "ws://h:65536/", "ws://h:/", "ws://h:8a/", "ws://[::1/", "ws://[]/", "ws://[::1]x/",
      "ws://h/a b"
   };
   for (auto url : invalid) {
      if (parse_location(url, loc)) {
         Log.error("Invalid URL was accepted: %.*s", int(url.size()), url.data());
         return false;
      }
   }
   CHECK(not parse_location("ws://h/\x80", loc), "Non-ASCII URL was accepted");
   CHECK(not parse_location("ws://[fe80::1%25eth0]/", loc), "IPv6 zone identifier was accepted");
   return true;
}

//********************************************************************************************************************

bool test_parse_header_fields(kt::Log &Log)
{
   std::vector<HeaderField> fields;
   CHECK(parse_header_fields("Upgrade: websocket\nX-Repeat: a\nx-repeat:  b \nEmpty: \nColon: a:b\n", fields),
      "Valid header output was rejected");
   CHECK(fields.size() IS 5, "Field count differs");
   CHECK((fields[0].name IS "Upgrade") and (fields[0].value IS "websocket"), "First field differs");
   CHECK((fields[2].name IS "x-repeat") and (fields[2].value IS "b"), "Repeated field or trimming differs");
   CHECK(fields[3].value.empty(), "Empty value differs");
   CHECK(fields[4].value IS "a:b", "Value was not split at the first colon");

   CHECK(parse_header_fields("", fields) and fields.empty(), "Empty header output");
   CHECK(parse_header_fields("A: 1\r\nB: 2", fields) and (fields.size() IS 2) and (fields[0].value IS "1"),
      "CRLF and unterminated last line");
   CHECK(not parse_header_fields("A: 1\nno colon\n", fields) and fields.empty(), "Line without a colon was accepted");
   CHECK(not parse_header_fields("Bad Name: 1\n", fields), "Invalid field name was accepted");
   return true;
}

//********************************************************************************************************************

bool test_validate_response(kt::Log &Log)
{
   constexpr std::string_view accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
   std::vector<HeaderField> fields;
   std::vector<std::string_view> none, requested = { "chat", "superchat" };
   std::string_view selected;

   auto validate = [&](std::string_view Headers, const std::vector<std::string_view> &Requested) {
      if (not parse_header_fields(Headers, fields)) return std::string_view("unparsed");
      return validate_response(fields, accept, Requested, selected);
   };

   CHECK(validate("Upgrade: websocket\nsec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\n", none).empty(),
      "Valid response with a lower-case name was rejected");
   CHECK(selected.empty(), "Subprotocol selected without a header");
   CHECK(validate("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\nSec-WebSocket-Protocol: superchat\n",
      requested).empty() and (selected IS "superchat"), "Requested subprotocol was not selected");

   CHECK(not validate("Upgrade: websocket\n", none).empty(), "Missing accept was permitted");
   CHECK(not validate("Sec-WebSocket-Accept: AAAA\n", none).empty(), "Wrong accept was permitted");
   CHECK(not validate("Sec-WebSocket-Accept: s3pplmbitxaq9kygzzhzrbk+xoo=\n", none).empty(),
      "Accept comparison ignored case");
   CHECK(not validate("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\n", none).empty(), "Repeated accept was permitted");
   CHECK(not validate("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\nSec-WebSocket-Protocol: chat\n",
      none).empty(), "Unrequested subprotocol was permitted");
   CHECK(not validate("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\nSec-WebSocket-Protocol: Chat\n",
      requested).empty(), "Subprotocol comparison ignored case");
   CHECK(not validate("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\nSec-WebSocket-Protocol: chat\n"
      "Sec-WebSocket-Protocol: chat\n", requested).empty(), "Repeated subprotocol was permitted");
   CHECK(not validate("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\nSec-WebSocket-Protocol: chat, superchat\n",
      requested).empty(), "A subprotocol list was permitted");
   CHECK(not validate("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\nSec-WebSocket-Extensions: x\n",
      none).empty(), "An extension was permitted");
   return true;
}

//********************************************************************************************************************

bool test_find_request_end(kt::Log &Log)
{
   CHECK(find_request_end("") IS 0, "Empty input has an end");
   CHECK(find_request_end("GET / HTTP/1.1\r\nHost: a\r\n") IS 0, "Incomplete request has an end");
   CHECK(find_request_end("GET / HTTP/1.1\r\nHost: a\r\n\r") IS 0, "Partial terminator accepted");
   CHECK(find_request_end("GET / HTTP/1.1\r\nHost: a\r\n\r\n") IS 27, "CRLF terminator not found");
   CHECK(find_request_end("GET / HTTP/1.1\r\nHost: a\r\n\r\nxyz") IS 27, "Trailing bytes included in the head");
   CHECK(find_request_end("GET / HTTP/1.1\nHost: a\n\nxyz") IS 24, "Bare LF terminator not found");
   CHECK(find_request_end("GET / HTTP/1.1\r\n\r\n") IS 18, "Request without fields not terminated");
   return true;
}

//********************************************************************************************************************

bool test_parse_request(kt::Log &Log)
{
   constexpr std::string_view valid =
      "GET /chat?room=a%20b HTTP/1.1\r\n"
      "Host: server.example.com\r\n"
      "Upgrade: websocket\r\n"
      "Connection: keep-alive, Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Origin: http://example.com\r\n"
      "Sec-WebSocket-Protocol: chat, superchat\r\n"
      "Sec-WebSocket-Protocol: v2\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";

   Request request;
   std::string_view reason;

   CHECK(parse_request(valid, request, reason) IS 0, "Valid request rejected");
   CHECK(request.target IS "/chat?room=a%20b", "Target not preserved");
   CHECK((request.path IS "/chat") and (request.query IS "room=a%20b"), "Path and query not split");
   CHECK(request.host IS "server.example.com", "Host not parsed");
   CHECK(request.key IS "dGhlIHNhbXBsZSBub25jZQ==", "Key not parsed");
   CHECK(request.origin IS "http://example.com", "Origin not parsed");
   CHECK((request.protocols.size() IS 3) and (request.protocols[0] IS "chat") and
      (request.protocols[2] IS "v2"), "Repeated Sec-WebSocket-Protocol fields not combined in order");

   // Returns the valid request with the first occurrence of Old replaced by New.

   auto variant = [&](std::string_view Old, std::string_view New) {
      std::string text(valid);
      auto pos = text.find(Old);
      if (pos != std::string::npos) text.replace(pos, Old.size(), New);
      return text;
   };

   auto status = [&](const std::string &Text) {
      Request result;
      std::string_view why;
      return parse_request(Text, result, why);
   };

   CHECK(status(variant("Upgrade: websocket", "upgrade: WebSocket")) IS 0,
      "Field names or values not case-insensitive");
   CHECK(status(variant("\r\n", "\n")) IS 0, "Bare LF on the request line rejected");
   CHECK(status(variant("/chat?room=a%20b", "/")) IS 0, "Root path rejected");

   CHECK(status(variant("GET ", "POST ")) IS STATUS_BAD_REQUEST, "POST accepted");
   CHECK(status(variant("HTTP/1.1", "HTTP/1.0")) IS STATUS_BAD_REQUEST, "HTTP/1.0 accepted");
   CHECK(status(variant("/chat?room=a%20b", "chat")) IS STATUS_BAD_REQUEST, "Relative target accepted");
   CHECK(status(variant("/chat?room=a%20b", "ws://host/chat")) IS STATUS_BAD_REQUEST, "Absolute form accepted");
   CHECK(status(variant("/chat?room=a%20b", "/chat#x")) IS STATUS_BAD_REQUEST, "Fragment accepted");
   CHECK(status(variant("/chat?room=a%20b HTTP", "/chat HTTP/1.1 HTTP")) IS STATUS_BAD_REQUEST,
      "Extra request line element accepted");
   CHECK(status(variant("Host: server.example.com\r\n", "")) IS STATUS_BAD_REQUEST, "Missing Host accepted");
   CHECK(status(variant("Host: server.example.com\r\n", "Host: a\r\nHost: b\r\n")) IS STATUS_BAD_REQUEST,
      "Repeated Host accepted");
   CHECK(status(variant("Host: server.example.com", "Host:")) IS STATUS_BAD_REQUEST, "Empty Host accepted");
   CHECK(status(variant("Upgrade: websocket", "Upgrade: h2c")) IS STATUS_BAD_REQUEST, "Wrong Upgrade accepted");
   CHECK(status(variant("Upgrade: websocket\r\n", "")) IS STATUS_BAD_REQUEST, "Missing Upgrade accepted");
   CHECK(status(variant("keep-alive, Upgrade", "keep-alive")) IS STATUS_BAD_REQUEST, "Bad Connection accepted");
   CHECK(status(variant("Sec-WebSocket-Version: 13", "Sec-WebSocket-Version: 8")) IS STATUS_UPGRADE_REQUIRED,
      "Version 8 not answered with 426");
   CHECK(status(variant("Sec-WebSocket-Version: 13\r\n", "")) IS STATUS_BAD_REQUEST, "Missing version accepted");
   CHECK(status(variant("Sec-WebSocket-Version: 13\r\n",
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Version: 13\r\n")) IS STATUS_BAD_REQUEST,
      "Repeated version accepted");
   CHECK(status(variant("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n", "")) IS STATUS_BAD_REQUEST,
      "Missing key accepted");
   CHECK(status(variant("dGhlIHNhbXBsZSBub25jZQ==", "dGhlIHNhbXBsZQ==")) IS STATUS_BAD_REQUEST,
      "Short key accepted");
   CHECK(status(variant("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n",
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"))
      IS STATUS_BAD_REQUEST, "Repeated key accepted");
   CHECK(status(variant("chat, superchat", "chat, super chat")) IS STATUS_BAD_REQUEST, "Invalid protocol accepted");
   CHECK(status(variant("Origin: http://example.com\r\n", "Content-Length: 5\r\n")) IS STATUS_BAD_REQUEST,
      "Request body accepted");
   CHECK(status(variant("Origin: http://example.com\r\n", "Content-Length: 0\r\n")) IS 0,
      "Zero Content-Length rejected");
   CHECK(status(variant("Origin: http://example.com\r\n", "Transfer-Encoding: chunked\r\n")) IS STATUS_BAD_REQUEST,
      "Chunked body accepted");
   CHECK(status(variant("Origin: http://example.com\r\n", "Bad Name: x\r\n")) IS STATUS_BAD_REQUEST,
      "Invalid field name accepted");
   CHECK(status(variant("Origin: http://example.com\r\n", "Origin: http://example.com\r\n folded\r\n"))
      IS STATUS_BAD_REQUEST, "Obsolete line folding accepted");
   CHECK(status(variant("Origin: http://example.com", "Origin: a\x01z")) IS STATUS_BAD_REQUEST,
      "Control character accepted");
   return true;
}

//********************************************************************************************************************

bool test_server_responses(kt::Log &Log)
{
   std::vector<std::string> supported = { "v2", "chat" };
   std::vector<std::string_view> requested = { "chat", "superchat", "v2" }, other = { "x" }, upper = { "CHAT" }, none;

   CHECK(select_protocol(supported, requested) IS "v2", "Server preference order not applied");
   CHECK(select_protocol(supported, other).empty(), "Unmatched protocol selected");
   CHECK(select_protocol(supported, none).empty(), "Protocol selected without a request");
   CHECK(select_protocol(supported, upper).empty(), "Protocol selection ignored case");

   CHECK(switching_response("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "") IS
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n", "Switching response is wrong");
   CHECK(switching_response("abc", "chat").find("\r\nSec-WebSocket-Protocol: chat\r\n\r\n") != std::string::npos,
      "Subprotocol missing from the switching response");

   CHECK(rejection_response(400) IS "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
      "400 response is wrong");
   CHECK(rejection_response(426).find("\r\nSec-WebSocket-Version: 13\r\n") != std::string::npos,
      "426 response does not name the version");
   CHECK(rejection_response(404).starts_with("HTTP/1.1 404 Not Found\r\n"), "404 response is wrong");
   CHECK(rejection_response(403).starts_with("HTTP/1.1 403 Forbidden\r\n"), "403 response is wrong");
   CHECK(rejection_response(431).starts_with("HTTP/1.1 431 "), "431 response is wrong");
   return true;
}

//********************************************************************************************************************
// Send queue

// Mask generators.  The counter generator produces a predictable sequence so that two drains can be compared.

uint8_t glMaskCounter = 0;

bool rfc_mask(MaskKey &Key) { Key = RFC_MASK; return true; }
bool counter_mask(MaskKey &Key) { for (auto &byte : Key) byte = ++glMaskCounter; return true; }
bool failing_mask(MaskKey &) { return false; }

// Drains Queue, offering at most Accept bytes of each chunk to an imaginary transport.  Returns false if pending()
// did not decrease by exactly the accepted count.

bool drain_queue(SendQueue &Queue, std::vector<uint8_t> &Output, size_t Accept, size_t ScratchSize = 4096)
{
   std::vector<uint8_t> scratch(ScratchSize);
   while (true) {
      auto chunk = Queue.next_chunk(scratch);
      if (chunk.empty()) return Queue.idle();
      auto accepted = std::min(Accept, chunk.size());
      auto before = Queue.pending();
      Output.insert(Output.end(), chunk.begin(), chunk.begin() + accepted);
      Queue.consume(accepted);
      if (Queue.pending() != before - accepted) return false;
   }
}

bool test_send_queue_encoding(kt::Log &Log)
{
   // RFC 6455 section 5.7: a masked "Hello" from a client and an unmasked one from a server.

   SendQueue client(Role::CLIENT, &rfc_mask);
   CHECK(client.push_message(Opcode::TEXT, to_bytes("Hello")) IS QueueResult::OKAY, "Message was refused");
   CHECK(client.pending() IS 11 and client.encoded_size(5) IS 11, "Masked frame size is wrong");
   std::vector<uint8_t> output;
   CHECK(drain_queue(client, output, 1000), "Client queue did not drain");
   const std::vector<uint8_t> masked = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
   CHECK(output IS masked, "Masked frame does not match RFC 6455 section 5.7");

   SendQueue server(Role::SERVER, nullptr);
   server.push_message(Opcode::TEXT, to_bytes("Hello"));
   output.clear();
   CHECK(drain_queue(server, output, 1000), "Server queue did not drain");
   CHECK(output IS to_bytes("\x81\x05Hello"), "Unmasked frame does not match RFC 6455 section 5.7");

   // An empty message is a single empty final frame.

   server.push_message(Opcode::BINARY, {});
   output.clear();
   CHECK(drain_queue(server, output, 1000) and (output IS to_bytes(std::string_view("\x82\x00", 2))),
      "Empty message was not encoded as one empty frame");

   // Fragment sizes: 250 bytes in 100 byte fragments is 100 + 100 + 50 with headers of 2 bytes each.

   SendQueue fragmented(Role::SERVER, nullptr, 100);
   CHECK(fragmented.encoded_size(250) IS 256 and fragmented.encoded_size(200) IS 204, "Fragmented size is wrong");
   CHECK(fragmented.encoded_size(0) IS 2, "Empty message size is wrong");
   std::vector<uint8_t> payload(250, 'x');
   fragmented.push_message(Opcode::BINARY, payload);
   output.clear();
   CHECK(drain_queue(fragmented, output, 1000) and (output.size() IS 256), "Fragmented message has the wrong size");
   CHECK(output[0] IS 0x02 and output[102] IS 0x00 and output[204] IS 0x80, "Fragment opcodes or FIN bits are wrong");

   // Changing the fragment size reframes queued messages and updates their encoded-byte accounting.

   SendQueue reframed(Role::SERVER, nullptr);
   reframed.push_message(Opcode::BINARY, payload);
   CHECK(reframed.pending() IS 254, "Unfragmented pending size is wrong");
   reframed.set_fragment_size(100);
   CHECK(reframed.pending() IS 256, "Reframing did not update the pending size");
   output.clear();
   CHECK(drain_queue(reframed, output, 1000) and (output.size() IS 256), "Reframed message did not drain");

   SendQueue empty_reframed(Role::SERVER, nullptr);
   empty_reframed.push_message(Opcode::BINARY, {});
   std::vector<uint8_t> scratch(64);
   CHECK(not empty_reframed.next_chunk(scratch).empty(), "Empty frame did not start");
   empty_reframed.set_fragment_size(100);
   CHECK(empty_reframed.pending() IS 2, "Reframing an active empty message changed its pending size");
   output.clear();
   CHECK(drain_queue(empty_reframed, output, 1000), "Reframed empty message did not drain");

   // A mask generator failure stops the queue.

   SendQueue broken(Role::CLIENT, &failing_mask);
   broken.push_message(Opcode::TEXT, to_bytes("x"));
   CHECK(broken.next_chunk(scratch).empty() and broken.has_failed(), "Mask failure was not reported");
   return true;
}

bool test_send_queue_partial_writes(kt::Log &Log)
{
   // The same messages drained whole and in awkward pieces must produce identical bytes that decode to the originals.

   std::vector<std::vector<uint8_t>> payloads;
   for (size_t size : { size_t(0), size_t(1), size_t(125), size_t(126), size_t(250), size_t(65535), size_t(65536),
      size_t(70001) }) {
      std::vector<uint8_t> payload(size);
      for (size_t i = 0; i < size; i++) payload[i] = uint8_t(i * 7 + size);
      payloads.push_back(std::move(payload));
   }

   auto fill = [&](SendQueue &Queue) {
      for (auto &payload : payloads) Queue.push_message(Opcode::BINARY, payload);
      Queue.push_control(Opcode::PING, to_bytes("p"));
   };

   glMaskCounter = 0;
   SendQueue whole(Role::CLIENT, &counter_mask, 30000);
   fill(whole);
   std::vector<uint8_t> reference;
   CHECK(drain_queue(whole, reference, SIZE_MAX), "Whole drain failed");

   for (size_t accept : { size_t(1), size_t(2), size_t(3), size_t(7), size_t(13), size_t(125), size_t(4096) }) {
      for (size_t scratch : { size_t(MAX_FRAME_HEADER), size_t(100), size_t(65536) }) {
         if ((scratch > 4096) and (accept < 4096)) continue; // Each chunk copies the scratch size; keep the test fast
         glMaskCounter = 0;
         SendQueue queue(Role::CLIENT, &counter_mask, 30000);
         fill(queue);
         std::vector<uint8_t> output;
         CHECK(drain_queue(queue, output, accept, scratch), "Partial drain did not track pending bytes");
         CHECK(output IS reference, "Partial writes changed the byte stream");
      }
   }

   auto result = read_stream(Role::SERVER, reference, reference.size());
   CHECK(result.error_code IS 0, "Encoded stream failed to decode");
   CHECK(result.messages.size() IS payloads.size(), "Decoded message count is wrong");
   for (size_t i = 0; i < payloads.size(); i++) {
      CHECK(result.messages[i].data IS payloads[i], "Decoded payload differs from the original");
   }
   CHECK(result.controls.size() IS 1 and result.controls[0].opcode IS Opcode::PING, "Ping was not encoded");
   return true;
}

bool test_send_queue_control_frames(kt::Log &Log)
{
   std::vector<uint8_t> scratch(4096);

   // A Ping queued during the first fragment is sent at the next frame boundary, ahead of the remaining fragments.

   SendQueue queue(Role::SERVER, nullptr, 10);
   std::vector<uint8_t> payload(25, 'd');
   queue.push_message(Opcode::BINARY, payload);
   auto chunk = queue.next_chunk(scratch);
   CHECK(chunk.size() IS 12, "First fragment has the wrong size");
   std::vector<uint8_t> output(chunk.begin(), chunk.begin() + 5);
   queue.consume(5);
   queue.push_control(Opcode::PING, to_bytes("p1"));
   CHECK(drain_queue(queue, output, SIZE_MAX), "Queue did not drain");
   CHECK(output.size() IS 12 + 4 + 12 + 7, "Interleaved stream has the wrong size");
   CHECK(output[12] IS 0x89 and output[16] IS 0x00 and output[28] IS 0x80, "Ping was not placed at a frame boundary");
   auto result = read_stream(Role::CLIENT, output, output.size());
   CHECK(result.error_code IS 0 and result.messages.size() IS 1 and result.messages[0].data IS payload,
      "Interleaved stream did not decode");

   // Unsent Pongs are replaced by the most recent one.

   SendQueue pongs(Role::SERVER, nullptr);
   pongs.push_control(Opcode::PONG, to_bytes("first"));
   pongs.push_control(Opcode::PONG, to_bytes("second"));
   CHECK(pongs.pending() IS 8, "Replaced Pong was still counted");
   output.clear();
   CHECK(drain_queue(pongs, output, SIZE_MAX) and (output IS to_bytes("\x8a\x06second")), "Pong was not replaced");
   CHECK(pongs.push_control(Opcode::CLOSE, {}) IS QueueResult::INVALID, "Close was accepted as a control frame");
   std::vector<uint8_t> big(126, 'x');
   CHECK(pongs.push_control(Opcode::PING, big) IS QueueResult::INVALID, "Oversized Ping was accepted");

   // A graceful Close follows every queued frame.

   SendQueue graceful(Role::SERVER, nullptr);
   graceful.push_message(Opcode::TEXT, to_bytes("a"));
   graceful.push_message(Opcode::TEXT, to_bytes("b"));
   std::array<uint8_t, MAX_CONTROL_PAYLOAD> close;
   auto length = encode_close_payload(close, 1000, "bye");
   CHECK(graceful.push_close(std::span(close.data(), length), false) IS QueueResult::OKAY, "Close was refused");
   CHECK(graceful.close_queued() and (not graceful.close_sent()), "Close state is wrong after queuing");
   CHECK(graceful.push_message(Opcode::TEXT, to_bytes("c")) IS QueueResult::CLOSED, "Data was accepted after Close");
   CHECK(graceful.push_control(Opcode::PONG, {}) IS QueueResult::CLOSED, "Pong was accepted after Close");
   CHECK(graceful.push_close({}, false) IS QueueResult::CLOSED, "A second Close was accepted");
   output.clear();
   CHECK(drain_queue(graceful, output, 2), "Graceful close did not drain");
   CHECK(graceful.close_sent(), "Close was not reported as sent");
   result = read_stream(Role::CLIENT, output, output.size());
   CHECK(result.messages.size() IS 2 and result.close_codes.size() IS 1 and result.close_codes[0] IS 1000 and
      result.close_reason IS "bye", "Graceful close stream is wrong");

   // A failing Close follows the frame in progress; unstarted data and remaining fragments are dropped.

   SendQueue failing(Role::SERVER, nullptr, 10);
   failing.push_message(Opcode::BINARY, payload);
   failing.push_message(Opcode::BINARY, payload);
   failing.push_control(Opcode::PONG, to_bytes("x"));
   chunk = failing.next_chunk(scratch); // Starts the Pong
   failing.consume(1);
   length = encode_close_payload(close, 1002, "");
   failing.push_close(std::span(close.data(), length), true);
   CHECK(failing.pending() IS 2 + 4, "Discarded frames were still counted");
   output.assign(chunk.begin(), chunk.begin() + 1);
   CHECK(drain_queue(failing, output, SIZE_MAX), "Failing close did not drain");
   CHECK(output IS to_bytes(std::string_view("\x8a\x01x\x88\x02\x03\xea", 7)), "Failing close stream is wrong");

   SendQueue midway(Role::SERVER, nullptr, 10);
   midway.push_message(Opcode::BINARY, payload);
   chunk = midway.next_chunk(scratch);
   midway.consume(3);
   midway.push_close(std::span(close.data(), length), true);
   output.assign(chunk.begin(), chunk.begin() + 3);
   CHECK(drain_queue(midway, output, SIZE_MAX) and (output.size() IS 12 + 4), "Frame in progress was not completed");
   CHECK(output[12] IS 0x88, "Close did not follow the frame in progress");
   return true;
}

bool test_send_queue_limit(kt::Log &Log)
{
   SendQueue queue(Role::SERVER, nullptr, 0, 100);
   std::vector<uint8_t> payload(80, 'x');
   CHECK(queue.push_message(Opcode::BINARY, payload) IS QueueResult::OKAY, "Message within the limit was refused");
   CHECK(queue.pending() IS 82, "Pending size is wrong");

   std::vector<uint8_t> small(17, 'y'); // 19 encoded bytes exceed the remaining 18
   CHECK(queue.push_message(Opcode::BINARY, small) IS QueueResult::LIMIT, "Message beyond the limit was accepted");
   CHECK(queue.pending() IS 82, "A refused message changed the queue");
   CHECK(queue.push_control(Opcode::PING, small) IS QueueResult::LIMIT, "Ping beyond the limit was accepted");
   CHECK(queue.push_control(Opcode::PING, small, true) IS QueueResult::OKAY, "Exempt Ping was refused");
   CHECK(queue.push_control(Opcode::PONG, small) IS QueueResult::OKAY, "Pong was refused");

   std::vector<uint8_t> output;
   CHECK(drain_queue(queue, output, 7), "Queue did not drain");
   std::vector<uint8_t> exact(98, 'z'); // Encodes to exactly 100 bytes
   CHECK(queue.push_message(Opcode::BINARY, exact) IS QueueResult::OKAY, "Message at the limit was refused");

   SendQueue empty(Role::SERVER, nullptr, 0, 100);
   std::vector<uint8_t> oversized(99, 'o');
   CHECK(empty.push_message(Opcode::BINARY, oversized) IS QueueResult::LIMIT, "Oversized message was accepted");

   SendQueue unlimited(Role::SERVER, nullptr);
   std::vector<uint8_t> large(1024 * 1024);
   CHECK(unlimited.push_message(Opcode::BINARY, large) IS QueueResult::OKAY, "Unlimited queue refused a message");
   return true;
}

} // namespace

//********************************************************************************************************************

void protocol_unit_tests(int &Passed, int &Total)
{
   struct TestCase {
      const char *name;
      bool (*function)(kt::Log &Log);
   };
   constexpr std::array<TestCase, 28> tests = { {
      { "utf8_valid", test_utf8_valid },
      { "utf8_invalid", test_utf8_invalid },
      { "utf8_split", test_utf8_split },
      { "encode_rfc_examples", test_encode_rfc_examples },
      { "header_boundaries", test_header_boundaries },
      { "apply_mask_offsets", test_apply_mask_offsets },
      { "non_minimal_lengths", test_non_minimal_lengths },
      { "reserved_bits_and_opcodes", test_reserved_bits_and_opcodes },
      { "control_frames", test_control_frames },
      { "masking_by_role", test_masking_by_role },
      { "fragment_sequencing", test_fragment_sequencing },
      { "split_feeding", test_split_feeding },
      { "fragmented_text_utf8", test_fragmented_text_utf8 },
      { "size_limits", test_size_limits },
      { "close_payloads", test_close_payloads },
      { "accept_key", test_accept_key },
      { "keys", test_keys },
      { "header_tokens", test_header_tokens },
      { "parse_location", test_parse_location },
      { "parse_header_fields", test_parse_header_fields },
      { "validate_response", test_validate_response },
      { "find_request_end", test_find_request_end },
      { "parse_request", test_parse_request },
      { "server_responses", test_server_responses },
      { "send_queue_encoding", test_send_queue_encoding },
      { "send_queue_partial_writes", test_send_queue_partial_writes },
      { "send_queue_control_frames", test_send_queue_control_frames },
      { "send_queue_limit", test_send_queue_limit }
   } };

   for (const auto &test : tests) {
      kt::Log log("WebSocketTests");
      log.branch("Running %s", test.name);
      Total++;
      if (test.function(log)) {
         Passed++;
         log.msg("%s passed", test.name);
      }
      else log.error("%s failed", test.name);
   }
}

#endif // UNIT_TESTS
