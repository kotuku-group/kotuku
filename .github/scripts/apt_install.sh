#!/usr/bin/env bash
# Installs apt packages on GitHub-hosted Ubuntu runners.  Usage: apt_install.sh [package ...]
#
# The Azure Ubuntu mirror occasionally trickles a single package at a few kB/s, which never trips apt's own timeout
# and has stalled CI for 10+ minutes.  Downloads are therefore run separately under a time limit and retried; killing a
# download-only run is safe because dpkg has not started, and apt resumes partial files on the next attempt.
#
# cmake and build-essential are only installed if the runner image lacks them.  The image's CMake in /usr/local/bin
# shadows the apt copy, so installing it from apt is wasted effort.

set -euo pipefail

TIMEOUT=120
ATTEMPTS=3

pkgs=("$@")
command -v cmake >/dev/null || pkgs+=(cmake)
{ command -v g++ >/dev/null && command -v make >/dev/null; } || pkgs+=(build-essential)

retry() {
   local attempt
   for attempt in $(seq 1 $ATTEMPTS); do
      if sudo timeout $TIMEOUT "$@"; then return 0; fi
      echo "::warning::'$*' timed out or failed (attempt $attempt of $ATTEMPTS)"
   done
   return 1
}

retry apt-get update
if [ ${#pkgs[@]} -gt 0 ]; then
   retry apt-get install -y --download-only "${pkgs[@]}"
   sudo apt-get install -y "${pkgs[@]}"
fi

cmake --version | head -n 1
