
set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED True)
set (CMAKE_POSITION_INDEPENDENT_CODE ON) # Enables -fPIC and -fPIE in gcc

if (WIN32)
   add_library(init-win STATIC init-win.cpp)
   target_include_directories (init-win PRIVATE "${PROJECT_SOURCE_DIR}/include")
else ()
   add_library(init-unix STATIC init-unix.cpp)
   target_include_directories (init-unix PRIVATE "${PROJECT_SOURCE_DIR}/include")
   target_link_libraries (init-unix PRIVATE dl)
endif ()
