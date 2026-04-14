// Copyright © 2025-2026 Paul Manias

#include "parser/token_types.h"

#include <cmath>

#include "lj_obj.h"

[[nodiscard]] Token Token::from_current(LexState &State)
{
   Token token;
   token.token_kind = to_token_kind(State.tok);
   token.raw_token  = State.tok;
   token.source     = State.current_token_span();
   token.data.assign(State.L, State.tokval);
   return token;
}

[[nodiscard]] Token Token::from_lookahead(LexState &State)
{
   Token token;
   LexToken lookahead = (State.lookahead != TK_eof) ? State.lookahead : State.lookahead_token();
   token.token_kind   = to_token_kind(lookahead);
   token.raw_token    = lookahead;
   token.source       = State.lookahead_token_span();
   token.data.assign(State.L, State.lookaheadval);
   return token;
}

[[nodiscard]] Token Token::from_buffered(LexState &State, const LexState::BufferedToken &Buffered)
{
   Token token;
   token.token_kind    = to_token_kind(Buffered.token);
   token.raw_token     = Buffered.token;
   token.source.line   = Buffered.line;
   token.source.column = Buffered.column;
   token.source.offset = Buffered.offset;
   token.data.assign(State.L, Buffered.value);
   return token;
}

[[nodiscard]] Token Token::from_span(SourceSpan span, TokenKind kind)
{
   Token token;
   token.token_kind = kind;
   token.raw_token = (LexToken)kind;
   token.source = span;
   return token;
}

[[nodiscard]] bool Token::is_literal() const
{
   switch (this->token_kind) {
      case TokenKind::Number:
      case TokenKind::String:
      case TokenKind::Nil:
      case TokenKind::TrueToken:
      case TokenKind::FalseToken:
         return true;
      default:
         return false;
   }
}
