// LuaJIT VM tags, values and objects.
//
// Copyright (C) 2025-2026 Paul Manias
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
// Portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h

#pragma once

#include "lua.h"
#include "lj_def.h"
#include "lj_arch.h"
#include <array>
#include <format>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <ankerl/unordered_dense.h>
#include "../../../struct_def.h"

#ifndef PLATFORM_CONFIG_H
#include <kotuku/config.h>
#endif

#include <kotuku/system/errors.h>

// GC object types

union GCobj;
struct GCstr;
struct GCudata;
struct GCproto;
struct GCupval;
union GCfunc;
struct GCfuncC;
struct GCfuncL;
struct GCtab;
struct GChead;
struct Node;

// State objects

struct lua_State;
struct global_State;
struct GCState;
struct StrInternState;

// Parser objects

class TipEmitter;

// Debug objects

struct CapturedStackTrace;
struct FileSource;

// Value types

union TValue;
struct MRef;
struct GCRef;
struct SBuf;

// External classes

class ParserDiagnostics;
class objScript;

// Memory and GC object sizes.

using MSize = uint32_t;  // NB: Can't be changed - would affect offsets in GC objects
using GCSize = uint64_t; // NB: Can't be changed - would affect offsets in GC objects

enum class AstNodeKind : uint16_t {
   LiteralExpr,
   IdentifierExpr,
   VarArgExpr,
   UnaryExpr,
   BinaryExpr,
   UpdateExpr,
   TernaryExpr,
   PresenceExpr,
   PipeExpr,
   CallExpr,
   MemberExpr,
   IndexExpr,
   SafeMemberExpr,
   SafeIndexExpr,
   SafeCallExpr,
   ResultFilterExpr,
   TableExpr,
   FunctionExpr,
   DeferredExpr,  // Deferred expression <{ expr }>
   RangeExpr,     // Range literal {start..stop} or {start...stop}
   ChooseExpr,    // Choose expression: choose value from pattern -> result ... end
   BlockStmt,
   AssignmentStmt,
   LocalDeclStmt,
   GlobalDeclStmt,
   LocalFunctionStmt,
   FunctionStmt,
   IfStmt,
   WhileStmt,
   RepeatStmt,
   NumericForStmt,
   GenericForStmt,
   BreakStmt,
   ContinueStmt,
   ReturnStmt,
   DeferStmt,
   DoStmt,
   ConditionalShorthandStmt,
   TryExceptStmt,  // try...except...end exception handling
   RaiseStmt,      // raise expression [, message]
   CheckStmt,      // check expression
   ImportStmt,     // import 'module' statement
   WithStmt,       // with obj1, obj2 do ... end
   ExpressionStmt
};

// Parameter type annotation for static analysis
enum class TiriType : uint8_t {
   Any = 0,     // No type constraint (default)
   Nil,
   Bool,
   Num,
   Str,
   Table,
   Array,
   Func,
   Thread,
   Object,       // Kotuku object (LT_TOBJECT)
   Range,        // Range expression (runtime: LJ_TUDATA)
   Unknown
};

// Maximum number of explicitly typed return values per function
constexpr size_t MAX_RETURN_TYPES = 8;

//********************************************************************************************************************
// Memory reference

struct MRef {
   uint64_t ptr;
public:
   template<typename T = void> [[nodiscard]] constexpr inline T * get() const noexcept { return (T *)(void *)ptr; }
   template<typename T> constexpr inline void set_fn(T *p) noexcept { ptr = uint64_t((void *)p); }
   constexpr inline void set(std::nullptr_t) noexcept { ptr = 0; } // Overload for nullptr
   template<typename T = uint64_t> constexpr inline void set(T u) noexcept { ptr = uint64_t(u); }
   constexpr inline void set(MRef v) noexcept { ptr = v.ptr; }
};

// MRef accessor functions

template<typename T> [[nodiscard]] constexpr inline T * mref(const MRef &r) noexcept { return r.get<T>(); }
[[nodiscard]] constexpr inline uint64_t mrefu(const MRef &r) noexcept { return r.ptr; }
template<typename T> constexpr inline void setmref(MRef &r, T *p) noexcept { r.set_fn(p); }
constexpr inline void setmref(MRef &r, std::nullptr_t) noexcept { r.set(nullptr); } // Overload for nullptr
constexpr inline void setmrefu(MRef &r, uint64_t u) noexcept { r.set(u); }
constexpr inline void setmrefr(MRef &r, MRef v) noexcept { r.set(v); }

//********************************************************************************************************************
// GC object references

struct GCRef {
   uint64_t gcptr64;   //  True 64 bit pointer.
};

// Common GC header for all collectable objects.
// Note: This macro must remain as it defines struct fields inline.

#define GCHeader   GCRef nextgc; uint8_t marked; uint8_t gct

// This occupies 6 bytes, so use the next 2 bytes for non-32 bit fields.

// GCRef accessor functions - 64-bit only
// These just cast - they don't dereference, so can be inline functions

[[nodiscard]] inline GCobj* gcref(GCRef r) noexcept { return (GCobj*)r.gcptr64; }
template<typename T> [[nodiscard]] inline T* gcrefp_fn(GCRef r) noexcept { return (T*)(void*)r.gcptr64; }
[[nodiscard]] constexpr inline uint64_t gcrefu(GCRef r) noexcept { return r.gcptr64; }
[[nodiscard]] constexpr inline bool gcrefeq(GCRef r1, GCRef r2) noexcept { return r1.gcptr64 IS r2.gcptr64; }
template<typename T> inline void setgcrefp_fn(GCRef& r, T* p) noexcept { r.gcptr64 = uint64_t(p); }

// Overload for integer types (used in some low-level operations)
// Note: On 64-bit, uintptr_t is typically uint64_t, so only need one

constexpr inline void setgcrefp_fn(GCRef& r, uint64_t v) noexcept { r.gcptr64 = v; }
constexpr inline void setgcrefp_fn(GCRef& r, int v) noexcept { r.gcptr64 = uint64_t(v); }
constexpr inline void setgcrefnull(GCRef& r) noexcept { r.gcptr64 = 0; }
constexpr inline void setgcrefr(GCRef& r, GCRef v) noexcept { r.gcptr64 = v.gcptr64; }

// These dereference GCobj->gch, so must remain macros until GCobj is defined
// They will be converted to inline functions after GCobj definition

#define setgcref(r, gc)      ((r).gcptr64 = (uint64_t)&(gc)->gch)
#define setgcreft(r, gc, it) ((r).gcptr64 = (uint64_t)&(gc)->gch | (((uint64_t)(it)) << 47))

// Compatibility macro for template function

#define gcrefp(r, t)    gcrefp_fn<t>(r)
#define setgcrefp(r, p) setgcrefp_fn((r), (p))

// gcnext dereferences GCobj, will be defined as inline function after GCobj

#define gcnext(gc)   (gcref((gc)->gch.nextgc))

// IMPORTANT NOTE:
//
// All uses of the setgcref* macros MUST be accompanied with a write barrier.
//
// This is to ensure the integrity of the incremental GC. The invariant to preserve is that a black object never
// points to a white object.  I.e. never store a white object into a field of a black object.
//
// It's ok to LEAVE OUT the write barrier ONLY in the following cases:
// - The source is not a GC object (NULL).
// - The target is a GC root. I.e. everything in global_State.
// - The target is a lua_State field (threads are never black).
// - The target is a stack slot, see setgcV et al.
// - The target is an open upvalue, i.e. pointing to a stack slot.
// - The target is a newly created object (i.e. marked white). But make sure nothing invokes the GC inbetween.
// - The target and the source are the same object (self-reference).
// - The target already contains the object (e.g. moving elements around).
//
// The most common case is a store to a stack slot. All other cases where a barrier has been omitted are annotated
// with a NOBARRIER comment.
//
// The same logic applies for stores to table slots (array part or hash part). ALL uses of lj_tab_set* require a
// barrier for the stored value *and* the stored key, based on the above rules. In practice this means a barrier is
// needed if *either* of the key or value are a GC object.
//
// It's ok to LEAVE OUT the write barrier in the following special cases:
// - The stored value is nil. The key doesn't matter because it's either not resurrected or lj_tab_newkey() will take
//   care of the key barrier.
// - The key doesn't matter if the *previously* stored value is guaranteed to be non-nil (because the key is kept
//   alive in the table).
// - The key doesn't matter if it's guaranteed not to be part of the table, since lj_tab_newkey() takes care of the
//   key barrier. This applies trivially to new tables, but watch out for resurrected keys. Storing a nil value
//   leaves the key in the table!
//
// In case of doubt use lj_gc_anybarriert() as it's rather cheap. It's used by the interpreter for all table stores.
//
// Note: In contrast to Lua's GC, LuaJIT's GC does *not* specially mark dead keys in tables. The reference is left in,
// but it's guaranteed to be never dereferenced as long as the value is nil. It's ok if the key is freed or if any
// object subsequently gets the same address.
//
// Not destroying dead keys helps to keep key hash slots stable. This avoids specialization back-off for HREFK when a
// value flips between nil and non-nil and the GC gets in the way. It also allows safely hoisting HREF/HREFK across GC
// steps. Dead keys are only removed if a table is resized (i.e. by NEWREF) and xREF must not be CSEd across a resize.
//
// The trade-off is that a write barrier for tables must take the key into account, too. Implicitly resurrecting the
// key by storing a non-nil value may invalidate the incremental GC invariant.

// Common type definitions

// Types for handling bytecodes. Need this here, details in lj_bc.h.
using BCIns = uint64_t;  //  Bytecode instruction (64-bit for native pointer storage).
using BCPOS = uint32_t;  //  Bytecode position.
using BCREG = uint32_t;  //  Bytecode register.

// Bytecode line number with file index encoding.
// Upper 8 bits store file index (0-254, 255 = overflow), lower 24 bits store line number.
// This allows tracking source locations across imported files.

class BCLine {
public:
   static constexpr int32_t FILE_SHIFT = 24;
   static constexpr int32_t LINE_MASK = 0x00FFFFFF;
   static constexpr int32_t FILE_MASK = int32_t(0xFF000000);

   // Sentinel values for special cases
   static constexpr int32_t NO_LINE = -1;       // No line information available
   static constexpr int32_t BUILTIN = ~0;       // Builtin function (no source)

   // Constructors
   constexpr BCLine() noexcept : m_value(0) {}
   constexpr BCLine(int32_t raw) noexcept : m_value(raw) {}
   constexpr BCLine(uint8_t fileIndex, int32_t line) noexcept
      : m_value((int32_t(fileIndex) << FILE_SHIFT) | (line & LINE_MASK)) {}

   // Static factory for encoding file index and line
   [[nodiscard]] static constexpr BCLine encode(uint8_t fileIndex, int32_t line) noexcept { return BCLine(fileIndex, line); }

   // Decoding methods
   [[nodiscard]] constexpr uint8_t fileIndex() const noexcept { return uint8_t((m_value & FILE_MASK) >> FILE_SHIFT); }
   [[nodiscard]] constexpr int32_t lineNumber() const noexcept { return m_value & LINE_MASK; }

   // Semantic queries
   [[nodiscard]] constexpr bool isValid() const noexcept { return m_value != NO_LINE and m_value != BUILTIN; }
   [[nodiscard]] constexpr bool isBuiltin() const noexcept { return m_value IS BUILTIN; }

   // Implicit conversion for backward compatibility with existing code that uses BCLine as int32_t.
   // This allows BCLine to work seamlessly with arithmetic, comparison, and bitwise operations.
   constexpr operator int32_t() const noexcept { return m_value; }

   // Increment operators (return BCLine to maintain type)
   constexpr BCLine& operator++() noexcept { ++m_value; return *this; }
   constexpr BCLine operator++(int) noexcept { BCLine tmp = *this; ++m_value; return tmp; }

   // Raw value access (explicit alternative to implicit conversion)
   [[nodiscard]] constexpr int32_t raw() const noexcept { return m_value; }

private:
   int32_t m_value;
};

// std::format support for BCLine (formats as its integer value)
template<>
struct std::formatter<BCLine> : std::formatter<int32_t> {
   auto format(BCLine line, std::format_context& ctx) const {
      return std::formatter<int32_t>::format(line.raw(), ctx);
   }
};

// Internal assembler functions. Never call these directly from C.
using ASMFunction = void(*)(void);

// Resizable string buffer. Need this here, details in lj_buf.h.
#define SBufHeader   char *w, *e, *b; MRef L
typedef struct SBuf {
   SBufHeader;
} SBuf;

// Tags and values

// Frame link.

typedef union {
   int32_t ftsz;      //  Frame type and size of previous frame.
   MRef pcr;      //  Or PC for Lua frames.
} FrameLink;

// Tagged value.

typedef LJ_ALIGN(8) union TValue {
   uint64_t u64;      //  64 bit pattern overlaps number.
   lua_Number n;      //  Number object overlaps split tag/value object.
   GCRef gcr;         //  GCobj reference with tag.
   int64_t it64;
   struct {
      LJ_ENDIAN_LOHI(
         int32_t i;   //  Integer value.
      , uint32_t it;  //  Internal object tag. Must overlap MSW of number.
         )
   };
   int64_t ftsz;      //  Frame type and size of previous frame, or PC.

   struct {
      LJ_ENDIAN_LOHI(
         uint32_t lo;  //  Lower 32 bits of number.
      , uint32_t hi;   //  Upper 32 bits of number.
         )
   } u32;
} TValue;

using cTValue = const TValue;

[[nodiscard]] inline TValue* tvref(MRef r) noexcept { return r.get<TValue>(); }

//********************************************************************************************************************
// Format for 64 bit GC references (LJ_GC64):
//
// The upper 13 bits must be 1 (0xfff8...) for a special NaN. The next 4 bits hold the internal tag. The lowest 47
// bits either hold a pointer, a zero-extended 32 bit integer or all bits set to 1 for primitive types.
//
//64-bit TValue layout:
// ┌─────────────────────────────────────────────────────────────────┐
// │ 63      51│50    47│46                                         0│
// ├───────────┼────────┼────────────────────────────────────────────┤
// │  13 bits  │ 4 bits │              47 bits                       │
// │  NaN sig  │ itype  │         pointer/value                      │
// │  (all 1s) │  tag   │                                            │
// └───────────┴────────┴────────────────────────────────────────────┘
//                    ───────MSW───────.───────LSW─────
// primitive types    │1..1│itype│1..................1│
// GC objects         │1..1│itype│────GCRef───────────│
// lightuserdata      │1..1│itype│seg│──────ofs───────│
// int (LJ_DUALNUM)   │1..1│itype│0..0│─────int───────│
// number             ────────────double───────────────
//
// ORDERING of LJ_T Tags
// ---------------------
// Primitive types nil/false/true must be first
// lightuserdata next
// GC objects are at the end
// table/userdata must be lowest
// Also check lj_ir.h for similar ordering constraints.

// Internal type tags. Maxes out at 16 tags.

inline constexpr uint32_t LJ_TNIL      = ~0u;  // Nil
inline constexpr uint32_t LJ_TFALSE    = ~1u;  // False
inline constexpr uint32_t LJ_TTRUE     = ~2u;  // True
inline constexpr uint32_t LJ_TLIGHTUD  = ~3u;  // Lightuserdata
inline constexpr uint32_t LJ_TSTR      = ~4u;  // String
inline constexpr uint32_t LJ_TUPVAL    = ~5u;  // Unused in TValue(?) could be shared?
inline constexpr uint32_t LJ_TTHREAD   = ~6u;  // Unused?
inline constexpr uint32_t LJ_TPROTO    = ~7u;  // Function prototype
inline constexpr uint32_t LJ_TFUNC     = ~8u;  // Function
inline constexpr uint32_t LJ_TTRACE    = ~9u;  // Unused in TValue(?) could be shared?
inline constexpr uint32_t LJ_TOBJECT   = ~10u; // Object (Kotuku)
inline constexpr uint32_t LJ_TTAB      = ~11u; // Table
inline constexpr uint32_t LJ_TUDATA    = ~12u; // Userdata
inline constexpr uint32_t LJ_TARRAY    = ~13u; // Native array type
// This is just the canonical number type used in some places.
inline constexpr uint32_t LJ_TNUMX     = ~14u; // Number
// ~15u is unused

// Integers have itype == LJ_TISNUM doubles have itype < LJ_TISNUM
// Always LJ_TNUMX for 64-bit GC64 mode
inline constexpr uint32_t LJ_TISNUM = LJ_TNUMX;
inline constexpr uint32_t LJ_TISTRUECOND = LJ_TFALSE;
inline constexpr uint32_t LJ_TISPRI      = LJ_TTRUE;
inline constexpr uint32_t LJ_TISGCV      = LJ_TSTR + 1;
inline constexpr uint32_t LJ_TISTABUD    = LJ_TTAB;

// Type marker for slot holding a traversal index. Must be lightuserdata.
inline constexpr uint32_t LJ_KEYINDEX = 0xfffe7fffu;

// GC64 pointer mask - always defined for 64-bit
inline constexpr uint64_t LJ_GCVMASK = (uint64_t(1) << 47) - 1;

// Lightuserdata is segmented to stay within 47 bits
inline constexpr int LJ_LIGHTUD_BITS_SEG = 8;
inline constexpr int LJ_LIGHTUD_BITS_LO  = 47 - LJ_LIGHTUD_BITS_SEG;

//********************************************************************************************************************
// String object

typedef uint32_t LuaStrHash;   //  String hash value.
typedef uint32_t StrID;      //  String ID.

// String object header. String payload follows.
typedef struct GCstr {
   GCHeader;           // 16-bit aligned
   uint8_t reserved;   // Used by lexer for fast lookup of reserved words.
   uint8_t flags;      // Currently unused, but available for marking strings in relation to the thing they represent
   StrID sid;          // Interned string ID.
   LuaStrHash hash;    // Hash of string.
   MSize len;          // Size of string.
} GCstr;

inline GCstr* strref(GCRef r) noexcept;  // Defined after GCobj

[[nodiscard]] inline const char* strdata(const GCstr* s) noexcept { return (const char*)(s + 1); }
[[nodiscard]] inline char* strdatawr(GCstr* s) noexcept { return (char*)(s + 1); }

// strVdata uses strV which is defined later
#define strVdata(o)   strdata(strV(o))

//********************************************************************************************************************
// Userdata object

typedef struct GCudata {
   GCHeader;
   uint8_t udtype;    //  Userdata type.
   uint8_t unused2;
   GCRef env;         //  Should be at same offset in GCfunc.
   MSize len;         //  Size of payload.
   GCRef metatable;   //  [32] Must be at same offset in GCtab.
   uint32_t align1;   //  To force 8 byte alignment of the payload.
} GCudata;

// Userdata types.
enum {
   UDTYPE_USERDATA,   //  Regular userdata.
   UDTYPE_IO_FILE_DEPRECATED,    // Was I/O library FILE.
   UDTYPE_FFI_CLIB_DEPRECATED,   // Was FFI C library namespace.
   UDTYPE_BUFFER_DEPRECATED,     // Was String buffer.
   UDTYPE_THUNK,      //  Thunk (deferred evaluation).
   UDTYPE__MAX
};

//********************************************************************************************************************
// Thunk userdata payload - stored after GCudata header.  Used for deferred/lazy evaluation of expressions

typedef struct ThunkPayload {
   GCRef deferred_func;    // The deferred closure (GCfunc)
   TValue cached_value;    // Cached resolved value
   uint8_t resolved;       // Resolution flag (0 = not resolved, 1 = resolved)
   uint8_t expected_type;  // LJ type tag for type() (LUA_TSTRING, LUA_TNUMBER, etc.)
   uint16_t padding;       // Padding for alignment
} ThunkPayload;

// Helper to get thunk payload from userdata
#define thunk_payload(u)  ((ThunkPayload *)uddata(u))

[[nodiscard]] inline void* uddata(GCudata* u) noexcept { return (void*)(u + 1); }
[[nodiscard]] inline MSize sizeudata(const GCudata* u) noexcept { return sizeof(GCudata) + u->len; }

//********************************************************************************************************************
// Function Prototype Object
//
// GCproto represents the compiled, immutable blueprint of a function. It is created during parsing and
// contains all the static information needed to execute the function: bytecode instructions, constants, upvalue
// descriptors, and debug information.
//
// - A prototype is NOT a closure. Multiple closures (GCfunc) can share the same prototype.
// - Prototypes are immutable after creation - they store the "code" while closures capture the "environment".
// - Child prototypes (nested functions) are stored in the constants array (sizekgc).
// - The bytecode array immediately follows the GCproto structure in memory.
// - Constants are stored in a "split" array with GC objects at negative indices and numbers at positive indices.
//
// Memory Layout (contiguous allocation):
//   [GCproto header] [bytecode...] [upvalue descriptors...] [constants (GCRef then lua_Number)...] [debug info...]
//
// Usage:
// - Created by the parser (parse_internal.h, parser.cpp) during compilation
// - Referenced by GCfuncL closures via the pc field (funcproto() retrieves it)
// - Used by the bytecode interpreter and JIT compiler for execution
// - Accessed by debug facilities for stack traces, line info, and variable names
// - Serialised/deserialised by lj_bcwrite.cpp and lj_bcread.cpp for bytecode dumps
//
// Related: GCfunc (closure that references a prototype), lj_func.cpp (prototype lifecycle)

inline constexpr int32_t SCALE_NUM_GCO = int32_t(sizeof(lua_Number) / sizeof(GCRef));

[[nodiscard]] constexpr inline MSize round_nkgc(MSize n) noexcept
{
   return (n + SCALE_NUM_GCO - 1) & ~(SCALE_NUM_GCO - 1);
}

// Maximum number of explicitly typed return values per function prototype
inline constexpr size_t PROTO_MAX_RETURN_TYPES = 8;

//********************************************************************************************************************
// Function prototype registry types for C function type signatures.  These structures enable the parser to look up
// type signatures for registered C functions and interface methods (e.g., print, tostring, string.find, obj.new) at
// compile time.

// Prototype flags for function metadata

enum class FProtoFlags : uint8_t {
   None     = 0x00,
   Variadic = 0x01,  // Function accepts variable arguments beyond listed params
   NoNil    = 0x02   // All declared parameters are required (no nil values permitted)
};

inline constexpr FProtoFlags operator|(FProtoFlags a, FProtoFlags b) {
   return FProtoFlags(uint8_t(a) | uint8_t(b));
}

inline constexpr FProtoFlags operator&(FProtoFlags a, FProtoFlags b) {
   return FProtoFlags(uint8_t(a) & uint8_t(b));
}

constexpr size_t FPROTO_MAX_PARAMS = 16; // Maximum parameter count for prototypes

// Function prototype storing type signature

struct fprototype {
   uint8_t result_count;     // Number of return values (0 to PROTO_MAX_RETURN_TYPES)
   uint8_t param_count;      // Number of parameters (0 to FPROTO_MAX_PARAMS)
   FProtoFlags flags;        // Optional flags
   uint8_t _pad;             // Alignment padding
   std::array<TiriType, PROTO_MAX_RETURN_TYPES> result_types;

   // Parameter types follow (accessed via param_types())

   inline TiriType * param_types() noexcept { return (TiriType *)(this + 1); }

   inline const TiriType * param_types() const noexcept { return (const TiriType *)(this + 1); }

   inline TiriType first_result() const noexcept {
      return result_count > 0 ? result_types[0] : TiriType::Unknown;
   }
};

// Lookup key for prototype registry (interface_hash=0 for globals)

struct ProtoKey {
   uint32_t interface_hash;
   uint32_t function_hash;
   bool operator==(const ProtoKey& o) const {
      return interface_hash IS o.interface_hash and function_hash IS o.function_hash;
   }
};

struct ProtoKeyHash {
   size_t operator()(const ProtoKey& k) const noexcept {
      static_assert(sizeof(size_t) >= 8, "ProtoKeyHash requires 64-bit size_t");
      return size_t(k.interface_hash)<<32 | size_t(k.function_hash);
   }
};

//********************************************************************************************************************
// Try-except exception handling metadata structures.
// These structures support bytecode-level try-except statements.

// Handler metadata stored per-proto - describes a single except clause

struct TryHandlerDesc {
   uint64_t filter_packed;   // Packed 16-bit error codes (up to 4), 0 = catch-all
   BCPOS    handler_pc;      // Bytecode position for handler entry
   BCREG    exception_reg;   // Register to store exception table, 0xFF = no variable
};

// Try block descriptor - describes a try block and its handlers

struct TryBlockDesc {
   uint16_t first_handler;   // Index into proto's try_handlers array
   uint8_t  handler_count;   // Number of except clauses for this try block
   uint8_t  entry_slots;     // Active slot count at try entry (first free register)
   uint8_t  flags;           // Bit 0 = TRY_FLAG_TRACE (capture stack trace on exception)
};

// Flags for TryBlockDesc.flags

inline constexpr uint8_t TRY_FLAG_TRACE = 0x01;  // Capture stack trace on exception

// Maximum nesting depth for try blocks

inline constexpr int LJ_MAX_TRY_DEPTH = 32;

// Exception frame for try-except blocks (runtime state)
// Note: frame_base and saved_top are offsets from L->stack (not absolute pointers) because the Lua stack can be
// reallocated during execution.  Use savestack(L, ptr) to convert to offset, restorestack(L, offset) to convert back.

struct TryFrame {
   GCfunc    *func;            // Function containing the try block
   ptrdiff_t frame_base;       // Offset of L->base when BC_TRYENTER executed
   ptrdiff_t saved_top;        // Offset of L->top when BC_TRYENTER executed
   uint16_t  try_block_index;  // Index into GCproto::try_blocks
   uint8_t   flags;            // Copy of TryBlockDesc.flags (e.g. TRY_FLAG_TRACE)
   BCREG     saved_nactvar;    // Active slot count at try entry (first free register)
};

// Stack of try frames for exception unwinding

struct TryFrameStack {
   TryFrame frames[LJ_MAX_TRY_DEPTH];
   int depth = 0;
};

typedef struct GCproto {
   GCHeader;
   uint8_t  numparams; //  Number of parameters.
   uint8_t  framesize; //  Fixed frame size.
   MSize    sizebc;    //  Number of bytecode instructions.
   uint32_t unused_gc64; // Padding for 64-bit alignment
   GCRef    gclist;
   MRef     k;        //  Split constant array (points to the middle).
   MRef     uv;       //  Upvalue list. local slot|0x8000 or parent uv idx.
   MSize    sizekgc;  //  Number of collectable constants.
   MSize    sizekn;   //  Number of lua_Number constants.
   MSize    sizept;   //  Total size including colocated arrays.
   uint8_t  sizeuv;   //  Number of upvalues.
   uint8_t  flags;    //  Miscellaneous flags (see below).
   uint16_t trace;    //  Anchor for chain of root traces.
   //  The following fields are for debugging/tracebacks only
   GCRef  chunk_name;  //  Name of the chunk this function was defined in.
   BCLine firstline;  //  First line of the function definition.
   BCLine numline;    //  Number of lines for the function definition.
   uint8_t file_source_idx;  //  Index into lua_State::file_sources (fallback for edge cases).
   MRef   lineinfo;   //  BCLine[sizebc-1] array - file index in upper 8 bits, line in lower 24.
   MRef   uvinfo;     //  Upvalue names.
   MRef   varinfo;    //  Names and compressed extents of local variables.
   uint64_t closeslots;  //  Bitmap of locals with <close> attribute (max 64 slots)
   // Return type information for runtime type checking
   std::array<TiriType, PROTO_MAX_RETURN_TYPES> result_types{};  // Return types, set by fs_finish()
   // Try-except exception handling metadata
   TryBlockDesc   *try_blocks;        // Array of try block descriptors (nullptr if none)
   TryHandlerDesc *try_handlers;      // Array of handler descriptors (nullptr if none)
   uint16_t        try_block_count;   // Number of try blocks
   uint16_t        try_handler_count; // Number of handlers
} GCproto;

// Flags for prototype.
inline constexpr uint8_t PROTO_CHILD        = 0x01;   //  Has child prototypes.
inline constexpr uint8_t PROTO_VARARG       = 0x02;   //  Vararg function.
inline constexpr uint8_t PROTO_FFI          = 0x04;   //  Uses BC_KCDATA for FFI datatypes.
inline constexpr uint8_t PROTO_NOJIT        = 0x08;   //  JIT disabled for this function.
inline constexpr uint8_t PROTO_ILOOP        = 0x10;   //  Patched bytecode with ILOOP etc.
// Only used during parsing.
inline constexpr uint8_t PROTO_HAS_RETURN   = 0x20;   //  Already emitted a return.
inline constexpr uint8_t PROTO_FIXUP_RETURN = 0x40;   //  Need to fixup emitted returns.
inline constexpr uint8_t PROTO_TYPEFIX      = 0x80;   //  Runtime type inference enabled (no explicit return types).
// Top bits used for counting created closures.
inline constexpr uint8_t PROTO_CLCOUNT      = 0x20;   //  Base of saturating 3 bit counter.
inline constexpr int PROTO_CLC_BITS         = 3;
inline constexpr int PROTO_CLC_POLY         = 3 * PROTO_CLCOUNT;  //  Polymorphic threshold.

inline constexpr uint16_t PROTO_UV_LOCAL     = 0x8000;   //  Upvalue for local slot.
inline constexpr uint16_t PROTO_UV_IMMUTABLE = 0x4000;   //  Immutable upvalue.

[[nodiscard]] inline GCobj* proto_kgc(const GCproto* pt, ptrdiff_t idx) noexcept {
   return check_exp(uintptr_t(intptr_t(idx)) >= uintptr_t(-intptr_t(pt->sizekgc)),
      gcref(pt->k.get<GCRef>()[idx]));
}

[[nodiscard]] inline TValue* proto_knumtv(const GCproto* pt, MSize idx) noexcept {
   return check_exp(uintptr_t(idx) < pt->sizekn, &(pt->k.get<TValue>()[idx]));
}

[[nodiscard]] inline BCIns* proto_bc(const GCproto* pt) noexcept {
   return (BCIns*)((char*)pt + sizeof(GCproto));
}

[[nodiscard]] inline BCPOS proto_bcpos(const GCproto* pt, const BCIns* pc) noexcept {
   return BCPOS(pc - proto_bc(pt));
}

[[nodiscard]] inline uint16_t* proto_uv(const GCproto* pt) noexcept {
   return pt->uv.get<uint16_t>();
}

// Forward declarations - defined after GCobj is complete
inline GCstr* proto_chunk_name(const GCproto* pt) noexcept;
inline const char* proto_chunk_namestr(const GCproto* pt) noexcept;

[[nodiscard]] inline const void* proto_lineinfo(const GCproto* pt) noexcept
{
   return pt->lineinfo.get<const void>();
}

[[nodiscard]] inline const uint8_t* proto_uvinfo(const GCproto* pt) noexcept
{
   return mref<const uint8_t>(pt->uvinfo);
}

[[nodiscard]] inline const uint8_t* proto_varinfo(const GCproto* pt) noexcept
{
   return mref<const uint8_t>(pt->varinfo);
}

//********************************************************************************************************************
// Upvalue object

typedef struct GCupval {
   GCHeader;
   uint8_t closed;      // Set if closed (i.e. uv->v == &uv->u.value).
   uint8_t immutable;   // Immutable value.
   union {
      TValue tv;        // If closed: the value itself.
      struct {          // If open: double linked list, anchored at thread.
         GCRef prev;
         GCRef next;
      };
   };
   MRef v;           // Points to stack slot (open) or above (closed).
   uint32_t dhash;   // Disambiguation hash: dh1 != dh2 => cannot alias.
} GCupval;

// Forward declarations - defined after GCobj is complete

inline GCupval * uvprev(GCupval *uv) noexcept;
inline GCupval * uvnext(GCupval *uv) noexcept;

[[nodiscard]] inline TValue * uvval(GCupval* uv) noexcept { return mref<TValue>(uv->v); }

// Function object (closures)

// Common header for functions. env should be at same offset in GCudata.
#define GCfuncHeader \
  GCHeader; uint8_t ffid; uint8_t nupvalues; \
  GCRef env; GCRef gclist; MRef pc

typedef struct GCfuncC {
   GCfuncHeader;
   lua_CFunction f;   //  C function to be called.
   TValue upvalue[1];   //  Array of upvalues (TValue).
} GCfuncC;

typedef struct GCfuncL {
   GCfuncHeader;
   GCRef uvptr[1];   //  Array of _pointers_ to upvalue objects (GCupval).
} GCfuncL;

typedef union GCfunc {
   GCfuncC c;
   GCfuncL l;
} GCfunc;

inline constexpr uint8_t FF_LUA = 0;
inline constexpr uint8_t FF_C   = 1;

[[nodiscard]] inline bool isluafunc(const GCfunc* fn) noexcept { return fn->c.ffid IS FF_LUA; }
[[nodiscard]] inline bool iscfunc(const GCfunc* fn) noexcept { return fn->c.ffid IS FF_C; }
[[nodiscard]] inline bool isffunc(const GCfunc* fn) noexcept { return fn->c.ffid > FF_C; }

[[nodiscard]] inline GCproto* funcproto(const GCfunc* fn) noexcept {
   return check_exp(isluafunc(fn), (GCproto*)(mref<char>(fn->l.pc) - sizeof(GCproto)));
}

[[nodiscard]] constexpr inline size_t sizeCfunc(MSize n) noexcept {
   return sizeof(GCfuncC) - sizeof(TValue) + sizeof(TValue) * n;
}

[[nodiscard]] constexpr inline size_t sizeLfunc(MSize n) noexcept {
   return sizeof(GCfuncL) - sizeof(GCRef) + sizeof(GCRef) * n;
}

// Table object

// Hash node.

typedef struct Node {
   TValue val;      //  Value object. Must be first field.
   TValue key;      //  Key object.
   MRef next;       //  Hash chain.
} Node;

static_assert(offsetof(Node, val) == 0);

typedef struct GCtab {
   GCHeader;
   uint8_t  nomm;      // Negative cache for fast metamethods.
   int8_t   colo;      // Array colocation.
   MRef     array;     // [16] Array part.
   GCRef    gclist;    // [24] GC list for marking (must match GCudata.gclist)
   GCRef    metatable; // [32] Must be at same offset in GCudata.
   MRef     node;      // Hash part.
   uint32_t asize;     // Size of array part (keys [0, asize-1]).
   uint32_t hmask;     // Hash part mask (size of hash part - 1).
   MRef     freetop;   // Top of free elements.
} GCtab;

[[nodiscard]] constexpr inline size_t sizetabcolo(MSize n) noexcept { return n * sizeof(TValue) + sizeof(GCtab); }

// Forward declaration - defined after GCobj is complete
inline GCtab* tabref(GCRef r) noexcept;

[[nodiscard]] constexpr inline Node* noderef(MRef r) noexcept { return mref<Node>(r); }
[[nodiscard]] inline Node* nextnode(Node *n) noexcept { return mref<Node>(n->next); }
[[nodiscard]] inline Node* getfreetop(const GCtab *t, Node *) noexcept { return noderef(t->freetop); }
inline void setfreetop(GCtab *t, Node *, Node *v) noexcept { t->freetop.set(v); }

// Array element type constants

enum class AET : uint8_t {
   // NOTE: Changes to this table require an update to glArrayConversion
   // Primitive types first
   BYTE = 0,   // byte
   INT16,      // int16_t
   INT32,      // int32_t
   INT64,      // int64_t
   FLOAT,      // float
   DOUBLE,     // double
   // Vulnerable types follow (anything involving or could involve a pointer)
   PTR,        // void*
   CSTR,       // const char *
   STR_CPP,    // std::string (C++ string)
   STR_GC,     // GCstr * (interned string)
   TABLE,      // GCtab * (table reference)
   ARRAY,      // GCarray * (array reference)
   ANY,        // TValue (mixed type storage)
   STRUCT,     // Structured data (uses structdef)
   OBJECT,     // OBJECTPTR for external object references originating from the Kotuku API; otherwise GCobject
   MAX,
   VULNERABLE = PTR
};

// Array flags
inline constexpr uint8_t ARRAY_READONLY  = 0x01;  // Cannot modify elements
inline constexpr uint8_t ARRAY_EXTERNAL  = 0x02;  // Data not owned by array (storage is raw pointer)
inline constexpr uint8_t ARRAY_CACHED    = 0x00;  // Copy external data into owned storage (default, flag is 0)

struct array_meta {
   uint8_t itype = 0;  // Internal type tag (LJ_T*).  NB: Shortened to 8 bits
   int8_t type = 0;    // Lua type (LUA_T*)
   bool primitive = true;
};

extern const array_meta glArrayConversion[size_t(AET::MAX)];

// Native typed array object. Fixed-size, homogeneous element storage.
// Storage uses a heap-allocated buffer for owned data.

struct GCarray {
   GCHeader;
   AET     elemtype;    // [10] Element type
   uint8_t flags;       // [11] Array flags
   uint8_t luatype;     // [12] Lua type (LUA_T*)
   uint8_t itype;       // [13] Internal type (LJ_T*).  NB: Shortened to 8 bits
   uint16_t _pad0;      // [14] Padding to align storage at offset 16 (like GCtab.array)
   void    *storage;    // [16] Heap-allocated storage for owned data, or external pointer (matches GCtab.array)
   GCRef   gclist;      // [24] GC list for marking (must match GCudata.gclist)
   GCRef   metatable;   // [32] Optional metatable (must match GCudata.metatable)
   MSize   len;         // Number of elements currently in use
   MSize   capacity;    // Number of elements that can be stored (allocated capacity)
   MSize   elemsize;    // Size of each element in bytes
   struct struct_record *structdef;  // Optional: struct definition for struct arrays
   std::vector<char> *strcache; // Optional: cached string content for CSTRING/STRING_CPP arrays

public:
   // Initialise the array structure. Storage must be pre-allocated by the caller using lj_mem_new()
   // for proper GC tracking. NOTE: lj_mem_newgco() already sets nextgc and marked - do NOT overwrite
   // them! We avoid member initialiser lists to prevent GCC from zero-initializing the GCHeader
   // fields (nextgc, marked) that were set by lj_mem_newgco().
   void init(void *Data, AET Type, MSize ElemSize, MSize Length, MSize Capacity, uint8_t Flags,
             struct struct_record *StructDef = nullptr) noexcept
   {
      gct       = ~LJ_TARRAY;
      luatype   = glArrayConversion[size_t(Type)].type;
      itype     = glArrayConversion[size_t(Type)].itype;
      elemtype  = Type;
      flags     = Flags;
      _pad0     = 0;
      storage   = Data;
      setgcrefnull(gclist);
      setgcrefnull(metatable);
      len       = Length;
      capacity  = Capacity;
      elemsize  = ElemSize;
      structdef = StructDef;
      strcache  = nullptr;
   }

   // Destructor only handles strcache. Storage is freed by lj_array_free() for proper GC tracking.
   ~GCarray() {
      if (strcache) delete strcache;
   }

   // Prevent copying (GC objects should not be copied)

   GCarray(const GCarray&) = delete;
   GCarray& operator=(const GCarray&) = delete;

   // Get pointer to element data

   [[nodiscard]] inline void * arraydata() noexcept { return storage; }
   [[nodiscard]] inline const void * arraydata() const noexcept { return storage; }

   // Get typed pointer to element data (convenience template)
   template<typename T> [[nodiscard]] inline T * get() noexcept { return (T *)storage; }
   template<typename T> [[nodiscard]] inline const T * get() const noexcept { return (const T *)storage; }

   // Zero-initialise the array data area (zeros all data up to capacity)

   void zero() { // NB: Intentionally ignores the read-only flag.
      if (storage) std::memset(storage, 0, capacity * elemsize);
   }

   [[nodiscard]] inline MSize arraylen() const noexcept { return len; }
   [[nodiscard]] inline bool is_readonly() const noexcept { return (flags & ARRAY_READONLY) != 0; }
   [[nodiscard]] inline bool is_external() const noexcept { return (flags & ARRAY_EXTERNAL) != 0; }
   [[nodiscard]] int type_flags() const noexcept;
   [[nodiscard]] inline size_t alloc_size() const noexcept { return sizeof(GCarray); }
   [[nodiscard]] inline size_t storage_size() const noexcept { return is_external() ? 0 : size_t(capacity) * elemsize; }
};

// Ensure metatable field is at the same offset in GCtab, GCarray, GCudata
static_assert(offsetof(GCarray, metatable) IS offsetof(GCtab, metatable));
static_assert(offsetof(GCarray, gclist) IS offsetof(GCtab, gclist));

// Forward declaration - defined after GCobj is complete
inline GCarray* arrayref(GCRef r) noexcept;

//********************************************************************************************************************
// Field tables are maintained per-class (cached globally) rather than per-instance.

// Flags for GCobject.flags
inline constexpr uint8_t GCOBJ_DETACHED = 0x01;  // Object is external reference, not owned
inline constexpr uint8_t GCOBJ_LOCKED   = 0x02;  // Lock acquired via AccessObject()

struct GCobject {
   GCHeader;                    // [0]  nextgc, marked, gct (10 bytes)
   uint8_t udtype;              // [10] Reserved for sub-types (future use)
   uint8_t flags;               // [11] Object flags (GCOBJ_DETACHED, GCOBJ_LOCKED)
   int32_t uid;                 // [12] Kotuku object unique ID (OBJECTID)
   uint32_t accesscount;        // [16] Access count for lock management
   uint32_t reserved;           // [20] Reserved for alignment
   GCRef gclist;                // [24] GC list for marking (must match GCtab.gclist)
   GCRef metatable;             // [32] Optional metatable (must match GCtab.metatable)
   struct Object *ptr;          // [40] Direct pointer to Kotuku object (OBJECTPTR, null if detached)
   class objMetaClass *classptr; // [48] Direct pointer to class metadata (objMetaClass*)

   inline bool is_detached() { return (flags & GCOBJ_DETACHED) != 0; }
   inline bool is_locked() { return (flags & GCOBJ_LOCKED) != 0; }
   inline void set_detached(bool v) { if (v) flags |= GCOBJ_DETACHED; else flags &= ~GCOBJ_DETACHED; }
   inline void set_locked(bool v) { if (v) flags |= GCOBJ_LOCKED; else flags &= ~GCOBJ_LOCKED; }
};

// Ensure metatable and gclist fields are at same offset as other GC types
static_assert(offsetof(GCobject, metatable) == offsetof(GCtab, metatable));
static_assert(offsetof(GCobject, gclist) == offsetof(GCtab, gclist));

// Forward declaration - defined after GCobj is complete
inline GCobject* objectref(GCRef r) noexcept;

//********************************************************************************************************************
// VM states.

enum {
   LJ_VMST_INTERP,   //  Interpreter.
   LJ_VMST_C,        //  C function.
   LJ_VMST_GC,       //  Garbage collector.
   LJ_VMST_EXIT,     //  Trace exit handler.
   LJ_VMST_RECORD,   //  Trace recorder.
   LJ_VMST_OPT,      //  Optimiser.
   LJ_VMST_ASM,      //  Assembler.
   LJ_VMST__MAX
};

#define setvmstate(g, st)   ((g)->vmstate = ~LJ_VMST_##st)

// Metamethod order determines enum values hardcoded in lj_vm.obj.
// New metamethods must be added at the end to avoid shifting indices.
// Inserting in the middle breaks all metamethod dispatch until lj_vm.obj is rebuilt.

#define MMDEF_FFI(_)
#define MMDEF_PAIRS(_) _(pairs) _(ipairs)

// X-macro defining all metamethods - used for enum generation and name strings
#define MMDEF(_) \
  _(index) _(newindex) _(gc) _(mode) _(eq) _(len) \
  /* Only the above (fast) metamethods are negative cached (max. 8). */ \
  _(lt) _(le) _(concat) _(call) \
  /* The following must be in ORDER ARITH. */ \
  _(add) _(sub) _(mul) _(div) _(mod) _(pow) _(unm) \
  /* The following are used in the standard libraries. */ \
  _(metatable) _(tostring) \
  _(close) MMDEF_FFI(_) MMDEF_PAIRS(_)

// Metamethod IDs - uses typedef enum because MMDEF generates conditional members
// and the X-macro pattern is required for string generation in lj_meta.cpp

typedef enum : uint8_t {
#define MMENUM(name)   MM_##name,
   MMDEF(MMENUM)
#undef MMENUM
   MM__MAX,
   MM____ = MM__MAX,
   MM_FAST = MM_len
} MMS;

// GC root IDs

typedef enum : uint32_t {
   GCROOT_MMNAME,                              // Metamethod names
   GCROOT_MMNAME_LAST = GCROOT_MMNAME + MM__MAX - 1,
   GCROOT_BASEMT,                              // Metatables for base types
   GCROOT_BASEMT_NUM = GCROOT_BASEMT + ~LJ_TNUMX,
   GCROOT_IO_INPUT,                            // Userdata for default I/O input file
   GCROOT_IO_OUTPUT,                           // Userdata for default I/O output file
   GCROOT_MAX
} GCRootID;

// GC root accessors - must remain as macros due to header ordering

#define basemt_it(g, it)    ((g)->gcroot[GCROOT_BASEMT + ~(it)])
#define basemt_obj(g, o)    ((g)->gcroot[GCROOT_BASEMT + itypemap(o)])
#define mmname_str(g, mm)   (strref((g)->gcroot[GCROOT_MMNAME + int(mm)]))

// Garbage collector states. Order matters.
// Using enum class for type safety; values must match GCState.state field usage.

enum class GCPhase : uint8_t {
   Pause       = 0,
   Propagate   = 1,
   Atomic      = 2,
   SweepString = 3,
   Sweep       = 4,
   Finalize    = 5
};

// Garbage collector state.

typedef struct GCState {
   GCSize  total;        // Memory currently allocated.
   GCSize  threshold;    // Memory threshold.
   uint8_t currentwhite; // Current white color.
   GCPhase state;        // GC state.
   uint8_t nocdatafin;   // No cdata finaliser called. [DEPRECATED]
   uint8_t lightudnum;   //  Number of lightuserdata segments - 1 (64-bit only).
   MSize   sweepstr;     // Sweep position in string table.
   GCRef   root;         // List of all collectable objects.
   MRef    sweep;        // Sweep position in root list.
   GCRef   gray;         // List of gray objects.
   GCRef   grayagain;    // List of objects for atomic traversal.
   GCRef   weak;         // List of weak tables (to be cleared).
   GCRef   mmudata;      // List of userdata (to be finalized).
   GCSize  debt;         // Debt (how much GC is behind schedule).
   GCSize  estimate;     // Estimate of memory actually in use.
   MSize   stepmul;      // Incremental GC step granularity.
   MSize   pause;        // Pause between successive GC cycles.
   MRef    lightudseg;   //  Upper bits of lightuserdata segments (64-bit).
} GCState;

// String interning state.
typedef struct StrInternState {
   GCRef *tab;       //  String hash table anchors.
   MSize mask;       //  String hash mask (size of hash table - 1).
   MSize num;        //  Number of strings in hash table.
   StrID id;         //  Next string ID.
   uint8_t second;   //  String interning table uses secondary hashing.
} StrInternState;

// Global state, shared by all threads of a Lua universe.
typedef struct global_State {
   lua_Alloc allocf;         // Memory allocator.
   void      *allocd;        // Memory allocator data.
   GCState   gc;             // Garbage collector.
   GCstr     strempty;       // Empty string.
   uint8_t   stremptyz;      // Zero terminator of empty string.
   uint8_t   hookmask;       // Hook mask.
   uint8_t   dispatchmode;   // Dispatch mode.
   uint8_t   vmevmask;       // VM event mask.
   StrInternState str;       // String interning.
   volatile int32_t vmstate; // VM state or current JIT code trace number.
   GCRef    mainthref;       // Link to main thread.
   SBuf     tmpbuf;          // Temporary string buffer.
   TValue   tmptv, tmptv2;   // Temporary TValues.
   Node     nilnode;         // Fallback 1-element hash part (nil key and value).
   TValue   registrytv;      // Anchor for registry.
   GCupval  uvhead;          // Head of double-linked list of all open upvalues.
   int32_t  hookcount;       // Instruction hook countdown.
   int32_t  hookcstart;      // Start count for instruction hook counter.
   lua_Hook hookf;           // Hook function.
   lua_CFunction wrapf;      // Wrapper for C function calls.
   lua_CFunction panic;      // Called as a last resort for errors.
   BCIns     bc_cfunc_int;   // Bytecode for internal C function calls.
   BCIns     bc_cfunc_ext;   // Bytecode for external C function calls.
   GCRef     cur_L;          // Currently executing lua_State.
   MRef      jit_base;       // Current JIT code L->base or NULL.
   MRef      ctype_state;    // Pointer to C type state.
   PRNGState prng;           // Global PRNG state.
   void     *funcnames;      // Map of GCproto* to function names (std::unordered_map<>*).
   GCRef     gcroot[GCROOT_MAX];  //  GC roots.
} global_State;

// Forward declarations - defined after GCobj is complete

inline lua_State* mainthread(global_State *g) noexcept;
inline TValue* niltv(lua_State *L) noexcept;
[[nodiscard]] constexpr inline bool tvisnil(cTValue *o) noexcept;  // Defined in TValue section

// niltvg doesn't depend on GCobj, can be defined here

[[nodiscard]] inline TValue* niltvg(global_State *g) noexcept {
   return check_exp(tvisnil(&g->nilnode.val), &g->nilnode.val);
}

// Hook management. Hook event masks are defined in lua.h.

inline constexpr uint8_t HOOK_EVENTMASK    = 0x0f;
inline constexpr uint8_t HOOK_ACTIVE       = 0x10;
inline constexpr int HOOK_ACTIVE_SHIFT     = 4;
inline constexpr uint8_t HOOK_VMEVENT      = 0x20;
inline constexpr uint8_t HOOK_GC           = 0x40;
inline constexpr uint8_t HOOK_PROFILE      = 0x80;

[[nodiscard]] inline bool hook_active(const global_State *g) noexcept { return (g->hookmask & HOOK_ACTIVE) != 0; }
inline void hook_enter(global_State *g) noexcept { g->hookmask |= HOOK_ACTIVE; }
inline void hook_entergc(global_State *g) noexcept { g->hookmask = (g->hookmask | (HOOK_ACTIVE | HOOK_GC)) & ~HOOK_PROFILE; }
inline void hook_vmevent(global_State *g) noexcept { g->hookmask |= (HOOK_ACTIVE | HOOK_VMEVENT); }
inline void hook_leave(global_State *g) noexcept { g->hookmask &= ~HOOK_ACTIVE; }
[[nodiscard]] inline uint8_t hook_save(const global_State *g) noexcept { return g->hookmask & ~HOOK_EVENTMASK; }
inline void hook_restore(global_State *g, uint8_t h) noexcept { g->hookmask = (g->hookmask & HOOK_EVENTMASK) | h; }

// Per-thread state object.  See lua_newstate() in lj_state.cpp for initialisation.

struct lua_State {
   GCHeader;            // NB: C++ placement new can trash any preset values here.
   uint8_t dummy_ffid;  //  Fake FF_C for curr_funcisL() on dummy frames.
   uint8_t status = LUA_OK; //  Thread status.
   MRef    glref;       //  Link to global state.
   GCRef   gclist;      //  GC chain.
   TValue  *base;       //  Base of currently executing function.
   TValue  *top;        //  First free slot in the stack.
   MRef    maxstack;    //  Last free slot in the stack.
   MRef    stack;       //  Stack base.
   GCRef   openupval;   //  List of open upvalues in the stack.
   GCRef   env;         //  Thread environment (table of globals).
   void    *cframe;     //  End of C stack frame chain.
   MSize   stacksize;   //  True stack size (incl. LJ_STACK_EXTRA).
   class objScript *script;  // Back-reference to the script that owns this lua_State
   bool    sent_traceback;   // True if traceback has been sent for the current error
   uint8_t resolving_thunk;  // Flag to prevent recursive thunk resolution
   ParserDiagnostics *parser_diagnostics; // Stores ParserDiagnostics* during parsing errors
   TipEmitter *parser_tips;               // Stores TipEmitter* during parsing for code hints
   TValue close_err;  // Current error for __close handlers (nil if no error)
   // Try-except exception handling runtime state (lazily allocated)
   TryFrameStack try_stack;      // Exception frame stack (nullptr until first BC_TRYENTER)
   const BCIns   *try_handler_pc; // Handler PC for error re-entry (set during unwind)
   CapturedStackTrace *pending_trace; // Trace captured during exception handling (for try<trace>)
   ERR    CaughtError = ERR::Okay; // Catches ERR results from module functions.

   // FileSource tracking for accurate error reporting in imported files
   std::vector<FileSource> file_sources;  // Index 0 = main file, 255 = overflow
   ankerl::unordered_dense::map<uint32_t, uint8_t> file_index_map;  // path_hash -> index

   // Stack of pending import lexers for cleanup if SEH throws during import parsing.
   // Note: Windows SEH doesn't call C++ destructors, so we track these for manual cleanup.
   // Uses void* to avoid circular include with LexState.
   std::vector<void *> pending_import_lexers;

   // Constructor/destructor not actually used as yet.
/*
   lua_State(class objScript* pScript) : Script(pScript) {

   }

   ~lua_State() {
   }
*/
};

[[nodiscard]] inline global_State * G(lua_State *L) noexcept { return mref<global_State>(L->glref); }
[[nodiscard]] inline TValue * registry(lua_State *L) noexcept { return &G(L)->registrytv; }

// Forward declarations - defined after GCobj is complete
// Functions to access the currently executing (Lua) function.

inline GCfunc * curr_func(lua_State *) noexcept;
inline bool curr_funcisL(lua_State *) noexcept;
inline GCproto * curr_proto(lua_State *) noexcept;
inline TValue * curr_topL(lua_State *) noexcept;
inline TValue * curr_top(lua_State *) noexcept;

#if defined(LUA_USE_ASSERT) || defined(LUA_USE_APICHECK)
LJ_FUNC_NORET void lj_assert_fail(global_State *g, const char* file, int line,
   const char* func, const char* fmt, ...);
#endif

//********************************************************************************************************************
// GC header for generic access to common fields of GC objects.

typedef struct GChead {
   GCHeader;
   uint8_t unused1;
   uint8_t unused2;
   GCRef env;
   GCRef gclist;
   GCRef metatable;
} GChead;

// The env field SHOULD be at the same offset for all GC objects.
static_assert(offsetof(GChead, env) == offsetof(GCfuncL, env));
static_assert(offsetof(GChead, env) == offsetof(GCudata, env));

// The metatable field MUST be at the same offset for all GC objects.
static_assert(offsetof(GChead, metatable) == offsetof(GCtab, metatable));
static_assert(offsetof(GChead, metatable) == offsetof(GCudata, metatable));

// The gclist field MUST be at the same offset for all GC objects.
static_assert(offsetof(GChead, gclist) == offsetof(lua_State, gclist));
static_assert(offsetof(GChead, gclist) == offsetof(GCproto, gclist));
static_assert(offsetof(GChead, gclist) == offsetof(GCfuncL, gclist));
static_assert(offsetof(GChead, gclist) == offsetof(GCtab, gclist));

typedef union GCobj {
   GChead    gch;
   GCstr     str;
   GCupval   uv;
   lua_State th;
   GCproto   pt;
   GCfunc    fn;
   GCtab     tab;
   GCarray   arr;
   GCudata   ud;
   GCobject  obj;  // Native Kotuku object
   ~GCobj() = delete;
} GCobj;

// Functions to convert a GCobj pointer into a specific value.

[[nodiscard]] inline GCstr *     gco_to_string(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TSTR, &o->str); }
[[nodiscard]] inline GCupval *   gco_to_upval(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TUPVAL, &o->uv); }
[[nodiscard]] inline lua_State * gco_to_thread(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TTHREAD, &o->th); }
[[nodiscard]] inline GCproto *   gco_to_proto(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TPROTO, &o->pt); }
[[nodiscard]] inline GCfunc *    gco_to_function(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TFUNC, &o->fn); }
[[nodiscard]] inline GCtab *     gco_to_table(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TTAB, &o->tab); }
[[nodiscard]] inline GCudata *   gco_to_userdata(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TUDATA, &o->ud); }
[[nodiscard]] inline GCarray *   gco_to_array(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TARRAY, &o->arr); }
[[nodiscard]] inline GCobject *  gco_to_object(GCobj *o) noexcept { return check_exp(o->gch.gct IS ~LJ_TOBJECT, &o->obj); }

// Convert any collectable object into a GCobj pointer.
template<typename T> [[nodiscard]] inline GCobj * obj2gco(T *v) noexcept { return (GCobj*)v; }

// Deferred inline function definitions (require complete GCobj type) --

// gcval: get GC object pointer from tagged value (just pointer math, no dereference)

[[nodiscard]] inline GCobj * gcval(cTValue *o) noexcept { return (GCobj*)(gcrefu(o->gcr) & LJ_GCVMASK); }

// String accessors

[[nodiscard]] inline GCstr * strref(GCRef r) noexcept { return &gcref(r)->str; }

// Prototype accessors

[[nodiscard]] inline GCstr * proto_chunk_name(const GCproto *pt) noexcept { return strref(pt->chunk_name); }
[[nodiscard]] inline const char * proto_chunk_namestr(const GCproto *pt) noexcept { return strdata(proto_chunk_name(pt)); }

// Table accessors
[[nodiscard]] inline GCtab * tabref(GCRef r) noexcept { return &gcref(r)->tab; }

// Array accessors
[[nodiscard]] inline GCarray * arrayref(GCRef r) noexcept { return &gcref(r)->arr; }

// Kotuku object accessors
[[nodiscard]] inline GCobject * objectref(GCRef r) noexcept { return &gcref(r)->obj; }

// Thread/state accessors

[[nodiscard]] inline lua_State * mainthread(global_State *g) noexcept { return &gcref(g->mainthref)->th; }

// Function accessors for currently executing function

[[nodiscard]] inline GCfunc * curr_func(lua_State *L) noexcept { return &gcval(L->base - 2)->fn; }
[[nodiscard]] inline bool curr_funcisL(lua_State *L) noexcept { return isluafunc(curr_func(L)); }
[[nodiscard]] inline GCproto * curr_proto(lua_State *L) noexcept { return funcproto(curr_func(L)); }
[[nodiscard]] inline TValue * curr_topL(lua_State *L) noexcept { return L->base + curr_proto(L)->framesize; }
[[nodiscard]] inline TValue * curr_top(lua_State *L) noexcept { return curr_funcisL(L) ? curr_topL(L) : L->top; }

// Upvalue list navigation

[[nodiscard]] inline GCupval * uvprev(GCupval *uv) noexcept { return &gcref(uv->prev)->uv; }
[[nodiscard]] inline GCupval * uvnext(GCupval *uv) noexcept { return &gcref(uv->next)->uv; }

// method_context() is for use from inline Lua methods, such as those used in the object interface design.
// This is a highly efficient equivalent to calling index2adr() on lua_upvalueindex(1) and there are no checks, so use
// cautiously.

[[nodiscard]] inline TValue * method_context(lua_State *L) { return &curr_func(L)->c.upvalue[0]; }

// niltv defined at end of file (needs tvisnil which is defined below)

//********************************************************************************************************************
// TValue getters/setters

// Type test functions - 64-bit GC64 mode only

[[nodiscard]] constexpr inline uint32_t itype(cTValue *o) noexcept { return uint32_t(o->it64 >> 47); }
[[nodiscard]] constexpr inline bool tvisnil(cTValue *o) noexcept { return o->it64 IS -1; }
[[nodiscard]] constexpr inline bool tvisfalse(cTValue *o) noexcept { return itype(o) IS LJ_TFALSE; }
[[nodiscard]] constexpr inline bool tvistrue(cTValue *o) noexcept { return itype(o) IS LJ_TTRUE; }
[[nodiscard]] constexpr inline bool tvisbool(cTValue *o) noexcept { return tvisfalse(o) or tvistrue(o); }
[[nodiscard]] constexpr inline bool tvislightud(cTValue *o) noexcept { return itype(o) IS LJ_TLIGHTUD; }
[[nodiscard]] constexpr inline bool tvisstr(cTValue *o) noexcept { return itype(o) IS LJ_TSTR; }
[[nodiscard]] constexpr inline bool tvisfunc(cTValue *o) noexcept { return itype(o) IS LJ_TFUNC; }
[[nodiscard]] constexpr inline bool tvisthread(cTValue *o) noexcept { return itype(o) IS LJ_TTHREAD; }
[[nodiscard]] constexpr inline bool tvisproto(cTValue *o) noexcept { return itype(o) IS LJ_TPROTO; }
[[nodiscard]] constexpr inline bool tvistab(cTValue *o) noexcept { return itype(o) IS LJ_TTAB; }
[[nodiscard]] constexpr inline bool tvisudata(cTValue *o) noexcept { return itype(o) IS LJ_TUDATA; }
[[nodiscard]] constexpr inline bool tvisarray(cTValue *o) noexcept { return itype(o) IS LJ_TARRAY; }
[[nodiscard]] constexpr inline bool tvisobject(cTValue *o) noexcept { return itype(o) IS LJ_TOBJECT; }
[[nodiscard]] constexpr inline bool tvisnumber(cTValue *o) noexcept { return itype(o) <= LJ_TISNUM; }
[[nodiscard]] constexpr inline bool tvisint(cTValue *o) noexcept { return LJ_DUALNUM and itype(o) IS LJ_TISNUM; }
[[nodiscard]] constexpr inline bool tvisnum(cTValue *o) noexcept { return itype(o) < LJ_TISNUM; }
[[nodiscard]] constexpr inline bool tvistruecond(cTValue *o) noexcept { return itype(o) < LJ_TISTRUECOND; }
[[nodiscard]] constexpr inline bool tvispri(cTValue *o) noexcept { return itype(o) >= LJ_TISPRI; }
[[nodiscard]] constexpr inline bool tvistabud(cTValue *o) noexcept { return itype(o) <= LJ_TISTABUD; }
[[nodiscard]] constexpr inline bool tvisgcv(cTValue *o) noexcept { return (itype(o) - LJ_TISGCV) > (LJ_TNUMX - LJ_TISGCV); }

// Special functions to test numbers for NaN, +0, -0, +1 and raw equality.

[[nodiscard]] constexpr inline bool tvisnan(cTValue *o) noexcept { return o->n != o->n; }
[[nodiscard]] constexpr inline bool tviszero(cTValue *o) noexcept { return (o->u64 << 1) IS 0; }
[[nodiscard]] constexpr inline bool tvispzero(cTValue *o) noexcept { return o->u64 IS 0; }
[[nodiscard]] constexpr inline bool tvismzero(cTValue *o) noexcept { return o->u64 IS U64x(80000000, 00000000); }
[[nodiscard]] constexpr inline bool tvispone(cTValue *o) noexcept { return o->u64 IS U64x(3ff00000, 00000000); }
[[nodiscard]] constexpr inline bool rawnumequal(cTValue* o1, cTValue* o2) noexcept { return o1->u64 IS o2->u64; }

// Convert internal type to type map index - 64-bit GC64 mode

[[nodiscard]] constexpr inline uint32_t itypemap(cTValue *o) noexcept { return tvisnumber(o) ? ~LJ_TNUMX : ~itype(o); }

// Functions to get tagged values (gcval is defined in deferred section above)

[[nodiscard]] inline int boolV(cTValue *o) noexcept { return check_exp(tvisbool(o), int(LJ_TFALSE - itype(o))); }

// Lightuserdata segment/offset extraction - 64-bit only

[[nodiscard]] constexpr inline uint64_t lightudseg(uint64_t u) noexcept { return (u >> LJ_LIGHTUD_BITS_LO) & ((1 << LJ_LIGHTUD_BITS_SEG) - 1); }
[[nodiscard]] constexpr inline uint64_t lightudlo(uint64_t u) noexcept { return u & ((uint64_t(1) << LJ_LIGHTUD_BITS_LO) - 1); }
[[nodiscard]] constexpr inline uint32_t lightudup(uint64_t p) noexcept { return uint32_t((p >> LJ_LIGHTUD_BITS_LO) << (LJ_LIGHTUD_BITS_LO - 32)); }

// Lightuserdata value extraction - 64-bit only

[[nodiscard]] static LJ_AINLINE void* lightudV(global_State *g, cTValue* o) noexcept
{
   uint64_t u = o->u64;
   uint64_t seg = lightudseg(u);
   uint32_t* segmap = mref<uint32_t>(g->gc.lightudseg);
   lj_assertG(tvislightud(o), "lightuserdata expected");
   lj_assertG(seg <= g->gc.lightudnum, "bad lightuserdata segment %d", seg);
   return (void*)(((uint64_t)segmap[seg] << 32) | lightudlo(u));
}

[[nodiscard]] inline GCobj * gcV(cTValue *o) noexcept { return check_exp(tvisgcv(o), gcval(o)); }
[[nodiscard]] inline GCstr * strV(cTValue *o) noexcept { return check_exp(tvisstr(o), &gcval(o)->str); }
[[nodiscard]] inline GCfunc* funcV(cTValue *o) noexcept { return check_exp(tvisfunc(o), &gcval(o)->fn); }
[[nodiscard]] inline lua_State * threadV(cTValue *o) noexcept { return check_exp(tvisthread(o), &gcval(o)->th); }
[[nodiscard]] inline GCproto * protoV(cTValue *o) noexcept { return check_exp(tvisproto(o), &gcval(o)->pt); }
[[nodiscard]] inline GCtab * tabV(cTValue *o) noexcept { return check_exp(tvistab(o), &gcval(o)->tab); }
[[nodiscard]] inline GCudata * udataV(cTValue *o) noexcept { return check_exp(tvisudata(o), &gcval(o)->ud); }
[[nodiscard]] inline GCarray * arrayV(cTValue *o) noexcept { return check_exp(tvisarray(o), &gcval(o)->arr); }
[[nodiscard]] inline GCarray * arrayV(lua_State *L, int Arg) noexcept { return arrayV(L->base + Arg - 1); }
[[nodiscard]] inline GCobject * objectV(cTValue *o) noexcept { return check_exp(tvisobject(o), &gcval(o)->obj); }
[[nodiscard]] inline GCobject * objectV(lua_State *L, int Arg) noexcept { return objectV(L->base + Arg - 1); }
[[nodiscard]] inline lua_Number numV(cTValue *o) noexcept { return check_exp(tvisnum(o), o->n); }
[[nodiscard]] inline int32_t intV(cTValue *o) noexcept { return check_exp(tvisint(o), int32_t(o->i)); }

// Functions to set tagged values.

inline void setitype(TValue* o, uint32_t i) noexcept { o->it = i << 15; }
inline void setnilV(TValue* o) noexcept { o->it64 = -1; }

inline void setpriV(TValue* o, uint32_t x) noexcept {
   // Avoid C4319 warning by doing negation at 64-bit level: use ~ on the shifted value.
   o->it64 = int64_t(~(((uint64_t(x) ^ 0xFFFFFFFFULL)) << 47));
}

inline void setboolV(TValue* o, int x) noexcept { o->it64 = int64_t(~(uint64_t(x + 1) << 47)); }

inline void setrawlightudV(TValue* o, void* p) { o->u64 = (uint64_t)p | (((uint64_t)LJ_TLIGHTUD) << 47); }

#define contptr(f)     ((void *)(f))
#define setcont(o, f)  ((o)->u64 = (uint64_t)(uintptr_t)contptr(f))

LJ_DATA const char* const lj_obj_itypename[];

inline void checklivetv(lua_State* L, TValue* o, const char* msg) noexcept
{
#if LUA_USE_ASSERT
   if (tvisgcv(o)) {
      auto tv_type = (uint32_t)~itype(o);
      auto gc_type = (uint32_t)gcval(o)->gch.gct;
      CSTRING tv_name = (tv_type <= (uint32_t)~LJ_TNUMX) ? lj_obj_itypename[tv_type] : "invalid";
      CSTRING gc_name = (gc_type <= (uint32_t)~LJ_TNUMX) ? lj_obj_itypename[gc_type] : "invalid";
      lj_assertL(tv_type IS gc_type,
         "%s: TValue type %d (%s) vs GC type %d (%s), GCobj %p, TValue %p, marked 0x%02x",
         msg, tv_type, tv_name, gc_type, gc_name, (void*)gcval(o), (void*)o, gcval(o)->gch.marked);
      // Copy of isdead check from lj_gc.h to avoid circular include.
      lj_assertL(!(gcval(o)->gch.marked & (G(L)->gc.currentwhite ^ 3) & 3),
         "%s: dead GC object %p, type %d (%s), marked 0x%02x, currentwhite %d",
         msg, (void*)gcval(o), gc_type, gc_name, gcval(o)->gch.marked, G(L)->gc.currentwhite);
   }
#endif
}

inline void setgcVraw(TValue* o, GCobj* v, uint32_t IType) noexcept
{
   setgcreft(o->gcr, v, IType);
}

inline void setgcV(lua_State* L, TValue* o, GCobj* v, uint32_t It) noexcept
{
   setgcVraw(o, v, It);
   checklivetv(L, o, "store to dead GC object");
}

inline void setstrV(lua_State* L, TValue* o, const GCstr* v) noexcept
{
   setgcV(L, o, obj2gco(v), LJ_TSTR);
}

inline void setthreadV(lua_State* L, TValue* o, const lua_State* v) noexcept
{
   setgcV(L, o, obj2gco(v), LJ_TTHREAD);
}

inline void setprotoV(lua_State* L, TValue* o, const GCproto* v) noexcept
{
   setgcV(L, o, obj2gco(v), LJ_TPROTO);
}

inline void setfuncV(lua_State* L, TValue* o, const GCfunc* v) noexcept
{
   setgcV(L, o, obj2gco(v), LJ_TFUNC);
}

inline void settabV(lua_State* L, TValue* o, const GCtab* v) noexcept { setgcV(L, o, obj2gco(v), LJ_TTAB); }
inline void setudataV(lua_State* L, TValue* o, const GCudata* v) noexcept { setgcV(L, o, obj2gco(v), LJ_TUDATA); }
inline void setarrayV(lua_State* L, TValue* o, const GCarray* v) noexcept { setgcV(L, o, obj2gco(v), LJ_TARRAY); }
inline void setobjectV(lua_State* L, TValue* o, const GCobject* v) noexcept { setgcV(L, o, obj2gco(v), LJ_TOBJECT); }
constexpr inline void setnumV(TValue* o, lua_Number x) noexcept { o->n = x; }
inline void setnanV(TValue* o) noexcept { o->u64 = U64x(fff80000, 00000000); }
inline void setpinfV(TValue* o) noexcept { o->u64 = U64x(7ff00000, 00000000); }
inline void setminfV(TValue* o) noexcept { o->u64 = U64x(fff00000, 00000000); }

inline void setintV(TValue* o, int32_t i) noexcept {
#if LJ_DUALNUM
   o->i = uint32_t(i);
   setitype(o, LJ_TISNUM);
#else
   o->n = lua_Number(i);
#endif
}

inline void setint64V(TValue* o, int64_t i) noexcept {
   if (LJ_DUALNUM and LJ_LIKELY(i IS int64_t(int32_t(i)))) setintV(o, int32_t(i));
   else setnumV(o, lua_Number(i));
}

inline void setintptrV(TValue* o, intptr_t i) noexcept { setint64V(o, i); }

// Copy tagged values.
inline void copyTV(lua_State* L, TValue* o1, const TValue* o2)
{
   *o1 = *o2;
#if LUA_USE_ASSERT
   if (tvisgcv(o1)) {
      auto gc_type = (uint32_t)gcval(o1)->gch.gct;
      if (~itype(o1) != gc_type or gc_type > (uint32_t)~LJ_TNUMX) {
         const char* location = "unknown";
         ptrdiff_t stack_offset = -1;
         TValue* stack_base = tvref(L->stack);
         TValue* stack_end = mref<TValue>(L->maxstack) + 1;
         if ((char*)o2 >= (char*)stack_base and (char*)o2 < (char*)stack_end) {
            location = "stack";
            stack_offset = o2 - stack_base;
         }
         else if ((char*)o1 >= (char*)stack_base and (char*)o1 < (char*)stack_end) {
            location = "stack(dst)";
            stack_offset = o1 - stack_base;
         }
         ptrdiff_t base_offset = (L->base) ? (o2 - L->base) : -1;
         fprintf(stderr, "[checklivetv] copyTV stale ref: src %p (%s, stack_off=%td, base_off=%td) -> dst %p, "
            "L->base=%p, L->top=%p, nframes_approx=%td\n",
            (const void*)o2, location, stack_offset, base_offset,
            (void*)o1, (void*)L->base, (void*)L->top, L->top - L->base);
      }
   }
#endif
   checklivetv(L, o1, "copy of dead GC object");
}

//********************************************************************************************************************
// Domain specific context helpers

[[nodiscard]] inline GCobject * object_context(lua_State *L) { return objectV(&curr_func(L)->c.upvalue[0]); }

//********************************************************************************************************************
// Number to integer conversion

[[nodiscard]] inline int32_t lj_num2bit(lua_Number n) noexcept
{
   TValue o;
   o.n = n + 6755399441055744.0;  //  2^52 + 2^51
   return (int32_t)o.u32.lo;
}

[[nodiscard]] constexpr inline int32_t lj_num2int(lua_Number n) noexcept
{
   return int32_t(n); // Expected to compile to a cvttsd2si instruction or equivalent
}

// This must match the JIT backend behavior. In particular for archs that don't have a common hardware instruction for
// this conversion.  Note that signed FP to unsigned int conversions have an undefined result and should never be
// relied upon in portable FFI code.  See also: C99 or C11 standard, 6.3.1.4, footnote of (1).

[[nodiscard]] inline uint64_t lj_num2u64(lua_Number n) noexcept {
#if LJ_TARGET_X86ORX64
   int64_t i = (int64_t)n;
   if (i < 0) i = (int64_t)(n - 18446744073709551616.0);
   return (uint64_t)i;
#else
   return (uint64_t)n;
#endif
}

[[nodiscard]] inline int32_t numberVint(cTValue *o) noexcept {
   if (LJ_LIKELY(tvisint(o))) return intV(o);
   else return lj_num2int(numV(o));
}

[[nodiscard]] inline lua_Number numberVnum(cTValue *o) noexcept {
   if (LJ_UNLIKELY(tvisint(o))) return (lua_Number)intV(o);
   else return numV(o);
}

// Names and maps for internal and external object tags.
LJ_DATA const char* const lj_obj_typename[1 + LUA_TARRAY + 1];
LJ_DATA const char* const lj_obj_itypename[~LJ_TNUMX + 1];

[[nodiscard]] inline const char* lj_typename(cTValue *o) noexcept {
   return lj_obj_itypename[itypemap(o)];
}

// Compare two objects without calling metamethods.
LJ_FUNC int lj_obj_equal(cTValue* o1, cTValue* o2);
LJ_FUNC const void* lj_obj_ptr(global_State *g, cTValue* o);

// Late deferred function definitions (need tvisnil, G)

[[nodiscard]] inline TValue* niltv(lua_State *L) noexcept {
   return check_exp(tvisnil(&G(L)->nilnode.val), &G(L)->nilnode.val);
}
