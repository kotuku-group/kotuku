# MP3 module

set (MOD "mp3")
set (INC_MOD_MP3 TRUE PARENT_SCOPE)

add_library (${MOD})
set_module_defaults (${MOD} "Mp3")
target_sources (${MOD} PRIVATE "mp3.cpp")

# MP3 tests should be manually enabled if working on the MP3 module.
# flute_test (mp3_audio "tests/test-mp3.tiri")
