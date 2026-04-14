// Unit tests for VM assembly functions (lj_vm_floor, lj_vm_ceil, lj_vm_trunc, lj_vm_modi, lj_vm_cpuid)
// and fast string functions (string.byte, string.char, string.sub).
// Tests verify both correctness of results and register preservation according to calling conventions.

#include <kotuku/main.h>

#ifdef ENABLE_UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_vm.h"
#include "lj_arch.h"

#include "../../defs.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

static objScript *glStringTestScript = nullptr;

#if defined(_MSC_VER)
#define NOINLINE_TEST __declspec(noinline)
#elif defined(__GNUC__)
#define NOINLINE_TEST __attribute__((noinline))
#else
#define NOINLINE_TEST
#endif

namespace {

//********************************************************************************************************************
// Test infrastructure

struct TestCase {
   const char* name;
   bool (*fn)(pf::Log& Log);
};

// Helper to check if two doubles are equal (handling NaN and signed zero)
static bool doubles_equal(double A, double B)
{
   // Handle NaN: NaN != NaN, so check explicitly
   if (std::isnan(A) and std::isnan(B)) return true;

   // Handle signed zero: -0.0 == 0.0 in C++, but we want to distinguish them
   if (A == 0.0 and B == 0.0) {
      return std::signbit(A) == std::signbit(B);
   }

   return A == B;
}

// Helper to format double for logging (shows sign of zero, NaN, inf)
static const char* format_double(double Value, char* Buffer, size_t BufferSize)
{
   if (std::isnan(Value)) {
      snprintf(Buffer, BufferSize, "NaN");
   }
   else if (std::isinf(Value)) {
      snprintf(Buffer, BufferSize, "%sinf", Value < 0 ? "-" : "+");
   }
   else if (Value == 0.0) {
      snprintf(Buffer, BufferSize, "%s0.0", std::signbit(Value) ? "-" : "+");
   }
   else {
      snprintf(Buffer, BufferSize, "%.17g", Value);
   }
   return Buffer;
}

//********************************************************************************************************************
// Register preservation verification
//
// x64 Windows callee-saved: RBX, RBP, RDI, RSI, R12-R15, XMM6-XMM15
// x64 POSIX callee-saved: RBX, RBP, R12-R15
// x86 callee-saved: EBX, EBP, ESI, EDI

// Register corruption bitmask values (from asm_verify_registers)
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

#if LJ_TARGET_X86ORX64

#if defined(_MSC_VER) && defined(_M_X64)
// MSVC x64 - use external MASM assembly for register capture
// Windows x64 callee-saved: RBX, RBP, RDI, RSI, R12-R15, XMM6-XMM15

// Structure must be 16-byte aligned for XMM registers and match layout in register_capture_x64.asm
struct alignas(16) RegisterSnapshot {
   uint64_t rbx;       // offset 0
   uint64_t rbp;       // offset 8
   uint64_t rdi;       // offset 16
   uint64_t rsi;       // offset 24
   uint64_t r12;       // offset 32
   uint64_t r13;       // offset 40
   uint64_t r14;       // offset 48
   uint64_t r15;       // offset 56
   uint64_t rsp;       // offset 64
   alignas(16) uint8_t xmm6[16];   // offset 80
   alignas(16) uint8_t xmm7[16];   // offset 96
   alignas(16) uint8_t xmm8[16];   // offset 112
   alignas(16) uint8_t xmm9[16];   // offset 128
   alignas(16) uint8_t xmm10[16];  // offset 144
   alignas(16) uint8_t xmm11[16];  // offset 160
   alignas(16) uint8_t xmm12[16];  // offset 176
   alignas(16) uint8_t xmm13[16];  // offset 192
   alignas(16) uint8_t xmm14[16];  // offset 208
   alignas(16) uint8_t xmm15[16];  // offset 224
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

// External MASM functions defined in register_capture_x64.asm
extern "C" void asm_capture_registers(RegisterSnapshot* snap);
extern "C" int asm_verify_registers(const RegisterSnapshot* before, const RegisterSnapshot* after);
extern "C" int asm_call_and_capture(RegisterSnapshot* before, RegisterSnapshot* after,
   bool (*fn)(void*), void* ctx);

static constexpr bool glHasRegisterCapture = true;

static void capture_registers(RegisterSnapshot* Snap)
{
   asm_capture_registers(Snap);
}

static bool verify_registers(const RegisterSnapshot* Before, const RegisterSnapshot* After, pf::Log& Log,
   uint32_t IgnoreMask = 0)
{
   int result = asm_verify_registers(Before, After);
   result &= ~IgnoreMask;  // Clear ignored registers from the result
   if (result == 0) return true;

   // Report which registers were corrupted
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

#elif defined(__GNUC__) && defined(__x86_64__)
// GCC/Clang x64 - use inline assembly

struct RegisterSnapshot {
   uint64_t rbx;
   uint64_t rbp;
   uint64_t r12;
   uint64_t r13;
   uint64_t r14;
   uint64_t r15;
   uint64_t rsp;
};

static constexpr bool glHasRegisterCapture = true;

static void capture_registers(RegisterSnapshot* Snap)
{
   __asm__ __volatile__(
      "movq %%rbx, %0\n\t"
      "movq %%rbp, %1\n\t"
      "movq %%r12, %2\n\t"
      "movq %%r13, %3\n\t"
      "movq %%r14, %4\n\t"
      "movq %%r15, %5\n\t"
      "movq %%rsp, %6\n\t"
      : "=m"(Snap->rbx), "=m"(Snap->rbp), "=m"(Snap->r12),
        "=m"(Snap->r13), "=m"(Snap->r14), "=m"(Snap->r15),
        "=m"(Snap->rsp)
      :
      : "memory"
   );
}

static bool verify_registers(const RegisterSnapshot* Before, const RegisterSnapshot* After, pf::Log& Log,
   uint32_t IgnoreMask = 0)
{
   bool passed = true;

   if ((Before->rbx != After->rbx) and not (IgnoreMask & REG_RBX)) {
      Log.error("RBX corrupted: 0x%016llx -> 0x%016llx",
         (unsigned long long)Before->rbx, (unsigned long long)After->rbx);
      passed = false;
   }
   if ((Before->rbp != After->rbp) and not (IgnoreMask & REG_RBP)) {
      Log.error("RBP corrupted: 0x%016llx -> 0x%016llx",
         (unsigned long long)Before->rbp, (unsigned long long)After->rbp);
      passed = false;
   }
   if ((Before->r12 != After->r12) and not (IgnoreMask & REG_R12)) {
      Log.error("R12 corrupted: 0x%016llx -> 0x%016llx",
         (unsigned long long)Before->r12, (unsigned long long)After->r12);
      passed = false;
   }
   if ((Before->r13 != After->r13) and not (IgnoreMask & REG_R13)) {
      Log.error("R13 corrupted: 0x%016llx -> 0x%016llx",
         (unsigned long long)Before->r13, (unsigned long long)After->r13);
      passed = false;
   }
   if ((Before->r14 != After->r14) and not (IgnoreMask & REG_R14)) {
      Log.error("R14 corrupted: 0x%016llx -> 0x%016llx",
         (unsigned long long)Before->r14, (unsigned long long)After->r14);
      passed = false;
   }
   if ((Before->r15 != After->r15) and not (IgnoreMask & REG_R15)) {
      Log.error("R15 corrupted: 0x%016llx -> 0x%016llx",
         (unsigned long long)Before->r15, (unsigned long long)After->r15);
      passed = false;
   }
   // RSP should be restored after function call
   if ((Before->rsp != After->rsp) and not (IgnoreMask & REG_RSP)) {
      Log.error("RSP corrupted: 0x%016llx -> 0x%016llx",
         (unsigned long long)Before->rsp, (unsigned long long)After->rsp);
      passed = false;
   }

   return passed;
}

#elif defined(__GNUC__) && defined(__i386__)
// GCC/Clang x86

struct RegisterSnapshot {
   uint32_t ebx;
   uint32_t ebp;
   uint32_t esi;
   uint32_t edi;
   uint32_t esp;
};

static constexpr bool glHasRegisterCapture = true;

// Register corruption bitmask values for x86
enum RegisterBit : uint32_t {
   REG_RBX   = 1 << 0,  // EBX on x86
   REG_RBP   = 1 << 1,  // EBP on x86
   REG_RDI   = 1 << 2,  // EDI on x86
   REG_RSI   = 1 << 3,  // ESI on x86
   REG_RSP   = 1 << 8,  // ESP on x86
};

static void capture_registers(RegisterSnapshot* Snap)
{
   __asm__ __volatile__(
      "movl %%ebx, %0\n\t"
      "movl %%ebp, %1\n\t"
      "movl %%esi, %2\n\t"
      "movl %%edi, %3\n\t"
      "movl %%esp, %4\n\t"
      : "=m"(Snap->ebx), "=m"(Snap->ebp), "=m"(Snap->esi),
        "=m"(Snap->edi), "=m"(Snap->esp)
      :
      : "memory"
   );
}

static bool verify_registers(const RegisterSnapshot* Before, const RegisterSnapshot* After, pf::Log& Log,
   uint32_t IgnoreMask = 0)
{
   bool passed = true;

   if ((Before->ebx != After->ebx) and not (IgnoreMask & REG_RBX)) {
      Log.error("EBX corrupted: 0x%08x -> 0x%08x", Before->ebx, After->ebx);
      passed = false;
   }
   if ((Before->ebp != After->ebp) and not (IgnoreMask & REG_RBP)) {
      Log.error("EBP corrupted: 0x%08x -> 0x%08x", Before->ebp, After->ebp);
      passed = false;
   }
   if ((Before->esi != After->esi) and not (IgnoreMask & REG_RSI)) {
      Log.error("ESI corrupted: 0x%08x -> 0x%08x", Before->esi, After->esi);
      passed = false;
   }
   if ((Before->edi != After->edi) and not (IgnoreMask & REG_RDI)) {
      Log.error("EDI corrupted: 0x%08x -> 0x%08x", Before->edi, After->edi);
      passed = false;
   }
   if ((Before->esp != After->esp) and not (IgnoreMask & REG_RSP)) {
      Log.error("ESP corrupted: 0x%08x -> 0x%08x", Before->esp, After->esp);
      passed = false;
   }

   return passed;
}

#else
// Unknown platform

struct RegisterSnapshot { int dummy; };
static constexpr bool glHasRegisterCapture = false;
enum RegisterBit : uint32_t { REG_RBP = 1 << 1 };  // Define for compatibility
static void capture_registers(RegisterSnapshot*) {}
static bool verify_registers(const RegisterSnapshot*, const RegisterSnapshot*, pf::Log&, uint32_t = 0) { return true; }

#endif

//********************************************************************************************************************
// lj_vm_floor tests

static bool test_floor_positive_fraction(pf::Log& Log)
{
   double input = 3.7;
   double expected = 3.0;
   double result = lj_vm_floor(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("floor(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_floor_negative_fraction(pf::Log& Log)
{
   double input = -3.7;
   double expected = -4.0;
   double result = lj_vm_floor(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("floor(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_floor_positive_integer(pf::Log& Log)
{
   double input = 5.0;
   double expected = 5.0;
   double result = lj_vm_floor(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("floor(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_floor_negative_integer(pf::Log& Log)
{
   double input = -5.0;
   double expected = -5.0;
   double result = lj_vm_floor(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("floor(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_floor_positive_zero(pf::Log& Log)
{
   double input = 0.0;
   double expected = 0.0;
   double result = lj_vm_floor(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("floor(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_floor_negative_zero(pf::Log& Log)
{
   double input = -0.0;
   double expected = -0.0;
   double result = lj_vm_floor(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("floor(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_floor_large_value(pf::Log& Log)
{
   // Test value at 2^52 boundary (where IEEE754 doubles become integer-only)
   double input = 4503599627370496.5;  // 2^52 + 0.5
   double expected = 4503599627370496.0;
   double result = lj_vm_floor(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("floor(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_floor_infinity(pf::Log& Log)
{
   double pos_inf = std::numeric_limits<double>::infinity();
   double neg_inf = -std::numeric_limits<double>::infinity();

   double result_pos = lj_vm_floor(pos_inf);
   double result_neg = lj_vm_floor(neg_inf);

   if (not std::isinf(result_pos) or result_pos < 0) {
      Log.error("floor(+inf) should be +inf");
      return false;
   }
   if (not std::isinf(result_neg) or result_neg > 0) {
      Log.error("floor(-inf) should be -inf");
      return false;
   }
   return true;
}

static bool test_floor_nan(pf::Log& Log)
{
   double nan_val = std::numeric_limits<double>::quiet_NaN();
   double result = lj_vm_floor(nan_val);

   if (not std::isnan(result)) {
      char buf[64];
      Log.error("floor(NaN) = %s, expected NaN", format_double(result, buf, sizeof(buf)));
      return false;
   }
   return true;
}

static bool test_floor_register_preservation(pf::Log& Log)
{
   if constexpr (not glHasRegisterCapture) {
      Log.msg("register capture not available on this platform, skipping");
      return true;
   }

   RegisterSnapshot before, after;
   capture_registers(&before);

   // Call floor with various values to exercise the function
   volatile double result1 = lj_vm_floor(3.7);
   volatile double result2 = lj_vm_floor(-2.3);
   volatile double result3 = lj_vm_floor(0.0);
   (void)result1; (void)result2; (void)result3;

   capture_registers(&after);

   return verify_registers(&before, &after, Log);
}

//********************************************************************************************************************
// lj_vm_ceil tests

static bool test_ceil_positive_fraction(pf::Log& Log)
{
   double input = 3.2;
   double expected = 4.0;
   double result = lj_vm_ceil(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("ceil(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_ceil_negative_fraction(pf::Log& Log)
{
   double input = -3.2;
   double expected = -3.0;
   double result = lj_vm_ceil(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("ceil(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_ceil_positive_integer(pf::Log& Log)
{
   double input = 5.0;
   double expected = 5.0;
   double result = lj_vm_ceil(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("ceil(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_ceil_negative_integer(pf::Log& Log)
{
   double input = -5.0;
   double expected = -5.0;
   double result = lj_vm_ceil(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("ceil(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_ceil_negative_zero(pf::Log& Log)
{
   double input = -0.0;
   double expected = -0.0;  // ceil(-0.0) should preserve -0.0
   double result = lj_vm_ceil(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("ceil(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_ceil_register_preservation(pf::Log& Log)
{
   if constexpr (not glHasRegisterCapture) {
      Log.msg("register capture not available on this platform, skipping");
      return true;
   }

   RegisterSnapshot before, after;
   capture_registers(&before);

   volatile double result1 = lj_vm_ceil(3.2);
   volatile double result2 = lj_vm_ceil(-2.8);
   volatile double result3 = lj_vm_ceil(0.0);
   (void)result1; (void)result2; (void)result3;

   capture_registers(&after);

   return verify_registers(&before, &after, Log);
}

//********************************************************************************************************************
// lj_vm_trunc tests (only available when JIT is enabled)

#if LJ_HASJIT

static bool test_trunc_positive_fraction(pf::Log& Log)
{
   double input = 3.9;
   double expected = 3.0;
   double result = lj_vm_trunc(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("trunc(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_trunc_negative_fraction(pf::Log& Log)
{
   double input = -3.9;
   double expected = -3.0;  // trunc rounds toward zero
   double result = lj_vm_trunc(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("trunc(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_trunc_negative_zero(pf::Log& Log)
{
   double input = -0.0;
   double expected = -0.0;
   double result = lj_vm_trunc(input);

   if (not doubles_equal(result, expected)) {
      char buf1[64], buf2[64];
      Log.error("trunc(%s) = %s, expected %s",
         format_double(input, buf1, sizeof(buf1)),
         format_double(result, buf2, sizeof(buf2)),
         format_double(expected, buf1, sizeof(buf1)));
      return false;
   }
   return true;
}

static bool test_trunc_register_preservation(pf::Log& Log)
{
   if constexpr (not glHasRegisterCapture) {
      Log.msg("register capture not available on this platform, skipping");
      return true;
   }

   RegisterSnapshot before, after;
   capture_registers(&before);

   volatile double result1 = lj_vm_trunc(3.9);
   volatile double result2 = lj_vm_trunc(-2.1);
   volatile double result3 = lj_vm_trunc(0.0);
   (void)result1; (void)result2; (void)result3;

   capture_registers(&after);

   return verify_registers(&before, &after, Log);
}

#endif // LJ_HASJIT

//********************************************************************************************************************
// lj_vm_modi tests (integer modulo with Lua semantics)

#if LJ_HASJIT && !(LJ_TARGET_ARM || LJ_TARGET_ARM64 || LJ_TARGET_PPC)

static bool test_modi_positive_positive(pf::Log& Log)
{
   int32_t a = 17;
   int32_t b = 5;
   int32_t expected = 2;  // 17 % 5 = 2
   int32_t result = lj_vm_modi(a, b);

   if (result != expected) {
      Log.error("modi(%d, %d) = %d, expected %d", a, b, result, expected);
      return false;
   }
   return true;
}

static bool test_modi_negative_positive(pf::Log& Log)
{
   // Lua modulo: result has same sign as divisor
   int32_t a = -17;
   int32_t b = 5;
   int32_t expected = 3;  // -17 % 5 = 3 in Lua (not -2 as in C)
   int32_t result = lj_vm_modi(a, b);

   if (result != expected) {
      Log.error("modi(%d, %d) = %d, expected %d", a, b, result, expected);
      return false;
   }
   return true;
}

static bool test_modi_positive_negative(pf::Log& Log)
{
   // Lua modulo: result has same sign as divisor
   int32_t a = 17;
   int32_t b = -5;
   int32_t expected = -3;  // 17 % -5 = -3 in Lua (not 2 as in C)
   int32_t result = lj_vm_modi(a, b);

   if (result != expected) {
      Log.error("modi(%d, %d) = %d, expected %d", a, b, result, expected);
      return false;
   }
   return true;
}

static bool test_modi_negative_negative(pf::Log& Log)
{
   int32_t a = -17;
   int32_t b = -5;
   int32_t expected = -2;  // -17 % -5 = -2
   int32_t result = lj_vm_modi(a, b);

   if (result != expected) {
      Log.error("modi(%d, %d) = %d, expected %d", a, b, result, expected);
      return false;
   }
   return true;
}

static bool test_modi_zero_dividend(pf::Log& Log)
{
   int32_t a = 0;
   int32_t b = 5;
   int32_t expected = 0;
   int32_t result = lj_vm_modi(a, b);

   if (result != expected) {
      Log.error("modi(%d, %d) = %d, expected %d", a, b, result, expected);
      return false;
   }
   return true;
}

static bool test_modi_exact_divisor(pf::Log& Log)
{
   int32_t a = 15;
   int32_t b = 5;
   int32_t expected = 0;
   int32_t result = lj_vm_modi(a, b);

   if (result != expected) {
      Log.error("modi(%d, %d) = %d, expected %d", a, b, result, expected);
      return false;
   }
   return true;
}

#endif // LJ_HASJIT && !(LJ_TARGET_ARM || LJ_TARGET_ARM64 || LJ_TARGET_PPC)

//********************************************************************************************************************
// lj_vm_cpuid tests (x86/x64 only)

static bool test_cpuid_vendor_string(pf::Log& Log)
{
   uint32_t res[4] = {0};

   // CPUID function 0 returns vendor string
   int ret = lj_vm_cpuid(0, res);

   if (ret == 0) {
      Log.error("lj_vm_cpuid returned 0 (CPUID not supported)");
      return false;
   }

   // res[0] = max function, res[1..3] = vendor string (EBX, EDX, ECX)
   char vendor[13];
   memcpy(vendor, &res[1], 4);      // EBX
   memcpy(vendor + 4, &res[3], 4);  // EDX
   memcpy(vendor + 8, &res[2], 4);  // ECX
   vendor[12] = '\0';

   Log.msg("CPUID vendor: %s, max function: %u", vendor, res[0]);

   // Check for known vendors
   if (strncmp(vendor, "GenuineIntel", 12) != 0 and
       strncmp(vendor, "AuthenticAMD", 12) != 0 and
       strncmp(vendor, "VIA VIA VIA ", 12) != 0 and
       strncmp(vendor, "HygonGenuine", 12) != 0) {
      // Unknown vendor - might be a VM or unusual CPU, just warn
      Log.msg("warning: unknown CPU vendor '%s'", vendor);
   }

   return true;
}

static bool test_cpuid_feature_flags(pf::Log& Log)
{
   uint32_t res[4] = {0};

   // First check max function
   if (lj_vm_cpuid(0, res) == 0) {
      Log.error("lj_vm_cpuid function 0 failed");
      return false;
   }

   if (res[0] < 1) {
      Log.msg("CPUID function 1 not supported, skipping feature flag test");
      return true;
   }

   // CPUID function 1 returns feature flags
   memset(res, 0, sizeof(res));
   if (lj_vm_cpuid(1, res) == 0) {
      Log.error("lj_vm_cpuid function 1 failed");
      return false;
   }

   // res[2] = ECX features, res[3] = EDX features
   bool has_sse2 = (res[3] & (1 << 26)) != 0;
   bool has_sse3 = (res[2] & (1 << 0)) != 0;
   bool has_sse41 = (res[2] & (1 << 19)) != 0;

   Log.msg("CPU features: SSE2=%d SSE3=%d SSE4.1=%d", has_sse2, has_sse3, has_sse41);

   // x64 requires SSE2
#if LJ_TARGET_X64
   if (not has_sse2) {
      Log.error("SSE2 should be available on x64");
      return false;
   }
#endif

   return true;
}

static bool test_cpuid_register_preservation(pf::Log& Log)
{
   if constexpr (not glHasRegisterCapture) {
      Log.msg("register capture not available on this platform, skipping");
      return true;
   }

   RegisterSnapshot before, after;
   capture_registers(&before);

   uint32_t res[4];
   volatile int ret1 = lj_vm_cpuid(0, res);
   volatile int ret2 = lj_vm_cpuid(1, res);
   (void)ret1; (void)ret2;

   capture_registers(&after);

   return verify_registers(&before, &after, Log);
}

#endif // LJ_TARGET_X86ORX64

//********************************************************************************************************************
// String function assembly tests
//
// These test the fast assembly implementations of string functions in vm_x64.dasc.
// The assembly fast-functions handle specific cases:
//   - string.byte: 1-arg case only (returns first character's byte value)
//   - string.char: 1-arg case only (values 0-255)
//   - string.sub: 2-3 arg cases with numeric indices
//
// Multi-arg cases and edge cases fall back to C implementations.
// These tests focus on the assembly paths and verify register preservation.

struct LuaStateHolder {
   LuaStateHolder()
   {
      this->state = luaL_newstate(glStringTestScript);
   }

   ~LuaStateHolder()
   {
      if (this->state) {
         lua_close(this->state);
      }
   }

   lua_State* get() const { return this->state; }

private:
   lua_State* state = nullptr;
};

// Execute Lua code and check result
static NOINLINE_TEST bool run_lua_test(lua_State* L, std::string_view Code, std::string& Error)
{
   if (lua_load(L, Code, "string-test")) {
      Error = lua_tostring(L, -1);
      lua_pop(L, 1);
      return false;
   }
   if (lua_pcall(L, 0, LUA_MULTRET, 0)) {
      Error = lua_tostring(L, -1);
      lua_pop(L, 1);
      return false;
   }
   return true;
}

struct LuaTestCallContext {
   lua_State* state;
   std::string_view code;
   std::string* error;
};

static bool run_lua_test_ctx(void* Context)
{
   LuaTestCallContext* ctx = static_cast<LuaTestCallContext*>(Context);
   return run_lua_test(ctx->state, ctx->code, *ctx->error);
}

#if defined(_MSC_VER) && defined(_M_X64)
#pragma optimize("", off)
static NOINLINE_TEST bool call_and_capture(RegisterSnapshot& Before, RegisterSnapshot& After,
   bool (*Fn)(void*), void* Context)
{
   volatile RegisterSnapshot* before_ptr = &Before;
   volatile RegisterSnapshot* after_ptr = &After;
   capture_registers(const_cast<RegisterSnapshot*>(before_ptr));
   bool result = Fn(Context);
   capture_registers(const_cast<RegisterSnapshot*>(after_ptr));
   return result;
}
#pragma optimize("", on)
#else
static bool call_and_capture(RegisterSnapshot& Before, RegisterSnapshot& After, bool (*Fn)(void*),
   void* Context)
{
   capture_registers(&Before);
   bool result = Fn(Context);
   capture_registers(&After);
   return result;
}
#endif

static bool run_lua_test_with_capture(RegisterSnapshot& Before, RegisterSnapshot& After, lua_State* L,
   std::string_view Code, std::string& Error)
{
   LuaTestCallContext context{ L, Code, &Error };
   return call_and_capture(Before, After, run_lua_test_ctx, &context);
}

//********************************************************************************************************************
// string.byte assembly tests
// The assembly fast-path handles ONLY the 1-arg case (no position arguments)
// See vm_x64.dasc line ~1882: .ffunc string_byte

// Tests the assembly fast-path: string.byte with 1 arg returns first char byte
static bool test_asm_string_byte_first_char(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // 1-arg case: uses assembly fast-path
   if (not run_lua_test_with_capture(before, after, L, "return string.byte('ABC')", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   // Ignore RBP/RDI/RSI - interpreter uses them as persistent VM registers
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) {
      Log.error("register corruption detected in string.byte assembly");
      return false;
   }
#endif

   lua_Number Result = lua_tonumber(L, -1);
   if (Result IS 65) return true;  // 'A' = 65

   Log.error("expected 65, got %g", Result);
   return false;
}

// Tests assembly handling of empty string (should return no results)
static bool test_asm_string_byte_empty_string(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Empty string with 1 arg: assembly checks len < 1 and returns 0 results
   if (not run_lua_test_with_capture(before, after, L, "return string.byte('')", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) {
      Log.error("register corruption detected in string.byte assembly");
      return false;
   }
#endif

   if (lua_gettop(L) IS 0 or lua_isnil(L, -1)) return true;

   Log.error("expected nil/no result for empty string");
   return false;
}

// Tests single-byte string (boundary case)
static bool test_asm_string_byte_single_byte(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   if (not run_lua_test_with_capture(before, after, L, "return string.byte('X')", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   lua_Number Result = lua_tonumber(L, -1);
   if (Result IS 88) return true;  // 'X' = 88

   Log.error("expected 88, got %g", Result);
   return false;
}

// Tests high byte value (255)
static bool test_asm_string_byte_high_value(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Create string with byte 255 and read it back
   if (not run_lua_test_with_capture(before, after, L, "return string.byte(string.char(255))", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   lua_Number Result = lua_tonumber(L, -1);
   if (Result IS 255) return true;

   Log.error("expected 255, got %g", Result);
   return false;
}

// Tests null byte (0)
static bool test_asm_string_byte_null_byte(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   if (not run_lua_test_with_capture(before, after, L, "return string.byte(string.char(0))", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   lua_Number Result = lua_tonumber(L, -1);
   if (Result IS 0) return true;

   Log.error("expected 0, got %g", Result);
   return false;
}

//********************************************************************************************************************
// string.char assembly tests
// The assembly fast-path handles ONLY the 1-arg case with value 0-255
// See vm_x64.dasc line ~1896: .ffunc string_char

// Tests assembly fast-path: single char in valid range
static bool test_asm_string_char_single(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // 1-arg case with valid value: uses assembly fast-path
   if (not run_lua_test_with_capture(before, after, L, "return string.char(65)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) {
      Log.error("register corruption detected in string.char assembly");
      return false;
   }
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("A")) return true;

   Log.error("expected 'A', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests boundary value 0 (null byte)
static bool test_asm_string_char_zero(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   if (not run_lua_test_with_capture(before, after, L, "return #string.char(0)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   lua_Number Len = lua_tonumber(L, -1);
   if (Len IS 1) return true;

   Log.error("expected length 1, got %g", Len);
   return false;
}

// Tests boundary value 255 (max byte)
static bool test_asm_string_char_max(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // 255 is max valid value for assembly path
   if (not run_lua_test_with_capture(before, after, L, "return string.byte(string.char(255))", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   lua_Number Result = lua_tonumber(L, -1);
   if (Result IS 255) return true;

   Log.error("expected 255, got %g", Result);
   return false;
}

// Tests value just below boundary (254)
static bool test_asm_string_char_254(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   if (not run_lua_test_with_capture(before, after, L, "return string.byte(string.char(254))", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   lua_Number Result = lua_tonumber(L, -1);
   if (Result IS 254) return true;

   Log.error("expected 254, got %g", Result);
   return false;
}

//********************************************************************************************************************
// string.sub assembly tests
// The assembly fast-path handles 2-3 arg cases with numeric indices
// See vm_x64.dasc line ~1926: .ffunc string_sub

// Tests basic substring (assembly path)
static bool test_asm_string_sub_basic(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // 3-arg case: uses assembly fast-path
   // Note: end parameter is exclusive for positive values
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', 1, 4)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) {
      Log.error("register corruption detected in string.sub assembly");
      return false;
   }
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("BCD")) return true;

   Log.error("expected 'BCD', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests empty string input (assembly handles len==0 case)
static bool test_asm_string_sub_empty_input(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Empty string: assembly checks len==0 and jumps to fff_emptystr
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('', 0, 10)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("")) return true;

   Log.error("expected empty string, got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests negative start index (assembly handles negative index conversion)
static bool test_asm_string_sub_negative_start(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Negative start: assembly converts via label 7
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', -3)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("CDE")) return true;

   Log.error("expected 'CDE', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests negative end index (assembly handles via label 5)
static bool test_asm_string_sub_negative_end(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Negative end: assembly converts via label 5
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', 0, -2)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("ABCD")) return true;

   Log.error("expected 'ABCD', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests end > length (assembly handles overflow via label 6)
static bool test_asm_string_sub_end_overflow(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // End > length: assembly clamps via label 6
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABC', 0, 100)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("ABC")) return true;

   Log.error("expected 'ABC', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests start > end (assembly handles via fff_emptystr)
static bool test_asm_string_sub_empty_result(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Start > end: assembly returns empty string
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', 3, 1)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("")) return true;

   Log.error("expected empty string, got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests single character extraction
static bool test_asm_string_sub_single_char(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Note: end parameter is exclusive for positive values, so 2,3 extracts char at index 2
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', 2, 3)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("C")) return true;

   Log.error("expected 'C', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests 2-arg form (start to end of string)
static bool test_asm_string_sub_to_end(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // 2-arg case: uses -1 as default end
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', 2)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("CDE")) return true;

   Log.error("expected 'CDE', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests both indices negative
static bool test_asm_string_sub_both_negative(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', -4, -2)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("BCD")) return true;

   Log.error("expected 'BCD', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests start = 0 (first character, 0-based)
static bool test_asm_string_sub_from_zero(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Note: end parameter is exclusive for positive values
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', 0, 3)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("ABC")) return true;

   Log.error("expected 'ABC', got '%s'", Result ? Result : "(nil)");
   return false;
}

// Tests underflow (start < -len, should clamp to 0)
static bool test_asm_string_sub_start_underflow(pf::Log& Log)
{
   LuaStateHolder Holder;
   lua_State* L = Holder.get();
   if (not L) { Log.error("failed to create Lua state"); return false; }
   luaL_openlibs(L);

#if LJ_TARGET_X86ORX64
   RegisterSnapshot before, after;
#endif

   std::string Error;
   // Start -100 on 5-char string: assembly clamps via label 8
   // Note: end parameter is exclusive for positive values
   if (not run_lua_test_with_capture(before, after, L, "return string.sub('ABCDE', -100, 3)", Error)) {
      Log.error("test failed: %s", Error.c_str());
      return false;
   }

#if LJ_TARGET_X86ORX64
   if (not verify_registers(&before, &after, Log, REG_RBP | REG_RDI | REG_RSI)) return false;
#endif

   const char* Result = lua_tostring(L, -1);
   if (Result and std::string_view(Result) IS std::string_view("ABC")) return true;

   Log.error("expected 'ABC', got '%s'", Result ? Result : "(nil)");
   return false;
}

}  // namespace

//********************************************************************************************************************
// Test runner

extern void vm_asm_unit_tests(int &Passed, int &Total)
{
#if LJ_TARGET_X86ORX64
   constexpr std::array<TestCase, 23> Tests = { {
      // lj_vm_floor tests
      { "floor_positive_fraction", test_floor_positive_fraction },
      { "floor_negative_fraction", test_floor_negative_fraction },
      { "floor_positive_integer", test_floor_positive_integer },
      { "floor_negative_integer", test_floor_negative_integer },
      { "floor_positive_zero", test_floor_positive_zero },
      { "floor_negative_zero", test_floor_negative_zero },
      { "floor_large_value", test_floor_large_value },
      { "floor_infinity", test_floor_infinity },
      { "floor_nan", test_floor_nan },
      { "floor_register_preservation", test_floor_register_preservation },

      // lj_vm_ceil tests
      { "ceil_positive_fraction", test_ceil_positive_fraction },
      { "ceil_negative_fraction", test_ceil_negative_fraction },
      { "ceil_positive_integer", test_ceil_positive_integer },
      { "ceil_negative_integer", test_ceil_negative_integer },
      { "ceil_negative_zero", test_ceil_negative_zero },
      { "ceil_register_preservation", test_ceil_register_preservation },

#if LJ_HASJIT
      // lj_vm_trunc tests
      { "trunc_positive_fraction", test_trunc_positive_fraction },
      { "trunc_negative_fraction", test_trunc_negative_fraction },
      { "trunc_negative_zero", test_trunc_negative_zero },
      { "trunc_register_preservation", test_trunc_register_preservation },
#endif

      // lj_vm_cpuid tests
      { "cpuid_vendor_string", test_cpuid_vendor_string },
      { "cpuid_feature_flags", test_cpuid_feature_flags },
      { "cpuid_register_preservation", test_cpuid_register_preservation },
   } };

   for (const TestCase& Test : Tests) {
      pf::Log Log("VmAsmTests");
      Log.branch("Running %s", Test.name);
      ++Total;
      if (Test.fn(Log)) {
         ++Passed;
         Log.msg("%s passed", Test.name);
      }
      else {
         Log.error("%s failed", Test.name);
      }
   }

#if !(LJ_TARGET_ARM || LJ_TARGET_ARM64 || LJ_TARGET_PPC)
   // lj_vm_modi tests (separate array due to conditional compilation)
   constexpr std::array<TestCase, 6> ModiTests = { {
      { "modi_positive_positive", test_modi_positive_positive },
      { "modi_negative_positive", test_modi_negative_positive },
      { "modi_positive_negative", test_modi_positive_negative },
      { "modi_negative_negative", test_modi_negative_negative },
      { "modi_zero_dividend", test_modi_zero_dividend },
      { "modi_exact_divisor", test_modi_exact_divisor },
   } };

   for (const TestCase& Test : ModiTests) {
      pf::Log Log("VmAsmTests");
      Log.branch("Running %s", Test.name);
      ++Total;
      if (Test.fn(Log)) {
         ++Passed;
         Log.msg("%s passed", Test.name);
      }
      else {
         Log.error("%s failed", Test.name);
      }
   }
#endif

#else
   // Non-x86/x64 platforms
   pf::Log Log("VmAsmTests");
   Log.msg("VM assembly tests only available on x86/x64 platforms");
#endif

   // String function assembly tests (run on all platforms)
   // These test the fast assembly implementations with register preservation checks
   if (NewObject(CLASSID::TIRI, &glStringTestScript) IS ERR::Okay) {
      glStringTestScript->setStatement("");
      if (Action(AC::Init, glStringTestScript, nullptr) IS ERR::Okay) {
         constexpr std::array<TestCase, 20> StringAsmTests = { {
            // string.byte assembly tests (1-arg fast path)
            { "asm_string_byte_first_char", test_asm_string_byte_first_char },
            { "asm_string_byte_empty_string", test_asm_string_byte_empty_string },
            { "asm_string_byte_single_byte", test_asm_string_byte_single_byte },
            { "asm_string_byte_high_value", test_asm_string_byte_high_value },
            { "asm_string_byte_null_byte", test_asm_string_byte_null_byte },

            // string.char assembly tests (1-arg fast path, 0-255)
            { "asm_string_char_single", test_asm_string_char_single },
            { "asm_string_char_zero", test_asm_string_char_zero },
            { "asm_string_char_max", test_asm_string_char_max },
            { "asm_string_char_254", test_asm_string_char_254 },

            // string.sub assembly tests (2-3 arg fast path)
            { "asm_string_sub_basic", test_asm_string_sub_basic },
            { "asm_string_sub_empty_input", test_asm_string_sub_empty_input },
            { "asm_string_sub_negative_start", test_asm_string_sub_negative_start },
            { "asm_string_sub_negative_end", test_asm_string_sub_negative_end },
            { "asm_string_sub_end_overflow", test_asm_string_sub_end_overflow },
            { "asm_string_sub_empty_result", test_asm_string_sub_empty_result },
            { "asm_string_sub_single_char", test_asm_string_sub_single_char },
            { "asm_string_sub_to_end", test_asm_string_sub_to_end },
            { "asm_string_sub_both_negative", test_asm_string_sub_both_negative },
            { "asm_string_sub_from_zero", test_asm_string_sub_from_zero },
            { "asm_string_sub_start_underflow", test_asm_string_sub_start_underflow },
         } };

         for (const TestCase& Test : StringAsmTests) {
            pf::Log Log("VmAsmTests");
            Log.branch("Running %s", Test.name);
            ++Total;
            if (Test.fn(Log)) {
               ++Passed;
               Log.msg("%s passed", Test.name);
            }
            else {
               Log.error("%s failed", Test.name);
            }
         }
      }
      FreeResource(glStringTestScript);
      glStringTestScript = nullptr;
   }
}

#endif // ENABLE_UNIT_TESTS
