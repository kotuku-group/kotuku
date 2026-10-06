set (MOD crypto)
set (INC_MOD_CRYPTO TRUE PARENT_SCOPE)

idl_gen ("${MOD}.tdl" NAME ${MOD}_defs OUTPUT "${INCLUDE_OUTPUT}/modules/${MOD}.h"
   ARGS "output-defs=crypto_def.c" "output-proto=crypto_def.c"
   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${MOD}.cpp"
   APPEND_IDL "crypto_def.c")

add_library (${MOD})
set_module_defaults (${MOD} "Crypto")
target_sources (${MOD} PRIVATE crypto.cpp)

if (WIN32)
   target_sources (${MOD} PRIVATE backend_windows.cpp backend_windows_native.cpp)
   target_link_libraries (${MOD} PRIVATE bcrypt advapi32)
else ()
   include (FetchContent)
   FetchContent_Declare (bearssl
      GIT_REPOSITORY https://www.bearssl.org/git/BearSSL
      GIT_TAG 7bea48e5e850ab4cafbe68d3765cdaba13a86d6f)
   FetchContent_MakeAvailable (bearssl)

   set (BEARSSL_ROOT "${bearssl_SOURCE_DIR}")
   target_include_directories (${MOD} PRIVATE "${BEARSSL_ROOT}/inc" "${BEARSSL_ROOT}/src")
   target_sources (${MOD} PRIVATE
      backend_bearssl.cpp
      "${BEARSSL_ROOT}/src/codec/ccopy.c"
      "${BEARSSL_ROOT}/src/codec/dec32be.c"
      "${BEARSSL_ROOT}/src/codec/dec32le.c"
      "${BEARSSL_ROOT}/src/codec/dec64be.c"
      "${BEARSSL_ROOT}/src/codec/enc32be.c"
      "${BEARSSL_ROOT}/src/codec/enc32le.c"
      "${BEARSSL_ROOT}/src/codec/enc64be.c"
      "${BEARSSL_ROOT}/src/hash/md5.c"
      "${BEARSSL_ROOT}/src/hash/sha1.c"
      "${BEARSSL_ROOT}/src/hash/sha2big.c"
      "${BEARSSL_ROOT}/src/hash/sha2small.c"
      "${BEARSSL_ROOT}/src/int/i31_add.c"
      "${BEARSSL_ROOT}/src/int/i31_bitlen.c"
      "${BEARSSL_ROOT}/src/int/i31_decode.c"
      "${BEARSSL_ROOT}/src/int/i31_decmod.c"
      "${BEARSSL_ROOT}/src/int/i31_encode.c"
      "${BEARSSL_ROOT}/src/int/i31_fmont.c"
      "${BEARSSL_ROOT}/src/int/i31_modpow2.c"
      "${BEARSSL_ROOT}/src/int/i31_montmul.c"
      "${BEARSSL_ROOT}/src/int/i31_muladd.c"
      "${BEARSSL_ROOT}/src/int/i31_ninv31.c"
      "${BEARSSL_ROOT}/src/int/i31_sub.c"
      "${BEARSSL_ROOT}/src/int/i31_tmont.c"
      "${BEARSSL_ROOT}/src/int/i32_div32.c"
      "${BEARSSL_ROOT}/src/mac/hmac.c"
      "${BEARSSL_ROOT}/src/rsa/rsa_i31_pkcs1_vrfy.c"
      "${BEARSSL_ROOT}/src/rsa/rsa_i31_pub.c"
      "${BEARSSL_ROOT}/src/rsa/rsa_pkcs1_sig_unpad.c")
endif ()

if (BUILD_TESTS)
   flute_test (crypto_primitives "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_crypto.tiri")
   flute_test (crypto_oauth2_pkce "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_oauth2_pkce.tiri")
endif ()

if (UNIT_TESTS)
   if (WIN32)
      set_target_properties (${MOD} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
   endif ()
   add_executable (test_crypto_native tests/test_primitives.cpp)
   target_link_libraries (test_crypto_native PRIVATE crypto)
   add_test (NAME crypto_native COMMAND test_crypto_native)
   if (WIN32)
      set_tests_properties (crypto_native PROPERTIES
         ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:${MOD}>"
         LABELS crypto)
   else ()
      set_tests_properties (crypto_native PROPERTIES LABELS crypto)
   endif ()
endif ()
