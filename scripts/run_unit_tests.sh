#!/bin/bash

# This file is made to support the unit tests workflow.
# It should only require the directories build/tests, scripts/, and tests/ to function,
# and cmake (with ctest) installed.
# (otherwise, update the workflow too, but try to avoid to keep things self-contained)

ROOT_DIR="$(dirname "$0")/.."

cd "${ROOT_DIR}" || exit 1

# Run the whole suite, excluding tests tagged [NotWorking].
# --no-tests=error fails the job if the filter matches nothing (instead of passing green).
ctest --test-dir build/tests -LE "NotWorking" --no-tests=error --output-junit "$(pwd)/ctest_results.xml" --output-on-failure -j
