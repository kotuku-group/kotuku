# Origo executable

# Use the release version from pkg/version.txt, shared by Origo and Tiri compatibility checks.
set(KOTUKU_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(KOTUKU_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(KOTUKU_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
string(TIMESTAMP KOTUKU_COPYRIGHT_YEAR "%Y")

message(STATUS "Kotuku version: ${KOTUKU_VERSION_MAJOR}.${KOTUKU_VERSION_MINOR}.${KOTUKU_VERSION_PATCH}")

# Generate version.h with git commit/branch information.
#
# The git query runs at BUILD time (via generate_version.cmake driven by a custom
# target below) rather than at configure time, so that `origo --version` always
# reports the commit the binary was actually built against.  We invoke the same
# script once here to guarantee version.h exists before the first compile.

set(KOTUKU_VERSION_H "${CMAKE_CURRENT_BINARY_DIR}/version.h")

set(KOTUKU_VERSION_ARGS
   "-DSRC_DIR=${PROJECT_SOURCE_DIR}"
   "-DVERSION_IN=${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
   "-DVERSION_OUT=${KOTUKU_VERSION_H}"
   "-DKOTUKU_VERSION_MAJOR=${KOTUKU_VERSION_MAJOR}"
   "-DKOTUKU_VERSION_MINOR=${KOTUKU_VERSION_MINOR}"
   "-DKOTUKU_VERSION_PATCH=${KOTUKU_VERSION_PATCH}"
   "-DKOTUKU_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
)

execute_process(
   COMMAND ${CMAKE_COMMAND} ${KOTUKU_VERSION_ARGS}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/generate_version.cmake"
)

# Refresh version.h on every build so the git commit is never stale.  The script
# only rewrites the file when its content changes, so this does not force a
# needless recompile when the git state is unchanged.
add_custom_target(origo_version ALL
   COMMAND ${CMAKE_COMMAND} ${KOTUKU_VERSION_ARGS}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/generate_version.cmake"
   BYPRODUCTS "${KOTUKU_VERSION_H}"
   COMMENT "Refreshing version.h with current git commit"
   VERBATIM
)

# Generate versioned manifest and resource files
set (KOTUKU_ICON_PATH "${PROJECT_SOURCE_DIR}/src/link/kotuku.ico")
set (TIRI_ICON_PATH "${PROJECT_SOURCE_DIR}/src/link/tiri.ico")
set (KOTUKU_MANIFEST_PATH "${CMAKE_CURRENT_BINARY_DIR}/kotuku.manifest")
set (KOTUKU_RESOURCE_PATH "${CMAKE_CURRENT_BINARY_DIR}/resource.rc")

configure_file(
    "${PROJECT_SOURCE_DIR}/src/link/kotuku.manifest.in"
    "${KOTUKU_MANIFEST_PATH}"
    @ONLY
)

configure_file(
    "${PROJECT_SOURCE_DIR}/src/link/resource.rc.in"
    "${KOTUKU_RESOURCE_PATH}"
    @ONLY
)

if (RUN_ANYWHERE)
   set (CMAKE_INSTALL_RPATH "$ORIGIN/lib/lib")
else ()
   set (CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib/kotuku")
endif ()

if (WIN32)
   if (ORIGO_CONSOLE)
      add_executable (origo_cmd "origo.cpp" "sandbox-win.cpp" "${KOTUKU_RESOURCE_PATH}")
   else ()
      # WIN32 keyword sets the Windows subsystem to GUI (no console window)
      add_executable (origo_cmd WIN32 "origo.cpp" "sandbox-win.cpp" "${KOTUKU_RESOURCE_PATH}")
   endif ()

   # Disable automatic manifest generation since we provide our own
   if (MSVC)
      if (ORIGO_CONSOLE)
         set_target_properties(origo_cmd PROPERTIES LINK_FLAGS "/MANIFEST:NO /SUBSYSTEM:CONSOLE")
      else ()
         set_target_properties(origo_cmd PROPERTIES LINK_FLAGS "/MANIFEST:NO")
      endif ()
   endif()
else ()
   add_executable (origo_cmd "origo.cpp")
endif ()

target_link_libraries (origo_cmd PRIVATE ${INIT_LINK})

target_include_directories (origo_cmd PRIVATE "${PROJECT_SOURCE_DIR}/src/link" "${CMAKE_CURRENT_BINARY_DIR}")

if (KOTUKU_MULTI_CONFIG)
   message (STATUS "Symbols will be stripped from Release builds only.")
elseif (CMAKE_BUILD_TYPE MATCHES "Release")
   message (STATUS "Symbols will be stripped from this build (no debugging).")
else ()
   message (STATUS "Symbols will be left intact in this build (debug enabled).")
endif ()

set_target_properties (origo_cmd PROPERTIES CXX_STANDARD 20 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release")
if (NOT MSVC)
   target_link_options (origo_cmd PRIVATE $<$<CONFIG:Release>:-s>)
endif ()

set_target_properties (origo_cmd PROPERTIES OUTPUT_NAME "origo")

# Ensure the icon package is available before the Origo binary is built

add_dependencies (origo_cmd icon_package)

# Ensure version.h is refreshed with the current git commit before origo compiles
add_dependencies (origo_cmd origo_version)

if (KOTUKU_STATIC)
   add_dependencies (origo_cmd kotuku_static)
else ()
   add_dependencies (origo_cmd ${REQUIRED_MODS})
endif ()

if (KOTUKU_INSTALL)
   if (RUN_ANYWHERE)
      set (ORIGO_INSTALL_DESTINATION .)
      set (TIRI_SYMLINK_DIR "${CMAKE_INSTALL_PREFIX}")
   else ()
      set (ORIGO_INSTALL_DESTINATION "bin")
      set (TIRI_SYMLINK_DIR "${CMAKE_INSTALL_PREFIX}/bin")
   endif ()

   if (WIN32 AND NOT MSVC)
      cmake_host_system_information (RESULT MSYSTEM_PREFIX QUERY MSYSTEM_PREFIX)
      set (ORIGO_RUNTIME_SEARCH_ARGUMENTS)
      if (MSYSTEM_PREFIX)
         list (APPEND ORIGO_RUNTIME_SEARCH_ARGUMENTS DIRECTORIES "${MSYSTEM_PREFIX}/bin")
      endif ()

      install (TARGETS origo_cmd
         RUNTIME_DEPENDENCY_SET origo_runtime_dependencies
         RUNTIME DESTINATION "${ORIGO_INSTALL_DESTINATION}")
      install (RUNTIME_DEPENDENCY_SET origo_runtime_dependencies
         ${ORIGO_RUNTIME_SEARCH_ARGUMENTS}
         PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
         POST_EXCLUDE_REGEXES ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
         RUNTIME DESTINATION "${ORIGO_INSTALL_DESTINATION}")
   else ()
      install (TARGETS origo_cmd RUNTIME DESTINATION "${ORIGO_INSTALL_DESTINATION}")
   endif ()

   if (NOT WIN32)
      install (CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink origo \"${TIRI_SYMLINK_DIR}/tiri\")")
   endif ()
endif ()
