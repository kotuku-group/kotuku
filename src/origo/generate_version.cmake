# Regenerates version.h with the current git commit/branch at BUILD time.
#
# This script is invoked by a custom build step (not at configure time) so that
# `origo --version` always reports the git commit the binary was actually built
# against, rather than whatever HEAD happened to be when CMake last configured.
#
# Required inputs (passed via -D):
#   SRC_DIR            - project source directory (the git working tree root)
#   VERSION_IN         - path to version.h.in template
#   VERSION_OUT        - path to the generated version.h
#   KOTUKU_VERSION_MAJOR / MINOR / PATCH - date-based version components
#   KOTUKU_BUILD_TYPE  - CMake build type string

find_package(Git QUIET)

if (GIT_FOUND AND EXISTS "${SRC_DIR}/.git")
   execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY ${SRC_DIR}
      OUTPUT_VARIABLE GIT_COMMIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
   )
   execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
      WORKING_DIRECTORY ${SRC_DIR}
      OUTPUT_VARIABLE GIT_BRANCH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
   )
endif ()

if (NOT GIT_COMMIT_HASH)
   set(GIT_COMMIT_HASH "unknown")
endif ()
if (NOT GIT_BRANCH)
   set(GIT_BRANCH "unknown")
endif ()

# configure_file() only rewrites the output when its content changes, so this is
# a no-op (and triggers no recompilation) when the git state is unchanged.
configure_file("${VERSION_IN}" "${VERSION_OUT}" @ONLY)
