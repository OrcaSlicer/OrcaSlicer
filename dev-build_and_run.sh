#!/usr/bin/env bash
set -e

# Usage:
#   Linux/macOS:  ./dev-build_and_run.sh [--no-kill] [file.3mf]
#   Windows(Git Bash):      bash dev-build_and_run.bat [--no-kill] [file.3mf]
#   Overrides:    BUILD_DIR=build CONFIG=RelWithDebInfo CMAKE=cmake TARGET=OrcaSlicer_app_gui

BUILD_DIR="${BUILD_DIR:-build}"
CONFIG="${CONFIG:-RelWithDebInfo}"
TARGET="${TARGET:-OrcaSlicer_app_gui}"
CMAKE="${CMAKE:-cmake}"

KILL_EXISTING=1
_ARGS=()
for a in "$@"; do
    if [[ "$a" == "--no-kill" ]]; then
        KILL_EXISTING=0
    else
        _ARGS+=("$a")
    fi
done
if [[ ${#_ARGS[@]} -gt 0 ]]; then
    set -- "${_ARGS[@]}"
else
    set --
fi

log() { echo "[dev-build_and_run] $*"; }

ensure_bash_on_wsl() {
    # Re-run under Windows Git Bash if executed inside WSL
    if [[ -n "${WSL_DISTRO_NAME:-}" ]]; then
        exec "/mnt/c/Program Files/Git/bin/bash.exe" "$(wslpath -w "$(pwd)")/$(basename "$0")" "$@"
    fi
}

find_cmake() {
    # Fallback CMake lookup if missing from PATH
    if ! command -v "${CMAKE}" >/dev/null 2>&1 && [[ -n "${USERPROFILE:-}" ]] && \
       [[ -e "${USERPROFILE}/tools/cmake-3.31.6-windows-x86_64/bin/cmake.exe" ]]; then
        CMAKE="${USERPROFILE}/tools/cmake-3.31.6-windows-x86_64/bin/cmake.exe"
    fi
}

setup_msvc_env() {
    # Import MSVC environment on Windows (INCLUDE/LIB unset in plain Git Bash).
    # Missing VS is a warning here, not fatal: the build still attempts to run.
    source "$(dirname "$0")/scripts/pipeline-helpers/msvc_env.sh"
    import_msvc_env "[dev-build_and_run] " || true
}

build() {
    "${CMAKE}" --build "${BUILD_DIR}" --config "${CONFIG}" --target "${TARGET}" --parallel
}

kill_orca_slicer() {
    log "killing running OrcaSlicer instances..."
    case "$OSTYPE" in
        msys*|cygwin*|mingw*)
            powershell -NoProfile -Command "Stop-Process -Name orca-slicer -Force -ErrorAction SilentlyContinue" || true
            ;;
        *)
            pkill -f "orca-slicer" 2>/dev/null || true
            ;;
    esac
}

launch_macos() {
    log "launching ${BUILD_DIR}/bin/OrcaSlicer.app"
    open "${BUILD_DIR}/bin/OrcaSlicer.app" "$@"
}

launch_windows() {
    BIN="$(compgen -G "./${BUILD_DIR}/src/orca-slicer*.exe" | head -1)"
    log "launching ${BIN}"
    WIN_BIN="$(cygpath -w "${BIN}")"
    if [[ $# -gt 0 ]]; then
        WIN_ARG="$(cygpath -w "$1")"
        powershell -NoProfile -Command "Start-Process -FilePath '${WIN_BIN}' -ArgumentList '${WIN_ARG}'"
    else
        powershell -NoProfile -Command "Start-Process -FilePath '${WIN_BIN}'"
    fi
}

launch_linux() {
    log "launching ./${BUILD_DIR}/src/orca-slicer"
    ./"${BUILD_DIR}"/src/orca-slicer* "$@" &
}

ensure_bash_on_wsl "$@"
find_cmake
setup_msvc_env

log "config: BUILD_DIR=${BUILD_DIR} CONFIG=${CONFIG} TARGET=${TARGET} CMAKE=${CMAKE}"
log "building..."
build

if [[ "$KILL_EXISTING" == "1" ]]; then
    kill_orca_slicer
fi

case "$OSTYPE" in
    darwin*)              launch_macos "$@" ;;
    msys*|cygwin*|mingw*) launch_windows "$@" ;;
    *)                    launch_linux "$@" ;;
esac