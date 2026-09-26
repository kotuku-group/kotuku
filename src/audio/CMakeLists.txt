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
   target_sources (${MOD} PRIVATE "wasapi.cpp")
   target_link_libraries (${MOD} PRIVATE ole32 avrt uuid)
else ()
   if (ALSA_INSTALLED)
      target_compile_definitions (${MOD} PRIVATE "ALSA_ENABLED")
      target_sources (${MOD} PRIVATE "device_enum.cpp")
      target_link_libraries (${MOD} PRIVATE "asound")
   endif ()
endif ()

# Embedded unit tests do not require an audio device.

if (UNIT_TESTS)
   target_compile_definitions (${MOD} PRIVATE UNIT_TESTS)
   find_package (Threads REQUIRED)
   target_link_libraries (${MOD} PRIVATE Threads::Threads)
   if (WIN32)
      target_compile_definitions (${MOD} PRIVATE AUDIO_TEST_BACKEND)
   endif ()
   flute_test (audio_unit_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_unit_tests.tiri")
   if (BUILD_TESTS)
      set_tests_properties (audio_unit_tests PROPERTIES LABELS "audio;unit" TIMEOUT 30)
   endif ()
endif ()

# Audio tests should be manually enabled only when working on the Audio module.

if (AUDIO_TESTS)
   flute_test (audio "tests/test_audio.tiri")
   flute_test (audio_add_sample_span "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_add_sample_span.tiri")

   if (ALSA_INSTALLED OR WIN32)
      flute_test (audio_batches "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audio_batches.tiri")
      flute_test (audio_playback_control "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_playback_control.tiri")
      flute_test (audio_gain_pan "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gain_pan.tiri")
      flute_test (audio_equaliser "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audioequaliser.tiri")
      flute_test (audio_dsp_infrastructure "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dsp_infrastructure.tiri")
      flute_test (audio_effect_schema "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audioeffect_schema.tiri")
   endif ()

   if (WIN32 AND BUILD_TESTS)
      target_compile_definitions (${MOD} PRIVATE AUDIO_TEST_BACKEND)
      flute_test (audio_windows_effects "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_effects.tiri")
      option (KOTUKU_AUDIO_HARDWARE_TESTS "Run playback tests against the configured audio device" OFF)
   endif ()

   if (ALSA_INSTALLED)
      flute_test (audio_alsa_backend "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alsa_backend.tiri")
      option (KOTUKU_AUDIO_HARDWARE_TESTS "Run playback tests against the configured ALSA device" OFF)
      if (KOTUKU_AUDIO_HARDWARE_TESTS)
         flute_test (audio_alsa_hardware "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alsa_hardware.tiri")
         if (BUILD_TESTS)
            set_tests_properties (audio_alsa_hardware PROPERTIES LABELS "audio;hardware" TIMEOUT 30 RUN_SERIAL TRUE)
         endif ()
      endif ()
   endif ()

endif ()
