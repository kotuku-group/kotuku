// Bulk TValue operations.
// Copyright (C) 2026 Paul Manias

#pragma once

#include "lj_obj.h"

extern void lj_bulk_nil_tvalue(TValue *Dst, MSize Count);
extern void lj_bulk_copy_tvalue(TValue *Dst, const TValue *Src, MSize Count);
extern void lj_bulk_move_tvalue(TValue *Dst, const TValue *Src, MSize Count);

