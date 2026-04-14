# Origo executable

# Generate version information based on current date (YYYY.MM.DD format)
string(TIMESTAMP KOTUKU_VERSION_MAJOR "%Y")
string(TIMESTAMP KOTUKU_VERSION_MINOR "%m")
string(TIMESTAMP KOTUKU_VERSION_PATCH "%d")
string(TIMESTAMP KOTUKU_COPYRIGHT_YEAR "%Y")

# Remove leading zeros from month and day for proper version format
string(REGEX REPLACE "^0+" "" KOTUKU_VERSION_MINOR "${KOTUKU_VERSION_MINOR}")
string(REGEX REPLACE "^0+" "" KOTUKU_VERSION_PATCH "${KOTUKU_VERSION_PATCH}")

# Ensure we don't get empty strings for single digit values
if(KOTUKU_VERSION_MINOR STREQUAL "")
    set(KOTUKU_VERSION_MINOR "0")
endif()
if(KOTUKU_VERSION_PATCH STREQUAL "")
    set(KOTUKU_VERSION_PATCH "0")
endif()

message(STATUS "Kotuku version: ${KOTUKU_VERSION_MAJOR}.${KOTUKU_VERSION_MINOR}.${KOTUKU_VERSION_PATCH}")

# Extract git information if available
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
   execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      OUTPUT_VARIABLE GIT_COMMIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
   )
   execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      OUTPUT_VARIABLE GIT_BRANCH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
   )
else()
   set(GIT_COMMIT_HASH "unknown")
   set(GIT_BRANCH "unknown")
endif()

message(STATUS "Git branch: ${GIT_BRANCH}, commit: ${GIT_COMMIT_HASH}")

# Generate version header with git information
# This uses configure_file which only updates the output if content changes,
# preventing unnecessary rebuilds when git info hasn't changed
configure_file(
   "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
   "${CMAKE_CURRENT_BINARY_DIR}/version.h"
   @ONLY
)

# Generate versioned manifest and resource files
configure_file(
    "${PROJECT_SOURCE_DIR}/src/link/kotuku.manifest.in"
    "${PROJECT_SOURCE_DIR}/src/link/kotuku.manifest"
    @ONLY
)

configure_file(
    "${PROJECT_SOURCE_DIR}/src/link/resource.rc.in"
    "${PROJECT_SOURCE_DIR}/src/link/resource.rc"
    @ONLY
)

if (RUN_ANYWHERE)
   set (CMAKE_INSTALL_RPATH "$ORIGIN/lib/lib")
else ()
   set (CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib/kotuku")
endif ()

if (WIN32)
   # Specifying the WIN32 option will generate a Windows GUI application
   add_executable (origo_cmd WIN32 "origo.cpp" "sandbox-win.cpp" "${PROJECT_SOURCE_DIR}/src/link/resource.rc")

   # Disable automatic manifest generation since we provide our own
   if (MSVC)
      set_target_properties(origo_cmd PROPERTIES LINK_FLAGS "/MANIFEST:NO")
   endif()
else ()
   add_executable (origo_cmd "origo.cpp")
endif ()

target_link_libraries (origo_cmd PRIVATE ${INIT_LINK})

target_include_directories (origo_cmd PRIVATE "${PROJECT_SOURCE_DIR}/src/link" "${CMAKE_CURRENT_BINARY_DIR}")

if (CMAKE_BUILD_TYPE MATCHES "Release")
   message (STATUS "Symbols will be stripped from this build (no debugging).")
   if (NOT MSVC)
      set_target_properties (origo_cmd PROPERTIES CXX_STANDARD 20 LINK_FLAGS_RELEASE "-s" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release")
   else ()
      set_target_properties (origo_cmd PROPERTIES CXX_STANDARD 20 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release")
   endif ()
else ()
   message (STATUS "Symbols will be left intact in this build (debug enabled).")
   set_target_properties (origo_cmd PROPERTIES CXX_STANDARD 20 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release")
endif ()

set_target_properties (origo_cmd PROPERTIES OUTPUT_NAME "origo")

# Attach headers to the Origo binary so that we can ensure that all headers are built

add_dependencies (origo_cmd build_headers icon_package)

if (KOTUKU_STATIC)
   add_dependencies (origo_cmd kotuku_static)
else ()
   add_dependencies (origo_cmd ${REQUIRED_MODS})
endif ()

if (KOTUKU_INSTALL)
   if (WIN32 AND NOT MSVC)
      # MSYS2/MinGW builds have library dependencies that we need to take a copy of.
      find_file(MSYS_DEP2 "libstdc++-6.dll" HINTS "/mingw64/bin")
      if (NOT MSYS_DEP2 STREQUAL "MSYS_DEP2-NOTFOUND")
         install (FILES ${MSYS_DEP2} DESTINATION .)
      endif ()

      find_file(MSYS_DEP4 "libgcc_s_seh-1.dll" HINTS "/mingw64/bin")
      if (NOT MSYS_DEP4 STREQUAL "MSYS_DEP4-NOTFOUND")
         install (FILES ${MSYS_DEP4} DESTINATION .)
      endif ()

      find_file(MSYS_DEP5 "libwinpthread-1.dll" HINTS "/mingw64/bin")
      if (NOT MSYS_DEP5 STREQUAL "MSYS_DEP5-NOTFOUND")
         install (FILES ${MSYS_DEP5} DESTINATION .)
      endif ()
   endif ()

   if (RUN_ANYWHERE)
      install (TARGETS origo_cmd DESTINATION .)
      set (TIRI_SYMLINK_DIR "${CMAKE_INSTALL_PREFIX}")
   else ()
      install (TARGETS origo_cmd DESTINATION "bin")
      set (TIRI_SYMLINK_DIR "${CMAKE_INSTALL_PREFIX}/bin")
   endif ()

   if (NOT WIN32)
      install (CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink origo \"${TIRI_SYMLINK_DIR}/tiri\")")
   endif ()
endif ()
