# Backstage Module

set (MOD backstage)
set (INC_MOD_BACKSTAGE TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h")

add_library (${MOD})
target_link_libraries (${MOD} PUBLIC unicode base64)
set_module_defaults (${MOD} "Bk")

target_sources (${MOD} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${MOD}.cpp")

if (NOT (ORIGO_CMD STREQUAL "ORIGO_CMD-NOTFOUND"))
   set (BACKSTAGE_ROUTES_STAMP "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/backstage_routes.stamp")
   set (BACKSTAGE_ROUTES_TABLE "${CMAKE_CURRENT_SOURCE_DIR}/routes.cpp")

   add_custom_command (OUTPUT "${BACKSTAGE_ROUTES_STAMP}"
      BYPRODUCTS "${BACKSTAGE_ROUTES_TABLE}"
      COMMAND ${ORIGO_CMD} "${CMAKE_CURRENT_SOURCE_DIR}/interface.tiri" "--log-warning"
      COMMAND ${CMAKE_COMMAND} -E touch "${BACKSTAGE_ROUTES_STAMP}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/interface.tiri"
      COMMENT "Generating Backstage route table and missing route stubs"
      VERBATIM)

   add_custom_target (backstage_routes
      DEPENDS "${BACKSTAGE_ROUTES_STAMP}"
      SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/interface.tiri")

   add_dependencies (${MOD} backstage_routes)
endif ()

flute_test (backstage_ping "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_backstage.tiri")
