// Bytecode writer.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_bcwrite_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_buf.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_jit.h"
#include "lj_strfmt.h"
#include "lj_bcdump.h"
#include "lj_vm.h"
#include "../debug/filesource.h"

// Context for bytecode writer.
typedef struct BCWriteCtx {
   SBuf sb;            // Output buffer.
   GCproto* pt;        // Root prototype.
   lua_Writer wfunc;   // Writer callback.
   void* wdata;        // Writer callback data.
   int strip;          // Strip debug info.
   int status;         // Status from writer callback.
   uint8_t source_wire[256];
   uint8_t source_mapped[256];
   const CompilationSourceMap *sources;
   const uint8_t *struct_manifest;
   uint32_t struct_manifest_size;
#ifdef LUA_USE_ASSERT
   global_State* g;
#endif
} BCWriteCtx;

static MSize bcwrite_uleb128_size(uint32_t);

static bool bcwrite_source_id(BCWriteCtx *Ctx, uint8_t Runtime, uint8_t &Wire)
{
   if (Ctx->source_mapped[Runtime]) {
      Wire = Ctx->source_wire[Runtime];
      return true;
   }
   if (Runtime IS FILESOURCE_OVERFLOW_INDEX) {
      Wire = FILESOURCE_OVERFLOW_INDEX;
      return true;
   }
   return false;
}

static bool bcwrite_validate_proto(BCWriteCtx *Ctx, GCproto *Proto)
{
   uint8_t wire;
   if (not bcwrite_source_id(Ctx, Proto->file_source_idx, wire)) return false;
   if (proto_lineinfo(Proto)) {
      const BCLine *lines = (const BCLine *)proto_lineinfo(Proto);
      for (MSize i = 0; i + 1 < Proto->sizebc; ++i) {
         if (not bcwrite_source_id(Ctx, lines[i].fileIndex(), wire)) return false;
      }
   }
   if (Proto->flags & PROTO_CHILD) {
      GCRef *constant = mref<GCRef>(Proto->k) - 1;
      for (MSize i = 0; i < Proto->sizekgc; ++i, --constant) {
         GCobj *object = gcref(*constant);
         if (object->gch.gct IS ~LJ_TPROTO and not bcwrite_validate_proto(Ctx, gco_to_proto(object))) return false;
      }
   }
   return true;
}

static MSize bcwrite_source_string_size(GCstr *String)
{
   return bcwrite_uleb128_size(String->len) + String->len;
}

static char * bcwrite_source_string(char *Buffer, GCstr *String)
{
   Buffer = lj_strfmt_wuleb128(Buffer, String->len);
   return lj_buf_wmem(Buffer, strdata(String), String->len);
}

static void bcwrite_sources(BCWriteCtx *Ctx)
{
   lj_buf_reset(&Ctx->sb);
   auto entries = compilation_source_entries(Ctx->sources);
   MSize block_size = 3;
   for (uint32_t i = 0; i < Ctx->sources->count; ++i) {
      block_size += 4 + bcwrite_uleb128_size(entries[i].first_line.lineNumber()) +
         bcwrite_uleb128_size(entries[i].total_lines.lineNumber()) +
         bcwrite_uleb128_size(entries[i].import_line.lineNumber()) +
         bcwrite_source_string_size(gco_to_string(gcref(entries[i].canonical_path))) +
         bcwrite_source_string_size(gco_to_string(gcref(entries[i].display_filename))) +
         bcwrite_source_string_size(gco_to_string(gcref(entries[i].declared_namespace)));
   }
   char *p = lj_buf_need(&Ctx->sb, 5 + block_size);
   p = lj_strfmt_wuleb128(p, block_size);
   *p++ = Ctx->sources->version;
   *p++ = Ctx->sources->count;
   *p++ = Ctx->sources->root;
   for (uint32_t i = 0; i < Ctx->sources->count; ++i) {
      *p++ = uint8_t(entries[i].role);
      *p++ = entries[i].parent;
      *p++ = 0;
      *p++ = 0;
      p = lj_strfmt_wuleb128(p, entries[i].first_line.lineNumber());
      p = lj_strfmt_wuleb128(p, entries[i].total_lines.lineNumber());
      p = lj_strfmt_wuleb128(p, entries[i].import_line.lineNumber());
      p = bcwrite_source_string(p, gco_to_string(gcref(entries[i].canonical_path)));
      p = bcwrite_source_string(p, gco_to_string(gcref(entries[i].display_filename)));
      p = bcwrite_source_string(p, gco_to_string(gcref(entries[i].declared_namespace)));
   }
   Ctx->sb.w = p;
   Ctx->status = Ctx->wfunc(sbufL(&Ctx->sb), Ctx->sb.b, MSize(p - Ctx->sb.b), Ctx->wdata);
   lj_buf_reset(&Ctx->sb);
}

static void bcwrite_structs(BCWriteCtx *Ctx)
{
   if (Ctx->status != 0) return;
   lj_buf_reset(&Ctx->sb);
   char *p = lj_buf_need(&Ctx->sb, 5 + Ctx->struct_manifest_size);
   p = lj_strfmt_wuleb128(p, Ctx->struct_manifest_size);
   p = lj_buf_wmem(p, Ctx->struct_manifest, Ctx->struct_manifest_size);
   Ctx->status = Ctx->wfunc(sbufL(&Ctx->sb), Ctx->sb.b, MSize(p - Ctx->sb.b), Ctx->wdata);
   lj_buf_reset(&Ctx->sb);
}

#ifdef LUA_USE_ASSERT
#define lj_assertBCW(c, ...)   lj_assertG_(ctx->g, (c), __VA_ARGS__)
#else
#define lj_assertBCW(c, ...)   ((void)ctx)
#endif

//********************************************************************************************************************
// Bytecode writer

// Write a single constant key/value of a template table.

static void bcwrite_ktabk(BCWriteCtx* ctx, cTValue* o, int narrow)
{
   char* p = lj_buf_more(&ctx->sb, 1 + 10);
   if (tvisstr(o)) {
      const GCstr* str = strV(o);
      MSize len = str->len;
      p = lj_buf_more(&ctx->sb, 5 + len);
      p = lj_strfmt_wuleb128(p, BCDUMP_KTAB_STR + len);
      p = lj_buf_wmem(p, strdata(str), len);
   }
   else if (tvisint(o)) {
      *p++ = BCDUMP_KTAB_INT;
      p = lj_strfmt_wuleb128(p, intV(o));
   }
   else if (tvisnum(o)) {
      if (!LJ_DUALNUM and narrow) {  // Narrow number constants to integers.
         lua_Number num = numV(o);
         int32_t k = lj_num2int(num);
         if (num IS (lua_Number)k) {  // -0 is never a constant.
            *p++ = BCDUMP_KTAB_INT;
            p = lj_strfmt_wuleb128(p, k);
            ctx->sb.w = p;
            return;
         }
      }
      *p++ = BCDUMP_KTAB_NUM;
      p = lj_strfmt_wuleb128(p, o->u32.lo);
      p = lj_strfmt_wuleb128(p, o->u32.hi);
   }
   else {
      lj_assertBCW(tvispri(o), "unhandled type %d", itype(o));
      *p++ = BCDUMP_KTAB_NIL + ~itype(o);
   }
   ctx->sb.w = p;
}

//********************************************************************************************************************
// Write a template table.

static void bcwrite_ktab(BCWriteCtx* ctx, char* p, const GCtab* t)
{
   MSize narray = 0, nhash = 0;
   if (t->asize > 0) {  // Determine max. length of array part.
      ptrdiff_t i;
      TValue* array = tvref(t->array);
      for (i = (ptrdiff_t)t->asize - 1; i >= 0; i--)
         if (!tvisnil(&array[i])) break;
      narray = (MSize)(i + 1);
   }

   if (t->hmask > 0) {  // Count number of used hash slots.
      MSize i, hmask = t->hmask;
      Node* node = noderef(t->node);
      for (i = 0; i <= hmask; i++)
         nhash += !tvisnil(&node[i].val);
   }

   // Write classification flags, number of array slots and number of hash slots.

   p = lj_strfmt_wuleb128(p, t->flags & TAB_NOT_SEQUENCE);
   p = lj_strfmt_wuleb128(p, narray);
   p = lj_strfmt_wuleb128(p, nhash);
   ctx->sb.w = p;
   if (narray) {  // Write array entries (may contain nil).
      MSize i;
      TValue* o = tvref(t->array);
      for (i = 0; i < narray; i++, o++)
         bcwrite_ktabk(ctx, o, 1);
   }

   if (nhash) {  // Write hash entries.
      MSize i = nhash;
      Node* node = noderef(t->node) + t->hmask;
      for (;; node--)
         if (!tvisnil(&node->val)) {
            bcwrite_ktabk(ctx, &node->key, 0);
            bcwrite_ktabk(ctx, &node->val, 1);
            if (--i IS 0) break;
         }
   }
}

//********************************************************************************************************************
// Write GC constants of a prototype.

static void bcwrite_kgc(BCWriteCtx *ctx, GCproto *pt)
{
   MSize i, sizekgc = pt->sizekgc;
   GCRef *kr = mref<GCRef>(pt->k) - (ptrdiff_t)sizekgc;
   for (i = 0; i < sizekgc; i++, kr++) {
      GCobj *o = gcref(*kr);
      MSize tp, need = 1;
      char *p;
      // Determine constant type and needed size.
      if (o->gch.gct IS ~LJ_TSTR) {
         tp = BCDUMP_KGC_STR + gco_to_string(o)->len;
         need = 5 + gco_to_string(o)->len;
      }
      else if (o->gch.gct IS ~LJ_TPROTO) {
         lj_assertBCW((pt->flags & PROTO_CHILD), "prototype has unexpected child");
         tp = BCDUMP_KGC_CHILD;
      }
      else {
         lj_assertBCW(o->gch.gct IS ~LJ_TTAB, "bad constant GC type %d", o->gch.gct);
         tp = BCDUMP_KGC_TAB;
         need = 1 + 3 * 5;
      }

      // Write constant type.
      p = lj_buf_more(&ctx->sb, need);
      p = lj_strfmt_wuleb128(p, tp);

      // Write constant data (if any).
      if (tp >= BCDUMP_KGC_STR) p = lj_buf_wmem(p, strdata(gco_to_string(o)), gco_to_string(o)->len);
      else if (tp IS BCDUMP_KGC_TAB) {
         bcwrite_ktab(ctx, p, gco_to_table(o));
         continue;
      }
      ctx->sb.w = p;
   }
}

//********************************************************************************************************************
// Write number constants of a prototype.

static void bcwrite_knum(BCWriteCtx* ctx, GCproto* pt)
{
   MSize i, sizekn = pt->sizekn;
   cTValue *o = mref<TValue>(pt->k);
   char *p = lj_buf_more(&ctx->sb, 10 * sizekn);
   for (i = 0; i < sizekn; i++, o++) {
      int32_t k;
      lua_Number num;
      if (tvisint(o)) {
         k = intV(o);
         goto save_int;
      }
      else {
         // Write a 33 bit ULEB128 for the int (lsb=0) or loword (lsb=1).
         if (!LJ_DUALNUM) {  // Narrow number constants to integers.
            num = numV(o);
            k = lj_num2int(num);
            if (num IS (lua_Number)k) {  // -0 is never a constant.
            save_int:
               p = lj_strfmt_wuleb128(p, 2 * (uint32_t)k | ((uint32_t)k & 0x80000000u));
               if (k < 0) p[-1] = (p[-1] & 7) | ((k >> 27) & 0x18);
               continue;
            }
         }

         p = lj_strfmt_wuleb128(p, 1 + (2 * o->u32.lo | (o->u32.lo & 0x80000000u)));
         if (o->u32.lo >= 0x80000000u) p[-1] = (p[-1] & 7) | ((o->u32.lo >> 27) & 0x18);
         p = lj_strfmt_wuleb128(p, o->u32.hi);
      }
   }
   ctx->sb.w = p;
}

//********************************************************************************************************************
// Write bytecode instructions.

static char * bcwrite_bytecode(BCWriteCtx *ctx, char *p, GCproto *pt)
{
   MSize nbc = pt->sizebc - 1;  //  Omit the [JI]FUNC* header.
   char *q = p;  // Buffer position may not be 64-bit aligned
   char *bytecode = q;
   p = lj_buf_wmem(p, proto_bc(pt) + 1, nbc * (MSize)sizeof(BCIns));

   // Unpatch modified bytecode containing ILOOP/JLOOP etc.
   // Uses memcpy for portable access since buffer may not be 64-bit aligned.

   if ((pt->flags & PROTO_ILOOP) or pt->trace) {
      jit_State *J = L2J(sbufL(&ctx->sb));
      MSize i;
      for (i = 0; i < nbc; i++, q += sizeof(BCIns)) {
         BCIns ins;
         memcpy(&ins, q, sizeof(BCIns));
         BCOp op = bc_op(ins);
         if (op IS BC_IFORL or op IS BC_IITERL or op IS BC_ILOOP or op IS BC_JFORI) {
            setbc_op(&ins, (uint8_t)(op - BC_IFORL + BC_FORL));
            memcpy(q, &ins, sizeof(BCIns));
         }
         else if (op IS BC_JFORL or op IS BC_JITERL or op IS BC_JLOOP) {
            BCREG rd = bc_d(ins);
            ins = traceref(J, rd)->startins;  // Copy full 64-bit instruction
            memcpy(q, &ins, sizeof(BCIns));
         }
      }
   }

   // Struct field indices are layout-derived runtime hints.  Keep only the canonical unresolved sentinel on wire;
   // the consumer resolves and caches the field against its reconstructed declaration on first access.  Do this after
   // restoring trace-patched instructions because a trace's original instruction still carries the producer's hint.
   for (MSize i = 0; i < nbc; ++i) {
      BCIns instruction;
      memcpy(&instruction, bytecode + i * sizeof(BCIns), sizeof(instruction));
      BCOp op = bc_op(instruction);
      if (op IS BC_STGETF or op IS BC_STSETF) {
         setbc_p32(&instruction, 0xffffffffu);
         memcpy(bytecode + i * sizeof(BCIns), &instruction, sizeof(instruction));
      }
   }

   return p;
}

//********************************************************************************************************************
// Calculate and write the portable prototype signature.

static MSize bcwrite_uleb128_size(uint32_t Value)
{
   MSize size = 1;
   while (Value >= 0x80) {
      Value >>= 7;
      size++;
   }
   return size;
}

static MSize bcwrite_signature_size(const GCproto *Proto)
{
   const auto signature = proto_signature(Proto);
   if (not signature) return 0;

   MSize size = 2 + bcwrite_uleb128_size(signature->parameter_count) +
      bcwrite_uleb128_size(signature->result_count) + bcwrite_uleb128_size(signature->result_entry_count);
   auto entries = proto_parameter_types(Proto);
   MSize entry_count = MSize(signature->parameter_count) + signature->result_entry_count;
   for (MSize i = 0; i < entry_count; ++i) {
      size += 2 + bcwrite_uleb128_size(entries[i].constraint) +
         bcwrite_uleb128_size(proto_array_member_encoded(entries[i]));
   }
   return size;
}

static char * bcwrite_signature(char *Buffer, const GCproto *Proto)
{
   const auto signature = proto_signature(Proto);
   if (not signature) return Buffer;

   *Buffer++ = signature->version;
   *Buffer++ = signature->flags;
   Buffer = lj_strfmt_wuleb128(Buffer, signature->parameter_count);
   Buffer = lj_strfmt_wuleb128(Buffer, signature->result_count);
   Buffer = lj_strfmt_wuleb128(Buffer, signature->result_entry_count);

   auto entries = proto_parameter_types(Proto);
   MSize entry_count = MSize(signature->parameter_count) + signature->result_entry_count;
   for (MSize i = 0; i < entry_count; ++i) {
      *Buffer++ = uint8_t(entries[i].type);
      *Buffer++ = entries[i].flags;
      Buffer = lj_strfmt_wuleb128(Buffer, entries[i].constraint);
      Buffer = lj_strfmt_wuleb128(Buffer, proto_array_member_encoded(entries[i]));
   }
   return Buffer;
}

//********************************************************************************************************************
// Calculate and write the portable module dependency descriptors.
//
// Only canonical names are written.  The reader resolves them through the global module registry, so a chunk written
// by one process resolves correctly in another even if the module's export list has been reordered.

static MSize bcwrite_dependency_size(const GCproto *Proto)
{
   const auto table = proto_dependencies(Proto);
   if (not table) return 0;

   MSize size = 1 + bcwrite_uleb128_size(table->dependency_count) + bcwrite_uleb128_size(table->function_count);

   auto dependencies = proto_dependency_list(table);
   for (MSize i = 0; i < table->dependency_count; ++i) {
      GCstr *name = gco_to_string(gcref(dependencies[i].name));
      size += bcwrite_uleb128_size(name->len) + name->len +
         bcwrite_uleb128_size(dependencies[i].first_function) + bcwrite_uleb128_size(dependencies[i].function_count);
   }

   auto functions = proto_dependency_functions(table);
   for (MSize i = 0; i < table->function_count; ++i) {
      GCstr *name = gco_to_string(gcref(functions[i].name));
      size += bcwrite_uleb128_size(name->len) + name->len + bcwrite_uleb128_size(functions[i].module);
   }

   return size;
}

static char * bcwrite_dependencies(char *Buffer, const GCproto *Proto)
{
   const auto table = proto_dependencies(Proto);
   if (not table) return Buffer;

   *Buffer++ = table->version;
   Buffer = lj_strfmt_wuleb128(Buffer, table->dependency_count);
   Buffer = lj_strfmt_wuleb128(Buffer, table->function_count);

   auto dependencies = proto_dependency_list(table);
   for (MSize i = 0; i < table->dependency_count; ++i) {
      GCstr *name = gco_to_string(gcref(dependencies[i].name));
      Buffer = lj_strfmt_wuleb128(Buffer, name->len);
      Buffer = lj_buf_wmem(Buffer, strdata(name), name->len);
      Buffer = lj_strfmt_wuleb128(Buffer, dependencies[i].first_function);
      Buffer = lj_strfmt_wuleb128(Buffer, dependencies[i].function_count);
   }

   auto functions = proto_dependency_functions(table);
   for (MSize i = 0; i < table->function_count; ++i) {
      GCstr *name = gco_to_string(gcref(functions[i].name));
      Buffer = lj_strfmt_wuleb128(Buffer, name->len);
      Buffer = lj_buf_wmem(Buffer, strdata(name), name->len);
      Buffer = lj_strfmt_wuleb128(Buffer, functions[i].module);
   }

   return Buffer;
}

//********************************************************************************************************************
// Write prototype.

static void bcwrite_proto(BCWriteCtx *ctx, GCproto *pt)
{
   MSize sizedbg = 0;
   MSize sizesig = bcwrite_signature_size(pt);
   MSize sizedep = bcwrite_dependency_size(pt);
   char *p;

   // Recursively write children of prototype.
   if ((pt->flags & PROTO_CHILD)) {
      ptrdiff_t i, n = pt->sizekgc;
      GCRef* kr = mref<GCRef>(pt->k) - 1;
      for (i = 0; i < n; i++, kr--) {
         GCobj* o = gcref(*kr);
         if (o->gch.gct IS ~LJ_TPROTO) bcwrite_proto(ctx, gco_to_proto(o));
      }
   }

   // Start writing the prototype info to a buffer.
   p = lj_buf_need(&ctx->sb, 5 + 5 + 8 * 5 + sizesig + sizedep +
      (pt->sizebc - 1) * (MSize)sizeof(BCIns) + pt->sizeuv * 2);
   p += 5;  //  Leave room for final size.

   // Write prototype header.
   *p++ = (pt->flags & (PROTO_CHILD | PROTO_VARARG | PROTO_FFI));
   *p++ = pt->numparams;
   *p++ = pt->framesize;
   *p++ = pt->sizeuv;
   uint8_t prototype_source = 0;
   lj_assertBCW(bcwrite_source_id(ctx, pt->file_source_idx, prototype_source), "unmapped prototype source");
   *p++ = prototype_source;
   p = lj_strfmt_wuleb128(p, pt->sizekgc);
   p = lj_strfmt_wuleb128(p, pt->sizekn);
   p = lj_strfmt_wuleb128(p, pt->sizebc - 1);
   p = lj_strfmt_wuleb128(p, sizesig);
   p = lj_strfmt_wuleb128(p, sizedep);
   if (!ctx->strip) {
      if (proto_lineinfo(pt)) sizedbg = pt->sizept - (MSize)((char*)proto_lineinfo(pt) - (char*)pt);
      p = lj_strfmt_wuleb128(p, sizedbg);
      if (sizedbg) {
         p = lj_strfmt_wuleb128(p, pt->firstline.lineNumber());  // Write decoded line for bytecode portability
         p = lj_strfmt_wuleb128(p, pt->numline);
      }
   }

   p = bcwrite_signature(p, pt);
   p = bcwrite_dependencies(p, pt);

   // Write bytecode instructions and upvalue refs.
   p = bcwrite_bytecode(ctx, p, pt);
   p = lj_buf_wmem(p, proto_uv(pt), pt->sizeuv * 2);
   ctx->sb.w = p;

   // Write constants.
   bcwrite_kgc(ctx, pt);
   bcwrite_knum(ctx, pt);

   // Write debug info, if not stripped.
   // Note: lineinfo is a BCLine[sizebc-1] array (32-bit per instruction) with file index in upper 8 bits.
   // The sizedbg is calculated from memory layout and includes lineinfo, uvinfo, and varinfo.

   if (sizedbg) {
      p = lj_buf_more(&ctx->sb, sizedbg);
      const MSize line_size = (pt->sizebc - 1) * MSize(sizeof(BCLine));
      const BCLine *lines = (const BCLine *)proto_lineinfo(pt);
      for (MSize i = 0; i + 1 < pt->sizebc; ++i) {
         uint8_t source = 0;
         lj_assertBCW(bcwrite_source_id(ctx, lines[i].fileIndex(), source), "unmapped line source");
         BCLine remapped = BCLine::encode(source, lines[i].lineNumber());
         p = lj_buf_wmem(p, &remapped, sizeof(remapped));
      }
      p = lj_buf_wmem(p, (char *)proto_lineinfo(pt) + line_size, sizedbg - line_size);
      ctx->sb.w = p;
   }

   // Exception descriptors are semantic metadata and survive stripped dumps.
   p = lj_buf_more(&ctx->sb, 10 + MSize(pt->try_block_count) * 25 + MSize(pt->try_handler_count) * 30);
   p = lj_strfmt_wuleb128(p, pt->try_block_count);
   p = lj_strfmt_wuleb128(p, pt->try_handler_count);
   for (uint16_t i = 0; i < pt->try_block_count; ++i) {
      const TryBlockDesc &block = pt->try_blocks[i];
      p = lj_strfmt_wuleb128(p, block.first_handler);
      p = lj_strfmt_wuleb128(p, block.handler_count);
      p = lj_strfmt_wuleb128(p, block.entry_slots);
      p = lj_strfmt_wuleb128(p, block.flags);
   }
   for (uint16_t i = 0; i < pt->try_handler_count; ++i) {
      const TryHandlerDesc &handler = pt->try_handlers[i];
      p = lj_strfmt_wuleb128(p, uint32_t(handler.filter_packed));
      p = lj_strfmt_wuleb128(p, uint32_t(handler.filter_packed >> 32));
      p = lj_strfmt_wuleb128(p, handler.handler_pc);
      p = lj_strfmt_wuleb128(p, handler.exception_reg);
   }
   ctx->sb.w = p;

   // Pass buffer to writer function.
   if (ctx->status IS 0) {
      MSize n = sbuflen(&ctx->sb) - 5;
      MSize nn = (lj_fls(n) + 8) * 9 >> 6;
      char* q = ctx->sb.b + (5 - nn);
      p = lj_strfmt_wuleb128(q, n);  //  Fill in final size.
      lj_assertBCW(p IS ctx->sb.b + 5, "bad ULEB128 write");
      ctx->status = ctx->wfunc(sbufL(&ctx->sb), q, nn + n, ctx->wdata);
   }
}

//********************************************************************************************************************
// Write header of bytecode dump.

static void bcwrite_header(BCWriteCtx* ctx)
{
   GCstr* chunk_name = proto_chunk_name(ctx->pt);
   const char* name = strdata(chunk_name);
   MSize len = chunk_name->len;
   char* p = lj_buf_need(&ctx->sb, 5 + 5 + len);
   *p++ = BCDUMP_HEAD1;
   *p++ = BCDUMP_HEAD2;
   *p++ = BCDUMP_HEAD3;
   *p++ = BCDUMP_VERSION;
   *p++ = (ctx->strip ? BCDUMP_F_STRIP : 0) +
      LJ_BE * BCDUMP_F_BE +
      ((ctx->pt->flags & PROTO_FFI) ? BCDUMP_F_FFI : 0) +
      LJ_FR2 * BCDUMP_F_FR2;

   if (!ctx->strip) {
      p = lj_strfmt_wuleb128(p, len);
      p = lj_buf_wmem(p, name, len);
   }
   ctx->status = ctx->wfunc(sbufL(&ctx->sb), ctx->sb.b,
      (MSize)(p - ctx->sb.b), ctx->wdata);
   if (ctx->status IS 0) bcwrite_sources(ctx);
   if (ctx->status IS 0) bcwrite_structs(ctx);
}

//********************************************************************************************************************
// Write footer of bytecode dump.

static void bcwrite_footer(BCWriteCtx* ctx)
{
   if (ctx->status IS 0) {
      uint8_t zero = 0;
      ctx->status = ctx->wfunc(sbufL(&ctx->sb), &zero, 1, ctx->wdata);
   }
}

//********************************************************************************************************************
// Protected callback for bytecode writer.

static TValue* cpwriter(lua_State* L, lua_CFunction dummy, void* ud)
{
   BCWriteCtx* ctx = (BCWriteCtx*)ud;

   (void)lj_buf_need(&ctx->sb, 1024);  //  Avoids resize for most prototypes.
   bcwrite_header(ctx);
   bcwrite_proto(ctx, ctx->pt);
   bcwrite_footer(ctx);
   return nullptr;
}

//********************************************************************************************************************
// Write bytecode for a prototype.

int lj_bcwrite(lua_State *L, GCproto *pt, lua_Writer writer, void *data, int strip)
{
   BCWriteCtx ctx;
   int status;
   ctx.pt = pt;
   ctx.wfunc = writer;
   ctx.wdata = data;
   ctx.strip = strip;
   ctx.status = 0;
   memset(ctx.source_wire, 0, sizeof(ctx.source_wire));
   memset(ctx.source_mapped, 0, sizeof(ctx.source_mapped));
   ctx.sources = proto_compilation_sources(pt);
   ctx.struct_manifest = proto_struct_manifest(pt, &ctx.struct_manifest_size);
   if (not ctx.sources or ctx.sources->version != COMPILATION_SOURCE_VERSION or ctx.sources->count IS 0 or
       ctx.sources->count > FILESOURCE_MAX_COUNT or ctx.sources->root >= ctx.sources->count) return 1;
   if (not ctx.struct_manifest or ctx.struct_manifest_size < 2 or
       ctx.struct_manifest[0] != STRUCT_MANIFEST_VERSION) return 1;
   auto entries = compilation_source_entries(ctx.sources);
   for (uint32_t i = 0; i < ctx.sources->count; ++i) {
      GCstr *path = gco_to_string(gcref(entries[i].canonical_path));
      GCstr *filename = gco_to_string(gcref(entries[i].display_filename));
      if (filename->len IS 0 or entries[i].first_line.lineNumber() IS 0 or
          entries[i].total_lines.lineNumber() IS 0) return 1;
      if (i IS ctx.sources->root) {
         if ((entries[i].role != CompilationSourceRole::Main and
              entries[i].role != CompilationSourceRole::Synthetic) or
             entries[i].parent != FILESOURCE_OVERFLOW_INDEX or entries[i].import_line.lineNumber() != 0 or
             ((entries[i].role IS CompilationSourceRole::Synthetic) != (path->len IS 0))) return 1;
      }
      else if (entries[i].role != CompilationSourceRole::Import or entries[i].parent >= i or path->len IS 0 or
               entries[i].import_line.lineNumber() IS 0 or entries[i].import_line.lineNumber() >
               entries[entries[i].parent].total_lines.lineNumber()) return 1;
      for (uint32_t prior = 0; prior < i; ++prior) {
         GCstr *prior_path = gco_to_string(gcref(entries[prior].canonical_path));
         if (path->len and path->len IS prior_path->len and
             memcmp(strdata(path), strdata(prior_path), path->len) IS 0) return 1;
      }
      const uint8_t runtime = entries[i].runtime_index;
      if (runtime IS FILESOURCE_OVERFLOW_INDEX) continue;
      if (ctx.source_mapped[runtime]) return 1;
      ctx.source_mapped[runtime] = 1;
      ctx.source_wire[runtime] = uint8_t(i);
   }
   if (not bcwrite_validate_proto(&ctx, pt)) return 1;
#ifdef LUA_USE_ASSERT
   ctx.g = G(L);
#endif
   lj_buf_init(L, &ctx.sb);
   status = lj_vm_cpcall(L, nullptr, &ctx, cpwriter);
   if (status IS 0) status = ctx.status;
   lj_buf_free(G(sbufL(&ctx.sb)), &ctx.sb);
   return status;
}
