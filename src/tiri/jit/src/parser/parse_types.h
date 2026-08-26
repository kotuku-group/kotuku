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
#include "../runtime/lj_contract.h"
#include "static_type_descriptor.h"
#include "strong_index.h"

// Forward declarations
class LexState;

// Expression kinds.

enum class ExpKind : uint8_t {
   // Primitive constants retain their bytecode encoding.  Category membership is defined by the predicates below,
   // rather than by enum position.
   Nil,
   False,
   True,
   Str,        // sval = string value
   Num,        // nval = number value
   // Non-constant expressions:
   Local,      // info = local register, aux = vstack index
   Upval,      // info = upvalue index, aux = vstack index
   Global,     // sval = string value (explicit global or known global reference)
   Unscoped,   // sval = string value (undeclared variable - scope determined by context)
   Indexed,    // info = table register, aux = index reg/byte/string const
   IndexedArray, // info = array register, aux = index reg/byte (array indexing)
   SafeIndexedArray, // info = array register, aux = index reg/byte (safe array indexing - nil for out-of-bounds)
   IndexedObject, // info = object register, aux = string const (object field access)
   IndexedStruct, // info = struct register, aux = string const (struct field access)
   Jmp,        // info = instruction PC
   Relocable,  // info = instruction PC
   NonReloc,   // info = result register
   Call,       // info = instruction PC, aux = base
   Void
};

[[nodiscard]] static constexpr bool expkind_is_primitive(ExpKind Kind) noexcept
{
   return Kind IS ExpKind::Nil or Kind IS ExpKind::False or Kind IS ExpKind::True;
}

[[nodiscard]] static constexpr bool expkind_is_constant(ExpKind Kind) noexcept
{
   return expkind_is_primitive(Kind) or Kind IS ExpKind::Str or Kind IS ExpKind::Num;
}

// Variable-like expressions may appear on the left of an assignment.  Keep this list explicit: adding an ExpKind
// must not silently change assignment semantics because of its ordinal position.
[[nodiscard]] static constexpr bool expkind_is_variable_like(ExpKind Kind) noexcept
{
   switch (Kind) {
      case ExpKind::Local:
      case ExpKind::Upval:
      case ExpKind::Global:
      case ExpKind::Unscoped:
      case ExpKind::Indexed:
      case ExpKind::IndexedArray:
      case ExpKind::SafeIndexedArray:
      case ExpKind::IndexedObject:
      case ExpKind::IndexedStruct:
         return true;
      default:
         return false;
   }
}

// BC_KPRI encodes these values directly.  Their values are deliberately asserted rather than inferred from the
// declaration order used by the category predicates above.
static_assert(uint8_t(ExpKind::Nil) IS 0u);
static_assert(uint8_t(ExpKind::False) IS 1u);
static_assert(uint8_t(ExpKind::True) IS 2u);

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

// Transient parser-side form of a runtime contract.  The raw structure definition is used only while compiling;
// bcemit_contract() serialises its stable name into an interned descriptor before emitting bytecode.

struct RuntimeContract {
   TiriType type = TiriType::Unknown;
   CLASSID object_class_id = CLASSID::NIL;
   struct_record *struct_def = nullptr;
   ArrayElementDescriptor array_element{};
   GCstr *label = nullptr;
   ContractBoundary boundary = ContractBoundary::Local;
   uint8_t position = 0;
   bool nullable = true;
   bool required = false;
   bool is_const = false;
   bool initialising = false;
   bool global_hint = false;
   bool retained_value = false;
};

// Associates a transient contract with the register it validates.  Dense batching uses the register separately from
// RuntimeContract::position because the latter is stable diagnostic metadata rather than a relative stack offset.

struct RuntimeContractSlot {
   BCREG register_index = 0;
   RuntimeContract contract;
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

// Strong type aliases using distinct tag types

using BCPos = StrongIndex<struct BCPosTag, BCPOS>;
using BCReg = StrongIndex<struct BCRegTag, BCREG>;

// Indexed expressions retain the compact bytecode-friendly representation in ExpDesc::u.s.aux.  This wrapper owns
// its interpretation so parser and emitter code cannot mistake a constant for a register.
enum class IndexOperandKind : uint8_t {
   Register,
   ByteConstant,
   StringConstant
};

class IndexOperand {
public:
   constexpr explicit IndexOperand(uint32_t Storage) : storage_(Storage) {}

   [[nodiscard]] static constexpr IndexOperand register_index(BCREG Register) { return IndexOperand(Register); }
   [[nodiscard]] static constexpr IndexOperand byte_constant(BCREG Value) {
      return IndexOperand(BCMAX_C + 1 + Value);
   }
   [[nodiscard]] static constexpr IndexOperand string_constant(BCREG Constant) {
      return IndexOperand(~uint32_t(Constant));
   }

   [[nodiscard]] constexpr IndexOperandKind kind() const {
      if (int32_t(storage_) < 0) return IndexOperandKind::StringConstant;
      if (storage_ > BCMAX_C) return IndexOperandKind::ByteConstant;
      return IndexOperandKind::Register;
   }
   [[nodiscard]] constexpr bool is_register() const { return this->kind() IS IndexOperandKind::Register; }
   [[nodiscard]] constexpr bool is_numeric() const { return this->kind() != IndexOperandKind::StringConstant; }
   [[nodiscard]] constexpr bool is_string_constant() const { return this->kind() IS IndexOperandKind::StringConstant; }
   [[nodiscard]] constexpr BCREG register_index() const { return BCREG(storage_); }
   [[nodiscard]] constexpr BCREG byte_constant() const { return BCREG(storage_ - (BCMAX_C + 1)); }
   [[nodiscard]] constexpr BCREG string_constant() const { return BCREG(~storage_); }
   [[nodiscard]] constexpr uint32_t raw() const { return storage_; }

private:
   uint32_t storage_;
};

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
   struct_record *struct_def = nullptr; // Resolved layout for Struct result types
   StaticValueHandle static_value{};
   StaticResultSetHandle static_results{};
   uint32_t struct_field_index = 0xFFFFFFFFu; // Pre-resolved field index for STGETF/STSETF
   BCPOS alternate_call = NO_JMP; // Alternate call block used by runtime built-in method dispatch
   BCPOS safe_nil_init = NO_JMP; // Nil initialiser for a skipped safe call; widened with fixed result requests
   BCPOS alternate_safe_nil_init = NO_JMP; // Additional nil block used by runtime method fallback dispatch
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
   [[nodiscard]] inline bool is_constant() const { return expkind_is_constant(this->k); }
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

   // Extended falsey check (nil, false, 0, ""; empty collections are runtime-only)
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
      this->static_value = {};
      this->static_results = {};
      this->alternate_call = NO_JMP;
      this->safe_nil_init = NO_JMP;
      this->alternate_safe_nil_init = NO_JMP;
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
   uint16_t context_block; // Temporary context descriptor, or UINT16_MAX.
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
   Approx,
   Contains,
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
   return (op >= BinOpr::NotEqual and op <= BinOpr::GreaterThan) or op IS BinOpr::Approx or op IS BinOpr::Contains;
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
   lj_assertX(expkind_is_primitive(e->k), "Bad constant primitive");
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
