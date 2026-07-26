#!/usr/bin/env bash
# Headless CAD-kernel test loop. THE verification contract for delegated work:
# exit 0 means the kernel builds and the selected Catch2 tags pass. Nothing else counts.
#
# Builds only the `libslic3r_tests` target (not the GUI app), so a round trip is
# minutes, not tens of minutes. Needs no display: every [CadDocument] case builds a
# CadDocument, recompute()s it and asserts on geometry.
#
# Usage:
#   scripts/kernel-test.sh                        # [CadDocument] tags, default volume
#   scripts/kernel-test.sh --tags '[CadDocument],[Sketch]'
#   scripts/kernel-test.sh --vol wt_mirror        # private build cache (parallel workers)
#   scripts/kernel-test.sh --host tommaso@100.103.234.2   # build on a remote host
#
# Parallel workers MUST pass a distinct --vol: two builds sharing one cache corrupt
# each other. A new volume pays one full build; runs after that are incremental.
#
# --host exists because the deps image lives wherever it was first built. It rsyncs this
# working tree to a per-volume staging dir on that host and re-runs this same script
# there, so the verification contract is identical either way. Drop --host once the image
# is present locally.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# orcacad-deps, NOT snaporca-deps: this fork is mainline-based and needs Eigen 5.0.1,
# CGAL 5.6.3, wx 3.3.2 and Python 3.12 Development.Embed, none of which snaporca-deps has.
# With the wrong image CMake dies at configure, which is exactly why this fork went
# M1-M8 without ever compiling (see commit 1633005bba).
IMAGE="${IMAGE:-orcacad-deps}"
# Must NOT default to snaporca_buildcache: that is the other fork's volume, and pointing
# this fork at it makes the two silently trade build artefacts. docker-iter-build.sh had the
# identical defect and was fixed to orcacad_buildcache; this script was missed.
VOL="${BUILD_VOL:-orcacad_kerneltest}"
# No exclusions. Both cases that used to be quarantined now run: the solver SIGABRT on
# circle-line tangency is fixed (snaporca-tkz), and the internal-thread case turned out to have
# correct geometry and a wrong reference in the test (snaporca-kzy). A green run here now means
# the whole CAD suite passed, not "everything except the two we gave up on".
TAGS="${TAGS:-[CadDocument]}"
HOST=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --vol)   VOL="$2";   shift 2 ;;
    --tags)  TAGS="$2";  shift 2 ;;
    --image) IMAGE="$2"; shift 2 ;;
    --host)  HOST="$2";  shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

echo "REPO=$REPO IMAGE=$IMAGE VOL=$VOL TAGS=$TAGS HOST=${HOST:-local}"

if [[ -n "$HOST" ]]; then
  # Stage per-volume so parallel workers never share a remote tree.
  REMOTE="kt-$VOL"   # relative: ssh and rsync both start in the remote home dir
  # SC2029: $REMOTE expanding on the CLIENT is the point -- it is derived from $VOL here,
  # and the remote has no such variable. The rsync destination below expands it the same way.
  # shellcheck disable=SC2029
  ssh "$HOST" "mkdir -p $REMOTE"
  # Only the inputs the build reads. --delete keeps a stale file from a prior worker from
  # silently compiling in.
  rsync -a --delete \
    "$REPO/src" "$REPO/tests" "$REPO/resources" "$REPO/cmake" "$REPO/scripts" \
    "$REPO/CMakeLists.txt" \
    "$HOST:$REMOTE/"
  exec ssh "$HOST" "cd $REMOTE && scripts/kernel-test.sh --vol '$VOL' --tags '$TAGS' --image '$IMAGE'"
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "FATAL: image '$IMAGE' not present locally. Either transfer it, or pass" >&2
  echo "       --host <user@host> to build where the image already exists." >&2
  exit 3
fi

# A private volume is always built CLEAN on first use. Cloning a warm cache from another
# tree looks tempting but is a correctness trap: rsync preserves source mtimes, so ninja
# compares foreign object timestamps against them, decides everything is up to date, and
# relinks stale objects. That produced a binary with NO [CadDocument] tests at all while
# reporting success -- a green run that tested nothing. Pay the one-off full build instead;
# incremental rebuilds within a volume are correct because the mtimes then share a lineage.
if ! docker volume inspect "$VOL" >/dev/null 2>&1; then
  echo "=== $VOL: new volume, clean configure + full build (one-off, slow) ==="
  docker volume create "$VOL" >/dev/null
fi

# tests/ is mounted too -- unlike docker-iter-build.sh, this script exists precisely to
# compile tests being edited. CMakeLists.txt and cmake/ carry the SLIC3R_CAD gate; taking
# them from the baked image instead leaves the gate off and the CAD symbols vanish.
docker run --rm \
  -v "$REPO/src":/OrcaSlicer/src \
  -v "$REPO/tests":/OrcaSlicer/tests \
  -v "$REPO/resources":/OrcaSlicer/resources \
  -v "$REPO/CMakeLists.txt":/OrcaSlicer/CMakeLists.txt \
  -v "$REPO/cmake":/OrcaSlicer/cmake \
  -v "$REPO/deps_src":/OrcaSlicer/deps_src \
  -v "$VOL":/OrcaSlicer/build \
  "$IMAGE" \
  bash -lc "set -e
    cd /OrcaSlicer
    DESTDIR=/OrcaSlicer/deps/build/destdir/usr/local
    export PATH=\$DESTDIR/bin:\$PATH   # wx-config, and anything else the deps prefix ships
    # Configure unconditionally. Guarding on 'CMakeCache.txt exists' is wrong: a FAILED
    # configure still writes that file, so the guard then skips reconfiguring forever and
    # every later run reuses a poisoned cache while ignoring corrected flags. A no-op
    # reconfigure costs seconds; that bug costs an afternoon.
    # SLIC3R_GTK=3 and BUILD_TESTS=ON are not optional extras: src/CMakeLists.txt turns
    # SLIC3R_GTK into 'wx-config --toolkit=gtk<N>', so omitting it asks for toolkit 'gtk'
    # and no wx build matches -> 'Could NOT find wxWidgets'. BUILD_TESTS=ON is what makes
    # the libslic3r_tests target exist at all. build_linux.sh sets both (lines 218, 226).
    cmake -S . -B build -G 'Ninja Multi-Config' \
      -DCMAKE_PREFIX_PATH=\$DESTDIR \
      -DwxWidgets_CONFIG_EXECUTABLE=\$DESTDIR/bin/wx-config \
      -DSLIC3R_GTK=3 -DBUILD_TESTS=ON \
      -DSLIC3R_CAD=ON -DSLIC3R_STATIC=1 -DORCA_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release --target libslic3r_tests
    ./build/tests/libslic3r/Release/libslic3r_tests '$TAGS' --order decl"
