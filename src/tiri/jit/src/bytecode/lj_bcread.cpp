// Bytecode reader.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_bcread_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_ff.h"
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
#include "lj_meta.h"
#include "lj_contract.h"
#include "../debug/lj_debug.h"
#include "../../../defs.h"

#include <limits>
#include <vector>

// Reuse some lexer fields for our own purposes.

#define bcread_flags(State)    State->level
#define bcread_swap(State)     ((bcread_flags(State) & BCDUMP_F_BE) != LJ_BE*BCDUMP_F_BE)
#define bcread_oldtop(L, ls)   restorestack(L, State->lastline)
#define bcread_savetop(L, ls, top) State->lastline = (BCLine)savestack(L, (top))

// Reader limits are intentionally lower than the allocator's architectural limits. A bytecode file is a transport
// format, not a way to request multi-gigabyte single objects or unbounded validation work.
static constexpr MSize BCREAD_MAX_PROTO_SIZE = 64u * 1024u * 1024u;
static constexpr uint64_t BCREAD_MAX_TOTAL_ALLOCATION = 256u * 1024u * 1024u;
static constexpr uint64_t BCREAD_MAX_VALIDATION_WORK = 16u * 1024u * 1024u;
static constexpr uint32_t BCREAD_MAX_PROTOTYPES = 65535;
static constexpr uint16_t BCREAD_MAX_DEPTH = LJ_MAX_XLEVEL;

static LJ_NOINLINE void bcread_error(LexState *State, ErrMsg em);

static MSize bcread_checked_add(LexState *State, MSize Left, MSize Right)
{
   if (Right > (std::numeric_limits<MSize>::max)() - Left) bcread_error(State, ErrMsg::BCBAD);
   return Left + Right;
}

static MSize bcread_checked_multiply(LexState *State, MSize Left, MSize Right)
{
   if (Left and Right > (std::numeric_limits<MSize>::max)() / Left) bcread_error(State, ErrMsg::BCBAD);
   return Left * Right;
}

static MSize bcread_checked_align(LexState *State, MSize Value, MSize Alignment)
{
   MSize mask = Alignment - 1;
   return bcread_checked_add(State, Value, mask) & ~mask;
}

static void bcread_account(LexState *State, uint64_t Amount)
{
   if (Amount > BCREAD_MAX_VALIDATION_WORK - State->bytecode_validation_work) {
      bcread_error(State, ErrMsg::BCBAD);
   }
   State->bytecode_validation_work += Amount;
}

static void bcread_reserve_allocation(LexState *State, uint64_t Amount)
{
   if (Amount > BCREAD_MAX_TOTAL_ALLOCATION - State->bytecode_allocation) {
      bcread_error(State, ErrMsg::BCBAD);
   }
   State->bytecode_allocation += Amount;
}

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
   if (not State->rfunc) { // A string-view load already supplied the entire file.
      if (need) bcread_error(State, ErrMsg::BCBAD);
      State->c = -1;
      return;
   }

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
   bcread_need(State, len);
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
   bcread_need(State, 1);
   return uint32_t(uint8_t(*State->p++));
}

//********************************************************************************************************************
// Read ULEB128 value from buffer.

static LJ_AINLINE uint32_t bcread_uleb128(LexState *State)
{
   uint32_t value = 0;
   for (unsigned shift = 0; shift <= 28; shift += 7) {
      const uint32_t byte = bcread_byte(State);
      if ((shift IS 28) and (byte > 0x0f)) bcread_error(State, ErrMsg::BCBAD);
      value |= (byte & 0x7f) << shift;
      if (not (byte & 0x80)) return value;
   }
   bcread_error(State, ErrMsg::BCBAD);
   return 0;
}

//********************************************************************************************************************
// Read top 32 bits of 33 bit ULEB128 value from buffer.

static uint32_t bcread_uleb128_33(LexState *State)
{
   const uint32_t first = bcread_byte(State);
   uint32_t value = (first >> 1) & 0x3f;
   if (not (first & 0x80)) return value;
   for (unsigned shift = 6; shift <= 27; shift += 7) {
      const uint32_t byte = bcread_byte(State);
      if ((shift IS 27) and (byte > 0x1f)) bcread_error(State, ErrMsg::BCBAD);
      value |= (byte & 0x7f) << shift;
      if (not (byte & 0x80)) return value;
   }
   bcread_error(State, ErrMsg::BCBAD);
   return 0;
}

//********************************************************************************************************************
// Read debug info of a prototype.
// lineinfo is now a BCLine[sizebc-1] array (32-bit per instruction) with file index in upper 8 bits.

static void bcread_dbg(LexState *State, GCproto *pt, MSize sizedbg)
{
   uint8_t* lineinfo = (uint8_t*)proto_lineinfo(pt);
   bcread_block(State, lineinfo, sizedbg);
   // Swap BCLine values if the endianness differs (always 32-bit)
   if (bcread_swap(State)) {
      MSize i, n = pt->sizebc - 1;
      BCLine* p = (BCLine*)lineinfo;
      for (i = 0; i < n; i++) p[i] = BCLine(lj_bswap(p[i].raw()));
   }

   MSize line_size = bcread_checked_multiply(State, pt->sizebc - 1, MSize(sizeof(BCLine)));
   if (line_size > sizedbg) bcread_error(State, ErrMsg::BCBAD);
   const uint8_t *cursor = lineinfo + line_size;
   const uint8_t *end = lineinfo + sizedbg;
   setmref(pt->uvinfo, cursor);

   // Upvalue names are a sequence of sizeuv terminated strings. Every scan remains inside the debug record.
   for (MSize i = 0; i < pt->sizeuv; ++i) {
      const void *terminator = memchr(cursor, 0, size_t(end - cursor));
      if (not terminator) bcread_error(State, ErrMsg::BCBAD);
      cursor = (const uint8_t *)terminator + 1;
   }
   setmref(pt->varinfo, cursor);

   auto read_uleb = [&]() -> uint32_t {
      uint32_t value = 0;
      for (uint32_t shift = 0; shift <= 28; shift += 7) {
         if (cursor >= end) bcread_error(State, ErrMsg::BCBAD);
         uint32_t byte = *cursor++;
         if ((shift IS 28) and byte > 0x0f) bcread_error(State, ErrMsg::BCBAD);
         value |= (byte & 0x7f) << shift;
         if (not (byte & 0x80)) return value;
      }
      bcread_error(State, ErrMsg::BCBAD);
      return 0;
   };

   uint32_t last_pc = 0;
   uint32_t variable_count = 0;
   while (cursor < end) {
      uint8_t name = *cursor++;
      if (name IS VARNAME_END) {
         if (cursor != end) bcread_error(State, ErrMsg::BCBAD);
         return;
      }
      if (name >= VARNAME__MAX) {
         const void *terminator = memchr(cursor, 0, size_t(end - cursor));
         if (not terminator) bcread_error(State, ErrMsg::BCBAD);
         cursor = (const uint8_t *)terminator + 1;
      }
      uint32_t start_delta = read_uleb();
      uint32_t extent = read_uleb();
      if (start_delta > pt->sizebc - last_pc) bcread_error(State, ErrMsg::BCBAD);
      last_pc += start_delta;
      if (extent > pt->sizebc - last_pc) bcread_error(State, ErrMsg::BCBAD);
      if (++variable_count > LJ_MAX_LOCVAR) bcread_error(State, ErrMsg::BCBAD);
   }
   bcread_error(State, ErrMsg::BCBAD); // Missing varinfo terminator.
}

//********************************************************************************************************************
// Read a single constant key/value of a template table.

static void bcread_ktabk(LexState *State, TValue* o)
{
   MSize tp = bcread_uleb128(State);
   if (tp >= BCDUMP_KTAB_STR) {
      MSize len = tp - BCDUMP_KTAB_STR;
      const char* p = (const char*)bcread_mem(State, len);
      bcread_reserve_allocation(State, uint64_t(len) + sizeof(GCstr));
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
      if (tp > BCDUMP_KTAB_TRUE) bcread_error(State, ErrMsg::BCBAD);
      setpriV(o, ~uint64_t(tp));
   }
}

//********************************************************************************************************************
// Read a template table.

static GCtab* bcread_ktab(LexState *State)
{
   MSize flags = bcread_uleb128(State);
   if (flags & ~MSize(TAB_NOT_SEQUENCE)) bcread_error(State, ErrMsg::BCBAD);
   MSize narray = bcread_uleb128(State);
   MSize nhash = bcread_uleb128(State);
   if (narray > LJ_MAX_ASIZE or nhash > (uint32_t(1) << LJ_MAX_HBITS)) bcread_error(State, ErrMsg::BCBAD);
   bcread_account(State, uint64_t(narray) + uint64_t(nhash) * 2);
   bcread_reserve_allocation(State, sizeof(GCtab) + uint64_t(narray) * sizeof(TValue) +
      uint64_t(nhash) * 2 * sizeof(Node));
   GCtab* t = lj_tab_new(State->L, narray, hsize2hbits(nhash));
   t->flags |= uint8_t(flags);
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
         if (tvisnil(&key)) bcread_error(State, ErrMsg::BCBAD);
         bcread_ktabk(State, lj_tab_set(State->L, t, &key));
      }
   }
   return t;
}

//********************************************************************************************************************
// Read GC constants of a prototype.

static uint16_t bcread_kgc(LexState *State, GCproto *pt, MSize sizekgc)
{
   MSize i;
   uint16_t depth = 1;
   MSize child_count = 0;
   GCRef* kr = mref<GCRef>(pt->k) - (ptrdiff_t)sizekgc;
   for (i = 0; i < sizekgc; i++, kr++) {
      MSize tp = bcread_uleb128(State);
      if (tp >= BCDUMP_KGC_STR) {
         MSize len = tp - BCDUMP_KGC_STR;
         const char* p = (const char*)bcread_mem(State, len);
         bcread_reserve_allocation(State, uint64_t(len) + sizeof(GCstr));
         setgcref(*kr, obj2gco(lj_str_new(State->L, p, len)));
      }
      else if (tp == BCDUMP_KGC_TAB) {
         setgcref(*kr, obj2gco(bcread_ktab(State)));
      }
      else {
         lua_State* L = State->L;
         if (tp != BCDUMP_KGC_CHILD or State->bytecode_prototype_depths.empty()) {
            bcread_error(State, ErrMsg::BCBAD);
         }
         if (L->top <= bcread_oldtop(L, ls))  //  Stack underflow?
            bcread_error(State, ErrMsg::BCBAD);
         uint16_t child_depth = State->bytecode_prototype_depths.back();
         State->bytecode_prototype_depths.pop_back();
         if (child_depth >= BCREAD_MAX_DEPTH) bcread_error(State, ErrMsg::BCBAD);
         depth = std::max<uint16_t>(depth, uint16_t(child_depth + 1));
         child_count++;
         L->top--;
         setgcref(*kr, obj2gco(protoV(L->top)));
      }
   }
   if ((child_count != 0) != bool(pt->flags & PROTO_CHILD)) bcread_error(State, ErrMsg::BCBAD);
   return depth;
}

//********************************************************************************************************************
// Read number constants of a prototype.

static void bcread_knum(LexState *State, GCproto *pt, MSize sizekn)
{
   MSize i;
   TValue* o = mref<TValue>(pt->k);
   for (i = 0; i < sizekn; i++, o++) {
      bcread_need(State, 1);
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
   if (sizebc < 2) bcread_error(State, ErrMsg::BCBAD);

   BCIns* bc = proto_bc(pt);
   BCREG context_entries[BCMAX_A + 1];
   MSize context_entry_depth = 0;
   uint64_t close_arm_slots = 0;
   uint64_t close_consume_slots = 0;
   std::vector<ProtoContextBlockDesc> context_blocks;
   bc[0] = BCINS_AD((pt->flags & PROTO_VARARG) ? BC_FUNCV : BC_FUNCF,
      pt->framesize, 0);
   bcread_block(State, bc + 1, (sizebc - 1) * (MSize)sizeof(BCIns));
   // Swap bytecode instructions if the endianness differs (64-bit BCIns).
   if (bcread_swap(State)) {
      MSize i;
      for (i = 1; i < sizebc; i++) bc[i] = lj_bswap64(bc[i]);
   }
   for (MSize i = 1; i < sizebc; ++i) {
      BCOp op = bc_op(bc[i]);
      if (op >= BC__MAX) bcread_error(State, ErrMsg::BCBAD);
      if (op IS BC_BFUNC) {
         BuiltinCallableID id = BuiltinCallableID(bc_d(bc[i]));
         if (not builtin_callable_valid(id) or not lj_builtin_callable(State->L, id)) {
            bcread_error(State, ErrMsg::BCBAD);
         }
      }
      if (op IS BC_BMETH) {
         ptrdiff_t target = ptrdiff_t(i) + 1 + bc_j(bc[i]);
         if (target <= 0 or target >= ptrdiff_t(sizebc)) bcread_error(State, ErrMsg::BCBAD);
      }
      if (op IS BC_TCTX) {
         // Contextual designation only ever applies to a table the compiler has just materialised in A.  Requiring the
         // preceding instruction to be the matching constructor rejects a marker retargeted at a foreign table and
         // makes a duplicated marker impossible, because the second copy no longer follows a constructor.
         BCREG slot = bc_a(bc[i]);
         if (slot >= pt->framesize) bcread_error(State, ErrMsg::BCBAD);
         if (i IS 1) bcread_error(State, ErrMsg::BCBAD);
         BCOp previous = bc_op(bc[i - 1]);
         if ((previous != BC_TNEW and previous != BC_TDUP) or bc_a(bc[i - 1]) != slot) {
            bcread_error(State, ErrMsg::BCBAD);
         }
      }
      else if (op IS BC_CLOSEARM or op IS BC_CLOSE) {
         BCREG slot = bc_a(bc[i]);
         if (slot >= pt->framesize or slot >= 64) bcread_error(State, ErrMsg::BCBAD);
         if (op IS BC_CLOSEARM) close_arm_slots |= uint64_t(1) << slot;
         else close_consume_slots |= uint64_t(1) << slot;
      }
      else if (op IS BC_DEFERARM) {
         BCREG callable_slot = bc_a(bc[i]);
         BCREG scope_base = bc_b(bc[i]);
         BCREG argument_count = bc_c(bc[i]);
         if (callable_slot >= pt->framesize or scope_base > callable_slot or
             callable_slot + argument_count >= pt->framesize) {
            bcread_error(State, ErrMsg::BCBAD);
         }
      }
      else if (op IS BC_DEFERCONSUME or op IS BC_RETHROW) {
         if (bc_a(bc[i]) >= pt->framesize) bcread_error(State, ErrMsg::BCBAD);
      }
      else if (op IS BC_VIEW) {
         if (bc_a(bc[i]) != 0 or bc_d(bc[i]) > 1) bcread_error(State, ErrMsg::BCBAD);
      }
      else if (op IS BC_CTXENTER or op IS BC_CTXCALL or op IS BC_CTXCALLM or op IS BC_CTXLEAVE or
               op IS BC_CTXCALLT) {
         BCREG call_base = bc_a(bc[i]);
         if (call_base IS 0 or call_base >= pt->framesize) bcread_error(State, ErrMsg::BCBAD);

         if (op IS BC_CTXENTER) {
            if (context_entry_depth >= BCMAX_A + 1) bcread_error(State, ErrMsg::BCBAD);
            context_entries[context_entry_depth++] = call_base;
         }
         else if (op IS BC_CTXCALL or op IS BC_CTXCALLM or op IS BC_CTXCALLT) {
            if (context_entry_depth IS 0 or context_entries[context_entry_depth - 1] != call_base) {
               bcread_error(State, ErrMsg::BCBAD);
            }
            --context_entry_depth;
         }

         if (op IS BC_CTXCALL or op IS BC_CTXCALLM) {
            if (i + 1 >= sizebc or bc_op(bc[i + 1]) != BC_CTXLEAVE or bc_a(bc[i + 1]) != call_base) {
               bcread_error(State, ErrMsg::BCBAD);
            }
         }
         else if (op IS BC_CTXLEAVE) {
            if (i IS 1 or (bc_op(bc[i - 1]) != BC_CTXCALL and bc_op(bc[i - 1]) != BC_CTXCALLM) or
                bc_a(bc[i - 1]) != call_base) {
               bcread_error(State, ErrMsg::BCBAD);
            }
            BCREG result_shift = bc_d(bc[i]);
            if (result_shift > call_base) bcread_error(State, ErrMsg::BCBAD);
         }
      }
      else if (op IS BC_CTXBEGIN) {
         uint16_t descriptor = uint16_t(bc_d(bc[i]));
         BCREG slot = bc_a(bc[i]);
         if (slot >= pt->framesize or context_blocks.size() >= UINT16_MAX or descriptor != context_blocks.size()) {
            bcread_error(State, ErrMsg::BCBAD);
         }
         context_blocks.push_back(ProtoContextBlockDesc{
            .begin_pc = i,
            .end_pc = 0,
            .entry_slots = BCREG(slot + 1)
         });
      }
      else if (op IS BC_CTXEND) {
         uint16_t descriptor = uint16_t(bc_d(bc[i]));
         if (descriptor >= context_blocks.size() or bc_a(bc[i]) != 0) bcread_error(State, ErrMsg::BCBAD);
         if (i > context_blocks[descriptor].end_pc) context_blocks[descriptor].end_pc = i;
      }
   }
   if (context_entry_depth != 0) bcread_error(State, ErrMsg::BCBAD);

   // Propagate the lexical block stack through every control-flow component.  A program point may only be reached with
   // one exact descriptor stack, which rejects jumps into a block and exits that bypass their matching CTXEND.
   // Unreachable components are seeded with their lexical descriptor stack because the emitter retains valid source
   // after a terminator.  Multiple CTXEND instructions for return/break/continue paths remain valid because only the
   // selected edge reaches each copy.
   std::vector<std::vector<uint16_t>> block_states(sizebc);
   std::vector<uint8_t> state_seen(sizebc, 0);
   std::vector<uint8_t> begin_seen(context_blocks.size(), 0);
   std::vector<uint8_t> end_seen(context_blocks.size(), 0);
   std::vector<MSize> worklist;
   state_seen[1] = 1;
   worklist.push_back(1);

   auto enqueue = [&](ptrdiff_t Target, const std::vector<uint16_t> &Stack) {
      if (Target <= 0 or Target >= ptrdiff_t(sizebc)) bcread_error(State, ErrMsg::BCBAD);
      MSize target = MSize(Target);
      if (not state_seen[target]) {
         state_seen[target] = 1;
         block_states[target] = Stack;
         worklist.push_back(target);
      }
      else if (block_states[target] != Stack) bcread_error(State, ErrMsg::BCBAD);
   };

   while (true) {
      while (not worklist.empty()) {
         MSize pc = worklist.back();
         worklist.pop_back();
         BCOp op = bc_op(bc[pc]);
         std::vector<uint16_t> stack = block_states[pc];

         if (op IS BC_CTXBEGIN) {
            uint16_t descriptor = uint16_t(bc_d(bc[pc]));
            stack.push_back(descriptor);
            begin_seen[descriptor] = 1;
         }
         else if (op IS BC_CTXEND) {
            uint16_t descriptor = uint16_t(bc_d(bc[pc]));
            if (stack.empty() or stack.back() != descriptor) bcread_error(State, ErrMsg::BCBAD);
            stack.pop_back();
            end_seen[descriptor] = 1;
         }

         if (op IS BC_RET or op IS BC_RET0 or op IS BC_RET1 or op IS BC_RETM or
             op IS BC_CALLT or op IS BC_CALLMT or op IS BC_CTXCALLT or op IS BC_RAISE) {
            if (not stack.empty()) bcread_error(State, ErrMsg::BCBAD);
            continue;
         }

         if (op IS BC_JMP) {
            enqueue(ptrdiff_t(pc) + 1 + bc_j(bc[pc]), stack);
         }
         else if (bcmode_d(op) IS BCMjump) {
            enqueue(ptrdiff_t(pc) + 1 + bc_j(bc[pc]), stack);
            if (pc + 1 < sizebc) enqueue(pc + 1, stack);
         }
         else if (op <= BC_ISFALSEY) {
            if (pc + 1 < sizebc) enqueue(pc + 1, stack);
            if (pc + 2 < sizebc) enqueue(pc + 2, stack);
         }
         else if (pc + 1 < sizebc) enqueue(pc + 1, stack);
      }

      MSize root = 1;
      while (root < sizebc and state_seen[root]) root++;
      if (root >= sizebc) break;

      std::vector<uint16_t> stack;
      for (uint16_t descriptor = 0; descriptor < context_blocks.size(); ++descriptor) {
         const ProtoContextBlockDesc &block = context_blocks[descriptor];
         if (block.begin_pc < root and root <= block.end_pc) stack.push_back(descriptor);
      }
      state_seen[root] = 1;
      block_states[root] = std::move(stack);
      worklist.push_back(root);
   }

   for (size_t i = 0; i < context_blocks.size(); ++i) {
      if (not begin_seen[i] or not end_seen[i] or context_blocks[i].end_pc <= context_blocks[i].begin_pc) {
         bcread_error(State, ErrMsg::BCBAD);
      }
   }

   if (close_consume_slots & ~close_arm_slots) bcread_error(State, ErrMsg::BCBAD);
   pt->closeslots = close_arm_slots;

   if (not context_blocks.empty()) {
      MSize byte_size = bcread_checked_multiply(State, MSize(context_blocks.size()),
         MSize(sizeof(ProtoContextBlockDesc)));
      bcread_account(State, context_blocks.size());
      bcread_reserve_allocation(State, byte_size);
      pt->context_blocks = (ProtoContextBlockDesc *)lj_mem_new(State->L, byte_size);
      memcpy(pt->context_blocks, context_blocks.data(), byte_size);
      pt->context_block_count = uint16_t(context_blocks.size());
   }
}

//********************************************************************************************************************

static void bcread_builtin_methods(LexState *State, GCproto *Prototype)
{
   for (MSize i = 1; i < Prototype->sizebc; ++i) {
      BCIns instruction = proto_bc(Prototype)[i];
      if (bc_op(instruction) != BC_BMETH) continue;
      uint32_t constant = bc_p32(instruction);
      if (constant >= Prototype->sizekgc or
          proto_kgc(Prototype, ~(ptrdiff_t)constant)->gch.gct != ~LJ_TSTR) {
         bcread_error(State, ErrMsg::BCBAD);
      }
   }
}

//********************************************************************************************************************
// Validate operands whose bounds or kinds depend on the completed constant arrays.

static void bcread_validate_bytecode(LexState *State, GCproto *Prototype)
{
   bool interpreter_required = false;
   auto validate_constant = [&](BCMode Mode, uint32_t Value) {
      if (Mode IS BCMnum) {
         if (Value >= Prototype->sizekn) bcread_error(State, ErrMsg::BCBAD);
         return;
      }
      if (Mode IS BCMstr or Mode IS BCMtab or Mode IS BCMfunc or Mode IS BCMcdata) {
         if (Mode IS BCMcdata) bcread_error(State, ErrMsg::BCBAD);
         if (Value >= Prototype->sizekgc) bcread_error(State, ErrMsg::BCBAD);
         GCobj *constant = proto_kgc(Prototype, ~(ptrdiff_t)Value);
         uint8_t expected = Mode IS BCMstr ? uint8_t(~LJ_TSTR) :
            Mode IS BCMtab ? uint8_t(~LJ_TTAB) : uint8_t(~LJ_TPROTO);
         if (constant->gch.gct != expected) bcread_error(State, ErrMsg::BCBAD);
      }
      else if (Mode IS BCMuv) {
         if (Value >= Prototype->sizeuv) bcread_error(State, ErrMsg::BCBAD);
      }
      else if (Mode IS BCMpri and Value > uint32_t(~LJ_TTRUE)) bcread_error(State, ErrMsg::BCBAD);
   };
   auto validate_mode = [&](BCMode Mode, uint32_t Value) {
      if (Mode IS BCMrbase) {
         if (Value > Prototype->framesize) bcread_error(State, ErrMsg::BCBAD);
      }
      else if (Mode IS BCMdst or Mode IS BCMbase or Mode IS BCMvar) {
         if (Value >= Prototype->framesize) bcread_error(State, ErrMsg::BCBAD);
      }
      else validate_constant(Mode, Value);
   };

   for (MSize pc = 1; pc < Prototype->sizebc; ++pc) {
      BCIns instruction = proto_bc(Prototype)[pc];
      BCOp op = bc_op(instruction);
      if (bc_is_func_header(op) or op IS BC_KCDATA or op IS BC_OCALL or op IS BC_JFORI or op IS BC_IFORL or
          op IS BC_JFORL or op IS BC_IITERL or op IS BC_JITERL or op IS BC_ILOOP or op IS BC_JLOOP) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      if (op != BC_BMETH and op != BC_STGETF and op != BC_STSETF and bc_p32(instruction) != 0) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      if ((op IS BC_STGETF or op IS BC_STSETF) and bc_p32(instruction) != 0xffffffffu) {
         bcread_error(State, ErrMsg::BCBAD);
      }

      validate_mode(bcmode_a(op), bc_a(instruction));
      if (bcmode_hasd(op)) {
         BCMode mode = bcmode_d(op);
         validate_mode(mode, bc_d(instruction));
         if (mode IS BCMjump) {
            ptrdiff_t target = ptrdiff_t(pc) + 1 + bc_j(instruction);
            if (target <= 0 or target >= ptrdiff_t(Prototype->sizebc)) bcread_error(State, ErrMsg::BCBAD);
         }
      }
      else {
         validate_mode(bcmode_b(op), bc_b(instruction));
         validate_mode(bcmode_c(op), bc_c(instruction));
      }

      if ((op IS BC_CAT and bc_b(instruction) > bc_c(instruction)) or
          (op IS BC_KNIL and bc_a(instruction) > bc_d(instruction))) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      if (op IS BC_CAT and (bc_b(instruction) >= Prototype->framesize or
          bc_c(instruction) >= Prototype->framesize)) bcread_error(State, ErrMsg::BCBAD);
      if ((op IS BC_CALL or op IS BC_CALLM or op IS BC_ITERC or op IS BC_ITERN or op IS BC_ITERA or
           op IS BC_CTXCALL or op IS BC_CTXCALLM) and bc_c(instruction) and
          uint32_t(bc_a(instruction)) + bc_c(instruction) > Prototype->framesize) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      if ((op IS BC_CALLT or op IS BC_CALLMT or op IS BC_CTXCALLT) and bc_d(instruction) and
          uint32_t(bc_a(instruction)) + bc_d(instruction) > Prototype->framesize) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      if ((op IS BC_RET or op IS BC_RET1) and bc_d(instruction) > 1 and
          uint32_t(bc_a(instruction)) + bc_d(instruction) - 1 > Prototype->framesize) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      if (bc_is_for_loop(op) and uint32_t(bc_a(instruction)) + FORL_EXT >= Prototype->framesize) {
         bcread_error(State, ErrMsg::BCBAD);
      }

      if (op IS BC_MRSAVE or op IS BC_MRRESTORE) interpreter_required = true;
      if (op IS BC_CONTRACT or op IS BC_TYPETEST) {
         GCobj *constant = proto_kgc(Prototype, ~(ptrdiff_t)bc_d(instruction));
         RuntimeContractDescriptor descriptor;
         if (not decode_runtime_contract(gco_to_string(constant), descriptor) or
             descriptor.contract_count IS 0 or
             uint32_t(bc_a(instruction)) + descriptor.static_value_count > Prototype->framesize) {
            bcread_error(State, ErrMsg::BCBAD);
         }
         if (op IS BC_TYPETEST) {
            if (descriptor.boundary != ContractBoundary::Local or descriptor.flags != 0 or
                descriptor.static_value_count != 1 or descriptor.contract_count != 1 or
                descriptor.entries[0].position != 1 or not descriptor.entries[0].label.empty()) {
               bcread_error(State, ErrMsg::BCBAD);
            }
         }
         else if (contract_requires_interpreter(descriptor)) interpreter_required = true;
      }
   }
   proto_set_interpreter_required(Prototype, interpreter_required);
}

static void bcread_validate_child_upvalues(LexState *State, GCproto *Prototype)
{
   for (ptrdiff_t i = -ptrdiff_t(Prototype->sizekgc); i < 0; ++i) {
      GCobj *constant = proto_kgc(Prototype, i);
      if (constant->gch.gct != uint8_t(~LJ_TPROTO)) continue;
      GCproto *child = gco_to_proto(constant);
      for (MSize uv_index = 0; uv_index < child->sizeuv; ++uv_index) {
         uint16_t capture = proto_uv(child)[uv_index];
         if (capture & 0x3f00) bcread_error(State, ErrMsg::BCBAD);
         uint32_t index = capture & 0xff;
         if ((capture & PROTO_UV_LOCAL) ? index >= Prototype->framesize : index >= Prototype->sizeuv) {
            bcread_error(State, ErrMsg::BCBAD);
         }
      }
   }
}

//********************************************************************************************************************
// Validate descriptor-indexed module activation after the dependency block has been installed.

static void bcread_module_activations(LexState *State, GCproto *Proto)
{
   auto table = proto_dependencies(Proto);
   uint8_t activations[PROTO_MAX_DEPENDENCIES] = {};

   for (MSize i = 1; i < Proto->sizebc; ++i) {
      BCIns instruction = proto_bc(Proto)[i];
      if (bc_op(instruction) != BC_MODACT) continue;

      uint32_t dependency = bc_d(instruction);
      if (not table or dependency >= table->dependency_count) bcread_error(State, ErrMsg::BCBAD);
      const ProtoDependency &descriptor = proto_dependency_list(table)[dependency];
      if (uint32_t(bc_a(instruction)) + descriptor.function_count > Proto->framesize or activations[dependency]) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      activations[dependency] = 1;
   }

   if (table) {
      for (uint32_t dependency = 0; dependency < table->dependency_count; ++dependency) {
         if (not activations[dependency]) bcread_error(State, ErrMsg::BCBAD);
      }
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
// Read and validate a portable prototype signature.

struct BCReadSignature {
   bool present = false;
   uint8_t flags = 0;
   uint8_t parameter_count = 0;
   uint8_t result_count = 0;
   uint8_t result_entry_count = 0;
   std::array<ProtoTypeEntry, 256> parameters{};
   std::array<ProtoTypeEntry, PROTO_MAX_RETURN_TYPES> results{};
};

static bool bcread_signature_uleb(const uint8_t *&Cursor, const uint8_t *End, uint32_t &Value)
{
   Value = 0;
   uint32_t shift = 0;
   for (uint32_t count = 0; count < 5 and Cursor < End; ++count) {
      uint8_t byte = *Cursor++;
      if (count IS 4 and (byte & 0xf0)) return false;
      Value |= uint32_t(byte & 0x7f) << shift;
      if (not (byte & 0x80)) return true;
      shift += 7;
   }
   return false;
}

static void bcread_signature_entry(LexState *State, const uint8_t *&Cursor, const uint8_t *End,
   ProtoTypeEntry &Entry)
{
   if (End - Cursor < 2) bcread_error(State, ErrMsg::BCBAD);
   uint8_t type = *Cursor++;
   uint8_t flags = *Cursor++;
   uint32_t constraint = 0;
   if (not bcread_signature_uleb(Cursor, End, constraint)) bcread_error(State, ErrMsg::BCBAD);
   uint32_t reserved = 0;
   if (not bcread_signature_uleb(Cursor, End, reserved)) bcread_error(State, ErrMsg::BCBAD);

   constexpr uint8_t known_flags = PROTO_TYPE_NULLABLE | PROTO_TYPE_REQUIRED | PROTO_TYPE_ORIGIN_MASK |
      PROTO_TYPE_STRENGTH_MASK;
   if (type > uint8_t(TiriType::Unknown) or (flags & ~known_flags) or
       uint8_t((flags & PROTO_TYPE_ORIGIN_MASK) >> PROTO_TYPE_ORIGIN_SHIFT) >
          uint8_t(ProtoTypeOrigin::Inferred) or
       uint8_t((flags & PROTO_TYPE_STRENGTH_MASK) >> PROTO_TYPE_STRENGTH_SHIFT) >
          uint8_t(ProtoTypeStrength::Trusted) or
       ((flags & PROTO_TYPE_NULLABLE) and (flags & PROTO_TYPE_REQUIRED))) {
      bcread_error(State, ErrMsg::BCBAD);
   }

   TiriType entry_type = TiriType(type);
   if (constraint and entry_type != TiriType::Struct and entry_type != TiriType::Object and
       entry_type != TiriType::Array) {
      bcread_error(State, ErrMsg::BCBAD);
   }
   if (entry_type IS TiriType::Array) {
      if (not reserved or reserved > uint32_t(AET::MAX)) bcread_error(State, ErrMsg::BCBAD);
      AET member = AET(reserved - 1);
      if ((member IS AET::STRUCT) != (constraint != 0)) bcread_error(State, ErrMsg::BCBAD);
   }
   else if (reserved) bcread_error(State, ErrMsg::BCBAD);

   Entry = ProtoTypeEntry{
      .constraint = constraint,
      .type = entry_type,
      .flags = flags
   };
   set_proto_array_member_encoded(Entry, uint16_t(reserved));
}

static void bcread_signature(LexState *State, MSize Size, MSize NumParams, BCReadSignature &Result)
{
   if (Size IS 0) {
      if (NumParams != 0) bcread_error(State, ErrMsg::BCBAD);
      return;
   }

   bcread_need(State, Size);
   const uint8_t *cursor = bcread_mem(State, Size);
   const uint8_t *end = cursor + Size;
   if (end - cursor < 2) bcread_error(State, ErrMsg::BCBAD);
   uint8_t version = *cursor++;
   if (version != PROTO_SIGNATURE_VERSION) bcread_error(State, ErrMsg::BCBAD);

   uint8_t flags = *cursor++;
   constexpr uint8_t known_flags = proto_signature_flag(ProtoSignatureFlag::ParameterVariadic) |
      proto_signature_flag(ProtoSignatureFlag::ResultVariadic) |
      proto_signature_flag(ProtoSignatureFlag::ExplicitResults) |
      proto_signature_flag(ProtoSignatureFlag::DynamicResults);
   if (flags & ~known_flags) bcread_error(State, ErrMsg::BCBAD);

   uint32_t parameter_count = 0;
   uint32_t result_count = 0;
   uint32_t result_entry_count = 0;
   if (not bcread_signature_uleb(cursor, end, parameter_count) or
       not bcread_signature_uleb(cursor, end, result_count) or
       not bcread_signature_uleb(cursor, end, result_entry_count) or
       parameter_count != NumParams or parameter_count > 255 or result_count > 255 or
       result_entry_count > PROTO_MAX_RETURN_TYPES) {
      bcread_error(State, ErrMsg::BCBAD);
   }

   const bool explicit_results = flags & proto_signature_flag(ProtoSignatureFlag::ExplicitResults);
   const bool dynamic_results = flags & proto_signature_flag(ProtoSignatureFlag::DynamicResults);
   const bool variadic_results = flags & proto_signature_flag(ProtoSignatureFlag::ResultVariadic);
   if ((explicit_results and dynamic_results) or (variadic_results and (not explicit_results or result_count IS 0)) or
       (dynamic_results and (result_count != 0 or result_entry_count != 0)) or
       (explicit_results and result_entry_count != std::min<uint32_t>(result_count, PROTO_MAX_RETURN_TYPES)) or
       (not explicit_results and not dynamic_results and
          result_entry_count != std::min<uint32_t>(result_count, PROTO_MAX_RETURN_TYPES))) {
      bcread_error(State, ErrMsg::BCBAD);
   }

   Result.present = true;
   Result.flags = flags;
   Result.parameter_count = uint8_t(parameter_count);
   Result.result_count = uint8_t(result_count);
   Result.result_entry_count = uint8_t(result_entry_count);
   for (uint32_t i = 0; i < parameter_count; ++i) {
      bcread_signature_entry(State, cursor, end, Result.parameters[i]);
   }
   for (uint32_t i = 0; i < result_entry_count; ++i) {
      bcread_signature_entry(State, cursor, end, Result.results[i]);
      if (not explicit_results and not dynamic_results and
          (proto_type_origin(Result.results[i]) != ProtoTypeOrigin::Inferred or
           proto_type_strength(Result.results[i]) != ProtoTypeStrength::Trusted)) {
         bcread_error(State, ErrMsg::BCBAD);
      }
   }
   if (cursor != end) bcread_error(State, ErrMsg::BCBAD);
}

static void bcread_install_signature(GCproto *Proto, void *Buffer, const BCReadSignature &Source)
{
   if (not Source.present) return;

   auto signature = (ProtoSignature *)Buffer;
   signature->version = PROTO_SIGNATURE_VERSION;
   signature->flags = Source.flags;
   signature->parameter_count = Source.parameter_count;
   signature->result_count = Source.result_count;
   signature->result_entry_count = Source.result_entry_count;
   memset(signature->reserved, 0, sizeof(signature->reserved));

   auto parameters = (ProtoTypeEntry *)(signature + 1);
   if (Source.parameter_count > 0) {
      memcpy(parameters, Source.parameters.data(), Source.parameter_count * sizeof(ProtoTypeEntry));
   }
   if (Source.result_entry_count > 0) {
      memcpy(parameters + Source.parameter_count, Source.results.data(),
         Source.result_entry_count * sizeof(ProtoTypeEntry));
   }
   setmref(Proto->signature, signature);
   Proto->signature_size = uint16_t(proto_signature_size(Source.parameter_count, Source.result_entry_count));
}

//********************************************************************************************************************
// Read and validate the portable module dependency descriptors.
//
// Names are validated and located here but deliberately neither interned nor resolved.  Interning is deferred to
// installation because a GCstr held only by this structure would be unreachable across the prototype allocation, and
// the reader's stack discipline reserves L->top for child prototypes.  Resolution is deferred to activation, so that
// a chunk still loads when a module is temporarily unavailable and the failure is reported at the documented point.
//
// Validation is structural only, and every count is bounded before any allocation depends on it.

struct BCReadDependencies {
   bool present = false;

   // A private copy of the dependency block.  The reader's buffer is refilled by later reads and the names must
   // outlive that, but they cannot be interned early either: an unanchored GCstr would not survive the prototype
   // allocation, and interning after the allocation would run the collector over a prototype whose constant array is
   // still uninitialised.  Copying the bytes decouples both concerns for a few hundred bytes at most.

   std::string storage;

   struct Entry {
      uint32_t name_offset = 0;  // Offset of the name within 'storage'
      uint32_t name_length = 0;
      uint32_t first = 0;        // Dependencies: first function index.  Functions: owning dependency index.
      uint32_t count = 0;        // Dependencies only
   };

   std::vector<Entry> dependencies;
   std::vector<Entry> functions;

   [[nodiscard]] std::string_view name_of(const Entry &Value) const {
      return std::string_view(this->storage).substr(Value.name_offset, Value.name_length);
   }
};

static void bcread_dependency_name(LexState *State, const uint8_t *&Cursor, const uint8_t *End,
   const uint8_t *Base, BCReadDependencies::Entry &Entry)
{
   uint32_t length = 0;
   if (not bcread_signature_uleb(Cursor, End, length)) bcread_error(State, ErrMsg::BCBAD);

   // A zero-length or oversized name cannot be a canonical module or function name, and would otherwise be carried
   // into the registry lookup as an unresolvable entry.

   if (length IS 0 or length > LJ_MAX_STR or uint32_t(End - Cursor) < length) bcread_error(State, ErrMsg::BCBAD);

   Entry.name_offset = uint32_t(Cursor - Base);
   Entry.name_length = length;
   Cursor += length;
}

static void bcread_dependencies(LexState *State, MSize Size, BCReadDependencies &Result)
{
   if (Size IS 0) return;

   bcread_need(State, Size);
   const uint8_t *base = bcread_mem(State, Size);
   const uint8_t *cursor = base;
   const uint8_t *end = base + Size;
   if (end - cursor < 1) bcread_error(State, ErrMsg::BCBAD);

   uint8_t version = *cursor++;
   if (version != PROTO_DEPENDENCY_VERSION) bcread_error(State, ErrMsg::BCBAD);

   uint32_t dependency_count = 0;
   uint32_t function_count = 0;
   if (not bcread_signature_uleb(cursor, end, dependency_count) or
       not bcread_signature_uleb(cursor, end, function_count) or
       dependency_count IS 0 or dependency_count > PROTO_MAX_DEPENDENCIES or
       function_count > PROTO_MAX_DEPENDENCY_FUNCTIONS) {
      bcread_error(State, ErrMsg::BCBAD);
   }

   Result.storage.assign((const char *)base, Size);
   Result.dependencies.reserve(dependency_count);
   Result.functions.reserve(function_count);

   uint32_t expected_first = 0;
   for (uint32_t i = 0; i < dependency_count; ++i) {
      BCReadDependencies::Entry entry;
      bcread_dependency_name(State, cursor, end, base, entry);
      if (not bcread_signature_uleb(cursor, end, entry.first) or
          not bcread_signature_uleb(cursor, end, entry.count)) {
         bcread_error(State, ErrMsg::BCBAD);
      }

      // The writer emits one contiguous, ordered run per dependency.  Requiring the same here means a slice can be
      // trusted without a further bounds check at activation, and rejects overlapping or out-of-range ranges.

      if (entry.first != expected_first or entry.count > function_count - entry.first) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      expected_first = entry.first + entry.count;

      // Aliases within one source unit are pooled.  Imported units retain isolated activation scopes inside the same
      // prototype, so two valid descriptors may carry the same canonical module name.

      Result.dependencies.push_back(entry);
   }

   if (expected_first != function_count) bcread_error(State, ErrMsg::BCBAD);

   for (uint32_t i = 0; i < function_count; ++i) {
      BCReadDependencies::Entry entry;
      bcread_dependency_name(State, cursor, end, base, entry);
      if (not bcread_signature_uleb(cursor, end, entry.first) or entry.first >= dependency_count) {
         bcread_error(State, ErrMsg::BCBAD);
      }

      // The owning dependency's declared range must actually contain this entry, otherwise a slice would expose a
      // function belonging to a different module.

      const auto &owner = Result.dependencies[entry.first];
      if (i < owner.first or i >= owner.first + owner.count) bcread_error(State, ErrMsg::BCBAD);

      Result.functions.push_back(entry);
   }

   if (cursor != end) bcread_error(State, ErrMsg::BCBAD);
   Result.present = true;
}

// Intern the validated names and write them into the colocated table.
//
// This must run only once the prototype's constant array is complete, because lj_str_new() can step the collector and
// the descriptor names are reachable solely through the prototype.  Each reference is therefore installed as soon as
// its string exists, and the table's counts are written first so that a traversal never sees an unbounded array.

static void bcread_install_dependencies(lua_State *L, GCproto *Proto, void *Buffer,
   const BCReadDependencies &Source)
{
   if (not Source.present) return;

   auto table = (ProtoDependencyTable *)Buffer;
   table->version = PROTO_DEPENDENCY_VERSION;
   table->reserved = 0;

   // dependency_count is final from the outset because the function array's position is derived from it.  Only
   // function_count grows as entries are filled, so a traversal during interning sees a prefix of complete entries.

   table->dependency_count = uint16_t(Source.dependencies.size());
   table->function_count = 0;

   auto dependencies = proto_dependency_list(table);
   auto functions = proto_dependency_functions(table);

   for (size_t i = 0; i < Source.dependencies.size(); ++i) {
      dependencies[i].first_function = uint16_t(Source.dependencies[i].first);
      dependencies[i].function_count = uint16_t(Source.dependencies[i].count);
      setgcref(dependencies[i].name, obj2gco(&G(L)->strempty));
   }

   setmref(Proto->dependencies, table);

   for (size_t i = 0; i < Source.dependencies.size(); ++i) {
      std::string_view name = Source.name_of(Source.dependencies[i]);
      setgcref(dependencies[i].name, obj2gco(lj_str_new(L, name.data(), name.size())));
   }

   for (size_t i = 0; i < Source.functions.size(); ++i) {
      std::string_view name = Source.name_of(Source.functions[i]);
      GCstr *interned = lj_str_new(L, name.data(), name.size());
      setgcref(functions[i].name, obj2gco(interned));
      functions[i].module = uint16_t(Source.functions[i].first);
      functions[i].reserved = 0;
      table->function_count = uint32_t(i + 1);
   }
}

//********************************************************************************************************************
// Read a prototype.

// Exception metadata follows the optional debug block. Read bounded integers and validate every register and PC
// before any loaded TRYENTER can reach the runtime.

static void bcread_exceptions(LexState *State, GCproto *Proto)
{
   auto cursor = (const uint8_t *)State->p;
   auto end = (const uint8_t *)State->pe;
   auto read = [&]() -> uint32_t {
      uint32_t value = 0;
      if (not bcread_signature_uleb(cursor, end, value)) bcread_error(State, ErrMsg::BCBAD);
      return value;
   };
   uint32_t blocks = read();
   uint32_t handlers = read();
   if (blocks > 0xffff or handlers > 0xffff) bcread_error(State, ErrMsg::BCBAD);
   bcread_account(State, uint64_t(blocks) + handlers);
   if (blocks) {
      MSize bytes = bcread_checked_multiply(State, blocks, MSize(sizeof(TryBlockDesc)));
      bcread_reserve_allocation(State, bytes);
      Proto->try_blocks = (TryBlockDesc *)lj_mem_new(State->L, bytes);
      Proto->try_block_count = uint16_t(blocks);
   }
   if (handlers) {
      MSize bytes = bcread_checked_multiply(State, handlers, MSize(sizeof(TryHandlerDesc)));
      bcread_reserve_allocation(State, bytes);
      Proto->try_handlers = (TryHandlerDesc *)lj_mem_new(State->L, bytes);
      Proto->try_handler_count = uint16_t(handlers);
   }
   for (uint32_t i = 0; i < blocks; ++i) {
      uint32_t first = read();
      uint32_t count = read();
      uint32_t slots = read();
      uint32_t flags = read();
      if (first > handlers or count > 0xff or count > handlers - first or
          slots > Proto->framesize or flags > TRY_FLAG_TRACE) bcread_error(State, ErrMsg::BCBAD);
      Proto->try_blocks[i] = TryBlockDesc{ uint16_t(first), uint8_t(count), uint8_t(slots), uint8_t(flags) };
   }
   for (uint32_t i = 0; i < handlers; ++i) {
      uint64_t filter = read();
      filter |= uint64_t(read()) << 32;
      uint32_t pc = read();
      uint32_t slot = read();
      if (pc IS 0 or pc >= Proto->sizebc or (slot != 0xff and slot >= Proto->framesize)) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      Proto->try_handlers[i] = TryHandlerDesc{ filter, pc, slot };
   }
   for (BCPOS pc = 1; pc < Proto->sizebc; ++pc) {
      BCIns ins = proto_bc(Proto)[pc];
      if (bc_op(ins) IS BC_TRYENTER) {
         if (bc_d(ins) >= blocks or bc_a(ins) != Proto->try_blocks[bc_d(ins)].entry_slots) {
            bcread_error(State, ErrMsg::BCBAD);
         }
      }
   }
   State->p = (const char *)cursor;
}

GCproto *lj_bcread_proto(LexState *State)
{
   GCproto *pt;
   MSize framesize, numparams, flags, sizeuv, sizekgc, sizekn, sizebc, sizept;
   MSize ofsk, ofsuv, ofssig, ofsdep, ofsdbg;
   MSize sizedbg = 0;
   MSize sizesig = 0;
   MSize sizedep = 0;
   BCLine firstline = 0, numline = 0;
   BCReadSignature signature;
   BCReadDependencies dependencies;

   // Read prototype header.
   flags     = bcread_byte(State);
   numparams = bcread_byte(State);
   framesize = bcread_byte(State);
   sizeuv    = bcread_byte(State);
   const uint8_t source_wire = bcread_byte(State);
   if ((source_wire >= State->compilation_sources.size()) and
       source_wire != FILESOURCE_SYNTHETIC_INDEX and source_wire != FILESOURCE_OVERFLOW_INDEX) {
      bcread_error(State, ErrMsg::BCBAD);
   }
   sizekgc   = bcread_uleb128(State);
   sizekn    = bcread_uleb128(State);
   sizebc    = bcread_checked_add(State, bcread_uleb128(State), 1);
   sizesig = bcread_uleb128(State);
   sizedep = bcread_uleb128(State);
   if (!(bcread_flags(State) & BCDUMP_F_STRIP)) {
      sizedbg = bcread_uleb128(State);
      if (sizedbg) {
         uint32_t raw_firstline = bcread_uleb128(State);
         uint32_t raw_numline = bcread_uleb128(State);
         if (raw_firstline > uint32_t(BCLine::LINE_MASK) or
             raw_numline > uint32_t(BCLine::LINE_MASK) - raw_firstline) bcread_error(State, ErrMsg::BCBAD);
         if (source_wire < State->compilation_sources.size() and raw_firstline >
             uint32_t(State->compilation_sources[source_wire].total_lines.lineNumber())) {
            bcread_error(State, ErrMsg::BCBAD);
         }
         firstline = BCLine(int32_t(raw_firstline));
         numline = BCLine(int32_t(raw_numline));
      }
   }
   constexpr MSize known_flags = PROTO_CHILD | PROTO_VARARG;
   if ((flags & ~known_flags) or framesize IS 0 or framesize > LJ_MAX_SLOTS or numparams > framesize or
       sizeuv > LJ_MAX_UPVAL or sizekgc > BCMAX_D + 1u or sizekn > BCMAX_D + 1u or
       sizebc > LJ_MAX_BCINS or ++State->bytecode_prototype_count > BCREAD_MAX_PROTOTYPES) {
      bcread_error(State, ErrMsg::BCBAD);
   }
   bcread_account(State, uint64_t(sizebc) + sizekgc + sizekn + sizeuv);
   bcread_signature(State, sizesig, numparams, signature);

   // The validated dependency names are referenced in place within the read buffer, which the next refill may move.
   // No further bcread_need()/bcread_want() call may occur before bcread_install_dependencies() has interned them.

   bcread_dependencies(State, sizedep, dependencies);

   // Calculate total size of prototype including all colocated arrays.

   MSize signature_memory_size = signature.present ?
      MSize(proto_signature_size(signature.parameter_count, signature.result_entry_count)) : 0;
   MSize dependency_memory_size = dependencies.present ?
      MSize(proto_dependency_size(dependencies.dependencies.size(), dependencies.functions.size())) : 0;
   sizept = bcread_checked_add(State, MSize(sizeof(GCproto)),
      bcread_checked_multiply(State, sizebc, MSize(sizeof(BCIns))));
   sizept = bcread_checked_add(State, sizept,
      bcread_checked_multiply(State, sizekgc, MSize(sizeof(GCRef))));
   sizept = bcread_checked_align(State, sizept, MSize(alignof(TValue)));
   ofsk = sizept;
   sizept = bcread_checked_add(State, sizept,
      bcread_checked_multiply(State, sizekn, MSize(sizeof(TValue))));
   ofsuv = sizept;
   sizept = bcread_checked_add(State, sizept, bcread_checked_multiply(State, (sizeuv + 1) & ~1u, 2));
   ofssig = sizept;
   sizept = bcread_checked_add(State, sizept, signature_memory_size);
   // The descriptors hold GCRef fields and must be naturally aligned; the upvalue array is only 2-byte granular.
   sizept = bcread_checked_align(State, sizept, MSize(alignof(ProtoDependency)));
   ofsdep = sizept;
   sizept = bcread_checked_add(State, sizept, dependency_memory_size);
   ofsdbg = sizept;
   sizept = bcread_checked_add(State, sizept, sizedbg);
   if (sizept > BCREAD_MAX_PROTO_SIZE) bcread_error(State, ErrMsg::BCBAD);
   bcread_reserve_allocation(State, sizept);

   // Allocate prototype object and initialize its fields.

   pt = (GCproto*)lj_mem_newgco(State->L, (MSize)sizept);
   proto_metadata_init(pt);
   pt->gct = ~LJ_TPROTO;
   pt->numparams = (uint8_t)numparams;
   pt->framesize = (uint8_t)framesize;
   pt->sizebc = sizebc;
   setmref(pt->k, (char*)pt + ofsk);
   setmref(pt->uv, (char*)pt + ofsuv);
   // Publish only valid GC references before any subsequent read can allocate or trigger collection.
   GCRef *constants = mref<GCRef>(pt->k) - ptrdiff_t(sizekgc);
   for (MSize i = 0; i < sizekgc; ++i) setgcref(constants[i], obj2gco(&G(State->L)->strempty));
   pt->sizekgc = sizekgc;
   pt->sizekn = sizekn;
   pt->sizept = sizept;
   pt->sizeuv = (uint8_t)sizeuv;
   pt->flags = (uint8_t)flags;
   pt->trace = 0;
   pt->file_source_idx = source_wire;
   setgcref(pt->chunk_name, obj2gco(State->chunk_name));
   bcread_install_signature(pt, (char *)pt + ofssig, signature);

   // Close potentially uninitialized gap between bc and kgc.

   *(uint32_t*)((char*)pt + ofsk - sizeof(GCRef) * (sizekgc + 1)) = 0;

   // Read bytecode instructions and upvalue refs.

   bcread_bytecode(State, pt, sizebc);
   bcread_uv(State, pt, sizeuv);

   // Read constants.

   uint16_t prototype_depth = bcread_kgc(State, pt, sizekgc);
   bcread_knum(State, pt, sizekn);
   bcread_builtin_methods(State, pt);
   bcread_validate_bytecode(State, pt);
   bcread_validate_child_upvalues(State, pt);

   // Deferred until the constant array is complete, because interning the names can step the collector and the
   // prototype must be safe to traverse by then.

   bcread_install_dependencies(State->L, pt, (char *)pt + ofsdep, dependencies);
   bcread_module_activations(State, pt);

   // Read and initialize debug info.

   pt->firstline = firstline;
   pt->numline = numline;
   if (sizedbg) {
      // lineinfo is now a fixed-size BCLine[sizebc-1] array (32-bit per instruction)
      MSize sizeli = bcread_checked_multiply(State, sizebc - 1, MSize(sizeof(BCLine)));
      if (sizedbg < sizeli + 1) bcread_error(State, ErrMsg::BCBAD);
      setmref(pt->lineinfo, (char*)pt + ofsdbg);
      bcread_dbg(State, pt, sizedbg);
      BCLine *lines = (BCLine *)proto_lineinfo(pt);
      for (MSize i = 0; i + 1 < sizebc; ++i) {
         const uint8_t wire = lines[i].fileIndex();
         if ((wire >= State->compilation_sources.size()) and wire != FILESOURCE_SYNTHETIC_INDEX and
             wire != FILESOURCE_OVERFLOW_INDEX) bcread_error(State, ErrMsg::BCBAD);
         if (wire < State->compilation_sources.size() and lines[i].lineNumber() >
             State->compilation_sources[wire].total_lines.lineNumber()) bcread_error(State, ErrMsg::BCBAD);
      }
   }
   else {
      setmref(pt->lineinfo, nullptr);
      setmref(pt->uvinfo, nullptr);
      setmref(pt->varinfo, nullptr);
   }
   bcread_exceptions(State, pt);
   lj_contract_build_cache(State->L, pt);
   State->bytecode_prototype_depths.push_back(prototype_depth);
   return pt;
}

//********************************************************************************************************************
// Read and check header of bytecode dump.

static int bcread_header(LexState *State)
{
   uint32_t flags;
   bcread_want(State, 3 + 5 + 5);
   if (bcread_byte(State) != BCDUMP_HEAD2 or bcread_byte(State) != BCDUMP_HEAD3) return 0;
   uint8_t version = uint8_t(bcread_byte(State));
   if (version != BCDUMP_VERSION) return 0;
   State->bytecode_version = version;
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
   const MSize source_block_size = bcread_uleb128(State);
   if (source_block_size < 3 or source_block_size > BCREAD_MAX_VALIDATION_WORK) return 0;
   bcread_need(State, source_block_size);
   const uint8_t *cursor = (const uint8_t *)State->p;
   const uint8_t *end = cursor + source_block_size;
   auto byte = [&]() -> uint8_t {
      if (cursor >= end) bcread_error(State, ErrMsg::BCBAD);
      return *cursor++;
   };
   auto uleb = [&]() -> uint32_t {
      uint32_t value = 0;
      for (unsigned shift = 0; shift <= 28; shift += 7) {
         uint32_t current = byte();
         if (shift IS 28 and current > 0x0f) bcread_error(State, ErrMsg::BCBAD);
         value |= (current & 0x7f) << shift;
         if (not (current & 0x80)) return value;
      }
      bcread_error(State, ErrMsg::BCBAD);
      return 0;
   };
   auto string = [&]() -> std::string {
      uint32_t length = uleb();
      if (length > uint32_t(end - cursor)) bcread_error(State, ErrMsg::BCBAD);
      std::string result((const char *)cursor, length);
      cursor += length;
      return result;
   };
   const uint8_t source_version = byte();
   const uint8_t source_count = byte();
   const uint8_t source_root = byte();
   if (source_version != COMPILATION_SOURCE_VERSION or source_count IS 0 or
       source_count > FILESOURCE_MAX_COUNT or source_root != 0) return 0;
   State->compilation_sources.clear();
   State->compilation_sources.reserve(source_count);
   bcread_account(State, source_count);
   bcread_reserve_allocation(State, source_block_size + source_count * sizeof(CompilationSourceRecord));
   for (uint32_t i = 0; i < source_count; ++i) {
      const auto role = CompilationSourceRole(byte());
      const uint8_t parent = byte();
      if (byte() or byte()) bcread_error(State, ErrMsg::BCBAD);
      const uint32_t first_line = uleb();
      const uint32_t total_lines = uleb();
      const uint32_t import_line = uleb();
      std::string path = string();
      std::string filename = string();
      std::string declared_namespace = string();
      if (role != CompilationSourceRole::Main and role != CompilationSourceRole::Import and
          role != CompilationSourceRole::Synthetic) bcread_error(State, ErrMsg::BCBAD);
      if (first_line IS 0 or total_lines IS 0 or first_line > uint32_t(BCLine::LINE_MASK) or
          total_lines > uint32_t(BCLine::LINE_MASK) - first_line + 1) bcread_error(State, ErrMsg::BCBAD);
      if (i IS source_root) {
         if ((role != CompilationSourceRole::Main and role != CompilationSourceRole::Synthetic) or
             parent != FILESOURCE_OVERFLOW_INDEX or import_line != 0) bcread_error(State, ErrMsg::BCBAD);
      }
      else if (role != CompilationSourceRole::Import or parent >= i or import_line IS 0 or
               import_line > uint32_t(State->compilation_sources[parent].total_lines.lineNumber())) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      if ((role IS CompilationSourceRole::Synthetic) != path.empty() or filename.empty() or
          path.find('\0') != std::string::npos or filename.find('\0') != std::string::npos or
          declared_namespace.find('\0') != std::string::npos) {
         bcread_error(State, ErrMsg::BCBAD);
      }
      for (const auto &existing : State->compilation_sources) {
         if (not path.empty() and existing.canonical_path IS path) bcread_error(State, ErrMsg::BCBAD);
      }
      State->compilation_sources.push_back(CompilationSourceRecord{
         .role = role,
         .canonical_path = std::move(path),
         .display_filename = std::move(filename),
         .declared_namespace = std::move(declared_namespace),
         .first_line = BCLine(int32_t(first_line)),
         .total_lines = BCLine(int32_t(total_lines)),
         .import_line = BCLine(int32_t(import_line)),
         .parent = parent
      });
   }
   if (cursor != end) bcread_error(State, ErrMsg::BCBAD);
   State->p = (const char *)end;

   const MSize struct_block_size = bcread_uleb128(State);
   if (struct_block_size < 2 or struct_block_size > BCREAD_MAX_VALIDATION_WORK) return 0;
   bcread_need(State, struct_block_size);
   bcread_account(State, struct_block_size);
   bcread_reserve_allocation(State, struct_block_size);
   State->bytecode_struct_manifest.assign((const uint8_t *)State->p,
      (const uint8_t *)State->p + struct_block_size);
   std::string detail;
   ERR struct_error = load_declared_struct_manifest(State->L,
      std::string_view(State->p, struct_block_size), State->loaded_structs, &detail);
   if (struct_error != ERR::Okay) {
      bcread_error(State, ErrMsg::BCBAD);
   }
   State->p += struct_block_size;
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
   State->bytecode_prototype_count = 0;
   State->bytecode_allocation = 0;
   State->bytecode_validation_work = 0;
   State->bytecode_prototype_depths.clear();
   State->compilation_sources.clear();
   State->bytecode_struct_manifest.clear();
   State->loaded_structs.clear();
   State->loaded_structs_committed = false;

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
      if (len > BCREAD_MAX_PROTO_SIZE) bcread_error(State, ErrMsg::BCBAD);
      bcread_need(State, len);
      startp = State->p;
      const char *saved_end = State->pe;
      lua_Reader saved_reader = State->rfunc;
      State->pe = startp + len;
      State->rfunc = nullptr;
      pt = lj_bcread_proto(State);
      if (State->p != startp + len) bcread_error(State, ErrMsg::BCBAD);
      State->pe = saved_end;
      State->rfunc = saved_reader;
      setprotoV(L, L->top, pt);
      incr_top(L);
   }

   if ((State->pe != State->p and !State->endmark) or L->top - 1 != bcread_oldtop(L, ls) or
       State->bytecode_prototype_depths.size() != 1)
      bcread_error(State, ErrMsg::BCBAD);

   // Publish source records only after all bytecode validation has succeeded.  Keep the root on the stack while
   // installing interned strings and state records can allocate.
   GCproto *root = protoV(L->top - 1);
   attach_loaded_compilation_sources(L, root, State->compilation_sources);
   auto manifest = (uint8_t *)lj_mem_new(L, MSize(State->bytecode_struct_manifest.size()));
   memcpy(manifest, State->bytecode_struct_manifest.data(), State->bytecode_struct_manifest.size());
   setmref(root->struct_manifest, manifest);
   root->struct_manifest_size = uint32_t(State->bytecode_struct_manifest.size());
   State->loaded_structs_committed = true;
   L->top--;
   return root;
}
