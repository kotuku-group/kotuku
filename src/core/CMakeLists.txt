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

SET (CORE_DEFS
   "${CMAKE_CURRENT_SOURCE_DIR}/compression/class_compressed_stream_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_config_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_file_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_script_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_task_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/classes/class_thread_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/compression/class_compression_def.c")

if (BUILD_DEFS)
   add_custom_command (
      OUTPUT
         ${CORE_DEFS}
         "${DOCS}/modules/classes/compressedstream.xml"
         "${DOCS}/modules/classes/compression.xml"
         "${DOCS}/modules/classes/config.xml"
         "${DOCS}/modules/classes/file.xml"
         "${DOCS}/modules/classes/metaclass.xml"
         "${DOCS}/modules/classes/module.xml"
         "${DOCS}/modules/classes/script.xml"
         "${DOCS}/modules/classes/storagedevice.xml"
         "${DOCS}/modules/classes/task.xml"
         "${DOCS}/modules/classes/thread.xml"
         "${DOCS}/modules/classes/time.xml"
         "${INCLUDE_OUTPUT}/modules/core.h"
         "${INCLUDE_OUTPUT}/system/errors.h"
         "${INCLUDE_OUTPUT}/system/registry.h"
         "${INCLUDE_OUTPUT}/system/fields.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/idl.h"
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=defs/errors.tdl" "output=${INCLUDE_OUTPUT}/system/errors.h" "sdk=${PROJECT_SOURCE_DIR}"
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=defs/registry.tdl" "output=${INCLUDE_OUTPUT}/system/registry.h" "sdk=${PROJECT_SOURCE_DIR}"
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=defs/fields.tdl" "output=${INCLUDE_OUTPUT}/system/fields.h" "sdk=${PROJECT_SOURCE_DIR}"
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=defs/core.tdl" "sdk=${PROJECT_SOURCE_DIR}" "output-proto=prototypes.h" "output-defs=data_functions.c" "files={" ${FUNCTIONS} "}"
      COMMAND ${ORIGO_CMD} ${IDL_COMPILE_SCRIPT} "--log-warning" "src=defs/core.tdl" "output=${CMAKE_CURRENT_SOURCE_DIR}/idl.h" "sdk=${PROJECT_SOURCE_DIR}" "format=c"
      DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/defs/core.tdl"
         "${CMAKE_CURRENT_SOURCE_DIR}/defs/errors.tdl"
         "${CMAKE_CURRENT_SOURCE_DIR}/defs/registry.tdl"
         "${CMAKE_CURRENT_SOURCE_DIR}/defs/fields.tdl"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/idl/common.tdl"
         ${FUNCTIONS}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Producing definition files for the Core library"
      VERBATIM)

   add_custom_target (core_defs DEPENDS ${CORE_DEFS}
      "${INCLUDE_OUTPUT}/modules/core.h"
      "${INCLUDE_OUTPUT}/system/errors.h"
      "${INCLUDE_OUTPUT}/system/registry.h"
      "${INCLUDE_OUTPUT}/system/fields.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/idl.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/core.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/errors.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/registry.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/defs/fields.tdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/idl/common.tdl")

   add_dependencies (build_headers core_defs)
endif ()

if (NOT MSVC)
   # A bug in (MinGW?) GCC requires the following flag is applied to ensure that __packed__ works correctly.
   set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mno-ms-bitfields")
   set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mno-ms-bitfields")
   #target_compile_options (core PRIVATE "$<${GCC_COMPAT}:$<BUILD_INTERFACE:-mno-ms-bitfields>>")
endif ()

# DBG_DIAGNOSTICS: Print enhanced diagnostic messages on crash - e.g. semaphore and memory locks.
# DBG_SLEEP: Debug any area of the system that puts the process to sleep.

add_library (core)

target_compile_definitions (core PRIVATE "__system__" "DBG_DIAGNOSTICS" "PRV_METACLASS")

if (MSVC)
   target_compile_definitions (core PRIVATE "_CRT_NONSTDC_NO_DEPRECATE")
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
   target_link_libraries (core PRIVATE wsock32 psapi)
   if (KOTUKU_VLOG OR MSVC)
      # Header generation for libdbghelp: dlltool -k -d dbghelp.def -l libdbghelp.a
      # Copy libdbghelp.a to your MinGW libs folder.
      target_link_libraries (core PRIVATE dbghelp)
   endif ()
else ()
   target_link_libraries (core PRIVATE pthread dl)
endif ()

flute_test (core_filesystem "tests/filesystem.tiri")
flute_test (core_misc "tests/misc.tiri")
flute_test (core_compression "tests/compression.tiri")
flute_test (core_config "tests/config.tiri")
flute_test (core_time "tests/test_time.tiri")

if (INSTALL_TESTS)
   # Windows-compatible tests
   add_executable (test_pf_vector "tests/pf_vector.cpp")
   add_executable (test_wildcmp "tests/wildcmp.cpp")

   set_target_properties (test_pf_vector test_wildcmp PROPERTIES CXX_STANDARD 20 LINK_FLAGS_RELEASE "-s")
   install (TARGETS test_pf_vector test_wildcmp DESTINATION "tests/")

   # Unix-only tests (require pthread)
   if (NOT WIN32)
      add_executable (test_objlocking "tests/object_locking.cpp")
      add_executable (test_memlocking "tests/memory_locking.cpp")

      target_link_libraries (test_objlocking PRIVATE ${INIT_LINK})
      target_link_libraries (test_memlocking PRIVATE ${INIT_LINK})

      set_target_properties (test_memlocking test_objlocking PROPERTIES CXX_STANDARD 20 LINK_FLAGS_RELEASE "-s")
      install (TARGETS test_objlocking test_memlocking DESTINATION "tests/")
   endif ()
endif ()
