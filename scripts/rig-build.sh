#!/usr/bin/env bash
# Rebuild the GUI binary the design rig launches — in a THROWAWAY container, writing into the
# same build-cache volume the rig's long-lived GUI container reads from.
#
# NEVER build inside the GUI container (snaporca-gui / orcacad-gui). Its baked /OrcaSlicer tree
# is the Jun-13 Snapmaker-derived source, so a `cmake .` in there silently reconfigures the
# shared build dir as project(Snapmaker_Orca) and this fork's targets vanish. That is Trap 1 of
# five; all of them, with symptoms and exact recovery commands, are in docs/rig_build_traps.md.
# Read that file before debugging a configure or link failure this script reports.
#
# Usage:
#   scripts/rig-build.sh              # configure + build the fork's GUI target
#   DRY_RUN=1 scripts/rig-build.sh    # print the resolved fork identity and exit, no container
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Fork identity is DERIVED from the repo, never hardcoded, so this file is byte-identical in
# both forks and cannot be mirrored into the wrong one. Pointing a fork at the other fork's
# image or volume is not a slow failure: with the wrong image CMake dies at configure, and with
# the wrong volume the two forks silently trade build artefacts.
PROJECT="$(sed -n 's/^project(\([A-Za-z_0-9]*\)).*/\1/p' "$REPO/CMakeLists.txt" | head -1)"
case "$PROJECT" in
    Snapmaker_Orca) PREFIX=snaporca; BIN=snapmaker-orca ;;
    OrcaSlicer)     PREFIX=orcacad;  BIN=orca-slicer    ;;
    *) echo "FATAL: unrecognised project($PROJECT) in $REPO/CMakeLists.txt" >&2; exit 2 ;;
esac
TARGET="$PROJECT"
IMAGE="${PREFIX}-deps"
BUILD_VOL="${PREFIX}_buildcache"

echo "REPO=$REPO PROJECT=$PROJECT IMAGE=$IMAGE BUILD_VOL=$BUILD_VOL TARGET=$TARGET BIN=$BIN"

if [ -n "${DRY_RUN:-}" ]; then
    echo "DRY_RUN: resolution only, no container started"
    exit 0
fi

# Three memory bounds. On 2026-08-21 both forks ran this script at the same time, each with
# ninja -j$(nproc)=16: ~36 cc1plus holding 42 GB of a 62 GB box -> global OOM at 21:05, a
# 2h28m kill storm, ssh unreachable, lightdm destroyed, 2946 session kill events. Neither
# build produced a single object. The bounds, weakest to strongest:
#   flock    — the lock path is shared by both forks on purpose, so they SERIALISE instead of
#              summing. Peak is one build's worth no matter who else starts one.
#   -j8      — ~8 TUs in flight, ~10-20 GB, and the box stays usable while it compiles.
#              JOBS=n overrides for a machine with more headroom.
#   --memory — the actual guarantee. A runaway build hits its own cgroup limit and dies alone;
#              the host never reaches global OOM again, whatever -j or flock do.
#              --memory-swap equal to --memory forbids swap, which is what made ssh hang.
JOBS="${JOBS:-8}"
MEM="${MEM:-32g}"
LOCK=/tmp/orca-rig-build.lock

exec 9>"$LOCK"
if ! flock -n 9; then
    echo "another fork's rig-build holds $LOCK — waiting (this is the OOM guard, not a hang)"
    flock 9
fi

# Every one of these mounts covers a trap, none is decorative:
#   CMakeLists.txt + cmake/ carry the SLIC3R_CAD gate — inherit the baked copies and the cache
#     says SLIC3R_CAD=ON while -DSLIC3R_CAD is never defined, so every #ifdef block compiles out.
#   deps_src/ carries pybind11, which the image predates.
#   src/, resources/, localization/, version.inc are the code under test.
rc=0
docker run --rm \
    --memory="$MEM" --memory-swap="$MEM" \
    -v "$REPO/src":/OrcaSlicer/src \
    -v "$REPO/resources":/OrcaSlicer/resources \
    -v "$REPO/cmake":/OrcaSlicer/cmake \
    -v "$REPO/deps_src":/OrcaSlicer/deps_src \
    -v "$REPO/localization":/OrcaSlicer/localization \
    -v "$REPO/CMakeLists.txt":/OrcaSlicer/CMakeLists.txt \
    -v "$REPO/version.inc":/OrcaSlicer/version.inc \
    -v "$BUILD_VOL":/OrcaSlicer/build \
    "$IMAGE" bash -lc "
        cd /OrcaSlicer/build || exit 1
        cmake . > /tmp/cfg.log 2>&1 || { echo 'CONFIGURE FAILED'; tail -25 /tmp/cfg.log; exit 1; }
        # Twice, deliberately. src/libslic3r/CMakeLists.txt publishes OCCT_LIBS as CACHE INTERNAL
        # at the END of its own configure, so a first pass after that list changes links the
        # PREVIOUS one and drops TKBool/TKOffset — a wall of TopOpeBRepBuild undefined references
        # that reads as a broken OCCT install and is not. Trap 4.
        cmake . > /tmp/cfg2.log 2>&1 || { echo 'RECONFIGURE FAILED'; tail -25 /tmp/cfg2.log; exit 1; }
        ninja -f build-Release.ninja -j$JOBS $TARGET > /tmp/bld.log 2>&1
        rc=\$?
        echo \"EXIT=\$rc\"
        grep -n 'error:' /tmp/bld.log | head -20
        tail -4 /tmp/bld.log
        ls -la /OrcaSlicer/build/src/Release/$BIN 2>/dev/null
        exit \$rc
    " || rc=$?

# A target-only build writes src/Release/, but this fork's gui-session.sh may default BIN to the
# PACKAGED path that only build_linux.sh refreshes — launching with the default would then run a
# stale binary. Pass BIN explicitly. See docs/rig_build_traps.md.
echo "=== launch the rig on the binary just built ==="
echo "  docker exec -e BIN=/OrcaSlicer/build/src/Release/$BIN ${PREFIX}-gui /OrcaSlicer/scripts/gui-session.sh"
exit "$rc"
