// Semantic styles for the bundled Scintilla 2.28 lexers.  Unclassified styles inherit TextColour.
// Recolouring does not reset fonts, document state or contracted folds.

void ScintillaKTK::SetStyleColours()
{
   auto colour = [this](const RGB8 &Value, std::initializer_list<int> Styles) {
      for (auto style : Styles) {
         SendScintilla(SCI_STYLESETFORE, style, SCICOLOUR(Value.Red, Value.Green, Value.Blue));
      }
   };

   auto &background = scintilla->BkgdColour;
   auto &text = scintilla->TextColour;
   for (int style=0; style <= STYLE_MAX; style++) {
      SendScintilla(SCI_STYLESETFORE, style, SCICOLOUR(text.Red, text.Green, text.Blue));
      SendScintilla(SCI_STYLESETBACK, style, SCICOLOUR(background.Red, background.Green, background.Blue));
   }

   switch (scintilla->Lexer) {
      case SCLEX::CPP:
         colour(scintilla->CommentColour, {
            SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC, SCE_C_COMMENTLINEDOC, SCE_C_COMMENTDOCKEYWORD });
         colour(scintilla->NumberColour, { SCE_C_NUMBER });
         colour(scintilla->KeywordColour, { SCE_C_WORD });
         colour(scintilla->StringColour, {
            SCE_C_STRING, SCE_C_CHARACTER, SCE_C_UUID, SCE_C_VERBATIM, SCE_C_STRINGRAW, SCE_C_TRIPLEVERBATIM,
            SCE_C_REGEX });
         colour(scintilla->PreprocessorColour, { SCE_C_PREPROCESSOR });
         colour(scintilla->OperatorColour, { SCE_C_OPERATOR });
         colour(scintilla->TypeColour, { SCE_C_WORD2, SCE_C_GLOBALCLASS });
         colour(scintilla->ErrorColour, { SCE_C_STRINGEOL, SCE_C_COMMENTDOCKEYWORDERROR });
         break;

      case SCLEX::TIRI:
         colour(scintilla->CommentColour, { SCE_LUA_COMMENT, SCE_LUA_COMMENTLINE, SCE_LUA_COMMENTDOC });
         colour(scintilla->NumberColour, { SCE_LUA_NUMBER });
         colour(scintilla->KeywordColour, { SCE_LUA_WORD });
         colour(scintilla->StringColour, { SCE_LUA_STRING, SCE_LUA_CHARACTER, SCE_LUA_LITERALSTRING });
         colour(scintilla->PreprocessorColour, { SCE_LUA_PREPROCESSOR });
         colour(scintilla->OperatorColour, { SCE_LUA_OPERATOR });
         colour(scintilla->TypeColour, {
            SCE_LUA_WORD2, SCE_LUA_WORD3, SCE_LUA_WORD4, SCE_LUA_WORD5, SCE_LUA_WORD6, SCE_LUA_WORD7, SCE_LUA_WORD8 });
         colour(scintilla->ErrorColour, { SCE_LUA_STRINGEOL });
         break;

      case SCLEX::PYTHON:
         colour(scintilla->CommentColour, { SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK });
         colour(scintilla->NumberColour, { SCE_P_NUMBER });
         colour(scintilla->KeywordColour, { SCE_P_WORD });
         colour(scintilla->StringColour, { SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE, SCE_P_TRIPLEDOUBLE });
         colour(scintilla->OperatorColour, { SCE_P_OPERATOR });
         colour(scintilla->TypeColour, { SCE_P_CLASSNAME, SCE_P_DEFNAME, SCE_P_WORD2 });
         colour(scintilla->PreprocessorColour, { SCE_P_DECORATOR });
         colour(scintilla->ErrorColour, { SCE_P_STRINGEOL });
         break;

      case SCLEX::PERL:
         colour(scintilla->CommentColour, { SCE_PL_COMMENTLINE, SCE_PL_POD, SCE_PL_POD_VERB });
         colour(scintilla->NumberColour, { SCE_PL_NUMBER });
         colour(scintilla->KeywordColour, { SCE_PL_WORD });
         colour(scintilla->StringColour, {
            SCE_PL_STRING, SCE_PL_CHARACTER, SCE_PL_REGEX, SCE_PL_REGSUBST, SCE_PL_LONGQUOTE, SCE_PL_BACKTICKS,
            SCE_PL_HERE_DELIM, SCE_PL_HERE_Q, SCE_PL_HERE_QQ, SCE_PL_HERE_QX, SCE_PL_STRING_Q, SCE_PL_STRING_QQ,
            SCE_PL_STRING_QX, SCE_PL_STRING_QR, SCE_PL_STRING_QW, SCE_PL_DATASECTION, SCE_PL_FORMAT });
         colour(scintilla->OperatorColour, { SCE_PL_OPERATOR, SCE_PL_PUNCTUATION });
         colour(scintilla->PreprocessorColour, { SCE_PL_PREPROCESSOR });
         colour(scintilla->TypeColour, {
            SCE_PL_SCALAR, SCE_PL_ARRAY, SCE_PL_HASH, SCE_PL_SYMBOLTABLE, SCE_PL_VARIABLE_INDEXER,
            SCE_PL_SUB_PROTOTYPE, SCE_PL_FORMAT_IDENT });
         colour(scintilla->ErrorColour, { SCE_PL_ERROR });
         break;

      case SCLEX::SQL:
         colour(scintilla->CommentColour, {
            SCE_SQL_COMMENT, SCE_SQL_COMMENTLINE, SCE_SQL_COMMENTDOC, SCE_SQL_COMMENTLINEDOC,
            SCE_SQL_COMMENTDOCKEYWORD, SCE_SQL_SQLPLUS_COMMENT });
         colour(scintilla->NumberColour, { SCE_SQL_NUMBER });
         colour(scintilla->KeywordColour, { SCE_SQL_WORD });
         colour(scintilla->StringColour, { SCE_SQL_STRING, SCE_SQL_CHARACTER });
         colour(scintilla->OperatorColour, { SCE_SQL_OPERATOR });
         colour(scintilla->PreprocessorColour, { SCE_SQL_SQLPLUS, SCE_SQL_SQLPLUS_PROMPT });
         colour(scintilla->TypeColour, {
            SCE_SQL_WORD2, SCE_SQL_USER1, SCE_SQL_USER2, SCE_SQL_USER3, SCE_SQL_USER4, SCE_SQL_QUOTEDIDENTIFIER });
         colour(scintilla->ErrorColour, { SCE_SQL_COMMENTDOCKEYWORDERROR });
         break;

      case SCLEX::VB:
      case SCLEX::VBSCRIPT:
         colour(scintilla->CommentColour, { SCE_B_COMMENT });
         colour(scintilla->NumberColour, { SCE_B_NUMBER, SCE_B_DATE, SCE_B_HEXNUMBER, SCE_B_BINNUMBER });
         colour(scintilla->KeywordColour, { SCE_B_KEYWORD });
         colour(scintilla->StringColour, { SCE_B_STRING });
         colour(scintilla->PreprocessorColour, { SCE_B_PREPROCESSOR });
         colour(scintilla->OperatorColour, { SCE_B_OPERATOR });
         colour(scintilla->TypeColour, {
            SCE_B_KEYWORD2, SCE_B_KEYWORD3, SCE_B_KEYWORD4, SCE_B_CONSTANT, SCE_B_ASM, SCE_B_LABEL });
         colour(scintilla->ErrorColour, { SCE_B_STRINGEOL, SCE_B_ERROR });
         break;

      case SCLEX::PROPERTIES:
         colour(scintilla->CommentColour, { SCE_PROPS_COMMENT });
         colour(scintilla->KeywordColour, { SCE_PROPS_SECTION });
         colour(scintilla->OperatorColour, { SCE_PROPS_ASSIGNMENT });
         colour(scintilla->StringColour, { SCE_PROPS_DEFVAL });
         colour(scintilla->TypeColour, { SCE_PROPS_KEY });
         break;

      case SCLEX::ERRORLIST:
         colour(scintilla->ErrorColour, {
            SCE_ERR_PYTHON, SCE_ERR_GCC, SCE_ERR_MS, SCE_ERR_BORLAND, SCE_ERR_PERL, SCE_ERR_NET, SCE_ERR_LUA,
            SCE_ERR_PHP, SCE_ERR_ELF, SCE_ERR_IFC, SCE_ERR_IFORT, SCE_ERR_ABSF, SCE_ERR_TIDY, SCE_ERR_JAVA_STACK,
            SCE_ERR_DIFF_DELETION });
         colour(scintilla->PreprocessorColour, { SCE_ERR_CMD, SCE_ERR_CTAG, SCE_ERR_DIFF_MESSAGE });
         colour(scintilla->TypeColour, { SCE_ERR_DIFF_CHANGED });
         colour(scintilla->StringColour, { SCE_ERR_DIFF_ADDITION });
         colour(scintilla->NumberColour, { SCE_ERR_VALUE });
         break;

      case SCLEX::MAKEFILE:
         colour(scintilla->CommentColour, { SCE_MAKE_COMMENT });
         colour(scintilla->PreprocessorColour, { SCE_MAKE_PREPROCESSOR });
         colour(scintilla->TypeColour, { SCE_MAKE_IDENTIFIER, SCE_MAKE_TARGET });
         colour(scintilla->OperatorColour, { SCE_MAKE_OPERATOR });
         colour(scintilla->ErrorColour, { SCE_MAKE_IDEOL });
         break;

      case SCLEX::BATCH:
         colour(scintilla->CommentColour, { SCE_BAT_COMMENT });
         colour(scintilla->KeywordColour, { SCE_BAT_WORD });
         colour(scintilla->TypeColour, { SCE_BAT_LABEL, SCE_BAT_IDENTIFIER });
         colour(scintilla->PreprocessorColour, { SCE_BAT_HIDE, SCE_BAT_COMMAND });
         colour(scintilla->OperatorColour, { SCE_BAT_OPERATOR });
         break;

      case SCLEX::DIFF:
         colour(scintilla->CommentColour, { SCE_DIFF_COMMENT });
         colour(scintilla->PreprocessorColour, { SCE_DIFF_COMMAND, SCE_DIFF_HEADER });
         colour(scintilla->NumberColour, { SCE_DIFF_POSITION });
         colour(scintilla->ErrorColour, { SCE_DIFF_DELETED });
         colour(scintilla->StringColour, { SCE_DIFF_ADDED });
         colour(scintilla->TypeColour, { SCE_DIFF_CHANGED });
         break;

      case SCLEX::PASCAL:
         colour(scintilla->CommentColour, { SCE_PAS_COMMENT, SCE_PAS_COMMENT2, SCE_PAS_COMMENTLINE });
         colour(scintilla->PreprocessorColour, { SCE_PAS_PREPROCESSOR, SCE_PAS_PREPROCESSOR2 });
         colour(scintilla->NumberColour, { SCE_PAS_NUMBER, SCE_PAS_HEXNUMBER });
         colour(scintilla->KeywordColour, { SCE_PAS_WORD });
         colour(scintilla->StringColour, { SCE_PAS_STRING, SCE_PAS_CHARACTER });
         colour(scintilla->OperatorColour, { SCE_PAS_OPERATOR });
         colour(scintilla->TypeColour, { SCE_PAS_ASM });
         colour(scintilla->ErrorColour, { SCE_PAS_STRINGEOL });
         break;

      case SCLEX::RUBY:
         colour(scintilla->CommentColour, { SCE_RB_COMMENTLINE, SCE_RB_POD });
         colour(scintilla->NumberColour, { SCE_RB_NUMBER });
         colour(scintilla->KeywordColour, { SCE_RB_WORD, SCE_RB_WORD_DEMOTED });
         colour(scintilla->StringColour, {
            SCE_RB_STRING, SCE_RB_CHARACTER, SCE_RB_REGEX, SCE_RB_BACKTICKS, SCE_RB_HERE_DELIM, SCE_RB_HERE_Q,
            SCE_RB_HERE_QQ, SCE_RB_HERE_QX, SCE_RB_STRING_Q, SCE_RB_STRING_QQ, SCE_RB_STRING_QX, SCE_RB_STRING_QR,
            SCE_RB_STRING_QW, SCE_RB_DATASECTION });
         colour(scintilla->OperatorColour, { SCE_RB_OPERATOR });
         colour(scintilla->TypeColour, {
            SCE_RB_CLASSNAME, SCE_RB_DEFNAME, SCE_RB_GLOBAL, SCE_RB_SYMBOL, SCE_RB_MODULE_NAME, SCE_RB_INSTANCE_VAR,
            SCE_RB_CLASS_VAR, SCE_RB_STDIN, SCE_RB_STDOUT, SCE_RB_STDERR });
         colour(scintilla->ErrorColour, { SCE_RB_ERROR });
         break;

      case SCLEX::ASSEMBLER:
         colour(scintilla->CommentColour, { SCE_ASM_COMMENT, SCE_ASM_COMMENTBLOCK, SCE_ASM_COMMENTDIRECTIVE });
         colour(scintilla->NumberColour, { SCE_ASM_NUMBER });
         colour(scintilla->StringColour, { SCE_ASM_STRING, SCE_ASM_CHARACTER });
         colour(scintilla->OperatorColour, { SCE_ASM_OPERATOR });
         colour(scintilla->KeywordColour, { SCE_ASM_CPUINSTRUCTION, SCE_ASM_MATHINSTRUCTION, SCE_ASM_EXTINSTRUCTION });
         colour(scintilla->TypeColour, { SCE_ASM_REGISTER });
         colour(scintilla->PreprocessorColour, { SCE_ASM_DIRECTIVE, SCE_ASM_DIRECTIVEOPERAND });
         colour(scintilla->ErrorColour, { SCE_ASM_STRINGEOL });
         break;

      case SCLEX::CSS:
         colour(scintilla->CommentColour, { SCE_CSS_COMMENT });
         colour(scintilla->KeywordColour, {
            SCE_CSS_TAG, SCE_CSS_PSEUDOCLASS, SCE_CSS_PSEUDOELEMENT, SCE_CSS_EXTENDED_PSEUDOCLASS,
            SCE_CSS_EXTENDED_PSEUDOELEMENT });
         colour(scintilla->TypeColour, {
            SCE_CSS_CLASS, SCE_CSS_IDENTIFIER, SCE_CSS_IDENTIFIER2, SCE_CSS_IDENTIFIER3, SCE_CSS_ID,
            SCE_CSS_ATTRIBUTE, SCE_CSS_EXTENDED_IDENTIFIER });
         colour(scintilla->StringColour, { SCE_CSS_VALUE, SCE_CSS_DOUBLESTRING, SCE_CSS_SINGLESTRING });
         colour(scintilla->OperatorColour, { SCE_CSS_OPERATOR });
         colour(scintilla->PreprocessorColour, { SCE_CSS_IMPORTANT, SCE_CSS_DIRECTIVE, SCE_CSS_MEDIA });
         colour(scintilla->ErrorColour, { SCE_CSS_UNKNOWN_PSEUDOCLASS, SCE_CSS_UNKNOWN_IDENTIFIER });
         break;

      case SCLEX::BASH:
         colour(scintilla->CommentColour, { SCE_SH_COMMENTLINE });
         colour(scintilla->NumberColour, { SCE_SH_NUMBER });
         colour(scintilla->KeywordColour, { SCE_SH_WORD });
         colour(scintilla->StringColour, {
            SCE_SH_STRING, SCE_SH_CHARACTER, SCE_SH_BACKTICKS, SCE_SH_HERE_DELIM, SCE_SH_HERE_Q });
         colour(scintilla->OperatorColour, { SCE_SH_OPERATOR });
         colour(scintilla->TypeColour, { SCE_SH_SCALAR, SCE_SH_PARAM });
         colour(scintilla->ErrorColour, { SCE_SH_ERROR });
         break;

      case SCLEX::REBOL:
         colour(scintilla->CommentColour, { SCE_REBOL_COMMENTLINE, SCE_REBOL_COMMENTBLOCK });
         colour(scintilla->PreprocessorColour, { SCE_REBOL_PREFACE, SCE_REBOL_ISSUE });
         colour(scintilla->StringColour, {
            SCE_REBOL_CHARACTER, SCE_REBOL_QUOTEDSTRING, SCE_REBOL_BRACEDSTRING, SCE_REBOL_FILE, SCE_REBOL_EMAIL,
            SCE_REBOL_URL });
         colour(scintilla->NumberColour, {
            SCE_REBOL_NUMBER, SCE_REBOL_PAIR, SCE_REBOL_TUPLE, SCE_REBOL_BINARY, SCE_REBOL_MONEY, SCE_REBOL_DATE,
            SCE_REBOL_TIME });
         colour(scintilla->OperatorColour, { SCE_REBOL_OPERATOR });
         colour(scintilla->KeywordColour, { SCE_REBOL_WORD });
         colour(scintilla->TypeColour, {
            SCE_REBOL_TAG, SCE_REBOL_WORD2, SCE_REBOL_WORD3, SCE_REBOL_WORD4, SCE_REBOL_WORD5, SCE_REBOL_WORD6,
            SCE_REBOL_WORD7, SCE_REBOL_WORD8 });
         break;

      case SCLEX::HTML:
      case SCLEX::XML:
      case SCLEX::ASP:
      case SCLEX::PHPSCRIPT:
         colour(scintilla->CommentColour, {
            SCE_H_COMMENT, SCE_H_XCCOMMENT, SCE_H_SGML_COMMENT, SCE_H_SGML_1ST_PARAM_COMMENT, SCE_HJ_COMMENT,
            SCE_HJ_COMMENTLINE, SCE_HJ_COMMENTDOC, SCE_HJA_COMMENT, SCE_HJA_COMMENTLINE, SCE_HJA_COMMENTDOC,
            SCE_HB_COMMENTLINE, SCE_HBA_COMMENTLINE, SCE_HP_COMMENTLINE, SCE_HPA_COMMENTLINE, SCE_HPHP_COMMENT,
            SCE_HPHP_COMMENTLINE });
         colour(scintilla->KeywordColour, {
            SCE_H_TAG, SCE_H_TAGEND, SCE_H_SGML_COMMAND, SCE_HJ_KEYWORD, SCE_HJA_KEYWORD, SCE_HB_WORD, SCE_HBA_WORD,
            SCE_HP_WORD, SCE_HPA_WORD, SCE_HPHP_WORD });
         colour(scintilla->TypeColour, {
            SCE_H_ATTRIBUTE, SCE_H_ENTITY, SCE_H_SGML_ENTITY, SCE_H_SGML_1ST_PARAM, SCE_HP_CLASSNAME,
            SCE_HP_DEFNAME, SCE_HPA_CLASSNAME, SCE_HPA_DEFNAME, SCE_HPHP_VARIABLE, SCE_HPHP_HSTRING_VARIABLE,
            SCE_HPHP_COMPLEX_VARIABLE });
         colour(scintilla->NumberColour, {
            SCE_H_NUMBER, SCE_HJ_NUMBER, SCE_HJA_NUMBER, SCE_HB_NUMBER, SCE_HBA_NUMBER, SCE_HP_NUMBER,
            SCE_HPA_NUMBER, SCE_HPHP_NUMBER });
         colour(scintilla->StringColour, {
            SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING, SCE_H_VALUE, SCE_H_CDATA, SCE_H_SGML_DOUBLESTRING,
            SCE_H_SGML_SIMPLESTRING, SCE_HJ_DOUBLESTRING, SCE_HJ_SINGLESTRING, SCE_HJ_REGEX, SCE_HJA_DOUBLESTRING,
            SCE_HJA_SINGLESTRING, SCE_HJA_REGEX, SCE_HB_STRING, SCE_HBA_STRING, SCE_HP_STRING, SCE_HP_CHARACTER,
            SCE_HP_TRIPLE, SCE_HP_TRIPLEDOUBLE, SCE_HPA_STRING, SCE_HPA_CHARACTER, SCE_HPA_TRIPLE,
            SCE_HPA_TRIPLEDOUBLE, SCE_HPHP_HSTRING, SCE_HPHP_SIMPLESTRING });
         colour(scintilla->OperatorColour, {
            SCE_H_OTHER, SCE_HJ_SYMBOLS, SCE_HJA_SYMBOLS, SCE_HP_OPERATOR, SCE_HPA_OPERATOR, SCE_HPHP_OPERATOR });
         colour(scintilla->PreprocessorColour, {
            SCE_H_XMLSTART, SCE_H_XMLEND, SCE_H_SCRIPT, SCE_H_ASP, SCE_H_ASPAT, SCE_H_QUESTION, SCE_H_SGML_DEFAULT,
            SCE_H_SGML_SPECIAL, SCE_H_SGML_BLOCK_DEFAULT });
         colour(scintilla->ErrorColour, {
            SCE_H_TAGUNKNOWN, SCE_H_ATTRIBUTEUNKNOWN, SCE_H_SGML_ERROR, SCE_HJ_STRINGEOL, SCE_HJA_STRINGEOL,
            SCE_HB_STRINGEOL, SCE_HBA_STRINGEOL });
         break;

      default: break;
   }

   colour(scintilla->MarginColour, { STYLE_LINENUMBER });
   colour(scintilla->KeywordColour, { STYLE_BRACELIGHT });
   colour(scintilla->ErrorColour, { STYLE_BRACEBAD });
   auto &margin = scintilla->MarginBkgd;
   auto margin_colour = SCICOLOUR(margin.Red, margin.Green, margin.Blue);
   SendScintilla(SCI_STYLESETBACK, STYLE_LINENUMBER, margin_colour);
   SendScintilla(SCI_SETFOLDMARGINCOLOUR, 1, margin_colour);
   SendScintilla(SCI_SETFOLDMARGINHICOLOUR, 1, margin_colour);
   SendScintilla(SCI_STYLESETBACK, STYLE_BRACELIGHT, margin_colour);
   SendScintilla(SCI_STYLESETBACK, STYLE_BRACEBAD, margin_colour);
   for (int marker=SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; marker++) {
      SendScintilla(SCI_MARKERSETFORE, marker, margin_colour);
      auto &foreground = scintilla->MarginColour;
      SendScintilla(SCI_MARKERSETBACK, marker, SCICOLOUR(foreground.Red, foreground.Green, foreground.Blue));
   }
   InvalidateStyleRedraw();
}
