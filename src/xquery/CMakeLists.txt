# XQuery Module

# Enable unit tests for deep testing of non-release builds only.
if (NOT DEFINED ENABLE_UNIT_TESTS)
#   set (ENABLE_UNIT_TESTS ON)
endif ()

set (MOD xquery)
set (INC_MOD_XQUERY TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl"
   NAME ${MOD}_defs
   OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   FILES "xquery.cpp"
   ARGS  "output-defs=xquery_def.cpp" "output-proto=xquery_def.cpp"
   APPEND_IDL "${MOD}_def.cpp")

add_library (${MOD})
target_link_libraries (${MOD} PUBLIC unicode)
set_module_defaults (${MOD} "Xq")

# XQuery Support Library

add_library (${MOD}_lib OBJECT
   "${CMAKE_CURRENT_SOURCE_DIR}/${MOD}.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_common.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_navigation.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_context.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_predicates.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_values.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_expression.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/eval_flwor.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/date_time_utils.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/eval/eval.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/api/xquery_functions.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/api/xquery_axis.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/api/xquery_prolog.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/functions/func_math.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/parse/xquery_parser.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/parse/xquery_tokeniser.cpp")

target_include_directories (${MOD}_lib PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties (${MOD}_lib PROPERTIES CXX_STANDARD 20 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions (${MOD}_lib PRIVATE "PRV_XML" "PRV_XQUERY_MODULE")
add_dependencies (${MOD} build_headers xml)

# Enable precompiled headers for the xquery functions object library
# This must be done before the module target to avoid circular dependencies

target_precompile_headers(${MOD}_lib PRIVATE
   # Heavy STL headers used across most files
   <string>
   <string_view>
   <vector>
   <memory>
   <optional>
   <format>
   <utility>
   <unordered_map>
   <unordered_set>
   <algorithm>
   # Kotuku headers
   <kotuku/main.h>
   <kotuku/modules/xml.h>
   <kotuku/modules/xquery.h>
   <kotuku/strings.hpp>
   # Third-party dependency
   <ankerl/unordered_dense.h>
   # Core internal headers
   "${CMAKE_CURRENT_SOURCE_DIR}/../xml/xml.h"
   "${CMAKE_CURRENT_SOURCE_DIR}/xquery.h")

# Enable unity builds to speed up compilation of the xquery functions library

set_target_properties(${MOD}_lib PROPERTIES UNITY_BUILD ON)
set_target_properties(${MOD}_lib PROPERTIES UNITY_BUILD_BATCH_SIZE 8)

# Module Code

target_sources (${MOD} PRIVATE
   $<TARGET_OBJECTS:${MOD}_lib>
   $<TARGET_OBJECTS:xml_schema>)

if (ENABLE_UNIT_TESTS)
   target_compile_definitions(${MOD}_lib PRIVATE "ENABLE_UNIT_TESTS")
endif ()

# Tests

flute_test (${MOD}_reserved_words "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reserved_words.tiri")
flute_test (${MOD}_func_ext "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_func_ext.tiri")
flute_test (${MOD}_sequences "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sequences.tiri")
flute_test (${MOD}_string_uri "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_string_uri.tiri")
flute_test (${MOD}_duration "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_duration.tiri")
flute_test (${MOD}_datetime "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_datetime.tiri")
flute_test (${MOD}_edge_cases "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_edge_cases.tiri")
flute_test (${MOD}_qname "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_qname.tiri")
flute_test (${MOD}_documents "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_documents.tiri")
flute_test (${MOD}_accessor "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_accessor.tiri")
flute_test (${MOD}_constructors "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_constructors.tiri")
flute_test (${MOD}_maps_arrays "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_maps_arrays.tiri")
flute_test (${MOD}_prolog "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prolog.tiri")
flute_test (${MOD}_core "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_core.tiri")
flute_test (${MOD}_predicates "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_predicates.tiri")
flute_test (${MOD}_axes "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_axes.tiri")
flute_test (${MOD}_advanced "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_advanced.tiri")
flute_test (${MOD}_advanced_paths "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_advanced_paths.tiri")
flute_test (${MOD}_flwor "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_flwor.tiri")
flute_test (${MOD}_module_loading "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_module_loading.tiri")
flute_test (${MOD}_type_expr "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_type_expr.tiri")
flute_test (${MOD}_type_constructors "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_type_constructors.tiri")
flute_test (${MOD}_sequence_cardinality "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sequence_cardinality.tiri")
flute_test (${MOD}_math "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_math.tiri")
flute_test (${MOD}_qt_math "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_qt_math.tiri")
flute_test (${MOD}_qt_maps_arrays "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_qt_maps_arrays.tiri")

if (ENABLE_UNIT_TESTS)
   flute_test (xquery_unit_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_unit_tests.tiri")
endif ()
