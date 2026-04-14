// LuaJIT common internal definitions.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#pragma once

#include "lua.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>

// Various VM limits.
constexpr uint32_t LJ_MAX_MEM32 = 0x7fffff00u;   //  Max. 32 bit memory allocation.
constexpr uint64_t LJ_MAX_MEM64 = ((uint64_t)1 << 47);  //  Max. 64 bit memory allocation.
// Max. total memory allocation - always 64-bit.
#define LJ_MAX_MEM   LJ_MAX_MEM64
#define LJ_MAX_ALLOC LJ_MAX_MEM   //  Max. individual allocation length.
#define LJ_MAX_STR   LJ_MAX_MEM32   //  Max. string length.
#define LJ_MAX_BUF   LJ_MAX_MEM32   //  Max. buffer length.
#define LJ_MAX_UDATA LJ_MAX_MEM32   //  Max. userdata length.

constexpr uint32_t LJ_MAX_STRTAB = (1u << 26);      //  Max. string table size.
constexpr int LJ_MAX_HBITS = 26;      //  Max. hash bits.
constexpr int LJ_MAX_ABITS = 28;      //  Max. bits of array key.
constexpr uint32_t LJ_MAX_ASIZE = ((1u << (LJ_MAX_ABITS - 1)) + 1);  //  Max. array part size.
constexpr int LJ_MAX_COLOSIZE = 16;      //  Max. elems for colocated array.

#define LJ_MAX_LINE   LJ_MAX_MEM32     //  Max. source code line number.
constexpr int LJ_MAX_XLEVEL = 200;     //  Max. syntactic nesting level.
constexpr uint32_t LJ_MAX_BCINS = (1u << 26); //  Max. # of bytecode instructions.
constexpr int LJ_MAX_SLOTS = 250;      //  Max. # of slots in a Lua func.
constexpr int LJ_MAX_LOCVAR = 200;     //  Max. # of local variables.
constexpr int LJ_MAX_UPVAL = 60;       //  Max. # of upvalues.

constexpr int LJ_MAX_IDXCHAIN = 100; //  __index/__newindex chain limit.
#define LJ_STACK_EXTRA   (5+2)       //  Extra stack space (metamethods).

constexpr int LJ_NUM_CBPAGE = 1;      //  Number of FFI callback pages.

// Minimum table/buffer sizes.
constexpr int LJ_MIN_GLOBAL = 6;     // Min. global table size (hbits).
constexpr int LJ_MIN_REGISTRY = 2;   // Min. registry size (hbits).
constexpr int LJ_MIN_STRTAB = 256;   // Min. string table size (pow2).
constexpr int LJ_MIN_SBUF = 32;      // Min. string buffer length.
constexpr int LJ_MIN_VECSZ = 8;      // Min. size for growable vectors.
constexpr int LJ_MIN_IRSZ = 32;      // Min. size for growable IR.

// JIT compiler limits.
constexpr int LJ_MAX_JSLOTS = 250;    // Max. # of stack slots for a trace.
constexpr int LJ_MAX_PHI = 64;        // Max. # of PHIs for a loop.
constexpr int LJ_MAX_EXITSTUBGR = 16; // Max. # of exit stub groups.

// Mark unused parameters/variables to suppress compiler warnings.
// Prefer [[maybe_unused]] attribute directly where possible.
#ifndef UNUSED
#define UNUSED(x)   ((void)(x))
#endif

// Utility macro for constructing 64-bit constants from hex values.
#define U64x(hi, lo)   (((uint64_t)0x##hi << 32) + (uint64_t)0x##lo)

// Pointer cast inline functions.
template<typename T>
inline constexpr int32_t i32ptr(T p) { return (int32_t)(intptr_t)(void *)(p); }

template<typename T>
inline constexpr uint32_t u32ptr(T p) { return (uint32_t)(intptr_t)(void *)(p); }

template<typename T>
inline constexpr int64_t i64ptr(T p) { return (int64_t)(intptr_t)(void *)(p); }

template<typename T>
inline constexpr uint64_t u64ptr(T p) { return (uint64_t)(intptr_t)(void *)(p); }

// Always use 64-bit pointers for GC references
#define igcptr(p)   i64ptr(p)

// Type check inline functions.
template<typename T>
inline constexpr bool checki8(T x) { return x IS (int32_t)(int8_t)(x); }

template<typename T>
inline constexpr bool checku8(T x) { return x IS (int32_t)(uint8_t)(x); }

template<typename T>
inline constexpr bool checki16(T x) { return x IS (int32_t)(int16_t)(x); }

template<typename T>
inline constexpr bool checku16(T x) { return x IS (int32_t)(uint16_t)(x); }

template<typename T>
inline constexpr bool checki32(T x) { return x IS (int32_t)(x); }

template<typename T>
inline constexpr bool checku32(T x) { return x IS (uint32_t)(x); }

template<typename T>
inline constexpr bool checkptr47(T x) { return ((uint64_t)(uintptr_t)(x) >> 47) IS 0; }

// 47-bit pointer check for GC64 mode (128 TB address space)
#define checkptrGC(x)   checkptr47((x))

// Rotate inline functions - compilers transform this into rotate instructions.
template<typename T>
inline constexpr T lj_rol(T x, int n) {
   return (x << n) | (x >> (-(int)(n) & (8 * sizeof(x) - 1)));
}

template<typename T>
inline constexpr T lj_ror(T x, int n) {
   return (x << (-(int)(n) & (8 * sizeof(x) - 1))) | (x >> n);
}

// A really naive Bloom filter. But sufficient for our needs.
typedef uintptr_t BloomFilter;
constexpr size_t BLOOM_MASK = (8 * sizeof(BloomFilter) - 1);

inline constexpr uintptr_t bloombit(uintptr_t x) {
   return (uintptr_t)1 << (x & BLOOM_MASK);
}

inline void bloomset(BloomFilter& b, uintptr_t x) {
   b |= bloombit(x);
}

inline constexpr uintptr_t bloomtest(BloomFilter b, uintptr_t x) {
   return b & bloombit(x);
}

#if defined(__GNUC__) || defined(__clang__)

#define LJ_NORET   [[noreturn]]
#define LJ_ALIGN(n)   __attribute__((aligned(n)))
#define LJ_INLINE   inline
#define LJ_AINLINE   inline __attribute__((always_inline))
#define LJ_NOINLINE   __attribute__((noinline))

#if defined(__ELF__) || defined(__MACH__) || defined(__psp2__)
#if !((defined(__sun__) && defined(__svr4__)) || defined(__CELLOS_LV2__))
#ifdef __cplusplus
#define LJ_NOAPI   extern "C" __attribute__((visibility("hidden")))
#else
#define LJ_NOAPI   extern __attribute__((visibility("hidden")))
#endif
#endif
#endif

/* Note: fastcall was only used on 32-bit x86, no longer needed for 64-bit. */

#define LJ_LIKELY(x)   __builtin_expect(!!(x), 1)
#define LJ_UNLIKELY(x)   __builtin_expect(!!(x), 0)

#define lj_ffs(x)   ((uint32_t)__builtin_ctz(x))
// Don't ask ...
#if defined(__INTEL_COMPILER) && (defined(__i386__) || defined(__x86_64__))
static LJ_AINLINE uint32_t lj_fls(uint32_t x)
{
   uint32_t r; __asm__("bsrl %1, %0" : "=r" (r) : "rm" (x) : "cc"); return r;
}
#else
#define lj_fls(x)   ((uint32_t)(__builtin_clz(x)^31))
#endif

#if defined(__arm__)
static LJ_AINLINE uint32_t lj_bswap(uint32_t x)
{
   uint32_t r;
#if __ARM_ARCH_6__ || __ARM_ARCH_6J__ || __ARM_ARCH_6T2__ || __ARM_ARCH_6Z__ ||\
    __ARM_ARCH_6ZK__ || __ARM_ARCH_7__ || __ARM_ARCH_7A__ || __ARM_ARCH_7R__
   __asm__("rev %0, %1" : "=r" (r) : "r" (x));
   return r;
#else
#ifdef __thumb__
   r = x ^ lj_ror(x, 16);
#else
   __asm__("eor %0, %1, %1, ror #16" : "=r" (r) : "r" (x));
#endif
   return ((r & 0xff00ffffu) >> 8) ^ lj_ror(x, 8);
#endif
}

static LJ_AINLINE uint64_t lj_bswap64(uint64_t x)
{
   return ((uint64_t)lj_bswap((uint32_t)x) << 32) | lj_bswap((uint32_t)(x >> 32));
}
#elif (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3) || __clang__
static LJ_AINLINE uint32_t lj_bswap(uint32_t x)
{
   return (uint32_t)__builtin_bswap32((int32_t)x);
}

static LJ_AINLINE uint64_t lj_bswap64(uint64_t x)
{
   return (uint64_t)__builtin_bswap64((int64_t)x);
}
#elif defined(__x86_64__)
static LJ_AINLINE uint32_t lj_bswap(uint32_t x)
{
   uint32_t r; __asm__("bswap %0" : "=r" (r) : "0" (x)); return r;
}

static LJ_AINLINE uint64_t lj_bswap64(uint64_t x)
{
   uint64_t r; __asm__("bswap %0" : "=r" (r) : "0" (x)); return r;
}
#else
static LJ_AINLINE uint32_t lj_bswap(uint32_t x)
{
   return (x << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) | (x >> 24);
}

static LJ_AINLINE uint64_t lj_bswap64(uint64_t x)
{
   return (uint64_t)lj_bswap((uint32_t)(x >> 32)) |
      ((uint64_t)lj_bswap((uint32_t)x) << 32);
}
#endif

typedef union __attribute__((packed)) Unaligned16 {
   uint16_t u;
   uint8_t b[2];
} Unaligned16;

typedef union __attribute__((packed)) Unaligned32 {
   uint32_t u;
   uint8_t b[4];
} Unaligned32;

// Unaligned load of uint16_t.
static LJ_AINLINE uint16_t lj_getu16(const void* p)
{
   return ((const Unaligned16*)p)->u;
}

// Unaligned load of uint32_t.
static LJ_AINLINE uint32_t lj_getu32(const void* p)
{
   return ((const Unaligned32*)p)->u;
}

#elif defined(_MSC_VER)

#define LJ_NORET   [[noreturn]]
#define LJ_ALIGN(n)   __declspec(align(n))
#define LJ_INLINE   inline
#define LJ_AINLINE   __forceinline
#define LJ_NOINLINE   __declspec(noinline)

#ifdef _M_PPC
unsigned int _CountLeadingZeros(long);
#pragma intrinsic(_CountLeadingZeros)
static LJ_AINLINE uint32_t lj_fls(uint32_t x)
{
   return _CountLeadingZeros(x) ^ 31;
}
#else
unsigned char _BitScanForward(unsigned long*, unsigned long);
unsigned char _BitScanReverse(unsigned long*, unsigned long);
#pragma intrinsic(_BitScanForward)
#pragma intrinsic(_BitScanReverse)

inline uint32_t lj_ffs(uint32_t x) { unsigned long r; _BitScanForward(&r, x); return (uint32_t)r; }
inline uint32_t lj_fls(uint32_t x) { unsigned long r; _BitScanReverse(&r, x); return (uint32_t)r; }
#endif

unsigned long _byteswap_ulong(unsigned long);
uint64_t _byteswap_uint64(uint64_t);
#define lj_bswap(x)   (_byteswap_ulong((x)))
#define lj_bswap64(x)   (_byteswap_uint64((x)))

#if defined(_M_PPC) && defined(LUAJIT_NO_UNALIGNED)
/*
** Replacement for unaligned loads on Xbox 360. Disabled by default since it's
** usually more costly than the occasional stall when crossing a cache-line.
*/
static LJ_AINLINE uint16_t lj_getu16(const void* v)
{
   const uint8_t* p = (const uint8_t*)v;
   return (uint16_t)((p[0] << 8) | p[1]);
}
static LJ_AINLINE uint32_t lj_getu32(const void* v)
{
   const uint8_t* p = (const uint8_t*)v;
   return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}
#else
// Unaligned loads are generally ok on x86/x64.
#define lj_getu16(p)   (*(uint16_t *)(p))
#define lj_getu32(p)   (*(uint32_t *)(p))
#endif

#else
#error "missing defines for your compiler"
#endif

#ifndef LJ_NORET
#define LJ_NORET   [[noreturn]]
#endif
#ifndef LJ_NOAPI
#ifdef __cplusplus
#define LJ_NOAPI   extern "C"
#else
#define LJ_NOAPI   extern
#endif
#endif
#ifndef LJ_LIKELY
#define LJ_LIKELY(x)   (x)
#define LJ_UNLIKELY(x)   (x)
#endif

// Attributes for internal functions.
#define LJ_DATA      LJ_NOAPI
#define LJ_DATADEF
#define LJ_ASMF      LJ_NOAPI
#define LJ_FUNCA   LJ_NOAPI
#if defined(ljamalg_c)
#define LJ_FUNC      static
#define LJ_CXXFUNC   static
#else
#define LJ_FUNC      LJ_NOAPI
#define LJ_CXXFUNC   extern  // C++ linkage for functions returning C++ types
#endif
#if defined(__GNUC__) || defined(__clang__)
#define LJ_USED   __attribute__((used))
#else
#define LJ_USED
#endif
#define LJ_FUNC_NORET   LJ_FUNC LJ_NORET
#define LJ_FUNCA_NORET   LJ_FUNCA LJ_NORET
#define LJ_ASMF_NORET   LJ_ASMF LJ_NORET

// Internal assertions.
// NOTE: If you want to set a breakpoint for a raised assert, do so in lj_assert_fail()

#if defined(LUA_USE_ASSERT) || defined(LUA_USE_APICHECK)
// Forward declaration needed before macro definitions (full definition in lj_obj.h)
struct global_State;
LJ_FUNC_NORET void lj_assert_fail(global_State* g, const char* file, int line, const char* func, const char* fmt, ...);

// Use comma operator to ensure consistent void type for MSVC compatibility.
// The (void)0 cast ensures both branches of the ternary have the same type.
#define lj_assert_check(g, c, ...) ((c) ? (void)0 : (lj_assert_fail((g), __FILE__, __LINE__, __func__, __VA_ARGS__), (void)0))
#define lj_checkapi(c, ...)   lj_assert_check(G(L), (c), __VA_ARGS__)
#else
#define lj_checkapi(c, ...)   ((void)L)
#endif

#ifdef LUA_USE_ASSERT
#define lj_assertG_(g, c, ...)   lj_assert_check((g), (c), __VA_ARGS__)
#define lj_assertG(c, ...)   lj_assert_check(g, (c), __VA_ARGS__)
#define lj_assertL(c, ...)   lj_assert_check(G(L), (c), __VA_ARGS__)
#define lj_assertX(c, ...)   lj_assert_check(NULL, (c), __VA_ARGS__)
#define check_exp(c, e)      (lj_assertX((c), #c), (e))
#else
#define lj_assertG_(g, c, ...)   ((void)0)
#define lj_assertG(c, ...)   ((void)g)
#define lj_assertL(c, ...)   ((void)L)
#define lj_assertX(c, ...)   ((void)0)
#define check_exp(c, e)      (e)
#endif

// PRNG state. Need this here, details in lj_prng.h.
typedef struct PRNGState {
   uint64_t u[4];
} PRNGState;
