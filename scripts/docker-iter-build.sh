#!/usr/bin/env bash
# Incremental slicer build against the orcacad-deps base image.
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
#   IMAGE=orcacad-deps scripts/docker-iter-build.sh
#
# On success the binary is inside the persistent volume at
# /OrcaSlicer/build/package/bin/snapmaker-orca (copy it out with a follow-up
# `docker run --rm -v orcacad_buildcache:/b alpine cp ...` or via this script's tail).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# orcacad-deps, NOT snaporca-deps: see the note in kernel-test.sh — the wrong image
# fails at CMake configure, not at link time.
IMAGE="${IMAGE:-orcacad-deps}"
BUILD_VOL="${BUILD_VOL:-orcacad_buildcache}"

echo "REPO=$REPO  IMAGE=$IMAGE  BUILD_VOL=$BUILD_VOL"

# The root CMakeLists.txt and cmake/ must be mounted too, not taken from the baked image:
# they carry the build-time gates (e.g. SLIC3R_CAD -> add_definitions(-DSLIC3R_CAD)) that the
# mounted headers are compiled against. With a stale baked copy the gate silently stays off and
# the build fails with "class GLCanvas3D has no member named set_design_sketch_tool".
docker run --rm \
  -v "$REPO/src":/OrcaSlicer/src \
  -v "$REPO/resources":/OrcaSlicer/resources \
  -v "$REPO/CMakeLists.txt":/OrcaSlicer/CMakeLists.txt \
  -v "$REPO/cmake":/OrcaSlicer/cmake \
  -v "$REPO/deps_src":/OrcaSlicer/deps_src \
  -v "$BUILD_VOL":/OrcaSlicer/build \
  "$IMAGE" \
  bash -lc 'cd /OrcaSlicer && ./build_linux.sh -sr'

echo "=== build finished; checking for binary ==="
docker run --rm -v "$BUILD_VOL":/b "$IMAGE" \
  bash -lc 'ls -lh /b/package/bin/snapmaker-orca 2>/dev/null && file /b/package/bin/snapmaker-orca || echo "NO BINARY"'
