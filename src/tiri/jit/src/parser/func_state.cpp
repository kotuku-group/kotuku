
#include "func_state.h"
#include "lexer.h"

[[nodiscard]] VarInfo & FuncState::var_get(int32_t Slot) {
   lj_assertX(Slot >= 0 and Slot < int32_t(varmap.size()), "variable slot out of range");
   return ls->vstack[varmap[Slot]];
}

[[nodiscard]] const VarInfo & FuncState::var_get(int32_t Slot) const {
   lj_assertX(Slot >= 0 and Slot < int32_t(varmap.size()), "variable slot out of range");
   return ls->vstack[varmap[Slot]];
}

//********************************************************************************************************************
// Initialize runtime-dependent fields of FuncState.

void FuncState::init(LexState* LexState, lua_State* LuaState, MSize Vbase, bool IsRoot)
{
   this->ls      = LexState;
   this->L       = LuaState;
   this->vbase   = Vbase;
   this->is_root = IsRoot;
   this->kt      = lj_tab_new(LuaState, 0, 0);

   // Anchor table of constants in stack to avoid being collected.
   settabV(LuaState, LuaState->top, this->kt);
   incr_top(LuaState);
}
