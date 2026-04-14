// Lua parser - Register allocation and bytecode emission.
//
// Copyright © 2025-2026 Paul Manias
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
// Major portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#include "parse_regalloc.h"

#include <kotuku/main.h>
#include <format>

#include "ast/nodes.h"  // For TiriType, tiri_type_to_lj_tag(), type_name()
#include "lj_err.h"     // For lj_err_throw, ErrMsg
#include "lj_tab.h"     // For lj_tab_getstr

[[nodiscard]] BCPOS bcemit_jmp(FuncState *);

//********************************************************************************************************************
// Emit a compile-time type mismatch error.
// This is called when we can statically determine that an assignment will fail type checking.

static void err_type_mismatch(FuncState *fs, TiriType actual_type, TiriType expected_type)
{
   lj_lex_error(fs->ls, 0, ErrMsg::BADASSIGN, type_name(actual_type).data(), type_name(expected_type).data());
}

//********************************************************************************************************************
// Emit a compile-time object class mismatch error.
// This is called when both LHS and RHS are Object types but have different class IDs.

static void err_object_class_mismatch(FuncState *fs, CLASSID actual_class_id, CLASSID expected_class_id)
{
   lj_lex_error(fs->ls, 0, ErrMsg::BADCLASS, ResolveClassID(expected_class_id), ResolveClassID(actual_class_id));
}

[[nodiscard]] inline bool is_register_key(int32_t Aux) { return (Aux >= 0) and (Aux <= BCMAX_C); }

//********************************************************************************************************************
// Register allocation methods

void RegisterAllocator::bump(BCReg Count)
{
   BCREG target = this->func_state->freereg + Count.raw();
   if (target > this->func_state->framesize) {
      if (target >= LJ_MAX_SLOTS) this->func_state->ls->err_syntax(ErrMsg::XSLOTS);
      this->func_state->framesize = uint8_t(target);
   }
}

BCReg RegisterAllocator::reserve_slots(BCReg Count)
{
   if (not Count.raw()) return BCReg(this->func_state->freereg);

   auto start = BCReg(this->func_state->freereg);
   this->bump(Count);
   this->func_state->freereg += Count.raw();
   this->trace_allocation(start, Count, "reserve_slots");
   return start;
}

RegisterSpan RegisterAllocator::reserve_span(BCReg Count)
{
   if (not Count.raw()) return RegisterSpan();

   BCReg start = this->reserve_slots(Count);
   return RegisterSpan(this, start, Count, start + Count.raw());
}

RegisterSpan RegisterAllocator::reserve_span_soft(BCReg Count)
{
   if (not Count.raw()) return RegisterSpan();

   BCReg start = this->reserve_slots(Count);
   // Soft spans do not enforce RAII cleanup - callers manage freereg explicitly.
   // We pass nullptr for the allocator so that the span's destructor becomes a no-op.
   // This is critical because soft spans may outlive the allocator that created them
   // (e.g. when returned in PreparedAssignment structs from prepare_assignment_targets).
   return RegisterSpan(nullptr, start, Count, BCReg(0));
}

void RegisterAllocator::release_span_internal(BCReg Start, BCReg Count, BCReg ExpectedTop)
{
   if (not Count.raw()) return;

   if (this->func_state->is_temp_register(Start)) {
      // Soft spans (ExpectedTop == 0) are used in contexts where the caller explicitly manages freereg (e.g.
      // assignment emitters that duplicate table operands and later restore freereg to varmap.size()). For
      // these spans we do not enforce RAII invariants or adjust freereg here.
      if (ExpectedTop.raw() IS 0) {
         this->trace_release(Start, Count, "release_span_internal_soft");
         return;
      }

      if (this->func_state->freereg > ExpectedTop.raw()) {
         pf::Log("Parser").warning("Register depth mismatch, %d != %d, function @ line %d - "
            "RegisterSpan was created with freereg=%d but released as %d. "
            "This indicates intermediate operations modified freereg or cleanup is out of order.",
            ExpectedTop.raw(), this->func_state->freereg, this->func_state->linedefined.lineNumber(),
            ExpectedTop.raw(), this->func_state->freereg);
      }

      // Check for span size mismatch
      if (Start.raw() + Count.raw() != ExpectedTop.raw()) {
         pf::Log("Parser").warning("Span size mismatch: start=%u count=%u expected_top=%u at line %d",
            Start.raw(), Count.raw(), ExpectedTop.raw(), this->func_state->linedefined.lineNumber());
      }

      this->func_state->freereg = ExpectedTop.raw() - Count.raw();

      // Check that after release, freereg equals the span start
      if (this->func_state->freereg != Start.raw()) {
         pf::Log("Parser").warning("Bad regfree: freereg=%u should equal start=%u at line %d",
            this->func_state->freereg, Start.raw(), this->func_state->linedefined.lineNumber());
      }

      this->trace_release(Start, Count, "release_span_internal");
   }
}

void RegisterAllocator::release(RegisterSpan &Span)
{
   if (Span.allocator_) {
      this->release_span_internal(Span.start_, Span.count_, Span.expected_top_);
      Span.allocator_ = nullptr;
   }
}

void RegisterAllocator::release(AllocatedRegister &Handle)
{
   if (Handle.allocator_) {
      this->release_span_internal(Handle.index_, BCReg(1), Handle.expected_top_);
      Handle.allocator_ = nullptr;
   }
}

void RegisterAllocator::release_register(BCReg Register)
{
   BCReg expected_top = Register + BCREG(1);
   if (this->func_state->is_temp_register(Register) and expected_top.raw() IS this->func_state->freereg) {
      this->release_span_internal(Register, BCReg(1), expected_top);
   }
}

void RegisterAllocator::release_expression(ExpDesc *Expression)
{
   if (Expression->k IS ExpKind::NonReloc) {
      BCReg reg = BCReg(Expression->u.s.info);
      BCReg expected_top = reg + BCREG(1);
      if (this->func_state->is_temp_register(reg) and expected_top.raw() IS this->func_state->freereg) {
         this->release_span_internal(reg, BCReg(1), expected_top);
      }
   }
}

void RegisterAllocator::collapse_freereg(BCReg ResultReg)
{
   BCREG target = ResultReg.raw() + 1;
   if (target < this->func_state->varmap.size()) target = this->func_state->varmap.size();

   while (this->func_state->freereg > target) {
      BCREG previous = this->func_state->freereg;
      BCREG top = previous - 1;
      this->release_register(BCReg(top));
      if (this->func_state->freereg IS previous) break;
   }
}

TableOperandCopies RegisterAllocator::duplicate_table_operands(const ExpDesc &Expression)
{
   TableOperandCopies copies{};
   copies.duplicated = Expression;

   if (Expression.k IS ExpKind::Indexed) {
      uint32_t original_aux = Expression.u.s.aux;
      BCREG duplicate_count = 1;
      bool has_register_index = is_register_key(int32_t(original_aux));

      if (has_register_index) duplicate_count++;

      // Use a soft span here because assignment/update emitters that rely on these duplicates manage freereg
      // explicitly (they collapse freereg back to varmap.size() after completing the operation). Enforcing
      // strict RAII invariants for this span would produce false-positive warnings in perfectly valid patterns like:
      //   t[i] = t[i] | f(i)
      // where additional temporaries are allocated above the duplicated base and later dropped by restoring freereg.

      copies.reserved = this->reserve_span_soft(BCReg(duplicate_count));

      BCREG base_reg = copies.reserved.start().raw();
      bcemit_AD(this->func_state, BC_MOV, base_reg, Expression.u.s.info);
      copies.duplicated.u.s.info = base_reg;

      if (has_register_index) {
         BCREG index_reg = BCREG(base_reg + 1);
         bcemit_AD(this->func_state, BC_MOV, index_reg, BCREG(original_aux));
         copies.duplicated.u.s.aux = index_reg;
      }
   }

   return copies;
}

//********************************************************************************************************************
// Bump frame size.

inline void bcreg_bump(FuncState *fs, BCREG n)
{
   RegisterAllocator allocator(fs);
   allocator.bump(BCReg(n));
}

//********************************************************************************************************************
// Reserve registers.

inline void bcreg_reserve(FuncState *fs, BCREG n)
{
   RegisterAllocator allocator(fs);
   allocator.reserve(BCReg(n));
}

//********************************************************************************************************************
// Free register.

inline void bcreg_free(FuncState *fs, BCREG reg)
{
   RegisterAllocator allocator(fs);
   allocator.release_register(BCReg(reg));
}

//********************************************************************************************************************
// Free register for expression.

inline void expr_free(FuncState *fs, ExpDesc *e)
{
   RegisterAllocator allocator(fs);
   allocator.release_expression(e);
}

//********************************************************************************************************************
// Emit bytecode instruction.
// Exported for use by OperatorEmitter facade

BCPOS bcemit_INS(FuncState *fs, BCIns ins)
{
   BCPOS pc = fs->pc;
   LexState* ls = fs->ls;
   ControlFlowGraph cfg(fs);
   ControlFlowEdge pending = cfg.make_unconditional(fs->pending_jmp());
   pending.patch_with_value(BCPos(pc), BCReg(NO_REG), BCPos(pc));
   fs->clear_pending_jumps();

   if (pc >= fs->bclim) [[unlikely]] {
      ptrdiff_t base = fs->bcbase - ls->bc_stack;
      checklimit(fs, ls->size_bc_stack, LJ_MAX_BCINS, "bytecode instructions");
      lj_mem_growvec(fs->L, ls->bc_stack, ls->size_bc_stack, LJ_MAX_BCINS, BCInsLine);
      fs->bclim = BCPOS(ls->size_bc_stack - base);
      fs->bcbase = ls->bc_stack + base;
   }

   fs->bcbase[pc].ins = ins;
   fs->bcbase[pc].line = ls->effective_line();
   fs->pc = pc + 1;
   return pc;
}

//********************************************************************************************************************
// Bytecode emitter for expressions

// Discharge non-constant expression to any register.

static void expr_discharge(FuncState *fs, ExpDesc *e)
{
   BCIns ins;
   if (e->k IS ExpKind::Upval) {
      ins = BCINS_AD(BC_UGET, 0, e->u.s.info);
   }
   else if (e->k IS ExpKind::Global or e->k IS ExpKind::Unscoped) {
      // Check if trying to read blank identifier.
      if (is_blank_identifier(e->u.sval)) {
         lj_lex_error(fs->ls, 0, ErrMsg::XBLANKREAD);
      }
      // For *reads*, if an expression remains Unscoped on discharge then it defaults to global table lookup as this
      // allows externally defined globals (e.g. from 'require' or 'loadFile') to work as expected.
      ins = BCINS_AD(BC_GGET, 0, const_str(fs, e));
   }
   else if (e->k IS ExpKind::Indexed) {
      BCREG rc = e->u.s.aux;
      if (int32_t(rc) < 0) {
         int32_t idx = ~rc;
         if (idx <= BCMAX_C) {
            ins = BCINS_ABC(BC_TGETS, 0, e->u.s.info, idx);
         }
         else {
            // Overflow: load string constant into temp register, use TGETV
            BCREG key_reg = fs->freereg;
            bcreg_reserve(fs, 1);
            bcemit_AD(fs, BC_KSTR, key_reg, BCREG(idx));
            bcreg_free(fs, key_reg);
            ins = BCINS_ABC(BC_TGETV, 0, e->u.s.info, key_reg);
         }
      }
      else if (rc > BCMAX_C) ins = BCINS_ABC(BC_TGETB, 0, e->u.s.info, rc - (BCMAX_C + 1));
      else {
         bcreg_free(fs, rc);
         ins = BCINS_ABC(BC_TGETV, 0, e->u.s.info, rc);
      }
      bcreg_free(fs, e->u.s.info);
   }
   else if (e->k IS ExpKind::IndexedArray) {
      // Array indexing - emit BC_AGETV or BC_AGETB
      // Note: Arrays don't support string keys, so no BC_AGETS equivalent
      BCREG rc = e->u.s.aux;
      if (rc > BCMAX_C) ins = BCINS_ABC(BC_AGETB, 0, e->u.s.info, rc - (BCMAX_C + 1));
      else {
         bcreg_free(fs, rc);
         ins = BCINS_ABC(BC_AGETV, 0, e->u.s.info, rc);
      }
      bcreg_free(fs, e->u.s.info);
   }
   else if (e->k IS ExpKind::SafeIndexedArray) {
      // Safe array indexing - emit BC_ASGETV or BC_ASGETB (returns nil for out-of-bounds)
      BCREG rc = e->u.s.aux;
      if (rc > BCMAX_C) ins = BCINS_ABC(BC_ASGETB, 0, e->u.s.info, rc - (BCMAX_C + 1));
      else {
         bcreg_free(fs, rc);
         ins = BCINS_ABC(BC_ASGETV, 0, e->u.s.info, rc);
      }
      bcreg_free(fs, e->u.s.info);
   }
   else if (e->k IS ExpKind::IndexedObject) {
      // Object field access - emit BC_OBGETF for string key (object fields are always strings)
      // aux holds negated string constant index (same encoding as BC_TGETS)
      BCREG rc = e->u.s.aux;
      fs_check_assert(fs, int32_t(rc) < 0, "object field index must be string constant");
      BCREG idx = (BCREG)~rc;
      fs_check_assert(fs, idx <= BCMAX_C, "object field string constant index out of range");
      ins = BCINS_ABCP(BC_OBGETF, 0, e->u.s.info, idx, 0xFFFFFFFFu);
      bcreg_free(fs, e->u.s.info);
   }
   else if (e->k IS ExpKind::Call) {
      e->u.s.info = e->u.s.aux;
      e->k = ExpKind::NonReloc;
      return;
   }
   else if (e->k IS ExpKind::Local) {
      e->k = ExpKind::NonReloc;
      return;
   }
   else return;

   e->u.s.info = bcemit_INS(fs, ins);
   e->k = ExpKind::Relocable;
}

//********************************************************************************************************************
// Emit bytecode to set a range of registers to nil.

static void bcemit_nil(FuncState *fs, BCREG from, BCREG n)
{
   if (fs->pc > fs->lasttarget) {  // No jumps to current position?
      BCIns* ip = &fs->last_instruction().ins;
      BCREG pto, pfrom = bc_a(*ip);
      switch (bc_op(*ip)) {  // Try to merge with the previous instruction.
         case BC_KPRI:
            if (bc_d(*ip) != ~LJ_TNIL) break;
            if (from IS pfrom) {
               if (n IS 1) return;
            }
            else if (from IS pfrom + 1) {
               from = pfrom;
               n++;
            }
            else break;
            *ip = BCINS_AD(BC_KNIL, from, from + n - 1);  // Replace KPRI.
            return;
         case BC_KNIL:
            pto = bc_d(*ip);
            if (pfrom <= from and from <= pto + 1) {  // Can we connect both ranges?
               if (from + n - 1 > pto) setbc_d(ip, from + n - 1);  // Patch previous instruction range.
               return;
            }
            break;
         default:
            break;
      }
   }

   // Emit new instruction or replace old instruction.
   bcemit_INS(fs, n IS 1 ? BCINS_AD(BC_KPRI, from, ExpKind::Nil) :
      BCINS_AD(BC_KNIL, from, from + n - 1));
}

//********************************************************************************************************************
// Discharge an expression to a specific register. Ignore branches.

static void expr_toreg_nobranch(FuncState *fs, ExpDesc *e, BCREG reg)
{
   BCIns ins;
   expr_discharge(fs, e);
   if (e->k IS ExpKind::Str) {
      ins = BCINS_AD(BC_KSTR, reg, const_str(fs, e));
   }
   else if (e->k IS ExpKind::Num) {
#if LJ_DUALNUM
      cTValue* tv = e->num_tv();
      if (tvisint(tv) and checki16(intV(tv)))
         ins = BCINS_AD(BC_KSHORT, reg, BCREG(uint16_t(intV(tv))));
      else
#else
      lua_Number n = e->number_value();
      int32_t k = lj_num2int(n);
      if (checki16(k) and n IS lua_Number(k))
         ins = BCINS_AD(BC_KSHORT, reg, BCREG(uint16_t(k)));
      else
#endif
         ins = BCINS_AD(BC_KNUM, reg, const_num(fs, e));
   }
   else if (e->k IS ExpKind::Relocable) {
      setbc_a(bcptr(fs, e), reg);
      goto noins;
   }
   else if (e->k IS ExpKind::NonReloc) {
      if (reg IS e->u.s.info) goto noins;
      ins = BCINS_AD(BC_MOV, reg, e->u.s.info);
   }
   else if (e->k IS ExpKind::Nil) {
      bcemit_nil(fs, reg, 1);
      goto noins;
   }
   else if (e->k <= ExpKind::True) {
      ins = BCINS_AD(BC_KPRI, reg, const_pri(e));
   }
   else {
      fs_check_assert(fs,e->k IS ExpKind::Void or e->k IS ExpKind::Jmp, "bad expr type %d", int(e->k));
      return;
   }

   bcemit_INS(fs, ins);

noins:
   e->u.s.info = reg;
   e->k = ExpKind::NonReloc;
}

//********************************************************************************************************************
// Discharge an expression to a specific register.

static void expr_toreg(FuncState *fs, ExpDesc *e, BCREG reg)
{
   expr_toreg_nobranch(fs, e, reg);
   ControlFlowGraph cfg(fs);

   if (e->k IS ExpKind::Jmp) {
      ControlFlowEdge true_edge = cfg.make_true_edge(BCPos(e->t));
      true_edge.append(BCPos(e->u.s.info));
      e->t = true_edge.head().raw();
   }

   if (e->has_jump()) {  // Discharge expression with branches.
      BCPOS jend, jfalse = NO_JMP, jtrue = NO_JMP;
      ControlFlowEdge true_edge = cfg.make_true_edge(BCPos(e->t));
      ControlFlowEdge false_edge = cfg.make_false_edge(BCPos(e->f));

      if (true_edge.produces_values() or false_edge.produces_values()) {
         BCPOS jval = (e->k IS ExpKind::Jmp) ? NO_JMP : bcemit_jmp(fs);
         jfalse = bcemit_AD(fs, BC_KPRI, reg, BCREG(ExpKind::False));
         bcemit_AJ(fs, BC_JMP, fs->freereg, 1);
         jtrue = bcemit_AD(fs, BC_KPRI, reg, BCREG(ExpKind::True));
         ControlFlowEdge jval_edge = cfg.make_unconditional(BCPos(jval));
         jval_edge.patch_here();
      }

      jend = fs->pc;
      fs->lasttarget = jend;
      false_edge.patch_with_value(BCPos(jend), BCReg(reg), BCPos(jfalse));
      true_edge.patch_with_value(BCPos(jend), BCReg(reg), BCPos(jtrue));
   }

   e->f = e->t = NO_JMP;
   e->u.s.info = reg;
   e->k = ExpKind::NonReloc;
}

//********************************************************************************************************************
// Discharge an expression to the next free register.

static void expr_tonextreg(FuncState *fs, ExpDesc *e)
{
   expr_discharge(fs, e);
   expr_free(fs, e);
   bcreg_reserve(fs, 1);
   expr_toreg(fs, e, fs->freereg - 1);
}

//********************************************************************************************************************
// Discharge an expression to any register.

static BCREG expr_toanyreg(FuncState *fs, ExpDesc *e)
{
   expr_discharge(fs, e);
   if (e->k IS ExpKind::NonReloc) [[likely]] {
      if (!e->has_jump()) [[likely]] return e->u.s.info;  // Already in a register.
      if (e->u.s.info >= fs->varmap.size()) {
         expr_toreg(fs, e, e->u.s.info);  // Discharge to temp. register.
         return e->u.s.info;
      }
   }
   expr_tonextreg(fs, e);  // Discharge to next register.
   return e->u.s.info;
}

//********************************************************************************************************************
// Partially discharge expression to a value.

static void expr_toval(FuncState *fs, ExpDesc *e)
{
   if (e->has_jump()) [[unlikely]]
      expr_toanyreg(fs, e);
   else [[likely]]
      expr_discharge(fs, e);
}

//********************************************************************************************************************
// Emit store for LHS expression.

static void bcemit_store(FuncState *fs, ExpDesc *LHS, ExpDesc *RHS)
{
   BCIns ins;
   if (LHS->k IS ExpKind::Local) {
      fs->ls->vstack[LHS->u.s.aux].info |= VarInfoFlag::VarReadWrite;
      VarInfo *vinfo = &fs->ls->vstack[LHS->u.s.aux];
      TiriType fixed = vinfo->fixed_type;

      // Check if this variable has a defined type and needs runtime type checking
      if ((fixed != TiriType::Unknown) and (fixed != TiriType::Any)) {
         bool needs_check = true;
         TiriType static_rhs_type = TiriType::Unknown; // TODO: needs_check can emit simpler tests when RHS type is known.

         // Check for statically-known types - either from literals or from expression result types
         if (RHS->k IS ExpKind::Nil) {
            needs_check = false;  // nil is always allowed (clears the variable)
         }
         else if (RHS->k IS ExpKind::False or RHS->k IS ExpKind::True) {
            static_rhs_type = TiriType::Bool;
            if (fixed IS TiriType::Bool) needs_check = false;
            else err_type_mismatch(fs, TiriType::Bool, fixed);
         }
         else if (RHS->k IS ExpKind::Str) {
            static_rhs_type = TiriType::Str;
            if (fixed IS TiriType::Str) needs_check = false;
            else err_type_mismatch(fs, TiriType::Str, fixed);
         }
         else if (RHS->k IS ExpKind::Num) {
            static_rhs_type = TiriType::Num;
            if (fixed IS TiriType::Num) needs_check = false;
            else err_type_mismatch(fs, TiriType::Num, fixed);
         }
         // Check if expression has known result type (e.g., from function call with declared return type,
         // or operators with statically known result types like arithmetic, comparison, concatenation)
         else if (RHS->result_type != TiriType::Unknown and RHS->result_type != TiriType::Any) {
            // TODO: For maxing optimisation, uncomment this assert to help find areas where type checks aren't
            // being handled by the compiler.  Ideally we would handle all type checking at compile time, in which
            // case this assert is never raised.
            //fs_check_assert(fs, RHS->result_type IS fixed, "expected function return type (RHS) to match variable (LHS)");

            static_rhs_type = RHS->result_type;

            // If the RHS result type matches the variable's fixed type, skip runtime check
            // Otherwise, fall through to runtime check (we don't emit compile-time errors for
            // expression results unless type_confirmed is set, because source location tracking
            // isn't accurate at this point)
            needs_check = (RHS->result_type != fixed);

            // For object field accesses with confirmed types from class dictionary lookups,
            // emit compile-time type mismatch error
            if (needs_check and RHS->type_confirmed) {
               err_type_mismatch(fs, RHS->result_type, fixed);
            }

            // For Object types with known class IDs, check for class mismatch at compile time
            if (not needs_check and fixed IS TiriType::Object and vinfo->object_class_id != CLASSID::NIL) {
               if (vinfo->object_class_id != RHS->object_class_id) {
                  // Object class mismatch - emit compile-time error
                  err_object_class_mismatch(fs, RHS->object_class_id, vinfo->object_class_id);
               }
            }
         }

         if (needs_check) {
            // For dynamic values, emit runtime type check
            // First materialise value to a register

            BCREG src_reg = expr_toanyreg(fs, RHS);

            // Skip type check for nil values (nil is always allowed as a "clear" operation)
            // BC_ISNEP checks if value != nil, skips next instruction if true (not nil)
            // We want to skip BC_ISTYPE when value IS nil, so use BC_ISEQP with nil primitive

            ExpDesc nilv(ExpKind::Nil);
            bcemit_INS(fs, BCINS_AD(BC_ISEQP, src_reg, const_pri(&nilv)));
            BCPOS skip_pos = bcemit_jmp(fs);

            // Emit type check instruction
            // For numbers, use BC_ISNUM (because BC_ISTYPE doesn't work for numbers without LJ_DUALNUM)
            // For other types, use BC_ISTYPE with (base_tag + 1) as the D operand

            if (fixed IS TiriType::Num) {
               // BC_ISNUM checks if value is a number (any floating point value)
               // The D operand should be ~LJ_TNUMX + 2 = 16 (see lj_meta_istype which does tp--)
               bcemit_AD(fs, BC_ISNUM, src_reg, ~LJ_TNUMX + 2);
            }
            else {
               // BC_ISTYPE - the D operand is (base_tag + 1) to match the VM's itype comparison:
               //   itype = ~base = -(base+1), so itype + (base+1) = 0 when types match
               uint8_t lj_tag = tiri_type_to_lj_tag(fixed);
               bcemit_AD(fs, BC_ISTYPE, src_reg, lj_tag + 1);
            }

            // Patch jump to skip over BC_ISTYPE when value is nil
            ControlFlowGraph cfg(fs);
            ControlFlowEdge skip_edge = cfg.make_unconditional(BCPos(skip_pos));
            skip_edge.patch_here();

            // Update expression state - value is now in src_reg
            RHS->k = ExpKind::NonReloc;
            RHS->u.s.info = src_reg;
         }
      }

      expr_free(fs, RHS);
      expr_toreg(fs, RHS, LHS->u.s.info);
      return;
   }
   else if (LHS->k IS ExpKind::Upval) {
      fs->ls->vstack[LHS->u.s.aux].info |= VarInfoFlag::VarReadWrite;
      expr_toval(fs, RHS);
      if (RHS->k <= ExpKind::True) ins = BCINS_AD(BC_USETP, LHS->u.s.info, const_pri(RHS));
      else if (RHS->k IS ExpKind::Str) ins = BCINS_AD(BC_USETS, LHS->u.s.info, const_str(fs, RHS));
      else if (RHS->k IS ExpKind::Num) ins = BCINS_AD(BC_USETN, LHS->u.s.info, const_num(fs, RHS));
      else ins = BCINS_AD(BC_USETV, LHS->u.s.info, expr_toanyreg(fs, RHS));
   }
   else if (LHS->k IS ExpKind::Global or LHS->k IS ExpKind::Unscoped) {
      // Note: Const global reassignment is checked during type analysis phase
      // Unscoped should normally be resolved in emit_lvalue_expr(), but handle it here defensively
      BCREG ra = expr_toanyreg(fs, RHS);
      ins = BCINS_AD(BC_GSET, ra, const_str(fs, LHS));
   }
   else if (LHS->k IS ExpKind::IndexedArray or LHS->k IS ExpKind::SafeIndexedArray) {
      // Array index assignment - emit BC_ASETV or BC_ASETB
      // Note: SafeIndexedArray uses same SET bytecodes as IndexedArray (safe is only for reads)
      BCREG ra, rc;
      ra = expr_toanyreg(fs, RHS);
      rc = LHS->u.s.aux;
      if (rc > BCMAX_C) {
         ins = BCINS_ABC(BC_ASETB, ra, LHS->u.s.info, rc - (BCMAX_C + 1));
      }
      else {
#ifdef LUA_USE_ASSERT
         // Free late alloced key reg to avoid assert on free of value reg.
         if (RHS->k IS ExpKind::NonReloc and ra >= fs->varmap.size() and rc >= ra) bcreg_free(fs, rc);
#endif
         ins = BCINS_ABC(BC_ASETV, ra, LHS->u.s.info, rc);
      }
   }
   else if (LHS->k IS ExpKind::IndexedObject) {
      // Object field assignment - emit BC_OBSETF for string key (object fields are always strings)
      BCREG ra, rc;
      ra = expr_toanyreg(fs, RHS);
      rc = LHS->u.s.aux;
      fs_check_assert(fs, int32_t(rc) < 0, "object field index must be string constant");
      ins = BCINS_ABCP(BC_OBSETF, ra, LHS->u.s.info, (~rc) & 0xFFu, 0xFFFFFFFFu);
   }
   else {
      // Table index assignment - emit BC_TSETV, BC_TSETB, or BC_TSETS
      BCREG ra, rc;
      fs_check_assert(fs, LHS->k IS ExpKind::Indexed, "bad expr type %d", int(LHS->k));
      ra = expr_toanyreg(fs, RHS);
      rc = LHS->u.s.aux;
      if (int32_t(rc) < 0) {
         /* String constant key: rc encodes the index as ~index. */
         int32_t stridx = ~int32_t(rc);
         if (stridx <= BCMAX_C) {
            ins = BCINS_ABC(BC_TSETS, ra, LHS->u.s.info, BCREG(stridx));
         } else {
            /* Overflow-safe path: load string constant into a register and use TSETV. */
            BCREG key_reg = fs->freereg;
            bcreg_reserve(fs, 1);
            bcemit_AD(fs, BC_KSTR, key_reg, BCREG(stridx));
            ins = BCINS_ABC(BC_TSETV, ra, LHS->u.s.info, key_reg);
#ifdef LUA_USE_ASSERT
            /* Free late alloced key reg to avoid assert on free of value reg. */
            /* This can only happen when called from expr_table(). */
            if (RHS->k IS ExpKind::NonReloc and ra >= fs->varmap.size() and rc >= ra) bcreg_free(fs, rc);
#endif
            bcemit_INS(fs, ins);
            bcreg_free(fs, key_reg);
            expr_free(fs, RHS);
            return;
         }
      } else if (rc > BCMAX_C) {
         ins = BCINS_ABC(BC_TSETB, ra, LHS->u.s.info, rc - (BCMAX_C + 1));
      } else {
#ifdef LUA_USE_ASSERT
         // Free late alloced key reg to avoid assert on free of value reg.
         // This can only happen when called from expr_table().
         if (RHS->k IS ExpKind::NonReloc and ra >= fs->varmap.size() and rc >= ra) bcreg_free(fs, rc);
#endif
         ins = BCINS_ABC(BC_TSETV, ra, LHS->u.s.info, rc);
      }
   }
   bcemit_INS(fs, ins);
   expr_free(fs, RHS);
}

//********************************************************************************************************************
// Emit method lookup expression.

static void bcemit_method(FuncState *fs, ExpDesc *e, ExpDesc *key)
{
   BCREG idx, func, obj = expr_toanyreg(fs, e);
   expr_free(fs, e);
   func = fs->freereg;
   bcemit_AD(fs, BC_MOV, func + 1 + LJ_FR2, obj);  // Copy object to 1st argument.
   fs_check_assert(fs, key->is_str_constant(), "bad usage");
   idx = const_str(fs, key);
   if (idx <= BCMAX_C) {
      bcreg_reserve(fs, 2 + LJ_FR2);
      bcemit_ABC(fs, BC_TGETS, func, obj, idx);
   }
   else {
      bcreg_reserve(fs, 3 + LJ_FR2);
      bcemit_AD(fs, BC_KSTR, func + 2 + LJ_FR2, idx);
      bcemit_ABC(fs, BC_TGETV, func, obj, func + 2 + LJ_FR2);
      fs->freereg--;
   }
   e->u.s.info = func;
   e->k = ExpKind::NonReloc;
}

//********************************************************************************************************************
// Emit unconditional branch.

[[nodiscard]] BCPOS bcemit_jmp(FuncState *fs)
{
   BCPOS jpc = fs->jpc;
   BCPOS j = fs->pc - 1;
   BCIns* ip = &fs->bcbase[j].ins;
   fs->clear_pending_jumps();
   if (int32_t(j) >= int32_t(fs->lasttarget) and bc_op(*ip) IS BC_UCLO) {
      setbc_j(ip, NO_JMP);
      fs->lasttarget = j + 1;
   }
   else j = bcemit_AJ(fs, BC_JMP, fs->freereg, NO_JMP);
   ControlFlowGraph cfg(fs);
   ControlFlowEdge edge = cfg.make_unconditional(BCPos(j));
   edge.append(BCPos(jpc));
   return edge.head().raw();
}

//********************************************************************************************************************
// Invert branch condition of bytecode instruction.

inline void invertcond(FuncState *fs, ExpDesc *e)
{
   BCIns* ip = &fs->bytecode_at(BCPos(e->u.s.info - 1)).ins;
   setbc_op(ip, bc_op(*ip) ^ 1);
}

//********************************************************************************************************************
// Emit conditional branch.

[[nodiscard]] BCPOS bcemit_branch(FuncState *fs, ExpDesc *e, int cond)
{
   BCPOS pc;

   if (e->k IS ExpKind::Relocable) {
      BCIns* ip = bcptr(fs, e);
      if (bc_op(*ip) IS BC_NOT) {
         *ip = BCINS_AD(cond ? BC_ISF : BC_IST, 0, bc_d(*ip));
         return bcemit_jmp(fs);
      }
   }

   if (e->k != ExpKind::NonReloc) {
      bcreg_reserve(fs, 1);
      expr_toreg_nobranch(fs, e, fs->freereg - 1);
   }

   bcemit_AD(fs, cond ? BC_ISTC : BC_ISFC, NO_REG, e->u.s.info);
   pc = bcemit_jmp(fs);
   expr_free(fs, e);
   return pc;
}

//********************************************************************************************************************
// Emit branch on true condition.

static void bcemit_branch_t(FuncState *fs, ExpDesc *e)
{
   BCPOS pc;
   expr_discharge(fs, e);
   if (e->k IS ExpKind::Str or e->k IS ExpKind::Num or e->k IS ExpKind::True) pc = NO_JMP;  // Never jump.
   else if (e->k IS ExpKind::Jmp) invertcond(fs, e), pc = e->u.s.info;
   else if (e->k IS ExpKind::False or e->k IS ExpKind::Nil) expr_toreg_nobranch(fs, e, NO_REG), pc = bcemit_jmp(fs);
   else pc = bcemit_branch(fs, e, 0);
   ControlFlowGraph cfg(fs);
   ControlFlowEdge false_edge = cfg.make_false_edge(BCPos(e->f));
   false_edge.append(BCPos(pc));
   e->f = false_edge.head().raw();
   ControlFlowEdge true_edge = cfg.make_true_edge(BCPos(e->t));
   true_edge.patch_here();
   e->t = NO_JMP;
}

//********************************************************************************************************************
// Debug verification and tracing methods

void RegisterAllocator::verify_no_leaks(const char* Context) const
{
   BCREG total_locals = BCREG(this->func_state->varmap.size());
   BCREG freereg = this->func_state->freereg;

   if (freereg > total_locals) {
      pf::Log("Parser").warning("Register leak at %s: %d temporary registers not released (total locals=%d, free reg=%d)",
         Context, int(freereg - total_locals), int(total_locals), int(freereg));
   }
}

void RegisterAllocator::trace_allocation(BCReg Start, BCReg Count, const char* Context) const
{
   auto prv = (prvTiri *)this->func_state->L->script->ChildPrivate;
   if ((prv->JitOptions & JOF::TRACE_REGISTERS) != JOF::NIL) {
      pf::Log("Parser").msg("Regalloc: reserve R%d..R%d (%d slots) at %s",
         int(Start.raw()), int(Start.raw() + Count.raw() - 1), int(Count.raw()), Context);
   }
}

void RegisterAllocator::trace_release(BCReg Start, BCReg Count, const char* Context) const
{
   auto prv = (prvTiri *)this->func_state->L->script->ChildPrivate;
   if ((prv->JitOptions & JOF::TRACE_REGISTERS) != JOF::NIL) {
      pf::Log("Parser").msg("Regalloc: release R%d..R%d (%d slots) at %s",
         int(Start.raw()), int(Start.raw() + Count.raw() - 1), int(Count.raw()), Context);
   }
}

//********************************************************************************************************************
// Expression management methods - migrated from static functions to RegisterAllocator methods

void RegisterAllocator::discharge(ExpDesc &Expression)
{
   expr_discharge(this->func_state, &Expression);
}

void RegisterAllocator::discharge_to_register(ExpDesc &Expression, BCReg Target)
{
   expr_toreg(this->func_state, &Expression, Target.raw());
}

void RegisterAllocator::discharge_to_register_nobranch(ExpDesc &Expression, BCReg Target)
{
   expr_toreg_nobranch(this->func_state, &Expression, Target.raw());
}

void RegisterAllocator::discharge_to_next_register(ExpDesc &Expression)
{
   expr_tonextreg(this->func_state, &Expression);
}

BCReg RegisterAllocator::discharge_to_any_register(ExpDesc &Expression)
{
   return BCReg(expr_toanyreg(this->func_state, &Expression));
}

void RegisterAllocator::discharge_to_value(ExpDesc &Expression)
{
   expr_toval(this->func_state, &Expression);
}

void RegisterAllocator::store_value(ExpDesc& Variable, ExpDesc& Value)
{
   bcemit_store(this->func_state, &Variable, &Value);
}

void RegisterAllocator::emit_nil_range(BCReg Start, BCReg Count)
{
   bcemit_nil(this->func_state, Start.raw(), Count.raw());
}
