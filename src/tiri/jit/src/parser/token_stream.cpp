// Copyright © 2025-2026 Paul Manias

#include "parser/token_stream.h"

#include "parser/parser_context.h"
#include "lexer.h"

TokenStreamAdapter::TokenStreamAdapter(LexState& state) : lex_state(&state) { }

Token TokenStreamAdapter::current() const { return Token::from_current(*this->lex_state); }

Token TokenStreamAdapter::peek(size_t lookahead) const
{
   if (lookahead IS 0) return this->current();
   this->lex_state->ensure_lookahead(lookahead);
   bool has_direct_lookahead = (this->lex_state->lookahead != TK_eof);
   if (lookahead IS 1 and has_direct_lookahead) return Token::from_lookahead(*this->lex_state);

   size_t buffer_index = lookahead - 1;
   if (has_direct_lookahead) {
      this->lex_state->assert_condition(buffer_index > 0, "buffer underflow");
      buffer_index--;
   }

   const auto* buffered = this->lex_state->buffered_token(buffer_index);
   this->lex_state->assert_condition(buffered != nullptr, "missing buffered token for lookahead distance %zu", lookahead);
   return Token::from_buffered(*this->lex_state, *buffered);
}

Token TokenStreamAdapter::advance()
{
   Token previous = this->current();
   this->lex_state->next();
   Token current = this->current();
   if (this->lex_state->active_context) this->lex_state->active_context->trace_token_advance(previous, current);
   return current;
}

void TokenStreamAdapter::sync_from_lex(LexState& state) { this->lex_state = &state; }

