// Lua parser - Register allocation and bytecode emission.
//
// Copyright © 2025-2026 Paul Manias
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
// Major portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#include "parse_regalloc.h"

#include <kotuku/main.h>
#include <format>
#include <string>

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

static void err_object_class_mismatch(FuncState *Fs, CLASSID ActualClassId, CLASSID ExpectedClassId)
{
   lj_lex_error(Fs->ls, 0, ErrMsg::BADCLASS, ResolveClassID(ExpectedClassId), ResolveClassID(ActualClassId));
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
         kt::Log("Parser").warning("Register depth mismatch, %d != %d, function @ line %d - "
            "RegisterSpan was created with freereg=%d but released as %d. "
            "This indicates intermediate operations modified freereg or cleanup is out of order.",
            ExpectedTop.raw(), this->func_state->freereg, this->func_state->linedefined.lineNumber(),
            ExpectedTop.raw(), this->func_state->freereg);
      }

      // Check for span size mismatch
      if (Start.raw() + Count.raw() != ExpectedTop.raw()) {
         kt::Log("Parser").warning("Span size mismatch: start=%u count=%u expected_top=%u at line %d",
            Start.raw(), Count.raw(), ExpectedTop.raw(), this->func_state->linedefined.lineNumber());
      }

      this->func_state->freereg = ExpectedTop.raw() - Count.raw();

      // Check that after release, freereg equals the span start
      if (this->func_state->freereg != Start.raw()) {
         kt::Log("Parser").warning("Bad regfree: freereg=%u should equal start=%u at line %d",
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
            BCREG table_reg = e->u.s.info;
            e->u.s.info = bcemit_tgets(fs, 0, table_reg, BCREG(idx));
            bcreg_free(fs, table_reg);
            e->k = ExpKind::Relocable;
            return;
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
      if (idx > BCMAX_C) {
         err_limit(fs, BCMAX_C + 1, "object field string constants");
         return;
      }
      ins = BCINS_ABCP(BC_OBGETF, 0, e->u.s.info, idx, 0xFFFFFFFFu);
      bcreg_free(fs, e->u.s.info);
   }
   else if (e->k IS ExpKind::IndexedStruct) {
      // Struct field access - emit BC_STGETF for a string key.
      BCREG rc = e->u.s.aux;
      fs_check_assert(fs, int32_t(rc) < 0, "struct field index must be string constant");
      BCREG idx = (BCREG)~rc;
      if (idx > BCMAX_C) {
         err_limit(fs, BCMAX_C + 1, "struct field string constants");
         return;
      }
      ins = BCINS_ABCP(BC_STGETF, 0, e->u.s.info, idx, e->struct_field_index);
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
// Encode and emit one portable runtime contract descriptor.

static void contract_append_text(FuncState *fs, std::string &Descriptor, std::string_view Text,
   const char *Description)
{
   if (Text.size() > UINT8_MAX) err_limit(fs, UINT8_MAX, Description);
   Descriptor.push_back(char(uint8_t(Text.size())));
   Descriptor.append(Text);
}

static void bcemit_contract(FuncState *fs, BCREG Base, std::span<const RuntimeContract> Contracts,
   BCREG StaticValueCount, bool DynamicCount, bool Variadic)
{
   if (Contracts.empty()) return;

   // Dynamic result counts still require interpreter-only MRSAVE/MRRESTORE handling.  Fixed contracts have exact
   // recorder predicates, including callable values, named structures, ranges and userdata.
   if (DynamicCount) fs->flags |= PROTO_NOJIT;
   for (const RuntimeContract &contract : Contracts) {
      // Global const descriptors validate before the store and publish environment policy afterwards.  The recorder
      // only emits type guards for BC_CONTRACT, so the post-store side effect must remain interpreter-only.
      if (contract.is_const) {
         fs->flags |= PROTO_NOJIT;
         break;
      }
   }

   std::string descriptor;
   descriptor.reserve(5 + Contracts.size() * 8);
   descriptor.push_back(char(TIRI_CONTRACT_VERSION));
   descriptor.push_back(char(uint8_t(Contracts.front().boundary)));

   uint8_t descriptor_flags = 0;
   if (DynamicCount) descriptor_flags |= contract_flag(ContractDescriptorFlag::DynamicCount);
   if (Variadic) descriptor_flags |= contract_flag(ContractDescriptorFlag::Variadic);
   descriptor.push_back(char(descriptor_flags));
   descriptor.push_back(char(uint8_t(StaticValueCount)));
   descriptor.push_back(char(uint8_t(Contracts.size())));

   for (const RuntimeContract &contract : Contracts) {
      descriptor.push_back(char(uint8_t(contract.type)));
      uint8_t entry_flags = 0;
      if (contract.nullable) entry_flags |= contract_flag(ContractEntryFlag::Nullable);
      if (contract.required) entry_flags |= contract_flag(ContractEntryFlag::Required);
      if (contract.is_const) entry_flags |= contract_flag(ContractEntryFlag::Const);
      if (contract.initialising) entry_flags |= contract_flag(ContractEntryFlag::Initialising);
      descriptor.push_back(char(entry_flags));
      descriptor.push_back(char(contract.position));

      std::string_view struct_name;
      if (contract.struct_def) struct_name = contract.struct_def->Name;
      contract_append_text(fs, descriptor, struct_name, "runtime contract structure name");

      std::string_view label;
      if (contract.label and contract.label != NAME_BLANK) {
         label = std::string_view(strdata(contract.label), contract.label->len);
      }
      contract_append_text(fs, descriptor, label, "runtime contract label");
   }

   GCstr *encoded = fs->ls->keepstr(std::string_view(descriptor.data(), descriptor.size()));
   ExpDesc constant(encoded);
   bcemit_AD(fs, BC_CONTRACT, Base, const_str(fs, &constant));
}

//********************************************************************************************************************
// Literal values are closed proofs.  Every other ExpDesc type is advisory and receives a runtime check.

[[nodiscard]] static bool contract_literal_proved(FuncState *fs, ExpDesc *Value, TiriType Expected)
{
   if (Value->k IS ExpKind::Nil) return true;

   TiriType actual = TiriType::Unknown;
   if (Value->k IS ExpKind::False or Value->k IS ExpKind::True) actual = TiriType::Bool;
   else if (Value->k IS ExpKind::Str) actual = TiriType::Str;
   else if (Value->k IS ExpKind::Num) actual = TiriType::Num;
   else return false;

   if (actual IS Expected) return true;
   err_type_mismatch(fs, actual, Expected);
   return false;
}

static void bcemit_value_contract(
   FuncState *fs, ExpDesc *Value, const RuntimeContract &Contract, bool ForceRuntimeCheck = false)
{
   if (Contract.type IS TiriType::Unknown) return;
   if (Contract.type IS TiriType::Any) {
      if (not ForceRuntimeCheck) return;
      BCREG source = expr_toanyreg(fs, Value);
      bcemit_contract(fs, source, std::span(&Contract, 1), 1);
      Value->k = ExpKind::NonReloc;
      Value->u.s.info = source;
      return;
   }
   if (contract_literal_proved(fs, Value, Contract.type) and not ForceRuntimeCheck) return;
   if (Value->static_value and fs->ls->active_context) {
      const auto &descriptor = fs->ls->active_context->descriptors().value(Value->static_value);
      bool same_type = descriptor.primary IS Contract.type;
      bool same_struct = Contract.type != TiriType::Struct or descriptor.struct_def IS Contract.struct_def;
      bool same_nullability = descriptor.nullable IS Contract.nullable;
      if (same_type and same_struct and same_nullability and descriptor.proved() and not ForceRuntimeCheck) return;
   }

   BCREG source = expr_toanyreg(fs, Value);
   bcemit_contract(fs, source, std::span(&Contract, 1), 1);
   Value->k = ExpKind::NonReloc;
   Value->u.s.info = source;
}

static void check_object_class_assignment(FuncState *Fs, const VarInfo &Variable, const ExpDesc &Value)
{
   if (Variable.fixed_type IS TiriType::Object and Variable.object_class_id != CLASSID::NIL and
       Value.result_type IS TiriType::Object and Value.object_class_id != Variable.object_class_id) {
      err_object_class_mismatch(Fs, Value.object_class_id, Variable.object_class_id);
   }
}

//********************************************************************************************************************
// Emit store for LHS expression.

static void bcemit_store(FuncState *fs, ExpDesc *LHS, ExpDesc *RHS)
{
   BCIns ins;
   RuntimeContract global_const_finaliser;
   BCREG global_const_base = 0;
   bool finalise_global_const = false;
   if (LHS->k IS ExpKind::Local) {
      fs->ls->vstack[LHS->u.s.aux].info |= VarInfoFlag::VarReadWrite;
      VarInfo *vinfo = &fs->ls->vstack[LHS->u.s.aux];
      check_object_class_assignment(fs, *vinfo, *RHS);
      RuntimeContract contract{
         .type = vinfo->fixed_type,
         .struct_def = vinfo->struct_def,
         .label = strref(vinfo->name),
         .boundary = ContractBoundary::Local,
         .position = uint8_t(vinfo->slot + 1)
      };
      bcemit_value_contract(fs, RHS, contract);

      expr_free(fs, RHS);
      expr_toreg(fs, RHS, LHS->u.s.info);
      return;
   }
   else if (LHS->k IS ExpKind::Upval) {
      fs->ls->vstack[LHS->u.s.aux].info |= VarInfoFlag::VarReadWrite;
      VarInfo *vinfo = &fs->ls->vstack[LHS->u.s.aux];
      check_object_class_assignment(fs, *vinfo, *RHS);
      RuntimeContract contract{
         .type = vinfo->fixed_type,
         .struct_def = vinfo->struct_def,
         .label = strref(vinfo->name),
         .boundary = ContractBoundary::Upvalue,
         .position = uint8_t(LHS->u.s.info + 1)
      };
      bcemit_value_contract(fs, RHS, contract);
      expr_toval(fs, RHS);
      if (RHS->k <= ExpKind::True) ins = BCINS_AD(BC_USETP, LHS->u.s.info, const_pri(RHS));
      else if (RHS->k IS ExpKind::Str) ins = BCINS_AD(BC_USETS, LHS->u.s.info, const_str(fs, RHS));
      else if (RHS->k IS ExpKind::Num) ins = BCINS_AD(BC_USETN, LHS->u.s.info, const_num(fs, RHS));
      else ins = BCINS_AD(BC_USETV, LHS->u.s.info, expr_toanyreg(fs, RHS));
   }
   else if (LHS->k IS ExpKind::Global or LHS->k IS ExpKind::Unscoped) {
      // Note: Const global reassignment is checked during type analysis phase
      // Unscoped should normally be resolved in emit_lvalue_expr(), but handle it here defensively
      auto found = fs->ls->global_type_hints.find(LHS->u.sval);
      if (found != fs->ls->global_type_hints.end() and
          found->second.contract_policy != GlobalContractPolicy::Advisory) {
         RuntimeContract contract{
            .type = found->second.primary,
            .struct_def = found->second.struct_def,
            .label = LHS->u.sval,
            .boundary = ContractBoundary::Global,
            .position = 1,
            .is_const = found->second.is_const,
            .initialising = found->second.is_const
         };
         // Declared global contracts must execute even when no predicate is required.  Concrete contracts and the
         // explicit 'any' opt-out are both attached to the environment for separately compiled chunks.
         bcemit_value_contract(fs, RHS, contract, true);
      }
      else if (GCstr *contract = lj_tab_get_global_contract(tabref(fs->L->env), LHS->u.sval)) {
         BCREG source = expr_toanyreg(fs, RHS);
         ExpDesc descriptor(contract);
         bcemit_AD(fs, BC_CONTRACT, source, const_str(fs, &descriptor));
         RHS->k = ExpKind::NonReloc;
         RHS->u.s.info = source;
      }
      BCREG ra = expr_toanyreg(fs, RHS);
      ins = BCINS_AD(BC_GSET, ra, const_str(fs, LHS));
      if (found != fs->ls->global_type_hints.end() and found->second.is_const) {
         global_const_finaliser = {
            .type = found->second.primary,
            .struct_def = found->second.struct_def,
            .label = LHS->u.sval,
            .boundary = ContractBoundary::Global,
            .position = 1,
            .is_const = true
         };
         global_const_base = ra;
         finalise_global_const = true;
      }
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
      BCREG idx = (BCREG)~rc;
      if (idx > BCMAX_C) {
         err_limit(fs, BCMAX_C + 1, "object field string constants");
         return;
      }
      ins = BCINS_ABCP(BC_OBSETF, ra, LHS->u.s.info, idx, 0xFFFFFFFFu);
   }
   else if (LHS->k IS ExpKind::IndexedStruct) {
      // Struct field assignment - emit BC_STSETF for a string key.
      BCREG ra = expr_toanyreg(fs, RHS);
      BCREG rc = LHS->u.s.aux;
      fs_check_assert(fs, int32_t(rc) < 0, "struct field index must be string constant");
      BCREG idx = (BCREG)~rc;
      if (idx > BCMAX_C) {
         err_limit(fs, BCMAX_C + 1, "struct field string constants");
         return;
      }
      ins = BCINS_ABCP(BC_STSETF, ra, LHS->u.s.info, idx, LHS->struct_field_index);
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
         bcemit_tsets(fs, ra, LHS->u.s.info, BCREG(stridx));
         expr_free(fs, RHS);
         return;
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
   if (finalise_global_const) {
      bcemit_contract(fs, global_const_base, std::span(&global_const_finaliser, 1), 1);
   }
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
   if (idx > BCMAX_D) {
      err_limit(fs, BCMAX_D + 1, "constants");
      return;
   }
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
      kt::Log("Parser").warning("Register leak at %s: %d temporary registers not released (total locals=%d, free reg=%d)",
         Context, int(freereg - total_locals), int(total_locals), int(freereg));
   }
}

void RegisterAllocator::trace_allocation(BCReg Start, BCReg Count, const char* Context) const
{
   if ((this->func_state->L->script->JitOptions & JOF::TRACE_REGISTERS) != JOF::NIL) {
      kt::Log("Parser").msg("Regalloc: reserve R%d..R%d (%d slots) at %s",
         int(Start.raw()), int(Start.raw() + Count.raw() - 1), int(Count.raw()), Context);
   }
}

void RegisterAllocator::trace_release(BCReg Start, BCReg Count, const char* Context) const
{
   if ((this->func_state->L->script->JitOptions & JOF::TRACE_REGISTERS) != JOF::NIL) {
      kt::Log("Parser").msg("Regalloc: release R%d..R%d (%d slots) at %s",
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
