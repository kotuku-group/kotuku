#include "../precis/opaque_string.h"

#include <utf8proc.h>

#include <cstdio>
#include <string>
#include <string_view>

static bool expect(std::string_view Input, std::string_view Expected) {
   std::string output;
   return rtc::precis::prepare_opaque_string(Input, output) and output IS Expected;
}

static bool reject(std::string_view Input) {
   std::string output = "untouched";
   return not rtc::precis::prepare_opaque_string(Input, output) and output IS "untouched";
}

int main() {
   // Exercise both null sizing and exhausted output buffers in recursive compatibility decomposition.
   const auto ligature = (const utf8proc_uint8_t *)"\xef\xac\x83"; // LATIN SMALL LIGATURE FFI
   auto options = utf8proc_option_t(UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT);
   utf8proc_int32_t first = 0;
   if (utf8proc_decompose(ligature, 3, nullptr, 0, options) != 3 or
       utf8proc_decompose(ligature, 3, &first, 1, options) != 3 or first != 'f') {
      std::fprintf(stderr, "utf8proc sizing or partial decomposition differs\n");
      return 1;
   }

   if (not expect("CaseSensitive", "CaseSensitive") or
       not expect("A\xcc\x8a", "\xc3\x85") or
       not expect("foo\xe1\x9a\x80" "bar", "foo bar") or
       not expect("\xef\xbc\xa6", "\xef\xbc\xa6") or
       not expect("\xc2\xb2", "\xc2\xb2")) {
      std::fprintf(stderr, "OpaqueString mapping or NFC differs\n");
      return 1;
   }
   const char supplementary[] = { char(0xf0), char(0x9f), char(0x98), char(0x80) };
   auto bounded = std::string_view(supplementary, sizeof(supplementary));
   if (not expect(bounded, bounded) or not reject(bounded.substr(0, 3)) or
       not reject(std::string_view("a\0b", 3)) or not reject("\x80") or
       not reject("\xc0\x80") or not reject("\xf4\x90\x80\x80")) {
      std::fprintf(stderr, "OpaqueString bounded UTF-8 validation differs\n");
      return 1;
   }
   if (not expect("l\xc2\xb7l", "l\xc2\xb7l") or
       not reject("a\xc2\xb7" "b") or
       not expect("\xcd\xb5\xce\xb1", "\xcd\xb5\xce\xb1") or
       not reject("\xcd\xb5" "a") or
       not expect("\xd7\x90\xd7\xb3", "\xd7\x90\xd7\xb3") or
       not reject("a\xd7\xb3") or
       not expect("\xe3\x82\xab\xe3\x83\xbb", "\xe3\x82\xab\xe3\x83\xbb") or
       not reject("a\xe3\x83\xbb")) {
      std::fprintf(stderr, "OpaqueString script context differs\n");
      return 1;
   }
   if (not expect("\xd8\xa8\xe2\x80\x8c\xd8\xa7", "\xd8\xa8\xe2\x80\x8c\xd8\xa7") or
       not reject("a\xe2\x80\x8c" ) or
       not expect("\xd9\xa0\xd9\xa1", "\xd9\xa0\xd9\xa1") or
       not reject("\xd9\xa0\xdb\xb0")) {
      std::fprintf(stderr, "OpaqueString joining or digit context differs\n");
      return 1;
   }
   if (not reject("") or not reject("a\t") or not reject("\xc2\xad") or
       not reject("\xed\xa0\x80") or not reject("\xcd\xb8") or
       not reject("\xee\x80\x80") or not reject("\xef\xb7\x90") or
       not reject("\xe1\x84\x80")) {
      std::fprintf(stderr, "OpaqueString accepted malformed or disallowed input\n");
      return 1;
   }
   return 0;
}
