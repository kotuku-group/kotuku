// Miscellaneous object handling.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_obj_c
#define LUA_CORE

#include "lj_obj.h"
#include <kotuku/main.h>

//********************************************************************************************************************
// Object type names.

LJ_DATADEF CSTRING const lj_obj_typename[] = {  // ORDER LUA_T
  "no value", "nil", "boolean", "userdata", "number", "string",
  "table", "function", "userdata", "thread", "proto", "object", "array"
};

LJ_DATADEF CSTRING const lj_obj_itypename[] = {  // ORDER LJ_T
  "nil", "boolean", "boolean", "userdata", "string", "upval", "thread",
  "proto", "function", "trace", "object", "table", "userdata", "array", "number"
};

//********************************************************************************************************************
// Compare two objects without calling metamethods.

int lj_obj_equal(cTValue *LHS, cTValue *RHS)
{
   if (itype(LHS) IS itype(RHS)) {
      if (tvispri(LHS)) return 1;
      if (!tvisnum(LHS)) return gcrefeq(LHS->gcr, RHS->gcr);
   }
   else if (!tvisnumber(LHS) or !tvisnumber(RHS)) return 0;
   return numberVnum(LHS) IS numberVnum(RHS);
}

//********************************************************************************************************************
// Return pointer to object or its object data.

const void * lj_obj_ptr(global_State *g, cTValue *o)
{
   if (tvisudata(o)) return uddata(udataV(o));
   else if (tvislightud(o)) return lightudV(g, o);
   else if (tvisarray(o)) return arrayV(o)->arraydata();
   else if (tvisgcv(o)) return gcV(o);
   else return nullptr;
}

//********************************************************************************************************************

[[nodiscard]] int GCarray::type_flags() const noexcept
{
   switch(elemtype) {
      case AET::BYTE:    return FD_BYTE;
      case AET::INT16:   return FD_WORD;
      case AET::INT32:   return FD_INT;
      case AET::INT64:   return FD_INT64;
      case AET::FLOAT:   return FD_FLOAT;
      case AET::DOUBLE:  return FD_DOUBLE;
      case AET::PTR:     return FD_POINTER;
      case AET::CSTR:    return FD_STRING;
      case AET::STR_CPP: return FD_STRING | FD_CPP;
      case AET::STR_GC:  return FD_STRING;
      case AET::STRUCT:  return FD_STRUCT;
      default:           return 0;
   }
}
