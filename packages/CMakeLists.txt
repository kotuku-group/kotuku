# Generate the import index and install each package in its versioned directory.

# Only the supported bundled libraries are published.  Locally present, unpublished manifests remain private.

set (BUNDLED_PACKAGES
   benchmark button checkbox colourdialog combobox config dialog divider dropdown filedialog filemgr filesearch
   fileview fontdialog git gui httpserver icon input json menu menubar options proxyserver scrollbar slider tempus
   toolbar tooltip url vfx view window)

set (PACKAGE_MANIFESTS)

foreach (PACKAGE_NAME IN LISTS BUNDLED_PACKAGES)
   set (PACKAGE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/${PACKAGE_NAME}/package.json")
   if (NOT EXISTS "${PACKAGE_MANIFEST}")
      message (FATAL_ERROR "Required bundled package manifest is missing: ${PACKAGE_MANIFEST}")
   endif ()
   list (APPEND PACKAGE_MANIFESTS "${PACKAGE_MANIFEST}")
endforeach ()

list (SORT PACKAGE_MANIFESTS)

# Reconfigure when a supported manifest changes.  New bundled packages must be added to BUNDLED_PACKAGES.

set_property (DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PACKAGE_MANIFESTS})

function (package_json_string OUTPUT MANIFEST_JSON MANIFEST_PATH)
   string (JSON VALUE_TYPE ERROR_VARIABLE JSON_ERROR TYPE "${MANIFEST_JSON}" ${ARGN})
   if (NOT JSON_ERROR STREQUAL "NOTFOUND")
      message (FATAL_ERROR "Unable to read ${ARGN} from ${MANIFEST_PATH}: ${JSON_ERROR}")
   endif ()
   if (NOT VALUE_TYPE STREQUAL "STRING")
      message (FATAL_ERROR "The ${ARGN} value in ${MANIFEST_PATH} must be a string")
   endif ()

   string (JSON VALUE GET "${MANIFEST_JSON}" ${ARGN})
   set (${OUTPUT} "${VALUE}" PARENT_SCOPE)
endfunction ()

set (PACKAGE_NAME_PATTERN "^[a-z0-9]+(-[a-z0-9]+)*(/[a-z0-9]+(-[a-z0-9]+)*)*$")
set (PACKAGE_VERSION_PATTERN "^(0|[1-9][0-9]*)(\\.(0|[1-9][0-9]*))*$")
set (PACKAGE_PATH_PATTERN "^([A-Za-z0-9][A-Za-z0-9._-]*/)*[A-Za-z0-9][A-Za-z0-9._-]*$")
set (PACKAGE_INDEX_ENTRIES)

foreach (PACKAGE_MANIFEST IN LISTS PACKAGE_MANIFESTS)
   file (READ "${PACKAGE_MANIFEST}" MANIFEST_JSON)
   package_json_string (PACKAGE_NAME "${MANIFEST_JSON}" "${PACKAGE_MANIFEST}" name)
   package_json_string (PACKAGE_VERSION "${MANIFEST_JSON}" "${PACKAGE_MANIFEST}" version)
   package_json_string (PACKAGE_ENTRY "${MANIFEST_JSON}" "${PACKAGE_MANIFEST}" entry)

   if (NOT PACKAGE_NAME MATCHES "${PACKAGE_NAME_PATTERN}")
      message (FATAL_ERROR "Invalid package name '${PACKAGE_NAME}' in ${PACKAGE_MANIFEST}")
   endif ()
   if (NOT PACKAGE_VERSION MATCHES "${PACKAGE_VERSION_PATTERN}")
      message (FATAL_ERROR "Invalid package version '${PACKAGE_VERSION}' in ${PACKAGE_MANIFEST}")
   endif ()
   if (NOT PACKAGE_ENTRY MATCHES "${PACKAGE_PATH_PATTERN}" OR NOT PACKAGE_ENTRY MATCHES "\\.tiri$")
      message (FATAL_ERROR "Invalid package entry '${PACKAGE_ENTRY}' in ${PACKAGE_MANIFEST}")
   endif ()

   get_filename_component (PACKAGE_FOLDER "${PACKAGE_MANIFEST}" DIRECTORY)
   get_filename_component (PACKAGE_FOLDER_NAME "${PACKAGE_FOLDER}" NAME)
   if (NOT PACKAGE_FOLDER_NAME MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
      message (FATAL_ERROR "Invalid package folder name '${PACKAGE_FOLDER_NAME}'")
   endif ()

   string (JSON PACKAGE_FILES_TYPE ERROR_VARIABLE PACKAGE_FILES_ERROR TYPE "${MANIFEST_JSON}" files)
   if (NOT PACKAGE_FILES_ERROR STREQUAL "NOTFOUND" OR NOT PACKAGE_FILES_TYPE STREQUAL "ARRAY")
      message (FATAL_ERROR "The files value in ${PACKAGE_MANIFEST} must be an array")
   endif ()

   string (JSON PACKAGE_FILE_COUNT LENGTH "${MANIFEST_JSON}" files)
   if (PACKAGE_FILE_COUNT EQUAL 0)
      message (FATAL_ERROR "The files array in ${PACKAGE_MANIFEST} cannot be empty")
   endif ()

   set (PACKAGE_FILES)
   math (EXPR LAST_PACKAGE_FILE_INDEX "${PACKAGE_FILE_COUNT} - 1")
   foreach (PACKAGE_FILE_INDEX RANGE 0 ${LAST_PACKAGE_FILE_INDEX})
      package_json_string (PACKAGE_FILE "${MANIFEST_JSON}" "${PACKAGE_MANIFEST}"
         files ${PACKAGE_FILE_INDEX} path)
      if (NOT PACKAGE_FILE MATCHES "${PACKAGE_PATH_PATTERN}")
         message (FATAL_ERROR "Invalid payload path '${PACKAGE_FILE}' in ${PACKAGE_MANIFEST}")
      endif ()
      if (NOT EXISTS "${PACKAGE_FOLDER}/${PACKAGE_FILE}")
         message (FATAL_ERROR "Payload path '${PACKAGE_FILE}' does not exist for ${PACKAGE_MANIFEST}")
      endif ()
      if (PACKAGE_FILE IN_LIST PACKAGE_FILES)
         message (FATAL_ERROR "Payload path '${PACKAGE_FILE}' is repeated in ${PACKAGE_MANIFEST}")
      endif ()
      list (APPEND PACKAGE_FILES "${PACKAGE_FILE}")
   endforeach ()

   if (NOT PACKAGE_ENTRY IN_LIST PACKAGE_FILES)
      message (FATAL_ERROR "Package entry '${PACKAGE_ENTRY}' is not listed in ${PACKAGE_MANIFEST}")
   endif ()

   macro (register_package_export IMPORT_NAME ENTRY_PATH)
      if (NOT "${IMPORT_NAME}" MATCHES "${PACKAGE_NAME_PATTERN}")
         message (FATAL_ERROR "Invalid export name '${IMPORT_NAME}' in ${PACKAGE_MANIFEST}")
      endif ()
      if (NOT "${ENTRY_PATH}" MATCHES "${PACKAGE_PATH_PATTERN}" OR NOT "${ENTRY_PATH}" MATCHES "\\.tiri$")
         message (FATAL_ERROR "Invalid export path '${ENTRY_PATH}' in ${PACKAGE_MANIFEST}")
      endif ()
      if (NOT "${ENTRY_PATH}" IN_LIST PACKAGE_FILES)
         message (FATAL_ERROR "Export path '${ENTRY_PATH}' is not listed in ${PACKAGE_MANIFEST}")
      endif ()

      list (APPEND PACKAGE_INDEX_ENTRIES
         "${IMPORT_NAME}!${PACKAGE_VERSION}!${PACKAGE_FOLDER_NAME}/${PACKAGE_VERSION}/${ENTRY_PATH}")
   endmacro ()

   string (JSON EXPORTS_TYPE ERROR_VARIABLE EXPORTS_ERROR TYPE "${MANIFEST_JSON}" exports)
   if (EXPORTS_ERROR STREQUAL "NOTFOUND")
      if (NOT EXPORTS_TYPE STREQUAL "ARRAY")
         message (FATAL_ERROR "The exports value in ${PACKAGE_MANIFEST} must be an array")
      endif ()

      string (JSON EXPORT_COUNT LENGTH "${MANIFEST_JSON}" exports)
      if (EXPORT_COUNT EQUAL 0)
         message (FATAL_ERROR "The exports array in ${PACKAGE_MANIFEST} cannot be empty")
      endif ()

      set (PRIMARY_EXPORT_FOUND FALSE)
      math (EXPR LAST_EXPORT_INDEX "${EXPORT_COUNT} - 1")
      foreach (EXPORT_INDEX RANGE 0 ${LAST_EXPORT_INDEX})
         string (JSON EXPORT_TYPE TYPE "${MANIFEST_JSON}" exports ${EXPORT_INDEX})
         string (JSON EXPORT_MEMBER_COUNT LENGTH "${MANIFEST_JSON}" exports ${EXPORT_INDEX})
         if (NOT EXPORT_TYPE STREQUAL "OBJECT" OR NOT EXPORT_MEMBER_COUNT EQUAL 1)
            message (FATAL_ERROR "Each export in ${PACKAGE_MANIFEST} must contain one import-to-path mapping")
         endif ()

         string (JSON EXPORT_NAME MEMBER "${MANIFEST_JSON}" exports ${EXPORT_INDEX} 0)
         package_json_string (EXPORT_PATH "${MANIFEST_JSON}" "${PACKAGE_MANIFEST}"
            exports ${EXPORT_INDEX} "${EXPORT_NAME}")
         register_package_export ("${EXPORT_NAME}" "${EXPORT_PATH}")

         if (EXPORT_NAME STREQUAL PACKAGE_NAME AND EXPORT_PATH STREQUAL PACKAGE_ENTRY)
            set (PRIMARY_EXPORT_FOUND TRUE)
         endif ()
      endforeach ()

      if (NOT PRIMARY_EXPORT_FOUND)
         message (FATAL_ERROR
            "The exports in ${PACKAGE_MANIFEST} do not map '${PACKAGE_NAME}' to '${PACKAGE_ENTRY}'")
      endif ()
   else ()
      register_package_export ("${PACKAGE_NAME}" "${PACKAGE_ENTRY}")
   endif ()

   if (KOTUKU_INSTALL)
      set (PACKAGE_INSTALL_DIRECTORY "${SHARE_TARGET}/packages/${PACKAGE_FOLDER_NAME}/${PACKAGE_VERSION}")
      if (LINKED_PACKAGES)
         set (PACKAGE_INSTALL_PARENT "${SHARE_TARGET}/packages/${PACKAGE_FOLDER_NAME}")
         string (CONFIGURE [[
            set (package_parent "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/@PACKAGE_INSTALL_PARENT@")
            set (package_link "${package_parent}/@PACKAGE_VERSION@")
            file (MAKE_DIRECTORY "${package_parent}")
            if (IS_SYMLINK "${package_link}")
               file (READ_SYMLINK "${package_link}" package_target)
               if (NOT package_target STREQUAL "@PACKAGE_FOLDER@")
                  file (REMOVE "${package_link}")
               endif ()
            elseif (EXISTS "${package_link}")
               message (FATAL_ERROR
                  "Cannot link package: ${package_link} is a real directory or file; remove it or use a clean prefix")
            endif ()
            if (NOT IS_SYMLINK "${package_link}")
               file (CREATE_LINK "@PACKAGE_FOLDER@" "${package_link}" SYMBOLIC RESULT package_link_error)
               if (NOT package_link_error STREQUAL "0")
                  message (FATAL_ERROR "Cannot link package ${package_link}: ${package_link_error}")
               endif ()
            endif ()
         ]] PACKAGE_LINK_CODE @ONLY)
         install (CODE "${PACKAGE_LINK_CODE}")
      else ()
         install (FILES "${PACKAGE_MANIFEST}" DESTINATION "${PACKAGE_INSTALL_DIRECTORY}")
         foreach (PACKAGE_FILE IN LISTS PACKAGE_FILES)
            get_filename_component (PACKAGE_FILE_DIRECTORY "${PACKAGE_FILE}" DIRECTORY)
            if (PACKAGE_FILE_DIRECTORY STREQUAL "")
               set (PACKAGE_FILE_DESTINATION "${PACKAGE_INSTALL_DIRECTORY}")
            else ()
               set (PACKAGE_FILE_DESTINATION "${PACKAGE_INSTALL_DIRECTORY}/${PACKAGE_FILE_DIRECTORY}")
            endif ()
            install (FILES "${PACKAGE_FOLDER}/${PACKAGE_FILE}" DESTINATION "${PACKAGE_FILE_DESTINATION}")
         endforeach ()
      endif ()
   endif ()
endforeach ()

list (SORT PACKAGE_INDEX_ENTRIES)

set (PACKAGE_INDEX_CONTENT "# Generated from package manifests.  Do not edit.\n")
set (PREVIOUS_IMPORT_NAME)
set (PREVIOUS_IMPORT_VERSION)

foreach (PACKAGE_INDEX_ENTRY IN LISTS PACKAGE_INDEX_ENTRIES)
   string (REPLACE "!" ";" PACKAGE_INDEX_FIELDS "${PACKAGE_INDEX_ENTRY}")
   list (GET PACKAGE_INDEX_FIELDS 0 IMPORT_NAME)
   list (GET PACKAGE_INDEX_FIELDS 1 IMPORT_VERSION)
   list (GET PACKAGE_INDEX_FIELDS 2 IMPORT_PATH)

   if (IMPORT_NAME STREQUAL PREVIOUS_IMPORT_NAME AND IMPORT_VERSION STREQUAL PREVIOUS_IMPORT_VERSION)
      message (FATAL_ERROR "Multiple packages export '${IMPORT_NAME}' version '${IMPORT_VERSION}'")
   endif ()

   if (NOT IMPORT_NAME STREQUAL PREVIOUS_IMPORT_NAME)
      if (NOT PREVIOUS_IMPORT_NAME STREQUAL "")
         string (APPEND PACKAGE_INDEX_CONTENT "\n")
      endif ()
      string (APPEND PACKAGE_INDEX_CONTENT "[${IMPORT_NAME}]\n")
   endif ()

   string (APPEND PACKAGE_INDEX_CONTENT "${IMPORT_VERSION} = ${IMPORT_PATH}\n")
   set (PREVIOUS_IMPORT_NAME "${IMPORT_NAME}")
   set (PREVIOUS_IMPORT_VERSION "${IMPORT_VERSION}")
endforeach ()

set (PACKAGE_INDEX "${CMAKE_CURRENT_BINARY_DIR}/index.cfg")
file (CONFIGURE OUTPUT "${PACKAGE_INDEX}" CONTENT "${PACKAGE_INDEX_CONTENT}" @ONLY NEWLINE_STYLE UNIX)

if (KOTUKU_INSTALL)
   install (FILES "${PACKAGE_INDEX}" DESTINATION "${SHARE_TARGET}/packages")
endif ()
