# JPEG module

set (MOD jpeg)
set (INC_MOD_JPEG TRUE PARENT_SCOPE)

add_library (${MOD})
set_module_defaults (${MOD} "Jpeg")

target_sources (${MOD} PRIVATE "jpeg.cpp")

# Only link to libjpeg-turbo if it was built (when JPEG and Image support is enabled)
if (NOT DISABLE_JPEG AND NOT DISABLE_IMAGE)
   add_dependencies (${MOD} libjpeg-turbo)
   target_link_libraries (${MOD} PRIVATE libjpeg-turbo ${MATH_LINK})
   target_include_directories (${MOD} PRIVATE ${LIBJPEG_TURBO_INCLUDE_DIRS})
else()
   target_link_libraries (${MOD} PRIVATE ${MATH_LINK})
endif()
