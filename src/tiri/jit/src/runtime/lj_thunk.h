// Thunk (deferred evaluation) support for Tiri.
// Thunks wrap closures in userdata with metatables for automatic resolution.

#pragma once

#include "lj_obj.h"

// Create a new thunk userdata on the stack
// func: The deferred closure
// expected_type: LJ type tag (LUA_TNUMBER, LUA_TSTRING, etc., or LUA_TNIL for unknown)

LJ_FUNC void lj_thunk_new(lua_State *L, GCfunc *func, int expected_type);

// Resolve a thunk if not already resolved
// thunk_udata: The thunk userdata (must be UDTYPE_THUNK)
// Returns: Pointer to resolved value (either cached or newly evaluated)

LJ_FUNC TValue * lj_thunk_resolve(lua_State *L, GCudata *thunk_udata);

// Get the current value of a thunk (resolved value if resolved, thunk itself if not)
// o: TValue that might be a thunk
// Returns: Pointer to the value (may be the thunk itself)

LJ_FUNC cTValue * lj_thunk_getvalue(lua_State *L, cTValue *o);

// Initialize thunk metatable (called during library initialization)

LJ_FUNC void lj_thunk_init(lua_State *L);

// Check if a TValue is a thunk userdata

inline bool lj_is_thunk(cTValue *o)
{
   if (tvisudata(o)) return udataV(o)->udtype IS UDTYPE_THUNK;
   else return false;
}

// Permanently resolve thunk at stack position, return pointer to resolved value

inline TValue * resolve_at(lua_State *L, int idx)
{
   TValue *o = L->base + idx;
   if (lj_is_thunk(o)) return lj_thunk_resolve(L, udataV(o));
   return o;
}
