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

set (vector_sources
   "${CMAKE_CURRENT_SOURCE_DIR}/vector.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/vector_functions.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/kerning.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/font_style.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/filters/filter.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/scene/scene.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/painters/painters.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/transformers/transformers.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/colourmaps.cpp")

target_sources (${MOD} PRIVATE
   ${vector_sources}
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

# Keep the main sources separate so an edit does not rebuild unrelated module implementations.
# Only the small AGG source files benefit from unity batching.
set_source_files_properties(${vector_sources} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
set_target_properties(${MOD} PROPERTIES UNITY_BUILD ON)
set_target_properties(${MOD} PROPERTIES UNITY_BUILD_BATCH_SIZE 8)

target_link_libraries (${MOD} PUBLIC unicode)
target_link_libraries (${MOD} PUBLIC linear_rgb)
target_link_libraries (${MOD} PRIVATE ${MATH_LINK})

# Only link freetype if font support is enabled
if (NOT DISABLE_FONT)
   target_link_libraries (${MOD} PRIVATE freetype)
endif()

flute_test(vector_readpainter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_readpainter.tiri")
flute_test(vector_fill_layers "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fill_layers.tiri")
flute_test(vector_parse_transform "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parse_transform.tiri")
flute_test(vector_push "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_push.tiri")
flute_test(vector_convolve_validation "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_convolve_validation.tiri")
flute_test(vector_lighting_units "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lighting_units.tiri")
flute_test(vector_filter_bounds "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_filter_bounds.tiri")
flute_test(vector_apply_path "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_apply_path.tiri")
flute_test(vector_clip_mask_cache "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_clip_mask_cache.tiri")
flute_test(vector_gouraud_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gouraud_gradient.tiri")
flute_test(vector_mesh_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mesh_gradient.tiri")
flute_test(vector_diffusion_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_diffusion_gradient.tiri")
flute_test(vector_conic_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_conic_gradient.tiri")
flute_test(vector_contour_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_contour_gradient.tiri")
flute_test(vector_gradient_easing_gamma "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gradient_easing_gamma.tiri")
flute_test(vector_gradient_transform "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gradient_transform.tiri")
flute_test(vector_distal_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_distal_gradient.tiri")
flute_test(vector_voronoi_gradient "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_voronoi_gradient.tiri")
flute_test(vector_isolated_bitmap_pool "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_isolated_bitmap_pool.tiri")
flute_test(vector_shared_invalidation "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shared_invalidation.tiri")
flute_test(vector_pattern_aspect_ratio "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pattern_aspect_ratio.tiri")
flute_test(vector_spread_reflect "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spread_reflect.tiri")
flute_test(vector_pattern_scale "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pattern_scale.tiri")
flute_test(vector_viewport_pattern "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_viewport_pattern.tiri")
flute_test(vector_callback_lifetimes "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_callback_lifetimes.tiri")
flute_test(vector_viewport_input_free "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_viewport_input_free.tiri")
flute_test(vector_focus "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_focus.tiri")
flute_test(vector_buffered_viewport "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_buffered_viewport.tiri")
flute_test(vector_path_command_spans "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_path_command_spans.tiri")
flute_test(vector_dash_array "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dash_array.tiri")
flute_test(vector_text_path_length "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_text_path_length.tiri")
flute_test(vector_text_font_size "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_text_font_size.tiri")
flute_test(vector_text_wrap "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_text_wrap.tiri")
flute_test(vector_text_selection_keyboard "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_text_selection_keyboard.tiri")
flute_test(vector_spiral "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spiral.tiri")
flute_test(vector_supershape "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_supershape.tiri")

flute_test(vector_drag_drop "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_drag_drop.tiri")
