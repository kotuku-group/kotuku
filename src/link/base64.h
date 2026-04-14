#pragma once

#include <kotuku/main.h>

namespace pf {

struct BASE64DECODE {
   uint8_t Step;             // Internal
   uint8_t PlainChar;        // Internal
   uint8_t Initialised:1;    // Internal
  BASE64DECODE() : Step(0), PlainChar(0), Initialised(0) { };
};

struct BASE64ENCODE {
   uint8_t Step;        // Internal
   uint8_t Result;      // Internal
   int  StepCount;   // Internal
  BASE64ENCODE() : Step(0), Result(0), StepCount(0) { };
};

const int CHARS_PER_LINE = 72;

int Base64Encode(BASE64ENCODE *, const void *, int, STRING, int );
ERR Base64Decode(BASE64DECODE *, CSTRING, int, APTR, int *);

};
