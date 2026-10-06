# WebSocket Module
#
# Implements RFC 6455 client and server connections.  Requires the Network, HTTP and Crypto modules.

set (MOD websocket)
set (INC_MOD_WEBSOCKET TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h" APPEND_IDL "class_websocket_def.c")

add_library (${MOD})
set_module_defaults (${MOD} "WebSocket")
target_sources (${MOD} PRIVATE "${MOD}.cpp" ws_frame.cpp ws_utf8.cpp ws_handshake.cpp ws_send_queue.cpp)

if (UNIT_TESTS)
   target_sources (${MOD} PRIVATE tests/unit_protocol.cpp)
   target_compile_definitions (${MOD} PRIVATE UNIT_TESTS)
   flute_test (websocket_unit_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_unit_tests.tiri")
endif ()

foreach (TEST_NAME client server loopback tls)
   flute_test (websocket_${TEST_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${TEST_NAME}.tiri")
endforeach ()

# OpenSSL trusts the self-signed localhost fixture through SSL_CERT_FILE, so that test_tls.tiri can verify server
# identity.  Windows native TLS ignores the variable and the test falls back to checking that the fixture is refused.

if (BUILD_TESTS AND NOT WIN32)
   set_tests_properties (websocket_tls PROPERTIES
      ENVIRONMENT "SSL_CERT_FILE=${CMAKE_INSTALL_PREFIX}/${CONFIG_TARGET}/ssl/localhost.pem")
endif ()
