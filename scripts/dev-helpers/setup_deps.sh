# One-time dependencies setup for development environment: install/init the platform toolchain and build

#!/usr/bin/env bash
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

deps_log() { echo "[setup_deps] $*"; }

detect_os() {
    case "$OSTYPE" in
        msys*|cygwin*|mingw*) echo "windows" ;;
        darwin*)               echo "macos" ;;
        *)                     echo "linux" ;;
    esac
}

deps_dir() {
    case "$(detect_os)" in
        macos) echo "$ROOT/deps/build/$(uname -m)/OrcaSlicer_dep/usr/local" ;;
        *)     echo "$ROOT/deps/build/OrcaSlicer_dep/usr/local" ;;
    esac
}

file_mtime() {
    case "$(detect_os)" in
        macos) stat -f %m "$1" ;;
        *)     stat -c %Y "$1" ;;
    esac
}

check_if_deps_stale() {
    local cfg cache
    cfg="$(file_mtime "$ROOT/deps/CMakeLists.txt" 2>/dev/null || echo 0)"
    cache="$(file_mtime "$ROOT/deps/build/CMakeCache.txt" 2>/dev/null || echo 0)"
    [[ -n "$cfg" && "$cfg" -gt "$cache" ]]
}

rebuild_hint() {
    case "$(detect_os)" in
        windows) echo "scripts/pipeline-helpers/build_release_vs.bat deps" ;;
        linux)   echo "scripts/pipeline-helpers/build_linux.sh -d" ;;
        macos)   echo "scripts/pipeline-helpers/build_release_macos.sh -d" ;;
    esac
}

build_deps() {
    case "$(detect_os)" in
        windows)
            # Import MSVC env on Windows (INCLUDE/LIB unset in a plain Git Bash).
            source "$ROOT/scripts/pipeline-helpers/msvc_env.sh"
            if ! import_msvc_env "[setup_deps] "; then
                deps_log "ERROR: Visual Studio with the C++ workload was not found." >&2
                exit 1
            fi
            deps_log "MSVC environment initialized."
            cd "$ROOT"
            cmd //c "scripts/pipeline-helpers/build_release_vs.bat deps"
            ;;
        linux)
            # -u installs system dependencies (may prompt for sudo), -d builds deps, -r skips RAM/disk checks.
            cd "$ROOT"
            "$ROOT/scripts/pipeline-helpers/build_linux.sh" -udr
            ;;
        macos)
            cd "$ROOT"
            "$ROOT/scripts/pipeline-helpers/build_release_macos.sh" -d
            ;;
    esac
}

ensure_deps() {
    local d
    d="$(deps_dir)"
    if [[ -e "$d" ]]; then
        deps_log "Dependencies already present at $d."
        if check_if_deps_stale; then
            deps_log "WARNING: deps/CMakeLists.txt is newer than the built dependencies, so they may be stale."
            deps_log "         Rebuild with:  $(rebuild_hint)"
        else
            deps_log "Dependencies look up to date."
        fi
    else
        deps_log "Dependencies not found - building now. The first build takes ~30-90 min."
        build_deps
        deps_log "Dependencies built."
    fi
}

# Run standalone only when executed, not when sourced by dev-build_and_run.sh.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    ensure_deps
    deps_log "Setup complete. Build with dev-build_and_run.sh"
fi
