// Unit tests for VM assembly details that cannot be observed from Tiri.

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arch.h"
#include "lj_asm.h"
#include "lj_vm.h"

#include "../../defs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

struct TestCase {
   const char *name;
   bool (*fn)(kt::Log &Log);
};

#if LJ_TARGET_X86ORX64

#if defined(_MSC_VER)
#if defined(_M_X64)
#define TIRI_TEST_MSVC_X64
#endif
#endif

#if defined(__GNUC__)
#if defined(__x86_64__)
#define TIRI_TEST_GNU_X64
#endif
#endif

enum RegisterBit : uint32_t {
   REG_RBX   = 1 << 0,
   REG_RBP   = 1 << 1,
   REG_RDI   = 1 << 2,
   REG_RSI   = 1 << 3,
   REG_R12   = 1 << 4,
   REG_R13   = 1 << 5,
   REG_R14   = 1 << 6,
   REG_R15   = 1 << 7,
   REG_RSP   = 1 << 8,
   REG_XMM6  = 1 << 9,
   REG_XMM7  = 1 << 10,
   REG_XMM8  = 1 << 11,
   REG_XMM9  = 1 << 12,
   REG_XMM10 = 1 << 13,
   REG_XMM11 = 1 << 14,
   REG_XMM12 = 1 << 15,
   REG_XMM13 = 1 << 16,
   REG_XMM14 = 1 << 17,
   REG_XMM15 = 1 << 18,
};

#if defined(TIRI_TEST_MSVC_X64)

struct alignas(16) RegisterSnapshot {
   uint64_t rbx, rbp, rdi, rsi, r12, r13, r14, r15, rsp;
   alignas(16) uint8_t xmm6[16], xmm7[16], xmm8[16], xmm9[16], xmm10[16];
   alignas(16) uint8_t xmm11[16], xmm12[16], xmm13[16], xmm14[16], xmm15[16];
};

static_assert(alignof(RegisterSnapshot) IS 16, "RegisterSnapshot alignment mismatch");
static_assert(offsetof(RegisterSnapshot, rbx) IS 0, "RegisterSnapshot rbx offset mismatch");
static_assert(offsetof(RegisterSnapshot, rbp) IS 8, "RegisterSnapshot rbp offset mismatch");
static_assert(offsetof(RegisterSnapshot, rdi) IS 16, "RegisterSnapshot rdi offset mismatch");
static_assert(offsetof(RegisterSnapshot, rsi) IS 24, "RegisterSnapshot rsi offset mismatch");
static_assert(offsetof(RegisterSnapshot, r12) IS 32, "RegisterSnapshot r12 offset mismatch");
static_assert(offsetof(RegisterSnapshot, r13) IS 40, "RegisterSnapshot r13 offset mismatch");
static_assert(offsetof(RegisterSnapshot, r14) IS 48, "RegisterSnapshot r14 offset mismatch");
static_assert(offsetof(RegisterSnapshot, r15) IS 56, "RegisterSnapshot r15 offset mismatch");
static_assert(offsetof(RegisterSnapshot, rsp) IS 64, "RegisterSnapshot rsp offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm6) IS 80, "RegisterSnapshot xmm6 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm7) IS 96, "RegisterSnapshot xmm7 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm8) IS 112, "RegisterSnapshot xmm8 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm9) IS 128, "RegisterSnapshot xmm9 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm10) IS 144, "RegisterSnapshot xmm10 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm11) IS 160, "RegisterSnapshot xmm11 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm12) IS 176, "RegisterSnapshot xmm12 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm13) IS 192, "RegisterSnapshot xmm13 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm14) IS 208, "RegisterSnapshot xmm14 offset mismatch");
static_assert(offsetof(RegisterSnapshot, xmm15) IS 224, "RegisterSnapshot xmm15 offset mismatch");
static_assert(sizeof(RegisterSnapshot) IS 240, "RegisterSnapshot size mismatch");

extern "C" void asm_capture_registers(RegisterSnapshot *Snapshot);
extern "C" int asm_verify_registers(const RegisterSnapshot *Before, const RegisterSnapshot *After);
extern "C" void asm_call_cpuid_and_capture(RegisterSnapshot *Before, RegisterSnapshot *After,
   int (*Function)(uint32_t, uint32_t *), uint32_t *Results);

static constexpr bool glHasRegisterCapture = true;

static void capture_registers(RegisterSnapshot *Snapshot)
{
   asm_capture_registers(Snapshot);
}

static bool verify_registers(const RegisterSnapshot *Before, const RegisterSnapshot *After, kt::Log &Log)
{
   int result = asm_verify_registers(Before, After);
   if (result IS 0) return true;
   if (result & REG_RBX) Log.error("RBX corrupted: 0x%016llx -> 0x%016llx", Before->rbx, After->rbx);
   if (result & REG_RBP) Log.error("RBP corrupted: 0x%016llx -> 0x%016llx", Before->rbp, After->rbp);
   if (result & REG_RDI) Log.error("RDI corrupted: 0x%016llx -> 0x%016llx", Before->rdi, After->rdi);
   if (result & REG_RSI) Log.error("RSI corrupted: 0x%016llx -> 0x%016llx", Before->rsi, After->rsi);
   if (result & REG_R12) Log.error("R12 corrupted: 0x%016llx -> 0x%016llx", Before->r12, After->r12);
   if (result & REG_R13) Log.error("R13 corrupted: 0x%016llx -> 0x%016llx", Before->r13, After->r13);
   if (result & REG_R14) Log.error("R14 corrupted: 0x%016llx -> 0x%016llx", Before->r14, After->r14);
   if (result & REG_R15) Log.error("R15 corrupted: 0x%016llx -> 0x%016llx", Before->r15, After->r15);
   if (result & REG_RSP) Log.error("RSP corrupted: 0x%016llx -> 0x%016llx", Before->rsp, After->rsp);
   if (result & REG_XMM6) Log.error("XMM6 corrupted");
   if (result & REG_XMM7) Log.error("XMM7 corrupted");
   if (result & REG_XMM8) Log.error("XMM8 corrupted");
   if (result & REG_XMM9) Log.error("XMM9 corrupted");
   if (result & REG_XMM10) Log.error("XMM10 corrupted");
   if (result & REG_XMM11) Log.error("XMM11 corrupted");
   if (result & REG_XMM12) Log.error("XMM12 corrupted");
   if (result & REG_XMM13) Log.error("XMM13 corrupted");
   if (result & REG_XMM14) Log.error("XMM14 corrupted");
   if (result & REG_XMM15) Log.error("XMM15 corrupted");
   return false;
}

#define HAS_CPUID_DIRECT_CAPTURE

#elif defined(TIRI_TEST_GNU_X64)

struct RegisterSnapshot {
   uint64_t rbx, rbp, r12, r13, r14, r15, rsp;
};

static constexpr bool glHasRegisterCapture = true;

#if !defined(_WIN32)
static __attribute__((naked, noinline)) void asm_call_cpuid_and_capture(RegisterSnapshot *, RegisterSnapshot *,
   int (*)(uint32_t, uint32_t *), uint32_t *)
{
   __asm__ __volatile__(
      "subq $40, %rsp\n\t"
      "movq %rdi, 0(%rsp)\n\t"
      "movq %rsi, 8(%rsp)\n\t"
      "movq %rdx, 16(%rsp)\n\t"
      "movq %rcx, 24(%rsp)\n\t"
      "movq 0(%rsp), %rax\n\t"
      "movq %rbx, 0(%rax)\n\t"
      "movq %rbp, 8(%rax)\n\t"
      "movq %r12, 16(%rax)\n\t"
      "movq %r13, 24(%rax)\n\t"
      "movq %r14, 32(%rax)\n\t"
      "movq %r15, 40(%rax)\n\t"
      "movq %rsp, 48(%rax)\n\t"
      "xorl %edi, %edi\n\t"
      "movq 24(%rsp), %rsi\n\t"
      "call *16(%rsp)\n\t"
      "movl $1, %edi\n\t"
      "movq 24(%rsp), %rsi\n\t"
      "call *16(%rsp)\n\t"
      "movq 8(%rsp), %rax\n\t"
      "movq %rbx, 0(%rax)\n\t"
      "movq %rbp, 8(%rax)\n\t"
      "movq %r12, 16(%rax)\n\t"
      "movq %r13, 24(%rax)\n\t"
      "movq %r14, 32(%rax)\n\t"
      "movq %r15, 40(%rax)\n\t"
      "movq %rsp, 48(%rax)\n\t"
      "addq $40, %rsp\n\t"
      "ret\n\t");
}
#define HAS_CPUID_DIRECT_CAPTURE
#endif

static void capture_registers(RegisterSnapshot *Snapshot)
{
   __asm__ __volatile__(
      "movq %%rbx, %0\n\t"
      "movq %%rbp, %1\n\t"
      "movq %%r12, %2\n\t"
      "movq %%r13, %3\n\t"
      "movq %%r14, %4\n\t"
      "movq %%r15, %5\n\t"
      "movq %%rsp, %6\n\t"
      : "=m"(Snapshot->rbx), "=m"(Snapshot->rbp), "=m"(Snapshot->r12),
        "=m"(Snapshot->r13), "=m"(Snapshot->r14), "=m"(Snapshot->r15), "=m"(Snapshot->rsp)
      :
      : "memory");
}

static bool verify_registers(const RegisterSnapshot *Before, const RegisterSnapshot *After, kt::Log &Log)
{
   bool passed = true;
   auto check = [&](uint64_t BeforeValue, uint64_t AfterValue, const char *Name) {
      if (BeforeValue IS AfterValue) return;
      Log.error("%s corrupted: 0x%016llx -> 0x%016llx", Name,
         (unsigned long long)BeforeValue, (unsigned long long)AfterValue);
      passed = false;
   };
   check(Before->rbx, After->rbx, "RBX");
   check(Before->rbp, After->rbp, "RBP");
   check(Before->r12, After->r12, "R12");
   check(Before->r13, After->r13, "R13");
   check(Before->r14, After->r14, "R14");
   check(Before->r15, After->r15, "R15");
   check(Before->rsp, After->rsp, "RSP");
   return passed;
}

#else

struct RegisterSnapshot { int unused; };
static constexpr bool glHasRegisterCapture = false;
static void capture_registers(RegisterSnapshot *) { }
static bool verify_registers(const RegisterSnapshot *, const RegisterSnapshot *, kt::Log &) { return true; }

#endif

static bool test_floor_register_preservation(kt::Log &Log)
{
   if constexpr (not glHasRegisterCapture) return true;
   RegisterSnapshot before, after;
   capture_registers(&before);
   volatile double result = lj_vm_floor(3.7) + lj_vm_floor(-2.3) + lj_vm_floor(0.0);
   (void)result;
   capture_registers(&after);
   return verify_registers(&before, &after, Log);
}

static bool test_ceil_register_preservation(kt::Log &Log)
{
   if constexpr (not glHasRegisterCapture) return true;
   RegisterSnapshot before, after;
   capture_registers(&before);
   volatile double result = lj_vm_ceil(3.2) + lj_vm_ceil(-2.8) + lj_vm_ceil(0.0);
   (void)result;
   capture_registers(&after);
   return verify_registers(&before, &after, Log);
}

#if LJ_HASJIT
static bool test_trunc_register_preservation(kt::Log &Log)
{
   if constexpr (not glHasRegisterCapture) return true;
   RegisterSnapshot before, after;
   capture_registers(&before);
   volatile double result = lj_vm_trunc(3.9) + lj_vm_trunc(-2.1) + lj_vm_trunc(0.0);
   (void)result;
   capture_registers(&after);
   return verify_registers(&before, &after, Log);
}
#endif

static bool test_cpuid_vendor_string(kt::Log &Log)
{
   uint32_t results[4] = { 0 };
   if (lj_vm_cpuid(0, results) IS 0) {
      Log.error("lj_vm_cpuid returned 0");
      return false;
   }
   char vendor[13];
   memcpy(vendor, &results[1], 4);
   memcpy(vendor + 4, &results[3], 4);
   memcpy(vendor + 8, &results[2], 4);
   vendor[12] = '\0';
   Log.msg("CPUID vendor: %s, max function: %u", vendor, results[0]);
   return true;
}

static bool test_cpuid_feature_flags(kt::Log &Log)
{
   uint32_t results[4] = { 0 };
   if (lj_vm_cpuid(0, results) IS 0) {
      Log.error("lj_vm_cpuid function 0 failed");
      return false;
   }
   if (results[0] < 1) return true;
   memset(results, 0, sizeof(results));
   if (lj_vm_cpuid(1, results) IS 0) {
      Log.error("lj_vm_cpuid function 1 failed");
      return false;
   }
#if LJ_TARGET_X64
   if (not (results[3] & (1 << 26))) {
      Log.error("SSE2 should be available on x64");
      return false;
   }
#endif
   return true;
}

static bool test_cpuid_register_preservation(kt::Log &Log)
{
   if constexpr (not glHasRegisterCapture) return true;
   RegisterSnapshot before, after;
   uint32_t results[4];
#if defined(HAS_CPUID_DIRECT_CAPTURE)
   asm_call_cpuid_and_capture(&before, &after, lj_vm_cpuid, results);
#else
   capture_registers(&before);
   volatile int first = lj_vm_cpuid(0, results);
   volatile int second = lj_vm_cpuid(1, results);
   (void)first;
   (void)second;
   capture_registers(&after);
#endif
   return verify_registers(&before, &after, Log);
}

static bool test_fast_function_register_preservation(kt::Log &Log)
{
   extTiri *script = nullptr;
   if (NewObject(CLASSID::TIRI, &script) != ERR::Okay) {
      Log.error("failed to create a Tiri object for VM fast-function testing");
      return false;
   }
   script->setStatement("");
   if (Action(AC::Init, script, nullptr) != ERR::Okay) {
      Log.error("failed to initialise the Tiri object for VM fast-function testing");
      FreeResource(script);
      return false;
   }

   lua_State *lua = luaL_newstate(script);
   if (not lua) {
      Log.error("failed to create a Tiri state for VM fast-function testing");
      FreeResource(script);
      return false;
   }
   luaL_openlibs(lua);

   constexpr std::string_view source =
      "return math.ldexp(1, 3), math.min(5, -2, 7), string.byte('ABC'), "
      "string.char(65), string.sub('ABCDE', 0, 3)";
   if (lua_load(lua, source, "vm-fast-function-registers")) {
      Log.error("failed to compile the VM fast-function fixture: %s", lua_tostring(lua, -1));
      lua_close(lua);
      FreeResource(script);
      return false;
   }

   RegisterSnapshot before, after;
   capture_registers(&before);
   int status = lua_pcall(lua, 0, 5, 0);
   capture_registers(&after);

   bool passed = status IS 0 and lua_tonumber(lua, -5) IS 8 and lua_tonumber(lua, -4) IS -2 and
      lua_tointeger(lua, -3) IS 65 and lua_isstring(lua, -2) and lua_isstring(lua, -1) and
      std::string_view(lua_tostring(lua, -2)) IS "A" and std::string_view(lua_tostring(lua, -1)) IS "ABC";
   if (not passed) Log.error("the VM fast-function fixture returned unexpected results");
   else passed = verify_registers(&before, &after, Log);

   lua_close(lua);
   FreeResource(script);
   return passed;
}

#if LJ_TARGET_X64
static bool test_x64_ir_assembler(kt::Log &Log)
{
   const char *failure = lj_asm_test_x64();
   if (not failure) return true;
   Log.error("%s", failure);
   return false;
}
#endif

#undef HAS_CPUID_DIRECT_CAPTURE
#undef TIRI_TEST_GNU_X64
#undef TIRI_TEST_MSVC_X64

#endif

} // namespace

extern void vm_asm_unit_tests(int &Passed, int &Total)
{
#if LJ_TARGET_X86ORX64
   constexpr std::array<TestCase, 6 + LJ_HASJIT + LJ_TARGET_X64> tests = { {
      { "floor_register_preservation", test_floor_register_preservation },
      { "ceil_register_preservation", test_ceil_register_preservation },
#if LJ_HASJIT
      { "trunc_register_preservation", test_trunc_register_preservation },
#endif
      { "cpuid_vendor_string", test_cpuid_vendor_string },
      { "cpuid_feature_flags", test_cpuid_feature_flags },
      { "cpuid_register_preservation", test_cpuid_register_preservation },
      { "fast_function_register_preservation", test_fast_function_register_preservation },
#if LJ_TARGET_X64
      { "x64_ir_assembler", test_x64_ir_assembler },
#endif
   } };
   for (const TestCase &test : tests) {
      kt::Log log("VmAsmTests");
      log.branch("Running %s", test.name);
      ++Total;
      if (test.fn(log)) {
         ++Passed;
         log.msg("%s passed", test.name);
      }
      else log.error("%s failed", test.name);
   }
#endif
}

#endif // UNIT_TESTS
