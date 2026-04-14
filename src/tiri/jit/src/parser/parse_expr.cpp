// Copyright © 2025-2026 Paul Manias
// Shared helper functions used by AST parser

#include <format>
#include <string>

#include "parser/parser_context.h"
#include "parser/parse_value.h"
#include "parser/parse_regalloc.h"
#include "parser/parse_control_flow.h"

//********************************************************************************************************************
// Return index expression.
// Used by ir_emitter and operator emitters.

static void expr_index(FuncState *State, ExpDesc *t, ExpDesc *e)
{
   // Already called: expr_toval(State, e).
   t->k = ExpKind::Indexed;
   if (e->is_num_constant()) {
#if LJ_DUALNUM
      if (tvisint(e->num_tv())) {
         int32_t k = intV(e->num_tv());
         if (checku8(k)) {
            t->u.s.aux = BCMAX_C + 1 + uint32_t(k);  // 256..511: const byte key
            return;
         }
      }
#else
      lua_Number n = e->number_value();
      int32_t k = lj_num2int(n);
      if (checku8(k) and n IS lua_Number(k)) {
         t->u.s.aux = BCMAX_C + 1 + uint32_t(k);  // 256..511: const byte key
         return;
      }
#endif
   }
   else if (e->is_str_constant()) {
      BCREG idx = const_str(State, e);
      if (idx <= BCMAX_C) {
         t->u.s.aux = ~idx;  // -256..-1: const string key
         return;
      }
   }

   RegisterAllocator allocator(State);
   ExpressionValue value(State, *e);
   t->u.s.aux = value.discharge_to_any_reg(allocator);  // 0..255: register
}

//********************************************************************************************************************
// Get value of constant expression.
// Used by ir_emitter for table constructor optimization.

static void expr_kvalue(FuncState *fs, TValue *v, ExpDesc *e)
{
   if (e->k <= ExpKind::True) setpriV(v, ~uint64_t(e->k));
   else if (e->k IS ExpKind::Str) setgcVraw(v, obj2gco(e->u.sval), LJ_TSTR);
   else {
      fs_check_assert(fs,tvisnumber(e->num_tv()), "bad number constant");
      *v = *e->num_tv();
   }
}

//********************************************************************************************************************

static int token_starts_expression(LexToken tok)
{
   switch (tok) {
      case TK_number:
      case TK_string:
      case TK_nil:
      case TK_true:
      case TK_false:
      case TK_dots:
      case TK_function:
      case TK_name:
      case '{':
      case '(':
      case TK_not:
      case TK_plusplus:
      case '-':
      case '~':
      case '#':
         return 1;
      default:
         return 0;
   }
}

//********************************************************************************************************************
// Determine if ?? operator should be treated as postfix presence check or binary if-empty.
// Used by AST pipeline (ast_builder.cpp).

bool LexState::should_emit_presence()
{
   // Use raw line numbers for comparison (lastline and lookahead_line are raw,
   // effective_line() is encoded with file index in upper bits)
   BCLine token_line = this->lastline;
   BCLine operator_line = this->effective_line().lineNumber();
   LexToken lookahead = (this->lookahead != TK_eof) ? this->lookahead : this->lookahead_token();
   BCLine lookahead_line = this->lookahead_line;
   // If the operator is on a different line than the token, it's definitely postfix
   if (operator_line > token_line) return true;
   // If the lookahead is on a different line than the operator, it's postfix
   if (lookahead_line > operator_line) return true;
   // Otherwise, check if the lookahead starts an expression
   return !token_starts_expression(lookahead);
}
