// Bytecode reader.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_bcread_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_bc.h"
#include "../parser/lexer.h"
#include "lj_bcdump.h"
#include "lj_state.h"
#include "lj_strfmt.h"

// Reuse some lexer fields for our own purposes.

#define bcread_flags(State)    State->level
#define bcread_swap(State)     ((bcread_flags(State) & BCDUMP_F_BE) != LJ_BE*BCDUMP_F_BE)
#define bcread_oldtop(L, ls)   restorestack(L, State->lastline)
#define bcread_savetop(L, ls, top) State->lastline = (BCLine)savestack(L, (top))

// Input buffer handling

//********************************************************************************************************************
// Throw reader error.

static LJ_NOINLINE void bcread_error(LexState *State, ErrMsg em)
{
   lua_State* L = State->L;
   const char* name = State->chunk_arg;
   if (*name == BCDUMP_HEAD1) name = "(binary)";
   else if (*name == '@' or *name == '=') name++;
   lj_strfmt_pushf(L, "%s: %s", name, err2msg(em));
   lj_err_throw(L, LUA_ERRSYNTAX);
}

//********************************************************************************************************************
// Refill buffer.

static LJ_NOINLINE void bcread_fill(LexState *State, MSize len, int need)
{
   State->assert_condition(len != 0, "empty refill");
   if (len > LJ_MAX_BUF or State->c < 0) bcread_error(State, ErrMsg::BCBAD);

   do {
      const char* buf;
      size_t sz;
      char* p = State->sb.b;
      MSize n = (MSize)(State->pe - State->p);
      if (n) {  // Copy remainder to buffer.
         if (sbuflen(&State->sb)) {  // Move down in buffer.
            State->assert_condition(State->pe == State->sb.w, "bad buffer pointer");
            if (State->p != p) memmove(p, State->p, n);
         }
         else {  // Copy from buffer provided by reader.
            p = lj_buf_need(&State->sb, len);
            memcpy(p, State->p, n);
         }
         State->p = p;
         State->pe = p + n;
      }
      State->sb.w = p + n;
      buf = State->rfunc(State->L, State->rdata, &sz);  //  Get more data from reader.
      if (buf == nullptr or sz == 0) {  // EOF?
         if (need) bcread_error(State, ErrMsg::BCBAD);
         State->c = -1;  //  Only bad if we get called again.
         break;
      }
      if (sz >= LJ_MAX_BUF - n) lj_err_mem(State->L);
      if (n) {  // Append to buffer.
         n += (MSize)sz;
         p = lj_buf_need(&State->sb, n < len ? len : n);
         memcpy(State->sb.w, buf, sz);
         State->sb.w = p + n;
         State->p = p;
         State->pe = p + n;
      }
      else {  // Return buffer provided by reader.
         State->p = buf;
         State->pe = buf + sz;
      }
   } while ((MSize)(State->pe - State->p) < len);
}

//********************************************************************************************************************
// Need a certain number of bytes.

static LJ_AINLINE void bcread_need(LexState *State, MSize len)
{
   if (LJ_UNLIKELY((MSize)(State->pe - State->p) < len))
      bcread_fill(State, len, 1);
}

//********************************************************************************************************************
// Want to read up to a certain number of bytes, but may need less.

static LJ_AINLINE void bcread_want(LexState *State, MSize len)
{
   if (LJ_UNLIKELY((MSize)(State->pe - State->p) < len))
      bcread_fill(State, len, 0);
}

//********************************************************************************************************************
// Return memory block from buffer.

static LJ_AINLINE uint8_t* bcread_mem(LexState *State, MSize len)
{
   uint8_t* p = (uint8_t*)State->p;
   State->p += len;
   State->assert_condition(State->p <= State->pe, "buffer read overflow");
   return p;
}

//********************************************************************************************************************
// Copy memory block from buffer.

static void bcread_block(LexState *State, void* q, MSize len)
{
   memcpy(q, bcread_mem(State, len), len);
}

//********************************************************************************************************************
// Read byte from buffer.

static LJ_AINLINE uint32_t bcread_byte(LexState *State)
{
   State->assert_condition(State->p < State->pe, "buffer read overflow");
   return (uint32_t)(uint8_t)*State->p++;
}

//********************************************************************************************************************
// Read ULEB128 value from buffer.

static LJ_AINLINE uint32_t bcread_uleb128(LexState *State)
{
   uint32_t v = lj_buf_ruleb128(&State->p);
   State->assert_condition(State->p <= State->pe, "buffer read overflow");
   return v;
}

//********************************************************************************************************************
// Read top 32 bits of 33 bit ULEB128 value from buffer.

static uint32_t bcread_uleb128_33(LexState *State)
{
   const uint8_t* p = (const uint8_t*)State->p;
   uint32_t v = (*p++ >> 1);
   if (LJ_UNLIKELY(v >= 0x40)) {
      int sh = -1;
      v &= 0x3f;
      do {
         v |= ((*p & 0x7f) << (sh += 7));
      } while (*p++ >= 0x80);
   }
   State->p = (char*)p;
   State->assert_condition(State->p <= State->pe, "buffer read overflow");
   return v;
}

//********************************************************************************************************************
// Read debug info of a prototype.
// lineinfo is now a BCLine[sizebc-1] array (32-bit per instruction) with file index in upper 8 bits.

static void bcread_dbg(LexState *State, GCproto *pt, MSize sizedbg)
{
   void* lineinfo = (void*)proto_lineinfo(pt);
   bcread_block(State, lineinfo, sizedbg);
   // Swap BCLine values if the endianness differs (always 32-bit)
   if (bcread_swap(State)) {
      MSize i, n = pt->sizebc - 1;
      BCLine* p = (BCLine*)lineinfo;
      for (i = 0; i < n; i++) p[i] = BCLine(lj_bswap(p[i].raw()));
   }
}

//********************************************************************************************************************
// Find pointer to varinfo.

static const void* bcread_varinfo(GCproto *pt)
{
   const uint8_t* p = proto_uvinfo(pt);
   MSize n = pt->sizeuv;
   if (n) while (*p++ or --n);
   return p;
}

//********************************************************************************************************************
// Read a single constant key/value of a template table.

static void bcread_ktabk(LexState *State, TValue* o)
{
   MSize tp = bcread_uleb128(State);
   if (tp >= BCDUMP_KTAB_STR) {
      MSize len = tp - BCDUMP_KTAB_STR;
      const char* p = (const char*)bcread_mem(State, len);
      setstrV(State->L, o, lj_str_new(State->L, p, len));
   }
   else if (tp == BCDUMP_KTAB_INT) {
      setintV(o, (int32_t)bcread_uleb128(State));
   }
   else if (tp == BCDUMP_KTAB_NUM) {
      o->u32.lo = bcread_uleb128(State);
      o->u32.hi = bcread_uleb128(State);
   }
   else {
      State->assert_condition(tp <= BCDUMP_KTAB_TRUE, "bad constant type %d", tp);
      setpriV(o, ~uint64_t(tp));
   }
}

//********************************************************************************************************************
// Read a template table.

static GCtab* bcread_ktab(LexState *State)
{
   MSize narray = bcread_uleb128(State);
   MSize nhash = bcread_uleb128(State);
   GCtab* t = lj_tab_new(State->L, narray, hsize2hbits(nhash));
   if (narray) {  // Read array entries.
      MSize i;
      TValue* o = tvref(t->array);
      for (i = 0; i < narray; i++, o++)
         bcread_ktabk(State, o);
   }
   if (nhash) {  // Read hash entries.
      MSize i;
      for (i = 0; i < nhash; i++) {
         TValue key;
         bcread_ktabk(State, &key);
         State->assert_condition(!tvisnil(&key), "nil key");
         bcread_ktabk(State, lj_tab_set(State->L, t, &key));
      }
   }
   return t;
}

//********************************************************************************************************************
// Read GC constants of a prototype.

static void bcread_kgc(LexState *State, GCproto *pt, MSize sizekgc)
{
   MSize i;
   GCRef* kr = mref<GCRef>(pt->k) - (ptrdiff_t)sizekgc;
   for (i = 0; i < sizekgc; i++, kr++) {
      MSize tp = bcread_uleb128(State);
      if (tp >= BCDUMP_KGC_STR) {
         MSize len = tp - BCDUMP_KGC_STR;
         const char* p = (const char*)bcread_mem(State, len);
         setgcref(*kr, obj2gco(lj_str_new(State->L, p, len)));
      }
      else if (tp == BCDUMP_KGC_TAB) {
         setgcref(*kr, obj2gco(bcread_ktab(State)));
      }
      else {
         lua_State* L = State->L;
         State->assert_condition(tp == BCDUMP_KGC_CHILD, "bad constant type %d", tp);
         if (L->top <= bcread_oldtop(L, ls))  //  Stack underflow?
            bcread_error(State, ErrMsg::BCBAD);
         L->top--;
         setgcref(*kr, obj2gco(protoV(L->top)));
      }
   }
}

//********************************************************************************************************************
// Read number constants of a prototype.

static void bcread_knum(LexState *State, GCproto *pt, MSize sizekn)
{
   MSize i;
   TValue* o = mref<TValue>(pt->k);
   for (i = 0; i < sizekn; i++, o++) {
      int isnum = (State->p[0] & 1);
      uint32_t lo = bcread_uleb128_33(State);
      if (isnum) {
         o->u32.lo = lo;
         o->u32.hi = bcread_uleb128(State);
      }
      else setintV(o, lo);
   }
}

//********************************************************************************************************************
// Read bytecode instructions.

static void bcread_bytecode(LexState *State, GCproto *pt, MSize sizebc)
{
   BCIns* bc = proto_bc(pt);
   bc[0] = BCINS_AD((pt->flags & PROTO_VARARG) ? BC_FUNCV : BC_FUNCF,
      pt->framesize, 0);
   bcread_block(State, bc + 1, (sizebc - 1) * (MSize)sizeof(BCIns));
   // Swap bytecode instructions if the endianness differs (64-bit BCIns).
   if (bcread_swap(State)) {
      MSize i;
      for (i = 1; i < sizebc; i++) bc[i] = lj_bswap64(bc[i]);
   }
}

//********************************************************************************************************************
// Read upvalue refs.

static void bcread_uv(LexState *State, GCproto *pt, MSize sizeuv)
{
   if (sizeuv) {
      uint16_t* uv = proto_uv(pt);
      bcread_block(State, uv, sizeuv * 2);
      // Swap upvalue refs if the endianess differs.
      if (bcread_swap(State)) {
         MSize i;
         for (i = 0; i < sizeuv; i++)
            uv[i] = (uint16_t)((uv[i] >> 8) | (uv[i] << 8));
      }
   }
}

//********************************************************************************************************************
// Read a prototype.

GCproto *lj_bcread_proto(LexState *State)
{
   GCproto *pt;
   MSize framesize, numparams, flags, sizeuv, sizekgc, sizekn, sizebc, sizept;
   MSize ofsk, ofsuv, ofsdbg;
   MSize sizedbg = 0;
   BCLine firstline = 0, numline = 0;

   // Read prototype header.
   flags     = bcread_byte(State);
   numparams = bcread_byte(State);
   framesize = bcread_byte(State);
   sizeuv    = bcread_byte(State);
   sizekgc   = bcread_uleb128(State);
   sizekn    = bcread_uleb128(State);
   sizebc    = bcread_uleb128(State) + 1;
   if (!(bcread_flags(State) & BCDUMP_F_STRIP)) {
      sizedbg = bcread_uleb128(State);
      if (sizedbg) {
         firstline = bcread_uleb128(State);
         numline = bcread_uleb128(State);
      }
   }

   // Calculate total size of prototype including all colocated arrays.

   sizept = (MSize)sizeof(GCproto) + sizebc * (MSize)sizeof(BCIns) + sizekgc * (MSize)sizeof(GCRef);
   sizept = (sizept + (MSize)sizeof(TValue) - 1) & ~((MSize)sizeof(TValue) - 1);
   ofsk   = sizept; sizept += sizekn * (MSize)sizeof(TValue);
   ofsuv  = sizept; sizept += ((sizeuv + 1) & ~1) * 2;
   ofsdbg = sizept; sizept += sizedbg;

   // Allocate prototype object and initialize its fields.

   pt = (GCproto*)lj_mem_newgco(State->L, (MSize)sizept);
   pt->gct = ~LJ_TPROTO;
   pt->numparams = (uint8_t)numparams;
   pt->framesize = (uint8_t)framesize;
   pt->sizebc = sizebc;
   setmref(pt->k, (char*)pt + ofsk);
   setmref(pt->uv, (char*)pt + ofsuv);
   pt->sizekgc = 0;  //  Set to zero until fully initialized.
   pt->sizekn = sizekn;
   pt->sizept = sizept;
   pt->sizeuv = (uint8_t)sizeuv;
   pt->flags = (uint8_t)flags;
   pt->trace = 0;
   setgcref(pt->chunk_name, obj2gco(State->chunk_name));

   // Close potentially uninitialized gap between bc and kgc.

   *(uint32_t*)((char*)pt + ofsk - sizeof(GCRef) * (sizekgc + 1)) = 0;

   // Read bytecode instructions and upvalue refs.

   bcread_bytecode(State, pt, sizebc);
   bcread_uv(State, pt, sizeuv);

   // Read constants.

   bcread_kgc(State, pt, sizekgc);
   pt->sizekgc = sizekgc;
   bcread_knum(State, pt, sizekn);

   // Read and initialize debug info.

   pt->firstline = firstline;
   pt->numline = numline;
   if (sizedbg) {
      // lineinfo is now a fixed-size BCLine[sizebc-1] array (32-bit per instruction)
      MSize sizeli = (sizebc - 1) * sizeof(BCLine);
      setmref(pt->lineinfo, (char*)pt + ofsdbg);
      setmref(pt->uvinfo, (char*)pt + ofsdbg + sizeli);
      bcread_dbg(State, pt, sizedbg);
      setmref(pt->varinfo, bcread_varinfo(pt));
   }
   else {
      setmref(pt->lineinfo, nullptr);
      setmref(pt->uvinfo, nullptr);
      setmref(pt->varinfo, nullptr);
   }
   return pt;
}

//********************************************************************************************************************
// Read and check header of bytecode dump.

static int bcread_header(LexState *State)
{
   uint32_t flags;
   bcread_want(State, 3 + 5 + 5);
   if (bcread_byte(State) != BCDUMP_HEAD2 or bcread_byte(State) != BCDUMP_HEAD3 or bcread_byte(State) != BCDUMP_VERSION) return 0;
   bcread_flags(State) = flags = bcread_uleb128(State);
   if ((flags & ~(BCDUMP_F_KNOWN)) != 0) return 0;
   if ((flags & BCDUMP_F_FR2) != LJ_FR2 * BCDUMP_F_FR2) return 0;
   if ((flags & BCDUMP_F_FFI)) return 0;

   if ((flags & BCDUMP_F_STRIP)) {
      State->chunk_name = lj_str_newz(State->L, State->chunk_arg);
   }
   else {
      MSize len = bcread_uleb128(State);
      bcread_need(State, len);
      State->chunk_name = lj_str_new(State->L, (const char*)bcread_mem(State, len), len);
   }
   return 1;  //  Ok.
}

//********************************************************************************************************************
// Read a bytecode dump.

GCproto *lj_bcread(LexState *State)
{
   lua_State* L = State->L;
   State->assert_condition(State->c == BCDUMP_HEAD1, "bad bytecode header");
   bcread_savetop(L, ls, L->top);
   lj_buf_reset(&State->sb);

   // Check for a valid bytecode dump header.
   if (!bcread_header(State)) bcread_error(State, ErrMsg::BCFMT);

   while (true) {  // Process all prototypes in the bytecode dump.
      GCproto *pt;
      MSize len;
      const char* startp;
      // Read length.
      if (State->p < State->pe and State->p[0] == 0) {  // Shortcut EOF.
         State->p++;
         break;
      }
      bcread_want(State, 5);
      len = bcread_uleb128(State);
      if (!len) break;  //  EOF
      bcread_need(State, len);
      startp = State->p;
      pt = lj_bcread_proto(State);
      if (State->p != startp + len)
         bcread_error(State, ErrMsg::BCBAD);
      setprotoV(L, L->top, pt);
      incr_top(L);
   }

   if ((State->pe != State->p and !State->endmark) or L->top - 1 != bcread_oldtop(L, ls))
      bcread_error(State, ErrMsg::BCBAD);

   // Pop off last prototype.
   L->top--;
   return protoV(L->top);
}
