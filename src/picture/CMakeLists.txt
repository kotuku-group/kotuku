# Picture module

set (MOD picture)
set (INC_MOD_PICTURE TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs APPEND_IDL "${MOD}_def.c" OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h")

set (SOURCES "${MOD}.cpp"
   "lib/png.c" "lib/pngset.c" "lib/pngget.c" "lib/pngread.c" "lib/pngpread.c"
   "lib/pngrtran.c" "lib/pngrutil.c" "lib/pngtrans.c" "lib/pngwrite.c" "lib/pngwtran.c"
   "lib/pngwutil.c" "lib/pngmem.c" "lib/pngerror.c")

add_library (${MOD})
set_module_defaults (${MOD} "Pic")
target_sources (${MOD} PRIVATE ${SOURCES})

target_include_directories (${MOD} PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/lib"
   ${ZLIB_HEADERS})

target_link_libraries (${MOD} PUBLIC linear_rgb)
target_link_libraries (${MOD} PRIVATE ${MATH_LINK} zlib)

