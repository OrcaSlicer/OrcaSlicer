#!/bin/bash

# This file is made to support the unit tests workflow.
# It should only require the directories build/tests, scripts/, and tests/ to function,
# and cmake (with ctest) installed.
# (otherwise, update the workflow too, but try to avoid to keep things self-contained)
#
# Usage: run_unit_tests.sh [TEST_DIR] [BUILD_CONFIG]
#   TEST_DIR      directory containing the built tests (default: build/tests)
#   BUILD_CONFIG  configuration to run; required for multi-config generators
#                 (Windows/macOS), harmless/omitted for single-config (Linux).

ROOT_DIR="$(dirname "$0")/.."

cd "${ROOT_DIR}" || exit 1

TEST_DIR="${1:-build/tests}"
BUILD_CONFIG="${2:-}"

# The slic3rutils plugin-host tests build numpy arrays through the CPython copied next
# to the test binary (see tests/slic3rutils/CMakeLists.txt); that runtime ships pip but
# no numpy, so those tests SKIP without it. Install it into that interpreter's own
# site-packages -- no PYTHONPATH needed, and re-checked every run because a rebuild of
# the test target wipes and re-copies the runtime. Needs network on the first run only;
# a failure here is not fatal, the tests just keep skipping.
find_args=("${TEST_DIR}" \( -path '*/python/bin/python3' -o -path '*/python/python.exe' \))
# Multi-config trees hold one copy per configuration; only bootstrap the one being run.
[ -n "${BUILD_CONFIG}" ] && find_args+=(-path "*/${BUILD_CONFIG}/*")
python_exe="$(find "${find_args[@]}" -print -quit 2>/dev/null)"
if [ -z "${python_exe}" ]; then
    echo "No bundled Python under ${TEST_DIR}; numpy-backed binding tests will skip."
elif ! "${python_exe}" -c "import numpy" >/dev/null 2>&1; then
    echo "Installing numpy into the embedded test interpreter (${python_exe})..."
    "${python_exe}" -m pip install --quiet --disable-pip-version-check --retries 1 "numpy<3" \
        || echo "numpy install failed; numpy-backed binding tests will skip."
fi

# Run the whole suite, excluding tests tagged [NotWorking].
# --no-tests=error fails the job if the filter matches nothing (instead of passing green).
args=(--test-dir "${TEST_DIR}" -LE "NotWorking" --no-tests=error --output-junit "$(pwd)/ctest_results.xml" --output-on-failure -j)
[ -n "${BUILD_CONFIG}" ] && args+=(--build-config "${BUILD_CONFIG}")
ctest "${args[@]}"
