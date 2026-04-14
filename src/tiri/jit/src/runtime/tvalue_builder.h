// TValue builder utilities for LuaJIT.
// Copyright © 2025-2026 Paul Manias.
//
// Type-safe RAII wrapper for constructing and modifying TValue objects with fluent chaining interface.

#pragma once

#include "lj_obj.h"

//********************************************************************************************************************
// TValueBuilder: Fluent interface for TValue construction
//
// Provides type-safe wrappers for common TValue manipulations with method chaining.  Useful for constructing values
// on the stack or in other locations with clear, readable code that avoids raw macro usage.
//
// Usage examples:
//    TValueBuilder(L, L->top++).set_int(42);
//    TValueBuilder(L, dst).copy_from(src);
//    TValueBuilder(L, result).set_bool(true);
//
// All methods return *this to enable chaining:
//    TValueBuilder(L, slot).set_nil().copy_from(other);  // Second call overwrites first
//
// Note: This is a lightweight wrapper with no overhead - it simply provides a more ergonomic interface to the
// underlying TValue manipulation macros.

class TValueBuilder {
   TValue* tv_;
   lua_State* L_;

public:
   // Construct a builder for a specific TValue slot
   constexpr TValueBuilder(lua_State* L, TValue* tv) noexcept
      : tv_(tv), L_(L) {}

   // -- Primitive value setters --

   // Set to nil
   constexpr TValueBuilder & set_nil() noexcept {
      setnilV(tv_);
      return *this;
   }

   // Set to boolean value
   constexpr TValueBuilder & set_bool(bool b) noexcept {
      setboolV(tv_, b);
      return *this;
   }

   // Set to primitive value (nil, true, false) using raw tag
   constexpr TValueBuilder & set_pri(uint32_t x) noexcept {
      setpriV(tv_, x);
      return *this;
   }

   // -- Numeric value setters --

   // Set to 32-bit integer (uses dual-number representation if enabled)
   constexpr TValueBuilder & set_int(int32_t i) noexcept {
      setintV(tv_, i);
      return *this;
   }

   // Set to 64-bit integer (converts to int32_t if possible, else lua_Number)
   constexpr TValueBuilder & set_int64(int64_t i) noexcept {
      setint64V(tv_, i);
      return *this;
   }

   // Set to intptr_t (platform-dependent integer size)
   constexpr TValueBuilder & set_intptr(intptr_t i) noexcept {
      setintptrV(tv_, i);
      return *this;
   }

   // Set to lua_Number (double)
   constexpr TValueBuilder & set_num(lua_Number n) noexcept {
      setnumV(tv_, n);
      return *this;
   }

   // Set to NaN
   constexpr TValueBuilder & set_nan() noexcept {
      setnanV(tv_);
      return *this;
   }

   // Set to positive infinity
   constexpr TValueBuilder & set_pinf() noexcept {
      setpinfV(tv_);
      return *this;
   }

   // Set to negative infinity
   constexpr TValueBuilder & set_minf() noexcept {
      setminfV(tv_);
      return *this;
   }

   // -- GC object setters (require lua_State for write barriers) --

   // Set to GC string
   constexpr TValueBuilder & set_str(GCstr* s) noexcept {
      setstrV(L_, tv_, s);
      return *this;
   }

   // Set to table
   constexpr TValueBuilder & set_tab(GCtab* t) noexcept {
      settabV(L_, tv_, t);
      return *this;
   }

   // Set to function (C or Lua)
   constexpr TValueBuilder & set_func(GCfunc* f) noexcept {
      setfuncV(L_, tv_, f);
      return *this;
   }

   // Set to thread (coroutine)
   constexpr TValueBuilder & set_thread(lua_State* th) noexcept {
      setthreadV(L_, tv_, th);
      return *this;
   }

   // Set to userdata
   constexpr TValueBuilder & set_udata(GCudata* u) noexcept {
      setudataV(L_, tv_, u);
      return *this;
   }

   // Set to object
   constexpr TValueBuilder & set_object(GCobject *obj) noexcept {
      setobjectV(L_, tv_, obj);
      return *this;
   }

   // Set to prototype (internal function prototype)
   constexpr TValueBuilder & set_proto(GCproto* p) noexcept {
      setprotoV(L_, tv_, p);
      return *this;
   }

   // Set to light userdata (raw pointer, no GC)
   // Note: Uses setrawlightudV which doesn't require lua_State
   constexpr TValueBuilder & set_lightud(void* p) noexcept {
      setrawlightudV(tv_, p);
      return *this;
   }

   // -- Value copying --

   // Copy from another TValue (includes write barrier check)
   constexpr TValueBuilder & copy_from(const TValue* src) noexcept {
      copyTV(L_, tv_, src);
      return *this;
   }

   // -- Accessors --

   [[nodiscard]] constexpr TValue * get() const noexcept { return tv_; }
   [[nodiscard]] constexpr lua_State * state() const noexcept { return L_; }

   // Prevent copying (would be confusing which TValue is being modified)
   TValueBuilder(const TValueBuilder &) = delete;
   TValueBuilder & operator=(const TValueBuilder &) = delete;

   // Allow moving (transfers ownership of the TValue slot)
   constexpr TValueBuilder(TValueBuilder && other) noexcept
      : tv_(other.tv_), L_(other.L_) {
      other.tv_ = nullptr;
   }

   constexpr TValueBuilder & operator=(TValueBuilder && other) noexcept {
      if (this != &other) {
         tv_ = other.tv_;
         L_ = other.L_;
         other.tv_ = nullptr;
      }
      return *this;
   }
};
