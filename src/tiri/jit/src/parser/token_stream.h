#pragma once

#include "parser/token_types.h"

class TokenStreamAdapter {
public:
   explicit TokenStreamAdapter(LexState& state);

   [[nodiscard]] Token current() const;
   [[nodiscard]] Token peek(size_t lookahead) const;
   Token advance();
   void sync_from_lex(LexState& state);

private:
   LexState* lex_state;
};

