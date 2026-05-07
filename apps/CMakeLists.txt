# Application script tests.

if (NOT DISABLE_HTTP AND NOT DISABLE_NETWORK)
   flute_test (apps_tuku "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tuku.tiri")
endif ()
