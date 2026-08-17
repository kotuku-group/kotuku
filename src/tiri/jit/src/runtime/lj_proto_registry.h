// Function Prototype Registry
// Copyright (C) 2026 Paul Manias
//
// Stores type signatures for registered C functions and interface methods, enabling compile-time type validation and
// result type inference.

#pragma once

#include "lj_obj.h"
#include <initializer_list>
#include <string_view>

// Initialise the prototype registry. Called once at library startup.

void init_proto_registry();

// Seal the registry once a complete registration pass has finished, permitting lock-free lookups thereafter.  Called
// from luaL_openlibs() after every library has registered its prototypes.  Registration remains safe to repeat for
// subsequent Lua states: repeats are content-identical and their per-state validation still runs.

void seal_proto_registry();

// Register a global/local function prototype

ERR reg_func_prototype(std::string_view Name, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags = FProtoFlags::None);

// Register an interface method prototype

ERR reg_iface_prototype(std::string_view Interface, std::string_view Method,
   std::initializer_list<TiriType> ResultTypes, std::initializer_list<TiriType> ParamTypes,
   FProtoFlags Flags = FProtoFlags::None);

// Register an instance-capable interface method.  The full native parameter list includes the receiver at position
// zero.  The exported interface field and canonical built-in slot are validated in every Lua state.

ERR reg_iface_method(lua_State *L, std::string_view Interface, std::string_view Method, TiriType ReceiverType,
   BuiltinCallableID Callable, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags = FProtoFlags::None, bool AllowAlias = false);

// Register a hidden instance method.  Unlike reg_iface_method(), this does not require a matching public interface
// field and stores only the receiver/member prototype entry.

ERR reg_intrinsic_method(lua_State *L, std::string_view Interface, std::string_view Method, TiriType ReceiverType,
   BuiltinCallableID Callable, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags = FProtoFlags::None);

// Lookup by string (computes hash internally)

const fprototype* get_prototype(std::string_view Interface, std::string_view Method);
const fprototype* get_func_prototype(std::string_view Name);
const fprototype* get_method_prototype(TiriType ReceiverType, std::string_view Method);

// Lookup by pre-computed hash (for parser integration where GCstr->hash is available)

const fprototype* get_prototype_by_hash(uint32_t IfaceHash, uint32_t FuncHash);
const fprototype* get_func_prototype_by_hash(uint32_t FuncHash);
const fprototype* get_method_prototype_by_hash(TiriType ReceiverType, uint32_t MethodHash);

// Runtime classification and lookup for unresolved dot-method calls.  Non-negative results are canonical built-in
// callable indices.  The negative results select one of the two normal member-lookup call frames.

[[nodiscard]] bool is_range_userdata(lua_State *L, GCudata *Userdata);
[[nodiscard]] TiriType runtime_receiver_type(lua_State *L, cTValue *Receiver);
void lj_bmeth_mark_method_compatible(GCtab *Metatable);
[[nodiscard]] bool lj_bmeth_is_method_compatible(cTValue *Receiver);
extern "C" [[nodiscard]] int32_t lj_bmeth_lookup(lua_State *L, cTValue *Receiver, GCstr *Method);
