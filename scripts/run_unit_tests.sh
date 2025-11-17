#!/bin/bash

# This file is made to support the unit tests workflow.
# It should only require the directories build/tests and scripts/ to function,
# and cmake (with ctest) installed.
# (otherwise, update the workflow too, but try to avoid to keep things self-contained)

ROOT_DIR="$(dirname "$0")/.."

cd "${ROOT_DIR}" || exit 1

# TODO: github.com/OrcaSlicer/OrcaSlicer/issues/10309 - Run all tests
ctest --test-dir build/tests/slic3rutils --output-junit "$(pwd)/ctest_results.xml" --output-on-failure
