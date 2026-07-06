# Vector module

set (MOD vector)
set (INC_MOD_VECTOR TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs OUTPUT "${INCLUDE_OUTPUT}/modules/vector.h"
   ARGS "output-defs=module_def.c" DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vector_functions.cpp")

idl_compile ("${MOD}.tdl" NAME ${MOD}_idl OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/idl.h")

add_library (${MOD})

set_module_defaults (${MOD} "Vec")

if (MSVC)
   target_compile_options (${MOD} PRIVATE /GS- /MP)
else ()
target_compile_options (${MOD} PRIVATE
  "$<${GCC_COMPAT}:$<BUILD_INTERFACE:-Wno-narrowing;-Wno-uninitialized>>")
endif ()

#add_definitions ("-DDBG_DRAW=1")

target_sources (${MOD} PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/vector.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/vector_functions.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/filters/filter.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/scene/scene.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/painters/painters.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/transformers/transformers.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/colourmaps.cpp"
   #"${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_arrowhead.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_bezier_arc.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_curves.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_contour.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_dash.cpp"
   #"${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_markers_term.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_smooth_poly1.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_stroke.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_image_filters.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_line_aa_basics.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_line_profile_aa.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_trans_affine.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_trans_single_path.cpp")

target_include_directories (${MOD} PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/include")

# Pre-compiling the main header speeds up compilation and eliminates the need to include it in source files.

target_precompile_headers(${MOD} PRIVATE
   "vector.h" "font.h")

# Enable unity builds to speed up compilation of the many AGG source files
set_target_properties(${MOD} PROPERTIES UNITY_BUILD ON)
set_target_properties(${MOD} PROPERTIES UNITY_BUILD_BATCH_SIZE 8)

target_link_libraries (${MOD} PUBLIC unicode)
target_link_libraries (${MOD} PUBLIC linear_rgb)
target_link_libraries (${MOD} PRIVATE ${MATH_LINK})

# Only link freetype if font support is enabled
if (NOT DISABLE_FONT)
   target_link_libraries (${MOD} PRIVATE freetype)
endif()

if (BUILD_TESTS)
   add_executable (test_agg_stroke_resolve
      "tests/test_agg_stroke_resolve.cpp"
      "tests/test_agg_stroke_resolve_agglib.cpp")
   target_include_directories (test_agg_stroke_resolve PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/agg/include")
   target_link_libraries (test_agg_stroke_resolve PRIVATE ${MATH_LINK})
   set_target_properties (test_agg_stroke_resolve PROPERTIES CXX_STANDARD 20)
   add_test (NAME vector_agg_stroke_resolve COMMAND test_agg_stroke_resolve)
   set_tests_properties (vector_agg_stroke_resolve PROPERTIES LABELS "vector;unit")
endif ()

flute_test(vector_readpainter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_readpainter.tiri")
flute_test(vector_parse_transform "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parse_transform.tiri")
flute_test(vector_convolve_validation "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_convolve_validation.tiri")
flute_test(vector_apply_path "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_apply_path.tiri")
flute_test(vector_clip_mask_cache "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_clip_mask_cache.tiri")
flute_test(vector_gouraud_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gouraud_gradient.tiri")
flute_test(vector_mesh_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mesh_gradient.tiri")
flute_test(vector_conic_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_conic_gradient.tiri")
flute_test(vector_contour_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_contour_gradient.tiri")
flute_test(vector_gradient_easing_gamma "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gradient_easing_gamma.tiri")
flute_test(vector_gradient_transform "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gradient_transform.tiri")
flute_test(vector_distal_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_distal_gradient.tiri")
flute_test(vector_voronoi_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_voronoi_gradient.tiri")
flute_test(vector_isolated_bitmap_pool "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_isolated_bitmap_pool.tiri")
flute_test(vector_shared_invalidation "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shared_invalidation.tiri")
flute_test(vector_callback_lifetimes "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_callback_lifetimes.tiri")
