// Copyright © 2025-2026 Paul Manias
// IR emitter implementation: table and range expression emission
// #included from ir_emitter.cpp

//********************************************************************************************************************
// Emit bytecode for a table constructor expression ({key=value, [expr]=value, value}), optimising constant fields.

ParserResult<ExpDesc> IrEmitter::emit_table_expr(const TableExprPayload &Payload)
{
   FuncState* fs = &this->func_state;
   GCtab* template_table = nullptr;
   int vcall = 0;
   int needarr = 0;
   int fixt = 0;
   uint32_t narr = 0;  // 0-based array indexing
   uint32_t nhash = 0;
   auto freg = fs->free_reg();
   BCPos pc = BCPos(bcemit_AD(fs, BC_TNEW, freg, 0));
   ExpDesc table;
   table.init(ExpKind::NonReloc, freg);
   RegisterAllocator allocator(fs);
   allocator.reserve(BCReg(1));
   freg++;

   for (const TableField &field : Payload.fields) {
      if (not field.value) return this->unsupported_expr(AstNodeKind::TableExpr, field.span);
      RegisterGuard entry_guard(fs);
      ExpDesc key;
      vcall = 0;

      switch (field.kind) {
         case TableFieldKind::Computed: {
            if (not field.key) return this->unsupported_expr(AstNodeKind::TableExpr, field.span);
            auto key_result = this->emit_expression(*field.key);
            if (not key_result.ok()) return key_result;
            key = key_result.value_ref();
            ExpressionValue key_toval(fs, key);
            key_toval.to_val();
            key = key_toval.legacy();
            if (not key.is_constant()) expr_index(fs, &table, &key);
            if (key.is_num_constant() and key.is_num_zero()) needarr = 1;
            else nhash++;
            break;
         }

         case TableFieldKind::Record: {
            if (not field.name.has_value() or field.name->symbol IS nullptr) {
               return this->unsupported_expr(AstNodeKind::TableExpr, field.span);
            }
            key.init(ExpKind::Str, 0);
            key.u.sval = field.name->symbol;
            nhash++;
            break;
         }

         case TableFieldKind::Array:
         default: {
            key.init(ExpKind::Num, 0);
            setintV(&key.u.nval, int(narr));
            narr++;
            needarr = vcall = 1;
            break;
         }
      }

      auto value_result = this->emit_expression(*field.value);
      if (not value_result.ok()) return value_result;

      ExpDesc val = value_result.value_ref();

      bool emit_constant = key.is_constant() and key.k != ExpKind::Nil and (key.k IS ExpKind::Str or val.is_constant_nojump());

      if (emit_constant) {
         TValue k;
         TValue *slot;
         if (not template_table) {
            BCReg kidx;
            template_table = lj_tab_new(fs->L, needarr ? narr : 0, hsize2hbits(nhash));
            kidx = BCReg(const_gc(fs, obj2gco(template_table), LJ_TTAB));
            fs->bcbase[pc.raw()].ins = BCINS_AD(BC_TDUP, freg - BCREG(1), kidx);
         }

         vcall = 0;
         expr_kvalue(fs, &k, &key);
         slot = lj_tab_set(fs->L, template_table, &k);
         lj_gc_anybarriert(fs->L, template_table);
         if (val.is_constant_nojump()) {
            expr_kvalue(fs, slot, &val);
            continue;
         }
         settabV(fs->L, slot, template_table);
         fixt = 1;
         emit_constant = false;
      }

      if (not emit_constant) {
         if (val.k != ExpKind::Call) {
            RegisterAllocator val_allocator(fs);
            ExpressionValue val_value(fs, val);
            val_value.discharge_to_any_reg(val_allocator);
            val = val_value.legacy();
            vcall = 0;
         }
         if (key.is_constant()) expr_index(fs, &table, &key);
         bcemit_store(fs, &table, &val);
      }
   }

   if (vcall) {
      BCInsLine* ilp = &fs->last_instruction();
      ExpDesc en(ExpKind::Num);
      en.u.nval.u32.lo = narr - 1;
      en.u.nval.u32.hi = 0x43300000;
      if (narr > 256) {
         fs->pc--;
         ilp--;
      }
      ilp->ins = BCINS_AD(BC_TSETM, freg, const_num(fs, &en));
      setbc_b(&ilp[-1].ins, 0);
   }

   if (pc IS fs->pc - 1) {
      table.u.s.info = pc;
      fs->freereg--;
      table.k = ExpKind::Relocable;
   }
   else table.k = ExpKind::NonReloc;

   if (not template_table) {
      BCIns *ip = &fs->bcbase[pc].ins;
      if (not needarr) narr = 0;
      else if (narr < 3) narr = 3;
      else if (narr > 0x7ff) narr = 0x7ff;
      setbc_d(ip, narr | (hsize2hbits(nhash) << 11));
   }
   else {
      if (needarr and template_table->asize < narr) {
         lj_tab_reasize(fs->L, template_table, narr - 1);
      }

      if (fixt) {
         Node *node = noderef(template_table->node);
         uint32_t hmask = template_table->hmask;
         for (uint32_t i = 0; i <= hmask; ++i) {
            Node *n = &node[i];
            if (tvistab(&n->val)) setnilV(&n->val);
         }
      }
      lj_gc_check(fs->L);
   }

   return ParserResult<ExpDesc>::success(table);
}
