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

# Internal reusable mDNS support; no public Network ABI is added in Phase 3.
add_library (network_mdns STATIC mdns/mdns.cpp mdns/mdns_socket.cpp)
set_target_properties (network_mdns PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
   POSITION_INDEPENDENT_CODE ON)
if (WIN32)
   target_link_libraries (network_mdns PUBLIC ws2_32 iphlpapi)
   target_compile_definitions (network_mdns PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
endif ()

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
   if (OPENSSL_CRYPTO_LIBRARY MATCHES "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$")
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

      # Preserve FindOpenSSL's static dependencies while replacing -lzstd with the versioned runtime library
      # when the development symlink is unavailable. Use the project's zlib target as before.
      list (APPEND NETWORK_LIBS "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}" zlib)
      foreach (OPENSSL_TARGET OpenSSL::SSL OpenSSL::Crypto)
         get_target_property (OPENSSL_DEPENDENCIES ${OPENSSL_TARGET} INTERFACE_LINK_LIBRARIES)
         if (OPENSSL_DEPENDENCIES)
            foreach (OPENSSL_DEPENDENCY IN LISTS OPENSSL_DEPENDENCIES)
               if (OPENSSL_DEPENDENCY STREQUAL "OpenSSL::Crypto" OR OPENSSL_DEPENDENCY STREQUAL "ZLIB::ZLIB")
                  continue ()
               elseif (OPENSSL_DEPENDENCY STREQUAL "zstd" AND OPENSSL_ZSTD_LIBRARY)
                  list (APPEND NETWORK_LIBS "${OPENSSL_ZSTD_LIBRARY}")
               else ()
                  list (APPEND NETWORK_LIBS "${OPENSSL_DEPENDENCY}")
               endif ()
            endforeach ()
         endif ()
      endforeach ()
   else ()
      list (APPEND NETWORK_LIBS OpenSSL::SSL OpenSSL::Crypto)
   endif ()

   # Shared with the WebRTC DTLS transport so that both link OpenSSL identically.
   set (KOTUKU_OPENSSL_LIBS "${NETWORK_LIBS}" PARENT_SCOPE)

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

# SSL test certificates.  The localhost fixtures are staged in tests/ssl so that no platform depends on the openssl
# executable.  Refer to generate_ssl_certificates.cmake if the fixtures ever need to be regenerated.

set (SSL_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/ssl")
set (SSL_FIXTURES
   "${SSL_FIXTURE_DIR}/localhost.pem"
   "${SSL_FIXTURE_DIR}/localhost-key.pem"
   "${SSL_FIXTURE_DIR}/localhost-rsa-key.pem"
   "${SSL_FIXTURE_DIR}/localhost-ec.pem"
   "${SSL_FIXTURE_DIR}/localhost-ec-key.pem"
   "${SSL_FIXTURE_DIR}/localhost.p12"
   "${SSL_FIXTURE_DIR}/localhost-password.p12")

set (NETWORK_SSL_TESTS_ENABLED FALSE)
if (NOT DISABLE_SSL AND (WIN32 OR OPENSSL_FOUND))
   set (NETWORK_SSL_TESTS_ENABLED TRUE)

   if (KOTUKU_INSTALL)
      install (FILES ${SSL_FIXTURES} DESTINATION "${CONFIG_TARGET}/ssl")
   endif ()
endif ()

set (NETWORK_TESTS
   bind_address callback_results dns_parallel dns_capacity server_io client_server error_handling ipv6 concurrency
   performance keep_alive package_regressions
   # UDP Tests
   udp_basic udp_ipv6 udp_broadcast_multicast udp_tcp_coexistence udp_performance
)

if (WIN32)
   list (APPEND NETWORK_TESTS iocp_pipeline)
endif ()

if (NETWORK_SSL_TESTS_ENABLED)
   list (APPEND NETWORK_TESTS ssl)
endif ()

foreach (TEST_NAME ${NETWORK_TESTS})
   flute_test(network_${TEST_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${TEST_NAME}.tiri")
endforeach ()

if (UNIT_TESTS)
   add_executable (test_network_mdns mdns/test_mdns.cpp)
   set_target_properties (test_network_mdns PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   target_link_libraries (test_network_mdns PRIVATE network_mdns)
   add_test (NAME network_mdns COMMAND test_network_mdns)
   set_tests_properties (network_mdns PROPERTIES LABELS network TIMEOUT 30)
endif ()

option (NETWORK_MDNS_FUZZ "Build the internal mDNS codec/state libFuzzer target" OFF)
if (NETWORK_MDNS_FUZZ)
   if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message (FATAL_ERROR "NETWORK_MDNS_FUZZ requires Clang with libFuzzer support.")
   endif ()
   add_executable (fuzz_network_mdns mdns/fuzz_mdns.cpp mdns/mdns.cpp)
   set_target_properties (fuzz_network_mdns PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   target_compile_options (fuzz_network_mdns PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
   target_link_options (fuzz_network_mdns PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
   if (BUILD_TESTS)
      add_test (NAME network_mdns_fuzz_regression COMMAND fuzz_network_mdns -runs=1
         "${CMAKE_CURRENT_SOURCE_DIR}/mdns/fixtures")
      set_tests_properties (network_mdns_fuzz_regression PROPERTIES LABELS network TIMEOUT 60)
   endif ()
endif ()
