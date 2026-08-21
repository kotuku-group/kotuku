// Fast function IDs.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lj_ffid.h"
#include <array>

// Fast function ID.

enum class FastFunc : unsigned int {
   LUA_ = FF_LUA,   //  Lua function (must be 0) - see lj_obj.h
   C_ = FF_C,       //  Regular C function (must be 1) - see lj_obj.h
#define FFDEF(name, ...)   name,
#include "lj_ffdef.h"
   _MAX
};

// Backward compatibility aliases
inline constexpr unsigned int FF_LUA_ = unsigned(FastFunc::LUA_);
inline constexpr unsigned int FF_C_ = unsigned(FastFunc::C_);
#define FFDEF(name, ...)   inline constexpr unsigned int FF_##name = unsigned(FastFunc::name);
#include "lj_ffdef.h"
inline constexpr unsigned int FF__MAX = unsigned(FastFunc::_MAX);

[[nodiscard]] inline constexpr BuiltinCallableID builtin_callable_id(FastFunc Value) noexcept
{
   return BuiltinCallableID(unsigned(Value));
}

[[nodiscard]] inline constexpr bool builtin_callable_valid(BuiltinCallableID Value) noexcept
{
   unsigned int index = builtin_callable_index(Value);
   return index > FF_C_ and index < FF__MAX;
}

inline constexpr std::array<const char *, FF__MAX> glBuiltinCallableNames = {
   nullptr,
   nullptr,
#define FF_STRINGIFY_INNER(value) #value
#define FF_STRINGIFY(value) FF_STRINGIFY_INNER(value)
#define FF_CANONICAL_SELECT(fallback, canonical, ...) canonical
#define FF_CANONICAL_IMPL(fallback, ...) FF_CANONICAL_SELECT(fallback __VA_OPT__(,) __VA_ARGS__, fallback)
#define FF_CANONICAL(name, ...) FF_CANONICAL_IMPL(FF_STRINGIFY(name) __VA_OPT__(,) __VA_ARGS__)
#define FFDEF(name, ...) FF_CANONICAL(name __VA_OPT__(,) __VA_ARGS__),
#include "lj_ffdef.h"
#undef FF_CANONICAL
#undef FF_CANONICAL_IMPL
#undef FF_CANONICAL_SELECT
#undef FF_STRINGIFY
#undef FF_STRINGIFY_INNER
};

[[nodiscard]] inline constexpr const char *builtin_callable_name(BuiltinCallableID Value) noexcept
{
   return builtin_callable_valid(Value) ? glBuiltinCallableNames[builtin_callable_index(Value)] : nullptr;
}

[[nodiscard]] inline constexpr uint64_t builtin_callable_abi_fingerprint() noexcept
{
   uint64_t hash = 1469598103934665603ull;
   for (const char *name : glBuiltinCallableNames) {
      if (not name) continue;
      while (*name) {
         hash ^= uint8_t(*name++);
         hash *= 1099511628211ull;
      }
      hash ^= 0xffu;
      hash *= 1099511628211ull;
   }
   return hash;
}

static_assert(FF__MAX > FF_C_ + 1);
static_assert(FF__MAX <= BUILTIN_CALLABLE_CAPACITY);
static_assert(FF__MAX - 1 <= std::numeric_limits<uint16_t>::max());
static_assert(builtin_callable_index(BuiltinCallableID::Invalid) >= FF__MAX);
#ifdef FFDEF_BFUNC_ABI
static_assert(builtin_callable_abi_fingerprint() IS 0x62328237390d0b55ull,
   "fast-function ordering changed: bump BCDUMP_VERSION and update the BC_BFUNC ABI fingerprint");
#endif

struct lua_State;
union GCfunc;

void lj_builtin_register(lua_State *L, BuiltinCallableID Id, GCfunc *Function);
[[nodiscard]] GCfunc *lj_builtin_callable(lua_State *L, BuiltinCallableID Id) noexcept;
void lj_builtin_set_context_independent(lua_State *L, BuiltinCallableID Id);
[[nodiscard]] bool lj_builtin_context_independent(lua_State *L, const GCfunc *Function) noexcept;
