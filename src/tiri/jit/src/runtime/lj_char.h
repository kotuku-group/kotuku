/*
** Character types.
** Donated to the public domain.
*/

#pragma once

#include "lj_def.h"

constexpr uint8_t LJ_CHAR_CNTRL = 0x01;
constexpr uint8_t LJ_CHAR_SPACE = 0x02;
constexpr uint8_t LJ_CHAR_PUNCT = 0x04;
constexpr uint8_t LJ_CHAR_DIGIT = 0x08;
constexpr uint8_t LJ_CHAR_XDIGIT = 0x10;
constexpr uint8_t LJ_CHAR_UPPER = 0x20;
constexpr uint8_t LJ_CHAR_LOWER = 0x40;
constexpr uint8_t LJ_CHAR_IDENT = 0x80;
constexpr uint8_t LJ_CHAR_ALPHA = (LJ_CHAR_LOWER | LJ_CHAR_UPPER);
constexpr uint8_t LJ_CHAR_ALNUM = (LJ_CHAR_ALPHA | LJ_CHAR_DIGIT);
constexpr uint8_t LJ_CHAR_GRAPH = (LJ_CHAR_ALNUM | LJ_CHAR_PUNCT);

// Only pass -1 or 0..255 to these macros. Never pass a signed char!
#define lj_char_isa(c, t)   ((lj_char_bits+1)[(c)] & t)
#define lj_char_ispunct(c)   lj_char_isa((c), LJ_CHAR_PUNCT)  // std::ispunct
#define lj_char_isident(c)   lj_char_isa((c), LJ_CHAR_IDENT)

LJ_DATA const uint8_t lj_char_bits[257];
