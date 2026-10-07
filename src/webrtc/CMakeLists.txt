# WebRTC
#
# Phase 0 of docs/plans/webrtc_native_support.md.  The webrtc module itself arrives in Phase 1; for now this directory
# builds the feasibility spikes:
#
#   test_webrtc_dtls   DTLS-SRTP transport over OpenSSL with a datagram BIO (unit test, label `webrtc`)
#   bench_webrtc_udp   NetSocket UDP receive throughput at WebRTC packet sizes (benchmark, label `webrtc_bench`)
#   tests/interop/     Chromium interop harness driven by Playwright (label `webrtc_interop`)
#
# Linux only.  Windows feasibility is assessed separately.

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
   message (STATUS "WebRTC spikes disabled: only Linux is supported in Phase 0.")
   return ()
endif ()

if (DISABLE_SSL OR NOT KOTUKU_OPENSSL_LIBS)
   message (STATUS "WebRTC spikes disabled: OpenSSL is required for DTLS.")
   return ()
endif ()

find_package (Threads REQUIRED)

if (UNIT_TESTS)
   add_executable (test_webrtc_dtls tests/test_dtls.cpp dtls_openssl.cpp)
   set_target_properties (test_webrtc_dtls PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   target_include_directories (test_webrtc_dtls PRIVATE "${OPENSSL_INCLUDE_DIR}")
   target_link_libraries (test_webrtc_dtls PRIVATE ${KOTUKU_OPENSSL_LIBS} ${CMAKE_DL_LIBS} Threads::Threads)
   add_test (NAME webrtc_dtls COMMAND test_webrtc_dtls)
   set_tests_properties (webrtc_dtls PROPERTIES LABELS webrtc TIMEOUT 60)
endif ()

# The benchmark boots the Core, so ctest runs it from the install folder.  It is not a pass/fail gate for normal CI
# runs and carries its own label: ctest -L webrtc_bench

if (BUILD_TESTS AND TARGET network)
   add_executable (bench_webrtc_udp tests/bench_udp.cpp)
   set_target_properties (bench_webrtc_udp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   target_link_libraries (bench_webrtc_udp PRIVATE ${INIT_LINK} Threads::Threads)
   add_dependencies (bench_webrtc_udp network)
   add_test (NAME webrtc_udp_bench COMMAND bench_webrtc_udp WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
   set_tests_properties (webrtc_udp_bench PROPERTIES LABELS webrtc_bench TIMEOUT 120 RUN_SERIAL TRUE)
endif ()

# Browser interop harness.  Registered only when Node.js and the playwright package are available, because Chromium is
# not part of the standard build dependencies.  The package is located through `npm root -g` unless
# WEBRTC_INTEROP_NODE_PATH names another node_modules folder.  Run with: ctest -L webrtc_interop

if (BUILD_TESTS)
   find_program (NODE_EXECUTABLE node)
   find_program (NPM_EXECUTABLE npm)
   set (WEBRTC_INTEROP_NODE_PATH "" CACHE PATH "node_modules folder containing playwright for the WebRTC interop test")

   if (NODE_EXECUTABLE AND NOT WEBRTC_INTEROP_NODE_PATH AND NPM_EXECUTABLE)
      execute_process (COMMAND "${NPM_EXECUTABLE}" root -g OUTPUT_VARIABLE NPM_GLOBAL_ROOT
         OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE NPM_ROOT_RESULT)
      if (NPM_ROOT_RESULT EQUAL 0)
         set (WEBRTC_INTEROP_NODE_PATH "${NPM_GLOBAL_ROOT}" CACHE PATH "" FORCE)
      endif ()
   endif ()

   if (NODE_EXECUTABLE AND WEBRTC_INTEROP_NODE_PATH)
      execute_process (
         COMMAND "${CMAKE_COMMAND}" -E env "NODE_PATH=${WEBRTC_INTEROP_NODE_PATH}"
            "${NODE_EXECUTABLE}" -e "require.resolve('playwright')"
         RESULT_VARIABLE PLAYWRIGHT_RESULT OUTPUT_QUIET ERROR_QUIET)
   endif ()

   if (NODE_EXECUTABLE AND WEBRTC_INTEROP_NODE_PATH AND (PLAYWRIGHT_RESULT EQUAL 0))
      if (RUN_ANYWHERE)
         set (WEBRTC_ORIGO "${CMAKE_INSTALL_PREFIX}/origo")
      else ()
         set (WEBRTC_ORIGO "${CMAKE_INSTALL_PREFIX}/bin/origo")
      endif ()

      add_test (NAME webrtc_interop
         COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_interop.cjs"
            --origo "${WEBRTC_ORIGO}"
            --script "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/signalling_server.tiri"
            --port 9310
            --out "${CMAKE_CURRENT_BINARY_DIR}/interop")
      set_tests_properties (webrtc_interop PROPERTIES
         LABELS webrtc_interop
         TIMEOUT 120
         RUN_SERIAL TRUE
         ENVIRONMENT "NODE_PATH=${WEBRTC_INTEROP_NODE_PATH}")
      message (STATUS "WebRTC browser interop test enabled (playwright found in ${WEBRTC_INTEROP_NODE_PATH}).")
   else ()
      message (STATUS "WebRTC browser interop test disabled: Node.js with the playwright package was not found.")
   endif ()
endif ()
