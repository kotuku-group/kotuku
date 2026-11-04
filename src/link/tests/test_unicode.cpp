#include "../unicode.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

static bool fail(const char *Message)
{
   std::fprintf(stderr, "%s\n", Message);
   return false;
}

static bool check_encoding()
{
   struct Example { int value; std::string_view bytes; };
   const Example examples[] = {
      { 0, std::string_view("\0", 1) }, { 0x7f, "\x7f" }, { 0x80, "\xc2\x80" },
      { 0x7ff, "\xdf\xbf" }, { 0x800, "\xe0\xa0\x80" }, { 0xd7ff, "\xed\x9f\xbf" },
      { 0xe000, "\xee\x80\x80" }, { 0xffff, "\xef\xbf\xbf" },
      { 0x10000, "\xf0\x90\x80\x80" }, { 0x10ffff, "\xf4\x8f\xbf\xbf" }
   };
   for (auto &example : examples) {
      for (size_t size = 0; size <= 5; ++size) {
         std::array<int8_t, 5> buffer;
         buffer.fill(0x55);
         auto written = UTF8WriteValue(example.value, std::span(buffer).first(size));
         auto expected = size < example.bytes.size() ? 0 : int(example.bytes.size());
         if (written != expected) return fail("Encoding length or capacity check failed");
         if (written and std::string_view((const char *)buffer.data(), written) != example.bytes) {
            return fail("Encoded bytes differ from boundary vector");
         }
         if (not std::all_of(buffer.begin() + written, buffer.end(), [](auto Byte) { return Byte IS 0x55; })) {
            return fail("Encoding changed bytes outside its output");
         }
      }
   }
   for (int value : { -1, std::numeric_limits<int>::min(), 0xd800, 0xdbff, 0xdc00, 0xdfff,
                      0x110000, 0x1fffff, 0x200000, 0x4000000, std::numeric_limits<int>::max() }) {
      std::array<int8_t, 6> buffer;
      buffer.fill(0x55);
      if (UTF8WriteValue(value, buffer) != 0) return fail("Invalid scalar was encoded");
      if (not std::all_of(buffer.begin(), buffer.end(), [](auto Byte) { return Byte IS 0x55; })) {
         return fail("Invalid scalar modified the output buffer");
      }
   }
   return true;
}

static bool check_decoding()
{
   for (auto input : std::array<std::string_view, 2> { std::string_view{}, "" }) {
      int length = -1;
      if (UTF8ReadValue(input, &length) != 0 or length != 0) return fail("Empty input contract failed");
      if (UTF8ReadValue(input, nullptr) != 0) return fail("Optional length handling failed");
   }
   const char *invalid[] = {
      "\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xc2", "\xc2\x7f", "\xc2\xc0",
      "\xe0\x80\x80", "\xe0\x9f\xbf", "\xe1", "\xe1\x80", "\xe1\x80" "A",
      "\xed\xa0\x80", "\xed\xbf\xbf", "\xf0\x80\x80\x80", "\xf0\x8f\xbf\xbf",
      "\xf1", "\xf1\x80", "\xf1\x80\x80", "\xf1\x80\x80" "A",
      "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xf8\x88\x80\x80\x80",
      "\xfc\x84\x80\x80\x80\x80", "\xfe", "\xff"
   };
   for (auto input : invalid) {
      // Exact-sized allocations let sanitisers detect reads beyond unterminated input.
      auto size = std::char_traits<char>::length(input);
      auto text = new char[size];
      std::copy_n(input, size, text);
      int length = -1;
      auto value = UTF8ReadValue(std::string_view(text, size), &length);
      auto without_length = UTF8ReadValue(std::string_view(text, size), nullptr);
      delete[] text;
      if (value != 0 or length != 1 or without_length != 0) return fail("Malformed UTF-8 was not rejected");
   }
   return true;
}

static bool check_all_scalars()
{
   for (int value = 0; value <= 0x10ffff; ++value) {
      if ((value >= 0xd800) and (value <= 0xdfff)) continue;
      std::array<int8_t, 5> buffer{};
      auto written = UTF8WriteValue(value, std::span(buffer).first(4));
      int length = -1;
      auto decoded = UTF8ReadValue(std::string_view((const char *)buffer.data(), written), &length);
      if (written < 1 or written > 4 or decoded != uint32_t(value) or length != written) {
         return fail("Unicode scalar round trip failed");
      }
   }
   return true;
}

static bool check_bounded_input()
{
   const char bytes[] = { char(0xf0), char(0x9f), char(0x98), char(0x80), '\0', 'A' };
   std::string_view text(bytes, sizeof(bytes));
   for (size_t size = 1; size < 4; ++size) {
      int length = -1;
      if (UTF8ReadValue(text.substr(0, size), &length) != 0 or length != 1) {
         return fail("Decoding read beyond the supplied view");
      }
   }
   int length = -1;
   if (UTF8ReadValue(text.substr(0, 4), &length) != 0x1f600 or length != 4) {
      return fail("Unterminated character decoding failed");
   }
   if (UTF8ReadValue(text.substr(4), &length) != 0 or length != 1) {
      return fail("Embedded NUL did not decode as one byte");
   }
   if (UTF8ReadValue(text.substr(5), &length) != 'A' or length != 1) {
      return fail("Decoding after embedded NUL failed");
   }
   return true;
}

int main()
{
   return check_encoding() and check_decoding() and check_all_scalars() and check_bounded_input() ? 0 : 1;
}
