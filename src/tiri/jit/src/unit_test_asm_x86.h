// Included by lj_asm.cpp so the tests exercise the actual assembler and register allocator.
// Synthetic IR covers 64-bit arithmetic shapes that scripts cannot reliably produce with FFI disabled.

#include <memory>
#include <new>
#include "debug/filesource.h"

struct AsmX64TestState {
   ASMState as;
   GG_State gg;
   IRIns ir[REF_BIAS + 8];
   MCode code[256];

   void reset()
   {
      as = {};
      memset(ir, 0, sizeof(ir));
      memset(code, 0, sizeof(code));
      as.J = &gg.J;
      setnilV(&gg.g.nilnode.val);
      as.ir = ir;
      as.mcp = as.mctop = code + sizeof(code);
      as.mclim = code;
      as.curins = REF_BIAS + 4;
      as.freeset = RSET_GPR & ~(RID2RSET(RID_EAX) | RID2RSET(RID_ECX) | RID2RSET(RID_EDX));
      for (unsigned i = 1; i <= 4; ++i) {
         ir[REF_BIAS + i].t.irt = IRT_U64;
         ir[REF_BIAS + i].r = RID_INIT;
      }
      ir[REF_BIAS + 1].r = RID_EAX;
      ir[REF_BIAS + 2].r = RID_ECX;
      ir[REF_BIAS + 4].r = RID_EDX;
      ir[REF_BIAS + 4].o = IR_ADD;
      ir[REF_BIAS + 4].op1 = REF_BIAS + 1;
      ir[REF_BIAS + 4].op2 = REF_BIAS + 2;
   }
};

const char* lj_asm_test_x64()
{
   // Keep the reference-indexed IR array off the native stack, including on Windows.
   std::unique_ptr<AsmX64TestState> state(new (std::nothrow) AsmX64TestState{});
   if (!state) return "Cannot allocate assembler test state";
   ASMState* as = &state->as;
   IRIns* result = &state->ir[REF_BIAS + 4];
   IRIns* constant = &state->ir[REF_BIAS - 4];
   IRIns* nested = &state->ir[REF_BIAS + 3];

   for (IRType type : { IRT_INT, IRT_U64 }) {
      state->reset();
      result->t.irt = type;
      if (!asm_lea(as, result)) return "Register ADD did not select LEA";
      const uint8_t expected[] = { 0x48, 0x8d, 0x14, 0x08 }; // lea rdx, [rax+rcx]
      unsigned prefix = type IS IRT_INT ? 1 : 0;
      if (as->mctop - as->mcp != 4 - prefix or memcmp(as->mcp, expected + prefix, 4 - prefix))
         return "LEA register operands or result width are incorrect";
   }

   const int64_t offsets[] = { INT64_C(-2147483648), INT64_C(2147483647),
      INT64_C(-2147483649), INT64_C(2147483648), INT64_C(4294967296) };
   for (int64_t offset : offsets) {
      state->reset();
      result->op2 = REF_BIAS - 4;
      constant->o = IR_KINT64;
      constant->t.irt = IRT_U64;
      constant[1].tv.u64 = uint64_t(offset);
      bool fits = offset >= INT64_C(-2147483648) and offset <= INT64_C(2147483647);
      if (bool(asm_lea(as, result)) != fits) return "LEA accepted an invalid displacement or rejected a valid one";
      if (fits) {
         int32_t encoded;
         memcpy(&encoded, as->mctop - 4, sizeof(encoded));
         if (as->mctop - as->mcp != 7 or as->mcp[0] != 0x48 or encoded != offset)
            return "LEA truncated a 64-bit constant incorrectly";
      }
      else if (as->mcp != as->mctop) return "Rejected LEA emitted instructions";
   }

   for (bool right : { false, true }) {
      for (IRType type : { IRT_INT, IRT_U64 }) {
         state->reset();
         constant->o = IR_KINT;
         constant->t.irt = IRT_INT;
         constant->i = 17;
         nested->o = IR_ADD;
         nested->t.irt = type;
         nested->op1 = right ? REF_BIAS + 2 : REF_BIAS + 1;
         nested->op2 = REF_BIAS - 4;
         if (right) result->op2 = REF_BIAS + 3;
         else result->op1 = REF_BIAS + 3;
         if (bool(asm_lea(as, result)) != (type IS IRT_U64))
            return "LEA nested ADD fusion did not respect the intermediate width";
         if (type IS IRT_U64) {
            const uint8_t expected[] = { 0x48, 0x8d, 0x54, 0x08, 0x11 }; // lea rdx, [rax+rcx+17]
            if (as->mctop - as->mcp != sizeof(expected) or memcmp(as->mcp, expected, sizeof(expected)))
               return "LEA nested ADD lost an operand or displacement";
         }
      }
   }

   for (IRType type : { IRT_INT, IRT_U64 }) {
      for (int factor : { 3, 5, 9, 7 }) {
         // Plain register multiplication, overflow guard, fused load, and a following zero comparison.
         for (unsigned variant = 0; variant < 4; ++variant) {
            state->reset();
            result->o = variant IS 1 ? IR_MULOV : IR_MUL;
            result->t.irt = uint8_t(type) | (variant IS 1 ? IRT_GUARD : 0);
            result->op2 = REF_BIAS - 4;
            constant->o = type IS IRT_INT ? IR_KINT : IR_KINT64;
            constant->t.irt = type;
            constant->r = RID_INIT;
            if (type IS IRT_INT) constant->i = factor;
            else constant[1].tv.u64 = uint64_t(factor);
            IRIns* source = &state->ir[result->op1];
            source->t.irt = type;
            if (variant IS 2) {
               source->o = IR_XLOAD;
               source->r = RID_INIT;
               source->op1 = REF_BIAS + 2;
            }
            if (variant IS 3) {
               emit_jcc(as, CC_E, state->code);
               emit_rr(as, XO_TEST, REX_64IR(result, RID_EDX), RID_EDX);
               as->flagmcp = as->mcp;
            }
            state->gg.J.exitstubgroup[0] = state->code;
            if (variant IS 1) asm_mulov(as, result);
            else asm_mul(as, result);
            unsigned leas = 0, multiplies = 0, overflow_guards = 0;
            for (MCode* p = as->mcp; p < as->mctop;) {
               unsigned length = asm_x86_inslen(p);
               if (!length or p + length > as->mctop) return "Invalid multiply instruction encoding";
               MCode* opcode = (*p & 0xf0) IS 0x40 ? p + 1 : p;
               if (*opcode IS 0x8d) {
                  ++leas;
                  uint8_t scale = factor IS 3 ? 0x40 : factor IS 5 ? 0x80 : 0xc0;
                  if (opcode[1] != 0x14 or opcode[2] != scale or
                     ((opcode != p and (*p & 8)) != (type IS IRT_U64)))
                     return "Scaled multiply LEA has incorrect operands or width";
               }
               if (*opcode IS 0x6b or *opcode IS 0x69) ++multiplies;
               if (*opcode IS 0x0f and opcode[1] IS 0x80) ++overflow_guards;
               p += length;
            }
            bool use_lea = variant IS 0 and factor != 7;
            if (leas != unsigned(use_lea) or multiplies != unsigned(!use_lea))
               return "Multiply selection failed to preserve flags, overflow checking or load fusion";
            if (overflow_guards != unsigned(variant IS 1)) return "Multiply lost its overflow guard";
         }
      }
   }

   for (IRType type : { IRT_NUM, IRT_TAB, IRT_STR }) {
      for (bool scratch : { false, true }) {
         for (IROp merge : { IR_HREF, IR_EQ, IR_NE }) {
            state->reset();
            GCstr string_key{};
            string_key.gct = uint8_t(~LJ_TSTR);
            string_key.sid = 123;
            result->o = IR_HREF;
            result->t.irt = IRT_P64;
            result->op2 = REF_BIAS - 4;
            constant->o = type IS IRT_NUM ? IR_KNUM : IR_KGC;
            constant->t.irt = type;
            uint64_t bits = type IS IRT_NUM ? UINT64_C(0x3fe0000000000000) :
               type IS IRT_STR ? u64ptr(&string_key) : u64ptr(&state->gg);
            constant[1].tv.u64 = bits;
            if (type != IRT_NUM) bits |= uint64_t(irt_toitype(constant->t)) << 47;
            as->freeset = scratch ? RID2RSET(RID_R8D) : RSET_EMPTY;
            state->gg.J.exitstubgroup[0] = state->code;
            asm_href(as, result, merge);
            unsigned full_compares = 0, split_compares = 0;
            MCode* key_load = nullptr;
            MCode* loop_target = nullptr;
            for (MCode* p = as->mcp; p < as->mctop;) {
               unsigned length = asm_x86_inslen(p);
               if (!length or p + length > as->mctop) return "Invalid constant hash lookup encoding";
               MCode* opcode = (*p & 0xf0) IS 0x40 ? p + 1 : p;
               if (*opcode IS 0x3b) {
                  ++full_compares;
                  if (p[0] != 0x4c or opcode[1] != 0x42 or opcode[2] != offsetof(Node, key.u64))
                     return "Constant key comparison lost its width or operands";
               }
               if ((*opcode IS 0x81 or *opcode IS 0x83) and (opcode[1] & 0x38) IS 0x38)
                  ++split_compares;
               if (p[0] IS 0x49 and p[1] IS 0xb8) {
                  uint64_t encoded;
                  memcpy(&encoded, p + 2, sizeof(encoded));
                  if (encoded != bits) return "Constant key materialisation lost tag or payload bits";
                  key_load = p;
               }
               if (*p IS 0x75 and int8_t(p[1]) < 0) loop_target = p + 2 + int8_t(p[1]);
               p += length;
            }
            if (full_compares != unsigned(scratch) or split_compares != (scratch ? 0u : 2u))
               return "Constant hash lookup failed to select the register-pressure fallback";
            if (scratch and (!key_load or !loop_target or key_load >= loop_target))
               return "Constant key load was not placed outside the hash-chain backedge";
         }
      }
   }

   for (bool scratch : { false, true }) {
      state->reset();
      // Include source and address registers in freeset deliberately: the store must still exclude them.
      as->freeset = RID2RSET(RID_EAX) | RID2RSET(RID_ECX) | RID2RSET(RID_EDX);
      if (scratch) rset_set(as->freeset, RID_R8D);
      as->mrm.base = RID_EAX;
      as->mrm.idx = RID_ECX;
      as->mrm.scale = XM_SCALE8;
      as->mrm.ofs = 24;
      IRType1 type;
      type.irt = IRT_TAB;
      asm_storetagged(as, type, RID_EDX);
      unsigned count = 0;
      for (MCode* p = as->mcp; p < as->mctop;) {
         unsigned length = asm_x86_inslen(p);
         if (!length or p + length > as->mctop) return "Tagged store broke instruction decoding";
         p += length;
         ++count;
      }
      if (count != (scratch ? 3u : 2u)) return "Unexpected tagged store instruction count";
      if (as->modset != (scratch ? RID2RSET(RID_R8D) : RSET_EMPTY))
         return "Tagged store clobbered a source/address register or spilled under pressure";
      if (scratch) {
         const uint8_t expected[] = { 0x4c, 0x89, 0x44, 0xc8, 0x18 }; // mov [rax+rcx*8+24], r8
         if (memcmp(as->mctop - sizeof(expected), expected, sizeof(expected)))
            return "Tagged store lost its fused address or full-width scratch register";
      }
   }
   return nullptr;
}
