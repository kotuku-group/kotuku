# Application script tests.

if (NOT DISABLE_HTTP AND NOT DISABLE_NETWORK)
   flute_test (apps_proximity "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_proximity.tiri")
   flute_test (apps_tuku "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tuku.tiri")
   flute_test (apps_tuku_server "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tuku_server.tiri")
endif ()
