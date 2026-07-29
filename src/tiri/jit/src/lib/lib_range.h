// Range library header for Tiri.
// Copyright © 2025-2026 Paul Manias.

#pragma once

#include <cstdint>

#include "lua.h"

struct lua_State;
union TValue;
using cTValue = const TValue;

// Range structure - stored as userdata payload

struct tiri_range {
   lua_Number start;  // First sequence value
   lua_Number stop;   // Sequence boundary (exclusive by default)
   lua_Number step;   // Non-zero sequence increment
   bool inclusive;    // If true, stop is included (default: false)
};

struct tiri_index_range {
   int32_t start;
   int32_t stop;
   int32_t step;
   bool inclusive;
};

enum RangePrepareFlag : uint32_t {
   RANGE_PREP_INCLUSIVE = 1u << 0,
   RANGE_PREP_HAS_STEP = 1u << 1
};

// Metatable name for range userdata

static constexpr const char* RANGE_METATABLE = "Tiri.range";

// Check if a stack value at the given index is a range userdata.
// Returns the tiri_range pointer if it is, nullptr otherwise.

tiri_range *check_range(lua_State *L, int idx);

// Check if a TValue is a range userdata (for use in metamethod implementations).
// Returns the tiri_range pointer if it is, nullptr otherwise.

tiri_range *check_range_tv(lua_State *L, cTValue *tv);

// Convert a range to collection indices without truncating fractional values.

void range_check_index(lua_State *L, const tiri_range *Range, tiri_index_range *Result);

// Prepare and generate values for direct range-loop bytecode.

lua_Number lj_range_prepare_step(
   lua_State *L, lua_Number Start, lua_Number Stop, lua_Number Step, uint32_t Flags);
lua_Number lj_range_prepare_count(
   lua_State *L, lua_Number Start, lua_Number Stop, lua_Number Step, uint32_t Inclusive);
int32_t lj_range_integer_values(lua_Number Start, lua_Number Stop, lua_Number Step);
lua_Number lj_range_value(lua_Number Ordinal, lua_Number Start, lua_Number Step);
extern "C" void lj_range_prepare(lua_State *L, TValue *Base, uint32_t Flags);
extern "C" void lj_range_value_at(TValue *Base);

// Slice function for tables and strings

int lj_range_slice(lua_State *L);
