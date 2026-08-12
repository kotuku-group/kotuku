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

#include <vector>

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
   MSize flags = bcread_uleb128(State);
   State->assert_condition((flags & ~MSize(TAB_NOT_SEQUENCE)) IS 0, "bad table flags %d", flags);
   MSize narray = bcread_uleb128(State);
   MSize nhash = bcread_uleb128(State);
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
   if (sizebc < 2) bcread_error(State, ErrMsg::BCBAD);

   BCIns* bc = proto_bc(pt);
   BCREG context_entries[BCMAX_A + 1];
   MSize context_entry_depth = 0;
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
      if (op IS BC_MRSAVE or op IS BC_MRRESTORE) {
         pt->flags |= PROTO_NOJIT;
      }
      else if (op IS BC_TCTX) {
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
         if (slot >= pt->framesize or descriptor != context_blocks.size()) bcread_error(State, ErrMsg::BCBAD);
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

   if (not context_blocks.empty()) {
      size_t byte_size = context_blocks.size() * sizeof(ProtoContextBlockDesc);
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
   ProtoTypeEntry &Entry, uint8_t Version)
{
   if (End - Cursor < 2) bcread_error(State, ErrMsg::BCBAD);
   uint8_t type = *Cursor++;
   uint8_t flags = *Cursor++;
   uint32_t constraint = 0;
   if (not bcread_signature_uleb(Cursor, End, constraint)) bcread_error(State, ErrMsg::BCBAD);
   uint32_t reserved = 0;
   if (Version >= 2 and not bcread_signature_uleb(Cursor, End, reserved)) bcread_error(State, ErrMsg::BCBAD);

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
      if (Version < 2 or not reserved or reserved > uint32_t(AET::MAX)) bcread_error(State, ErrMsg::BCBAD);
      AET member = AET(reserved - 1);
      if ((member IS AET::STRUCT) != (constraint != 0)) bcread_error(State, ErrMsg::BCBAD);
   }
   else if (reserved) bcread_error(State, ErrMsg::BCBAD);

   Entry = ProtoTypeEntry{
      .constraint = constraint,
      .type = entry_type,
      .flags = flags,
      .reserved = uint16_t(reserved)
   };
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
   if (version != 1 and version != PROTO_SIGNATURE_VERSION) bcread_error(State, ErrMsg::BCBAD);

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
      bcread_signature_entry(State, cursor, end, Result.parameters[i], version);
   }
   for (uint32_t i = 0; i < result_entry_count; ++i) {
      bcread_signature_entry(State, cursor, end, Result.results[i], version);
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
   sizekgc   = bcread_uleb128(State);
   sizekn    = bcread_uleb128(State);
   sizebc    = bcread_uleb128(State) + 1;
   sizesig = bcread_uleb128(State);
   sizedep = bcread_uleb128(State);
   if (!(bcread_flags(State) & BCDUMP_F_STRIP)) {
      sizedbg = bcread_uleb128(State);
      if (sizedbg) {
         firstline = bcread_uleb128(State);
         numline = bcread_uleb128(State);
      }
   }
   bcread_signature(State, sizesig, numparams, signature);

   // The validated dependency names are referenced in place within the read buffer, which the next refill may move.
   // No further bcread_need()/bcread_want() call may occur before bcread_install_dependencies() has interned them.

   bcread_dependencies(State, sizedep, dependencies);

   // Calculate total size of prototype including all colocated arrays.

   MSize signature_memory_size = signature.present ?
      MSize(proto_signature_size(signature.parameter_count, signature.result_entry_count)) : 0;
   MSize dependency_memory_size = dependencies.present ?
      MSize(proto_dependency_size(dependencies.dependencies.size(), dependencies.functions.size())) : 0;
   sizept = (MSize)sizeof(GCproto) + sizebc * (MSize)sizeof(BCIns) + sizekgc * (MSize)sizeof(GCRef);
   sizept = (sizept + (MSize)sizeof(TValue) - 1) & ~((MSize)sizeof(TValue) - 1);
   ofsk   = sizept; sizept += sizekn * (MSize)sizeof(TValue);
   ofsuv  = sizept; sizept += ((sizeuv + 1) & ~1) * 2;
   ofssig = sizept; sizept += signature_memory_size;
   // The descriptors hold GCRef fields and must be naturally aligned; the upvalue array is only 2-byte granular.
   sizept = (sizept + (MSize)alignof(ProtoDependency) - 1) & ~((MSize)alignof(ProtoDependency) - 1);
   ofsdep = sizept; sizept += dependency_memory_size;
   ofsdbg = sizept; sizept += sizedbg;

   // Allocate prototype object and initialize its fields.

   pt = (GCproto*)lj_mem_newgco(State->L, (MSize)sizept);
   proto_metadata_init(pt);
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
   bcread_install_signature(pt, (char *)pt + ofssig, signature);

   // Close potentially uninitialized gap between bc and kgc.

   *(uint32_t*)((char*)pt + ofsk - sizeof(GCRef) * (sizekgc + 1)) = 0;

   // Read bytecode instructions and upvalue refs.

   bcread_bytecode(State, pt, sizebc);
   bcread_uv(State, pt, sizeuv);

   // Read constants.

   bcread_kgc(State, pt, sizekgc);
   pt->sizekgc = sizekgc;
   bcread_knum(State, pt, sizekn);
   bcread_builtin_methods(State, pt);

   // Deferred until the constant array is complete, because interning the names can step the collector and the
   // prototype must be safe to traverse by then.

   bcread_install_dependencies(State->L, pt, (char *)pt + ofsdep, dependencies);
   bcread_module_activations(State, pt);

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
   lj_contract_build_cache(State->L, pt);
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
