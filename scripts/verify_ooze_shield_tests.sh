#!/usr/bin/env bash
# Run OozeShield fff_print tests (requires a prior Release build of fff_print_tests).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DOCKER_IMAGE="${ORCA_DOCKER_IMAGE:-orcaslicer-linux-builder:ubuntu-24.04--cmake-4.3.0--cfacc8808240}"
CONTAINER_ROOT="/__w/OrcaSlicer/OrcaSlicer"

run_ctest() {
    # Catch2 tag is OozeShield; use ctest -L (not -R, which matches test titles only).
    ctest --test-dir build/tests -L OozeShield --output-on-failure
}

build_tests_if_missing() {
  if [[ ! -x build/tests/fff_print/Release/fff_print_tests ]]; then
    echo "Building fff_print_tests (Release)..." >&2
    cmake --build build --config Release --target fff_print_tests -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
  fi
}

if [[ -f build/CMakeCache.txt ]] && grep -q "${CONTAINER_ROOT}/build" build/CMakeCache.txt 2>/dev/null; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "Build tree was configured inside Docker (${CONTAINER_ROOT}); docker is required to run ctest here." >&2
        exit 1
    fi
    exec docker run --rm -i \
        -v "${ROOT}:${CONTAINER_ROOT}" \
        -w "${CONTAINER_ROOT}" \
        -e "CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
        "${DOCKER_IMAGE}" \
        bash -c 'cmake --build build --config Release --target fff_print_tests -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}" && ctest --test-dir build/tests -L OozeShield --output-on-failure'
fi

build_tests_if_missing
run_ctest
