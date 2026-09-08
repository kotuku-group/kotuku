// Error Handling Utilities for LuaJIT
// Copyright © 2025-2026 Paul Manias.

#pragma once

#include "lj_obj.h"
#include "lj_err.h"

// CheckGuard: RAII-style argument validation guard
//
// Provides automatic error handling for argument validation. When a check fails, it throws an error via lj_err_arg()
// using longjmp.
//
// Note: This is NOT exception-based error handling. LuaJIT uses longjmp for error unwinding. The "guard" terminology
// refers to ensuring arguments are valid before proceeding with the operation.
//
// Usage:
//    CheckGuard check_positive(L, 1, x > 0, ErrMsg::BADVAL);
//    if (not check_positive.passed()) {
//       // Error already thrown via longjmp, this line is unreachable
//    }
//
// Or more commonly, just construct it for the side effect:
//    CheckGuard(L, arg, condition, ErrMsg::BADVAL);
//
// The guard automatically validates on construction. If the condition fails, it immediately calls lj_err_arg() which
// performs a longjmp.

class CheckGuard {
   lua_State *L_;
   int  arg_;
   bool passed_;

public:
   CheckGuard(lua_State *L, int arg, bool condition, ErrMsg msg) : L_(L), arg_(arg), passed_(condition) {
      if (not passed_) lj_err_arg(L, arg, msg);
   }

   [[nodiscard]] constexpr bool passed() const noexcept { return passed_; }

   // Prevent copying and moving (single-use guard)
   CheckGuard(const CheckGuard &) = delete;
   CheckGuard & operator=(const CheckGuard &) = delete;
   CheckGuard(CheckGuard &&) = delete;
   CheckGuard & operator=(CheckGuard &&) = delete;
};

// RangeGuard: RAII-style range validation guard
//
// Validates that a value is within an acceptable range. Throws an error if the value is out of bounds.
//
// Usage:
//    int32_t idx = lj_lib_checkint(L, 1);
//    RangeGuard(L, 1, idx, 0, max_value, ErrMsg::IDXRNG);

class RangeGuard {
   lua_State* L_;
   int arg_;
   bool passed_;

public:
   template<typename T>
   RangeGuard(lua_State* L, int arg, T value, T min_val, T max_val, ErrMsg msg)
      : L_(L), arg_(arg), passed_(value >= min_val and value <= max_val)
   {
      if (not passed_) lj_err_arg(L, arg, msg);
   }

   [[nodiscard]] constexpr bool passed() const noexcept { return passed_; }

   // Prevent copying and moving
   RangeGuard(const RangeGuard &) = delete;
   RangeGuard & operator=(const RangeGuard &) = delete;
   RangeGuard(RangeGuard &&) = delete;
   RangeGuard & operator=(RangeGuard &&) = delete;
};

// TypeGuard: RAII-style type validation guard
//
// Validates that a TValue has the expected type. Throws an error if the type
// doesn't match.
//
// Usage:
//    TValue* o = L->base + narg - 1;
//    TypeGuard(L, narg, o, LUA_TTABLE, ErrMsg::NOTABN);

class TypeGuard {
   lua_State* L_;
   int arg_;
   bool passed_;

public:
   TypeGuard(lua_State* L, int arg, const TValue* o, int expected_type)
      : L_(L), arg_(arg), passed_(false)
   {
      // Check type match based on expected type
      switch (expected_type) {
         case LUA_TNIL:      passed_ = tvisnil(o); break;
         case LUA_TBOOLEAN:  passed_ = tvisbool(o); break;
         case LUA_TNUMBER:   passed_ = tvisnumber(o); break;
         case LUA_TSTRING:   passed_ = tvisstr(o); break;
         case LUA_TTABLE:    passed_ = tvistab(o); break;
         case LUA_TFUNCTION: passed_ = tvisfunc(o); break;
         case LUA_TUSERDATA: passed_ = tvisudata(o); break;
         default:            passed_ = false; break;
      }

      if (not passed_) lj_err_argt(L, arg, expected_type);
   }

   [[nodiscard]] constexpr bool passed() const noexcept { return passed_; }

   // Prevent copying and moving
   TypeGuard(const TypeGuard &) = delete;
   TypeGuard & operator=(const TypeGuard &) = delete;
   TypeGuard(TypeGuard &&) = delete;
   TypeGuard & operator=(TypeGuard &&) = delete;
};

// NotNilGuard: RAII-style nil validation guard
//
// Validates that a value is not nil. This is a common pattern in library functions that require a value to be present.
//
// Usage:
//    TValue* o = L->base + narg - 1;
//    NotNilGuard(L, narg, o);

class NotNilGuard {
   lua_State* L_;
   int arg_;
   bool passed_;

public:
   NotNilGuard(lua_State* L, int arg, const TValue* o)
      : L_(L), arg_(arg), passed_(not tvisnil(o))
   {
      if (not passed_) lj_err_arg(L, arg, ErrMsg::NOVAL);
   }

   [[nodiscard]] constexpr bool passed() const noexcept { return passed_; }

   // Prevent copying and moving
   NotNilGuard(const NotNilGuard &) = delete;
   NotNilGuard & operator=(const NotNilGuard &) = delete;
   NotNilGuard(NotNilGuard &&) = delete;
   NotNilGuard & operator=(NotNilGuard &&) = delete;
};

// Convenience macros for common validation patterns
//
// These macros create CheckGuard instances with meaningful names for debugging.
// The variable name includes the line number to avoid collisions when multiple
// checks are performed in the same scope.

#define LJ_CHECK_ARG(L, arg, cond, msg) \
   CheckGuard _check_arg_##arg##_line_##__LINE__(L, arg, cond, msg)

#define LJ_CHECK_RANGE(L, arg, val, min, max, msg) \
   RangeGuard _range_guard_##arg##_line_##__LINE__(L, arg, val, min, max, msg)

#define LJ_CHECK_TYPE(L, arg, o, type) \
   TypeGuard _type_guard_##arg##_line_##__LINE__(L, arg, o, type)

#define LJ_CHECK_NOT_NIL(L, arg, o) \
   NotNilGuard _not_nil_guard_##arg##_line_##__LINE__(L, arg, o)

// Inline validation helpers
//
// These provide convenient inline validation without creating guard objects.
// Useful when you need the validation but don't need to check the result.

[[nodiscard]] inline bool check_arg_count(lua_State* L, int required) noexcept {
   return (L->top - L->base) >= required;
}

[[nodiscard]] inline bool check_arg_range(lua_State* L, int narg) noexcept {
   return (L->base + narg - 1) < L->top;
}

template<typename T>
[[nodiscard]] inline bool value_in_range(T value, T min_val, T max_val) noexcept {
   return value >= min_val and value <= max_val;
}

// Error message builders
//
// These helpers construct error messages for common validation patterns.

inline void require_arg_count(lua_State* L, int required) {
   if (not check_arg_count(L, required)) {
      lj_err_arg(L, required, ErrMsg::NOVAL);
   }
}

inline void require_arg_in_range(lua_State* L, int narg) {
   if (not check_arg_range(L, narg)) {
      lj_err_arg(L, narg, ErrMsg::NOVAL);
   }
}

template<typename T>
inline void require_value_range(lua_State* L, int narg, T value, T min_val, T max_val) {
   if (not value_in_range(value, min_val, max_val)) {
      lj_err_arg(L, narg, ErrMsg::IDXRNG);
   }
}
