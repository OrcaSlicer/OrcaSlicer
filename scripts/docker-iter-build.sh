#!/usr/bin/env bash
# Incremental slicer build against the snaporca-deps base image.
#
# The deps-baked image (built from scripts/Dockerfile.deps) carries the pinned
# dependencies at /OrcaSlicer/deps/build/destdir. This script mounts the LIVE source
# tree and resources over the baked copy so code/CMake edits apply immediately, and
# persists /OrcaSlicer/build in a named volume so ninja recompiles only what changed.
#
# Result: edit -> rebuild in seconds-to-minutes instead of a full Docker rebuild.
#
# Usage (run on the build host, e.g. behemoth, from anywhere):
#   scripts/docker-iter-build.sh
#   IMAGE=snaporca-deps scripts/docker-iter-build.sh
#
# On success the binary is inside the persistent volume at
# /OrcaSlicer/build/package/bin/snapmaker-orca (copy it out with a follow-up
# `docker run --rm -v snaporca_buildcache:/b alpine cp ...` or via this script's tail).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-snaporca-deps}"
BUILD_VOL="${BUILD_VOL:-snaporca_buildcache}"

echo "REPO=$REPO  IMAGE=$IMAGE  BUILD_VOL=$BUILD_VOL"

docker run --rm \
  -v "$REPO/src":/OrcaSlicer/src \
  -v "$REPO/resources":/OrcaSlicer/resources \
  -v "$BUILD_VOL":/OrcaSlicer/build \
  "$IMAGE" \
  bash -lc 'cd /OrcaSlicer && ./build_linux.sh -sr'

echo "=== build finished; checking for binary ==="
docker run --rm -v "$BUILD_VOL":/b "$IMAGE" \
  bash -lc 'ls -lh /b/package/bin/snapmaker-orca 2>/dev/null && file /b/package/bin/snapmaker-orca || echo "NO BINARY"'
