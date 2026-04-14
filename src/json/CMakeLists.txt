# JSON Module

set (MOD json)
set (INC_MOD_JSON TRUE PARENT_SCOPE)

add_library (${MOD})
set_module_defaults (${MOD} "Json")
target_sources (${MOD} PRIVATE ${MOD}.cpp)

flute_test (json "test_json.tiri")
