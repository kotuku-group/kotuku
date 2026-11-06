// Public lexer IDs are an adapter contract, not a Lexilla factory API.
#pragma once

struct LexerProfile {
   SCLEX ID;
   const char *Factory;
};

inline constexpr LexerProfile glLexerProfiles[] = {
   { SCLEX::NIL, "null" },
   { SCLEX::PYTHON, "python" },
   { SCLEX::CPP, "cpp" },
   { SCLEX::HTML, "hypertext" },
   { SCLEX::XML, "xml" },
   { SCLEX::PERL, "perl" },
   { SCLEX::SQL, "sql" },
   { SCLEX::VB, "vb" },
   { SCLEX::PROPERTIES, "props" },
   { SCLEX::ERRORLIST, "errorlist" },
   { SCLEX::MAKEFILE, "makefile" },
   { SCLEX::BATCH, "batch" },
   { SCLEX::TIRI, "lua" },
   { SCLEX::DIFF, "diff" },
   { SCLEX::PASCAL, "pascal" },
   { SCLEX::RUBY, "ruby" },
   { SCLEX::VBSCRIPT, "vbscript" },
   { SCLEX::ASP, "hypertext" },
   { SCLEX::ASSEMBLER, "asm" },
   { SCLEX::CSS, "css" },
   { SCLEX::BASH, "bash" },
   { SCLEX::PHPSCRIPT, "phpscript" },
   { SCLEX::REBOL, "rebol" }
};

inline const LexerProfile * find_lexer_profile(SCLEX ID)
{
   for (const auto &profile : glLexerProfiles) {
      if (profile.ID IS ID) return &profile;
   }
   return nullptr;
}
