#!/usr/bin/env bash
set -e

# One-time Windows deps setup: init MSVC env, build deps if missing (skip,
# warn if stale). Idempotent (~2s once deps exist). Run via dev-setup_deps.bat.

cd "$(dirname "$0")"
log() { echo "[dev-setup_deps] $*"; }

setup_msvc_env() {
    # Import MSVC env on Windows (INCLUDE/LIB unset in a plain Git Bash).
    # Source the shared helper next to the release scripts.
    source "$(dirname "$0")/scripts/pipeline-helpers/msvc_env.sh"
    if ! import_msvc_env "[dev-setup_deps] "; then
        log "ERROR: Visual Studio with the C++ workload was not found." >&2
        exit 1
    fi
    log "✅ MSVC environment initialized."
}

check_if_deps_stale() {
    local cfg cache
    cfg="$(stat -c %Y deps/CMakeLists.txt 2>/dev/null || echo 0)"
    cache="$(stat -c %Y deps/build/CMakeCache.txt 2>/dev/null || echo 0)"
    [[ -n "$cfg" && "$cfg" -gt "$cache" ]]
}

ensure_deps() {
    local deps_dir="deps/build/OrcaSlicer_dep/usr/local"
    if [[ -e "$deps_dir" ]]; then
        log "✅ Dependencies already present at deps/build/OrcaSlicer_dep."
        if check_if_deps_stale; then
            log "WARNING: deps/CMakeLists.txt is newer than the built dependencies, so they may be stale."
            log "         Rebuild with:  scripts/pipeline-helpers/build_release_vs.bat deps"
        else
            log "✅ Dependencies look up to date."
        fi
    else
        log "Dependencies not found - building now. The first build takes ~30-90 min."
        cmd //c "scripts/pipeline-helpers/build_release_vs.bat deps"
        log "✅ Dependencies built."
    fi
}

setup_msvc_env
ensure_deps
log "🏁 Setup complete. Build with dev-build_and_run.sh"