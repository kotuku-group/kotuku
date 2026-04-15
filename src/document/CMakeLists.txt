# Document module

set (MOD "document")
set (INC_MOD_DOCUMENT TRUE PARENT_SCOPE)

idl_gen ("defs/${MOD}.tdl" NAME ${MOD}_defs OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h" APPEND_IDL "defs/module_def.c"
   FILES "${MOD}.cpp"
   ARGS "output-defs=defs/module_def.c" "output-proto=defs/module_def.c" "prototypes=static")

idl_gen ("defs/hashes.tdl" NAME ${MOD}_hashes OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/defs/hashes.h")

add_library (${MOD})
target_link_libraries (${MOD} PUBLIC unicode)
set_module_defaults (${MOD} "Doc")
target_sources (${MOD} PRIVATE "${MOD}.cpp")

# Enable precompiled headers for the document module to speed up compilation
target_precompile_headers(${MOD} PRIVATE
   <kotuku/main.h>
   <kotuku/modules/xml.h>
   <kotuku/modules/document.h>
   <kotuku/modules/font.h>
   <kotuku/modules/display.h>
   <kotuku/modules/svg.h>
   <array>
   <variant>
   <stack>
   <cmath>
   <mutex>
   <charconv>)

flute_test (doc_layout "tests/test_layout.tiri")
flute_test (doc_benchmark "tests/test_benchmark.tiri")
flute_test (doc_stream "tests/test_stream.tiri")
flute_test (doc_roundtrip "tests/test_roundtrip.tiri")
flute_test (doc_xquery_stage1 "tests/test_xquery_stage1.tiri")
flute_test (doc_xquery_stage2 "tests/test_xquery_stage2.tiri")
