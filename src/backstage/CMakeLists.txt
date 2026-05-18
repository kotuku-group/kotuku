# Backstage Module

set (MOD backstage)
set (INC_MOD_BACKSTAGE TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h")

add_library (${MOD})
target_link_libraries (${MOD} PUBLIC unicode)
set_module_defaults (${MOD} "Bk")

target_sources (${MOD} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${MOD}.cpp")

add_custom_target (backstage_routes
   COMMAND ${ORIGO_CMD} "${CMAKE_CURRENT_SOURCE_DIR}/interface.tiri" "--log-warning"
   WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
   SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/interface.tiri"
   COMMENT "Generating Backstage route table and missing route stubs"
   VERBATIM)

flute_test (backstage_ping "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_backstage.tiri")
