# Font module

set (MOD "font")
set (INC_MOD_FONT TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl"
   NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/font.h"
   FILES "font.cpp"
   ARGS  "output-defs=font_def.c" "output-proto=font_def.c"
   APPEND_IDL "font_def.c")

# A bug in (MinGW?) GCC requires that mno-ms-bitfields is applied to ensure that __packed__ works correctly.

if (NOT MSVC)
   set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mno-ms-bitfields")
   set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mno-ms-bitfields")
endif ()

add_library (${MOD})

set_module_defaults (${MOD} "Font")

target_sources (${MOD} PRIVATE "font.cpp" ${FT_SOURCE})

target_link_libraries (${MOD} PUBLIC unicode)
target_link_libraries (${MOD} PRIVATE ${MATH_LINK} freetype)

# TODO: Kerning tests aren't working with the latest version of Freetype
flute_test(${MOD} "${CMAKE_CURRENT_SOURCE_DIR}/tests/font_tests.tiri")
