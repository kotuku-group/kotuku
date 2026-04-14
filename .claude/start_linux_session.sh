#!/bin/sh

# This script is for cloud sessions only

# Claude Cloud runs on Ubuntu 24, kernel 4.4 as of 2025-11-07
# C/C++ Compilers: GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja, Conan
# Utilities: git, make, jq, yq, ripgrep, tmux, vim, nano, curl, and more

if [ "$CLAUDE_CODE_REMOTE" != "true" ]; then
  exit 0
fi

git fetch origin
# Install gh and download the latest Kotuku artifact from GitHub
tools/codex_setup.sh

exit 0
