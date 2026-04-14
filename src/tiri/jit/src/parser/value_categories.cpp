// Value category abstractions - implementation.
// Copyright © 2025-2026 Paul Manias

#include "parser/value_categories.h"
#include "parser/parse_regalloc.h"

#include "lj_bc.h"

//********************************************************************************************************************
// Extended falsey semantics for Tiri's ?? operator:
// - nil is falsey
// - false is falsey
// - 0 (numeric zero) is falsey
// - "" (empty string) is falsey
// All other values are truthy

bool ExpDesc::is_falsey() const
{
   switch (this->k) {
      case ExpKind::Nil:
      case ExpKind::False:
         return true;

      case ExpKind::True:
         return false;

      case ExpKind::Num: // Check if number is exactly zero
         if (tvisint(&u.nval)) return intV(&u.nval) IS 0;
         else if (tvisnum(&u.nval)) return numV(&u.nval) IS 0.0;
         return false;

      case ExpKind::Str: // Check if string is empty
         return u.sval->len IS 0;

      default:
         // Non-constant expressions cannot be determined at compile time
         // Return false (conservative - assume truthy)
         return false;
   }
}

//********************************************************************************************************************
// LValue implementation

LValue LValue::from_expdesc(const ExpDesc* Desc)
{
   switch (Desc->k) {
      case ExpKind::Local: return LValue::make_local(BCREG(Desc->u.s.info));
      case ExpKind::Upval: return LValue::make_upvalue(Desc->u.s.info);
      case ExpKind::Global: return LValue::make_global(Desc->u.sval);

      case ExpKind::Indexed: {
         bool key_is_register = (int32_t(Desc->u.s.aux) >= 0) and (Desc->u.s.aux <= BCMAX_C);
         if (key_is_register) return LValue::make_indexed(BCREG(Desc->u.s.info), BCREG(Desc->u.s.aux));
         return LValue::make_member(BCREG(Desc->u.s.info), Desc->u.s.aux);
      }

      default:
         // Unsupported ExpKind for l-value (e.g., constants, relocable, void)
         // This is a programming error - caller should validate before conversion
         // For now, return a dummy local to avoid undefined behaviour
         return LValue::make_local(0);
   }
}
