# Kotuku Core Build File
#
# Release Config:  cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
# Debug Config:    cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=local -DRUN_ANYWHERE=TRUE
# Fast Config:     cmake -S . -B build/fast -DCMAKE_BUILD_TYPE=FastBuild -DCMAKE_INSTALL_PREFIX=local -DRUN_ANYWHERE=TRUE
#                  (FastBuild optimises for compilation speed rather than runtime performance - uses -O0, faster linkers, minimal debug info)
# Git Release:     cmake -S . -B build/release-git -DCMAKE_BUILD_TYPE=Release -DINSTALL_EXAMPLES=TRUE -DCMAKE_INSTALL_PREFIX=kotuku-linux64-$(date +'%Y%m%d') -DRUN_ANYWHERE=TRUE -DBUILD_TESTS=OFF
#
# A build can target a local folder by adding -DCMAKE_INSTALL_PREFIX=local -DRUN_ANYWHERE=TRUE
#
# Build:   cmake --build build/release --parallel -- -O
# Install: sudo cmake --install build/release
# Test:    ctest --testdir build/release -C Release --output-on-failure
#
# A static build can be enabled with -DKOTUKU_STATIC=ON if the intention is to embed Kotuku into another project.
# This may be useful if you intend to use a limited feature-set, like the vector graphics API.  Static builds
# should be customised with an isolated output folder that is exclusive to the target project.  Sharing a static
# build across multiple projects can otherwise lead to confusing outcomes.
#
# Windows release packages can be created using CPack integration:
#
#   cmake -S . -B release -DCMAKE_BUILD_TYPE=Release -DKOTUKU_STATIC=ON -DINSTALL_EXAMPLES=ON -DENABLE_CPACK=ON
#   cmake --build release --config Release
#   cmake --install release
#   cd release && cpack -G ZIP
#   cmake --build . --target package_sfx  # Creates Self-Extracting EXE (requires 7-Zip)
#
# Requirements:
#   - 7-Zip installed for SFX generation (optional, auto-detected)
#   - NSIS for installer generation (optional, auto-detected)

cmake_minimum_required(VERSION 3.15)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   message (STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release")
   set (CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif ()

add_compile_definitions($<$<CONFIG:Release>:KOTUKU_RELEASE_BUILD>)

include (CheckIncludeFiles)
include (CheckTypeSize)
include (CheckFunctionExists)
include (CheckSymbolExists)
include (CheckCSourceCompiles)
include (CheckIncludeFileCXX)
include (CheckStructHasMember)
include (CheckLibraryExists)
include (TestBigEndian)
include (ExternalProject)

if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
   include(CTest)
endif()

file(READ "${CMAKE_SOURCE_DIR}/pkg/version.txt" KOTUKU_VERSION_STRING)
string(STRIP "${KOTUKU_VERSION_STRING}" KOTUKU_VERSION_STRING)

project (Kotuku VERSION ${KOTUKU_VERSION_STRING} LANGUAGES C CXX)

cmake_policy (SET CMP0082 NEW) # Process install commands in the order that they appear
if(POLICY CMP0177)
   cmake_policy(SET CMP0177 NEW) # Normalise install destination paths (converts slashes etc)
endif()

# To switch on a custom option use -D, e.g. -DKOTUKU_VLOG=ON

option (KOTUKU_STATIC            "Build as a set of static libraries instead of the default modular build." OFF)
option (KOTUKU_INSTALL           "Create installation targets" ON)
option (KOTUKU_VLOG              "Enable verbose log messages in the generated binaries" OFF)
option (KOTUKU_ICON_DEPENDENCIES "Track icon source files as dependencies for rebuilding the icon package." ON)
option (ORIGO_CONSOLE            "Build Origo as a Windows console application rather than a GUI application (Windows only)" OFF)
if (UNIX)
   option (RUN_ANYWHERE  "Create a build that can run directly from the main folder.  Typically used if redefining CMAKE_INSTALL_PREFIX" OFF)
else ()
   set (RUN_ANYWHERE ON)
endif ()
option (BUILD_TESTS       "Enable Flute tests" ON)
option (BUILD_DEFS        "Build generated C/C++ headers and documentation as part of the default build" OFF)
option (DISABLE_SSL       "Disable built-in SSL support even if available on this system" OFF)
option (DISABLE_X11       "Disable X11 even if available on this system" OFF)
option (DISABLE_AUDIO     "Disable Audio API" OFF)
option (DISABLE_BACKSTAGE "Disable Backstage API" OFF)
option (DISABLE_DISPLAY   "Disable Display API" OFF)
option (DISABLE_DOCUMENT  "Disable Document API" OFF)
option (DISABLE_FONT      "Disable Font API" OFF)
option (DISABLE_HTTP      "Disable HTTP API" OFF)
option (DISABLE_MP3       "Disable MP3 support" OFF)
option (DISABLE_NETWORK   "Disable Network API" OFF)
option (DISABLE_IMAGE     "Disable Image API" OFF)
option (DISABLE_JPEG      "Disable JPEG support" OFF)
option (DISABLE_SCINTILLA "Disable Scintilla API" ON)
option (DISABLE_SVG       "Disable SVG support" OFF)
option (DISABLE_VECTOR    "Disable Vector API" OFF)
option (DISABLE_XSLT      "Disable XSLT API" OFF)
option (INSTALL_EXAMPLES  "Install the example scripts, typically for compiling a distribution" OFF)
option (INSTALL_INCLUDES  "Install the header files.  Turn this off for client releases." OFF)
option (INSTALL_TESTS     "Install the test programs." OFF)
option (ENABLE_ANALYSIS   "Enable run-time program analysis (with performance penalties) if available.  May negatively affect gdb." OFF)
option (GENERATE_DEBIAN_PKG "Generate Debian packaging files" OFF)
option (ENABLE_CPACK      "Enable CPack Windows packaging support" OFF)
option (FULL_OPTIMISATION "Use the highest levels of optimisation available" OFF)
option (ENABLE_SSL_BUNDLE_UPDATE "Enable the manual update_ssl_bundle target for downloading the SSL certificate bundle." OFF)

set (KOTUKU_NETWORK_SUPPORTED FALSE)
if (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
   set (KOTUKU_NETWORK_SUPPORTED TRUE)
endif ()

if (NOT DISABLE_NETWORK AND NOT KOTUKU_NETWORK_SUPPORTED)
   message (STATUS "Network API disabled: unsupported platform '${CMAKE_SYSTEM_NAME}'.")
   set (DISABLE_NETWORK ON CACHE BOOL "Disable Network API" FORCE)
endif ()

get_property (KOTUKU_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if (KOTUKU_MULTI_CONFIG AND CMAKE_BUILD_TYPE STREQUAL "FastBuild")
   list (FIND CMAKE_CONFIGURATION_TYPES FastBuild KOTUKU_FASTBUILD_INDEX)
   if (KOTUKU_FASTBUILD_INDEX EQUAL -1)
      list (APPEND CMAKE_CONFIGURATION_TYPES FastBuild)
      set (CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations" FORCE)
   endif ()
endif ()

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED True)
set (CMAKE_POSITION_INDEPENDENT_CODE ON) # Enables -fPIC and -fPIE in gcc
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

set (BUILD_RPATH_USE_ORIGIN TRUE)

set (INCLUDE_OUTPUT "${PROJECT_SOURCE_DIR}/include/kotuku")
set (DOCS "${PROJECT_SOURCE_DIR}/docs/xml")

# Ensure the top-level include directory is visible to all builds and tools that
# run during the build (some generated-header steps and tools may compile
# helper sources outside target-specific include directories). This helps
# avoid C1083 errors when headers under include/kotuku are referenced.
include_directories("${PROJECT_SOURCE_DIR}/include")

set(REQUIRED_MODS core tiri xml xquery regex json)

if (KOTUKU_STATIC)
   set (BUILD_SHARED_LIBS OFF)
else ()
   set (BUILD_SHARED_LIBS ON)
endif ()

if (RUN_ANYWHERE)
   set (KOTUKU_ROOT "${CMAKE_INSTALL_PREFIX}")
   set (SHARE_TARGET .)
   set (CONFIG_TARGET "config")
   set (LIB_TARGET "lib")
   set (ORIGO_CMD_PATH "${CMAKE_INSTALL_PREFIX}")
else ()
   set (KOTUKU_ROOT "${CMAKE_INSTALL_PREFIX}/share/kotuku")
   set (SHARE_TARGET "share/kotuku")
   set (CONFIG_TARGET "${SHARE_TARGET}/config")
   set (LIB_TARGET "lib/kotuku")
   set (ORIGO_CMD_PATH "${CMAKE_INSTALL_PREFIX}/bin")
endif ()

set (ORIGO_CMD_TARGET "${ORIGO_CMD_PATH}/origo")

test_big_endian (SYSTEM_ENDIAN_STYLE)
if (SYSTEM_ENDIAN_STYLE EQUAL 0) # Little endian (Intel)
   set (REVERSE_BYTEORDER TRUE)
   set (LITTLE_ENDIAN TRUE)
endif ()

if (NOT DISABLE_SSL)
   if (WIN32)
      # Win32 SSL support is supported natively
   else ()
      set (OPENSSL_USE_STATIC_LIBS TRUE)
      include (FindOpenSSL)
      if (NOT OpenSSL_FOUND)
         set (DISABLE_SSL TRUE)
      else ()
         set (CMAKE_REQUIRED_INCLUDES "${OPENSSL_INCLUDE_DIR}")
         check_include_file_cxx ("openssl/ssl.h" HAVE_OPENSSL_SSL_H)
         unset (CMAKE_REQUIRED_INCLUDES)
         if (NOT HAVE_OPENSSL_SSL_H)
            message (STATUS "OpenSSL libraries were found but openssl/ssl.h is missing. SSL support will be disabled.")
            set (DISABLE_SSL TRUE)
         endif ()
      endif ()
   endif ()
endif ()

if (NOT DISABLE_X11)
   include (FindX11)
endif ()

if (MINGW) # Undo the use of 'lib' as the default prefix set by MinGW
   set(CMAKE_SHARED_LIBRARY_PREFIX "")
   set(CMAKE_STATIC_LIBRARY_PREFIX "")
endif ()

# MSVC-specific optimization flags for Release builds
if (MSVC AND FULL_OPTIMISATION)
   # Replace default /O2 with /Ox for maximum optimization
   # /GL enables whole program optimization at compile time
   # /LTCG enables link-time code generation for cross-module optimization
   string(REPLACE "/O2" "/Ox /GL" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
   string(REPLACE "/O2" "/Ox /GL" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")

   if (KOTUKU_STATIC)
      set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
      set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
   endif ()
endif ()

include_directories("${PROJECT_SOURCE_DIR}/include"
   "${PROJECT_SOURCE_DIR}/include/kotuku/3rdparty"
   "${CMAKE_CURRENT_BINARY_DIR}") # For config.h

	CHECK_CXX_SOURCE_COMPILES ("
#include <stdio.h>
int main() {
   printf(\"%lld\", 0x1122334455667788);
   return 0;
}"
	PRINTF64_LLD
   FAIL_REGEX "warning")

	CHECK_CXX_SOURCE_COMPILES ("
#include <stdio.h>
int main() {
   printf(\"%I64lld\", 0x1122334455667788);
   return 0;
}"
	PRINTF64_ID
   FAIL_REGEX "warning")

	CHECK_CXX_SOURCE_COMPILES ("
#include <stdio.h>
#include <inttypes.h>
int main() {
   printf(\"%\" PRId64, 0x1122334455667788);
   return 0;
}"
	PRINTF64_PRID
   FAIL_REGEX "warning")

find_program (XXD_CMD "xxd")
find_program (OD_CMD "od")
find_program (ORIGO_CMD "origo" NO_DEFAULT_PATH PATHS "${ORIGO_CMD_PATH}")
find_file (IDL_C_SCRIPT "idl-c.tiri" PATHS "${PROJECT_SOURCE_DIR}/tools/idl")
find_file (IDL_COMPILE_SCRIPT "idl-compile.tiri" PATHS "${PROJECT_SOURCE_DIR}/tools/idl")
find_file (FLUTE_SCRIPT "flute.tiri" PATHS "${PROJECT_SOURCE_DIR}/tools")

set (KOTUKU_DEFS_AVAILABLE TRUE)

if (ORIGO_CMD STREQUAL "ORIGO_CMD-NOTFOUND")
   message (STATUS "Definition generation targets are unavailable because no installed Origo executable was found.")
   set (KOTUKU_DEFS_AVAILABLE FALSE)
   set (BUILD_DEFS OFF)
else ()
   if (BUILD_DEFS)
      message (STATUS "Definitions will be built with the Origo exe installed @ ${ORIGO_CMD}.")
   else ()
      message (STATUS "Definition generation is available with: cmake --build <build-dir> --target build_headers")
   endif ()
endif ()

if (BUILD_TESTS)
   message (STATUS "Integration tests are enabled.")
   enable_testing()
endif ()

if (IDL_C_SCRIPT STREQUAL "IDL_C_SCRIPT-NOTFOUND")
   message (FATAL_ERROR "The idl-c.tiri script was not found in ${PROJECT_SOURCE_DIR}/tools/idl")
endif ()

if (IDL_COMPILE_SCRIPT STREQUAL "IDL_COMPILE_SCRIPT-NOTFOUND")
   message (FATAL_ERROR "The idl-compile.tiri script was not found in ${PROJECT_SOURCE_DIR}/tools/idl")
endif ()

if (FLUTE_SCRIPT STREQUAL "FLUTE_SCRIPT-NOTFOUND")
   message (FATAL_ERROR "The flute.tiri script was not found in ${PROJECT_SOURCE_DIR}/tools")
endif ()

if (NOT MSVC)
   set (MATH_LINK m)
endif ()

if (WIN32)
   if (KOTUKU_STATIC)
      # NB: MinGW does not guarantee that libgcc and libstdc++ will be static even if these flags are specified.
      if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
         set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
      endif ()
      set (INIT_LINK "init-win" kotuku_static)
   else ()
      if (MINGW)
         set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
      endif ()
      set (INIT_LINK "init-win")
   endif ()
else ()
   # Note that compiling with -static in Linux is disabled because it appears to cause segfaults when writing __thread variables.
   if (KOTUKU_STATIC)
      set (INIT_LINK "init-unix" kotuku_static)
   else ()
      set (INIT_LINK "init-unix")
   endif ()
endif ()

if (BUILD_DEFS)
   add_custom_target (build_headers ALL COMMENT "Generate headers and definition files on the fly")
elseif (KOTUKU_DEFS_AVAILABLE)
   add_custom_target (build_headers COMMENT "Generate headers and definition files on demand")
else ()
   add_custom_target (build_headers
      COMMAND ${CMAKE_COMMAND} -E echo "Cannot generate definitions: install or build Origo, then reconfigure this build tree."
      COMMAND ${CMAKE_COMMAND} -E false
      COMMENT "Definition generation is unavailable")
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Register generated header targets with the aggregate target while also making them discoverable for
# module-local dependency wiring in set_module_defaults().

function (register_generated_header_target ARG_TARGET)
   add_dependencies (build_headers ${ARG_TARGET})
   set_property (DIRECTORY APPEND PROPERTY KOTUKU_GENERATED_HEADER_TARGETS ${ARG_TARGET})
endfunction ()

# --------------------------------------------------------------------------------------------------------------------
# Add generated header dependencies immediately when possible, otherwise queue them until all subdirectories have
# declared their targets.  This keeps normal builds parallel while avoiding missing generated headers in BUILD_DEFS
# configurations.

function (add_generated_header_dependencies ARG_TARGET)
   if (BUILD_DEFS)
      foreach (ARG_DEPENDENCY IN LISTS ARGN)
         if (TARGET ${ARG_TARGET} AND TARGET ${ARG_DEPENDENCY})
            add_dependencies (${ARG_TARGET} ${ARG_DEPENDENCY})
         else ()
            set_property (GLOBAL APPEND PROPERTY KOTUKU_PENDING_GENERATED_HEADER_DEPENDENCIES
               "${ARG_TARGET}|${ARG_DEPENDENCY}")
         endif ()
      endforeach ()
   endif ()
endfunction ()

function (resolve_generated_header_dependencies)
   if (BUILD_DEFS)
      get_property (KOTUKU_PENDING_HEADER_TARGETS GLOBAL PROPERTY KOTUKU_PENDING_GENERATED_HEADER_DEPENDENCIES)
      foreach (ARG_DEPENDENCY_PAIR IN LISTS KOTUKU_PENDING_HEADER_TARGETS)
         string (REPLACE "|" ";" ARG_DEPENDENCY_PARTS "${ARG_DEPENDENCY_PAIR}")
         list (GET ARG_DEPENDENCY_PARTS 0 ARG_TARGET)
         list (GET ARG_DEPENDENCY_PARTS 1 ARG_DEPENDENCY)
         if (TARGET ${ARG_TARGET} AND TARGET ${ARG_DEPENDENCY})
            add_dependencies (${ARG_TARGET} ${ARG_DEPENDENCY})
         endif ()
      endforeach ()
   endif ()
endfunction ()

# --------------------------------------------------------------------------------------------------------------------
# Use Flute to write tests against your code.  All Flute tests are integration tests, i.e. run them post-install
# against the target installation folder.
#
# The --no-crash-handler option is used because crash dump analysis is better without it.

function (flute_test name path)
   if (BUILD_TESTS)
      if (RUN_ANYWHERE)
         set (FLUTE_EXE "${CMAKE_INSTALL_PREFIX}/origo")
      else ()
         set (FLUTE_EXE "${CMAKE_INSTALL_PREFIX}/bin/origo")
      endif ()
      get_filename_component(FLUTE_FULL_PATH "${path}" ABSOLUTE)
      add_test (NAME ${name}
         COMMAND "${FLUTE_EXE}" "--no-crash-handler" "--log-warning" "--gfx-driver=headless" "${FLUTE_SCRIPT}" "file=${FLUTE_FULL_PATH}"
         WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
      set_tests_properties(${name} PROPERTIES LABELS ${name})
   endif ()
endfunction ()

# --------------------------------------------------------------------------------------------------------------------
# idl_gen() is used to generate both C/C++ headers and IDL strings from TDL files.

function (idl_gen)
   cmake_parse_arguments (PARSE_ARGV 0 IDLC "" "OUTPUT;APPEND_IDL;NAME" "ARGS;FILES;DEPENDS")

   if (KOTUKU_DEFS_AVAILABLE)
      if (DEFINED IDLC_NAME)
         set (OUTPUT_NAME ${IDLC_NAME})
      else ()
         string (REGEX REPLACE "[/:\.]" "_" OUTPUT_NAME ${IDLC_OUTPUT})
      endif ()

      if (DEFINED IDLC_FILES)
         list (APPEND IDLC_ARGS "files={" ${IDLC_FILES} "}")
      endif ()

      set (IDLC_STAMP "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${OUTPUT_NAME}.stamp")
      set (IDLC_BYPRODUCTS "${IDLC_OUTPUT}")

      foreach (ARG IN LISTS IDLC_ARGS)
         if (ARG MATCHES "^output-(defs|proto)=(.+)$")
            if (IS_ABSOLUTE "${CMAKE_MATCH_2}")
               list (APPEND IDLC_BYPRODUCTS "${CMAKE_MATCH_2}")
            else ()
               get_filename_component (IDLC_ARG_OUTPUT "${CMAKE_MATCH_2}" ABSOLUTE
                  BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
               list (APPEND IDLC_BYPRODUCTS "${IDLC_ARG_OUTPUT}")
            endif ()
         endif ()
      endforeach ()

      list (APPEND COMMAND_LIST COMMAND ${CMAKE_COMMAND} -E echo
         "[idl_gen] ${ORIGO_CMD} ${IDL_C_SCRIPT} --log-warning src=${ARGV0} output=${IDLC_OUTPUT} sdk=${PROJECT_SOURCE_DIR} ${IDLC_ARGS}")
      list (APPEND COMMAND_LIST COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=${ARGV0}"
         "output=${IDLC_OUTPUT}" "sdk=${PROJECT_SOURCE_DIR}" ${IDLC_ARGS})

      if (DEFINED IDLC_APPEND_IDL)
         if (IS_ABSOLUTE "${IDLC_APPEND_IDL}")
            list (APPEND IDLC_BYPRODUCTS "${IDLC_APPEND_IDL}")
         else ()
            get_filename_component (IDLC_APPEND_IDL_OUTPUT "${IDLC_APPEND_IDL}" ABSOLUTE
               BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            list (APPEND IDLC_BYPRODUCTS "${IDLC_APPEND_IDL_OUTPUT}")
         endif ()

         list (APPEND COMMAND_LIST COMMAND ${CMAKE_COMMAND} -E echo
            "[idl_gen] ${ORIGO_CMD} ${IDL_COMPILE_SCRIPT} --log-warning src=${ARGV0} output=${IDLC_APPEND_IDL} sdk=${PROJECT_SOURCE_DIR} format=c append")
         list (APPEND COMMAND_LIST COMMAND ${ORIGO_CMD} ${IDL_COMPILE_SCRIPT} "--log-warning" "src=${ARGV0}"
            "output=${IDLC_APPEND_IDL}" "sdk=${PROJECT_SOURCE_DIR}" "format=c" "append")
      endif ()

      list (REMOVE_DUPLICATES IDLC_BYPRODUCTS)

      add_custom_command (OUTPUT "${IDLC_STAMP}"
         BYPRODUCTS ${IDLC_BYPRODUCTS}
         ${COMMAND_LIST}
         COMMAND ${CMAKE_COMMAND} -E touch "${IDLC_STAMP}"
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         DEPENDS ${ARGV0} ${IDLC_DEPENDS}
         COMMENT "Generating C header file ${IDLC_OUTPUT} from ${ARGV0}"
         VERBATIM)

      add_custom_target (${OUTPUT_NAME} DEPENDS "${IDLC_STAMP}")

      register_generated_header_target (${OUTPUT_NAME})
   else ()
      message (STATUS "The generation of ${IDLC_OUTPUT} will be skipped.")
   endif ()
endfunction ()

# --------------------------------------------------------------------------------------------------------------------
# idl_compile() is used to generate universal IDL strings
# Always provide a full path for OUTPUT

function (idl_compile)
   cmake_parse_arguments (PARSE_ARGV 0 IDLC "" "OUTPUT;NAME" "ARGS")

   if (KOTUKU_DEFS_AVAILABLE)
      if (DEFINED IDLC_NAME)
         set (OUTPUT_NAME ${IDLC_NAME})
      else ()
         string (REGEX REPLACE "[/:\.]" "_" OUTPUT_NAME ${IDLC_OUTPUT})
      endif ()

      set (IDLC_STAMP "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${OUTPUT_NAME}.stamp")

      add_custom_command (OUTPUT "${IDLC_STAMP}"
         BYPRODUCTS ${IDLC_OUTPUT}
         COMMAND ${CMAKE_COMMAND} -E echo
            "[idl_compile] ${ORIGO_CMD} ${IDL_COMPILE_SCRIPT} --log-warning src=${ARGV0} output=${IDLC_OUTPUT} sdk=${PROJECT_SOURCE_DIR} format=c ${IDLC_ARGS}"
         COMMAND "${ORIGO_CMD}" "${IDL_COMPILE_SCRIPT}" "--log-warning" "src=${ARGV0}" "output=${IDLC_OUTPUT}" "sdk=${PROJECT_SOURCE_DIR}" "format=c" ${IDLC_ARGS}
         COMMAND ${CMAKE_COMMAND} -E touch "${IDLC_STAMP}"
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         DEPENDS ${ARGV0}
         COMMENT "Producing definition file ${IDLC_OUTPUT} from ${ARGV0}"
         VERBATIM)

      add_custom_target (${OUTPUT_NAME} DEPENDS "${IDLC_STAMP}")

      register_generated_header_target (${OUTPUT_NAME})
   else ()
      message (STATUS "The generation of ${IDLC_OUTPUT} will be skipped.")
   endif ()
endfunction ()

# --------------------------------------------------------------------------------------------------------------------
# This is a convenience function that configures the default settings needed for Kotuku modules

function (set_module_defaults ARG_TARGET ARG_NAMESPACE)
   target_include_directories (${ARG_TARGET} PRIVATE "${PROJECT_SOURCE_DIR}/include")

   add_compile_definitions ("MOD_NAME=${ARG_TARGET}" "MOD_NAMESPACE=${ARG_NAMESPACE}")

   if (MSVC)

   else ()
      # Collect code and data that is never referenced
      target_link_options (${ARG_TARGET} PRIVATE "$<${GCC_COMPAT}:$<LINK_ONLY:LINKER:--gc-sections>>")
   endif ()

   if (KOTUKU_STATIC)
      set_target_properties (${ARG_TARGET}
         PROPERTIES
         CXX_STANDARD 20)
   else ()
      if (NOT MSVC)
         target_link_options (${ARG_TARGET} PRIVATE $<$<CONFIG:Release>:-s>)
      endif ()

      set_target_properties (${ARG_TARGET}
         PROPERTIES
         PREFIX ""
         CXX_STANDARD 20
         CMAKE_POSITION_INDEPENDENT_CODE ON # Enables -fPIC and -fPIE in gcc
         LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib"
         RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib")

      if (KOTUKU_INSTALL)
         if (WIN32)
            install(TARGETS ${ARG_TARGET} RUNTIME DESTINATION "${LIB_TARGET}")
         else ()
            install(TARGETS ${ARG_TARGET}
               RUNTIME DESTINATION "${LIB_TARGET}"
               ARCHIVE DESTINATION "${LIB_TARGET}"
               LIBRARY DESTINATION "${LIB_TARGET}")
         endif ()
      endif ()
   endif ()

   get_property (KOTUKU_LOCAL_HEADER_TARGETS DIRECTORY PROPERTY KOTUKU_GENERATED_HEADER_TARGETS)
   if (BUILD_DEFS AND KOTUKU_LOCAL_HEADER_TARGETS)
      add_dependencies (${ARG_TARGET} ${KOTUKU_LOCAL_HEADER_TARGETS})
   endif ()

   # Ensure all modules have a minimum level of dependencies, this ensures a minimally
   # functioning environment is created when building individual targets with --target

   # Create a list of required modules excluding the current target to avoid circular dependencies
   if (NOT (ARG_TARGET STREQUAL core))
      if (ARG_TARGET IN_LIST REQUIRED_MODS)
         add_dependencies (${ARG_TARGET} core)
      else ()
         add_dependencies (${ARG_TARGET} ${REQUIRED_MODS})
      endif ()
   endif ()
endfunction ()

# --------------------------------------------------------------------------------------------------------------------
# 64-bit checks

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
   message (STATUS "This will be a 64-bit build for processor arch ${CMAKE_HOST_SYSTEM_PROCESSOR}")
   set (_LP64 ON) # Used by config.h
#   set (KOTUKU_ARCH "-m64")
else ()
   message (STATUS "This will be a 32-bit build for processor arch ${CMAKE_HOST_SYSTEM_PROCESSOR}")
#   set (KOTUKU_ARCH "-m32")
endif ()

if (KOTUKU_VLOG)
   add_compile_definitions ("_DEBUG")
endif ()

# Set system-specific compiler flags

if (MSVC)
   # Turn off ugly warnings
   add_compile_definitions ("_CRT_SECURE_NO_WARNINGS")

   # Use NODEFAULTLIB to override sub-projects linking to incorrect standard libraries.
   # https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-6.0/aa267384(v=vs.60)?redirectedfrom=MSDN

   if (KOTUKU_VLOG)
      set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
      add_link_options ("/NODEFAULTLIB:libc.lib" "/NODEFAULTLIB:libcmt.lib" "/NODEFAULTLIB:libc.lib" "/NODEFAULTLIB:libcmtd.lib" "/NODEFAULTLIB:msvcrt.lib")
   else ()
      set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
      add_link_options ("/NODEFAULTLIB:libcmt.lib" "/NODEFAULTLIB:libcmtd.lib" "/NODEFAULTLIB:msvcrtd.lib")
   endif ()

   # add_link_options ("/VERBOSE:LIB")
   if (ENABLE_ANALYSIS)
      # Note that this option is not compatible with /ZI (edit-and-continue).  To rectify complaints about the asan DLL, you may also need a line like this one added to each entry in launch.vs.json:
      # "env": { "Path": "${env:Path};C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.38.33130\\bin\\Hostx64\\x64" }
      set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /fsanitize=address")
      set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fsanitize=address")
   endif ()

   # We don't use exceptions in optimised configurations, so disable exception support there.
   string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
   add_compile_definitions ("$<$<OR:$<CONFIG:Release>,$<CONFIG:FastBuild>>:_HAS_EXCEPTIONS=0>")
   add_compile_options (
      "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:FastBuild>>>:/EHs-c->"
      "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:FastBuild>>>:/GR>"
      "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:FastBuild>>>>:/EHsc>")

   # Specifying the fast FP option shows no descernible improvement in benchmarked release builds.
   #set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fp:fast")
   #set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /fp:fast")
else ()
   if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      add_compile_definitions ("_DARWIN_C_SOURCE")
   elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
      add_compile_definitions ("_GNU_SOURCE")
      set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdiagnostics-color=auto -fdata-sections -ffunction-sections -fno-strict-aliasing -Wno-pointer-sign")
      set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color=auto -fdata-sections -ffunction-sections -fno-strict-aliasing")
   elseif (WIN32)
      add_definitions ("-D_WIN32" "-U__unix__")
      set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -fdiagnostics-color=auto -fdata-sections -ffunction-sections -fno-strict-aliasing -Wno-pointer-sign ${KOTUKU_ARCH}")
      set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdata-sections -fdiagnostics-color=auto -ffunction-sections -fno-strict-aliasing ${KOTUKU_ARCH}")
      if (PRINTF64_ID)
         add_compile_definitions ("PRINTF64I")
      endif ()
      if (PRINTF64_PRID)
         add_compile_definitions ("PRINTF64_PRID")
      endif ()
   elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
      # TODO
   endif ()

   set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-exceptions")

   if (ENABLE_ANALYSIS)
      # NB: The use of dlclose() is not permitted when analysis is enabled because it upsets the log output.
      # In addition, gdb breakpoints are unlikely to work with these features enabled.
      set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fsanitize=leak -DANALYSIS_ENABLED")
      set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=leak -DANALYSIS_ENABLED")
      set (DISABLE_AUDIO ON)
      if (NOT KOTUKU_STATIC)
         message (FATAL_ERROR "Address analysis requires the build to be compiled with KOTUKU_STATIC on.")
      endif ()
   endif ()

   set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wno-empty-body -Wno-unused-result -Wno-format-zero-length -Wno-unused-but-set-variable -Wno-stringop-overflow")
   set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-empty-body -Wno-unused-result -Wno-format-zero-length -Wno-unused-but-set-variable -Wno-stringop-overflow")

   # These default compile options are for use by target_compile_options()
   # TODO: Utilise target_compile_options() in the makefiles

   set (GCC_COMPAT "$<COMPILE_LANG_AND_ID:CXX,ARMClang,AppleClang,Clang,GNU>")
   set (DEFAULT_GCC_FLAGS "-Wall" "-Wno-empty-body" "-Wno-unused-result" "-Wno-format-zero-length" "-Wno-unused-but-set-variable" "-Wno-stringop-overflow")
   set (DEFAULT_GCXX_FLAGS "-Wall" "-Wno-empty-body" "-Wno-unused-result" "-Wno-format-zero-length" "-Wno-unused-but-set-variable" "-Wno-stringop-overflow")
endif ()

# --------------------------------------------------------------------------------------------------------------------
# FastBuild configuration - optimised for compilation speed rather than runtime performance

set (KOTUKU_FASTBUILD_ENABLED FALSE)
if (CMAKE_BUILD_TYPE STREQUAL "FastBuild")
   set (KOTUKU_FASTBUILD_ENABLED TRUE)
elseif (KOTUKU_MULTI_CONFIG)
   list (FIND CMAKE_CONFIGURATION_TYPES FastBuild KOTUKU_FASTBUILD_INDEX)
   if (NOT KOTUKU_FASTBUILD_INDEX EQUAL -1)
      set (KOTUKU_FASTBUILD_ENABLED TRUE)
   endif ()
endif ()

if (KOTUKU_FASTBUILD_ENABLED)
   message (STATUS "FastBuild configuration enabled - optimising for compilation speed")

   if (MSVC)
      # MSVC: Use /Od for no optimisation, /MP for parallel compilation, /Z7 for minimal debug info
      set (CMAKE_C_FLAGS_FASTBUILD "${CMAKE_C_FLAGS_FASTBUILD} /Od /MP /Z7")
      set (CMAKE_CXX_FLAGS_FASTBUILD "${CMAKE_CXX_FLAGS_FASTBUILD} /Od /MP /Z7")

      # Disable exception handling for faster compilation (matches Release build behaviour)
      add_compile_definitions ("$<$<CONFIG:FastBuild>:_HAS_EXCEPTIONS=0>")
   else ()
      # GCC/Clang: Use -O0 for no optimisation, -g1 for minimal debug info
      set (CMAKE_C_FLAGS_FASTBUILD "${CMAKE_C_FLAGS_FASTBUILD} -O0 -g1")
      set (CMAKE_CXX_FLAGS_FASTBUILD "${CMAKE_CXX_FLAGS_FASTBUILD} -O0 -g1")

      # Disable variable tracking for faster compilation
      set (CMAKE_C_FLAGS_FASTBUILD "${CMAKE_C_FLAGS_FASTBUILD} -fno-var-tracking -fno-var-tracking-assignments")
      set (CMAKE_CXX_FLAGS_FASTBUILD "${CMAKE_CXX_FLAGS_FASTBUILD} -fno-var-tracking -fno-var-tracking-assignments")

      # Try to use faster linkers if available
      include(CheckLinkerFlag)
      check_linker_flag(CXX "-fuse-ld=lld" HAVE_LLD_LINKER)
      if (HAVE_LLD_LINKER)
         set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
         set (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=lld")
         message (STATUS "Using LLD linker for faster linking")
      else ()
         check_linker_flag(CXX "-fuse-ld=gold" HAVE_GOLD_LINKER)
         if (HAVE_GOLD_LINKER)
            set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold")
            set (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=gold")
            message (STATUS "Using Gold linker for faster linking")
         endif ()
      endif ()

      # Disable exceptions for faster compilation (matches Release build behaviour)
      set (CMAKE_CXX_FLAGS_FASTBUILD "${CMAKE_CXX_FLAGS_FASTBUILD} -fno-exceptions")
   endif ()
endif ()

# Support for building Windows resource files with MINGW

if (MINGW)
  set (CMAKE_RC_COMPILER_INIT windres)
  enable_language (RC)
  set (CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -O coff -i <SOURCE> -o <OBJECT>")
endif (MINGW)

# --------------------------------------------------------------------------------------------------------------------
# Sub-libraries are defined as linkable objects so that they work correctly between static and modular builds.

add_library(linear_rgb OBJECT src/link/linear_rgb.cpp)
add_library(unicode OBJECT src/link/unicode.cpp)
add_library(base64 OBJECT src/link/base64.cpp)

# --------------------------------------------------------------------------------------------------------------------

set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

include(ExternalProject)

# Build zlib as an external project so the source archive is downloaded only when required.
if (KOTUKU_STATIC)
   set (ZLIB_TARGET_TYPE STATIC)
   set (ZLIB_BUILD_SHARED OFF)
   set (ZLIB_BUILD_STATIC ON)

   if (WIN32)
      if (MSVC)
         set (ZLIB_LIB_PATH_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/zs.lib")
         set (ZLIB_LIB_PATH_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/zsd.lib")
      else ()
         set (ZLIB_LIB_PATH_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/libzs.a")
         set (ZLIB_LIB_PATH_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/libzsd.a")
      endif ()
      if (NOT KOTUKU_MULTI_CONFIG AND CMAKE_BUILD_TYPE STREQUAL "Debug")
         set (ZLIB_LIB_PATH "${ZLIB_LIB_PATH_DEBUG}")
      else ()
         set (ZLIB_LIB_PATH "${ZLIB_LIB_PATH_RELEASE}")
      endif ()
   else ()
      set (ZLIB_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/libz.a")
   endif ()

   set (ZLIB_BUILD_BYPRODUCTS ${ZLIB_LIB_PATH})
   if (WIN32)
      list (APPEND ZLIB_BUILD_BYPRODUCTS ${ZLIB_LIB_PATH_DEBUG})
   endif ()
else ()
   set (ZLIB_TARGET_TYPE SHARED)
   set (ZLIB_BUILD_SHARED ON)
   set (ZLIB_BUILD_STATIC OFF)

   if (WIN32)
      if (MSVC)
         set (ZLIB_IMPLIB_PATH_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/z.lib")
         set (ZLIB_IMPLIB_PATH_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/zd.lib")
      else ()
         set (ZLIB_IMPLIB_PATH_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/libz.dll.a")
         set (ZLIB_IMPLIB_PATH_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/libzd.dll.a")
      endif ()
      set (ZLIB_RUNTIME_PATH_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/zlib/bin/z.dll")
      set (ZLIB_RUNTIME_PATH_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/zlib/bin/zd.dll")
      if (NOT KOTUKU_MULTI_CONFIG AND CMAKE_BUILD_TYPE STREQUAL "Debug")
         set (ZLIB_IMPLIB_PATH "${ZLIB_IMPLIB_PATH_DEBUG}")
         set (ZLIB_RUNTIME_PATH "${ZLIB_RUNTIME_PATH_DEBUG}")
      else ()
         set (ZLIB_IMPLIB_PATH "${ZLIB_IMPLIB_PATH_RELEASE}")
         set (ZLIB_RUNTIME_PATH "${ZLIB_RUNTIME_PATH_RELEASE}")
      endif ()
   elseif (APPLE)
      set (ZLIB_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/libz.dylib")
   else ()
      set (ZLIB_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/libz.so")
   endif ()

   set (ZLIB_BUILD_BYPRODUCTS ${ZLIB_RUNTIME_PATH})
   if (WIN32)
      list (APPEND ZLIB_BUILD_BYPRODUCTS
         ${ZLIB_IMPLIB_PATH}
         ${ZLIB_RUNTIME_PATH_DEBUG}
         ${ZLIB_IMPLIB_PATH_DEBUG})
   endif ()
endif ()

list (REMOVE_DUPLICATES ZLIB_BUILD_BYPRODUCTS)

set (ZLIB_CMAKE_ARGS
   -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
   -DCMAKE_INSTALL_BINDIR=bin
   -DCMAKE_INSTALL_LIBDIR=lib
   -DCMAKE_INSTALL_INCLUDEDIR=include
   -DCMAKE_INSTALL_MANDIR=share/man
   -DCMAKE_INSTALL_DOCDIR=share/doc
   -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
list (APPEND ZLIB_CMAKE_ARGS
   -DZLIB_BUILD_SHARED=${ZLIB_BUILD_SHARED}
   -DZLIB_BUILD_STATIC=${ZLIB_BUILD_STATIC}
   -DZLIB_BUILD_TESTING=OFF
   -DZLIB_INSTALL=ON)

if (NOT KOTUKU_MULTI_CONFIG)
   list (APPEND ZLIB_CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
endif ()

string (CONCAT ZLIB_URL_HASH
   "SHA512=16fea4df307a68cf0035858abe2fd550250618a97590e202037acd18a666f57afc10f8836cbbd"
   "472d54a0e76539d0e558cb26f059d53de52ff90634bbf4f47d4")

ExternalProject_Add(zlib-project
   URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
   URL_HASH ${ZLIB_URL_HASH}
   DOWNLOAD_EXTRACT_TIMESTAMP TRUE
   PREFIX "${CMAKE_CURRENT_BINARY_DIR}/zlib"
   INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/zlib"
   BUILD_BYPRODUCTS
      ${ZLIB_BUILD_BYPRODUCTS}
   CMAKE_ARGS
      ${ZLIB_CMAKE_ARGS}
   BUILD_ALWAYS FALSE
)

ExternalProject_Get_Property(zlib-project install_dir)
set (ZLIB_HEADERS "${install_dir}/include")

file (MAKE_DIRECTORY ${ZLIB_HEADERS})

add_library(zlib ${ZLIB_TARGET_TYPE} IMPORTED)

if (KOTUKU_STATIC AND WIN32 AND KOTUKU_MULTI_CONFIG)
   set_target_properties(zlib PROPERTIES
      IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
      IMPORTED_LOCATION_DEBUG ${ZLIB_LIB_PATH_DEBUG}
      IMPORTED_LOCATION_RELEASE ${ZLIB_LIB_PATH_RELEASE}
      IMPORTED_LOCATION_RELWITHDEBINFO ${ZLIB_LIB_PATH_RELEASE}
      IMPORTED_LOCATION_MINSIZEREL ${ZLIB_LIB_PATH_RELEASE}
      INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_HEADERS})
elseif (KOTUKU_STATIC)
   set_target_properties(zlib PROPERTIES
      IMPORTED_LOCATION ${ZLIB_LIB_PATH}
      INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_HEADERS})
elseif (WIN32 AND KOTUKU_MULTI_CONFIG)
   set_target_properties(zlib PROPERTIES
      IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
      IMPORTED_IMPLIB_DEBUG ${ZLIB_IMPLIB_PATH_DEBUG}
      IMPORTED_IMPLIB_RELEASE ${ZLIB_IMPLIB_PATH_RELEASE}
      IMPORTED_IMPLIB_RELWITHDEBINFO ${ZLIB_IMPLIB_PATH_RELEASE}
      IMPORTED_IMPLIB_MINSIZEREL ${ZLIB_IMPLIB_PATH_RELEASE}
      IMPORTED_LOCATION_DEBUG ${ZLIB_RUNTIME_PATH_DEBUG}
      IMPORTED_LOCATION_RELEASE ${ZLIB_RUNTIME_PATH_RELEASE}
      IMPORTED_LOCATION_RELWITHDEBINFO ${ZLIB_RUNTIME_PATH_RELEASE}
      IMPORTED_LOCATION_MINSIZEREL ${ZLIB_RUNTIME_PATH_RELEASE}
      INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_HEADERS})
elseif (WIN32)
   set_target_properties(zlib PROPERTIES
      IMPORTED_IMPLIB ${ZLIB_IMPLIB_PATH}
      IMPORTED_LOCATION ${ZLIB_RUNTIME_PATH}
      INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_HEADERS})
else ()
   set_target_properties(zlib PROPERTIES
      IMPORTED_LOCATION ${ZLIB_RUNTIME_PATH}
      INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_HEADERS})
endif ()

add_dependencies(zlib zlib-project)

if (KOTUKU_INSTALL AND NOT KOTUKU_STATIC)
   if (WIN32)
      install(CODE
         "file(REMOVE
            \"\${CMAKE_INSTALL_PREFIX}/${LIB_TARGET}/lib/zlib1.dll\"
            \"\${CMAKE_INSTALL_PREFIX}/${LIB_TARGET}/lib/zlibd1.dll\"
            \"\${CMAKE_INSTALL_PREFIX}/${LIB_TARGET}/lib/zlib.lib\"
            \"\${CMAKE_INSTALL_PREFIX}/${LIB_TARGET}/lib/zlibd.lib\")")
      install(FILES ${ZLIB_RUNTIME_PATH_DEBUG} CONFIGURATIONS Debug DESTINATION "${LIB_TARGET}/lib")
      install(FILES ${ZLIB_IMPLIB_PATH_DEBUG} CONFIGURATIONS Debug DESTINATION "${LIB_TARGET}/lib")
      install(FILES ${ZLIB_RUNTIME_PATH_RELEASE}
         CONFIGURATIONS Release RelWithDebInfo MinSizeRel
         DESTINATION "${LIB_TARGET}/lib")
      install(FILES ${ZLIB_IMPLIB_PATH_RELEASE}
         CONFIGURATIONS Release RelWithDebInfo MinSizeRel
         DESTINATION "${LIB_TARGET}/lib")
   else ()
      install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/zlib/lib/"
         DESTINATION "${LIB_TARGET}/lib"
         FILES_MATCHING
            PATTERN "libz.so*"
            PATTERN "libz*.dylib"
            PATTERN "cmake" EXCLUDE
            PATTERN "pkgconfig" EXCLUDE)
   endif ()
elseif (KOTUKU_INSTALL AND KOTUKU_STATIC AND WIN32)
   install(CODE
      "file(REMOVE
         \"\${CMAKE_INSTALL_PREFIX}/${LIB_TARGET}/lib/z.dll\"
         \"\${CMAKE_INSTALL_PREFIX}/${LIB_TARGET}/lib/zd.dll\"
         \"\${CMAKE_INSTALL_PREFIX}/${LIB_TARGET}/lib/z.lib\"
         \"\${CMAKE_INSTALL_PREFIX}/${LIB_TARGET}/lib/zd.lib\")")
endif ()

if (NOT DISABLE_FONT)
   add_subdirectory (3rdparty/freetype-2.13.2)
endif()

# --------------------------------------------------------------------------------------------------------------------
# Build libjpeg-turbo as an external project (only if JPEG and Image support is enabled)

if (NOT DISABLE_JPEG AND NOT DISABLE_IMAGE)
   if (MSVC)
      set(LIBJPEG_TURBO_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo/lib/jpeg-static.lib")
   else ()
      set(LIBJPEG_TURBO_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo/lib/libjpeg.a")
   endif ()

   set(LIBJPEG_TURBO_CMAKE_ARGS
      -DENABLE_SHARED=OFF
      -DENABLE_STATIC=ON
      -DWITH_TURBOJPEG=ON
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_INSTALL_LIBDIR=<INSTALL_DIR>/lib
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

   if (NOT KOTUKU_MULTI_CONFIG)
      list(APPEND LIBJPEG_TURBO_CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
   endif ()

   if (WIN32)
      unset(LIBJPEG_TURBO_NASM_COMPILER)
      unset(LIBJPEG_TURBO_NASM_COMPILER CACHE)

      if (DEFINED CMAKE_ASM_NASM_COMPILER AND EXISTS "${CMAKE_ASM_NASM_COMPILER}")
         set(LIBJPEG_TURBO_NASM_COMPILER "${CMAKE_ASM_NASM_COMPILER}")
      elseif (DEFINED ENV{ASM_NASM} AND EXISTS "$ENV{ASM_NASM}")
         set(LIBJPEG_TURBO_NASM_COMPILER "$ENV{ASM_NASM}")
      else ()
         find_program(LIBJPEG_TURBO_NASM_COMPILER
            NAMES nasm yasm
            HINTS
               "$ENV{ProgramFiles}/NASM"
               "$ENV{ProgramW6432}/NASM"
               "C:/Program Files/NASM"
               "C:/Program Files (x86)/NASM"
            PATH_SUFFIXES
               ""
               bin)
      endif ()

      if (LIBJPEG_TURBO_NASM_COMPILER)
         file(TO_CMAKE_PATH "${LIBJPEG_TURBO_NASM_COMPILER}" LIBJPEG_TURBO_NASM_COMPILER)
         list(APPEND LIBJPEG_TURBO_CMAKE_ARGS
            -DCMAKE_ASM_NASM_COMPILER:FILEPATH=${LIBJPEG_TURBO_NASM_COMPILER})
         message(STATUS "libjpeg-turbo NASM compiler: ${LIBJPEG_TURBO_NASM_COMPILER}")
      endif ()
   endif ()

   ExternalProject_Add(libjpeg-turbo-project
      URL https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.1.1.tar.gz
      URL_HASH SHA512=4937b63a27818cdb5087091b2d78837f7f385fd6b4d3e3fcaf4d9ad2944fed4a00020dcacb33e9c2fd4b0f9d9851fb4051ed3da86f606aca5167357262a73e89
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      PREFIX "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo"
      INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo"
      BUILD_BYPRODUCTS ${LIBJPEG_TURBO_LIB_PATH}
      CMAKE_ARGS
         ${LIBJPEG_TURBO_CMAKE_ARGS}
      BUILD_ALWAYS FALSE
   )

   ExternalProject_Get_Property(libjpeg-turbo-project install_dir)
   set(LIBJPEG_TURBO_INCLUDE_DIRS "${install_dir}/include")

   # Create the directory so changing INTERFACE_INCLUDE_DIRECTORIES doesn't fail
   file(MAKE_DIRECTORY ${LIBJPEG_TURBO_INCLUDE_DIRS})

   add_library(libjpeg-turbo STATIC IMPORTED)

   set_target_properties(libjpeg-turbo PROPERTIES
      IMPORTED_LOCATION ${LIBJPEG_TURBO_LIB_PATH}
      INTERFACE_INCLUDE_DIRECTORIES ${LIBJPEG_TURBO_INCLUDE_DIRS})

   add_dependencies(libjpeg-turbo libjpeg-turbo-project)
endif()

# --------------------------------------------------------------------------------------------------------------------

if (NOT DISABLE_AUDIO)
   add_subdirectory (src/audio)
   list (APPEND LIB_LIST audio)
endif ()

if (NOT DISABLE_BACKSTAGE AND NOT DISABLE_NETWORK)
   add_subdirectory (src/backstage)
   list (APPEND LIB_LIST backstage)
endif()

if (NOT DISABLE_DISPLAY)
   add_subdirectory (src/display)
   list (APPEND LIB_LIST display)
endif ()

if ((NOT DISABLE_DOCUMENT) AND (NOT DISABLE_DISPLAY) AND (NOT DISABLE_VECTOR) AND (NOT DISABLE_FONT))
   add_subdirectory (src/document)
   list (APPEND LIB_LIST document)
endif ()

if (NOT DISABLE_FONT AND NOT DISABLE_DISPLAY)
   add_subdirectory (src/font)
   list (APPEND LIB_LIST font)
endif ()

if (NOT DISABLE_HTTP AND NOT DISABLE_NETWORK)
   add_subdirectory (src/http)
   list (APPEND LIB_LIST http)
endif ()

if (NOT DISABLE_MP3 AND NOT DISABLE_AUDIO)
   add_subdirectory (src/mp3)
   list (APPEND LIB_LIST mp3)
endif ()

if (NOT DISABLE_NETWORK)
   add_subdirectory (src/network)
   list (APPEND LIB_LIST network)
endif ()

if (NOT DISABLE_IMAGE AND NOT DISABLE_DISPLAY)
   add_subdirectory (src/image)
   list (APPEND LIB_LIST image)
endif ()

if (NOT DISABLE_JPEG AND NOT DISABLE_IMAGE AND NOT DISABLE_DISPLAY)
   add_subdirectory (src/image_jpeg)
   list (APPEND LIB_LIST jpeg)
endif ()

if (NOT DISABLE_SCINTILLA AND NOT DISABLE_DISPLAY AND NOT DISABLE_VECTOR AND NOT DISABLE_FONT)
   add_subdirectory (src/scintilla)
   list (APPEND LIB_LIST scintilla)
endif ()

if (NOT DISABLE_SVG AND NOT DISABLE_VECTOR AND NOT DISABLE_DISPLAY AND NOT DISABLE_FONT)
   add_subdirectory (src/svg)
   list (APPEND LIB_LIST svg)
endif ()

if (NOT DISABLE_VECTOR AND NOT DISABLE_DISPLAY AND NOT DISABLE_FONT)
   add_subdirectory (src/vector)
   list (APPEND LIB_LIST vector)
endif ()

list (APPEND LIB_LIST ${REQUIRED_MODS})

add_subdirectory (src/link)
add_subdirectory (src/core)
add_subdirectory (src/tiri)
add_subdirectory (src/xml)
add_subdirectory (src/xquery)
add_subdirectory (src/json)
add_subdirectory (src/regex)

add_subdirectory (src/origo)

add_subdirectory (apps)
add_subdirectory (tools/tiri_lsp)

add_generated_header_dependencies (display xml_defs regex_defs image_defs)
add_generated_header_dependencies (document display_defs font_defs svg_defs xml_defs xquery_defs)
add_generated_header_dependencies (font display_defs xml_defs)
add_generated_header_dependencies (http network_defs)
add_generated_header_dependencies (init-unix core_defs)
add_generated_header_dependencies (init-win core_defs)
add_generated_header_dependencies (jpeg display_defs image_defs)
add_generated_header_dependencies (json xml_defs)
add_generated_header_dependencies (base64 core_defs)
add_generated_header_dependencies (linear_rgb core_defs)
add_generated_header_dependencies (mp3 audio_defs)
add_generated_header_dependencies (origo_cmd core_defs display_defs)
add_generated_header_dependencies (image display_defs)
add_generated_header_dependencies (scintilla display_defs font_defs vector_defs xml_defs)
add_generated_header_dependencies (svg display_defs image_defs vector_defs xml_defs)
add_generated_header_dependencies (tiri core_defs display_defs regex_defs xml_defs)
add_generated_header_dependencies (unicode core_defs)
add_generated_header_dependencies (vector display_defs font_defs image_defs xml_defs)
add_generated_header_dependencies (xml xquery_defs)
add_generated_header_dependencies (xquery regex_defs xml_defs)
resolve_generated_header_dependencies ()

# --------------------------------------------------------------------------------------------------------------------
# Create a combined static library that links all modules together
# This breaks the circular dependencies by having a separate target own all the inter-module linkage

if (KOTUKU_STATIC)
   # Create an interface library that groups all modules together
   add_library (kotuku_static INTERFACE)

   target_link_libraries (kotuku_static INTERFACE core ${LIB_LIST})

   message(STATUS "Created combined interface library 'kotuku_static' with modules: core ${LIB_LIST}")
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Generate the platform configuration header

configure_file ("src/config.h.in" "kotuku/config.h" NEWLINE_STYLE UNIX)

# --------------------------------------------------------------------------------------------------------------------
# Icons are installed as a zip file

if (KOTUKU_ICON_DEPENDENCIES)
   file(GLOB_RECURSE icon_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/data/icons/Default/*.svg")
endif ()

add_custom_command (
   OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/icons/Default.zip"
   COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/icons"
   COMMAND ${CMAKE_COMMAND} -E tar "cf" "${CMAKE_CURRENT_BINARY_DIR}/icons/Default.zip" --format=zip -- "."
   DEPENDS ${icon_files}
   WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/data/icons/Default"
   COMMENT "Zipping icons")

add_custom_target (icon_package ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/icons/Default.zip")

# --------------------------------------------------------------------------------------------------------------------
# SSL Certificate Bundle Update

if (NOT DISABLE_SSL AND ENABLE_SSL_BUNDLE_UPDATE)
   set(SSL_BUNDLE_PATH "${PROJECT_SOURCE_DIR}/data/ssl/ca-bundle.crt")
   set(SSL_BUNDLE_UPDATE_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/UpdateSslBundle.cmake")

   file(WRITE "${SSL_BUNDLE_UPDATE_SCRIPT}" [=[
if (NOT DEFINED SSL_BUNDLE_PATH)
   message(FATAL_ERROR "SSL_BUNDLE_PATH is not defined")
endif ()

get_filename_component(SSL_BUNDLE_DIR "${SSL_BUNDLE_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${SSL_BUNDLE_DIR}")

set(SSL_BUNDLE_URL "https://curl.se/ca/cacert.pem")

file(DOWNLOAD
   "${SSL_BUNDLE_URL}"
   "${SSL_BUNDLE_PATH}"
   TIMEOUT 30
   TLS_VERIFY ON
   STATUS SSL_BUNDLE_DOWNLOAD_STATUS)

list(GET SSL_BUNDLE_DOWNLOAD_STATUS 0 SSL_BUNDLE_DOWNLOAD_CODE)
list(GET SSL_BUNDLE_DOWNLOAD_STATUS 1 SSL_BUNDLE_DOWNLOAD_MESSAGE)

if (NOT SSL_BUNDLE_DOWNLOAD_CODE EQUAL 0)
   message(FATAL_ERROR "Failed to download ${SSL_BUNDLE_URL}: ${SSL_BUNDLE_DOWNLOAD_MESSAGE}")
endif ()

message(STATUS "Updated SSL certificate bundle: ${SSL_BUNDLE_PATH}")
]=])

   add_custom_target(update_ssl_bundle
      COMMAND ${CMAKE_COMMAND}
         "-DSSL_BUNDLE_PATH=${SSL_BUNDLE_PATH}"
         -P "${SSL_BUNDLE_UPDATE_SCRIPT}"
      COMMENT "Updating SSL certificate bundle"
      VERBATIM)
endif()

# --------------------------------------------------------------------------------------------------------------------
# Installation steps

if (KOTUKU_INSTALL)
   if (INSTALL_INCLUDES)
      install (DIRECTORY "${INCLUDE_OUTPUT}" DESTINATION "include")
      install (FILES "${CMAKE_BINARY_DIR}/kotuku/config.h" DESTINATION "include/kotuku/")
   endif ()

   if (NOT DISABLE_SSL)
      if (WIN32)
         install (DIRECTORY "${PROJECT_SOURCE_DIR}/data/ssl/" DESTINATION "${CONFIG_TARGET}/ssl"
            FILES_MATCHING
               PATTERN "ca-bundle.crt"
               PATTERN "localhost.pem"
               PATTERN "localhost.p12"
               PATTERN "localhost-password.p12")
      else ()
         install (DIRECTORY "${PROJECT_SOURCE_DIR}/data/ssl/" DESTINATION "${CONFIG_TARGET}/ssl")
      endif ()
   endif ()
   if (NOT DISABLE_FONT)
      install (DIRECTORY "${PROJECT_SOURCE_DIR}/data/fonts/" DESTINATION "${CONFIG_TARGET}/fonts")
   endif ()

   install (DIRECTORY "${PROJECT_SOURCE_DIR}/data/styles/" DESTINATION "${CONFIG_TARGET}/styles")
   install (FILES "${CMAKE_BINARY_DIR}/icons/Default.zip" DESTINATION "${CONFIG_TARGET}/styles/icons")

   install (DIRECTORY "${PROJECT_SOURCE_DIR}/apps/data" DESTINATION "${SHARE_TARGET}/apps")
   install (FILES
      "${PROJECT_SOURCE_DIR}/apps/find.tiri"
      "${PROJECT_SOURCE_DIR}/apps/paintbox.tiri"
      "${PROJECT_SOURCE_DIR}/apps/tuku.tiri"
      "${PROJECT_SOURCE_DIR}/apps/tuku_server.tiri"
      "${PROJECT_SOURCE_DIR}/apps/tuku_server_cfg.json"
      "${PROJECT_SOURCE_DIR}/apps/vuepoint.tiri"
      DESTINATION "${SHARE_TARGET}/apps")

   install (DIRECTORY "${PROJECT_SOURCE_DIR}/tools/tiri_lsp/include" DESTINATION "${SHARE_TARGET}/tools/tiri_lsp")
   install (FILES
      "${PROJECT_SOURCE_DIR}/tools/tiri_lsp/server.tiri"
      "${PROJECT_SOURCE_DIR}/tools/tiri_lsp/README.md"
      DESTINATION "${SHARE_TARGET}/tools/tiri_lsp")

   install (DIRECTORY "${PROJECT_SOURCE_DIR}/scripts/" DESTINATION "${SHARE_TARGET}/scripts")

   if (INSTALL_EXAMPLES)
      install (DIRECTORY "${PROJECT_SOURCE_DIR}/examples/" DESTINATION "${SHARE_TARGET}/examples")
   endif ()

   if (NOT KOTUKU_STATIC)
      install (CODE "file(REMOVE \"${KOTUKU_ROOT}/config/classes.bin\")")
      install (CODE "MESSAGE(STATUS \"${ORIGO_CMD_TARGET} --log-warning --gfx-driver=headless --verify\")")
      install (CODE "execute_process(COMMAND \"${ORIGO_CMD_TARGET}\" --log-warning --gfx-driver=headless --verify WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\")")
   endif ()

   if (RUN_ANYWHERE)
      install (CODE "MESSAGE(STATUS \"\nKotuku has been installed to '${CMAKE_INSTALL_PREFIX}'.  You may need to add this folder to your PATH variable in order to run origo from the command-line.\")")
   else ()
      install (CODE "MESSAGE(STATUS \"\nKotuku has been installed to '${CMAKE_INSTALL_PREFIX}' and shared files are in '${KOTUKU_ROOT}'.\")")
   endif ()
endif ()

# --------------------------------------------------------------------------------------------------------------------
# Debian Package Generation.  Use in conjunction with tools/build-deb.sh to create a 'true' Debian package.
# Alternatively use tools/build-deb-tarball.sh to convert the standard release to a portable Debian package.

if (GENERATE_DEBIAN_PKG)
   # Detect system architecture
   if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
      set(DEBIAN_ARCH "amd64")
   elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86")
      set(DEBIAN_ARCH "i386")
   elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
      set(DEBIAN_ARCH "arm64")
   elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "armv7")
      set(DEBIAN_ARCH "armhf")
   else()
      set(DEBIAN_ARCH "any")
   endif()

   # Get number of processors for parallel build
   include(ProcessorCount)
   ProcessorCount(NPROC)
   if(NPROC EQUAL 0)
      set(NPROC 1)
   endif()

   # Set version from project
   set(VERSION "${PROJECT_VERSION}")

   # Generate build date in Debian format
   string(TIMESTAMP BUILD_DATE "%a, %d %b %Y %H:%M:%S %z")

   # Set whether to install includes based on whether we're building dev package
   if (INSTALL_INCLUDES)
      set(INSTALL_INCLUDES_VAL "TRUE")
   else()
      set(INSTALL_INCLUDES_VAL "FALSE")
   endif()

   # Create debian directory in build folder
   file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/debian")

   # Generate debian control file
   configure_file(
      "${CMAKE_SOURCE_DIR}/pkg/debian/control.in"
      "${CMAKE_BINARY_DIR}/debian/control"
      @ONLY
   )

   # Generate debian rules file
   configure_file(
      "${CMAKE_SOURCE_DIR}/pkg/debian/rules.in"
      "${CMAKE_BINARY_DIR}/debian/rules"
      @ONLY
   )

   # Generate debian changelog
   # Try to get recent git commits for changelog entries
   find_package(Git QUIET)
   if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
      execute_process(
         COMMAND ${GIT_EXECUTABLE} log --oneline -n 5 --pretty=format:"  * %s"
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
         OUTPUT_VARIABLE GIT_LOG_ENTRIES
         OUTPUT_STRIP_TRAILING_WHITESPACE
         ERROR_QUIET
      )
      # Remove double quotes
      string(REPLACE "\"" "" GIT_LOG_ENTRIES "${GIT_LOG_ENTRIES}")
      # Replace newlines with spaces
      string(REPLACE "\n" " " GIT_LOG_ENTRIES "${GIT_LOG_ENTRIES}")
      # Replace carriage returns with spaces
      string(REPLACE "\r" " " GIT_LOG_ENTRIES "${GIT_LOG_ENTRIES}")
      # Replace tabs with spaces
      string(REPLACE "\t" " " GIT_LOG_ENTRIES "${GIT_LOG_ENTRIES}")
      set(CHANGELOG_ENTRIES "${GIT_LOG_ENTRIES}")
   else()
      set(CHANGELOG_ENTRIES "  * Release build")
   endif()

   configure_file(
      "${CMAKE_SOURCE_DIR}/pkg/debian/changelog.in"
      "${CMAKE_BINARY_DIR}/debian/changelog"
      @ONLY
   )

   # Copy static files
   file(COPY "${CMAKE_SOURCE_DIR}/pkg/debian/copyright"
        DESTINATION "${CMAKE_BINARY_DIR}/debian/")
   file(COPY "${CMAKE_SOURCE_DIR}/pkg/debian/compat"
        DESTINATION "${CMAKE_BINARY_DIR}/debian/")
   file(COPY "${CMAKE_SOURCE_DIR}/pkg/debian/kotuku.install"
        DESTINATION "${CMAKE_BINARY_DIR}/debian/")

   # Copy dev package install file if building with headers
   if (INSTALL_INCLUDES)
      file(COPY "${CMAKE_SOURCE_DIR}/pkg/debian/kotuku-dev.install"
           DESTINATION "${CMAKE_BINARY_DIR}/debian/")
   endif()

   # Make rules file executable
   file(CHMOD "${CMAKE_BINARY_DIR}/debian/rules" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

   message(STATUS "Debian packaging files generated in ${CMAKE_BINARY_DIR}/debian/")
   message(STATUS "Use tools/build-deb.sh script to build the package")
endif()

# --------------------------------------------------------------------------------------------------------------------
# Windows Package Generation using CPack

if(WIN32 AND ENABLE_CPACK)
   # Detect Windows architecture for package naming
   if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(WIN_ARCH "win64")
   else()
      set(WIN_ARCH "win32")
   endif()

   # Generate timestamp for release naming (matches existing pattern)
   string(TIMESTAMP RELEASE_DATE "%Y%m%d")

   # Basic package information
   set(CPACK_PACKAGE_NAME "Kotuku")
   set(CPACK_PACKAGE_VENDOR "Kotuku Open Group")
   set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
   set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Kotuku Application Framework")
   set(CPACK_PACKAGE_DESCRIPTION "A cross-platform application framework specialising in scalable user interfaces.")

   # Package file naming to match existing releases (kotuku-win64-YYYYMMDD)
   set(CPACK_PACKAGE_FILE_NAME "kotuku-${WIN_ARCH}-${RELEASE_DATE}")

   # Check for 7-Zip availability for SFX generation
   find_program(SEVENZIP_EXECUTABLE NAMES 7z 7za 7zip HINTS "C:/Program Files/7-Zip" "C:/Program Files (x86)/7-Zip")

   # Configure generators - ZIP for portable distribution, NSIS for installer, 7Z for SFX
   set(CPACK_GENERATOR "ZIP")
   if(SEVENZIP_EXECUTABLE)
      list(APPEND CPACK_GENERATOR "7Z")
      message(STATUS "7-Zip found at ${SEVENZIP_EXECUTABLE} - SFX support enabled")
   else()
      message(STATUS "7-Zip not found - SFX generation disabled")
   endif()

   if(NOT KOTUKU_STATIC)
      # Only provide NSIS installer for modular builds (static builds are portable)
      list(APPEND CPACK_GENERATOR "NSIS")
   endif()

   # ZIP Archive configuration (primary distribution method)
   set(CPACK_ARCHIVE_FILE_NAME "kotuku-${WIN_ARCH}-${RELEASE_DATE}")
   set(CPACK_SOURCE_IGNORE_FILES "/build/;/.git/;/\\\\.DS_Store")

   # NSIS installer configuration (for modular builds)
   if(NOT KOTUKU_STATIC)
      set(CPACK_NSIS_DISPLAY_NAME "Kotuku ${PROJECT_VERSION}")
      set(CPACK_NSIS_PACKAGE_NAME "Kotuku")
      set(CPACK_NSIS_INSTALLED_ICON_NAME "origo.exe")
      set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/src/link/kotuku.ico")
      set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/src/link/kotuku.ico")
      set(CPACK_NSIS_HELP_LINK "https://github.com/kotuku-group/kotuku")
      set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/kotuku-group/kotuku")
      set(CPACK_NSIS_CONTACT "support@kotuku.dev")

      # Create shortcuts for installer
      set(CPACK_NSIS_CREATE_ICONS_EXTRA
         "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Origo.lnk' '$INSTDIR\\\\origo.exe'"
         "CreateShortCut '$DESKTOP\\\\Origo.lnk' '$INSTDIR\\\\origo.exe'"
      )

      set(CPACK_NSIS_DELETE_ICONS_EXTRA
         "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Origo.lnk'"
         "Delete '$DESKTOP\\\\Origo.lnk'"
      )

      # Add to PATH for command-line access
      set(CPACK_NSIS_MODIFY_PATH ON)

      # Installation directory
      if(CMAKE_SIZEOF_VOID_P EQUAL 8)
         set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
      else()
         set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES")
      endif()
   endif()

   # License file (if available)
   if(EXISTS "${PROJECT_SOURCE_DIR}/LICENSE")
      set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
   elseif(EXISTS "${PROJECT_SOURCE_DIR}/LICENSE.md")
      set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE.md")
   endif()

   # Component configuration (matches existing install structure)
   set(CPACK_COMPONENTS_ALL Runtime Config Scripts Examples)
   set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Kotuku Runtime")
   set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Core Kotuku executable and libraries")
   set(CPACK_COMPONENT_RUNTIME_REQUIRED TRUE)

   set(CPACK_COMPONENT_CONFIG_DISPLAY_NAME "Configuration Files")
   set(CPACK_COMPONENT_CONFIG_DESCRIPTION "Fonts, styles, and configuration data")
   set(CPACK_COMPONENT_CONFIG_REQUIRED TRUE)

   set(CPACK_COMPONENT_SCRIPTS_DISPLAY_NAME "Tiri Scripts")
   set(CPACK_COMPONENT_SCRIPTS_DESCRIPTION "Standard library and GUI toolkit")
   set(CPACK_COMPONENT_SCRIPTS_REQUIRED TRUE)

   set(CPACK_COMPONENT_EXAMPLES_DISPLAY_NAME "Examples")
   set(CPACK_COMPONENT_EXAMPLES_DESCRIPTION "Example applications and demonstrations")
   set(CPACK_COMPONENT_EXAMPLES_REQUIRED FALSE)

   # Include CPack
   include(CPack)

   # Add custom SFX generation target if 7-Zip is available
   if(SEVENZIP_EXECUTABLE)
      set(SFX_STAGING_DIR "${CMAKE_BINARY_DIR}/sfx_temp")
      set(SFX_PACKAGE_DIR "Kotuku-${PROJECT_VERSION}")

      # Create SFX configuration file
      set(SFX_CONFIG_FILE "${CMAKE_BINARY_DIR}/sfx_config.txt")
      file(WRITE ${SFX_CONFIG_FILE}
";!@Install@!UTF-8!
Title=\"Kotuku ${PROJECT_VERSION}\"
BeginPrompt=\"Extract Kotuku to a directory of your choice.\"
ExtractTitle=\"Extracting Kotuku...\"
ExtractDialogText=\"Please wait while Kotuku is extracted.\"
InstallPath=\"%ProgramFiles%\\\\Kotuku\"
OverwriteMode=2
ExecuteFile=\"${SFX_PACKAGE_DIR}\\\\origo.exe\"
ExecuteParameters=\"\"
;!@InstallEnd@!
")

      add_custom_target(package_sfx
         COMMAND ${CMAKE_COMMAND} -E echo "Creating Self-Extracting Archive..."
         COMMAND ${CMAKE_COMMAND} -E remove_directory "${SFX_STAGING_DIR}"
         COMMAND ${CMAKE_COMMAND} -E make_directory "${SFX_STAGING_DIR}/${SFX_PACKAGE_DIR}"
         COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_INSTALL_PREFIX}" "${SFX_STAGING_DIR}/${SFX_PACKAGE_DIR}"
         COMMAND ${CMAKE_COMMAND} -E chdir "${SFX_STAGING_DIR}"
            ${SEVENZIP_EXECUTABLE} a -t7z -mx9 "${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}-sfx.exe"
            "${SFX_PACKAGE_DIR}" -sfx
         WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
         COMMENT "Creating Self-Extracting Archive: ${CPACK_PACKAGE_FILE_NAME}-sfx.exe"
         DEPENDS install
         VERBATIM
      )

      message(STATUS "SFX target 'package_sfx' created - use 'cmake --build . --target package_sfx' to generate SFX")
   endif()

   # Summary message
   set(PACKAGE_TYPES "ZIP archive")
   if(SEVENZIP_EXECUTABLE)
      set(PACKAGE_TYPES "${PACKAGE_TYPES} + Self-Extracting EXE")
   endif()
   if(NOT KOTUKU_STATIC)
      set(PACKAGE_TYPES "${PACKAGE_TYPES} + NSIS installer")
   endif()

   message(STATUS "Windows packaging configured: ${PACKAGE_TYPES}")
   message(STATUS "Package name: ${CPACK_PACKAGE_FILE_NAME}")
endif()
