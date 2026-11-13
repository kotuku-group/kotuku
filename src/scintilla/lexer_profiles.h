// Public lexer IDs are an adapter contract, not a Lexilla factory API.
#pragma once

struct LexerProfile {
   SCLEX ID;
   const char *Factory;
};

inline constexpr LexerProfile glLexerProfiles[] = {
#include "configured_lexer_profiles.h"
};

inline const LexerProfile * find_lexer_profile(SCLEX ID)
{
   for (const auto &profile : glLexerProfiles) {
      if (profile.ID IS ID) return &profile;
   }
   return nullptr;
}
