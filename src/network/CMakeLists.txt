# Network module
# If you need to do a deep debug of the network log, enable KOTUKU_VLOG at project level (it won't work
# at module level).

if (NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
   message (STATUS "Network module disabled: unsupported platform '${CMAKE_SYSTEM_NAME}'.")
   set (INC_MOD_NETWORK FALSE PARENT_SCOPE)
   return ()
endif ()

set (MOD network)
set (INC_MOD_NETWORK TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME "${MOD}_defs" OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   APPEND_IDL "module_def.c"
   ARGS "output-defs=module_def.c" "output-proto=module_def.c"
   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/network.cpp")

add_library (${MOD})

set_module_defaults (${MOD} "Net")

target_sources (${MOD} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/network.cpp")

if (WIN32)
   target_sources (${MOD} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/net_iocp.cpp")
   target_sources (${MOD} PRIVATE "win32/iocp.cpp")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
   target_sources (${MOD} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/net_linux.cpp")
else ()
   message (FATAL_ERROR "Network module has no platform backend for '${CMAKE_SYSTEM_NAME}'.")
endif ()

if (DISABLE_SSL)
   message (STATUS "Network module will not be compiled with SSL support (disabled by DISABLE_SSL).")
   target_compile_definitions (${MOD} PRIVATE "DISABLE_SSL")
elseif (WIN32)
   message (STATUS "Using Windows native SSL support.")
   target_sources (${MOD} PRIVATE "win32/ssl_wrapper.cpp")
   list (APPEND NETWORK_LIBS secur32 crypt32 bcrypt ncrypt)
elseif (OPENSSL_FOUND)
   message (STATUS "Network module will be compiled with OpenSSL support.")
   list (APPEND NETWORK_LIBS OpenSSL::SSL OpenSSL::Crypto)

   if (OPENSSL_CRYPTO_LIBRARY MATCHES "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$")
      list (APPEND NETWORK_LIBS zlib ${CMAKE_DL_LIBS})

      find_library (OPENSSL_ZSTD_LIBRARY NAMES zstd)
      if (NOT OPENSSL_ZSTD_LIBRARY)
         get_filename_component (OPENSSL_CRYPTO_LIBRARY_DIR "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)

         find_file (OPENSSL_ZSTD_RUNTIME_LIBRARY
            NAMES libzstd.so.1 libzstd.dylib
            PATHS
               "${OPENSSL_CRYPTO_LIBRARY_DIR}"
               "/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
               "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
               "/lib"
               "/usr/lib"
            PATH_SUFFIXES lib lib64)
         if (OPENSSL_ZSTD_RUNTIME_LIBRARY)
            set (OPENSSL_ZSTD_LIBRARY "${OPENSSL_ZSTD_RUNTIME_LIBRARY}")
         endif ()
      endif ()

      if (OPENSSL_ZSTD_LIBRARY)
         list (APPEND NETWORK_LIBS "${OPENSSL_ZSTD_LIBRARY}")
      endif ()
   endif ()

   target_include_directories (${MOD} PRIVATE "${OPENSSL_INCLUDE_DIR}")
else ()
   message (STATUS "Network module will not be compiled with SSL support (OpenSSL not found).")
   target_compile_definitions (${MOD} PRIVATE "DISABLE_SSL")
endif ()

if (WIN32)
   target_compile_definitions (${MOD} PRIVATE "_WINSOCK_DEPRECATED_NO_WARNINGS")
   list (APPEND NETWORK_LIBS ws2_32)
endif ()

target_link_libraries (${MOD} PRIVATE ${NETWORK_LIBS})

target_compile_definitions (${MOD} PRIVATE "MAX_ALIASES=64" "MAX_ADDRESSES=32" "HAVE_CONFIG_H=1")

# SSL Certificate Generation
find_program(MKCERT_EXECUTABLE mkcert PATHS ${CMAKE_SOURCE_DIR}/tools ENV PATH)
find_program(OPENSSL_EXECUTABLE openssl)

set(OPENSSL_RSA_TRADITIONAL_FLAG)
if(OPENSSL_EXECUTABLE)
   execute_process(
      COMMAND "${OPENSSL_EXECUTABLE}" rsa -help
      OUTPUT_VARIABLE OPENSSL_RSA_HELP
      ERROR_VARIABLE OPENSSL_RSA_HELP
      RESULT_VARIABLE OPENSSL_RSA_HELP_RESULT)
   if(OPENSSL_RSA_HELP MATCHES "(^|[ \\r\\n])-traditional([ \\r\\n]|$)")
      set(OPENSSL_RSA_TRADITIONAL_FLAG -traditional)
   endif()
endif()

set(SSL_DATA_DIR "${CMAKE_SOURCE_DIR}/data/ssl")
set(LOCALHOST_PEM "${SSL_DATA_DIR}/localhost.pem")
set(LOCALHOST_KEY_PEM "${SSL_DATA_DIR}/localhost-key.pem")
set(LOCALHOST_RSA_KEY_PEM "${SSL_DATA_DIR}/localhost-rsa-key.pem")
set(LOCALHOST_EC_PEM "${SSL_DATA_DIR}/localhost-ec.pem")
set(LOCALHOST_EC_KEY_PEM "${SSL_DATA_DIR}/localhost-ec-key.pem")
set(LOCALHOST_P12 "${SSL_DATA_DIR}/localhost.p12")
set(LOCALHOST_PASSWORD_P12 "${SSL_DATA_DIR}/localhost-password.p12")
set(MKCERT_PEM "${SSL_DATA_DIR}/localhost+2.pem")
set(MKCERT_KEY_PEM "${SSL_DATA_DIR}/localhost+2-key.pem")

if(MKCERT_EXECUTABLE AND OPENSSL_EXECUTABLE)
   # Custom command to generate SSL certificates only when they don't exist
   add_custom_command(
      OUTPUT "${LOCALHOST_PEM}" "${LOCALHOST_KEY_PEM}" "${LOCALHOST_RSA_KEY_PEM}" "${LOCALHOST_P12}"
             "${LOCALHOST_PASSWORD_P12}" "${LOCALHOST_EC_PEM}" "${LOCALHOST_EC_KEY_PEM}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${SSL_DATA_DIR}"
      COMMAND ${CMAKE_COMMAND} -E remove -f "${LOCALHOST_PEM}" "${LOCALHOST_KEY_PEM}" "${LOCALHOST_P12}"
              "${LOCALHOST_PASSWORD_P12}" "${LOCALHOST_RSA_KEY_PEM}" "${LOCALHOST_EC_PEM}"
              "${LOCALHOST_EC_KEY_PEM}" "${MKCERT_PEM}" "${MKCERT_KEY_PEM}"
      COMMAND ${CMAKE_COMMAND} -E chdir "${SSL_DATA_DIR}" "${MKCERT_EXECUTABLE}" localhost 127.0.0.1 ::1
      COMMAND ${CMAKE_COMMAND} -E rename "${MKCERT_PEM}" "${LOCALHOST_PEM}"
      COMMAND ${CMAKE_COMMAND} -E rename "${MKCERT_KEY_PEM}" "${LOCALHOST_KEY_PEM}"
      COMMAND "${OPENSSL_EXECUTABLE}" rsa -in "${LOCALHOST_KEY_PEM}" ${OPENSSL_RSA_TRADITIONAL_FLAG} -out "${LOCALHOST_RSA_KEY_PEM}"
      COMMAND "${OPENSSL_EXECUTABLE}" ecparam -name prime256v1 -genkey -noout -out "${LOCALHOST_EC_KEY_PEM}"
      COMMAND "${OPENSSL_EXECUTABLE}" req -new -x509 -key "${LOCALHOST_EC_KEY_PEM}" -out "${LOCALHOST_EC_PEM}"
              -days 3650 -subj "/CN=localhost"
      COMMAND "${OPENSSL_EXECUTABLE}" pkcs12 -export -out "${LOCALHOST_P12}"
              -inkey "${LOCALHOST_KEY_PEM}" -in "${LOCALHOST_PEM}" -passout pass:
      COMMAND "${OPENSSL_EXECUTABLE}" pkcs12 -export -out "${LOCALHOST_PASSWORD_P12}"
              -inkey "${LOCALHOST_KEY_PEM}" -in "${LOCALHOST_PEM}" -passout pass:kotuku-test
      COMMENT "Generating SSL certificates for localhost testing"
      VERBATIM
   )

   # Custom target that depends on the certificate files
   add_custom_target(generate_ssl_certificates DEPENDS "${LOCALHOST_PEM}" "${LOCALHOST_KEY_PEM}"
      "${LOCALHOST_RSA_KEY_PEM}" "${LOCALHOST_EC_PEM}" "${LOCALHOST_EC_KEY_PEM}" "${LOCALHOST_P12}"
      "${LOCALHOST_PASSWORD_P12}")

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
   bind_address dns_parallel server_io client_server error_handling ipv6 concurrency ssl iocp_pipeline
   performance keep_alive
   # UDP Tests
   udp_basic udp_ipv6 udp_broadcast_multicast udp_tcp_coexistence udp_performance
)

foreach (TEST_NAME ${NETWORK_TESTS})
   flute_test(network_${TEST_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${TEST_NAME}.tiri")
endforeach ()
