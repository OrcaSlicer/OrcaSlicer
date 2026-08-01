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
# /OrcaSlicer/build/package/bin/orca-slicer (copy it out with a follow-up
# `docker run --rm -v orcacad_buildcache:/b alpine cp ...` or via this script's tail).
# Rig build traps already paid for once each (stale project, NLopt cache, pybind11, OCCT_LIBS, SLIC3R_CAD gate): docs/rig_build_traps.md
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
#
# build_linux.sh must be mounted for the same reason, and here the stale copy is guaranteed
# wrong rather than merely risky: orcacad-deps is layered on snaporca-deps, so the baked script
# is the OTHER fork's and builds `--target Snapmaker_Orca`. This fork's target is `OrcaSlicer`,
# so without this mount configure succeeds and then ninja dies on "unknown target".
# scripts/ likewise: build_linux.sh's packaging step sources scripts/appimage_lib_policy.sh,
# which the baked snaporca tree does not have, so a fully successful link still exited
# non-zero with "missing AppImage helper" and the binary check never ran.
docker run --rm \
  -v "$REPO/src":/OrcaSlicer/src \
  -v "$REPO/resources":/OrcaSlicer/resources \
  -v "$REPO/CMakeLists.txt":/OrcaSlicer/CMakeLists.txt \
  -v "$REPO/cmake":/OrcaSlicer/cmake \
  -v "$REPO/deps_src":/OrcaSlicer/deps_src \
  -v "$REPO/build_linux.sh":/OrcaSlicer/build_linux.sh \
  -v "$REPO/scripts":/OrcaSlicer/scripts \
  -v "$BUILD_VOL":/OrcaSlicer/build \
  "$IMAGE" \
  bash -lc 'cd /OrcaSlicer && ./build_linux.sh -sr'

# src/CMakeLists.txt:151 renames the OrcaSlicer target's output to "orca-slicer" — not
# "snapmaker-orca", which is the other fork's binary name.
echo "=== build finished; checking for binary ==="
docker run --rm -v "$BUILD_VOL":/b "$IMAGE" \
  bash -lc 'ls -lh /b/package/bin/orca-slicer 2>/dev/null && file /b/package/bin/orca-slicer || echo "NO BINARY"'
