
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define EXP_ACCESS_VIOLATION    1
#define EXP_BREAKPOINT          2
#define EXP_MISALIGNED_DATA     3
#define EXP_INVALID_CALCULATION 4
#define EXP_DIVIDE_BY_ZERO      5
#define EXP_ILLEGAL_INSTRUCTION 6
#define EXP_STACK_OVERFLOW      7
#define EXP_END                 8 // Array size

#define TSTD_IN  0x01
#define TSTD_OUT 0x02
#define TSTD_ERR 0x04

struct rkTimeZoneTransition {
   int64_t Instant = 0;
   std::string Abbreviation;
   int OffsetBefore = 0;
   int OffsetAfter = 0;
   int DaylightSaving = 0;
};

struct rkTimeZoneInfo {
   std::string ZoneID;
   std::string NativeID;
   std::string Source;
   std::string DataPath;
   std::string Version;
   std::vector<rkTimeZoneTransition> Transitions;
   int BaseOffset = 0;
   int TransitionCount = 0;
   int TransitionsWritten = 0;
   int StartYear = 0;
   int EndYear = 0;
   int IsLocal = 0;
   int IsFallback = 0;
};
