#pragma once

#include "parse_types.h"
#include "lexer_types.h"

//********************************************************************************************************************
// FuncState tracks all parser state for a single function being compiled:
//
// - Register allocation (freereg, varmap, framesize)
// - Bytecode emission (pc, bcbase, bclim)
// - Jump management (jpc, lasttarget)
// - Scoping and upvalues (bl, uvmap)
// - Constants (kt, nkn, nkgc)
//
// Design Notes:
// - Raw members are retained for backward compatibility during gradual migration
// - Type-safe accessors (current_pc(), free_reg(), etc.) provide BCPos/BCReg returns
// - Helper methods encapsulate common patterns (reset_freereg(), is_temp_register())
// - std::span views provide bounds-checked access to arrays
// - Debug assertions validate invariants in development builds

struct LexState;
struct FuncScope;
struct BCInsLine;

struct FuncState {
   GCtab *kt = nullptr;      // Hash table for constants.
   LexState *ls = nullptr;   // Lexer state.
   lua_State *L = nullptr;   // Lua state.
   FuncScope *bl = nullptr;  // Current scope.
   BCPOS pc = 0;             // Next bytecode position.
   BCPOS lasttarget = 0;     // Bytecode position of last jump target.
   BCPOS jpc = NO_JMP;       // Pending jump list to next bytecode.
   BCREG freereg = 0;        // First free register.
   BCREG nkn = 0, nkgc = 0;  // Number of lua_Number/GCobj constants
   BCLine linedefined = 0;   // First line of the function definition.
   BCInsLine *bcbase = nullptr;  // Base of bytecode stack.
   BCPOS bclim = 0;          // Limit of bytecode stack.
   MSize vbase = 0;          // Base of variable stack for this function.
   uint8_t flags = 0;        // Prototype flags.
   uint8_t numparams = 0;    // Number of parameters.
   uint8_t framesize = 1;    // Fixed frame size (minimum is 1).
   uint8_t nuv = 0;          // Number of upvalues
   std::vector<VarIndex> varmap;  // Map from register to variable idx. Size equals number of active local variables.
   std::array<VarIndex, LJ_MAX_LOCVAR> pending_varmap{};  // Staging area for var_new() before var_add().
   BCREG pending_vars = 0;        // Number of pending variables awaiting var_add().
   std::array<VarIndex, LJ_MAX_UPVAL> uvmap{};   // Map from upvalue to variable idx.
   std::array<VarIndex, LJ_MAX_UPVAL> uvtmp{};   // Temporary upvalue map.

   // Track explicitly declared global names.  This prevents new unscoped variables from being interpreted as locals
   // and thus shadowing global variables.
   ankerl::unordered_dense::set<GCstr*> declared_globals;

   // Track global names declared with <const> attribute for compile-time reassignment checks.
   ankerl::unordered_dense::set<GCstr*> const_globals;

   // Function name for named function declarations (used for tostring() output).
   // Set before fs_finish() is called. nullptr for anonymous functions.
   GCstr* funcname = nullptr;

   // Return types for runtime type checking.  Set during function emission if explicit return types are declared.
   // TiriType::Unknown means no type constraint is applied for that position.
   std::array<TiriType, MAX_RETURN_TYPES> return_types = []() constexpr {
      std::array<TiriType, MAX_RETURN_TYPES> arr{};
      for (auto& t : arr) t = TiriType::Unknown;
      return arr;
   }();

   // Try-except metadata for bytecode-level exception handling.
   // These are populated during emit_try_except_stmt and copied to GCproto during fs_finish.
   std::vector<TryBlockDesc>   try_blocks;    // Try block descriptors
   std::vector<TryHandlerDesc> try_handlers;  // Handler descriptors
   uint8_t try_depth = 0;  // Current try nesting depth for break/continue cleanup
   bool is_root = false;   // True if this is the top-level (root) function

   // Default constructor - initialises all fields to safe defaults.
   FuncState() = default;

   // Initialize runtime-dependent fields. Called after construction when the owning LexState context is available.
   void init(LexState* LexState, lua_State* LuaState, MSize Vbase, bool IsRoot);

   // Return strong types for bytecode positions and registers

   [[nodiscard]] constexpr BCPos current_pc() const noexcept { return BCPos(pc); }
   [[nodiscard]] constexpr BCPos last_target() const noexcept { return BCPos(lasttarget); }
   [[nodiscard]] constexpr BCPos pending_jmp() const noexcept { return BCPos(jpc); }
   [[nodiscard]] constexpr BCPos bytecode_limit() const noexcept { return BCPos(bclim); }

   [[nodiscard]] constexpr BCReg free_reg() const noexcept { return BCReg(freereg); }
   [[nodiscard]] inline BCReg active_var_count() const noexcept { return BCReg(varmap.size()); }
   [[nodiscard]] constexpr BCReg frame_size() const noexcept { return BCReg(framesize); }

   // Reset free register to the first register after local variables.
   inline void reset_freereg() noexcept { freereg = BCREG(varmap.size()); }

   // Ensure freereg is at least at varmap.size() level.
   inline void ensure_freereg_at_locals() noexcept { if (freereg < varmap.size()) freereg = BCREG(varmap.size()); }

   // Check if a register is a temporary (above local variables).
   [[nodiscard]] inline bool is_temp_register(BCReg Reg) const noexcept { return Reg.raw() >= varmap.size(); }

   // Check if a register is a local variable slot.
   [[nodiscard]] inline bool is_local_register(BCReg Reg) const noexcept { return Reg.raw() < varmap.size(); }

   // Check if a register is at the top of the stack (can be freed).
   [[nodiscard]] constexpr bool is_stack_top(BCReg Reg) const noexcept { return Reg.raw() + 1 IS freereg; }

   // Get the next available register without allocating it.
   [[nodiscard]] constexpr BCReg next_free() const noexcept { return BCReg(freereg); }

   // Check if there are pending jumps to patch.
   [[nodiscard]] constexpr bool has_pending_jumps() const noexcept { return jpc != NO_JMP; }

   // Clear pending jump list.
   constexpr void clear_pending_jumps() noexcept { jpc = NO_JMP; }

   // Bytecode Access

   // Get bytecode instruction at a position (bounds-checked in debug).

   [[nodiscard]] inline BCInsLine& bytecode_at(BCPos Pos) noexcept {
      lj_assertX(Pos.raw() < pc, "bytecode position out of range");
      return bcbase[Pos.raw()];
   }

   [[nodiscard]] inline const BCInsLine& bytecode_at(BCPos Pos) const noexcept {
      lj_assertX(Pos.raw() < pc, "bytecode position out of range");
      return bcbase[Pos.raw()];
   }

   // Get the last emitted instruction (common pattern).

   [[nodiscard]] inline BCInsLine& last_instruction() noexcept {
      lj_assertX(pc > 0, "no instructions emitted");
      return bcbase[pc - 1];
   }

   [[nodiscard]] inline const BCInsLine& last_instruction() const noexcept {
      lj_assertX(pc > 0, "no instructions emitted");
      return bcbase[pc - 1];
   }

   // Get a span view of the bytecode up to current pc.

   [[nodiscard]] inline std::span<BCInsLine> bytecode_span() noexcept { return std::span<BCInsLine>(bcbase, pc); }
   [[nodiscard]] inline std::span<const BCInsLine> bytecode_span() const noexcept { return std::span<const BCInsLine>(bcbase, pc); }

   // Get a span view of active upvalue mappings.

   [[nodiscard]] inline std::span<VarIndex> upvalue_span() noexcept { return std::span<VarIndex>(uvmap.data(), nuv); }
   [[nodiscard]] inline std::span<const VarIndex> upvalue_span() const noexcept { return std::span<const VarIndex>(uvmap.data(), nuv); }

   // Get a span view of active variable mappings.

   [[nodiscard]] inline std::span<VarIndex> varmap_span() noexcept { return std::span<VarIndex>(varmap); }
   [[nodiscard]] inline std::span<const VarIndex> varmap_span() const noexcept { return std::span<const VarIndex>(varmap); }

   // Get variable info for a local variable slot.

   [[nodiscard]] VarInfo & var_get(int32_t Slot);

   [[nodiscard]] const VarInfo& var_get(int32_t Slot) const;

   // Get variable info using typed register index.
   [[nodiscard]] inline VarInfo& var_at(BCReg Reg) { return var_get(int32_t(Reg.raw())); }
   [[nodiscard]] inline const VarInfo& var_at(BCReg Reg) const { return var_get(int32_t(Reg.raw())); }

   // Constant Counts

   [[nodiscard]] constexpr BCReg num_constants() const noexcept { return BCReg(nkn); }
   [[nodiscard]] constexpr BCReg gc_constants() const noexcept { return BCReg(nkgc); }

   // Check if we're at the top-level function (no enclosing function).
   [[nodiscard]] constexpr bool is_top_level() const noexcept { return is_root; }

   // Check if we have an active scope block.
   [[nodiscard]] constexpr bool has_active_scope() const noexcept { return bl != nullptr; }

   // --- Debug Assertions ---
   // Assertions are implemented as methods that call the macro so __FILE__/__LINE__ work correctly.
   // The variadic format arguments are forwarded to allow printf-style messages.

#ifdef LUA_USE_ASSERT
   // Validate register allocation invariant: freereg >= varmap.size()
   inline void assert_regalloc() const {
      lj_assertG_(G(L), freereg >= varmap.size(), "bad register allocation: freereg < varmap.size()");
   }

   // Validate that freereg equals varmap.size() (common check after scope cleanup)
   inline void assert_freereg_at_locals() const {
      lj_assertG_(G(L), freereg IS varmap.size(), "bad register state: freereg != varmap.size()");
   }
#else
   constexpr void assert_regalloc() const noexcept { }
   constexpr void assert_freereg_at_locals() const noexcept { }
#endif
};


[[maybe_unused]] void err_limit(FuncState *fs, uint32_t limit, CSTRING what);

inline void checklimit(FuncState *fs, MSize v, MSize l, const char *m) { if (v >= l) err_limit(fs, l, m); }
inline void checklimitgt(FuncState *fs, MSize v, MSize l, const char *m) { if (v > l) err_limit(fs, l, m); }
