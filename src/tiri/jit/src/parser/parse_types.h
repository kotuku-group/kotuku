// Lua parser - Type definitions and structures.
//
// Copyright (C) 2025-2026 Paul Manias

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <concepts>
#include <type_traits>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <variant>
#include <ranges>
#include "../bytecode/lj_bc.h"

// Forward declarations
class LexState;

// Expression kinds.

enum class ExpKind : uint8_t {
   // Constant expressions must be first and in this order:
   Nil,
   False,
   True,
   Str,        // sval = string value
   Num,        // nval = number value
   Last = Num,
   // Non-constant expressions follow:
   Local,      // info = local register, aux = vstack index
   Upval,      // info = upvalue index, aux = vstack index
   Global,     // sval = string value (explicit global or known global reference)
   Unscoped,   // sval = string value (undeclared variable - scope determined by context)
   Indexed,    // info = table register, aux = index reg/byte/string const
   IndexedArray, // info = array register, aux = index reg/byte (array indexing)
   SafeIndexedArray, // info = array register, aux = index reg/byte (safe array indexing - nil for out-of-bounds)
   IndexedObject, // info = object register, aux = string const (object field access)
   Jmp,        // info = instruction PC
   Relocable,  // info = instruction PC
   NonReloc,   // info = result register
   Call,       // info = instruction PC, aux = base
   Void
};

// Expression kind helper function - returns true for variable-like expressions.
// Note: Unscoped is between Global and Indexed, so this range check covers it.
// IndexedArray, SafeIndexedArray, and IndexedObject are also considered variable-like expressions for assignment purposes.
[[nodiscard]] static constexpr bool vkisvar(ExpKind k) {
   return ExpKind::Local <= k and k <= ExpKind::IndexedObject;
}

enum class ExprFlag : uint8_t {
   None = 0x00u,
   PostfixIncStmt = 0x01u,
   HasRhsReg = 0x02u,
   BitwiseBase = 0x04u  // aux contains base register for bitwise call frame
};

enum class FuncScopeFlag : uint8_t {
   None = 0x00u,
   Loop = 0x01u,
   Break = 0x02u,
   Upvalue = 0x08u,
   NoClose = 0x10u,
   Continue = 0x20u
};

enum class VarInfoFlag : uint8_t {
   None = 0x00u,
   VarReadWrite = 0x01u,
   Jump = 0x02u,
   JumpTarget = 0x04u,
   Defer = 0x08u,
   DeferArg = 0x10u,
   Close = 0x20u,
   Const = 0x40u  // Variable is const (cannot be reassigned)
};

// Concept for flag types that support bitwise operations
template<typename Flag>
concept FlagType = std::same_as<Flag, ExprFlag> or
                   std::same_as<Flag, FuncScopeFlag> or
                   std::same_as<Flag, VarInfoFlag>;

template<FlagType Flag> [[nodiscard]] static constexpr Flag operator|(Flag Left, Flag Right) {
   using Underlying = std::underlying_type_t<Flag>;
   return Flag(Underlying(Left) | Underlying(Right));
}

template<FlagType Flag> [[nodiscard]] static constexpr Flag operator&(Flag Left, Flag Right) {
   using Underlying = std::underlying_type_t<Flag>;
   return Flag(Underlying(Left) & Underlying(Right));
}

template<FlagType Flag> [[nodiscard]] static constexpr Flag operator~(Flag Value) {
   using Underlying = std::underlying_type_t<Flag>;
   return Flag(~Underlying(Value));
}

template<FlagType Flag> static constexpr Flag & operator|=(Flag &Left, Flag Right) { return Left = Left | Right; }
template<FlagType Flag> static constexpr Flag & operator&=(Flag &Left, Flag Right) { return Left = Left & Right; }
template<FlagType Flag> [[nodiscard]] static constexpr bool has_flag(Flag Flags, Flag Mask) { return (Flags & Mask) != Flag::None; }

// Enhanced flag utilities for clearer flag handling
template<FlagType Flag> [[nodiscard]] static constexpr bool has_any(Flag Flags, Flag Mask) {
   return (Flags & Mask) != Flag::None;
}

template<FlagType Flag> [[nodiscard]] static constexpr bool has_all(Flag Flags, Flag Mask) {
   return (Flags & Mask) IS Mask;
}

template<FlagType Flag> static constexpr void clear_flag(Flag &Flags, Flag Mask) {
   Flags = Flags & ~Mask;
}

// Strong index types for type-safe register, position, and variable indices.
// Uses C++20 three-way comparison for automatic generation of all six comparison operators.

template<typename Tag, typename T>
struct StrongIndex {
   T value;

   constexpr StrongIndex() = default;
   constexpr explicit StrongIndex(T v) : value(v) {}
   constexpr T raw() const { return value; }

   // Implicit conversion to underlying type for ergonomic usage
   // This allows: int(bcpos), printf("%d", bcreg), bcpos >= 1, etc.
   // while still preventing implicit construction from raw types
   constexpr operator T() const { return value; }

   auto operator<=>(const StrongIndex&) const = default;
   bool operator==(const StrongIndex&) const = default;
};

// Arithmetic operators for StrongIndex types

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator+(StrongIndex<Tag, T> a, T offset) {
   return StrongIndex<Tag, T>(a.value + offset);
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator+(StrongIndex<Tag, T> a, StrongIndex<Tag, T> offset) {
   return StrongIndex<Tag, T>(a.value + offset.value);
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator-(StrongIndex<Tag, T> a, T offset) {
   return StrongIndex<Tag, T>(a.value - offset);
}

template<typename Tag, typename T>
constexpr T operator-(StrongIndex<Tag, T> a, StrongIndex<Tag, T> b) {
   return a.value - b.value;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T>& operator++(StrongIndex<Tag, T>& a) {
   ++a.value;
   return a;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator++(StrongIndex<Tag, T>& a, int) {
   auto old = a;
   ++a.value;
   return old;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T>& operator--(StrongIndex<Tag, T>& a) {
   --a.value;
   return a;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T> operator--(StrongIndex<Tag, T>& a, int) {
   auto old = a;
   --a.value;
   return old;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T>& operator+=(StrongIndex<Tag, T>& a, T offset) {
   a.value += offset;
   return a;
}

template<typename Tag, typename T>
constexpr StrongIndex<Tag, T>& operator-=(StrongIndex<Tag, T>& a, T offset) {
   a.value -= offset;
   return a;
}

// Strong type aliases using distinct tag types

using BCPos = StrongIndex<struct BCPosTag, BCPOS>;
using BCReg = StrongIndex<struct BCRegTag, BCREG>;

// Expression descriptor.

struct ExpDesc {
   union {
      struct { // For non-constant expressions like Local, Upval, Global, Indexed, Jmp, Relocable, NonReloc, Call, Void
         uint32_t info;  // Primary info.
         uint32_t aux;   // Secondary info.
      } s;
      TValue nval;   // ExpKind::Num number value.
      GCstr* sval;   // ExpKind::Str string value.
   } u;
   ExpKind k;      // Expression kind.
   ExprFlag flags; // Expression flags.
   TiriType result_type = TiriType::Unknown;  // Known result type (for Call: callee's first return type)
   CLASSID object_class_id = CLASSID::NIL; // CLASSID for Object result types
   bool type_confirmed = false;  // True if result_type is confirmed from class dictionary lookup
   BCPOS t;        // True condition jump list.
   BCPOS f;        // False condition jump list.

   // Constructors
   constexpr ExpDesc() : u{}, k(ExpKind::Void), flags(ExprFlag::None), result_type(TiriType::Unknown), t(NO_JMP), f(NO_JMP) {}

   constexpr ExpDesc(ExpKind Kind, uint32_t Info = 0) : u{}, k(Kind), flags(ExprFlag::None), result_type(TiriType::Unknown), t(NO_JMP), f(NO_JMP) {
      this->u.s.info = Info;
      this->u.s.aux = 0;
   }

   explicit constexpr ExpDesc(GCstr* Value) : u{}, k(ExpKind::Str), flags(ExprFlag::None), result_type(TiriType::Str), t(NO_JMP), f(NO_JMP) {
      this->u.sval = Value;
   }

   explicit constexpr ExpDesc(lua_Number Value) : u{}, k(ExpKind::Num), flags(ExprFlag::None), result_type(TiriType::Num), t(NO_JMP), f(NO_JMP) {
      setnumV(&this->u.nval, Value);
   }

   explicit constexpr ExpDesc(bool Value) : u{}, k(Value ? ExpKind::True : ExpKind::False), flags(ExprFlag::None), result_type(TiriType::Bool), t(NO_JMP), f(NO_JMP) {
      this->u.s.info = 0;
      this->u.s.aux = 0;
   }

   // Member methods for expression queries and manipulation
   [[nodiscard]] inline bool has_jump() const { return this->t != this->f; }
   [[nodiscard]] inline bool is_constant() const { return this->k <= ExpKind::Last; }
   [[nodiscard]] inline bool is_constant_nojump() const { return this->is_constant() and not this->has_jump(); }
   [[nodiscard]] inline bool is_num_constant() const { return this->k == ExpKind::Num; }
   [[nodiscard]] inline bool is_num_constant_nojump() const { return this->is_num_constant() and not this->has_jump(); }
   [[nodiscard]] inline bool is_str_constant() const { return this->k == ExpKind::Str; }
   [[nodiscard]] inline lua_Number number_value() { return numberVnum(this->num_tv()); }
   [[nodiscard]] inline bool is_nil() const { return this->k IS ExpKind::Nil; }
   [[nodiscard]] inline bool is_false() const { return this->k IS ExpKind::False; }
   [[nodiscard]] inline bool is_true() const { return this->k IS ExpKind::True; }
   [[nodiscard]] inline bool is_string() const { return this->k IS ExpKind::Str; }
   [[nodiscard]] inline bool is_number() const { return this->k IS ExpKind::Num; }
   [[nodiscard]] inline bool is_local() const { return this->k IS ExpKind::Local; }
   [[nodiscard]] inline bool is_upvalue() const { return this->k IS ExpKind::Upval; }
   [[nodiscard]] inline bool is_global() const { return this->k IS ExpKind::Global; }
   [[nodiscard]] inline bool is_indexed() const { return this->k IS ExpKind::Indexed; }
   [[nodiscard]] inline bool is_indexed_array() const { return this->k IS ExpKind::IndexedArray; }
   [[nodiscard]] inline bool is_safe_indexed_array() const { return this->k IS ExpKind::SafeIndexedArray; }
   [[nodiscard]] inline bool is_any_indexed() const { return this->k IS ExpKind::Indexed or this->k IS ExpKind::IndexedArray or this->k IS ExpKind::SafeIndexedArray; }
   [[nodiscard]] inline bool is_register() const { return this->k IS ExpKind::Local or this->k IS ExpKind::NonReloc; }

   // Extended falsey check (nil, false, 0, "")
   // Supports Tiri's extended falsey semantics for ?? operator
   [[nodiscard]] bool is_falsey() const;

   [[nodiscard]] inline TValue* num_tv() {
      lj_assertX(this->is_num_constant(), "expr must be number constant");
      return &this->u.nval;
   }

   inline void init(ExpKind kind, uint32_t info) {
      this->k = kind;
      this->u.s.info = info;
      this->flags = ExprFlag::None;
      this->result_type = TiriType::Unknown;
      this->f = this->t = NO_JMP;
   }

   [[nodiscard]] inline bool is_num_zero() {
      TValue* o = this->num_tv();
      return tvisint(o) ? (intV(o) == 0) : tviszero(o);
   }
};

// Per-function linked list of scope blocks.
// Design: FuncScope is always stack-allocated at call sites, so parent scopes naturally outlive
// child scopes via C++ stack semantics. The raw `prev` pointer is intentional for zero-overhead
// traversal without ownership concerns. Lifecycle is managed by ScopeGuard RAII wrapper.

struct FuncScope {
   FuncScope *prev;        // Link to outer scope (non-owning, stack guarantees validity).
   MSize vstart;           // Start of block-local variables.
   uint8_t nactvar;        // Number of active vars outside the scope.
   FuncScopeFlag flags;    // Scope flags.
};

// Type-safe special variable names to replace legacy sentinel pointers.

enum class SpecialName : uint8_t { None, Break, Continue, Blank };

struct VarName {
   std::variant<SpecialName, GCstr*> value;

   constexpr VarName() : value(SpecialName::None) {}
   constexpr VarName(SpecialName special) : value(special) {}
   constexpr VarName(GCstr* str) : value(str) {}

   [[nodiscard]] constexpr bool is_special() const { return std::holds_alternative<SpecialName>(value); }
   [[nodiscard]] constexpr bool is_break() const { return is_special() and std::get<SpecialName>(value) IS SpecialName::Break; }
   [[nodiscard]] constexpr bool is_continue() const { return is_special() and std::get<SpecialName>(value) IS SpecialName::Continue; }
   [[nodiscard]] constexpr bool is_blank() const { return is_special() and std::get<SpecialName>(value) IS SpecialName::Blank; }
   [[nodiscard]] constexpr GCstr* as_string() const { return std::get<GCstr*>(value); }

   [[nodiscard]] constexpr bool operator==(const VarName& other) const = default;
   [[nodiscard]] constexpr bool operator==(GCstr* str) const {
      return not is_special() and as_string() IS str;
   }
};

// Legacy sentinel pointers for backward compatibility during transition.
inline GCstr * const NAME_BREAK    = (GCstr*)uintptr_t(1);
inline GCstr * const NAME_CONTINUE = (GCstr*)uintptr_t(2);
inline GCstr * const NAME_BLANK    = (GCstr*)uintptr_t(3);

// Index into variable stack.
typedef uint16_t VarIndex;
inline constexpr int LJ_MAX_VSTACK = (65536 - LJ_MAX_UPVAL);

// Strong type for variable slot indices (defined after VarIndex)
using VarSlot = StrongIndex<struct VarSlotTag, VarIndex>;

// Variable info flags are defined in VarInfoFlag.

#include "func_state.h"

// Parser assertion macro - expands __FILE__/__LINE__ at call site for accurate error locations.
// Usage: fs_check_assert(fs, condition, "format string", args...);
// Note: Cannot be a method because __FILE__/__LINE__ would resolve inside the method, not at call site.
#ifdef LUA_USE_ASSERT
   #define fs_check_assert(fs, c, ...) lj_assertG_(G((fs)->L), (c), __VA_ARGS__)
#else
   #define fs_check_assert(fs, c, ...) ((void)(fs))
#endif

// Binary and unary operators. ORDER OPR

enum class BinOpr : int8_t {
   Add, Sub, Mul, Div, Mod, Pow,  // ORDER ARITH
   Concat,
   NotEqual, Equal,
   LessThan, GreaterEqual, LessEqual, GreaterThan,
   BitAnd, BitOr, BitXor, ShiftLeft, ShiftRight,
   LogicalAnd, LogicalOr, IfEmpty,
   HasFlag,
   Ternary,
   None
};

// Arithmetic offset helper for bytecode generation
[[nodiscard]] constexpr int to_arith_offset(BinOpr op) {
   return int(op) - int(BinOpr::Add);
}

// Operator classification helpers (non-template versions for parse_types.h)
// Template versions with concept constraints are in parse_internal.h

[[nodiscard]] constexpr bool is_arithmetic_op(BinOpr op) {
   return op >= BinOpr::Add and op <= BinOpr::Pow;
}

[[nodiscard]] constexpr bool is_comparison_op(BinOpr op) {
   return op >= BinOpr::NotEqual and op <= BinOpr::GreaterThan;
}

[[nodiscard]] constexpr bool is_bitwise_op(BinOpr op) {
   return op >= BinOpr::BitAnd and op <= BinOpr::ShiftRight;
}

[[nodiscard]] constexpr bool is_logical_op(BinOpr op) {
   return op IS BinOpr::LogicalAnd or op IS BinOpr::LogicalOr or op IS BinOpr::IfEmpty;
}

// Verify bytecode opcodes maintain correct offsets relative to their operator counterparts.

static_assert((int)BC_ISGE - (int)BC_ISLT == int(BinOpr::GreaterEqual) - int(BinOpr::LessThan), "BC_ISGE offset mismatch");
static_assert((int)BC_ISLE - (int)BC_ISLT == int(BinOpr::LessEqual) - int(BinOpr::LessThan), "BC_ISLE offset mismatch");
static_assert((int)BC_ISGT - (int)BC_ISLT == int(BinOpr::GreaterThan) - int(BinOpr::LessThan), "BC_ISGT offset mismatch");
static_assert((int)BC_SUBVV - (int)BC_ADDVV == int(BinOpr::Sub) - int(BinOpr::Add), "BC_SUBVV offset mismatch");
static_assert((int)BC_MULVV - (int)BC_ADDVV == int(BinOpr::Mul) - int(BinOpr::Add), "BC_MULVV offset mismatch");
static_assert((int)BC_DIVVV - (int)BC_ADDVV == int(BinOpr::Div) - int(BinOpr::Add), "BC_DIVVV offset mismatch");
static_assert((int)BC_MODVV - (int)BC_ADDVV == int(BinOpr::Mod) - int(BinOpr::Add), "BC_MODVV offset mismatch");

// Return bytecode encoding for primitive constant.

[[nodiscard]] static constexpr ExpKind const_pri(const ExpDesc* e) {
   lj_assertX(e->k <= ExpKind::True, "Bad constant primitive");
   return e->k;
}

// Register manipulation helpers (non-template versions for parse_types.h)
// Template versions with concept constraints are in parse_internal.h

[[nodiscard]] constexpr bool is_valid_register(BCREG reg) {
   return reg < NO_REG;
}

[[nodiscard]] constexpr bool is_valid_jump(BCPOS pos) {
   return pos != NO_JMP;
}

[[nodiscard]] constexpr BCREG next_register(BCREG reg) {
   return reg + 1;
}

[[nodiscard]] static inline bool tvhaskslot(const TValue* o) { return o->u32.hi == 0; }
[[nodiscard]] static inline uint32_t tvkslot(const TValue* o) { return o->u32.lo; }
