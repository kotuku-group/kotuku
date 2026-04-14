# This module integrates LuaJIT 2.1 with Kōtuku to provide the Tiri scripting language. The build
# process generates multiple header files through a multi-stage compilation process.
#
# SPECIAL FEATURES:
#  Enable LUAJIT_USE_SYSMALLOC temporarily if you need to figure out memory management and overflow issues.
#  Enable LUAJIT_USE_GDBJIT temporarily if debugging with GDB.
#  Enable ENABLE_UNIT_TESTS to include unit tests for non-release builds.  You will need to run test_unit_tests.tiri with
#    the --log-api option to view any output.
#
# GENERATED HEADERS OVERVIEW:
#
# From TDL files (Kōtuku Interface Definition Language):
#   hashes.h                      - Hash definitions for Tiri internal use
#   include/kotuku/modules/tiri.h - Public API header for Tiri module
#
# From LuaJIT build process:
#   lj_bcdef.h     - Bytecode definitions (opcode metadata)
#   lj_ffdef.h     - Fast function definitions (C function bindings)
#   lj_libdef.h    - Library definitions (standard library metadata)
#   lj_recdef.h    - Recorder definitions (trace compiler metadata)
#   lj_folddef.h   - Folding definitions (constant folding rules)
#   buildvm_arch.h - Architecture-specific VM definitions
#   jit/vmdef.lua  - VM definitions for JIT runtime
#
# JIT BUILD STAGES:
#
# 1. Host Tool Compilation (tools that run during build):
#    - minilua: Minimal Lua interpreter for running dynasm
#    - buildvm: Code generator that produces headers and VM code
#
# 2. Architecture Detection:
#    - Preprocesses lj_arch.h to detect target architecture
#    - Determines platform-specific flags for dynasm
#
# 3. Code Generation:
#    - dynasm generates buildvm_arch.h from vm_*.dasc files
#    - buildvm generates bytecode/library/recorder definitions
#    - buildvm generates VM assembly or object code
#
# 4. Library Compilation:
#    - Links with generated VM code to create static library

set (MOD "tiri")
set (INC_MOD_TIRI TRUE PARENT_SCOPE)

# Enable unit tests for deep testing of non-release builds only.
# set (ENABLE_UNIT_TESTS ON)

# IDL processing for headers and documentation

idl_gen ("hashes.tdl" NAME ${MOD}_hashes OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/hashes.h")

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   APPEND_IDL "module_def.cpp"
   ARGS "output-defs=module_def.cpp"
   FILES "${MOD}.cpp")

# We build libFFI as a release build in all situations because ASAN doesn't like Debug builds of libFFI.

if (MSVC)
   set (FFI_LINK "${CMAKE_BINARY_DIR}/libffi-3.3/lib/libffi.lib")
else ()
   set (FFI_LINK "${CMAKE_BINARY_DIR}/libffi-3.3/lib/liblibffi.a")
endif ()

ExternalProject_Add(libffi-3.3
   SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libffi-3.3
   CMAKE_ARGS
      ${GLOBAL_DEFAULT_ARGS}
      ${GLOBAL_THIRDPARTY_LIB_ARGS}
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/libffi-3.3
   INSTALL_DIR ${CMAKE_BINARY_DIR}/libffi-3.3
   BUILD_BYPRODUCTS ${FFI_LINK}
   COMMENT "Compiling FFI library")

# JIT library custom build

set (JIT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/jit/src")

# Define build directory for generated files (avoids polluting source tree)

set (JIT_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/jitlib-generated")
file(MAKE_DIRECTORY "${JIT_BUILD_DIR}")
file(MAKE_DIRECTORY "${JIT_BUILD_DIR}/jit")

# Generated header files

set (JIT_GENERATED_HEADERS
   "${JIT_BUILD_DIR}/lj_bcdef.h"
   "${JIT_BUILD_DIR}/lj_ffdef.h"
   "${JIT_BUILD_DIR}/lj_libdef.h"
   "${JIT_BUILD_DIR}/lj_recdef.h"
   "${JIT_BUILD_DIR}/lj_folddef.h"
)

set (JIT_VMDEF_LUA "${JIT_BUILD_DIR}/jit/vmdef.lua")
set (JIT_COMMON_DEFS PRV_METACLASS)
set (JIT_NON_MSVC_DEFS -DLUAJIT_DISABLE_BUFFER -DPRV_METACLASS)

option(KOTUKU_PARSER_TRACE "Emit parser token and expectation traces" OFF)
option(INCLUDE_TIPS "Include the parser tips system for code analysis" ON)

if (KOTUKU_PARSER_TRACE)
   list(APPEND JIT_COMMON_DEFS KOTUKU_PARSER_TRACE)
   list(APPEND JIT_NON_MSVC_DEFS -DKOTUKU_PARSER_TRACE)
endif ()

if (INCLUDE_TIPS)
   list(APPEND JIT_COMMON_DEFS INCLUDE_TIPS)
   list(APPEND JIT_NON_MSVC_DEFS -DINCLUDE_TIPS)
endif ()

if (ENABLE_UNIT_TESTS)
   list(APPEND JIT_COMMON_DEFS ENABLE_UNIT_TESTS)
   list(APPEND JIT_NON_MSVC_DEFS -DENABLE_UNIT_TESTS)
endif ()

# LUA_USE_ASSERT enables runtime assertions for early detection of VM state issues.
# This catches problems like invalid stack state (L->top < L->base) in gc_traverse_frames.
# See VMHelperGuard in stack_helpers.h for context on the issues this helps catch.
#
# IMPORTANT: LUA_USE_ASSERT is NOT added to JIT_COMMON_DEFS or JIT_NON_MSVC_DEFS
# because host tools (minilua, buildvm) use those lists but don't link with the runtime
# that provides lj_assert_fail(). Instead, we use:
#   - Generator expressions for MSVC multi-config builds (in target_compile_definitions)
#   - JIT_LIBRARY_DEFS for non-MSVC single-config builds (in custom commands)

set(JIT_LIBRARY_DEFS "")
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
   set(JIT_LIBRARY_DEFS -DLUA_USE_ASSERT)
endif ()

if (WIN32)
   # Convert paths to forward slashes for safe use in C string literals.
   file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" LJ_PDB_BUILD_PATH)
   file(TO_CMAKE_PATH "${CMAKE_INSTALL_PREFIX}" LJ_PDB_INSTALL_PATH)
endif ()

# Common host tool sources (used by both MSVC and non-MSVC builds)
set (JIT_HOST_DIR "${JIT_SRC}/host")
set (JIT_DYNASM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/jit/dynasm")
set (MINILUA_SRC "${JIT_HOST_DIR}/minilua.cpp")

# Library source files needed for buildvm header generation (used in both build paths)

set (LJLIB_C
   "${JIT_SRC}/lib/lib_base.cpp"
   "${JIT_SRC}/lib/lib_math.cpp"
   "${JIT_SRC}/lib/lib_bit.cpp"
   "${JIT_SRC}/lib/lib_string.cpp"
   "${JIT_SRC}/lib/lib_table.cpp"
   "${JIT_SRC}/lib/lib_range.cpp"
   "${JIT_SRC}/lib/lib_array.cpp"
   "${JIT_SRC}/lib/lib_object.cpp"
   "${JIT_SRC}/lib/lib_debug.cpp"
   "${JIT_SRC}/lib/lib_jit.cpp"
)

# Buildvm source files (used in both build paths)
set (BUILDVM_SOURCES
   "${JIT_HOST_DIR}/buildvm.cpp"
   "${JIT_HOST_DIR}/buildvm_asm.cpp"
   "${JIT_HOST_DIR}/buildvm_peobj.cpp"
   "${JIT_HOST_DIR}/buildvm_lib.cpp"
   "${JIT_HOST_DIR}/buildvm_fold.cpp"
)

# Common compiler flags and paths (use lists instead of strings for proper handling)
set (JIT_HOST_CFLAGS -O2 -Wall)
set (JIT_INCLUDE_DIRS -I${PROJECT_SOURCE_DIR}/include -I${CMAKE_BINARY_DIR} -I${JIT_SRC} -I${JIT_BUILD_DIR} -I${JIT_SRC}/bytecode -I${JIT_SRC}/debug -I${JIT_SRC}/jit -I${JIT_SRC}/lib -I${JIT_SRC}/parser -I${JIT_SRC}/runtime)
set (JIT_COMMON_CXXFLAGS -fPIC -O2 -fomit-frame-pointer -Wall -std=c++20 -Wno-invalid-offsetof)

# Platform-specific math library (empty on Windows, -lm on Unix)
if (WIN32)
   set(JIT_MATH_LIB "")
else ()
   set(JIT_MATH_LIB "-lm")
endif ()

if (MSVC)
   # JIT build for MSVC - separated into code generation and library compilation
   # Code generation creates headers and lj_vm.obj via custom batch script

   set (JIT_VM_OBJ "${JIT_BUILD_DIR}/lj_vm.obj")

   # Expand glob patterns for dependencies (only source headers, not generated ones)

   file(GLOB JIT_CORE_SOURCES
      "${JIT_SRC}/debug/lj_*.cpp"
      "${JIT_SRC}/debug/try_except.cpp"
      "${JIT_SRC}/runtime/lj_*.cpp"
      "${JIT_SRC}/lj_*.cpp")

   file(GLOB JIT_LIB_SOURCES "${JIT_SRC}/lib/lib_*.cpp")

   # Exclude library and parser files that are compiled into the Tiri module instead
   list(FILTER JIT_LIB_SOURCES EXCLUDE REGEX "lib_.*\\.cpp$")
   list(FILTER JIT_CORE_SOURCES EXCLUDE REGEX "lib\\.cpp$|load\\.cpp$|lj_parse\\.cpp$|lj_lex\\.cpp$|parser_unit_tests\\.cpp$")

   file(GLOB JIT_DEP_LJ_H
      "${JIT_SRC}/bytecode/lj_*.h"
      "${JIT_SRC}/debug/lj_*.h"
      "${JIT_SRC}/runtime/lj_*.h"
      "${JIT_SRC}/lj_*.h")

   # Platform-specific paths
   set (MINILUA_TARGET "${JIT_BUILD_DIR}/minilua.exe")
   set (BUILDVM_TARGET "${JIT_BUILD_DIR}/buildvm.exe")
   set (BUILDVM_ARCH_H "${JIT_BUILD_DIR}/buildvm_arch.h")

   # Enforce 64-bit only architecture
   if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
      message(FATAL_ERROR "Tiri requires a 64-bit build environment. 32-bit support has been removed.")
   endif()

   set (JIT_DASC "${JIT_SRC}/jit/vm_x64.dasc")
   set (JIT_DASMFLAGS -D WIN -D JIT)
   set (JIT_ARCH "x64")

   # Build minilua host tool
   add_custom_command(
      OUTPUT ${MINILUA_TARGET}
      COMMAND ${CMAKE_CXX_COMPILER} /nologo /c /W3 /D_CRT_SECURE_NO_DEPRECATE /std:c++20
         /Fo${JIT_BUILD_DIR}/minilua.obj
         ${MINILUA_SRC}
      COMMAND ${CMAKE_LINKER} /nologo /out:${MINILUA_TARGET}
         ${JIT_BUILD_DIR}/minilua.obj
      DEPENDS ${MINILUA_SRC}
      WORKING_DIRECTORY "${JIT_SRC}"
      COMMENT "Building minilua host tool for ${JIT_ARCH}"
      VERBATIM
   )

   # Generate buildvm_arch.h using dynasm
   add_custom_command(
      OUTPUT ${BUILDVM_ARCH_H}
      COMMAND ${MINILUA_TARGET} ${JIT_DYNASM_DIR}/dynasm.lua -LN ${JIT_DASMFLAGS} -o ${BUILDVM_ARCH_H} ${JIT_DASC}
      DEPENDS ${MINILUA_TARGET} "${JIT_DYNASM_DIR}/dynasm.lua" ${JIT_DASC} "${JIT_SRC}/lj_arch.h"
      WORKING_DIRECTORY "${JIT_SRC}"
      COMMENT "Generating buildvm_arch.h via dynasm for ${JIT_ARCH}"
      VERBATIM
   )

   # Build buildvm host tool
   # Note: lj_obj.h contains MMDEF which defines metamethod tables used by buildvm
   add_custom_command(
      OUTPUT ${BUILDVM_TARGET}
      COMMAND ${CMAKE_CXX_COMPILER} /nologo /c /W3 /std:c++20
         /D_CRT_SECURE_NO_DEPRECATE
         /I${JIT_SRC} /I${JIT_DYNASM_DIR} /I${JIT_BUILD_DIR} /I${JIT_SRC}/bytecode
         /I${JIT_SRC}/debug /I${JIT_SRC}/lib /I${JIT_SRC}/parser /I${JIT_SRC}/runtime
         /I${PROJECT_SOURCE_DIR}/include /I${CMAKE_BINARY_DIR}
         ${BUILDVM_SOURCES}
      COMMAND ${CMAKE_LINKER} /nologo /out:${BUILDVM_TARGET}
         ${JIT_BUILD_DIR}/buildvm.obj
         ${JIT_BUILD_DIR}/buildvm_asm.obj
         ${JIT_BUILD_DIR}/buildvm_peobj.obj
         ${JIT_BUILD_DIR}/buildvm_lib.obj
         ${JIT_BUILD_DIR}/buildvm_fold.obj
      DEPENDS ${BUILDVM_SOURCES} ${BUILDVM_ARCH_H} "${JIT_SRC}/runtime/lj_obj.h"
      WORKING_DIRECTORY "${JIT_BUILD_DIR}"
      COMMENT "Building buildvm host tool for ${JIT_ARCH}"
      VERBATIM
   )

   # Generate headers and VM object file using buildvm
   # lj_obj.h dependency ensures VM is regenerated when metamethod table changes
   add_custom_command(
      OUTPUT ${JIT_GENERATED_HEADERS} ${JIT_VM_OBJ} ${JIT_VMDEF_LUA}
      COMMAND ${BUILDVM_TARGET} -m peobj -o ${JIT_VM_OBJ}
      COMMAND ${BUILDVM_TARGET} -m bcdef -o ${JIT_BUILD_DIR}/lj_bcdef.h ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m ffdef -o ${JIT_BUILD_DIR}/lj_ffdef.h ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m libdef -o ${JIT_BUILD_DIR}/lj_libdef.h ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m recdef -o ${JIT_BUILD_DIR}/lj_recdef.h ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m folddef -o ${JIT_BUILD_DIR}/lj_folddef.h ${JIT_SRC}/lj_opt_fold.cpp
      COMMAND ${BUILDVM_TARGET} -m vmdef -o ${JIT_VMDEF_LUA} ${LJLIB_C}
      DEPENDS ${BUILDVM_TARGET} ${LJLIB_C} "${JIT_SRC}/lj_opt_fold.cpp" "${JIT_SRC}/runtime/lj_obj.h"
      WORKING_DIRECTORY "${JIT_SRC}"
      COMMENT "Generating JIT headers and VM object for ${JIT_ARCH}"
      VERBATIM
   )

   add_custom_target(jit_codegen DEPENDS ${JIT_GENERATED_HEADERS} ${JIT_VM_OBJ})

   # Create JIT static library
   add_library(jit_lib STATIC ${JIT_CORE_SOURCES} ${JIT_LIB_SOURCES} ${JIT_VM_OBJ})

   add_dependencies(jit_lib jit_codegen)

   set_target_properties(jit_lib PROPERTIES
      OUTPUT_NAME "lua51"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/jit/lib"
   )
   # Convert paths to forward slashes for safe use in C string literals
   file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" LJ_PDB_BUILD_PATH)
   file(TO_CMAKE_PATH "${CMAKE_INSTALL_PREFIX}" LJ_PDB_INSTALL_PATH)

   target_compile_definitions(jit_lib PRIVATE
      ${JIT_COMMON_DEFS}
      _CRT_SECURE_NO_DEPRECATE
      $<$<CONFIG:Debug>:LUA_USE_ASSERT>
      LJ_PDB_BUILD_DIR="${LJ_PDB_BUILD_PATH}"
      LJ_PDB_INSTALL_DIR="${LJ_PDB_INSTALL_PATH}")

   target_include_directories(jit_lib PRIVATE
      "${JIT_SRC}"
      "${JIT_SRC}/bytecode"
      "${JIT_SRC}/debug"
      "${JIT_SRC}/jit"
      "${JIT_SRC}/lib"
      "${JIT_SRC}/parser"
      "${JIT_SRC}/runtime"
      "${JIT_BUILD_DIR}"  # Include generated headers from build dir
   )

   # Disable specific warnings from JIT and disable GS security checks for performance
   # /MP enables multi-processor compilation for parallel builds within this target
   target_compile_options(jit_lib PRIVATE /MP /wd4244 /wd5287 /GS-)

   # Use generator expression for configuration-specific paths in MSVC multi-config builds
   # Check if using a multi-config generator (Visual Studio) vs single-config (Ninja)
   get_property(MULTICONFIG_GENERATOR GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
   if (MULTICONFIG_GENERATOR)
      set (JIT_LINK "${CMAKE_BINARY_DIR}/jit/lib/$<CONFIG>/lua51.lib")
   else()
      set (JIT_LINK "${CMAKE_BINARY_DIR}/jit/lib/lua51.lib")
   endif()

   # Create INTERFACE library to encapsulate JIT usage requirements
   add_library(jit_interface INTERFACE)
   target_include_directories(jit_interface INTERFACE "${JIT_SRC}")
   target_compile_definitions(jit_interface INTERFACE ${JIT_COMMON_DEFS})

   # Create an alias for backward compatibility
   add_custom_target(jitlib DEPENDS jit_lib)

else ()
   # Non-MSVC build (Linux, MinGW, etc.) - JIT build using CMake
   # Separated into code generation and library compilation
   # On Windows (MinGW), use peobj like MSVC; on Unix, use assembly
   # Library compilation is handled by CMake for proper incremental builds

   if (WIN32)
      # MinGW uses PE object format like MSVC
      set (JIT_VM_OBJ "${JIT_BUILD_DIR}/lj_vm.obj")
   else ()
      # Unix platforms use assembly source
      set (JIT_VM_S "${JIT_BUILD_DIR}/lj_vm.S")
      set (JIT_VM_O "${JIT_BUILD_DIR}/lj_vm.o")
   endif ()

   # Platform-specific paths
   set (MINILUA_TARGET "${JIT_BUILD_DIR}/minilua${CMAKE_EXECUTABLE_SUFFIX}")
   set (BUILDVM_TARGET "${JIT_BUILD_DIR}/buildvm${CMAKE_EXECUTABLE_SUFFIX}")
   set (BUILDVM_ARCH_H "${JIT_BUILD_DIR}/buildvm_arch.h")

   # Build minilua host tool first

   add_custom_command(
      OUTPUT ${MINILUA_TARGET}
      COMMAND ${CMAKE_CXX_COMPILER} ${JIT_HOST_CFLAGS} -o ${MINILUA_TARGET} ${MINILUA_SRC} ${JIT_MATH_LIB}
      DEPENDS ${MINILUA_SRC}
      COMMENT "Building minilua host tool"
      VERBATIM
   )

   # Enforce 64-bit only architecture
   if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
      message(FATAL_ERROR "Tiri requires a 64-bit build environment. 32-bit support has been removed.")
   endif()

   # Determine target architecture using CMake built-in variables
   # Normalize to lowercase for case-insensitive matching (Windows/cross-toolchains use uppercase)

   string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" JIT_SYSTEM_PROCESSOR_LOWER)

   if (JIT_SYSTEM_PROCESSOR_LOWER MATCHES "^(aarch64|arm64|arm)")
      set (JIT_TARGET_ARCH "arm64")
   elseif (JIT_SYSTEM_PROCESSOR_LOWER MATCHES "^(powerpc|ppc)")
      set (JIT_TARGET_ARCH "ppc")
   else()
      # Default to x64 for other architectures (all now require 64-bit)
      set (JIT_TARGET_ARCH "x64")
   endif ()

   # Generate buildvm_arch.h using dynasm
   # Use the detected architecture to select the appropriate .dasc file
   # Extract architecture flags from lj_arch.h preprocessing to pass to dynasm

   set (DYNASM_SCRIPT "${JIT_BUILD_DIR}/run_dynasm.cmake")

   # Create test_arch.c for preprocessing
   if (NOT EXISTS "${JIT_BUILD_DIR}/test_arch.c")
      file(WRITE "${JIT_BUILD_DIR}/test_arch.c" "#include <lj_arch.h>\n")
   endif ()

   # Write the dynasm runner script

   file(WRITE ${DYNASM_SCRIPT}
         "# Use architecture determined by CMake\n"
         "set(ARCH \"${JIT_TARGET_ARCH}\")\n"
         "set(DASM_DASC \"${JIT_SRC}/jit/vm_\${ARCH}.dasc\")\n"
         "if(NOT EXISTS \"\${DASM_DASC}\")\n"
         "   message(FATAL_ERROR \"Architecture file \${DASM_DASC} not found\")\n"
         "endif()\n"
         "# Detect architecture flags by preprocessing lj_arch.h\n"
         "execute_process(\n"
         "   COMMAND ${CMAKE_CXX_COMPILER} ${JIT_NON_MSVC_DEFS} -I${JIT_SRC} -E -dM ${JIT_BUILD_DIR}/test_arch.c\n"
         "   WORKING_DIRECTORY ${JIT_SRC}\n"
         "   OUTPUT_VARIABLE ARCH_DEFS\n"
         "   OUTPUT_STRIP_TRAILING_WHITESPACE\n"
         "   ERROR_QUIET\n"
         ")\n"
         "# Build DASM_AFLAGS based on detected defines\n"
         "set(DASM_AFLAGS \"\")\n"
         "if(ARCH_DEFS MATCHES \"LJ_LE 1\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"ENDIAN_LE\")\n"
         "else()\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"ENDIAN_BE\")\n"
         "endif()\n"
         "list(APPEND DASM_AFLAGS \"-D\" \"JIT\")\n"
         "if(ARCH_DEFS MATCHES \"LJ_DUALNUM 1\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"DUALNUM\")\n"
         "endif()\n"
         "if(ARCH_DEFS MATCHES \"LJ_ARCH_HASFPU 1\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"FPU\")\n"
         "endif()\n"
         "if(NOT ARCH_DEFS MATCHES \"LJ_ABI_SOFTFP 1\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"HFABI\")\n"
         "endif()\n"
         "if(ARCH_DEFS MATCHES \"LJ_NO_UNWIND 1\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"NO_UNWIND\")\n"
         "endif()\n"
         "if(WIN32)\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"WIN\")\n"
         "endif()\n"
         "if(ARCH_DEFS MATCHES \"LJ_ARCH_VERSION ([0-9]+)\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"VER=\${CMAKE_MATCH_1}\")\n"
         "endif()\n"
         "# Platform-specific flags\n"
         "# iOS flag for ARM platforms\n"
         "if(ARCH_DEFS MATCHES \"__APPLE__\" AND ARCH_DEFS MATCHES \"LJ_ARCH_ARM\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"IOS\")\n"
         "endif()\n"
         "# SQRT, ROUND, and GPR64 flags for PowerPC\n"
         "if(ARCH_DEFS MATCHES \"LJ_ARCH_PPC 1\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"SQRT\")\n"
         "   list(APPEND DASM_AFLAGS \"-D\" \"ROUND\")\n"
         "   if(ARCH_DEFS MATCHES \"LJ_ARCH_PPC64 1\")\n"
         "      list(APPEND DASM_AFLAGS \"-D\" \"GPR64\")\n"
         "   endif()\n"
         "endif()\n"
         "# Run dynasm\n"
         "execute_process(\n"
         "   COMMAND \"${MINILUA_TARGET}\" \"${JIT_DYNASM_DIR}/dynasm.lua\" \${DASM_AFLAGS} -o \"${BUILDVM_ARCH_H}\" \"\${DASM_DASC}\"\n"
         "   WORKING_DIRECTORY \"${JIT_SRC}\"\n"
         "   RESULT_VARIABLE DYNASM_RESULT\n"
         "   OUTPUT_QUIET ERROR_QUIET\n"
         ")\n"
         "if(DYNASM_RESULT)\n"
         "   message(FATAL_ERROR \"Dynasm failed with exit code \${DYNASM_RESULT}\")\n"
         "endif()\n"
      )

   add_custom_command(
      OUTPUT ${BUILDVM_ARCH_H}
      COMMAND ${CMAKE_COMMAND} -E env TARGET_GENDIR=${JIT_BUILD_DIR} ${CMAKE_COMMAND} -P ${DYNASM_SCRIPT}
      DEPENDS ${MINILUA_TARGET} ${JIT_DYNASM_DIR}/dynasm.lua ${DYNASM_SCRIPT}
         "${JIT_SRC}/lj_arch.h" "${JIT_SRC}/lua.h" "${JIT_SRC}/luaconf.h"
         "${JIT_SRC}/jit/vm_x64.dasc" "${JIT_SRC}/jit/vm_arm64.dasc" "${JIT_SRC}/jit/vm_ppc.dasc" "${JIT_BUILD_DIR}/test_arch.c"
      COMMENT "Generating buildvm_arch.h via dynasm"
      VERBATIM
   )

   # Build buildvm host tool
   # Note: lj_obj.h contains MMDEF which defines metamethod tables used by buildvm

   add_custom_command(
      OUTPUT ${BUILDVM_TARGET}
      COMMAND ${CMAKE_CXX_COMPILER} ${JIT_COMMON_CXXFLAGS}
         ${JIT_NON_MSVC_DEFS}
         ${JIT_INCLUDE_DIRS}
         -o ${BUILDVM_TARGET} ${BUILDVM_SOURCES} ${JIT_MATH_LIB}
      DEPENDS ${BUILDVM_SOURCES} ${BUILDVM_ARCH_H} "${JIT_SRC}/runtime/lj_obj.h"
      COMMENT "Building buildvm host tool"
      VERBATIM
   )

   # Generate headers and VM file using buildvm
   # Windows (MinGW) uses peobj to generate .obj directly like MSVC
   # Unix uses elfasm to generate .S assembly source that gets compiled later
   # lj_obj.h dependency ensures VM is regenerated when metamethod table changes

   # Set platform-specific VM generation parameters

   if (WIN32)
      # MinGW: Generate PE object file directly (no assembly step needed)
      set(JIT_VM_OUTPUT ${JIT_VM_OBJ})
      set(JIT_VM_MODE "peobj")
      set(JIT_VM_COMMENT "Generating JIT headers and VM object via buildvm")
   else ()
      # Unix: Generate assembly source for later compilation
      set(JIT_VM_OUTPUT ${JIT_VM_S})
      set(JIT_VM_MODE "elfasm")
      set(JIT_VM_COMMENT "Generating JIT headers and VM assembly via buildvm")
   endif ()

   add_custom_command(
      OUTPUT ${JIT_GENERATED_HEADERS} ${JIT_VM_OUTPUT} ${JIT_VMDEF_LUA}
      COMMAND ${BUILDVM_TARGET} -m bcdef -o ${JIT_BUILD_DIR}/lj_bcdef.h ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m ffdef -o ${JIT_BUILD_DIR}/lj_ffdef.h ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m libdef -o ${JIT_BUILD_DIR}/lj_libdef.h ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m recdef -o ${JIT_BUILD_DIR}/lj_recdef.h ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m folddef -o ${JIT_BUILD_DIR}/lj_folddef.h "${JIT_SRC}/lj_opt_fold.cpp"
      COMMAND ${BUILDVM_TARGET} -m vmdef -o ${JIT_VMDEF_LUA} ${LJLIB_C}
      COMMAND ${BUILDVM_TARGET} -m ${JIT_VM_MODE} -o ${JIT_VM_OUTPUT}
      DEPENDS ${BUILDVM_TARGET} ${LJLIB_C} "${JIT_SRC}/lj_opt_fold.cpp" "${JIT_SRC}/runtime/lj_obj.h"
      WORKING_DIRECTORY ${JIT_SRC}
      COMMENT ${JIT_VM_COMMENT}
      VERBATIM
   )

   if (WIN32)
      # MinGW: VM object file is already generated by buildvm, no compilation needed
      add_custom_target(jit_codegen
         DEPENDS ${JIT_GENERATED_HEADERS} ${JIT_VM_OBJ} ${JIT_VMDEF_LUA}
      )
   else ()
      # Unix: Generate assembly source (compilation happens in library target)
      add_custom_target(jit_codegen
         DEPENDS ${JIT_GENERATED_HEADERS} ${JIT_VM_S} ${JIT_VMDEF_LUA}
      )
   endif ()

   # Expand glob patterns for source files (mirrors MSVC approach)

   file(GLOB JIT_CORE_SOURCES
      "${JIT_SRC}/debug/lj_*.cpp"
      "${JIT_SRC}/debug/try_except.cpp"
      "${JIT_SRC}/runtime/lj_*.cpp"
      "${JIT_SRC}/lj_*.cpp")

   file(GLOB JIT_LIB_SOURCES "${JIT_SRC}/lib/lib_*.cpp")

   # Exclude library and parser files that are compiled into the Tiri module instead
   list(FILTER JIT_LIB_SOURCES EXCLUDE REGEX "lib_.*\\.cpp$")
   list(FILTER JIT_CORE_SOURCES EXCLUDE REGEX "lib\\.cpp$|load\\.cpp$|lj_parse\\.cpp$|lj_lex\\.cpp$|parser_unit_tests\\.cpp$")

   if (WIN32)
      list(APPEND JIT_LIBRARY_DEFS
         -DLJ_PDB_BUILD_DIR=\"${LJ_PDB_BUILD_PATH}\"
         -DLJ_PDB_INSTALL_DIR=\"${LJ_PDB_INSTALL_PATH}\")
   endif ()

   # Generate list of object files from source files
   set(JIT_OBJECT_FILES "")
   foreach(SOURCE_FILE ${JIT_CORE_SOURCES} ${JIT_LIB_SOURCES})
      get_filename_component(FILENAME ${SOURCE_FILE} NAME_WE)
      set(OBJECT_FILE "${JIT_BUILD_DIR}/${FILENAME}.o")
      list(APPEND JIT_OBJECT_FILES ${OBJECT_FILE})

      # Create custom command to compile each source file with explicit flags
      add_custom_command(
         OUTPUT ${OBJECT_FILE}
         COMMAND ${CMAKE_CXX_COMPILER} ${JIT_COMMON_CXXFLAGS}
            -funwind-tables -DLUAJIT_UNWIND_EXTERNAL
            ${JIT_NON_MSVC_DEFS}
            ${JIT_LIBRARY_DEFS}
            -DLUA_ROOT=\"${CMAKE_INSTALL_PREFIX}\"
            ${JIT_INCLUDE_DIRS}
            -Wno-trigraphs
            -c ${SOURCE_FILE} -o ${OBJECT_FILE}
         DEPENDS ${SOURCE_FILE} ${JIT_GENERATED_HEADERS} "${JIT_SRC}/lj_ircall.h"
         COMMENT "Compiling ${FILENAME}.cpp"
         VERBATIM
      )
   endforeach()

   if (WIN32)
      # MinGW: Create library from individual object files
      add_library(jit_lib STATIC)
      target_sources(jit_lib PRIVATE ${JIT_OBJECT_FILES} ${JIT_VM_OBJ})
      set_target_properties(jit_lib PROPERTIES LINKER_LANGUAGE C)
   else ()
      # Unix: Compile VM assembly with explicit flags, then create library

      # Compile VM assembly file
      add_custom_command(
         OUTPUT ${JIT_VM_O}
         COMMAND ${CMAKE_CXX_COMPILER} ${JIT_COMMON_CXXFLAGS}
            -funwind-tables -DLUAJIT_UNWIND_EXTERNAL
            ${JIT_NON_MSVC_DEFS}
            ${JIT_LIBRARY_DEFS}
            ${JIT_INCLUDE_DIRS}
            -c ${JIT_VM_S} -o ${JIT_VM_O}
         DEPENDS ${JIT_VM_S}
         COMMENT "Compiling JIT VM assembly"
         VERBATIM
      )

      # Create library from individual object files
      add_library(jit_lib STATIC)
      target_sources(jit_lib PRIVATE ${JIT_OBJECT_FILES} ${JIT_VM_O})
      set_target_properties(jit_lib PROPERTIES LINKER_LANGUAGE C)
   endif ()

   add_dependencies(jit_lib jit_codegen)

   set_target_properties(jit_lib PROPERTIES
      OUTPUT_NAME "jitlib"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/jit/lib"
   )

   # Set link path for tiri module
   set (JIT_LINK "${CMAKE_BINARY_DIR}/jit/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jitlib${CMAKE_STATIC_LIBRARY_SUFFIX}")

   # Create INTERFACE library to encapsulate JIT usage requirements
   add_library(jit_interface INTERFACE)
   target_include_directories(jit_interface INTERFACE "${JIT_SRC}")
   target_compile_definitions(jit_interface INTERFACE ${JIT_COMMON_DEFS})

   add_custom_target(jitlib DEPENDS jit_lib)  # Create an alias for backward compatibility
endif ()

# Tiri library build

add_library (${MOD})

set_module_defaults (${MOD} "Fl")

add_dependencies (${MOD} libffi-3.3 jitlib jit_codegen)

if (BUILD_DEFS)
   add_dependencies (${MOD} ${MOD}_hashes)
endif ()

set (TIRI_SOURCES "${MOD}.cpp" "tiri_module.cpp" "tiri_async.cpp" "tiri_struct.cpp" "tiri_processing.cpp"
   "tiri_number.cpp" "tiri_functions.cpp" "tiri_regex.cpp"
   "tiri_io.cpp" "tiri_class.cpp"
   "${JIT_SRC}/lib/lib.cpp"
   "${JIT_SRC}/lib/load.cpp"
   "${JIT_SRC}/lib/lib_aux.cpp"
   "${JIT_SRC}/lib/lib_array.cpp"
   "${JIT_SRC}/lib/lib_base.cpp"
   "${JIT_SRC}/lib/lib_bit.cpp"
   "${JIT_SRC}/lib/lib_debug.cpp"
   "${JIT_SRC}/lib/lib_init.cpp"
   "${JIT_SRC}/lib/lib_jit.cpp"
   "${JIT_SRC}/lib/lib_math.cpp"
   "${JIT_SRC}/lib/lib_object.cpp"
   "${JIT_SRC}/lib/lib_range.cpp"
   "${JIT_SRC}/lib/lib_string.cpp"
   "${JIT_SRC}/lib/lib_table.cpp"
   "${JIT_SRC}/bytecode/lj_bc.cpp"
   "${JIT_SRC}/bytecode/lj_bcread.cpp"
   "${JIT_SRC}/bytecode/lj_bcwrite.cpp"
   "${JIT_SRC}/debug/dump_bytecode.cpp"
   "${JIT_SRC}/debug/filesource.cpp"
   "${JIT_SRC}/parser/parser.cpp"
   "${JIT_SRC}/parser/lexer.cpp"
   "${JIT_SRC}/parser/parser_unit_tests.cpp"
   "${JIT_SRC}/parser/field_type_lookup.cpp")

if (INCLUDE_TIPS)
   list (APPEND TIRI_SOURCES "${JIT_SRC}/parser/parser_tips.cpp")
endif ()

if (ENABLE_UNIT_TESTS)
   list (APPEND TIRI_SOURCES "${JIT_SRC}/runtime/unit_test_indexing.cpp")
   list (APPEND TIRI_SOURCES "${JIT_SRC}/runtime/unit_test_vm_asm.cpp")
   list (APPEND TIRI_SOURCES "${JIT_SRC}/runtime/unit_tests_arrays.cpp")
   list (APPEND TIRI_SOURCES "${JIT_SRC}/jit/jit_unit_tests.cpp")

   # Add MASM assembly file for register capture on MSVC x64
   if (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
      enable_language(ASM_MASM)
      list (APPEND TIRI_SOURCES "${JIT_SRC}/runtime/register_capture_x64.asm")
   endif ()
endif ()

if (NOT DISABLE_DISPLAY)
   list (APPEND TIRI_SOURCES "tiri_input.cpp")
endif ()

target_sources (${MOD} PRIVATE ${TIRI_SOURCES})

target_include_directories (${MOD} PRIVATE
   "${JIT_SRC}/bytecode"
   "${JIT_SRC}/debug"
   "${JIT_SRC}/jit"
   "${JIT_SRC}/lib"
   "${JIT_SRC}/parser"
   "${JIT_SRC}/runtime"
   "${JIT_BUILD_DIR}"
   "${CMAKE_BINARY_DIR}/libffi-3.3/include")

target_link_libraries (${MOD} PRIVATE
   jit_interface
   ${FFI_LINK}
   ${JIT_LINK}
   ${MATH_LINK}) # The link order matters, math must come last

if (WIN32 AND NOT MSVC)
   target_link_libraries(${MOD} PRIVATE dbghelp)
endif ()

target_compile_definitions(${MOD} PRIVATE
   PRV_METACLASS
   JIT_DISABLE_BUFFER
   $<$<CONFIG:Debug>:LUA_USE_ASSERT>)

if (MSVC)
   # /MP enables multi-processor compilation for parallel builds within this target
   target_compile_options(${MOD} PRIVATE /MP)
else ()
   # Suppress offsetof warnings for non-standard-layout types (lua_State, GG_State contain C++ members)
   target_compile_options(${MOD} PRIVATE -Wno-invalid-offsetof)
endif ()

if (ENABLE_UNIT_TESTS)
   target_compile_definitions(${MOD} PRIVATE ENABLE_UNIT_TESTS)
endif ()

if (DISABLE_DISPLAY)
   target_compile_definitions (${MOD} PRIVATE DISABLE_DISPLAY)
endif ()

# Register Flute tests

set (TIRI_TESTS
   defer debuglog io object regex struct async async_actions async_wait xml strings math array array_any array_manip
   array_functional array_typed_syntax bitshift bitwise has_operator if_empty presence blank ternary table stress unit_tests
   jitoptions fstring const import_scope unicode ellipsis safe_nav close type_annotations type_fixing
   type_inference jit pipe pipe_iteration arrow_functions result_filter thunks deferred_expr shadow_assert ranges
   annotations locality anonymous_for key_values debug_filesources compound choose_from flow table_slice
   try_except import processing metatables with thunk_table_index debug
)

foreach (TEST_NAME ${TIRI_TESTS})
   flute_test(tiri_${TEST_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${TEST_NAME}.tiri")
endforeach ()
