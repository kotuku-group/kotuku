# Scintilla module.

set (MOD scintilla)
set (INC_MOD_SCINTILLA TRUE PARENT_SCOPE)

function (scintilla_disable_exceptions TARGET_NAME)
   if (MSVC)
      target_compile_options (${TARGET_NAME} PRIVATE /EHs-c-)
      target_compile_definitions (${TARGET_NAME} PRIVATE _HAS_EXCEPTIONS=0)
   elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options (${TARGET_NAME} PRIVATE -fno-exceptions)
   endif ()
endfunction ()

idl_gen ("${MOD}.tdl"
   NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   ARGS "output-defs=module_def.c" "output-proto=module_def.c" "prototypes=static"
   APPEND_IDL "module_def.c")

set (SCINTILLA_ENGINE_SOURCES
   scintilla/src/AutoComplete.cxx
   scintilla/src/CallTip.cxx
   scintilla/src/CaseConvert.cxx
   scintilla/src/CaseFolder.cxx
   scintilla/src/CellBuffer.cxx
   scintilla/src/ChangeHistory.cxx
   scintilla/src/CharacterCategoryMap.cxx
   scintilla/src/CharacterType.cxx
   scintilla/src/CharClassify.cxx
   scintilla/src/ContractionState.cxx
   scintilla/src/DBCS.cxx
   scintilla/src/Decoration.cxx
   scintilla/src/Document.cxx
   scintilla/src/EditModel.cxx
   scintilla/src/Editor.cxx
   scintilla/src/EditView.cxx
   scintilla/src/Geometry.cxx
   scintilla/src/Indicator.cxx
   scintilla/src/KeyMap.cxx
   scintilla/src/LineMarker.cxx
   scintilla/src/MarginView.cxx
   scintilla/src/PerLine.cxx
   scintilla/src/PositionCache.cxx
   scintilla/src/RESearch.cxx
   scintilla/src/RunStyles.cxx
   scintilla/src/Selection.cxx
   scintilla/src/Style.cxx
   scintilla/src/UndoHistory.cxx
   scintilla/src/UniConversion.cxx
   scintilla/src/UniqueString.cxx
   scintilla/src/ViewStyle.cxx
   scintilla/src/XPM.cxx
   scintilla/src/ScintillaBase.cxx)

add_library (scintilla_v5_engine OBJECT ${SCINTILLA_ENGINE_SOURCES})
add_library (scintilla class_scintilla.cxx class_scintilla_search.cxx)

# Match Lexilla's supported makefile: catalogue, all lexlib helpers and all lexer translation units.
# The catalogue references every module explicitly, retaining factories when linking the private static archive.

file (GLOB LEXILLA_LEXERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lexilla/lexers/Lex*.cxx")
file (GLOB LEXILLA_SUPPORT CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lexilla/lexlib/*.cxx")
add_library (scintilla_v5_lexilla STATIC lexilla/src/Lexilla.cxx ${LEXILLA_SUPPORT} ${LEXILLA_LEXERS})
set_target_properties (scintilla_v5_lexilla PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features (scintilla_v5_lexilla PRIVATE cxx_std_20)
target_compile_definitions (scintilla_v5_lexilla PRIVATE LEXILLA_NO_EXPORT)
scintilla_disable_exceptions (scintilla_v5_lexilla)
target_include_directories (scintilla_v5_lexilla PRIVATE lexilla/include lexilla/lexlib scintilla/include)
target_include_directories (scintilla PRIVATE lexilla/include)
target_link_libraries (scintilla PRIVATE scintilla_v5_engine scintilla_v5_lexilla)
target_link_libraries (scintilla PUBLIC unicode)
set_module_defaults (scintilla "Sci")

foreach (SCINTILLA_TARGET scintilla scintilla_v5_engine)
   scintilla_disable_exceptions (${SCINTILLA_TARGET})
   target_compile_features (${SCINTILLA_TARGET} PRIVATE cxx_std_20)
   target_compile_definitions (${SCINTILLA_TARGET} PRIVATE __KOTUKU__ NO_CXX11_REGEX)
   target_include_directories (${SCINTILLA_TARGET} PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scintilla/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/scintilla/include")
   if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options (${SCINTILLA_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
   endif ()
endforeach ()

# Keep the editor's Tiri keywords in step with the parser without linking to the Tiri module.

set (TIRI_TOKEN_SOURCE "${PROJECT_SOURCE_DIR}/src/tiri/jit/src/parser/lexer_types.h")
set_property (DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TIRI_TOKEN_SOURCE}")
file (READ "${TIRI_TOKEN_SOURCE}" TIRI_TOKEN_SOURCE_TEXT)
string (REGEX MATCHALL "TOKEN_DEF\\([a-z]+, +\"[a-z]+\", +TKF_RESERVED[^)]*\\)"
   TIRI_TOKEN_LINES "${TIRI_TOKEN_SOURCE_TEXT}")
set (TIRI_KEYWORDS "")
foreach (TIRI_TOKEN_LINE IN LISTS TIRI_TOKEN_LINES)
   string (REGEX REPLACE ".*TOKEN_DEF\\([^,]+, *\"([a-z]+)\".*" "\\1" TIRI_KEYWORD "${TIRI_TOKEN_LINE}")
   string (APPEND TIRI_KEYWORDS "   \"${TIRI_KEYWORD} \"\n")
endforeach ()
configure_file (tiri_keywords.h.in tiri_keywords.h @ONLY)

# Run the same public suites in either build tree; never duplicate their bodies.

foreach (SCINTILLA_TEST document render scroll repaint keyboard initial_focus clipboard margins highlighting
      completion hidpi mouse drag_drop changes diagnostics)
   flute_test (scintilla_${SCINTILLA_TEST}
      "${PROJECT_SOURCE_DIR}/src/scintilla/tests/test_scintilla_${SCINTILLA_TEST}.tiri")
endforeach ()

flute_test (scintilla_v5_lifecycle "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lifecycle.tiri")
flute_test (scintilla_v5_rendering "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rendering.tiri")
flute_test (scintilla_v5_input "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_input.tiri")
flute_test (scintilla_v5_lexer_state "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lexers.tiri")
