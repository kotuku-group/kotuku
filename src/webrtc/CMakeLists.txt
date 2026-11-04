# WebRTC module: PeerConnection and WebChannel data channels over ICE, DTLS and SCTP.
#
# Test labels:
#   webrtc                      Native protocol unit tests (UNIT_TESTS) and Flute tests
#   webrtc_interop              Chromium data-channel interop driven by Playwright
#   webrtc_mdns_interop         Chromium interop with mDNS host candidates
#   webrtc_turn_integration     Ephemeral coturn fixture (WEBRTC_TURN_SERVER, WEBRTC_TURN_UCLIENT)
#   webrtc_topology             Opt-in Linux namespace fixtures for relay fallback and path removal
#   webrtc_windows_essential    Windows only: the routine development/CI gate
#   webrtc_windows_extended     Windows only: every other webrtc_* test
#
# Browser, coturn, namespace and Windows acceptance fixtures are registered by tests/interop/interop.cmake.
# Fuzz targets are built with WEBRTC_FUZZ and Clang.

if (NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
   message (STATUS "WebRTC disabled: this platform is unsupported.")
   return ()
endif ()

# Windows uses the same socket-independent OpenSSL DTLS backend; Network TLS remains on Schannel.

if (WIN32 AND NOT DISABLE_SSL)
   add_compile_definitions (WIN32_LEAN_AND_MEAN NOMINMAX)
   find_package (OpenSSL 1.1.1 REQUIRED)
   set (KOTUKU_OPENSSL_LIBS OpenSSL::SSL OpenSSL::Crypto ws2_32 iphlpapi)
endif ()

if (DISABLE_SSL OR NOT KOTUKU_OPENSSL_LIBS)
   message (STATUS "WebRTC disabled: OpenSSL is required for DTLS.")
   return ()
endif ()

find_package (Threads REQUIRED)

option (WEBRTC_FUZZ "Build the WebRTC protocol parser libFuzzer targets (requires UNIT_TESTS)" OFF)

set (MOD webrtc)
set (INC_MOD_WEBRTC TRUE PARENT_SCOPE)

if (RUN_ANYWHERE)
   set (WEBRTC_ORIGO "${CMAKE_INSTALL_PREFIX}/origo${CMAKE_EXECUTABLE_SUFFIX}")
else ()
   set (WEBRTC_ORIGO "${CMAKE_INSTALL_PREFIX}/bin/origo${CMAKE_EXECUTABLE_SUFFIX}")
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Support libraries

# PRECIS OpaqueString preparation for STUN/TURN credentials.  utf8proc is patched locally until it is retired.

include (FetchContent)
set (UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
set (WEBRTC_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set (BUILD_SHARED_LIBS OFF)
FetchContent_Declare (utf8proc
   GIT_REPOSITORY https://github.com/JuliaStrings/utf8proc.git
   GIT_TAG a1b99daa2a3393884220264c927a48ba1251a9c6
   EXCLUDE_FROM_ALL
   SYSTEM)
FetchContent_MakeAvailable (utf8proc)
set (BUILD_SHARED_LIBS ${WEBRTC_SHARED_LIBS_SAVED})
include ("${CMAKE_CURRENT_SOURCE_DIR}/patch_utf8proc.cmake")
patch_utf8proc ("${utf8proc_SOURCE_DIR}/utf8proc.c")

add_library (webrtc_precis STATIC precis/opaque_string.cpp)
set_target_properties (webrtc_precis PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_link_libraries (webrtc_precis PUBLIC utf8proc unicode)

add_library (crypto_protocol STATIC ../crypto/protocol_primitives.cpp)
set_target_properties (crypto_protocol PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions (crypto_protocol PUBLIC CRYPTO_PROTOCOL_OPENSSL=1)
target_include_directories (crypto_protocol PRIVATE "${OPENSSL_INCLUDE_DIR}")
target_link_libraries (crypto_protocol PUBLIC ${KOTUKU_OPENSSL_LIBS})

# WEBRTC_OPENSSL_LIBS is OpenSSL alone, for fuzz targets that compile their dependencies with instrumentation.
set (WEBRTC_OPENSSL_LIBS ${KOTUKU_OPENSSL_LIBS})
list (PREPEND KOTUKU_OPENSSL_LIBS crypto_protocol webrtc_precis)

# Interface notifications are owned by the task driving PeerConnection, including native test implementations.

add_library (webrtc_interfaces STATIC ice/interface_monitor.cpp)
set_target_properties (webrtc_interfaces PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
   POSITION_INDEPENDENT_CODE ON)
if (WIN32)
   target_link_libraries (webrtc_interfaces PUBLIC ws2_32 iphlpapi)
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Protocol layers.  Each list includes the layers it depends on, so that unit tests can compile a single layer
# without the framework.  Host candidate parsing and resolution also require the network_mdns library.

set (WEBRTC_STUN_SOURCES stun/stun.cpp)
set (WEBRTC_ICE_SOURCES ice/ice.cpp ice/ice_agent.cpp ${WEBRTC_STUN_SOURCES})
set (WEBRTC_ICE_DTLS_SOURCES ice/ice_dtls.cpp dtls/dtls_openssl.cpp ${WEBRTC_ICE_SOURCES})
set (WEBRTC_TRANSPORT_SOURCES ice/ice_socket.cpp ice/ice_candidates.cpp ice/ice_servers.cpp turn/turn.cpp
   turn/turn_socket.cpp ${WEBRTC_ICE_DTLS_SOURCES})
set (WEBRTC_SCTP_SOURCES sctp/sctp_packet.cpp sctp/sctp_association.cpp sctp/sctp_transfer.cpp sctp/sctp_streams.cpp
   sctp/dcep.cpp)
set (WEBRTC_PEER_SOURCES peerconnection/peer_engine.cpp sdp/sdp.cpp ${WEBRTC_TRANSPORT_SOURCES}
   ${WEBRTC_SCTP_SOURCES})

# --------------------------------------------------------------------------------------------------------------------
# Module

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   APPEND_IDL "module_def.c"
   ARGS "output-defs=module_def.c"
   BYPRODUCTS "webchannel/webchannel_def.c" "peerconnection/peerconnection_def.c"
   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/webrtc.cpp")

add_library (${MOD})
set_module_defaults (${MOD} "WebRTC")
target_sources (${MOD} PRIVATE webrtc.cpp ${WEBRTC_PEER_SOURCES})
target_include_directories (${MOD} PRIVATE "${OPENSSL_INCLUDE_DIR}")
target_link_libraries (${MOD} PRIVATE ${KOTUKU_OPENSSL_LIBS} network_mdns webrtc_interfaces Threads::Threads)
add_dependencies (${MOD} network)

# --------------------------------------------------------------------------------------------------------------------
# Flute tests

flute_test (webrtc_turn_configuration "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_turn_configuration.tiri")
flute_test (webrtc_turn_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_turn_fixtures.tiri")
flute_test (webrtc_peer_loopback "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_peer_loopback.tiri")
flute_test (webrtc_peer_restart "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_peer_restart.tiri")
flute_test (webrtc_peer_stats "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_peer_stats.tiri")
flute_test (webrtc_webchannel_loopback "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_webchannel_loopback.tiri")
flute_test (webrtc_webchannel_lifecycle "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_webchannel_lifecycle.tiri")
flute_test (webrtc_mdns_privacy "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mdns_privacy.tiri")

if (BUILD_TESTS)
   set_tests_properties (webrtc_turn_fixtures PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
   set_tests_properties (webrtc_webchannel_loopback PROPERTIES TIMEOUT 150)
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Native unit tests

# The Windows bootstrap locates Core relative to the executable, rather than its working directory.
# Install native integration probes beside Origo so Core also selects this build's module and package paths.

function (webrtc_native_program Target Output)
   if (WIN32)
      install (TARGETS ${Target} RUNTIME DESTINATION .)
      set (${Output} "${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:${Target}>" PARENT_SCOPE)
   else ()
      set (${Output} "$<TARGET_FILE:${Target}>" PARENT_SCOPE)
   endif ()
endfunction ()

# webrtc_unit_test (Name [FIXTURES] [NATIVE] [PROGRAM Variable] [SOURCES ...] [LIBS ...])
#
# Builds tests/test_<Name>.cpp with the listed protocol sources and registers it as webrtc_<Name>.  FIXTURES defines
# WEBRTC_FIXTURE_DIR.  NATIVE links Core and runs the test from the install folder; PROGRAM receives its path.

function (webrtc_unit_test Name)
   cmake_parse_arguments (PARSE_ARGV 1 ARG "FIXTURES;NATIVE" "PROGRAM" "SOURCES;LIBS")
   set (TARGET_NAME test_webrtc_${Name})
   add_executable (${TARGET_NAME} tests/test_${Name}.cpp ${ARG_SOURCES})
   set_target_properties (${TARGET_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   target_include_directories (${TARGET_NAME} PRIVATE "${OPENSSL_INCLUDE_DIR}")
   if (ARG_NATIVE)
      target_link_libraries (${TARGET_NAME} PRIVATE ${INIT_LINK} ${ARG_LIBS})
   else ()
      target_link_libraries (${TARGET_NAME} PRIVATE ${ARG_LIBS})
   endif ()
   if (ARG_FIXTURES)
      target_compile_definitions (${TARGET_NAME} PRIVATE
         WEBRTC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
   endif ()
   if (ARG_NATIVE)
      add_dependencies (${TARGET_NAME} network)
      webrtc_native_program (${TARGET_NAME} PROGRAM_PATH)
      add_test (NAME webrtc_${Name} COMMAND "${PROGRAM_PATH}" WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
      if (ARG_PROGRAM)
         set (${ARG_PROGRAM} "${PROGRAM_PATH}" PARENT_SCOPE)
      endif ()
   else ()
      add_test (NAME webrtc_${Name} COMMAND ${TARGET_NAME})
   endif ()
   set_tests_properties (webrtc_${Name} PROPERTIES LABELS webrtc TIMEOUT 60)
endfunction ()

if (UNIT_TESTS)
   set (DTLS_LIBS ${KOTUKU_OPENSSL_LIBS} ${CMAKE_DL_LIBS} Threads::Threads)

   # STUN, TURN and PRECIS

   webrtc_unit_test (opaque_string LIBS webrtc_precis)
   webrtc_unit_test (stun SOURCES ${WEBRTC_STUN_SOURCES} LIBS ${KOTUKU_OPENSSL_LIBS})
   webrtc_unit_test (stun_long_term SOURCES ${WEBRTC_STUN_SOURCES} LIBS ${KOTUKU_OPENSSL_LIBS})
   webrtc_unit_test (turn FIXTURES SOURCES turn/turn.cpp ${WEBRTC_STUN_SOURCES} LIBS ${KOTUKU_OPENSSL_LIBS})

   # ICE and DTLS

   webrtc_unit_test (ice SOURCES ice/ice.cpp ${WEBRTC_STUN_SOURCES} LIBS ${KOTUKU_OPENSSL_LIBS})
   webrtc_unit_test (ice_agent SOURCES ${WEBRTC_ICE_SOURCES} LIBS ${KOTUKU_OPENSSL_LIBS})
   webrtc_unit_test (ice_candidates SOURCES ice/ice_candidates.cpp ${WEBRTC_STUN_SOURCES}
      LIBS ${KOTUKU_OPENSSL_LIBS} network_mdns)
   webrtc_unit_test (ice_servers SOURCES ice/ice_servers.cpp ice/ice_candidates.cpp ${WEBRTC_STUN_SOURCES}
      LIBS ${KOTUKU_OPENSSL_LIBS} network_mdns)
   webrtc_unit_test (dtls SOURCES dtls/dtls_openssl.cpp LIBS ${DTLS_LIBS})
   webrtc_unit_test (ice_dtls SOURCES ${WEBRTC_ICE_DTLS_SOURCES} LIBS ${DTLS_LIBS})
   webrtc_unit_test (full_ice_dtls SOURCES ${WEBRTC_ICE_DTLS_SOURCES} LIBS ${DTLS_LIBS})
   webrtc_unit_test (ice_socket NATIVE SOURCES ${WEBRTC_TRANSPORT_SOURCES}
      LIBS ${KOTUKU_OPENSSL_LIBS} network_mdns Threads::Threads)
   webrtc_unit_test (full_ice_socket NATIVE SOURCES ${WEBRTC_TRANSPORT_SOURCES}
      LIBS ${KOTUKU_OPENSSL_LIBS} network_mdns Threads::Threads)

   webrtc_unit_test (interface_monitor LIBS webrtc_interfaces)
   set_tests_properties (webrtc_interface_monitor PROPERTIES TIMEOUT 30)

   # SDP and SCTP

   webrtc_unit_test (sdp FIXTURES SOURCES sdp/sdp.cpp)
   webrtc_unit_test (sctp_packet FIXTURES SOURCES sctp/sctp_packet.cpp)
   foreach (SCTP_TEST association transfer streams)
      webrtc_unit_test (sctp_${SCTP_TEST} FIXTURES SOURCES ${WEBRTC_SCTP_SOURCES} LIBS crypto_protocol unicode)
   endforeach ()
   webrtc_unit_test (sctp_dtls SOURCES ${WEBRTC_SCTP_SOURCES} dtls/dtls_openssl.cpp LIBS ${DTLS_LIBS})

   # PeerConnection

   webrtc_unit_test (peer_connection NATIVE PROGRAM PEER_CONNECTION_PROGRAM SOURCES ${WEBRTC_PEER_SOURCES}
      LIBS ${KOTUKU_OPENSSL_LIBS} network_mdns webrtc_interfaces Threads::Threads)

   # Public API programs load the installed module rather than compiling the protocol sources.

   add_executable (test_webrtc_public_turn tests/test_public_turn.cpp)
   set_target_properties (test_webrtc_public_turn PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   target_link_libraries (test_webrtc_public_turn PRIVATE ${INIT_LINK})
   add_dependencies (test_webrtc_public_turn ${MOD})
   webrtc_native_program (test_webrtc_public_turn PUBLIC_TURN_PROGRAM)

   add_executable (test_webrtc_public_webchannel tests/test_public_webchannel.cpp)
   set_target_properties (test_webrtc_public_webchannel PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   target_link_libraries (test_webrtc_public_webchannel PRIVATE ${INIT_LINK})
   add_dependencies (test_webrtc_public_webchannel ${MOD})
   webrtc_native_program (test_webrtc_public_webchannel PUBLIC_WEBCHANNEL_PROGRAM)
   add_test (NAME webrtc_public_webchannel COMMAND "${PUBLIC_WEBCHANNEL_PROGRAM}" --log-warning
      WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
   set_tests_properties (webrtc_public_webchannel PROPERTIES LABELS webrtc TIMEOUT 200)
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Linux LD_PRELOAD fixtures that simulate multicast loss and interface changes beneath Flute tests.

if (UNIT_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
   # GCC's shared ASan runtime must precede an instrumented interposer in LD_PRELOAD.

   set (WEBRTC_PRELOAD_PREFIX "")
   if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND (ENABLE_ANALYSIS OR CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*address"))
      execute_process (COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libasan.so
         OUTPUT_VARIABLE WEBRTC_ASAN_RUNTIME OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
      if (NOT EXISTS "${WEBRTC_ASAN_RUNTIME}")
         message (FATAL_ERROR "Instrumented WebRTC interposers require the GCC ASan runtime.")
      endif ()
      set (WEBRTC_PRELOAD_PREFIX "${WEBRTC_ASAN_RUNTIME}:")
   endif ()

   add_library (webrtc_mdns_unavailable SHARED ../network/mdns/unavailable_fixture.cpp)
   target_link_libraries (webrtc_mdns_unavailable PRIVATE ${CMAKE_DL_LIBS})
   flute_test (webrtc_mdns_unavailable "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mdns_unavailable.tiri")
   if (BUILD_TESTS)
      set_tests_properties (webrtc_mdns_unavailable PROPERTIES
         ENVIRONMENT "LD_PRELOAD=${WEBRTC_PRELOAD_PREFIX}$<TARGET_FILE:webrtc_mdns_unavailable>")
   endif ()

   add_library (webrtc_interface_change SHARED tests/interface_change_fixture.cpp)
   target_link_libraries (webrtc_interface_change PRIVATE ${CMAKE_DL_LIBS})
   set_target_properties (webrtc_interface_change PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   foreach (RECOVERY_CASE recover free fail)
      if (RECOVERY_CASE STREQUAL "recover")
         set (RECOVERY_FILTER DeferredRecover)
      elseif (RECOVERY_CASE STREQUAL "free")
         set (RECOVERY_FILTER DeferredFree)
      else ()
         set (RECOVERY_FILTER DeferredFail)
      endif ()
      add_test (NAME webrtc_network_change_${RECOVERY_CASE} COMMAND "${WEBRTC_ORIGO}" "${FLUTE_SCRIPT}"
         "file=${CMAKE_CURRENT_SOURCE_DIR}/tests/test_peer_network_change.tiri" "test=${RECOVERY_FILTER}"
         --log-warning --gfx-driver=headless WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
      set_tests_properties (webrtc_network_change_${RECOVERY_CASE} PROPERTIES
         ENVIRONMENT "LD_PRELOAD=${WEBRTC_PRELOAD_PREFIX}$<TARGET_FILE:webrtc_interface_change>"
         LABELS webrtc TIMEOUT 60)
   endforeach ()

   # Consumed by the TURN integration fixture in tests/interop/interop.cmake.
   add_library (webrtc_direct_loss SHARED tests/direct_loss_fixture.cpp)
   target_link_libraries (webrtc_direct_loss PRIVATE ${CMAKE_DL_LIBS})
   set_target_properties (webrtc_direct_loss PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif ()

# --------------------------------------------------------------------------------------------------------------------
# libFuzzer targets compile their dependencies directly so that sanitiser coverage includes every parser.

if (UNIT_TESTS AND WEBRTC_FUZZ)
   if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
      message (FATAL_ERROR "WEBRTC_FUZZ requires Clang C and C++ compilers with libFuzzer support.")
   endif ()
   if (NOT ORIGO_CMD OR NOT EXISTS "${ORIGO_CMD}")
      message (FATAL_ERROR "WEBRTC_FUZZ requires installed Origo; configure ORIGO_CMD_PATH to its directory.")
   endif ()

   set (FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
   set (FUZZ_PRECIS_SOURCES precis/opaque_string.cpp ../link/unicode.cpp ../crypto/protocol_primitives.cpp
      "${utf8proc_SOURCE_DIR}/utf8proc.c")

   function (webrtc_fuzz_target Name)
      add_executable (fuzz_webrtc_${Name} ${ARGN})
      set_target_properties (fuzz_webrtc_${Name} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
      target_include_directories (fuzz_webrtc_${Name} PRIVATE "${OPENSSL_INCLUDE_DIR}" "${utf8proc_SOURCE_DIR}")
      target_compile_definitions (fuzz_webrtc_${Name} PRIVATE CRYPTO_PROTOCOL_OPENSSL=1 UTF8PROC_STATIC)
      target_link_libraries (fuzz_webrtc_${Name} PRIVATE ${WEBRTC_OPENSSL_LIBS})
      target_compile_options (fuzz_webrtc_${Name} PRIVATE ${FUZZ_FLAGS})
      target_link_options (fuzz_webrtc_${Name} PRIVATE ${FUZZ_FLAGS})
   endfunction ()

   # Materialise duplicate codec seeds only in the build tree.
   set_property (DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      tests/fixtures/sctp_wire.json tests/fixtures/dcep_wire.json
      tests/fixtures/turn_allocate_401.turn tests/fixtures/turn_allocate_success.turn
      tests/interop/prepare_fuzz_corpus.tiri)
   file (GLOB TURN_CODEC_SEEDS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fuzz_turn_codec/*")
   set_property (DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TURN_CODEC_SEEDS})
   foreach (PROTOCOL sctp turn)
      set (${PROTOCOL}_codec_CORPUS "${CMAKE_CURRENT_BINARY_DIR}/fuzz_${PROTOCOL}_codec")
      set (${PROTOCOL}_state_CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fuzz_${PROTOCOL}_state")
      execute_process (COMMAND "${ORIGO_CMD}"
         "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/prepare_fuzz_corpus.tiri"
         "name=${PROTOCOL}_codec" "destination=${${PROTOCOL}_codec_CORPUS}" --log-warning
         COMMAND_ERROR_IS_FATAL ANY)
   endforeach ()

   foreach (FUZZ_KIND codec state)
      webrtc_fuzz_target (turn_${FUZZ_KIND} tests/fuzz_turn_${FUZZ_KIND}.cpp turn/turn.cpp ${WEBRTC_STUN_SOURCES}
         ${FUZZ_PRECIS_SOURCES})
      webrtc_fuzz_target (sctp_${FUZZ_KIND} tests/fuzz_sctp_${FUZZ_KIND}.cpp ${WEBRTC_SCTP_SOURCES}
         ../link/unicode.cpp ../crypto/protocol_primitives.cpp)
      foreach (PROTOCOL sctp turn)
         add_test (NAME webrtc_${PROTOCOL}_${FUZZ_KIND}_fuzz_regression COMMAND fuzz_webrtc_${PROTOCOL}_${FUZZ_KIND}
            -runs=1 "${${PROTOCOL}_${FUZZ_KIND}_CORPUS}")
         set_tests_properties (webrtc_${PROTOCOL}_${FUZZ_KIND}_fuzz_regression PROPERTIES LABELS webrtc TIMEOUT 60)
      endforeach ()
   endforeach ()

   webrtc_fuzz_target (sdp tests/fuzz_sdp.cpp sdp/sdp.cpp)
   webrtc_fuzz_target (stun tests/fuzz_stun.cpp ${WEBRTC_STUN_SOURCES} ${FUZZ_PRECIS_SOURCES})
   add_test (NAME webrtc_stun_fuzz_regression COMMAND fuzz_webrtc_stun -runs=1
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fuzz_stun/utf8proc_null_buffer.stun")
   set_tests_properties (webrtc_stun_fuzz_regression PROPERTIES LABELS webrtc TIMEOUT 60)
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Optional external fixtures: browser interop, coturn, Linux namespaces and Windows acceptance.

include ("${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/interop.cmake")

# --------------------------------------------------------------------------------------------------------------------
# Windows development/CI gate.  Keep public behaviour and protocol regressions in the routine run;
# native interop probes and elevated topology fixtures remain available for targeted acceptance.

if (WIN32)
   set (WEBRTC_WINDOWS_ESSENTIALS
      webrtc_turn_configuration webrtc_webchannel_loopback webrtc_webchannel_lifecycle
      webrtc_sctp_packet webrtc_sctp_association webrtc_sctp_transfer webrtc_sctp_streams webrtc_sctp_dtls
      webrtc_opaque_string webrtc_turn webrtc_stun webrtc_stun_long_term webrtc_sdp
      webrtc_ice webrtc_ice_agent webrtc_ice_candidates webrtc_dtls
      webrtc_peer_connection webrtc_public_webchannel webrtc_interface_monitor
      webrtc_peer_restart webrtc_peer_stats webrtc_mdns_privacy
      webrtc_public_channels_interop webrtc_mdns_channels_interop webrtc_turn_integration)
   get_property (WEBRTC_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
   foreach (WEBRTC_TEST IN LISTS WEBRTC_REGISTERED_TESTS)
      if (NOT WEBRTC_TEST MATCHES "^webrtc_")
         continue ()
      endif ()
      if (WEBRTC_TEST IN_LIST WEBRTC_WINDOWS_ESSENTIALS)
         set_property (TEST ${WEBRTC_TEST} APPEND PROPERTY LABELS webrtc_windows_essential)
      else ()
         set_property (TEST ${WEBRTC_TEST} APPEND PROPERTY LABELS webrtc_windows_extended)
      endif ()
   endforeach ()
endif ()
