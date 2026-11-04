# Font module

set (MOD "font")
set (INC_MOD_FONT TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl"
   NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/font.h"
   ARGS  "output-defs=font_def.c" "output-proto=font_def.c"
   APPEND_IDL "font_def.c"
   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/font.cpp")

add_library (${MOD})

# A bug in (MinGW?) GCC requires this option so that __packed__ works correctly.
if (NOT MSVC)
   target_compile_options (${MOD} PRIVATE -mno-ms-bitfields)
endif ()

set_module_defaults (${MOD} "Font")

target_sources (${MOD} PRIVATE "font.cpp" ${FT_SOURCE})

target_link_libraries (${MOD} PUBLIC unicode)
target_link_libraries (${MOD} PRIVATE ${MATH_LINK} freetype)

# TODO: Kerning tests aren't working with the latest version of Freetype
flute_test(${MOD} "${CMAKE_CURRENT_SOURCE_DIR}/tests/font_tests.tiri")
