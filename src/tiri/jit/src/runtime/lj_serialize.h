/*
** Object de/serialization.
** Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
*/

#pragma once

#include "lj_obj.h"
#include "lj_buf.h"

#if LJ_HASBUFFER

inline constexpr int LJ_SERIALIZE_DEPTH = 100;   //  Default depth.

LJ_FUNC void lj_serialize_dict_prep_str(lua_State *L, GCtab *dict);
LJ_FUNC void lj_serialize_dict_prep_mt(lua_State *L, GCtab *dict);
LJ_FUNC [[nodiscard]] SBufExt * lj_serialize_put(SBufExt *sbx, cTValue *o);
LJ_FUNC [[nodiscard]] char * lj_serialize_get(SBufExt *sbx, TValue *o);
LJ_FUNC [[nodiscard]] GCstr * lj_serialize_encode(lua_State *L, cTValue *o);
LJ_FUNC void lj_serialize_decode(lua_State *L, TValue *o, GCstr *str);
LJ_FUNC [[nodiscard]] MSize lj_serialize_peektype(SBufExt *sbx);

#endif
