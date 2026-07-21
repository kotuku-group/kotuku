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

if (MSVC)
   target_compile_options (${MOD} PRIVATE /MP)
endif ()

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

# SSL test certificate generation
find_program(OPENSSL_EXECUTABLE openssl)

if (OPENSSL_EXECUTABLE)
   execute_process (
      COMMAND "${OPENSSL_EXECUTABLE}" version
      RESULT_VARIABLE OPENSSL_EXECUTABLE_VERSION_RESULT
      OUTPUT_VARIABLE OPENSSL_EXECUTABLE_VERSION_OUTPUT
      ERROR_VARIABLE OPENSSL_EXECUTABLE_VERSION_ERROR
      OUTPUT_STRIP_TRAILING_WHITESPACE)

   if ((NOT OPENSSL_EXECUTABLE_VERSION_RESULT EQUAL 0) OR
      (NOT OPENSSL_EXECUTABLE_VERSION_OUTPUT MATCHES "^OpenSSL[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)"))
      message (STATUS "The openssl executable is unavailable or did not report a supported version")
      set (OPENSSL_EXECUTABLE "")
   elseif (CMAKE_MATCH_1 VERSION_LESS KOTUKU_OPENSSL_MINIMUM_VERSION)
      message (STATUS
         "The openssl executable is version ${CMAKE_MATCH_1}; "
         "version ${KOTUKU_OPENSSL_MINIMUM_VERSION} or later is required")
      set (OPENSSL_EXECUTABLE "")
   endif ()
endif ()

set (SSL_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ssl")
set (SSL_FIXTURE_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/generate_ssl_certificates.cmake")
set (LOCALHOST_PEM "${SSL_FIXTURE_DIR}/localhost.pem")
set (LOCALHOST_KEY_PEM "${SSL_FIXTURE_DIR}/localhost-key.pem")
set (LOCALHOST_RSA_KEY_PEM "${SSL_FIXTURE_DIR}/localhost-rsa-key.pem")
set (LOCALHOST_EC_PEM "${SSL_FIXTURE_DIR}/localhost-ec.pem")
set (LOCALHOST_EC_KEY_PEM "${SSL_FIXTURE_DIR}/localhost-ec-key.pem")
set (LOCALHOST_P12 "${SSL_FIXTURE_DIR}/localhost.p12")
set (LOCALHOST_PASSWORD_P12 "${SSL_FIXTURE_DIR}/localhost-password.p12")
set (SSL_FIXTURES
   "${LOCALHOST_PEM}"
   "${LOCALHOST_KEY_PEM}"
   "${LOCALHOST_RSA_KEY_PEM}"
   "${LOCALHOST_EC_PEM}"
   "${LOCALHOST_EC_KEY_PEM}"
   "${LOCALHOST_P12}"
   "${LOCALHOST_PASSWORD_P12}")

set (NETWORK_SSL_TESTS_ENABLED FALSE)
if (NOT DISABLE_SSL AND (WIN32 OR OPENSSL_FOUND) AND OPENSSL_EXECUTABLE)
   set (NETWORK_SSL_TESTS_ENABLED TRUE)
   add_custom_target (generate_ssl_certificates ALL
      COMMAND ${CMAKE_COMMAND}
         "-DOPENSSL_EXECUTABLE=${OPENSSL_EXECUTABLE}"
         "-DOUTPUT_DIR=${SSL_FIXTURE_DIR}"
         -P "${SSL_FIXTURE_GENERATOR}"
      BYPRODUCTS ${SSL_FIXTURES} "${SSL_FIXTURE_DIR}/fixture-version.txt"
      DEPENDS "${SSL_FIXTURE_GENERATOR}"
      COMMENT "Checking Network SSL test certificates"
      VERBATIM)

   add_dependencies (${MOD} generate_ssl_certificates)

   if (KOTUKU_INSTALL)
      if (WIN32)
         install (FILES "${LOCALHOST_PEM}" "${LOCALHOST_P12}" "${LOCALHOST_PASSWORD_P12}"
            DESTINATION "${CONFIG_TARGET}/ssl")
      else ()
         install (FILES ${SSL_FIXTURES} DESTINATION "${CONFIG_TARGET}/ssl")
      endif ()
   endif ()

   message (STATUS "Network SSL test certificate generation enabled")
elseif (NOT DISABLE_SSL AND (WIN32 OR OPENSSL_FOUND) AND BUILD_TESTS)
   message (STATUS
      "A supported openssl executable was not found - Network SSL tests will be disabled and certificate fixtures "
      "will not be generated")
elseif (NOT DISABLE_SSL AND (WIN32 OR OPENSSL_FOUND))
   message (STATUS "openssl not found - Network SSL test certificates will not be generated")
endif ()

set (NETWORK_TESTS
   bind_address dns_parallel server_io client_server error_handling ipv6 concurrency iocp_pipeline
   performance keep_alive
   # UDP Tests
   udp_basic udp_ipv6 udp_broadcast_multicast udp_tcp_coexistence udp_performance
)

if (NETWORK_SSL_TESTS_ENABLED)
   list (APPEND NETWORK_TESTS ssl)
endif ()

foreach (TEST_NAME ${NETWORK_TESTS})
   flute_test(network_${TEST_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${TEST_NAME}.tiri")
endforeach ()
