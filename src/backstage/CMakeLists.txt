# Backstage Module

set (MOD backstage)
set (INC_MOD_BACKSTAGE TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h")

add_library (${MOD})
target_link_libraries (${MOD} PUBLIC unicode)
set_module_defaults (${MOD} "Bk")

target_sources (${MOD} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${MOD}.cpp")

flute_test (${MOD}_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/test.tiri")
