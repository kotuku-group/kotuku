# Regenerates the localhost SSL certificate fixtures that are staged in tests/ssl and installed to config:ssl.
#
# The fixtures are a permanent part of the repository so that no platform requires the openssl executable in order to
# build or test the Network module.  This script is never run by the build.  Run it manually from a Linux host, then
# commit the results:
#
#    cmake -P src/network/generate_ssl_certificates.cmake
#
# Optional definitions (precede -P): -DOPENSSL_EXECUTABLE=<path> and -DOUTPUT_DIR=<folder>.
#
# The generated files must remain loadable by both the OpenSSL and Windows Schannel backends.  PKCS#12 files use
# AES-256-CBC with a SHA-256 MAC, which both support.

if (NOT OPENSSL_EXECUTABLE)
   find_program (OPENSSL_EXECUTABLE openssl REQUIRED)
endif ()

if (NOT DEFINED OUTPUT_DIR)
   set (OUTPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/tests/ssl")
endif ()

set (FIXTURE_DAYS 36500)
set (FIXTURE_SUBJECT "/CN=localhost")
set (FIXTURE_ALT_NAMES "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1")
set (LOCALHOST_PEM "${OUTPUT_DIR}/localhost.pem")
set (LOCALHOST_KEY_PEM "${OUTPUT_DIR}/localhost-key.pem")
set (LOCALHOST_RSA_KEY_PEM "${OUTPUT_DIR}/localhost-rsa-key.pem")
set (LOCALHOST_EC_PEM "${OUTPUT_DIR}/localhost-ec.pem")
set (LOCALHOST_EC_KEY_PEM "${OUTPUT_DIR}/localhost-ec-key.pem")
set (LOCALHOST_P12 "${OUTPUT_DIR}/localhost.p12")
set (LOCALHOST_PASSWORD_P12 "${OUTPUT_DIR}/localhost-password.p12")

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

message (STATUS "Generating Network SSL test certificates in ${OUTPUT_DIR} with ${OPENSSL_VERSION}")
file (MAKE_DIRECTORY "${OUTPUT_DIR}")
file (REMOVE
   "${LOCALHOST_PEM}"
   "${LOCALHOST_KEY_PEM}"
   "${LOCALHOST_RSA_KEY_PEM}"
   "${LOCALHOST_EC_PEM}"
   "${LOCALHOST_EC_KEY_PEM}"
   "${LOCALHOST_P12}"
   "${LOCALHOST_PASSWORD_P12}")

# RSA certificate with a PKCS#8 private key

run_openssl (req -new -x509 -newkey rsa:2048 -sha256 -nodes
   -keyout "${LOCALHOST_KEY_PEM}"
   -out "${LOCALHOST_PEM}"
   -days ${FIXTURE_DAYS}
   -subj "${FIXTURE_SUBJECT}"
   -addext "${FIXTURE_ALT_NAMES}")

# The same RSA private key in the traditional PKCS#1 form

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

# EC certificate with a traditional SEC1 private key

run_openssl (ecparam -name prime256v1 -genkey -noout -out "${LOCALHOST_EC_KEY_PEM}")
run_openssl (req -new -x509 -sha256
   -key "${LOCALHOST_EC_KEY_PEM}"
   -out "${LOCALHOST_EC_PEM}"
   -days ${FIXTURE_DAYS}
   -subj "${FIXTURE_SUBJECT}"
   -addext "${FIXTURE_ALT_NAMES}")

# PKCS#12 bundles of the RSA certificate, without and with a password

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
