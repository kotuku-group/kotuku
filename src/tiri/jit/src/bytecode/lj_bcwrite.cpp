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

// Context for bytecode writer.
typedef struct BCWriteCtx {
   SBuf sb;            // Output buffer.
   GCproto* pt;        // Root prototype.
   lua_Writer wfunc;   // Writer callback.
   void* wdata;        // Writer callback data.
   int strip;          // Strip debug info.
   int status;         // Status from writer callback.
#ifdef LUA_USE_ASSERT
   global_State* g;
#endif
} BCWriteCtx;

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

   // Write number of array slots and hash slots.

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
         need = 1 + 2 * 5;
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

   return p;
}

//********************************************************************************************************************
// Write prototype.

static void bcwrite_proto(BCWriteCtx *ctx, GCproto *pt)
{
   MSize sizedbg = 0;
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
   p = lj_buf_need(&ctx->sb, 5 + 4 + 6 * 5 + (pt->sizebc - 1) * (MSize)sizeof(BCIns) + pt->sizeuv * 2);
   p += 5;  //  Leave room for final size.

   // Write prototype header.
   *p++ = (pt->flags & (PROTO_CHILD | PROTO_VARARG | PROTO_FFI));
   *p++ = pt->numparams;
   *p++ = pt->framesize;
   *p++ = pt->sizeuv;
   p = lj_strfmt_wuleb128(p, pt->sizekgc);
   p = lj_strfmt_wuleb128(p, pt->sizekn);
   p = lj_strfmt_wuleb128(p, pt->sizebc - 1);
   if (!ctx->strip) {
      if (proto_lineinfo(pt)) sizedbg = pt->sizept - (MSize)((char*)proto_lineinfo(pt) - (char*)pt);
      p = lj_strfmt_wuleb128(p, sizedbg);
      if (sizedbg) {
         p = lj_strfmt_wuleb128(p, pt->firstline.lineNumber());  // Write decoded line for bytecode portability
         p = lj_strfmt_wuleb128(p, pt->numline);
      }
   }

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
      p = lj_buf_wmem(p, proto_lineinfo(pt), sizedbg);
      ctx->sb.w = p;
   }

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
#ifdef LUA_USE_ASSERT
   ctx.g = G(L);
#endif
   lj_buf_init(L, &ctx.sb);
   status = lj_vm_cpcall(L, nullptr, &ctx, cpwriter);
   if (status IS 0) status = ctx.status;
   lj_buf_free(G(sbufL(&ctx.sb)), &ctx.sb);
   return status;
}
