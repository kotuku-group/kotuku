# Core Library

# IDL processing for headers and documentation

set (FUNCTIONS  # Full paths are required for add_custom_command() dependencies to work correctly
   "${CMAKE_CURRENT_SOURCE_DIR}/core.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_objects.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_events.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_fields_read.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_fields_write.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_filesystem.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_functions.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_locking.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_log.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_messages.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_memory.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/fs_folders.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/fs_identify.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/fs_resolution.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/fs_volumes.cpp")

set (CORE_CLASSES
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_metaclass.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_module.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_task.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_thread.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_time.cpp")

SET (CORE_DEFS
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_task_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_thread_def.c")

if (KOTUKU_DEFS_AVAILABLE)
   set (CORE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/core_defs.stamp")
   set (CORE_OUTPUTS
      ${CORE_DEFS}
      "${DOCS}/modules/core.xml"
      "${DOCS}/modules/classes/metaclass.xml"
      "${DOCS}/modules/classes/module.xml"
      "${DOCS}/modules/classes/task.xml"
      "${DOCS}/modules/classes/thread.xml"
      "${DOCS}/modules/classes/time.xml"
      "${INCLUDE_OUTPUT}/modules/core.h"
      "${INCLUDE_OUTPUT}/system/errors.h"
      "${INCLUDE_OUTPUT}/system/registry.h"
      "${INCLUDE_OUTPUT}/system/fields.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/data_functions.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/idl.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/prototypes.h")

   add_custom_command (
      OUTPUT "${CORE_STAMP}"
      BYPRODUCTS ${CORE_OUTPUTS}
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=defs/errors.tdl" "output=${INCLUDE_OUTPUT}/system/errors.h" "sdk=${PROJECT_SOURCE_DIR}"
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=defs/registry.tdl" "output=${INCLUDE_OUTPUT}/system/registry.h" "sdk=${PROJECT_SOURCE_DIR}"
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=defs/fields.tdl" "output=${INCLUDE_OUTPUT}/system/fields.h" "sdk=${PROJECT_SOURCE_DIR}"
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=defs/core.tdl" "sdk=${PROJECT_SOURCE_DIR}" "output-proto=prototypes.h" "output-defs=data_functions.c"
      COMMAND ${ORIGO_CMD} ${IDL_COMPILE_SCRIPT} "--log-warning" "src=defs/core.tdl" "output=${CMAKE_CURRENT_SOURCE_DIR}/idl.h" "sdk=${PROJECT_SOURCE_DIR}" "format=c"
      COMMAND ${CMAKE_COMMAND} -E touch "${CORE_STAMP}"
      DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/defs/core.tdl"
         "${CMAKE_CURRENT_SOURCE_DIR}/defs/errors.tdl"
         "${CMAKE_CURRENT_SOURCE_DIR}/defs/registry.tdl"
         "${CMAKE_CURRENT_SOURCE_DIR}/defs/fields.tdl"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/idl/common.tdl"
         ${FUNCTIONS}
         ${CORE_CLASSES}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Producing definition files for the Core library"
      VERBATIM)

   add_custom_target (core_defs DEPENDS "${CORE_STAMP}")

   register_generated_header_target (core_defs)
endif ()

# Config class

idl_gen ("defs/config.tdl"
   NAME config_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/config.h"
   BYPRODUCTS
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_config_def.c"
      "${DOCS}/modules/classes/config.xml"
   DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/common.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_config.cpp")

# Script class

idl_gen ("defs/script.tdl"
   NAME script_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/script.h"
   BYPRODUCTS
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_script_def.c"
      "${DOCS}/modules/classes/script.xml"
   DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/common.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_script.cpp")

# Compression & CompressedStream class

idl_gen ("defs/compression.tdl"
   NAME compression_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/compression.h"
   BYPRODUCTS
      "${CMAKE_CURRENT_SOURCE_DIR}/compression/class_compressed_stream_def.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/compression/class_compression_def.c"
      "${DOCS}/modules/classes/compression.xml"
      "${DOCS}/modules/classes/compressedstream.xml"
   DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/common.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/compression/class_compression.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/compression/class_compressed_stream.cpp")

# File class

idl_gen ("defs/filesystem.tdl"
   NAME filesystem_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/filesystem.h"
   BYPRODUCTS
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_file_def.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_storagedevice_def.c"
      "${DOCS}/modules/classes/file.xml"
      "${DOCS}/modules/classes/storagedevice.xml"
   DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/common.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_file.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_storagedevice.cpp")

# Process classes

idl_gen ("defs/processes.tdl"
   NAME process_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/processes.h"
   BYPRODUCTS
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_task_def.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_thread_def.c"
      "${DOCS}/modules/classes/thread.xml"
      "${DOCS}/modules/classes/task.xml"
   DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/common.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_task.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_thread.cpp")

if (NOT MSVC)
   # A bug in (MinGW?) GCC requires the following flag is applied to ensure that __packed__ works correctly.
   set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mno-ms-bitfields")
   set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mno-ms-bitfields")
   #target_compile_options (core PRIVATE "$<${GCC_COMPAT}:$<BUILD_INTERFACE:-mno-ms-bitfields>>")
endif ()

# DBG_DIAGNOSTICS: Print enhanced diagnostic messages on crash - e.g. semaphore and memory locks.
# DBG_SLEEP: Debug any area of the system that puts the process to sleep.

add_library (core)

target_compile_definitions (core PRIVATE "__system__" "DBG_DIAGNOSTICS" "PRV_METACLASS" "RESOURCE_POOL")

if (MSVC)
   target_compile_definitions (core PRIVATE "_CRT_NONSTDC_NO_DEPRECATE")
   target_compile_options (core PRIVATE /MP) # Enable multi-processor compilation
endif ()

# These definitions tell the Core where the system files are installed.

if (RUN_ANYWHERE)
   target_compile_definitions (core PRIVATE "_ROOT_PATH=" "_SYSTEM_PATH=" "_MODULE_PATH=\"kotuku:lib/\"")
else ()
   target_compile_definitions (core PRIVATE "_ROOT_PATH=\"${CMAKE_INSTALL_PREFIX}/\""
      "_SYSTEM_PATH=\"${CMAKE_INSTALL_PREFIX}/share/kotuku/\""
      "_MODULE_PATH=\"${CMAKE_INSTALL_PREFIX}/lib/kotuku/\"")
endif ()

set_module_defaults (core "Sys")

target_sources (core PRIVATE
   "core.cpp"
   "data.cpp"
   "data_actionlist.cpp"
   "internal.cpp"
   "lib_objects.cpp"
   "lib_events.cpp"
   "lib_log.cpp"
   "lib_fields_read.cpp"
   "lib_fields_write.cpp"
   "lib_filesystem.cpp"
   "lib_functions.cpp"
   "lib_locking.cpp"
   "lib_memory.cpp"
   "lib_messages.cpp"
   "classes/class_config.cpp"
   "classes/class_file.cpp"
   "classes/class_metaclass.cpp"
   "classes/class_module.cpp"
   "classes/class_script.cpp"
   "classes/class_storagedevice.cpp"
   "classes/class_task.cpp"
   "classes/class_thread.cpp"
   "classes/class_time.cpp"
   "compression/class_compression.cpp")

target_include_directories (core PRIVATE
   ${ZLIB_HEADERS})

target_link_libraries (core PRIVATE zlib)


if (WIN32)
   target_sources (core PRIVATE microsoft/windows.cpp)
   target_link_libraries (core PRIVATE wsock32 psapi advapi32 version)
   if (KOTUKU_VLOG OR MSVC)
      # Header generation for libdbghelp: dlltool -k -d dbghelp.def -l libdbghelp.a
      # Copy libdbghelp.a to your MinGW libs folder.
      target_link_libraries (core PRIVATE dbghelp)
   endif ()
elseif (ANDROID)
   target_sources (core PRIVATE classes/class_assets.cpp)
   target_link_libraries (core PRIVATE android pthread dl)
else ()
   target_link_libraries (core PRIVATE pthread dl)
endif ()

flute_test (core_classes "tests/test_classes.tiri")
flute_test (core_filesystem "tests/test_filesystem.tiri")
flute_test (core_misc "tests/test_misc.tiri")
flute_test (core_compression "tests/test_compression.tiri")
flute_test (core_config "tests/test_config.tiri")
flute_test (core_messages "tests/test_messages.tiri")
flute_test (core_time "tests/test_time.tiri")

if (INSTALL_TESTS)
   # Windows-compatible tests
   add_executable (test_pf_vector "tests/pf_vector.cpp")
   add_executable (test_wildcmp "tests/wildcmp.cpp")

   set_target_properties (test_pf_vector test_wildcmp PROPERTIES CXX_STANDARD 20 LINK_FLAGS_RELEASE "-s")
   install (TARGETS test_pf_vector test_wildcmp DESTINATION "tests/")

   # Unix-only tests
   if (NOT WIN32)
      add_executable (test_memlocking "tests/test_memory_locking.cpp")

      target_link_libraries (test_memlocking PRIVATE ${INIT_LINK})

      set_target_properties (test_memlocking PROPERTIES CXX_STANDARD 20 LINK_FLAGS_RELEASE "-s")
      install (TARGETS test_memlocking DESTINATION "tests/")
   endif ()
endif ()
