#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
RUN_TESTS="${RUN_TESTS:-0}" # set RUN_TESTS=1 to enable tests

printf '==> Configure (%s)\n' "${BUILD_TYPE}"
if [[ "${RUN_TESTS}" == "1" ]]; then
  cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DBUILD_TESTS=ON
else
  cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
fi

printf '==> Build OrcaSlicer\n'
cmake --build "${BUILD_DIR}" --target OrcaSlicer --config "${BUILD_TYPE}" --parallel

if [[ "${RUN_TESTS}" == "1" ]]; then
  printf '==> Build tests\n'
  cmake --build "${BUILD_DIR}" --target tests --config "${BUILD_TYPE}" --parallel

  printf '==> Run tests\n'
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

printf '✅ Pre-commit validation passed.\n'
