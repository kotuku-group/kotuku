# Network module
# If you need to do a deep debug of the network log, enable KOTUKU_VLOG at project level (it won't work
# at module level).

set (MOD network)
set (INC_MOD_NETWORK TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME "${MOD}_defs" OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   APPEND_IDL "module_def.c"
   FILES "network.cpp"
   ARGS "output-defs=module_def.c" "output-proto=module_def.c")

add_library (${MOD})

set_module_defaults (${MOD} "Net")

target_sources (${MOD} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/network.cpp")

if (DISABLE_SSL)
   message (STATUS "Network module will not be compiled with SSL support (disabled by DISABLE_SSL).")
   target_compile_definitions (${MOD} PRIVATE "DISABLE_SSL")
elseif (WIN32)
   message (STATUS "Using Windows native SSL support.")
   target_sources (${MOD} PRIVATE "win32/ssl_wrapper.cpp")
   list (APPEND NETWORK_LIBS secur32 crypt32)
elseif (OPENSSL_FOUND)
   message (STATUS "Network module will be compiled with OpenSSL support.")
   # Linking against the .a files is necessary to ensure a static link.
   list (APPEND NETWORK_LIBS ssl.a crypto.a)
   target_include_directories (${MOD} PRIVATE "${OPENSSL_INCLUDE_DIR}")
else ()
   message (STATUS "Network module will not be compiled with SSL support (OpenSSL not found).")
   target_compile_definitions (${MOD} PRIVATE "DISABLE_SSL")
endif ()

if (WIN32)
   target_compile_definitions (${MOD} PRIVATE "_WINSOCK_DEPRECATED_NO_WARNINGS")
   target_sources (${MOD} PRIVATE "win32/winsockwrappers.cpp")
   list (APPEND NETWORK_LIBS wsock32 gdi32 ws2_32)
endif ()

target_link_libraries (${MOD} PRIVATE ${NETWORK_LIBS})

target_compile_definitions (${MOD} PRIVATE "MAX_ALIASES=64" "MAX_ADDRESSES=32" "HAVE_CONFIG_H=1")

# SSL Certificate Generation
find_program(MKCERT_EXECUTABLE mkcert PATHS ${CMAKE_SOURCE_DIR}/tools ENV PATH)
find_program(OPENSSL_EXECUTABLE openssl)

set(SSL_DATA_DIR "${CMAKE_SOURCE_DIR}/data/ssl")
set(LOCALHOST_PEM "${SSL_DATA_DIR}/localhost.pem")
set(LOCALHOST_KEY_PEM "${SSL_DATA_DIR}/localhost-key.pem")
set(LOCALHOST_P12 "${SSL_DATA_DIR}/localhost.p12")
set(MKCERT_PEM "${SSL_DATA_DIR}/localhost+2.pem")
set(MKCERT_KEY_PEM "${SSL_DATA_DIR}/localhost+2-key.pem")

if(MKCERT_EXECUTABLE AND OPENSSL_EXECUTABLE)
   # Custom command to generate SSL certificates only when they don't exist
   add_custom_command(
      OUTPUT "${LOCALHOST_PEM}" "${LOCALHOST_P12}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${SSL_DATA_DIR}"
      COMMAND ${CMAKE_COMMAND} -E remove -f "${LOCALHOST_PEM}" "${LOCALHOST_KEY_PEM}" "${LOCALHOST_P12}" "${MKCERT_PEM}" "${MKCERT_KEY_PEM}"
      COMMAND ${CMAKE_COMMAND} -E chdir "${SSL_DATA_DIR}" "${MKCERT_EXECUTABLE}" localhost 127.0.0.1 ::1
      COMMAND ${CMAKE_COMMAND} -E rename "${MKCERT_PEM}" "${LOCALHOST_PEM}"
      COMMAND ${CMAKE_COMMAND} -E rename "${MKCERT_KEY_PEM}" "${LOCALHOST_KEY_PEM}"
      COMMAND "${OPENSSL_EXECUTABLE}" pkcs12 -export -out "${LOCALHOST_P12}"
              -inkey "${LOCALHOST_KEY_PEM}" -in "${LOCALHOST_PEM}" -passout pass:
      COMMAND ${CMAKE_COMMAND} -E remove -f "${LOCALHOST_KEY_PEM}"
      COMMENT "Generating SSL certificates for localhost testing"
      VERBATIM
   )

   # Custom target that depends on the certificate files
   add_custom_target(generate_ssl_certificates DEPENDS "${LOCALHOST_PEM}" "${LOCALHOST_P12}")

   # Make the network module depend on certificate generation
   add_dependencies(${MOD} generate_ssl_certificates)

   message(STATUS "SSL certificate generation enabled (mkcert and openssl found)")
else()
   if(NOT MKCERT_EXECUTABLE)
      message(STATUS "mkcert not found - SSL certificates will not be auto-generated")
      message(STATUS "Install mkcert from: https://github.com/FiloSottile/mkcert")
   endif()

   if(NOT OPENSSL_EXECUTABLE)
      message(STATUS "openssl not found - SSL certificates will not be auto-generated")
   endif()
endif()

set (NETWORK_TESTS
   bind_address dns_parallel server_io client_server error_handling ipv6 concurrency ssl
   # UDP Tests
   udp_basic udp_ipv6 udp_broadcast_multicast udp_tcp_coexistence
   # Performance test (may take a while)
   # udp_performance
)

foreach (TEST_NAME ${NETWORK_TESTS})
   flute_test(network_${TEST_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${TEST_NAME}.tiri")
endforeach ()
