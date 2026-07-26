# Audio module
# Debian system preparation: "apt-get install libasound2-dev"

set (MOD "audio")
set (INC_MOD_AUDIO TRUE PARENT_SCOPE)

if (NOT WIN32)
   CHECK_C_SOURCE_COMPILES ("
#include <alsa/asoundlib.h>
int main() {
   return 0;
}"
   ALSA_INSTALLED)
endif ()

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   APPEND_IDL "module_def.c"
   ARGS "output-defs=module_def.c" "output-proto=module_def.c")

set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast")

if (WIN32)
   set (C_INCLUDE_PATH "")
endif ()

add_library (${MOD})

set_module_defaults (${MOD} "Audio")

target_sources (${MOD} PRIVATE "audio.cpp")

if (WIN32)
   target_sources (${MOD} PRIVATE "dsound.cpp")
else ()
   if (ALSA_INSTALLED)
      target_compile_definitions (${MOD} PRIVATE "ALSA_ENABLED")
      target_sources (${MOD} PRIVATE "device_enum.cpp")
      target_link_libraries (${MOD} PRIVATE "asound")
   endif ()
endif ()

# Audio tests should be manually enabled if working on the Audio module.
# flute_test (audio "tests/test_audio.tiri")
flute_test (audio_add_sample_span "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_add_sample_span.tiri")

if (BUILD_TESTS)
   add_executable (test_audio_mixers tests/test_mixers.cpp)
   target_include_directories (test_audio_mixers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
   target_compile_features (test_audio_mixers PRIVATE cxx_std_20)
   set_target_properties (test_audio_mixers PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
      CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

   enable_testing()
   add_test (NAME AudioMixerFull
      COMMAND test_audio_mixers "--log-warning"
      CONFIGURATIONS Debug)

   set_tests_properties (AudioMixerFull PROPERTIES
      TIMEOUT 60
      LABELS "audio;unit;comprehensive")
endif()
