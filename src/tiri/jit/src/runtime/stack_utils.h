// Stack management utilities for LuaJIT.
// Copyright © 2025-2026 Paul Manias.
//
// RAII helpers for automatic stack management in library and API functions.

#pragma once

#include "lj_obj.h"

//********************************************************************************************************************
// StackFrame: RAII wrapper for automatic L->top restoration
//
// Ensures L->top is restored when the guard goes out of scope, preventing
// stack leaks even with early returns or error paths.
//
// Basic Usage:
//    StackFrame frame(L);
//    // ... push values to stack ...
//    frame.commit(nresults);  // Keep nresults on stack, disarm guard
//    // OR let destructor restore to saved_top
//
// Common Patterns:
//
// 1. Return N results (standard library function):
//    StackFrame frame(L);
//    setintV(L->top++, 42);
//    setstrV(L, L->top++, result_str);
//    frame.commit(2);  // Keep 2 results on stack
//    return 2;
//
// 2. Manual stack management (metamethods, tail calls):
//    StackFrame frame(L);
//    // ... complex operations ...
//    L->top = final_position;  // Manually set final position
//    frame.disarm();           // Prevent restoration
//    return nresults;
//
// 3. Error path cleanup (automatic):
//    StackFrame frame(L);
//    // ... push temporary values ...
//    if (error_condition) {
//       lj_err_arg(L, 1, LJ_ERR_INVARG);  // Stack auto-restored
//    }
//    frame.commit(1);
//    return 1;
//
// 4. No results returned:
//    StackFrame frame(L);
//    // ... work with stack ...
//    frame.commit(0);  // Clear any temporaries
//    return 0;
//
// When to use commit() vs disarm():
// - Use commit(n) when you want to keep exactly n values on the stack
// - Use disarm() when you've manually set L->top to the desired position
// - Let destructor run (neither) only for error paths that throw/longjmp
//
// Performance: Zero overhead - all methods are constexpr noexcept and inline.
// The guard compiles down to a single conditional branch in the destructor.

class StackFrame {
   lua_State *L_;
   TValue *saved_top_;

public:
   explicit StackFrame(lua_State* L) noexcept
      : L_(L), saved_top_(L->top) {}

   ~StackFrame() noexcept {
      if (L_) L_->top = saved_top_;
   }

   // Disarm the guard without restoring
   constexpr void disarm() noexcept { L_ = nullptr; }

   // Commit n results and disarm (keeps n values on stack)
   constexpr void commit(int nresults) noexcept {
      L_->top = saved_top_ + nresults;
      L_ = nullptr;
   }

   [[nodiscard]] constexpr TValue * saved() const noexcept { return saved_top_; }

   // Prevent copying
   StackFrame(const StackFrame&) = delete;
   StackFrame& operator=(const StackFrame&) = delete;

   // Allow moving
   StackFrame(StackFrame&& other) noexcept
      : L_(other.L_), saved_top_(other.saved_top_) {
      other.L_ = nullptr;
   }

   StackFrame& operator=(StackFrame&& other) noexcept {
      if (this != &other) {
         if (L_) L_->top = saved_top_;
         L_ = other.L_;
         saved_top_ = other.saved_top_;
         other.L_ = nullptr;
      }
      return *this;
   }
};
