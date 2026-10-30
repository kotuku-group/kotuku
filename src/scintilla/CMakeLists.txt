# Scintilla module

set (MOD scintilla)
set (INC_MOD_SCINTILLA TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl"
   NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   ARGS "output-defs=module_def.c" "output-proto=module_def.c" "prototypes=static"
   APPEND_IDL "module_def.c")

set (SOURCE
   "scintilla/lexlib/Accessor.cxx"
   "scintilla/lexlib/CharacterSet.cxx"
   "scintilla/lexlib/LexerBase.cxx"
   "scintilla/lexlib/LexerModule.cxx"
   "scintilla/lexlib/LexerNoExceptions.cxx"
   "scintilla/lexlib/LexerSimple.cxx"
   "scintilla/lexlib/PropSetSimple.cxx"
   "scintilla/lexlib/StyleContext.cxx"
   "scintilla/lexlib/WordList.cxx"
   "scintilla/src/AutoComplete.cxx"
   "scintilla/src/CallTip.cxx"
   "scintilla/src/Catalogue.cxx"
   "scintilla/src/CellBuffer.cxx"
   "scintilla/src/CharClassify.cxx"
   "scintilla/src/ContractionState.cxx"
   "scintilla/src/Decoration.cxx"
   "scintilla/src/Document.cxx"
   "scintilla/src/Editor.cxx"
   "scintilla/src/ExternalLexer.cxx"
   "scintilla/src/Indicator.cxx"
   "scintilla/src/KeyMap.cxx"
   "scintilla/src/LineMarker.cxx"
   "scintilla/src/PerLine.cxx"
   "scintilla/src/PositionCache.cxx"
   "scintilla/src/RESearch.cxx"
   "scintilla/src/RunStyles.cxx"
   "scintilla/src/ScintillaBase.cxx"
   "scintilla/src/Selection.cxx"
   "scintilla/src/Style.cxx"
   "scintilla/src/UniConversion.cxx"
   "scintilla/src/ViewStyle.cxx"
   "scintilla/src/XPM.cxx"
   "class_scintilla.cxx"
   "class_scintilla_search.cxx")

set (LEXERS
   "scintilla/lexers/LexAsm.cxx"
   "scintilla/lexers/LexBash.cxx"
   "scintilla/lexers/LexBasic.cxx"
   "scintilla/lexers/LexCPP.cxx"
   "scintilla/lexers/LexCSS.cxx"
   "scintilla/lexers/LexD.cxx"
   "scintilla/lexers/LexHaskell.cxx"
   "scintilla/lexers/LexHTML.cxx"
   "scintilla/lexers/LexLua.cxx"
   "scintilla/lexers/LexMSSQL.cxx"
   "scintilla/lexers/LexMySQL.cxx"
   "scintilla/lexers/LexOthers.cxx"
   "scintilla/lexers/LexPascal.cxx"
   "scintilla/lexers/LexPerl.cxx"
   "scintilla/lexers/LexRebol.cxx"
   "scintilla/lexers/LexPython.cxx"
   "scintilla/lexers/LexRuby.cxx"
   "scintilla/lexers/LexSQL.cxx"
   "scintilla/lexers/LexVB.cxx")

add_library (scintilla)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
   target_compile_options (scintilla PRIVATE "-Wno-char-subscripts")
endif ()

target_compile_definitions (scintilla PRIVATE "SCI_NAMESPACE" "__KOTUKU__" "SCI_LEXER")
target_link_libraries (${MOD} PUBLIC unicode)

set_module_defaults (scintilla "Sci")

target_sources (scintilla PRIVATE ${SOURCE} ${LEXERS})

target_include_directories (scintilla PRIVATE
   "${CMAKE_CURRENT_BINARY_DIR}"
   "${CMAKE_CURRENT_SOURCE_DIR}/scintilla/src"
   "${CMAKE_CURRENT_SOURCE_DIR}/scintilla/lexlib"
   "${CMAKE_CURRENT_SOURCE_DIR}/scintilla/include")

# Keep the editor's Tiri keywords in step with the parser without linking to the Tiri module.
set (TIRI_TOKEN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../tiri/jit/src/parser/lexer_types.h")
set_property (DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TIRI_TOKEN_SOURCE}")
file (READ "${TIRI_TOKEN_SOURCE}" TIRI_TOKEN_SOURCE_TEXT)
string (REGEX MATCHALL "TOKEN_DEF\\([a-z]+, +\"[a-z]+\", +TKF_RESERVED[^)]*\\)"
   TIRI_TOKEN_LINES "${TIRI_TOKEN_SOURCE_TEXT}")
set (TIRI_KEYWORDS "")
foreach (TIRI_TOKEN_LINE IN LISTS TIRI_TOKEN_LINES)
   string (REGEX REPLACE ".*TOKEN_DEF\\([^,]+, *\"([a-z]+)\".*" "\\1" TIRI_KEYWORD "${TIRI_TOKEN_LINE}")
   string (APPEND TIRI_KEYWORDS "   \"${TIRI_KEYWORD} \"\n")
endforeach ()
configure_file ("tiri_keywords.h.in" "tiri_keywords.h" @ONLY)

flute_test (scintilla_document "tests/test_scintilla_document.tiri")
flute_test (scintilla_render "tests/test_scintilla_render.tiri")
flute_test (scintilla_scroll "tests/test_scintilla_scroll.tiri")
flute_test (scintilla_keyboard "tests/test_scintilla_keyboard.tiri")
flute_test (scintilla_initial_focus "tests/test_scintilla_initial_focus.tiri")
flute_test (scintilla_clipboard "tests/test_scintilla_clipboard.tiri")
flute_test (scintilla_margins "tests/test_scintilla_margins.tiri")
flute_test (scintilla_highlighting "tests/test_scintilla_highlighting.tiri")
flute_test (scintilla_completion "tests/test_scintilla_completion.tiri")

flute_test (scintilla_mouse "tests/test_scintilla_mouse.tiri")
