#!/bin/bash

# This file is made to support the unit tests workflow.
# It should only require the directories build/tests, scripts/, and tests/ to function,
# and cmake (with ctest) installed.
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
# to the test binary (see tests/slic3rutils/CMakeLists.txt); that runtime ships no numpy,
# so those tests SKIP without it. Install it into that interpreter's own site-packages --
# no PYTHONPATH needed, and re-checked every run because a rebuild of the test target
# wipes and re-copies the runtime. Needs network on the first run only; a failure here is
# not fatal, the tests just keep skipping.
find_args=("${TEST_DIR}" \( -path '*/python/bin/python3' -o -path '*/python/python.exe' \))
# Multi-config trees hold one copy per configuration; only bootstrap the one being run.
[ -n "${BUILD_CONFIG}" ] && find_args+=(-path "*/${BUILD_CONFIG}/*")
python_exe="$(find "${find_args[@]}" -print -quit 2>/dev/null)"

# Unix stages the runtime with `make install`, which runs ensurepip; the Windows layout
# (deps/python3/stage_windows.cmake) ships no pip, so drive the install from whatever pip
# the host has, resolving wheels for the embedded interpreter's tags and not the host's.
install_numpy_from_host_pip() {
    local py_ver py_abi py_plat py_site host_py
    # tr strips the CR that a Windows interpreter's print() puts on every line.
    { read -r py_ver; read -r py_abi; read -r py_plat; read -r py_site; } < <(
        "${python_exe}" -c 'import sys, sysconfig
v = sys.version_info
print("%d.%d" % v[:2])
print("cp%d%d" % v[:2])
print(sysconfig.get_platform().replace("-", "_").replace(".", "_"))
print(sysconfig.get_paths()["purelib"])' 2>/dev/null | tr -d '\r')
    [ -n "${py_site}" ] || return 1
    for host_py in python3 python py; do
        # Skips a Windows Store stub, which resolves but has no pip behind it.
        "${host_py}" -m pip --version >/dev/null 2>&1 || continue
        "${host_py}" -m pip install --quiet --disable-pip-version-check --retries 1 \
            --target "${py_site}" --only-binary=:all: --implementation cp \
            --python-version "${py_ver}" --abi "${py_abi}" --platform "${py_plat}" \
            "numpy<3"
        return $?
    done
    return 1
}

if [ -z "${python_exe}" ]; then
    echo "No bundled Python under ${TEST_DIR}; numpy-backed binding tests will skip."
elif ! "${python_exe}" -c "import numpy" >/dev/null 2>&1; then
    echo "Installing numpy into the embedded test interpreter (${python_exe})..."
    if "${python_exe}" -m pip --version >/dev/null 2>&1; then
        "${python_exe}" -m pip install --quiet --disable-pip-version-check --retries 1 "numpy<3" \
            || echo "numpy install failed; numpy-backed binding tests will skip."
    else
        install_numpy_from_host_pip \
            || echo "numpy install failed; numpy-backed binding tests will skip."
    fi
fi

# Run the whole suite, excluding tests tagged [NotWorking] and tests labelled RequiresApp,
# which run the built orca-slicer binary that this directory does not contain.
# --no-tests=error fails the job if the filter matches nothing (instead of passing green).
args=(--test-dir "${TEST_DIR}" -LE "NotWorking|RequiresApp" --no-tests=error --output-junit "$(pwd)/ctest_results.xml" --output-on-failure -j)
[ -n "${BUILD_CONFIG}" ] && args+=(--build-config "${BUILD_CONFIG}")
ctest "${args[@]}"
