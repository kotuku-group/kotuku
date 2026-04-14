
cmake_minimum_required(VERSION 3.15)

project (libffi VERSION 3.3 LANGUAGES C)

if ("${CMAKE_C_COMPILER_ARCHITECTURE_ID}" STREQUAL "")
   set (HOST_ARCH ${CMAKE_SYSTEM_PROCESSOR})
else ()
   set (HOST_ARCH ${CMAKE_C_COMPILER_ARCHITECTURE_ID})
endif ()

if ("${TARGET_PLATFORM}" STREQUAL "")
   if (HOST_ARCH MATCHES x64|x86_64|AMD64|amd64)
      if (CMAKE_SYSTEM_NAME STREQUAL Windows)
         set (TARGET_PLATFORM X86_WIN64)
      else ()
         set (TARGET_PLATFORM X86_64)
      endif ()
   elseif (HOST_ARCH MATCHES i.*86.*|X86|x86)
      if (MSVC)
         set (TARGET_PLATFORM X86_WIN32)
      else ()
         set (TARGET_PLATFORM X86)
      endif ()

      if (CMAKE_SYSTEM_NAME STREQUAL Darwin)
         set (TARGET_PLATFORM X86_DARWIN)
      elseif (CMAKE_SYSTEM_NAME MATCHES FreeBSD|OpenBSD)
         set (TARGET_PLATFORM X86_FREEBSD)
      endif ()
   elseif (HOST_ARCH MATCHES aarch64|ARM64|arm64)
      if (MSVC)
         set (TARGET_PLATFORM ARM_WIN64)
      else ()
         set (TARGET_PLATFORM AARCH64)
      endif ()
   elseif (HOST_ARCH MATCHES arm.*|ARM.*)
      if (MSVC)
         set (TARGET_PLATFORM ARM_WIN32)
      else ()
         set (TARGET_PLATFORM ARM)
      endif ()
   else ()
      message(FATAL_ERROR "Unknown host.")
   endif ()
endif ()

# The script below will produce SOURCES_LIST

include (configure_options.cmake)

file (COPY ${FFI_CONFIG_FILE} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file (COPY src/${TARGETDIR}/ffitarget.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include)

include_directories (${CMAKE_CURRENT_BINARY_DIR}/include)
include_directories (${CMAKE_CURRENT_BINARY_DIR})
include_directories (include)

if (MSVC)
   set (CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
   set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

   # Disable MSVC run-time checks (RTC) for libffi because its alloca-heavy trampolines trigger benign _RTC_CheckStackVars2 frames
   # This must be done before the cache is set, so we do it here rather than using target properties
   STRING(REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
   set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}" CACHE STRING "Flags used by the C compiler during DEBUG builds." FORCE)
else ()
   set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fomit-frame-pointer -fstrict-aliasing -ffast-math")
endif ()

message(STATUS "Building for TARGET_PLATFORM: ${TARGET_PLATFORM}")

add_library (libffi STATIC)
set_property (TARGET libffi PROPERTY POSITION_INDEPENDENT_CODE 1)

target_sources (libffi PRIVATE
   src/closures.c src/java_raw_api.c src/prep_cif.c src/raw_api.c src/types.c
   ${SOURCES_LIST})

install(TARGETS libffi
    EXPORT ${PROJECT_NAME}Targets
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib)

install(EXPORT ${PROJECT_NAME}Targets
    DESTINATION share/${PROJECT_NAME})

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/include/ffi.h
    ${CMAKE_CURRENT_BINARY_DIR}/include/ffitarget.h
    DESTINATION include)
