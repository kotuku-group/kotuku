// Copyright © 2025-2026 Paul Manias

static void bcreg_reserve(FuncState* fs, BCREG n);
static void fscope_uvmark(FuncState* fs, BCREG level);
static void bcemit_close(FuncState* fs, BCREG slot);
static void execute_defers(FuncState* fs, BCREG limit);
static void execute_closes(FuncState* fs, BCREG limit);

// Jump types for break and continue statements.

enum { JUMP_BREAK, JUMP_CONTINUE };

//********************************************************************************************************************
// Check if a string is the blank identifier '_'.

static int is_blank_identifier(GCstr *name)
{
   if (name IS NAME_BLANK) return 1;
   return (name != nullptr and uintptr_t(name) >= VARNAME__MAX and name->len IS 1 and *(strdata(name)) IS '_');
}

//********************************************************************************************************************
// Define a new local variable.

void LexState::var_new(BCREG n_unused, GCstr *name, BCLine Line, BCLine Column)
{
   pf::Log log(__FUNCTION__);

   FuncState *fs = this->fs;
   MSize vtop = this->vtop;

   // The pending_vars count tracks how many var_new calls have been made before var_add.
   // varmap.size() + pending_vars gives the total including pending variables.
   log.trace("Name: %s, Count: %d+%d", uintptr_t(name) >= VARNAME__MAX ? CSTRING(name+1) : "_",
      int(fs->varmap.size()), int(fs->pending_vars));

   checklimit(fs, fs->varmap.size() + fs->pending_vars + 1, LJ_MAX_LOCVAR, "local variables");

   if (vtop >= this->size_vstack) [[unlikely]] {
      if (this->size_vstack >= LJ_MAX_VSTACK) lj_lex_error(this, 0, ErrMsg::XLIMC, LJ_MAX_VSTACK);
      lj_mem_growvec(this->L, this->vstack, this->size_vstack, LJ_MAX_VSTACK, VarInfo);
   }

   // Anchor the variable name in the current function's constant table if it's a real string.
   // This is necessary because identifiers may have been lexed while parsing a parent function,
   // so they're anchored in the parent's kt, not the current function's kt.

   if (name != NAME_BLANK and uintptr_t(name) >= VARNAME__MAX) {
      TValue* tv = lj_tab_setstr(this->L, fs->kt, name);
      if (tvisnil(tv)) setboolV(tv, 1);
   }

   fs_check_assert(fs, name IS NAME_BLANK or uintptr_t(name) < VARNAME__MAX or lj_tab_getstr(fs->kt, name) != nullptr, "unanchored variable name");

   // NOBARRIER: name is anchored in fs->kt and ls->vstack is not a GCobj.

   setgcref(this->vstack[vtop].name, obj2gco(name));

   // Store the vstack index in the pending area. var_add() will move these to varmap.
   fs->pending_varmap[fs->pending_vars++] = uint16_t(vtop);

   // Use provided line/column if given (non-zero), otherwise fall back to current token position

   this->vstack[vtop].line = (Line > 0) ? Line : this->current_token_line;
   this->vstack[vtop].column = (Column > 0) ? Column : this->current_token_column;
   this->vtop = vtop + 1;
}

//********************************************************************************************************************

void LexState::var_new_lit(BCREG n, std::string_view value) { this->var_new(n, this->keepstr(value)); }
void LexState::var_new_fixed(BCREG n, uintptr_t name) { this->var_new(n, (GCstr*)name); }

//********************************************************************************************************************
// Add local variables. Moves nvars pending variables from pending_varmap to varmap.
// Note: pending_vars may be greater than nvars (e.g., for-loops add 4 vars but call var_add(3) then var_add(1)).

void LexState::var_add(BCREG nvars)
{
   FuncState *fs = this->fs;
   lj_assertX(nvars <= fs->pending_vars, "var_add: not enough pending vars (need %d, have %d)", int(nvars), int(fs->pending_vars));

   BCREG base = BCREG(fs->varmap.size());

   // Move the first nvars pending entries to varmap
   for (BCREG i = 0; i < nvars; ++i) {
      fs->varmap.push_back(fs->pending_varmap[i]);
   }

   // Shift remaining pending entries down
   BCREG remaining = fs->pending_vars - nvars;
   for (BCREG i = 0; i < remaining; ++i) {
      fs->pending_varmap[i] = fs->pending_varmap[nvars + i];
   }
   fs->pending_vars = remaining;

   // Initialize the VarInfo entries
   for (BCREG i = 0; i < nvars; ++i) {
      BCREG slot = base + i;
      VarInfo *v = &fs->var_get(slot);
      v->startpc = fs->pc;
      v->slot = slot;
      v->info = VarInfoFlag::None;
      v->fixed_type = TiriType::Unknown;  // Initialize to Unknown (no type constraint)
      v->result_types.fill(TiriType::Unknown);  // No return type info for non-functions
   }
}

//********************************************************************************************************************
// Remove local variables.

void LexState::var_remove(BCREG tolevel)
{
   FuncState* fs = this->fs;
   while (fs->varmap.size() > tolevel) {
      fs->var_get(int32_t(fs->varmap.size()) - 1).endpc = fs->pc;
      fs->varmap.pop_back();
   }
}

//********************************************************************************************************************
// Lookup local variable name.

static std::optional<BCREG> var_lookup_local(FuncState *fs, GCstr *n)
{
   for (int i : std::views::iota(0, int(fs->varmap.size())) | std::views::reverse) {
      GCstr *varname = strref(fs->var_get(i).name);
      if (varname IS NAME_BLANK) [[unlikely]]
         continue;  // Skip blank identifiers.
      if (n IS varname) [[likely]]
         return BCREG(i);
   }
   return std::nullopt;  // Not found.
}

//********************************************************************************************************************
// Lookup or add upvalue index.

static MSize var_lookup_uv(FuncState *fs, MSize vidx, ExpDesc* e)
{
   MSize n = fs->nuv;

   // Check if upvalue already exists using range-based iteration.

   auto uvmap_view = std::span(fs->uvmap.data(), n);
   for (MSize i = 0; auto uv_idx : uvmap_view) {
      if (uv_idx IS vidx) return i;  // Already exists.
      i++;
   }

   // Otherwise create a new one.

   checklimit(fs, fs->nuv, LJ_MAX_UPVAL, "upvalues");
   fs_check_assert(fs,e->k IS ExpKind::Local or e->k IS ExpKind::Upval, "bad expr type %d", e->k);
   fs->uvmap[n] = uint16_t(vidx);
   fs->uvtmp[n] = uint16_t(e->k IS ExpKind::Local ? vidx : LJ_MAX_VSTACK + e->u.s.info);
   fs->nuv = n + 1;
   return n;
}

//********************************************************************************************************************
// Lookup variables in the function stack, iterating from the current function to the top-level.

static MSize var_lookup_(LexState* ls, GCstr* name, ExpDesc* e)
{
   auto &stack = ls->func_stack;
   if (stack.empty()) {
      e->init(ExpKind::Unscoped, 0);
      e->u.sval = name;
      return MSize(-1);
   }

   // Search from current function (back of stack) to top-level (front of stack)
   size_t current_idx = stack.size() - 1;

   for (size_t i = current_idx + 1; i > 0; --i) {
      size_t idx = i - 1;
      FuncState& search_fs = stack[idx];
      auto reg = var_lookup_local(&search_fs, name);

      if (reg.has_value()) {
         e->init(ExpKind::Local, reg.value());
         bool is_current = (idx IS current_idx);
         if (not is_current) fscope_uvmark(&search_fs, reg.value());  // Scope now has an upvalue.
         auto vidx = MSize(e->u.s.aux = uint32_t(search_fs.varmap[reg.value()]));

         // Propagate type info from VarInfo to ExpDesc for type checking on re-assignment
         VarInfo& vinfo = ls->vstack[vidx];
         e->result_type = vinfo.fixed_type;
         e->object_class_id = vinfo.object_class_id;

         // If found in outer function, create upvalues in all intervening functions
         if (not is_current) {
            for (size_t j = idx + 1; j <= current_idx; ++j) {
               e->u.s.info = uint8_t(var_lookup_uv(&stack[j], vidx, e));
               e->k = ExpKind::Upval;
            }
         }
         return vidx;
      }
   }

   // Not found in any function - scope is undetermined
   e->init(ExpKind::Unscoped, 0);
   e->u.sval = name;
   return MSize(-1);
}

// Lookup variable name.
MSize LexState::var_lookup(ExpDesc* e)
{
   return var_lookup_(this, this->lex_str(), e);
}

MSize LexState::var_lookup_symbol(GCstr* name, ExpDesc* e)
{
   if (name IS nullptr or name IS NAME_BLANK) {
      e->init(ExpKind::Global, 0);
      e->u.sval = name ? name : NAME_BLANK;
      return MSize(-1);
   }
   return var_lookup_(this, name, e);
}

//********************************************************************************************************************
// Jump and target handling

// Add a new jump or target

MSize LexState::gola_new(int jump_type, VarInfoFlag info, BCPOS pc)
{
   FuncState* fs = this->fs;
   MSize vtop = this->vtop;
   if (vtop >= this->size_vstack) [[unlikely]] {
      if (this->size_vstack >= LJ_MAX_VSTACK) lj_lex_error(this, 0, ErrMsg::XLIMC, LJ_MAX_VSTACK);
      lj_mem_growvec(this->L, this->vstack, this->size_vstack, LJ_MAX_VSTACK, VarInfo);
   }
   GCstr* name = (jump_type IS JUMP_BREAK) ? NAME_BREAK : NAME_CONTINUE;
   // NOBARRIER: name is anchored in fs->kt and ls->vstack is not a GCobj.
   setgcrefp(this->vstack[vtop].name, obj2gco(name));
   this->vstack[vtop].startpc = pc;
   this->vstack[vtop].slot = uint8_t(fs->varmap.size());
   this->vstack[vtop].info = info;
   this->vtop = vtop + 1;
   return vtop;
}

//********************************************************************************************************************
// Constexpr helper functions for goto/label flag checking.

[[nodiscard]] static inline bool gola_is_jump(const VarInfo* v) {
   return has_flag(v->info, VarInfoFlag::Jump);
}

[[nodiscard]] static inline bool gola_is_jump_target(const VarInfo* v) {
   return has_flag(v->info, VarInfoFlag::JumpTarget);
}

[[nodiscard]] static inline bool gola_is_jump_or_target(const VarInfo* v) {
   return has_flag(v->info, VarInfoFlag::Jump) or has_flag(v->info, VarInfoFlag::JumpTarget);
}

//********************************************************************************************************************
// Patch goto to jump to target.

void LexState::gola_patch(VarInfo* vg, VarInfo* vl)
{
   FuncState* fs = this->fs;
   BCPOS pc = vg->startpc;
   setgcrefnull(vg->name);  // Invalidate pending goto.
   setbc_a(&fs->bcbase[pc].ins, vl->slot);
   JumpListView(fs, pc).patch_to(vl->startpc);
}

//********************************************************************************************************************
// Patch goto to close upvalues.

void LexState::gola_close(VarInfo* vg)
{
   FuncState* fs = this->fs;
   BCPOS pc = vg->startpc;
   BCIns* ip = &fs->bcbase[pc].ins;
   fs_check_assert(fs,gola_is_jump(vg), "expected goto");
   fs_check_assert(fs,bc_op(*ip) IS BC_JMP or bc_op(*ip) IS BC_UCLO, "bad bytecode op %d", bc_op(*ip));
   setbc_a(ip, vg->slot);
   if (bc_op(*ip) IS BC_JMP) {
      BCPos next = JumpListView::next(fs, BCPos(pc));
      if (next.raw() != NO_JMP) JumpListView(fs, next.raw()).patch_to(pc);  // Jump to UCLO.
      setbc_op(ip, BC_UCLO);  // Turn into UCLO.
      setbc_j(ip, NO_JMP);
   }
}

//********************************************************************************************************************
// Resolve pending forward jumps (break/continue) for target.

void LexState::gola_resolve(FuncScope* bl, MSize idx)
{
   VarInfo* vg = this->vstack + bl->vstart;
   VarInfo* vl = this->vstack + idx;
   for (; vg < vl; vg++) {
      if (gcrefeq(vg->name, vl->name) and gola_is_jump(vg)) {
         this->gola_patch(vg, vl);
      }
   }
}

//********************************************************************************************************************
// Fixup remaining gotos and targets for scope.

void LexState::gola_fixup(FuncScope* bl)
{
   VarInfo *v = this->vstack + bl->vstart;
   VarInfo *ve = this->vstack + this->vtop;
   for (; v < ve; v++) {
      GCstr *name = strref(v->name);
      if (name != nullptr) {  // Only consider remaining valid gotos/targets.
         if (gola_is_jump_target(v)) {
            VarInfo *vg;
            setgcrefnull(v->name);  // Invalidate target that goes out of scope.
            for (vg = v + 1; vg < ve; vg++)  // Resolve pending backward gotos.
               if (strref(vg->name) IS name and gola_is_jump(vg)) {
                  if (has_flag(bl->flags, FuncScopeFlag::Upvalue) and vg->slot > v->slot) this->gola_close(vg);
                  this->gola_patch(vg, v);
               }
         }
         else if (gola_is_jump(v)) {
            if (bl->prev) {  // Propagate break/continue to outer scope.
               if (name IS NAME_BREAK) bl->prev->flags |= FuncScopeFlag::Break;
               else if (name IS NAME_CONTINUE) bl->prev->flags |= FuncScopeFlag::Continue;
               v->slot = bl->nactvar;
               if (has_flag(bl->flags, FuncScopeFlag::Upvalue)) this->gola_close(v);
            }
            else {  // No outer scope: no loop for break/continue.
               this->linenumber = this->fs->bcbase[v->startpc].line;
               if (name IS NAME_BREAK) lj_lex_error(this, 0, ErrMsg::XBREAK);
               else if (name IS NAME_CONTINUE) lj_lex_error(this, 0, ErrMsg::XCONTINUE);
            }
         }
      }
   }
}

//********************************************************************************************************************
// Scope handling

// Begin a scope.

static void fscope_begin(FuncState *fs, FuncScope *bl, FuncScopeFlag flags)
{
   bl->nactvar = uint8_t(fs->varmap.size());
   bl->flags = flags;
   bl->vstart = fs->ls->vtop;
   bl->prev = fs->bl;
   fs->bl = bl;
   fs->assert_freereg_at_locals();
}

//********************************************************************************************************************

static void execute_defers(FuncState *fs, BCREG limit)
{
   BCREG i = fs->varmap.size();
   BCREG oldfreereg;
   BCREG argc = 0;
   BCREG argslots[LJ_MAX_SLOTS];

   fs->ensure_freereg_at_locals();
   oldfreereg = fs->freereg;

   while (i > limit) {
      VarInfo *v = &fs->var_get(--i);
      if (has_flag(v->info, VarInfoFlag::DeferArg)) {
         fs_check_assert(fs,argc < LJ_MAX_SLOTS, "too many defer args");
         argslots[argc++] = v->slot;
         continue;
      }

      if (has_flag(v->info, VarInfoFlag::Defer)) {
         BCREG callbase = fs->freereg;
         BCREG j;
         RegisterAllocator allocator(fs);
         allocator.reserve(BCReg(argc + 1 + LJ_FR2));
         bcemit_AD(fs, BC_MOV, callbase, v->slot);
         for (j = 0; j < argc; j++) {
            BCREG src = argslots[argc - 1 - j];
            bcemit_AD(fs, BC_MOV, callbase + LJ_FR2 + j + 1, src);
         }
         bcemit_ABC(fs, BC_CALL, callbase, 1, argc + 1);
         argc = 0;
         continue;
      }
      fs_check_assert(fs,argc IS 0, "dangling defer arguments");
   }

   fs_check_assert(fs,argc IS 0, "dangling defer arguments");
   fs->freereg = oldfreereg;
}

//********************************************************************************************************************
// Emit close calls for to-be-closed locals.
//
// This emits bytecode to call the __close metamethod on a variable at the given slot. The generated bytecode:
// 1. Skips nil and false values (they cannot have metatables)
// 2. Calls getmetatable(value) to retrieve the metatable
// 3. Indexes the metatable with "__close" to get the close handler
// 4. Skips if no __close handler exists
// 5. Calls __close(value, nil) - nil indicates normal scope exit (not error unwinding)
//
// All skip jumps are patched to the end of the sequence. Errors in __close are not currently protected

static void bcemit_close(FuncState *fs, BCREG slot)
{
   RegisterAllocator allocator(fs);
   LexState* ls = fs->ls;

   // Emit BC_UCLO to prevent the JIT from recording through the close handler sequence.
   // The inline bytecode for calling __close metamethods uses temporary registers in ways
   // that confuse the JIT's type tracking, leading to assertion failures during hot-path
   // execution. BC_UCLO triggers LJ_TRERR_NYIBC which cleanly aborts JIT recording.
   // The jump offset of 0 means no branch is taken - it just marks this code as non-JIT-able.
   //
   // TODO: For better JIT performance, consider implementing native JIT support for __close:
   // 1. Add a dedicated BC_CLOSE bytecode that handles the __close metamethod call directly
   // 2. Implement JIT recording support for BC_CLOSE in lj_record.cpp (rec_close function)
   // 3. Generate optimised machine code that inlines the nil/false checks and metamethod lookup
   // 4. This would allow hot loops containing <close> variables to be fully JIT-compiled

   bcemit_AJ(fs, BC_UCLO, slot, 0);

   // Track jumps that skip the close operation
   BCPOS skip_jumps[4];
   int num_skip_jumps = 0;

   // Reserve registers for: getmetatable function, metatable result, __close function, args
   BCREG base = fs->freereg;
   allocator.reserve(BCReg(5 + LJ_FR2));

   // Step 1: Check if value is nil - skip if so (nil has no metatable)
   ExpDesc nilv(ExpKind::Nil);
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, slot, const_pri(&nilv)));
   skip_jumps[num_skip_jumps++] = bcemit_jmp(fs);

   // Step 2: Check if value is false - skip if so (false has no metatable)
   ExpDesc falsev(ExpKind::False);
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, slot, const_pri(&falsev)));
   skip_jumps[num_skip_jumps++] = bcemit_jmp(fs);

   // Step 3: Load getmetatable global and call it
   // getmetatable goes into base, argument (value) into base+1+LJ_FR2
   ExpDesc getmt;
   getmt.init(ExpKind::Global, 0);
   getmt.u.sval = ls->keepstr("getmetatable");
   expr_toreg(fs, &getmt, base);

   // Copy value to argument position
   bcemit_AD(fs, BC_MOV, base + 1 + LJ_FR2, slot);

   // Call getmetatable(value) -> result in base
   bcemit_ABC(fs, BC_CALL, base, 2, 2);  // 1 result, 1 arg

   // Step 4: Check if metatable is nil - skip if so
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, base, const_pri(&nilv)));
   skip_jumps[num_skip_jumps++] = bcemit_jmp(fs);

   // Step 5: Get __close from metatable (metatable is in base)
   // NOTE: BC_TGETS is an ABC-format instruction (dest, table, key), not AD-format.
   // Using BCINS_AD here would cause incorrect bytecode encoding.
   GCstr* close_str = ls->keepstr("__close");
   ExpDesc key;
   key.init(ExpKind::Str, 0);
   key.u.sval = close_str;
   BCREG close_fn_reg = base + 1;
   bcemit_tgets(fs, close_fn_reg, base, const_str(fs, &key));

   // Step 6: Check if __close is nil - skip if so
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, close_fn_reg, const_pri(&nilv)));
   skip_jumps[num_skip_jumps++] = bcemit_jmp(fs);

   // Step 7: Call __close(value, err)
   // __close function is in close_fn_reg, move to call base
   BCREG call_base = base;
   bcemit_AD(fs, BC_MOV, call_base, close_fn_reg);

   // First arg: the value being closed
   bcemit_AD(fs, BC_MOV, call_base + 1 + LJ_FR2, slot);

   // Second arg: read from _G.__close_err (nil for normal exit, error for unwinding)
   // This allows error propagation to __close handlers when set by error handling code
   ExpDesc err_global;
   err_global.init(ExpKind::Global, 0);
   err_global.u.sval = ls->keepstr("__close_err");
   expr_toreg(fs, &err_global, call_base + 2 + LJ_FR2);

   // Call __close(value, err) -> no results needed
   bcemit_ABC(fs, BC_CALL, call_base, 1, 3);  // 0 results, 2 args

   // Patch all skip jumps to here
   BCPOS end_pc = fs->pc;
   for (int i = 0; i < num_skip_jumps; i++) {
      JumpListView(fs, skip_jumps[i]).patch_to(end_pc);
   }
}

//********************************************************************************************************************
// Execute close handlers for scope.

static void execute_closes(FuncState* fs, BCREG limit)
{
   BCREG i = fs->varmap.size();
   BCREG oldfreereg;

   fs->ensure_freereg_at_locals();
   oldfreereg = fs->freereg;

   while (i > limit) {
      VarInfo *v = &fs->var_get(--i);
      if (has_flag(v->info, VarInfoFlag::Close)) bcemit_close(fs, v->slot);
   }

   fs->freereg = oldfreereg;
}

//********************************************************************************************************************
// End a scope.

static void fscope_end(FuncState* fs)
{
   if (not fs) return;

   FuncScope* bl = fs->bl;
   LexState* ls = fs->ls;
   fs->bl = bl->prev;
   // Run __close and defer handlers when scope ends (LIFO order: closes before defers)
   execute_closes(fs, bl->nactvar);
   execute_defers(fs, bl->nactvar);
   ls->var_remove(bl->nactvar);
   fs->reset_freereg();
   fs->assert_freereg_at_locals();
   if ((bl->flags & (FuncScopeFlag::Upvalue | FuncScopeFlag::NoClose)) IS FuncScopeFlag::Upvalue) bcemit_AJ(fs, BC_UCLO, bl->nactvar, 0);
   if (has_flag(bl->flags, FuncScopeFlag::Break)) {
      if (has_flag(bl->flags, FuncScopeFlag::Loop)) {
         VStackGuard vstack_guard(ls);
         MSize idx = ls->gola_new(JUMP_BREAK, VarInfoFlag::JumpTarget, fs->pc);
         ls->gola_resolve(bl, idx);
      }
      else {  // Need the fixup step to propagate the breaks.
         ls->gola_fixup(bl);
         return;
      }
   }
   if (has_flag(bl->flags, FuncScopeFlag::Continue)) {
      ls->gola_fixup(bl);
   }
}

//********************************************************************************************************************
// Mark scope as having an upvalue.

static void fscope_uvmark(FuncState* fs, BCREG level)
{
   FuncScope* bl;
   for (bl = fs->bl; bl and bl->nactvar > level; bl = bl->prev);
   if (bl) bl->flags |= FuncScopeFlag::Upvalue;
}

//********************************************************************************************************************
// Fixup bytecode for prototype.

static void fs_fixup_bc(FuncState* fs, GCproto* pt, BCIns* bc, MSize n)
{
   BCInsLine* base = fs->bcbase;
   MSize i;
   pt->sizebc = n;
   bc[0] = BCINS_AD((fs->flags & PROTO_VARARG) ? BC_FUNCV : BC_FUNCF, fs->framesize, 0);
   for (i = 1; i < n; i++) bc[i] = base[i].ins;
}

//********************************************************************************************************************
// Fixup upvalues for child prototype, step #2.

static void fs_fixup_uv2(FuncState* fs, GCproto* pt)
{
   VarInfo* vstack = fs->ls->vstack;
   uint16_t* uv = proto_uv(pt);
   MSize i, n = pt->sizeuv;
   for (i = 0; i < n; i++) {
      VarIndex vidx = uv[i];
      if (vidx >= LJ_MAX_VSTACK) uv[i] = vidx - LJ_MAX_VSTACK;
      else if (has_flag(vstack[vidx].info, VarInfoFlag::VarReadWrite)) uv[i] = vstack[vidx].slot | PROTO_UV_LOCAL;
      else uv[i] = vstack[vidx].slot | PROTO_UV_LOCAL | PROTO_UV_IMMUTABLE;
   }
}

//********************************************************************************************************************
// Fixup constants for prototype.

static void fs_fixup_k(FuncState* fs, GCproto* pt, void* kptr)
{
   GCtab* kt;
   TValue* array;
   Node* node;
   MSize i, hmask;
   checklimitgt(fs, fs->nkn, BCMAX_D + 1, "constants");
   checklimitgt(fs, fs->nkgc, BCMAX_D + 1, "constants");
   setmref(pt->k, kptr);
   pt->sizekn = fs->nkn;
   pt->sizekgc = fs->nkgc;
   kt = fs->kt;
   array = tvref(kt->array);
   for (i = 0; i < kt->asize; i++) {
      if (tvhaskslot(&array[i])) {
         TValue* tv = &((TValue*)kptr)[tvkslot(&array[i])];
         if (LJ_DUALNUM) setintV(tv, int32_t(i));
         else setnumV(tv, lua_Number(i));
      }
   }

   node = noderef(kt->node);
   hmask = kt->hmask;

   for (i = 0; i <= hmask; i++) {
      Node* n = &node[i];
      if (tvhaskslot(&n->val)) {
         ptrdiff_t kidx = ptrdiff_t(tvkslot(&n->val));
         fs_check_assert(fs,!tvisint(&n->key), "unexpected integer key");
         if (tvisnum(&n->key)) {
            TValue* tv = &((TValue*)kptr)[kidx];
            if (LJ_DUALNUM) {
               lua_Number nn = numV(&n->key);
               int32_t k = lj_num2int(nn);
               fs_check_assert(fs,!tvismzero(&n->key), "unexpected -0 key");
               if (lua_Number(k) IS nn) setintV(tv, k);
               else *tv = n->key;
            }
            else *tv = n->key;
         }
         else {
            GCobj* o = gcV(&n->key);
            setgcref(((GCRef*)kptr)[~kidx], o);
            lj_gc_objbarrier(fs->L, pt, o);
            if (tvisproto(&n->key)) fs_fixup_uv2(fs, gco_to_proto(o));
         }
      }
   }
}

//********************************************************************************************************************
// Fixup upvalues for prototype, step #1.

static void fs_fixup_uv1(FuncState *fs, GCproto *pt, uint16_t *uv)
{
   setmref(pt->uv, uv);
   pt->sizeuv = fs->nuv;
   memcpy(uv, fs->uvtmp.data(), fs->nuv * sizeof(VarIndex));
}

//********************************************************************************************************************

#ifndef LUAJIT_DISABLE_DEBUGINFO

// Prepare lineinfo for prototype.

static size_t fs_prep_line(FuncState *fs)
{
   return (fs->pc - 1) * sizeof(BCLine);
}

//********************************************************************************************************************
// Store lineinfo for prototype.
// BCLine values are stored directly (32-bit each) with file index in upper 8 bits and line number in lower 24 bits.
// This eliminates delta encoding and separate fileinfo allocation - file info is embedded in each BCLine.

static void fs_fixup_line(FuncState *fs, GCproto *pt, void *lineinfo, BCLine numline)
{
   BCInsLine *base = fs->bcbase + 1;  // Skip FUNCF/FUNCV instruction
   MSize n = fs->pc - 1;

   // Encode file index into firstline.  Extract the file index from the first instruction's line
   // (which was properly encoded by bcemit_INS using effective_line()), or fall back to the current
   // file index if there are no instructions following FUNCF/FUNCV.

   uint8_t file_idx = (n > 0) ? base[0].line.fileIndex() : fs->ls->current_file_index;
   pt->firstline = BCLine::encode(file_idx, fs->linedefined.lineNumber());
   pt->numline = numline;
   setmref(pt->lineinfo, lineinfo);

   auto li = (BCLine *)lineinfo;
   for (MSize i = 0; i < n; i++) li[i] = base[i].line;
}

//********************************************************************************************************************
// Prepare variable info for prototype.

size_t LexState::fs_prep_var(FuncState* FunctionState, size_t* OffsetVar)
{
   FuncState *fs = FunctionState;
   VarInfo *vs = this->vstack, * ve;
   BCPOS lastpc;

   lj_buf_reset(&this->sb);  // Copy to temp. string buffer.

   // Store upvalue names using range-based iteration.

   auto uvmap_range = std::span(fs->uvmap.data(), fs->nuv);
   for (auto uv_idx : uvmap_range) {
      GCstr *s = strref(vs[uv_idx].name);
      MSize len = s->len + 1;
      char *p = lj_buf_more(&this->sb, len);
      p = lj_buf_wmem(p, strdata(s), len);
      this->sb.w = p;
   }
   *OffsetVar = sbuflen(&this->sb);
   lastpc = 0;

   // Store local variable names and compressed ranges.

   for (ve = vs + this->vtop, vs += fs->vbase; vs < ve; vs++) {
      if (!gola_is_jump_or_target(vs)) {
         GCstr* s = strref(vs->name);
         BCPOS startpc;
         char *p;
         if (uintptr_t(s) < VARNAME__MAX) {
            p = lj_buf_more(&this->sb, 1 + 2 * 5);
            *p++ = char(uintptr_t(s));
         }
         else {
            MSize len = s->len + 1;
            p = lj_buf_more(&this->sb, len + 2 * 5);
            p = lj_buf_wmem(p, strdata(s), len);
         }
         startpc = vs->startpc;
         p = lj_strfmt_wuleb128(p, startpc - lastpc);
         p = lj_strfmt_wuleb128(p, vs->endpc - startpc);
         this->sb.w = p;
         lastpc = startpc;
      }
   }
   lj_buf_putb(&this->sb, '\0');  // Terminator for varinfo.
   return sbuflen(&this->sb);
}

//********************************************************************************************************************
// Fixup variable info for prototype.

void LexState::fs_fixup_var(GCproto *Prototype, uint8_t *Buffer, size_t OffsetVar)
{
   setmref(Prototype->uvinfo, Buffer);
   setmref(Prototype->varinfo, (char*)Buffer + OffsetVar);
   memcpy(Buffer, this->sb.b, sbuflen(&this->sb));  // Copy from temp. buffer.
}

#else

// Initialize with empty debug info, if disabled.
#define fs_prep_line(fs) (0)
#define fs_fixup_line(fs, pt, li, numline) pt->firstline = pt->numline = 0, setmref((pt)->lineinfo, nullptr)

size_t LexState::fs_prep_var(FuncState* FunctionState, size_t* OffsetVar)
{
   return 0;
}

void LexState::fs_fixup_var(GCproto* Prototype, uint8_t* Buffer, size_t OffsetVar)
{
   setmref((Prototype)->uvinfo, nullptr);
   setmref((Prototype)->varinfo, nullptr);
}

#endif

//********************************************************************************************************************
// Check if bytecode op returns.

[[nodiscard]] inline int bcopisret(BCOp op)
{
   switch (op) {
      case BC_CALLMT: case BC_CALLT:
      case BC_RETM: case BC_RET: case BC_RET0: case BC_RET1:
         return 1;
      default:
         return 0;
   }
}

//********************************************************************************************************************
// Fixup return instruction for prototype.

static void fs_fixup_ret(FuncState *fs)
{
   BCPOS lastpc = fs->pc;
   if (lastpc <= fs->lasttarget or !bcopisret(bc_op(fs->bytecode_at(BCPos(lastpc - 1)).ins))) {
      // Implicit return: run __close and defer handlers (LIFO order)
      execute_closes(fs, 0);
      execute_defers(fs, 0);
      if (has_flag(fs->bl->flags, FuncScopeFlag::Upvalue)) bcemit_AJ(fs, BC_UCLO, 0, 0);
      bcemit_AD(fs, BC_RET0, 0, 1);  // Need final return.
   }

   fs->bl->flags |= FuncScopeFlag::NoClose;  // Handled above.
   fscope_end(fs);
   fs_check_assert(fs,fs->bl IS nullptr, "bad scope nesting");

   // May need to fixup returns encoded before first function was created.

   if (fs->flags & PROTO_FIXUP_RETURN) {
      BCPOS pc;
      for (pc = 1; pc < lastpc; pc++) {
         BCIns ins = fs->bcbase[pc].ins;
         BCPOS offset;
         switch (bc_op(ins)) {
            case BC_CALLMT: case BC_CALLT:
            case BC_RETM: case BC_RET: case BC_RET0: case BC_RET1:
               offset = bcemit_INS(fs, ins);  // Copy original instruction.
               fs->bcbase[offset].line = fs->bcbase[pc].line;
               offset = offset - (pc + 1) + BCBIAS_J;
               if (offset > BCMAX_D) fs->ls->err_syntax(ErrMsg::XFIXUP);
               // Replace with UCLO plus branch.
               fs->bcbase[pc].ins = BCINS_AD(BC_UCLO, 0, offset);
               break;
            case BC_UCLO:
               return;  // We're done.
            default:
               break;
         }
      }
   }
}

//********************************************************************************************************************
// Build a qualified scope name from the function stack (e.g., "outer.inner" for nested functions).

static std::string build_scope_name(LexState* ls, FuncState *FState)
{
   auto &stack = ls->func_stack;

   // Find index of FState in func_stack
   size_t fs_idx = SIZE_MAX;
   for (size_t i = 0; i < stack.size(); ++i) {
      if (&stack[i] IS FState) {
         fs_idx = i;
         break;
      }
   }
   if (fs_idx IS SIZE_MAX) return "";

   // Collect names from top-level (index 0) to current function
   std::vector<std::string_view> names;
   for (size_t i = 0; i <= fs_idx; ++i) {
      FuncState &current = stack[i];
      if (current.funcname) {
         names.push_back(std::string_view(strdata(current.funcname), current.funcname->len));
      }
   }

   if (names.empty()) return "";

   std::string result;
   for (const auto &name : names) {
      if (not result.empty()) result += '.';
      result += name;
   }
   return result;
}

//********************************************************************************************************************
// Finish a FuncState and return the new prototype.

GCproto * LexState::fs_finish(BCLine Line)
{
   lua_State *L = this->L;
   FuncState *fs = this->fs;

   // Find the min/max raw line numbers in the bytecode.
   // Note: bcbase[].line values may be encoded with file index in upper 8 bits, so we decode them to get raw line
   // numbers for computing numline.
   // We also need to find the minimum line to handle cases where AST-constructed functions (like thunks) contain
   // expressions from earlier source lines.

   BCLine line = Line;
   BCLine min_line = fs->linedefined;
   if (fs->bcbase != nullptr and fs->pc > 0) {
      for (BCPOS pc = 0; pc < fs->pc; ++pc) {
         BCLine raw_line = fs->bcbase[pc].line.lineNumber();
         if (raw_line > 0) {  // Skip zero lines (shouldn't happen, but be safe)
            if (raw_line > line) line = raw_line;
            if (raw_line < min_line) min_line = raw_line;
         }
      }
   }

   // Adjust linedefined if bytecode contains instructions from earlier lines.
   // This can happen with AST-constructed functions (e.g., thunks wrapping f-strings).

   if (min_line < fs->linedefined) fs->linedefined = min_line;

   BCLine numline = line - fs->linedefined;
   size_t sizept, ofsk, ofsuv, ofsli, ofsdbg, ofsvar;
   GCproto *pt;

   // Apply final fixups.

   fs_fixup_ret(fs);

   // Capture variable declarations if JOF::DIAGNOSE is enabled.

   auto prv = (prvTiri *)this->L->script->ChildPrivate;
   if (((prv->JitOptions & JOF::DIAGNOSE) != JOF::NIL)) {
      std::string scope = build_scope_name(this, fs);

      // Capture local variables.
      for (MSize i = fs->vbase; i < this->vtop; i++) {
         VarInfo *var = &this->vstack[i];
         if (gola_is_jump_or_target(var)) continue;
         GCstr *name_str = strref(var->name);
         if (uintptr_t(name_str) < VARNAME__MAX) continue;
         if (name_str IS NAME_BLANK) continue;

         VariableInfo info {
            .line   = var->line,
            .column = var->column,
            .scope  = scope,
            .name   = std::string(strdata(name_str), name_str->len),
            .type   = var->fixed_type,
            .is_global = false
         };
         prv->CapturedVariables.push_back(std::move(info));
      }

      // Capture explicitly declared globals (only for main chunk, not nested functions).
      if (func_stack.size() IS 1) {
         for (GCstr *name : fs->declared_globals) {
            VariableInfo info;
            info.line   = 0;  // Line info not available for globals declared elsewhere
            info.column = 0;
            info.scope  = "";
            info.name   = std::string(strdata(name), name->len);
            info.type   = TiriType::Unknown;  // Type not tracked for globals
            info.is_global = true;
            prv->CapturedVariables.push_back(std::move(info));
         }
      }
   }

   // Calculate total size of prototype including all colocated arrays.

   sizept = sizeof(GCproto) + fs->pc * sizeof(BCIns) + fs->nkgc * sizeof(GCRef);
   sizept = (sizept + sizeof(TValue) - 1) & ~(sizeof(TValue) - 1);
   ofsk   = sizept;
   sizept += fs->nkn * sizeof(TValue);
   ofsuv  = sizept;
   sizept += ((fs->nuv + 1) & ~1) * 2;
   ofsli  = sizept;
   sizept += fs_prep_line(fs);
   ofsdbg = sizept;
   sizept += this->fs_prep_var(fs, &ofsvar);

   // Allocate prototype and initialize its fields.

   pt = (GCproto *)lj_mem_newgco(L, MSize(sizept));
   pt->gct       = ~LJ_TPROTO;
   pt->sizept    = MSize(sizept);
   pt->trace     = 0;
   pt->flags     = uint8_t(fs->flags & ~(PROTO_HAS_RETURN | PROTO_FIXUP_RETURN));
   pt->numparams = fs->numparams;
   pt->framesize = fs->framesize;
   pt->file_source_idx = this->current_file_index;  // FileSource tracking for error reporting
   setgcref(pt->chunk_name, obj2gco(this->chunk_name));

   // Register the function name if one was provided (for named function declarations).

   if (fs->funcname) lj_funcname_register(G(this->L), pt, strdata(fs->funcname), fs->funcname->len);

   // Build bitmap of locals with <close> attribute for error unwinding.
   // Use the actual register slot index from VarInfo::slot, not the vstack index.
   // Note: Slots >= 64 are rejected at parse time in emit_local_decl_stmt().

   pt->closeslots = 0;
   for (MSize i = fs->vbase; i < this->vtop; i++) {
      if (has_flag(this->vstack[i].info, VarInfoFlag::Close)) {
         uint8_t slot = this->vstack[i].slot;
         lj_assertX(slot < 64, "close slot overflow should be caught at parse time");
         pt->closeslots |= (1ULL << slot);
      }
   }

   pt->result_types = fs->return_types;

   // Copy try-except metadata to prototype.  These arrays are allocated separately and freed with the proto via the GC.

   if (not fs->try_blocks.empty()) {
      size_t blocks_size   = fs->try_blocks.size() * sizeof(TryBlockDesc);
      size_t handlers_size = fs->try_handlers.size() * sizeof(TryHandlerDesc);

      pt->try_blocks   = (TryBlockDesc *)lj_mem_new(L, blocks_size);
      pt->try_handlers = (TryHandlerDesc *)lj_mem_new(L, handlers_size);

      memcpy(pt->try_blocks, fs->try_blocks.data(), blocks_size);
      memcpy(pt->try_handlers, fs->try_handlers.data(), handlers_size);

      pt->try_block_count   = uint16_t(fs->try_blocks.size());
      pt->try_handler_count = uint16_t(fs->try_handlers.size());
   }
   else {
      pt->try_blocks        = nullptr;
      pt->try_handlers      = nullptr;
      pt->try_block_count   = 0;
      pt->try_handler_count = 0;
   }

   // Set PROTO_TYPEFIX flag for runtime type inference if:
   // 1. The function has NO explicit return type annotations, AND
   // 2. At least one return statement exists (has values to infer)

   bool has_explicit = false;
   for (size_t i = 0; i < fs->return_types.size(); ++i) {
      if (fs->return_types[i] != TiriType::Unknown) {
         has_explicit = true;
         break;
      }
   }

   if (not has_explicit and (fs->flags & PROTO_HAS_RETURN)) pt->flags |= PROTO_TYPEFIX;

   // Close potentially uninitialized gap between bc and kgc.

   *(uint32_t*)((char*)pt + ofsk - sizeof(GCRef) * (fs->nkgc + 1)) = 0;
   fs_fixup_bc(fs, pt, (BCIns*)((char*)pt + sizeof(GCproto)), fs->pc);
   fs_fixup_k(fs, pt, (void*)((char*)pt + ofsk));
   fs_fixup_uv1(fs, pt, (uint16_t*)((char*)pt + ofsuv));
   fs_fixup_line(fs, pt, (void*)((char*)pt + ofsli), numline);
   this->fs_fixup_var(pt, (uint8_t*)((char*)pt + ofsdbg), ofsvar);

   lj_vmevent_send(L, BC,
      setprotoV(L, L->top++, pt);
   );

   L->top--;  // Pop table of constants.
   this->vtop = fs->vbase;  // Reset variable stack.

   func_stack.pop_back();
   this->fs = func_stack.empty() ? nullptr : &func_stack.back();

   lj_assertL(this->fs != nullptr or this->tok IS TK_eof, "bad parser state");
   return pt;
}

//********************************************************************************************************************
// Initialize a new FuncState. Creates a new FuncState in the func_stack container and returns a reference to it.

FuncState & LexState::fs_init()
{
   func_stack.emplace_back();
   FuncState &fs = func_stack.back();
   fs.init(this, this->L, this->vtop, func_stack.size() IS 1);
   this->fs = &fs;
   return fs;
}
