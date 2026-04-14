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

// Register a global/local function prototype

ERR reg_func_prototype(std::string_view Name, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags = FProtoFlags::None);

// Register an interface method prototype

ERR reg_iface_prototype(std::string_view Interface, std::string_view Method,
   std::initializer_list<TiriType> ResultTypes, std::initializer_list<TiriType> ParamTypes,
   FProtoFlags Flags = FProtoFlags::None);

// Lookup by string (computes hash internally)

const fprototype* get_prototype(std::string_view Interface, std::string_view Method);
const fprototype* get_func_prototype(std::string_view Name);

// Lookup by pre-computed hash (for parser integration where GCstr->hash is available)

const fprototype* get_prototype_by_hash(uint32_t IfaceHash, uint32_t FuncHash);
const fprototype* get_func_prototype_by_hash(uint32_t FuncHash);
