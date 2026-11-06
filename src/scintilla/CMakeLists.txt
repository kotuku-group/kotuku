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

message (STATUS "Scintilla v5 backend: Scintilla 5.6.9; Lexilla 5.5.5; "
   "native desktop/platform acceptance pending")

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
      completion hidpi mouse drag_drop)
   flute_test (scintilla_${SCINTILLA_TEST}
      "${PROJECT_SOURCE_DIR}/src/scintilla/tests/test_scintilla_${SCINTILLA_TEST}.tiri")
endforeach ()

if (UNIT_TESTS)
   add_executable (test_scintilla_no_exceptions tests/test_no_exceptions.cpp
      scintilla/src/Selection.cxx scintilla/src/UndoHistory.cxx scintilla/src/UniConversion.cxx)
   target_compile_features (test_scintilla_no_exceptions PRIVATE cxx_std_20)
   target_include_directories (test_scintilla_no_exceptions PRIVATE scintilla/src scintilla/include)
   scintilla_disable_exceptions (test_scintilla_no_exceptions)
   add_test (NAME scintilla_no_exceptions COMMAND test_scintilla_no_exceptions)

   add_executable (test_scintilla_v5_types tests/test_adapter_types.cpp)
   target_compile_features (test_scintilla_v5_types PRIVATE cxx_std_20)
   add_test (NAME scintilla_v5_types COMMAND test_scintilla_v5_types)

   add_executable (test_scintilla_v5_surface tests/test_surface.cpp)
   if (NOT KOTUKU_STATIC)
      target_sources (test_scintilla_v5_surface PRIVATE scintilla/src/Geometry.cxx)
   endif ()
   target_compile_features (test_scintilla_v5_surface PRIVATE cxx_std_20)
   target_compile_definitions (test_scintilla_v5_surface PRIVATE __KOTUKU__)
   target_include_directories (test_scintilla_v5_surface PRIVATE scintilla/src scintilla/include)
   target_link_libraries (test_scintilla_v5_surface PRIVATE ${INIT_LINK} unicode)
   add_dependencies (test_scintilla_v5_surface scintilla)
   if (WIN32 OR KOTUKU_STATIC)
      install (TARGETS test_scintilla_v5_surface RUNTIME DESTINATION .)
      set (SCINTILLA_SURFACE_PROGRAM "${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:test_scintilla_v5_surface>")
   else ()
      set (SCINTILLA_SURFACE_PROGRAM "$<TARGET_FILE:test_scintilla_v5_surface>")
   endif ()
   add_test (NAME scintilla_v5_surface COMMAND "${SCINTILLA_SURFACE_PROGRAM}" --log-warning --gfx-driver=headless
      WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
   set_tests_properties (scintilla_v5_surface PROPERTIES LABELS scintilla_v5_surface TIMEOUT 30)

   add_executable (test_scintilla_v5_input tests/test_input.cpp)
   target_compile_features (test_scintilla_v5_input PRIVATE cxx_std_20)
   target_compile_definitions (test_scintilla_v5_input PRIVATE __KOTUKU__)
   target_include_directories (test_scintilla_v5_input PRIVATE scintilla/src scintilla/include)
   target_link_libraries (test_scintilla_v5_input PRIVATE ${INIT_LINK})
   add_dependencies (test_scintilla_v5_input scintilla)
   if (WIN32 OR KOTUKU_STATIC)
      install (TARGETS test_scintilla_v5_input RUNTIME DESTINATION .)
      set (SCINTILLA_INPUT_PROGRAM "${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:test_scintilla_v5_input>")
   else ()
      set (SCINTILLA_INPUT_PROGRAM "$<TARGET_FILE:test_scintilla_v5_input>")
   endif ()
   add_test (NAME scintilla_v5_scheduling COMMAND "${SCINTILLA_INPUT_PROGRAM}" --log-warning --gfx-driver=headless
      WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
   set_tests_properties (scintilla_v5_scheduling PROPERTIES LABELS scintilla_v5_scheduling TIMEOUT 30)
   add_executable (test_scintilla_v5_lexers tests/test_lexers.cpp)
   target_compile_features (test_scintilla_v5_lexers PRIVATE cxx_std_20)
   target_compile_definitions (test_scintilla_v5_lexers PRIVATE __KOTUKU__)
   target_include_directories (test_scintilla_v5_lexers PRIVATE scintilla/src scintilla/include lexilla/include)
   target_link_libraries (test_scintilla_v5_lexers PRIVATE ${INIT_LINK})
   add_dependencies (test_scintilla_v5_lexers scintilla)
   if (WIN32 OR KOTUKU_STATIC)
      install (TARGETS test_scintilla_v5_lexers RUNTIME DESTINATION .)
      set (SCINTILLA_LEXERS_PROGRAM "${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:test_scintilla_v5_lexers>")
   else ()
      set (SCINTILLA_LEXERS_PROGRAM "$<TARGET_FILE:test_scintilla_v5_lexers>")
   endif ()
   add_test (NAME scintilla_v5_lexers COMMAND "${SCINTILLA_LEXERS_PROGRAM}" --log-warning --gfx-driver=headless
      WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
   set_tests_properties (scintilla_v5_lexers PROPERTIES LABELS scintilla_v5_lexers TIMEOUT 30)
endif ()
flute_test (scintilla_v5_lifecycle "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lifecycle.tiri")
flute_test (scintilla_v5_rendering "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rendering.tiri")
flute_test (scintilla_v5_input "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_input.tiri")
flute_test (scintilla_v5_lexer_state "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lexers.tiri")
