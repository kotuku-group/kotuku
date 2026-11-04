if (NOT DEFINED OPENSSL_EXECUTABLE)
   message (FATAL_ERROR "OPENSSL_EXECUTABLE is not defined")
endif ()

if (NOT DEFINED OUTPUT_DIR)
   message (FATAL_ERROR "OUTPUT_DIR is not defined")
endif ()

set (FIXTURE_FORMAT_VERSION 2)
set (STAMP_FILE "${OUTPUT_DIR}/fixture-version.txt")
set (LOCALHOST_PEM "${OUTPUT_DIR}/localhost.pem")
set (LOCALHOST_KEY_PEM "${OUTPUT_DIR}/localhost-key.pem")
set (LOCALHOST_RSA_KEY_PEM "${OUTPUT_DIR}/localhost-rsa-key.pem")
set (LOCALHOST_EC_PEM "${OUTPUT_DIR}/localhost-ec.pem")
set (LOCALHOST_EC_KEY_PEM "${OUTPUT_DIR}/localhost-ec-key.pem")
set (LOCALHOST_P12 "${OUTPUT_DIR}/localhost.p12")
set (LOCALHOST_PASSWORD_P12 "${OUTPUT_DIR}/localhost-password.p12")

file (SHA256 "${CMAKE_CURRENT_LIST_FILE}" GENERATOR_HASH)

function (run_openssl)
   execute_process (
      COMMAND "${OPENSSL_EXECUTABLE}" ${ARGN}
      RESULT_VARIABLE OPENSSL_RESULT
      OUTPUT_VARIABLE OPENSSL_OUTPUT
      ERROR_VARIABLE OPENSSL_ERROR)

   if (NOT OPENSSL_RESULT EQUAL 0)
      message (FATAL_ERROR
         "OpenSSL command failed (${OPENSSL_RESULT}): ${OPENSSL_EXECUTABLE} ${ARGN}\n"
         "${OPENSSL_OUTPUT}${OPENSSL_ERROR}")
   endif ()
endfunction ()

execute_process (
   COMMAND "${OPENSSL_EXECUTABLE}" version
   RESULT_VARIABLE OPENSSL_VERSION_RESULT
   OUTPUT_VARIABLE OPENSSL_VERSION
   ERROR_VARIABLE OPENSSL_VERSION_ERROR
   OUTPUT_STRIP_TRAILING_WHITESPACE)

if (NOT OPENSSL_VERSION_RESULT EQUAL 0)
   message (FATAL_ERROR "Failed to query OpenSSL version: ${OPENSSL_VERSION_ERROR}")
endif ()

set (EXPECTED_STAMP "${FIXTURE_FORMAT_VERSION}\n${GENERATOR_HASH}\n${OPENSSL_VERSION}\n")
set (FIXTURES_CURRENT TRUE)

foreach (FIXTURE IN ITEMS
      "${LOCALHOST_PEM}"
      "${LOCALHOST_KEY_PEM}"
      "${LOCALHOST_RSA_KEY_PEM}"
      "${LOCALHOST_EC_PEM}"
      "${LOCALHOST_EC_KEY_PEM}"
      "${LOCALHOST_P12}"
      "${LOCALHOST_PASSWORD_P12}")
   if (NOT EXISTS "${FIXTURE}")
      set (FIXTURES_CURRENT FALSE)
   endif ()
endforeach ()

if (FIXTURES_CURRENT AND EXISTS "${STAMP_FILE}")
   file (READ "${STAMP_FILE}" EXISTING_STAMP)
   if (EXISTING_STAMP STREQUAL EXPECTED_STAMP)
      return ()
   endif ()
endif ()

message (STATUS "Generating Network SSL test certificates with ${OPENSSL_VERSION}")
file (REMOVE_RECURSE "${OUTPUT_DIR}")
file (MAKE_DIRECTORY "${OUTPUT_DIR}")

run_openssl (req -new -x509 -newkey rsa:2048 -sha256 -nodes
   -keyout "${LOCALHOST_KEY_PEM}"
   -out "${LOCALHOST_PEM}"
   -days 3650
   -subj "/CN=localhost"
   -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1")

execute_process (
   COMMAND "${OPENSSL_EXECUTABLE}" rsa -help
   OUTPUT_VARIABLE OPENSSL_RSA_HELP
   ERROR_VARIABLE OPENSSL_RSA_HELP)

set (OPENSSL_RSA_ARGS rsa -in "${LOCALHOST_KEY_PEM}")
if (OPENSSL_RSA_HELP MATCHES "(^|[ \\r\\n])-traditional([ \\r\\n]|$)")
   list (APPEND OPENSSL_RSA_ARGS -traditional)
endif ()
list (APPEND OPENSSL_RSA_ARGS -out "${LOCALHOST_RSA_KEY_PEM}")
run_openssl (${OPENSSL_RSA_ARGS})

run_openssl (ecparam -name prime256v1 -genkey -noout -out "${LOCALHOST_EC_KEY_PEM}")
run_openssl (req -new -x509 -sha256
   -key "${LOCALHOST_EC_KEY_PEM}"
   -out "${LOCALHOST_EC_PEM}"
   -days 3650
   -subj "/CN=localhost"
   -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1")

run_openssl (pkcs12 -export
   -out "${LOCALHOST_P12}"
   -inkey "${LOCALHOST_KEY_PEM}"
   -in "${LOCALHOST_PEM}"
   -keypbe AES-256-CBC
   -certpbe AES-256-CBC
   -macalg sha256
   -passout pass:)

run_openssl (pkcs12 -export
   -out "${LOCALHOST_PASSWORD_P12}"
   -inkey "${LOCALHOST_KEY_PEM}"
   -in "${LOCALHOST_PEM}"
   -keypbe AES-256-CBC
   -certpbe AES-256-CBC
   -macalg sha256
   -passout pass:kotuku-test)

file (WRITE "${STAMP_FILE}" "${EXPECTED_STAMP}")
