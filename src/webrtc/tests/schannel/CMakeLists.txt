# Standalone Phase 2 validation, usable before the module's backend selection is integrated in Phase 3.
cmake_minimum_required (VERSION 3.28)
project (webrtc_schannel_tests LANGUAGES CXX)

if (NOT WIN32)
   message (FATAL_ERROR "The Schannel backend requires Windows.")
endif ()

get_filename_component (KOTUKU_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set (KOTUKU_BUILD_DIR "${KOTUKU_SOURCE_DIR}/build/agents-${CMAKE_HOST_SYSTEM_NAME}" CACHE PATH
   "Configured Kotuku build containing kotuku/config.h")
if (NOT EXISTS "${KOTUKU_BUILD_DIR}/kotuku/config.h")
   message (FATAL_ERROR "Configure the agents-platform preset first, or set KOTUKU_BUILD_DIR.")
endif ()

add_library (schannel_dtls STATIC
   ../../dtls/dtls_schannel.cpp ../../dtls/dtls_transport.cpp
   ../../../crypto/protocol_cng.cpp ../../../crypto/protocol_primitives.cpp)
target_compile_features (schannel_dtls PUBLIC cxx_std_20)
target_compile_definitions (schannel_dtls PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX CRYPTO_PROTOCOL_CNG=1)
target_include_directories (schannel_dtls PUBLIC "${KOTUKU_SOURCE_DIR}/include" "${KOTUKU_BUILD_DIR}")
target_link_libraries (schannel_dtls PUBLIC secur32 crypt32 ncrypt bcrypt)

add_executable (test_webrtc_dtls ../test_dtls.cpp)
target_link_libraries (test_webrtc_dtls PRIVATE schannel_dtls)
install (TARGETS test_webrtc_dtls RUNTIME DESTINATION .)

enable_testing ()
add_test (NAME webrtc_dtls_schannel COMMAND "${CMAKE_INSTALL_PREFIX}/test_webrtc_dtls.exe")
set_tests_properties (webrtc_dtls_schannel PROPERTIES LABELS "webrtc;webrtc_windows_essential" TIMEOUT 40)
