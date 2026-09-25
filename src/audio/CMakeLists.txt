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

# Audio tests should be manually enabled if working on the Audio module.
# flute_test (audio "tests/test_audio.tiri")
flute_test (audio_add_sample_span "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_add_sample_span.tiri")

if (ALSA_INSTALLED OR WIN32)
   flute_test (audio_batches "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audio_batches.tiri")
   flute_test (audio_effect "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audioeffect.tiri")
   flute_test (audio_equaliser "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audioequaliser.tiri")
   flute_test (audio_effect_schema "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audioeffect_schema.tiri")
endif ()

if (WIN32 AND BUILD_TESTS)
   target_compile_definitions (${MOD} PRIVATE AUDIO_TEST_BACKEND)
   flute_test (audio_windows_effects "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_effects.tiri")
   add_executable (test_wasapi_transport tests/test_wasapi_transport.cpp wasapi.cpp)
   target_include_directories (test_wasapi_transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
   target_compile_features (test_wasapi_transport PRIVATE cxx_std_20)
   target_compile_definitions (test_wasapi_transport PRIVATE AUDIO_TEST_BACKEND)
   target_link_libraries (test_wasapi_transport PRIVATE ole32 avrt uuid)
   add_test (NAME AudioWasapiTransport COMMAND test_wasapi_transport CONFIGURATIONS Debug)
   set_tests_properties (AudioWasapiTransport PROPERTIES LABELS "audio;unit" TIMEOUT 30)
   option (KOTUKU_AUDIO_HARDWARE_TESTS "Run playback tests against the configured audio device" OFF)
   if (KOTUKU_AUDIO_HARDWARE_TESTS)
      flute_test (audio_windows_hardware "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_hardware.tiri")
      set_tests_properties (audio_windows_hardware PROPERTIES LABELS "audio;hardware" TIMEOUT 90 RUN_SERIAL TRUE)
   endif ()
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

if (BUILD_TESTS)
   add_executable (test_audio_batches tests/test_audio_batches.cpp)
   target_include_directories (test_audio_batches PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
   target_compile_features (test_audio_batches PRIVATE cxx_std_20)
   find_package (Threads REQUIRED)
   target_link_libraries (test_audio_batches PRIVATE Threads::Threads)
   add_test (NAME AudioBatches COMMAND test_audio_batches CONFIGURATIONS Debug)
   set_tests_properties (AudioBatches PROPERTIES LABELS "audio;unit" TIMEOUT 30)

   add_executable (test_audio_effect tests/test_audio_effect.cpp)
   target_include_directories (test_audio_effect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
   target_compile_features (test_audio_effect PRIVATE cxx_std_20)
   find_package (Threads REQUIRED)
   target_link_libraries (test_audio_effect PRIVATE Threads::Threads)
   add_test (NAME AudioEffectChain COMMAND test_audio_effect CONFIGURATIONS Debug)
   set_tests_properties (AudioEffectChain PROPERTIES LABELS "audio;unit" TIMEOUT 30)

   add_executable (test_audio_equaliser_dsp tests/test_audio_equaliser_dsp.cpp)
   target_include_directories (test_audio_equaliser_dsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
   target_compile_features (test_audio_equaliser_dsp PRIVATE cxx_std_20)
   target_link_libraries (test_audio_equaliser_dsp PRIVATE Threads::Threads)
   add_test (NAME AudioEqualiserDSP COMMAND test_audio_equaliser_dsp CONFIGURATIONS Debug)
   set_tests_properties (AudioEqualiserDSP PROPERTIES LABELS "audio;unit" TIMEOUT 30)

   add_executable (test_audio_effect_params tests/test_audio_effect_params.cpp)
   target_include_directories (test_audio_effect_params PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
   target_compile_features (test_audio_effect_params PRIVATE cxx_std_20)
   add_test (NAME AudioEffectParams COMMAND test_audio_effect_params CONFIGURATIONS Debug)
   set_tests_properties (AudioEffectParams PROPERTIES LABELS "audio;unit" TIMEOUT 30)

   if (ALSA_INSTALLED)
      add_executable (test_audio_worker tests/test_audio_worker.cpp)
      target_include_directories (test_audio_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_compile_features (test_audio_worker PRIVATE cxx_std_20)
      target_link_libraries (test_audio_worker PRIVATE pthread)
      add_test (NAME AudioWorker COMMAND test_audio_worker CONFIGURATIONS Debug)
      set_tests_properties (AudioWorker PROPERTIES LABELS "audio;unit" TIMEOUT 30)
   endif ()

   add_executable (test_audio_buffer tests/test_audio_buffer.cpp)
   target_include_directories (test_audio_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
   target_compile_features (test_audio_buffer PRIVATE cxx_std_20)
   add_test (NAME AudioBufferPolicy COMMAND test_audio_buffer CONFIGURATIONS Debug)
   set_tests_properties (AudioBufferPolicy PROPERTIES LABELS "audio;unit" TIMEOUT 30)

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
