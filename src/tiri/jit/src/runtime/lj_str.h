// String handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include <cstring>
#include <string_view>

#include "lj_obj.h"

// Ordered compare of strings. Returns <0, 0, or >0.
LJ_FUNC int32_t lj_str_cmp(GCstr* a, GCstr* b);

// Find fixed string p inside string s. Returns pointer to match or nullptr.
LJ_FUNC const char* lj_str_find(const char* s, const char* p, MSize slen, MSize plen);

// Find substring within a string using std::string_view.
[[nodiscard]] inline const char* lj_str_findsv(std::string_view s, std::string_view p) noexcept {
   return lj_str_find(s.data(), p.data(), MSize(s.size()), MSize(p.size()));
}

// Check whether a string contains pattern matching characters.
LJ_FUNC bool lj_str_haspattern(GCstr* s);

// Resize the string interning hash table.
LJ_FUNC void lj_str_resize(lua_State* L, MSize newmask);

// Intern a string and return the interned string object.
LJ_FUNCA GCstr* lj_str_new(lua_State* L, const char* str, size_t len);

// Free a string object (called during GC).
LJ_FUNC void lj_str_free(global_State* g, GCstr* s);

// Initialise string interning subsystem.
LJ_FUNC void lj_str_init(lua_State* L);

// Intern a null-terminated C string.
[[nodiscard]] inline GCstr* lj_str_newz(lua_State* L, const char* s) noexcept {
   return lj_str_new(L, s, strlen(s));
}

// Intern a string from std::string_view.
[[nodiscard]] inline GCstr* lj_str_newsv(lua_State* L, std::string_view sv) noexcept {
   return lj_str_new(L, sv.data(), sv.size());
}

// Calculate the memory size needed for a string of given length.
[[nodiscard]] constexpr inline MSize lj_str_size(MSize len) noexcept {
   return sizeof(GCstr) + ((len + 4) & ~MSize(3));
}

// Free the string interning hash table (requires lj_gc.h for lj_mem_free).
#define lj_str_freetab(g) (lj_mem_free(g, (g)->str.tab, ((g)->str.mask+1)*sizeof(GCRef)))

// Intern a string literal (compile-time length calculation).
#define lj_str_newlit(L, s) (lj_str_new(L, "" s, sizeof(s)-1))
