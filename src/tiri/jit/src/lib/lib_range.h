// Range library header for Tiri.
// Copyright © 2025-2026 Paul Manias.

#pragma once

#include <cstdint>

struct lua_State;
union TValue;
using cTValue = const TValue;

// Range structure - stored as userdata payload

struct tiri_range {
   int32_t start;     // Start index (always inclusive)
   int32_t stop;      // End index (exclusive by default)
   int32_t step;      // Step value (default: 1, or -1 for reverse)
   bool inclusive;    // If true, stop is included (default: false)
};

// Metatable name for range userdata

static constexpr const char* RANGE_METATABLE = "Tiri.range";

// Check if a stack value at the given index is a range userdata.  Returns the tiri_range pointer if it is, nullptr otherwise.

tiri_range *check_range(lua_State *L, int idx);

// Check if a TValue is a range userdata (for use in metamethod implementations).  Returns the tiri_range pointer if it is, nullptr otherwise.

tiri_range *check_range_tv(lua_State *L, cTValue *tv);

// Slice function for tables and strings

int lj_range_slice(lua_State *L);
