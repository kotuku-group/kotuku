# Vector module

set (MOD vector)
set (INC_MOD_VECTOR TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs OUTPUT "${INCLUDE_OUTPUT}/modules/vector.h"
   FILES "vector_functions.cpp" "vector_readpainter.cpp"
   ARGS "output-defs=module_def.c")

idl_compile ("${MOD}.tdl" NAME ${MOD}_idl OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/idl.h")

add_library (${MOD})

set_module_defaults (${MOD} "Vec")

if (MSVC)
   target_compile_options (${MOD} PRIVATE /GS-)
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
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_arc.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_arrowhead.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_bezier_arc.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_bspline.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_curves.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_contour.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_dash.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_markers_term.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_smooth_poly1.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_stroke.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vcgen_bspline.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_image_filters.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_line_aa_basics.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_line_profile_aa.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_sqrt_tables.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_trans_affine.cpp"
   #${CMAKE_CURRENT_SOURCE_DIR}/agg/src/gpc/gpc.c
   #${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_trans_double_path.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_trans_single_path.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vpgen_clip_polygon.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vpgen_clip_polyline.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/agg/src/agg_vpgen_segmentator.cpp")

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

flute_test(vector_readpainter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_readpainter.tiri")
