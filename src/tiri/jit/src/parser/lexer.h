// Lexical analyser.
// Copyright © 2025-2026 Paul Manias

#pragma once

#include <kotuku/main.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <stdarg.h>
#include <string_view>
#include <cstdint>
#include <memory>

#include "lj_obj.h"
#include "lj_err.h"
#include "../debug/filesource.h"
#include "func_state.h"

#ifdef INCLUDE_TIPS
#include <memory>
class TipEmitter;  // Forward declaration
#endif

#include "lexer_types.h"

// Forward declarations for parser scope helpers.

struct FuncScope;
struct FuncState;
struct ExpDesc;
enum class BinOpr : int8_t;

void lj_reserve_words(lua_State *);

class ParserContext;
template<typename T>
class ParserResult;
enum class TokenKind : uint16_t;

struct LocalDeclResult {
   BCREG declared = 0;
   BCREG initialised = 0;
};

// Lua lexer state.

class LexState {
public:
   struct BufferedToken {
      LexToken token = 0;
      TValue value;
      BCLine line = 0;
      BCLine column = 0;
      size_t offset = 0;
   };

   struct FuncState *fs;              // Current FuncState (points to func_stack.back()).
   std::deque<FuncState> func_stack;  // Stack of active FuncState objects.
   lua_State *L;                      // Lua state.
   TValue     tokval;       // Current token value.
   TValue     lookaheadval; // Lookahead token value.
   std::string_view source; // Complete source text (immutable).
   size_t     pos = 0;      // Current position in source.
   LexChar    c;            // Current character (cached).
   LexToken   tok;          // Current token.
   LexToken   lookahead;    // Lookahead token.
   SBuf       sb;           // String buffer for tokens.

   // Bytecode reader compatibility fields (used only by lj_bcread.cpp)
   const char *p = nullptr;  // Current position in bytecode buffer.
   const char *pe = nullptr; // End of bytecode buffer.
   lua_Reader rfunc = nullptr; // Reader callback for bytecode streaming.
   void *rdata = nullptr;    // Reader callback data.
   int endmark = 0;          // Trust bytecode end marker.

   BCLine     linenumber;   // Input line counter.
   BCLine     lastline;     // Line of last token.
   uint8_t    current_file_index = 0;  // File index for FileSource tracking (0 = main file)

   GCstr *    chunk_name;     // Current chunk name (interned string).
   const char *chunk_arg;     // Chunk name argument.
   const char *mode;          // Allow loading bytecode (b) and/or source text (t).
   GCstr *    empty_string_constant; // Cached empty string reference.
   VarInfo *  vstack;         // Stack for names and extents of local variables.
   MSize      size_vstack;    // Size of variable stack.
   MSize      vtop;           // Top of variable stack.
   BCInsLine* bc_stack;       // Stack for bytecode instructions/line numbers.
   MSize      size_bc_stack;  // Size of bytecode stack.
   uint32_t   level;          // Syntactical nesting level.
   uint32_t   ternary_depth;  // Number of pending ternary operators.
   uint8_t    pending_if_empty_colon; // Tracks ?: misuse after ??.
   int        is_bytecode;    // Set to 1 if input is bytecode, 0 if source text.
   int64_t    array_typed_size = -1;  // Size parameter for array<type, size> (-1 = no size specified)

   size_t   current_offset = 0;
   size_t   line_start_offset = 0;

   BCLine   current_token_line = 1;
   BCLine   current_token_column = 1;
   size_t   current_token_offset = 0;

   BCLine   lookahead_line = 1;
   BCLine   lookahead_column = 1;
   size_t   lookahead_offset = 0;

   BCLine   pending_token_line = 1;
   BCLine   pending_token_column = 1;
   size_t   pending_token_offset = 0;

   ParserContext* active_context = nullptr;
   std::deque<BufferedToken> buffered_tokens;
   bool diagnose_mode = false;  // When true, lexer errors are collected instead of thrown
   bool had_lex_error = false;  // Set when a recoverable lexer error occurred

#ifdef INCLUDE_TIPS
   // Tip system: 0 = off, 1 = best (critical), 2 = most (medium), 3 = all
   uint8_t tip_level = 0;
   std::unique_ptr<TipEmitter> tip_emitter;
#endif

   LexState() = default;  // Default constructor for bytecode reader usage
   LexState(lua_State* L, std::string_view Source, std::string_view Chunkarg, std::optional<std::string_view> Mode = std::nullopt);
   LexState(lua_State* L, lua_Reader Rfunc, void* Rdata, std::string_view Chunkarg, std::optional<std::string_view> Mode);  // Bytecode streaming constructor
   LexState(lua_State* L, const char* BytecodePtr, GCstr* ChunkName);  // Direct bytecode reading (for embedded bytecode)
   ~LexState();

   // Character stream operations
   [[nodiscard]] LexChar peek(size_t Offset = 0) const noexcept;
   [[nodiscard]] LexChar peek_next() const noexcept;

   void next();
   LexToken lookahead_token();
   const char* token2str(LexToken Tok);
   LJ_NOINLINE void err_syntax(ErrMsg Message);
   LJ_NOINLINE void err_token(LexToken Token);
   int lex_opt(LexToken Token);
   void lex_check(LexToken Token);
   void lex_match(LexToken What, LexToken Who, BCLine Line);
   GCstr* lex_str();

   // Variable management
   void var_new(BCREG Reg, GCstr* Name, BCLine Line = 0, BCLine Column = 0);
   void var_new_lit(BCREG Reg, std::string_view Value);
   void var_new_fixed(BCREG Reg, uintptr_t Name);
   void var_add(BCREG VariableCount);
   void var_remove(BCREG TargetLevel);
   MSize var_lookup(ExpDesc *);
   MSize var_lookup_symbol(GCstr* Name, ExpDesc *);

   // Break and continue management
   MSize gola_new(int JumpType, VarInfoFlag Info, BCPOS Position);
   void gola_patch(VarInfo* GotoInfo, VarInfo* LabelInfo);
   void gola_close(VarInfo* GotoInfo);
   void gola_resolve(FuncScope* Scope, MSize Index);
   void gola_fixup(FuncScope* Scope);

   // Function state lifecycle
   size_t fs_prep_var(FuncState* FunctionState, size_t* OffsetVar);
   void fs_fixup_var(GCproto* Prototype, uint8_t* Buffer, size_t OffsetVar);
   GCproto* fs_finish(BCLine Line);
   FuncState& fs_init();

   [[maybe_unused]] void assign_adjust(BCREG VariableCount, BCREG, ExpDesc *);
   [[nodiscard]] bool should_emit_presence();

   // Public parser helpers
   GCstr * keepstr(std::string_view Value);
   [[nodiscard]] GCstr* intern_empty_string();
   void ensure_lookahead(size_t count);
   [[nodiscard]] size_t available_lookahead() const;
   [[nodiscard]] const BufferedToken* buffered_token(size_t index) const;
   [[nodiscard]] SourceSpan current_token_span() const;
   [[nodiscard]] SourceSpan lookahead_token_span() const;
   void mark_token_start();
   void apply_buffered_token(const BufferedToken& token);
   BufferedToken scan_buffered_token();

   // Returns an encoded BCLine with file index in upper 8 bits and line number in lower 24 bits.
   // This allows error reporting to identify both the source file and line number.
   // Uses lastline (the line of the most recently consumed token) for bytecode emission.

   [[nodiscard]] inline BCLine effective_line() const noexcept {
      BCLine line = this->lastline;
      if (line < 1) line = 1;
      return BCLine::encode(this->current_file_index, line);
   }

#ifdef LUA_USE_ASSERT
   template<typename... Args>
   void assert_condition(bool Condition, const char* Format, Args... Arguments) {
      lj_assertG_(G(this->L), Condition, Format, Arguments...);
   }
#else
   template<typename... Args>
   void assert_condition(bool Condition, const char* Format, Args... Arguments) {
      (void)this; (void)Condition; (void)Format;
   }
#endif
};

void lj_lex_error(LexState *, LexToken, ErrMsg, ...);

// Error checking functions.

inline void checkcond(LexState *ls, bool c, ErrMsg em) { if (not (c)) { ls->err_syntax(em); } }

//********************************************************************************************************************
// RAII guard for import LexState instances.
//
// Windows SEH (used by lj_err_throw) doesn't call C++ destructors during stack unwinding.  This guard handles the
// normal path (destructor called on scope exit), while lua_load cleans up any orphaned lexers left in
// pending_import_lexers after catching SEH exceptions.
//
// Usage:
//    ImportLexerGuard guard(L, source, chunk_name);
//    guard->next();  // Access via operator->
//    // ... parsing code ...
//    // Destructor automatically cleans up on normal exit

class ImportLexerGuard {
public:
   ImportLexerGuard(lua_State *L, std::string_view Source, std::string ChunkName)
      : lua(L)
      , chunk_name(std::move(ChunkName))  // Store chunk name - LexState holds a raw pointer to it
      , lexer(new LexState(L, Source, chunk_name))
   {
      lua->pending_import_lexers.push_back(lexer);
   }

   ~ImportLexerGuard() {
      if (lexer) {
         // Remove from tracking vector (find and erase to handle nested imports correctly)
         auto &vec = lua->pending_import_lexers;
         auto it = std::find(vec.begin(), vec.end(), lexer);
         if (it != vec.end()) vec.erase(it);
         delete lexer;
      }
   }

   // Non-copyable, non-movable (prevents accidental double-delete)
   ImportLexerGuard(const ImportLexerGuard&) = delete;
   ImportLexerGuard& operator=(const ImportLexerGuard&) = delete;
   ImportLexerGuard(ImportLexerGuard&&) = delete;
   ImportLexerGuard& operator=(ImportLexerGuard&&) = delete;

   [[nodiscard]] LexState* get() const noexcept { return lexer; }
   [[nodiscard]] LexState* operator->() const noexcept { return lexer; }
   [[nodiscard]] LexState& operator*() const noexcept { return *lexer; }

private:
   lua_State *lua;
   std::string chunk_name;  // Must outlive lexer - LexState::chunk_arg points into this
   LexState *lexer;
};
