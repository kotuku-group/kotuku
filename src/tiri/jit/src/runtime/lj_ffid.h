// Fast-function and built-in callable identities shared with bootstrap host tools.
// Copyright (C) 2026 Paul Manias

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

inline constexpr uint8_t FF_LUA = 0;
inline constexpr uint8_t FF_C   = 1;

enum class BuiltinCallableID : uint16_t {
   Invalid = std::numeric_limits<uint16_t>::max()
};

[[nodiscard]] inline constexpr unsigned int builtin_callable_index(BuiltinCallableID Value) noexcept
{
   return unsigned(Value);
}

[[nodiscard]] inline constexpr bool builtin_callable_assigned(BuiltinCallableID Value) noexcept
{
   return Value != BuiltinCallableID::Invalid;
}

// GCfunc::ffid is an 8-bit field.  Keep the state layout independent of the generated fast-function count so buildvm
// can compile before it creates lj_ffdef.h.

inline constexpr size_t BUILTIN_CALLABLE_CAPACITY = size_t(std::numeric_limits<uint8_t>::max()) + 1;
