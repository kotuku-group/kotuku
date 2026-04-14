#!/bin/sh

# Cloud sessions only
if [ "$CLAUDE_CODE_REMOTE" != "true" ]; then
  exit 0
fi

# Download the latest binaries from GitHub.  These can't be cached locally, they go out of date too quick.
tools/codex_setup.sh

# Pre-build saves Codex from having to do that step.
cmake -S . -B build/agents -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=build/agents-install -DRUN_ANYWHERE=TRUE \
  -DKOTUKU_STATIC=ON -DDISABLE_SCINTILLA=ON -DDISABLE_AUDIO=ON -DDISABLE_X11=ON -DDISABLE_DISPLAY=ON \
  -DDISABLE_PICTURE=ON -DDISABLE_JPEG=ON -DBUILD_DEFS=OFF -DINSTALL_TESTS=OFF
cmake --build build/agents --config Debug --parallel
cmake --install build/agents

exit 0
