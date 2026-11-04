# MP3 module

set (MOD "mp3")
set (INC_MOD_MP3 TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   ARGS "output-defs=mp3_functions.cpp" "output-proto=mp3_functions.cpp"
   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/metadata.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/mp3.cpp"
   APPEND_IDL "mp3_functions.cpp")

add_library (${MOD})
set_module_defaults (${MOD} "Mp3")
target_sources (${MOD} PRIVATE "mp3.cpp")

# MP3 playback needs to be run manually when working on the MP3 module.
# flute_test (mp3_audio "tests/test_mp3_playback.tiri")
flute_test (mp3_decoder "tests/test_decoder.tiri")

flute_test (mp3_metadata "tests/test_metadata.tiri")
