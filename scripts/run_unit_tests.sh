#!/bin/bash

# This file is made to support the unit tests workflow.
# It should only require the directories build/tests, scripts/, and tests/ to function,
# and cmake (with ctest) installed -- plus network access to PyPI whenever numpy has to
# be installed into a freshly built test tree (see below).
# (otherwise, update the workflow too, but try to avoid to keep things self-contained)
#
# Usage: run_unit_tests.sh [TEST_DIR] [BUILD_CONFIG]
#   TEST_DIR      directory containing the built tests (default: build/tests)
#   BUILD_CONFIG  configuration to run; required for multi-config generators, which all
#                 build scripts use (build_linux.sh too: Ninja Multi-Config). Without it,
#                 tests registered with plain add_test() lose their labels and report "Not Run".

ROOT_DIR="$(dirname "$0")/.."

cd "${ROOT_DIR}" || exit 1

TEST_DIR="${1:-build/tests}"
BUILD_CONFIG="${2:-}"

# The slic3rutils plugin-host tests build numpy arrays through the CPython copied next
# to the test binary (see tests/slic3rutils/CMakeLists.txt), which ships no numpy.
# Install it with the uv staged beside that runtime -- the tool the app installs plugin
# dependencies with -- straight into the interpreter's own site-packages: no pip needed
# in the runtime, no PYTHONPATH. Re-checked every run because a rebuild of the test
# target re-copies the runtime; needs network whenever it installs. Pinned so a numpy
# release cannot change results on its own.
NUMPY_VERSION="2.5.3"

# Without numpy those tests assert the numpy-absent error path instead, so a local run
# only warns. Under CI it fails the run, which would otherwise stay green while silently
# dropping the array coverage. (The Flatpak leg runs this inside `flatpak build`, whose
# minimal environment has no CI, and its offline build stages no uv, so numpy stays
# best-effort there.)
numpy_unavailable() {
    if [ -n "${CI:-}" ]; then
        echo "error: $1" >&2
        exit 1
    fi
    echo "warning: $1; the numpy-backed binding tests will cover only the numpy-absent path."
}

has_pinned_numpy() {
    "${python_exe}" -c "import sys, numpy; sys.exit(numpy.__version__ != '${NUMPY_VERSION}')" >/dev/null 2>&1
}

find_args=("${TEST_DIR}" \( -path '*/python/bin/python3' -o -path '*/python/python.exe' \))
# Multi-config trees hold one copy per configuration; only bootstrap the one being run.
[ -n "${BUILD_CONFIG}" ] && find_args+=(-path "*/${BUILD_CONFIG}/*")
python_exe="$(find "${find_args[@]}" -print -quit 2>/dev/null)"

if [ -z "${python_exe}" ]; then
    numpy_unavailable "no bundled Python under ${TEST_DIR}"
elif ! has_pinned_numpy; then
    # Git Bash resolves this to uv.exe on Windows. Never fall back to a uv on PATH:
    # the app only runs its bundled uv, so the tests must too.
    uv_exe="${python_exe%/python/*}/tools/uv/uv"
    echo "Installing numpy ${NUMPY_VERSION} into the embedded test interpreter (${python_exe})..."
    if [ ! -x "${uv_exe}" ]; then
        numpy_unavailable "no uv staged beside the tests"
    elif ! "${uv_exe}" pip install --python "${python_exe}" --only-binary :all: "numpy==${NUMPY_VERSION}" \
            || ! has_pinned_numpy; then
        numpy_unavailable "could not install numpy ${NUMPY_VERSION} into ${python_exe}"
    fi
fi

# Run the whole suite, excluding tests tagged [NotWorking] and tests labelled RequiresApp,
# which run the built orca-slicer binary that this directory does not contain.
# --no-tests=error fails the job if the filter matches nothing (instead of passing green).
args=(--test-dir "${TEST_DIR}" -LE "NotWorking|RequiresApp" --no-tests=error --output-junit "$(pwd)/ctest_results.xml" --output-on-failure -j)
[ -n "${BUILD_CONFIG}" ] && args+=(--build-config "${BUILD_CONFIG}")
ctest "${args[@]}"
