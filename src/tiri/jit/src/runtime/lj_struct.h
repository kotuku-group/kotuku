// Native struct handling.
// Copyright (C) 2026 Paul Manias

#pragma once

#include "lj_obj.h"

extern GCstruct * lj_struct_new(lua_State *, struct struct_record &);
extern GCstruct * lj_struct_new_external(lua_State *, struct struct_record &, void *Data, uint8_t Flags);
extern void lj_struct_free(global_State *, GCstruct *);
