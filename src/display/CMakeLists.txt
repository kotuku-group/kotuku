# Display module

set (MOD "display")
set (INC_MOD_DISPLAY TRUE PARENT_SCOPE)

set (FUNCTIONS # Full paths are required for add_custom_command() dependencies to work correctly
   "${CMAKE_CURRENT_SOURCE_DIR}/display.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_bitmap.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_display.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_cursor.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/lib_input.cpp")

set (DISPLAY_DRIVERS
   "${CMAKE_CURRENT_SOURCE_DIR}/driver/headless.cpp")

set (CLASSES
   "${CMAKE_CURRENT_SOURCE_DIR}/class_bitmap.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_display.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_clipboard.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_controller.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_pointer.cpp"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_surface/class_surface.cpp")

set (DISPLAY_DEFS # Full paths are required for add_custom_command() dependencies to work correctly
   "${CMAKE_CURRENT_SOURCE_DIR}/class_bitmap_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_clipboard_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_controller_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_display_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_pointer_def.c"
   "${CMAKE_CURRENT_SOURCE_DIR}/class_surface/surface_def.c")

if (KOTUKU_DEFS_AVAILABLE) # Customised idl_c() equivalent, this is for defining the output from the TDL file
   set (DISPLAY_STAMP "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/display_defs.stamp")
   set (DISPLAY_OUTPUTS
      ${DISPLAY_DEFS}
      "${DOCS}/modules/display.xml"
      "${DOCS}/modules/classes/bitmap.xml"
      "${DOCS}/modules/classes/clipboard.xml"
      "${DOCS}/modules/classes/controller.xml"
      "${DOCS}/modules/classes/display.xml"
      "${DOCS}/modules/classes/pointer.xml"
      "${DOCS}/modules/classes/surface.xml"
      "${INCLUDE_OUTPUT}/modules/display.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/idl.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/module_def.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/prototypes.h")

   list (APPEND COMMAND_LIST COMMAND ${CMAKE_COMMAND} -E echo "[idl_c] ${ORIGO_CMD} ${IDL_C_SCRIPT} --log-warning src=${MOD}.tdl sdk=${PROJECT_SOURCE_DIR} output-proto=prototypes.h output-defs=module_def.c")
   list (APPEND COMMAND_LIST COMMAND ${CMAKE_COMMAND} -E echo "[idl_comp] ${ORIGO_CMD} ${IDL_COMPILE_SCRIPT} --log-warning src=${MOD}.tdl output=${CMAKE_CURRENT_SOURCE_DIR}/idl.h sdk=${PROJECT_SOURCE_DIR} format=c")

   add_custom_command (OUTPUT "${DISPLAY_STAMP}"
      BYPRODUCTS ${DISPLAY_OUTPUTS}
      ${COMMAND_LIST}
      COMMAND ${ORIGO_CMD} ${IDL_C_SCRIPT} "--log-warning" "src=${MOD}.tdl" "sdk=${PROJECT_SOURCE_DIR}"
              "output-proto=prototypes.h" "output-defs=module_def.c"
      COMMAND ${ORIGO_CMD} ${IDL_COMPILE_SCRIPT} "--log-warning" "src=${MOD}.tdl" "output=${CMAKE_CURRENT_SOURCE_DIR}/idl.h"
              "sdk=${PROJECT_SOURCE_DIR}" "format=c"
      COMMAND ${CMAKE_COMMAND} -E touch "${DISPLAY_STAMP}"
      DEPENDS
         "${MOD}.tdl"
         ${FUNCTIONS}
         "lib_surfaces.cpp"
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Producing C definition files for the ${MOD} module"
      VERBATIM)

   add_custom_target (display_defs DEPENDS "${DISPLAY_STAMP}")

   register_generated_header_target (display_defs)
endif ()

add_library (${MOD})

set_module_defaults (${MOD} "Gfx")

target_include_directories (${MOD} PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/lib")

target_link_libraries (${MOD} PUBLIC unicode)
target_link_libraries (${MOD} PUBLIC linear_rgb)
target_link_libraries (${MOD} PRIVATE ${MATH_LINK})

flute_test(display_bitmap_pixels "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bitmap_pixels.tiri")

if (UNIT_TESTS)
   add_executable (test_display_headless "tests/test_headless_driver.cpp" ${DISPLAY_DRIVERS})
   set_target_properties (test_display_headless PROPERTIES CXX_STANDARD 20)
   add_test (NAME display_headless_driver COMMAND test_display_headless)
   set_tests_properties (display_headless_driver PROPERTIES LABELS display)
endif ()

if (WIN32)
   target_compile_definitions (${MOD} PRIVATE DISPLAY_WINDOWS_DRIVER WIN_DRAGDROP)
   target_link_libraries (${MOD} PRIVATE ole32 oleaut32 uuid)
   target_sources (${MOD} PRIVATE ${FUNCTIONS} ${DISPLAY_DRIVERS} "win32/clipboard.c" ${CLASSES})

   add_library (display-windows)
   target_include_directories (display-windows PRIVATE "${PROJECT_SOURCE_DIR}/include")
   set_target_properties (display-windows PROPERTIES CXX_STANDARD 20)
   add_dependencies (display-windows ${REQUIRED_MODS})
   target_sources (display-windows PRIVATE
      "drivers/win32/module.cpp"
      "drivers/win32/win32_driver.cpp"
      "drivers/win32/windows.cpp"
      "drivers/win32/handlers.cpp"
      "drivers/win32/controller.cpp"
      "drivers/win32/controller_mapping.cpp")
   target_link_libraries (display-windows PRIVATE unicode gdi32 setupapi uuid xinput dinput8 dxguid wbemuuid ole32
      oleaut32 ${MATH_LINK})

   if (KOTUKU_STATIC)
      target_link_libraries (${MOD} PRIVATE display-windows)
   else ()
      target_sources (display-windows PRIVATE "drivers/win32/exports.def")
      set_target_properties (display-windows PROPERTIES
         PREFIX ""
         POSITION_INDEPENDENT_CODE ON
         ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib"
         LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib"
         RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib")
      if (KOTUKU_INSTALL)
         install (TARGETS display-windows
            RUNTIME DESTINATION "${LIB_TARGET}"
            ARCHIVE DESTINATION "${LIB_TARGET}"
            LIBRARY DESTINATION "${LIB_TARGET}")
      endif ()
   endif ()

   if (UNIT_TESTS)
      add_executable (test_display_windows_controller "tests/test_windows_controller.cpp"
         "drivers/win32/controller_mapping.cpp")
      set_target_properties (test_display_windows_controller PROPERTIES CXX_STANDARD 20)
      add_test (NAME display_windows_controller COMMAND test_display_windows_controller)
      set_tests_properties (display_windows_controller PROPERTIES LABELS display)

      add_executable (test_display_windows_driver "tests/test_windows_driver.cpp")
      target_link_libraries (test_display_windows_driver PRIVATE display-windows)
      if (KOTUKU_STATIC)
         target_link_libraries (test_display_windows_driver PRIVATE kotuku_static)
      endif ()
      set_target_properties (test_display_windows_driver PROPERTIES CXX_STANDARD 20)
      if (NOT KOTUKU_STATIC)
         add_custom_command (TARGET test_display_windows_driver POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:display-windows>"
               "$<TARGET_FILE_DIR:test_display_windows_driver>")
      endif ()
      add_test (NAME display_windows_driver COMMAND test_display_windows_driver)
      set_tests_properties (display_windows_driver PROPERTIES LABELS display)
   endif ()
elseif (ANDROID)
   target_compile_definitions (${MOD} PRIVATE DISPLAY_ANDROID_DRIVER)
   target_sources (${MOD} PRIVATE ${FUNCTIONS} ${DISPLAY_DRIVERS} ${CLASSES})

   add_library (display-android)
   target_include_directories (display-android PRIVATE "${PROJECT_SOURCE_DIR}/include")
   set_target_properties (display-android PROPERTIES CXX_STANDARD 20)
   set_target_properties (display-android PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
   add_dependencies (display-android ${REQUIRED_MODS})
   target_sources (display-android PRIVATE
      "drivers/android/module.cpp"
      "drivers/android/android_driver.cpp"
      "drivers/android/lib_pixels.cpp")
   target_link_libraries (display-android PRIVATE unicode android EGL GLESv1_CM ${MATH_LINK})

   if (KOTUKU_STATIC)
      target_link_libraries (${MOD} PRIVATE display-android)
   else ()
      set_target_properties (display-android PROPERTIES
         PREFIX ""
         POSITION_INDEPENDENT_CODE ON
         LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib"
         RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib")
      if (KOTUKU_INSTALL)
         install (TARGETS display-android
            RUNTIME DESTINATION "${LIB_TARGET}"
            ARCHIVE DESTINATION "${LIB_TARGET}"
            LIBRARY DESTINATION "${LIB_TARGET}")
      endif ()
   endif ()
   # GL2 Link: -ljnigraphics -lEGL -lGLESv2 -DGLES2
   # GL3 Link: -ljnigraphics -lEGL -lGLESv3 -DGLES3
else ()
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-strict-overflow")
	set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-strict-overflow")

   target_sources (${MOD} PRIVATE ${FUNCTIONS} ${DISPLAY_DRIVERS} ${CLASSES} "linux/controller.cpp")

   if (X11_FOUND)
      # Apt package requirements: libxrandr-dev libxxf86vm-dev

      target_compile_definitions (${MOD} PRIVATE DISPLAY_X11_DRIVER)
      add_library (display-x11)
      target_include_directories (display-x11 PRIVATE "${PROJECT_SOURCE_DIR}/include")
      set_target_properties (display-x11 PROPERTIES CXX_STANDARD 20)
      set_target_properties (display-x11 PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
      if (NOT KOTUKU_STATIC)
         target_link_options (display-x11 PRIVATE
            "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/drivers/x11/exports.map")
         set_target_properties (display-x11 PROPERTIES
            PREFIX ""
            POSITION_INDEPENDENT_CODE ON
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/lib")
         if (KOTUKU_INSTALL)
            install (TARGETS display-x11
               RUNTIME DESTINATION "${LIB_TARGET}"
               ARCHIVE DESTINATION "${LIB_TARGET}"
               LIBRARY DESTINATION "${LIB_TARGET}")
         endif ()
      endif ()
      add_dependencies (display-x11 ${REQUIRED_MODS})
      target_sources (display-x11 PRIVATE
         "drivers/x11/module.cpp"
         "drivers/x11/x11_driver.cpp"
         "drivers/x11/handlers.cpp"
         "drivers/x11/lib_pixels.cpp")
      target_link_libraries (display-x11 PRIVATE unicode Xrender Xext X11)
      set (X11_DRIVER_DEFINITIONS)
      if (X11_Xrandr_FOUND)
         target_link_libraries (display-x11 PRIVATE Xrandr)
         list (APPEND X11_DRIVER_DEFINITIONS XRANDR_ENABLED)
      endif ()
      # Applied at target level because x11_native.h guards its extension headers on these definitions, and every
      # file that includes it must agree on the layout of the structures that the header declares.

      if (X11_DRIVER_DEFINITIONS)
         target_compile_definitions (display-x11 PRIVATE ${X11_DRIVER_DEFINITIONS})
      endif ()

      if (KOTUKU_STATIC)
         target_link_libraries (${MOD} PRIVATE display-x11)
      endif ()

      if (UNIT_TESTS AND NOT KOTUKU_STATIC)
         add_executable (test_display_x11_driver "tests/test_x11_driver.cpp")
         target_link_libraries (test_display_x11_driver PRIVATE display-x11)
         set_target_properties (test_display_x11_driver PROPERTIES CXX_STANDARD 20)
         add_test (NAME display_x11_driver COMMAND test_display_x11_driver)
         set_tests_properties (display_x11_driver PROPERTIES LABELS display)
      endif ()
   endif ()
endif ()
