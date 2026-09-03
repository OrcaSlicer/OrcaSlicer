#!/usr/bin/env bash
# Build OrcaSlicer, then stop any running instance and launch the new one.
#
# Usage:
#   ./build_linux_and_launch.sh [build_linux.sh options] [--no-kill]
#
# Without options this runs `./build_linux.sh -s` (deps are built first when the
# default tree is missing). Options are passed through to build_linux.sh, e.g.
#   ./build_linux_and_launch.sh -l
#   ./build_linux_and_launch.sh -e
# `--no-kill` skips stopping a running OrcaSlicer before relaunching.
#
# The freshly built binary is launched from the default release tree; a build
# with a non-default config lands elsewhere, so launch it manually in that case.

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

NO_KILL=0
BUILD_ARGS=()
for a in "$@"; do
    if [[ "$a" == "--no-kill" ]]; then
        NO_KILL=1
    else
        BUILD_ARGS+=("$a")
    fi
done

if [[ ${#BUILD_ARGS[@]} -eq 0 ]]; then
    BUILD_ARGS=(-s)
fi

echo "== Building OrcaSlicer with: ./build_linux.sh ${BUILD_ARGS[*]} =="
./build_linux.sh "${BUILD_ARGS[@]}"

APP=""
for cand in "build/src/Release/orca-slicer" "build/src/orca-slicer" "build/package/bin/orca-slicer"; do
    if [[ -x "$cand" ]]; then
        APP="$cand"
        break
    fi
done

if [[ -z "$APP" ]]; then
    echo "Build finished, but no executable found in build/; launch it manually." >&2
    exit 1
fi

if [[ "$NO_KILL" == "1" ]]; then
    echo "dev launch: not stopping a running OrcaSlicer (--no-kill)"
else
    echo "dev launch: stopping running OrcaSlicer instances..."
    pkill -f "orca-slicer" 2>/dev/null || true
fi

echo "dev launch: launching $APP"
"$APP" &
exit 0
