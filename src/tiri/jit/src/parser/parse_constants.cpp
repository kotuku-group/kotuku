// Copyright © 2025-2026 Paul Manias

// Add a number constant.
// Exported for use by OperatorEmitter facade

[[nodiscard]] BCREG const_num(FuncState* fs, ExpDesc* e)
{
   lua_State* L = fs->L;
   TValue* o;
   fs_check_assert(fs, e->is_num_constant(), "bad usage: ExpKind=%d", int(e->k));
   o = lj_tab_set(L, fs->kt, &e->u.nval);
   if (tvhaskslot(o)) return tvkslot(o);
   o->u64 = fs->nkn;
   return fs->nkn++;
}

// Add a GC object constant.

[[nodiscard]] static BCREG const_gc(FuncState* fs, GCobj* gc, uint32_t itype)
{
   lua_State* L = fs->L;
   TValue key, * o;
   setgcV(L, &key, gc, itype);
   // NOBARRIER: the key is new or kept alive.
   o = lj_tab_set(L, fs->kt, &key);
   if (tvhaskslot(o)) return tvkslot(o);
   o->u64 = fs->nkgc;
   return fs->nkgc++;
}

// Add a string constant.
// Exported for use by OperatorEmitter facade

[[nodiscard]] BCREG const_str(FuncState* fs, ExpDesc* e)
{
   // Accepts string constants, globals, and unscoped (all store string in u.sval)
   fs_check_assert(fs, e->is_str_constant() or e->k IS ExpKind::Global or e->k IS ExpKind::Unscoped,
      "bad usage: ExpKind=%d", int(e->k));
   return const_gc(fs, obj2gco(e->u.sval), LJ_TSTR);
}

// Anchor string constant to avoid GC.

GCstr* LexState::keepstr(std::string_view str)
{
   lua_State* L = this->L;
   GCstr* s = lj_str_new(L, str.data(), str.size());
   TValue* tv = lj_tab_setstr(L, this->fs->kt, s);
   if (tvisnil(tv)) setboolV(tv, 1);
   lj_gc_objbarriert(L, this->fs->kt, s);
   lj_gc_check(L);
   return s;
}

GCstr* LexState::intern_empty_string()
{
   if (!this->empty_string_constant) this->empty_string_constant = this->keepstr(std::string_view());
   return this->empty_string_constant;
}

extern GCstr * lj_parse_keepstr(LexState* ls, const char* str, size_t len)
{
   return ls->keepstr(std::string_view(str, len));
}

// Jump list handling

[[nodiscard]] bool JumpListView::produces_values() const
{
   for (BCPos list = BCPos(list_head); not(list.raw() IS NO_JMP); list = next(func_state, list)) {
      BCIns prior = func_state->bcbase[list.raw() >= 1 ? list.raw() - 1 : list.raw()].ins;
      if (!(bc_op(prior) IS BC_ISTC or bc_op(prior) IS BC_ISFC or bc_a(prior) IS NO_REG))
         return true;
   }
   return false;
}

[[nodiscard]] bool JumpListView::patch_test_register(BCPOS Position, BCREG Register) const
{
   BCInsLine* line = &func_state->bcbase[Position >= 1 ? Position - 1 : Position];
   BCOp op = bc_op(line->ins);
   if (op IS BC_ISTC or op IS BC_ISFC) {
      if (Register != NO_REG and Register != bc_d(line->ins)) {
         setbc_a(&line->ins, Register);
      }
      else {
         setbc_op(&line->ins, op + (BC_IST - BC_ISTC));
         setbc_a(&line->ins, 0);
      }
   }
   else if (bc_a(line->ins) IS NO_REG) {
      if (Register IS NO_REG) {
         line->ins = BCINS_AJ(BC_JMP, bc_a(func_state->bcbase[Position].ins), 0);
      }
      else {
         setbc_a(&line->ins, Register);
         if (Register >= bc_a(line[1].ins))
            setbc_a(&line[1].ins, Register + 1);
      }
   }
   else return false;
   return true;
}

void JumpListView::drop_values() const
{
   for (BCPos list = BCPos(list_head); not(list.raw() IS NO_JMP); list = next(func_state, list))
      (void)patch_test_register(list.raw(), NO_REG);
}

void JumpListView::patch_instruction(BCPOS Position, BCPOS Destination) const
{
   FuncState* fs = func_state;
   BCIns* instruction = &func_state->bcbase[Position].ins;
   BCPOS offset = Destination - (Position + 1) + BCBIAS_J;
   fs_check_assert(fs,not(Destination IS NO_JMP), "uninitialized jump target");
   if (offset > BCMAX_D) func_state->ls->err_syntax(ErrMsg::XJUMP);
   setbc_d(instruction, offset);
}

[[nodiscard]] BCPOS JumpListView::append(BCPOS Other) const
{
   if (Other IS NO_JMP) return list_head;
   if (list_head IS NO_JMP) return Other;
   auto list = BCPos(list_head);
   BCPos next_pc;
   while (true) {
      next_pc = next(func_state, list);
      if (next_pc.raw() IS NO_JMP) break;
      list = next_pc;
   }
   patch_instruction(list.raw(), Other);
   return list_head;
}

void JumpListView::patch_with_value(BCPOS ValueTarget, BCREG Register, BCPOS DefaultTarget) const
{
   auto list = BCPos(list_head);
   while (not(list.raw() IS NO_JMP)) {
      BCPos next_pc = next(func_state, list);
      if (patch_test_register(list.raw(), Register)) patch_instruction(list.raw(), ValueTarget);
      else patch_instruction(list.raw(), DefaultTarget);
      list = next_pc;
   }
}

void JumpListView::patch_to_here() const
{
   func_state->lasttarget = func_state->pc;
   JumpListView pending(func_state, func_state->jpc);
   func_state->jpc = pending.append(list_head);
}

void JumpListView::patch_to(BCPOS Target) const
{
   if (Target IS func_state->pc) patch_to_here();
   else {
      FuncState* fs = func_state;
      fs_check_assert(fs,Target < func_state->pc, "bad jump target");
      patch_with_value(Target, NO_REG, Target);
   }
}

void JumpListView::patch_head(BCPOS Destination) const
{
   if (list_head IS NO_JMP) return;
   patch_instruction(list_head, Destination);
}
