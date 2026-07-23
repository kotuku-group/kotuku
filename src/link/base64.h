#pragma once

#include <kotuku/main.h>

#include <string_view>
#include <span>

namespace kt {

struct BASE64DECODE {
   uint8_t Step;             // Internal
   uint8_t PlainChar;        // Internal
   uint8_t Initialised:1;    // Internal
  BASE64DECODE() : Step(0), PlainChar(0), Initialised(0) { };
};

struct BASE64ENCODE {
   uint8_t Step;     // Internal
   uint8_t Result;   // Internal
   int  StepCount;   // Internal
  BASE64ENCODE() : Step(0), Result(0), StepCount(0) { };
};

const int CHARS_PER_LINE = 72;

int64_t Base64Encode(BASE64ENCODE *, std::span<const char>, std::span<char>);
ERR Base64Decode(BASE64DECODE *, std::string_view, std::span<int8_t>, int64_t *);

};
