# HTTP Module

set (MOD http)
set (INC_MOD_HTTP TRUE PARENT_SCOPE)

# Use XXD to convert the auth.tiri script to a C binary file

if (XXD_CMD STREQUAL "XXD_CMD-NOTFOUND")
   if (WIN32)
      # Use PowerShell Format-Hex as fallback on Windows
      add_custom_command (OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/auth.c"
         COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command "$bytes = [System.IO.File]::ReadAllBytes('${CMAKE_CURRENT_SOURCE_DIR}/auth.tiri'); $output = \"unsigned char glAuthScript[] = {`n  \"; for($i=0; $i -lt $bytes.Length; $i++) { if($i % 20 -eq 0 -and $i -ne 0) { $output += \"`n  \" }; $output += '0x' + $bytes[$i].ToString('x2'); if($i -lt $bytes.Length-1) { $output += ',' } }; $output += \"`n};`nunsigned int glAuthScriptLength = $($bytes.Length);`n\"; [System.IO.File]::WriteAllText('${CMAKE_CURRENT_SOURCE_DIR}/auth.c', $output)"
         DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/auth.tiri"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
         COMMENT "Generating C binary from auth.tiri using PowerShell"
         VERBATIM)

      add_custom_target (http_auth DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/auth.c")
      add_dependencies (build_headers http_auth)
   elseif (NOT (OD_CMD STREQUAL "OD_CMD-NOTFOUND"))
      # Linux/Unix fallback using od
      add_custom_command (OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/auth.c"
         COMMAND bash -c "{ printf 'unsigned char glAuthScript[] = {\\n'; od -An -tx1 -v '${CMAKE_CURRENT_SOURCE_DIR}/auth.tiri' | awk '{for(i=1;i<=NF;i++){if(NR>1||i>1)printf \", \"; if((((NR-1)*16+i-1)%20)==0 && (NR>1||i>1))printf \"\\n  \"; printf \"0x%s\",$i}} END{printf \"\\n};\\n\"}'; stat -c 'unsigned int glAuthScriptLength = %s;' '${CMAKE_CURRENT_SOURCE_DIR}/auth.tiri' 2>/dev/null || stat -f 'unsigned int glAuthScriptLength = %z;' '${CMAKE_CURRENT_SOURCE_DIR}/auth.tiri'; } > auth.c"
         DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/auth.tiri"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
         COMMENT "Generating C binary from auth.tiri using od"
         VERBATIM)

      add_custom_target (http_auth DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/auth.c")
      add_dependencies (build_headers http_auth)
   else ()
      message (STATUS "The xxd command is unavailable on this system")
   endif ()
else ()
   add_custom_command (OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/auth.c"
      COMMAND bash -c "\"${XXD_CMD}\" -c20 -i \"${CMAKE_CURRENT_SOURCE_DIR}/auth.tiri\" | sed 's/.*_auth_tiri\\[\\]/unsigned char glAuthScript[]/g;s/.*_auth_tiri_len/unsigned int glAuthScriptLength/g;s/, /,/g;s/ *$$//' > auth.c"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/auth.tiri"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Generating C binary from auth.tiri"
      VERBATIM)

   add_custom_target (http_auth DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/auth.c")
   add_dependencies (build_headers http_auth)
endif ()

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h" APPEND_IDL "${MOD}_def.c")

add_library (${MOD})
target_link_libraries (${MOD} PUBLIC base64)
set_module_defaults (${MOD} "Http")
target_sources (${MOD} PRIVATE "${MOD}.cpp" "auth.c")

flute_test (http "tests/test_http.tiri")
flute_test (http_validate "tests/test_validate_header.tiri")
