# WebRTC external test fixtures, included by src/webrtc/CMakeLists.txt.
#
# These tests need tools that are not standard build dependencies (Node.js with Playwright, Chromium, coturn, Linux
# namespaces or Windows elevation).  Each group is registered only when its prerequisites are available or its
# option is enabled.  The scripts in this folder are documented in readme.md.

option (WEBRTC_REQUIRE_INTEROP "Require all WebRTC browser interop tests at configure time" OFF)
option (WEBRTC_REQUIRE_TURN "Require the production TURN integration fixture" OFF)
option (WEBRTC_TEST_RELAY_NAMESPACE "Enable isolated blocked-direct relay fixture" OFF)
option (WEBRTC_TEST_INTERFACE_NAMESPACE "Run real Linux notifications in a disposable user/network namespace" OFF)
option (WEBRTC_TEST_DNS_NAMESPACE "Run bounded Linux DNS teardown in private namespaces" OFF)
option (WEBRTC_TEST_WINDOWS_NOTIFICATIONS "Run elevated Windows loopback address-change acceptance" OFF)

set (WEBRTC_TURN_SERVER "" CACHE FILEPATH "Pinned coturn turnserver executable")
set (WEBRTC_TURN_UCLIENT "" CACHE FILEPATH "Pinned coturn turnutils_uclient executable")

if (WIN32)
   set (WEBRTC_TURN_RELAY_MIN 40000 CACHE STRING "First TURN fixture relay port")
   set (WEBRTC_TURN_RELAY_MAX 40019 CACHE STRING "Last TURN fixture relay port")
   option (WEBRTC_WINDOWS_TRUST_FIXTURE_CA "Temporarily trust the TURN fixture CA for Schannel tests" OFF)
   set (WEBRTC_WINDOWS_TRUST_SCOPE user CACHE STRING "TURN fixture root store: user or elevated machine")
   set_property (CACHE WEBRTC_WINDOWS_TRUST_SCOPE PROPERTY STRINGS user machine)
else ()
   set (WEBRTC_TURN_RELAY_MIN 49160 CACHE STRING "First TURN fixture relay port")
   set (WEBRTC_TURN_RELAY_MAX 49179 CACHE STRING "Last TURN fixture relay port")
endif ()

if (WEBRTC_REQUIRE_INTEROP AND NOT BUILD_TESTS)
   message (FATAL_ERROR "WEBRTC_REQUIRE_INTEROP requires BUILD_TESTS.")
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Browser interop covers complete SCTP/data-channel exchanges; handshake-only bring-up probes are redundant.
# Registered only when Node.js and the playwright package are available; Chromium is not a standard build dependency.
# The package is located through `npm root -g` unless WEBRTC_INTEROP_NODE_PATH names another node_modules folder.
# Run with: ctest -L webrtc_interop

if (BUILD_TESTS AND TARGET network)
   add_executable (webrtc_native_sctp_probe tests/native_sctp_probe.cpp ${WEBRTC_TRANSPORT_SOURCES} sdp/sdp.cpp
      ${WEBRTC_SCTP_SOURCES})
   set_target_properties (webrtc_native_sctp_probe PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
   target_link_libraries (webrtc_native_sctp_probe PRIVATE ${INIT_LINK} ${KOTUKU_OPENSSL_LIBS} Threads::Threads
      network_mdns)
   add_dependencies (webrtc_native_sctp_probe network)
   webrtc_native_program (webrtc_native_sctp_probe SCTP_PROBE_PROGRAM)
endif ()

set (WEBRTC_BROWSER_AVAILABLE FALSE)
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
      find_program (WEBRTC_CHROME_EXECUTABLE NAMES google-chrome chromium chromium-browser)
      if (WEBRTC_CHROME_EXECUTABLE AND TARGET webrtc_native_sctp_probe)
         foreach (DTLS_ROLE client server)
            add_test (NAME webrtc_native_sctp_${DTLS_ROLE}
               COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/native_sctp.cjs"
                  "${SCTP_PROBE_PROGRAM}" "${WEBRTC_CHROME_EXECUTABLE}" ${DTLS_ROLE}
                  "${CMAKE_CURRENT_BINARY_DIR}/interop/sctp_${DTLS_ROLE}.json"
               WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
            set_tests_properties (webrtc_native_sctp_${DTLS_ROLE} PROPERTIES
               LABELS webrtc_interop TIMEOUT 30 RUN_SERIAL TRUE
               ENVIRONMENT "NODE_PATH=${WEBRTC_INTEROP_NODE_PATH}")
         endforeach ()
      endif ()

      add_test (NAME webrtc_public_channels_interop
         COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/public_channels.cjs"
            "${WEBRTC_ORIGO}" "${PROJECT_SOURCE_DIR}/examples/webrtc/signalling.tiri"
            "${WEBRTC_CHROME_EXECUTABLE}"
            "${CMAKE_CURRENT_BINARY_DIR}/interop/public_channels_interop.json"
         WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
      set_tests_properties (webrtc_public_channels_interop PROPERTIES
         LABELS webrtc_interop TIMEOUT 180 RUN_SERIAL TRUE
         ENVIRONMENT "NODE_PATH=${WEBRTC_INTEROP_NODE_PATH}")

      if (WEBRTC_CHROME_EXECUTABLE)
         set (WEBRTC_BROWSER_AVAILABLE TRUE)
      endif ()

      message (STATUS "WebRTC browser interop test enabled (playwright found in ${WEBRTC_INTEROP_NODE_PATH}).")
   else ()
      message (STATUS "WebRTC browser interop test disabled: Node.js with the playwright package was not found.")
   endif ()
endif ()

# mDNS host candidates against Chromium.

if (WEBRTC_BROWSER_AVAILABLE)
   add_test (NAME webrtc_mdns_interop COMMAND "${NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/public_mdns.cjs"
      "${WEBRTC_ORIGO}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/mdns_signalling.tiri"
      "${WEBRTC_CHROME_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/interop/mdns.json")
   set_tests_properties (webrtc_mdns_interop PROPERTIES LABELS webrtc_mdns_interop
      TIMEOUT 120 RUN_SERIAL TRUE ENVIRONMENT "NODE_PATH=${WEBRTC_INTEROP_NODE_PATH}")

   add_test (NAME webrtc_mdns_channels_interop COMMAND "${NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/public_channels.cjs"
      "${WEBRTC_ORIGO}"
      "${PROJECT_SOURCE_DIR}/examples/webrtc/signalling.tiri"
      "${WEBRTC_CHROME_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/interop/mdns_channels.json" MDNS)
   set_tests_properties (webrtc_mdns_channels_interop PROPERTIES LABELS webrtc_mdns_interop
      TIMEOUT 120 RUN_SERIAL TRUE ENVIRONMENT "NODE_PATH=${WEBRTC_INTEROP_NODE_PATH}")
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Explicit local coturn fixture; CI can require these prerequisites rather than accepting a skipped relay gate.

if (BUILD_TESTS AND TARGET test_webrtc_public_turn AND WEBRTC_TURN_SERVER AND WEBRTC_TURN_UCLIENT)
   find_package (Python3 COMPONENTS Interpreter REQUIRED)
   set (TURN_FIXTURE_ARGS
      --turnserver "${WEBRTC_TURN_SERVER}" --uclient "${WEBRTC_TURN_UCLIENT}"
      --origo "${WEBRTC_ORIGO}" --public-turn "${PUBLIC_TURN_PROGRAM}"
      --turn-adapter "${PEER_CONNECTION_PROGRAM}"
      --relay-min "${WEBRTC_TURN_RELAY_MIN}" --relay-max "${WEBRTC_TURN_RELAY_MAX}"
      --evidence-dir "${CMAKE_CURRENT_BINARY_DIR}/interop/turn")
   find_program (OPENSSL_EXECUTABLE NAMES openssl REQUIRED)
   list (APPEND TURN_FIXTURE_ARGS --openssl "${OPENSSL_EXECUTABLE}")
   if (WIN32 AND WEBRTC_WINDOWS_TRUST_FIXTURE_CA)
      list (APPEND TURN_FIXTURE_ARGS --windows-trust-ca --windows-trust-scope "${WEBRTC_WINDOWS_TRUST_SCOPE}")
   endif ()
   if (WEBRTC_CHROME_EXECUTABLE AND NODE_EXECUTABLE AND (PLAYWRIGHT_RESULT EQUAL 0))
      list (APPEND TURN_FIXTURE_ARGS --browser-turn "${WEBRTC_CHROME_EXECUTABLE}")
   endif ()
   add_test (NAME webrtc_turn_integration COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_turn_fixture.py" ${TURN_FIXTURE_ARGS})
   set_tests_properties (webrtc_turn_integration PROPERTIES LABELS webrtc_turn_integration
      TIMEOUT 480 RUN_SERIAL TRUE ENVIRONMENT "NODE_PATH=${WEBRTC_INTEROP_NODE_PATH}")

   # Direct-path loss and TLS shutdown stress use the LD_PRELOAD interposer built by the module CMakeLists.
   if (TARGET webrtc_direct_loss)
      set_property (TEST webrtc_turn_integration APPEND PROPERTY ENVIRONMENT
         "WEBRTC_PEER_TRANSITION=${PEER_CONNECTION_PROGRAM}"
         "WEBRTC_DIRECT_LOSS=${WEBRTC_PRELOAD_PREFIX}$<TARGET_FILE:webrtc_direct_loss>")
      add_test (NAME webrtc_tls_shutdown_stress COMMAND "${Python3_EXECUTABLE}"
         "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_linux_tls_shutdown.py"
         --origo "${WEBRTC_ORIGO}" --openssl "${OPENSSL_EXECUTABLE}"
         --evidence "${CMAKE_CURRENT_BINARY_DIR}/interop/tls_shutdown")
      set_tests_properties (webrtc_tls_shutdown_stress PROPERTIES LABELS webrtc TIMEOUT 180 RUN_SERIAL TRUE)
   endif ()
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Opt-in kernel fixtures require disposable Linux user/network namespaces and nftables.

if (WEBRTC_TEST_RELAY_NAMESPACE)
   if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT TEST webrtc_turn_integration OR
       NOT WEBRTC_CHROME_EXECUTABLE OR NOT NODE_EXECUTABLE OR
       NOT (PLAYWRIGHT_RESULT EQUAL 0))
      message (FATAL_ERROR "Relay namespace fixtures require Linux, coturn, Chromium, Node.js and Playwright.")
   endif ()
   find_program (WEBRTC_NFT_EXECUTABLE NAMES nft PATHS /usr/sbin /sbin REQUIRED)
   find_program (WEBRTC_IP_EXECUTABLE NAMES ip REQUIRED)
   find_program (WEBRTC_UNSHARE_EXECUTABLE NAMES unshare REQUIRED)

   add_test (NAME webrtc_firewall_relay COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_isolated_relay.py"
      --nft "${WEBRTC_NFT_EXECUTABLE}"
      --ip "${WEBRTC_IP_EXECUTABLE}" --unshare "${WEBRTC_UNSHARE_EXECUTABLE}"
      --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/run_turn_fixture.py"
      --turnserver "${WEBRTC_TURN_SERVER}" --uclient "${WEBRTC_TURN_UCLIENT}"
      --origo "${WEBRTC_ORIGO}" --public-turn "${PUBLIC_TURN_PROGRAM}"
      --chrome "${WEBRTC_CHROME_EXECUTABLE}"
      --evidence "${CMAKE_CURRENT_BINARY_DIR}/interop/firewall")
   set_tests_properties (webrtc_firewall_relay PROPERTIES LABELS webrtc_topology
      TIMEOUT 1150 RUN_SERIAL TRUE ENVIRONMENT "NODE_PATH=${WEBRTC_INTEROP_NODE_PATH}")
endif ()

if (WEBRTC_TEST_DNS_NAMESPACE)
   if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT TARGET test_webrtc_public_turn)
      message (FATAL_ERROR "DNS namespace fixture requires Linux and UNIT_TESTS.")
   endif ()
   find_package (Python3 COMPONENTS Interpreter REQUIRED)
   add_test (NAME webrtc_dns_teardown COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_linux_dns_close.py" --probe "${PUBLIC_TURN_PROGRAM}")
   set_tests_properties (webrtc_dns_teardown PROPERTIES LABELS webrtc TIMEOUT 45
      WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
endif ()

if (WEBRTC_TEST_INTERFACE_NAMESPACE)
   if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT TARGET test_webrtc_interface_monitor)
      message (FATAL_ERROR "Interface namespace fixture requires Linux and UNIT_TESTS.")
   endif ()
   find_package (Python3 COMPONENTS Interpreter REQUIRED)
   add_test (NAME webrtc_interface_notifications COMMAND "${WEBRTC_ORIGO}"
      "${FLUTE_SCRIPT}" "file=${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_interface_notifications.tiri"
      "monitor=$<TARGET_FILE:test_webrtc_interface_monitor>" --log-warning --gfx-driver=headless)
   set_tests_properties (webrtc_interface_notifications PROPERTIES LABELS webrtc TIMEOUT 30)
   add_test (NAME webrtc_ipv6_path_removal COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_linux_path_removal.py" --probe "${PUBLIC_TURN_PROGRAM}"
      --evidence "${CMAKE_CURRENT_BINARY_DIR}/interop/ipv6_path_removal")
   set_tests_properties (webrtc_ipv6_path_removal PROPERTIES LABELS webrtc_topology TIMEOUT 95
      WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Elevated Windows acceptance for address changes, path removal and DNS teardown.

if (WEBRTC_TEST_WINDOWS_NOTIFICATIONS)
   if (NOT WIN32 OR NOT TARGET test_webrtc_interface_monitor)
      message (FATAL_ERROR "Windows notification acceptance requires Windows and UNIT_TESTS.")
   endif ()
   find_package (Python3 COMPONENTS Interpreter REQUIRED)
   add_test (NAME webrtc_windows_notifications COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_windows_notifications.py"
      --monitor "$<TARGET_FILE:test_webrtc_interface_monitor>"
      --origo "${WEBRTC_ORIGO}")
   set_tests_properties (webrtc_windows_notifications PROPERTIES LABELS webrtc TIMEOUT 180 RUN_SERIAL TRUE)
   add_test (NAME webrtc_windows_path_removal COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_windows_path_removal.py"
      --probe "${PUBLIC_TURN_PROGRAM}"
      --evidence "${CMAKE_CURRENT_BINARY_DIR}/interop/windows_path_removal")
   set_tests_properties (webrtc_windows_path_removal PROPERTIES LABELS webrtc TIMEOUT 120 RUN_SERIAL TRUE)
   add_test (NAME webrtc_windows_dns_close COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_windows_dns_close.py"
      --probe "${PUBLIC_TURN_PROGRAM}")
   set_tests_properties (webrtc_windows_dns_close PROPERTIES LABELS webrtc TIMEOUT 90 RUN_SERIAL TRUE)
   add_test (NAME webrtc_windows_dns_capacity COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_windows_dns_close.py"
      --probe "${PUBLIC_TURN_PROGRAM}" --capacity)
   set_tests_properties (webrtc_windows_dns_capacity PROPERTIES LABELS webrtc TIMEOUT 90 RUN_SERIAL TRUE)
   add_test (NAME webrtc_windows_dns_free COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_windows_dns_close.py"
      --probe "${PUBLIC_TURN_PROGRAM}" --destroy)
   set_tests_properties (webrtc_windows_dns_free PROPERTIES LABELS webrtc TIMEOUT 90 RUN_SERIAL TRUE)
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Configure-time requirements

if (WEBRTC_REQUIRE_INTEROP)
   foreach (TEST_NAME webrtc_native_sctp_client webrtc_native_sctp_server webrtc_public_channels_interop
      webrtc_mdns_interop)
      if (NOT TEST ${TEST_NAME})
         message (FATAL_ERROR "Required test ${TEST_NAME} is unavailable; configure Node.js, Playwright "
            "and WEBRTC_CHROME_EXECUTABLE.")
      endif ()
   endforeach ()
   if (NOT EXISTS "${WEBRTC_CHROME_EXECUTABLE}")
      message (FATAL_ERROR "WEBRTC_CHROME_EXECUTABLE must name an installed browser.")
   endif ()
endif ()

if (WEBRTC_REQUIRE_TURN)
   if (NOT TEST webrtc_turn_integration OR NOT EXISTS "${WEBRTC_TURN_SERVER}" OR
       NOT EXISTS "${WEBRTC_TURN_UCLIENT}")
      message (FATAL_ERROR "Required TURN fixture needs BUILD_TESTS, UNIT_TESTS and pinned coturn executables.")
   endif ()
   if (NOT WEBRTC_CHROME_EXECUTABLE OR NOT NODE_EXECUTABLE OR NOT (PLAYWRIGHT_RESULT EQUAL 0))
      message (FATAL_ERROR "Required TURN fixture needs Chromium, Node.js and Playwright.")
   endif ()
endif ()
