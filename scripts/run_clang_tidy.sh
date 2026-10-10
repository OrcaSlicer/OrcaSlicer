#!/usr/bin/env bash
# Runs the clang-tidy check that gates pull requests (.github/workflows/clang_tidy.yml)
# on your branch, so its findings match what CI reports. Linux and macOS.
#
#   scripts/run_clang_tidy.sh            check your changes against OrcaSlicer's main
#   scripts/run_clang_tidy.sh --fix      also add the missing includes it names
#
# It configures a separate build directory (build-tidy) without the precompiled
# header, uses the clang-tidy on your system or installs the pinned one into a
# virtual environment inside it, and runs scripts/clang_tidy_diff.py the way CI
# does. Uncommitted changes are checked too.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run_clang_tidy.sh [options]

  -b, --base REV       revision to compare against (default: main of the remote that
                       points at OrcaSlicer/OrcaSlicer, else origin/main)
      --no-fetch       do not fetch that remote's main first
  -B, --build-dir DIR  build directory for the compile database (default: build-tidy)
  -d, --deps-dir DIR   dependency build directory (default: deps/build on Linux,
                       deps/build/<arch> on macOS)
  -j, --jobs N         parallel clang-tidy runs (default: all cores)
      --fix            apply clang-tidy's fixes (adds the missing includes)
  -y, --yes            install missing tools without asking; another clang-tidy
                       version found on the system is then not offered
  -h, --help           show this help
EOF
}

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BASE=""
FETCH=1
BUILD_DIR=build-tidy
DEPS_DIR=""
JOBS=""
FIX=0
YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        -b|--base) BASE="$2"; shift 2 ;;
        --no-fetch) FETCH=0; shift ;;
        -B|--build-dir) BUILD_DIR="$2"; shift 2 ;;
        -d|--deps-dir) DEPS_DIR="$2"; shift 2 ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        --fix) FIX=1; shift ;;
        -y|--yes) YES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$ROOT/$BUILD_DIR" ;;
esac

OS=$(uname -s)
case "$OS" in
    Linux) ;;
    Darwin) ;;
    *) echo "Unsupported system $OS. On Windows, use scripts/run_clang_tidy.ps1." >&2; exit 1 ;;
esac

# Ask before installing anything. Without a terminal, or with "no", print the
# command instead so it can be run by hand.
ask() {
    [ "$YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    local reply
    read -r -p "$1 [y/N] " reply
    [[ "$reply" =~ ^[Yy] ]]
}

# --- System tools -------------------------------------------------------------

missing=()
command -v git >/dev/null || missing+=(git)
command -v python3 >/dev/null || missing+=(python3)
command -v cmake >/dev/null || missing+=(cmake)
command -v ninja >/dev/null || missing+=(ninja)
if [ "$OS" = Linux ]; then
    # CI builds its compile database with clang; another compiler's flags change what
    # clang-tidy sees.
    command -v clang >/dev/null && command -v clang++ >/dev/null || missing+=(clang)
fi

if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing: ${missing[*]}"
    install_cmd=""
    if [ "$OS" = Darwin ]; then
        pkgs=()
        for m in "${missing[@]}"; do
            case "$m" in
                git|python3|cmake|ninja) pkgs+=("${m/python3/python}") ;;
            esac
        done
        if command -v brew >/dev/null; then
            install_cmd="brew install ${pkgs[*]}"
        else
            echo "Install Homebrew from https://brew.sh, then run: brew install ${pkgs[*]}" >&2
            exit 1
        fi
    elif command -v apt-get >/dev/null; then
        pkgs=()
        for m in "${missing[@]}"; do
            case "$m" in
                ninja) pkgs+=(ninja-build) ;;
                python3) pkgs+=(python3 python3-venv) ;;
                *) pkgs+=("$m") ;;
            esac
        done
        install_cmd="sudo apt-get install -y ${pkgs[*]}"
    elif command -v dnf >/dev/null; then
        install_cmd="sudo dnf install -y ${missing[*]/ninja/ninja-build}"
    elif command -v pacman >/dev/null; then
        install_cmd="sudo pacman -S --needed ${missing[*]/python3/python}"
    else
        echo "Install ${missing[*]} with your package manager and run this again." >&2
        exit 1
    fi
    if ask "Install them now with: $install_cmd ?"; then
        $install_cmd
    else
        echo "To install them yourself, run:" >&2
        echo "    $install_cmd" >&2
        exit 1
    fi
fi

# --- clang-tidy ---------------------------------------------------------------

REQUIREMENTS="$ROOT/scripts/clang_tidy_requirements.txt"
PINNED=$(sed -n 's/^clang-tidy==//p' "$REQUIREMENTS")
VENV="$BUILD_DIR/clang-tidy-venv"

is_pinned() {
    [ -x "$1" ] && "$1" --version 2>/dev/null | grep -q "version $PINNED"
}

CLANG_TIDY="${CLANG_TIDY:-}"
if [ -n "$CLANG_TIDY" ]; then
    # Set by the caller: use it as is.
    is_pinned "$CLANG_TIDY" || echo "Warning: $CLANG_TIDY is not clang-tidy $PINNED, so results may differ from CI." >&2
else
    # One already on the system comes first: the pinned version outright, another
    # version if the user accepts the difference. The pinned version is installed
    # into a virtual environment otherwise.
    INSTALLED=""
    for candidate in $(command -v clang-tidy "clang-tidy-${PINNED%%.*}" || true) \
                     "/usr/lib/llvm-${PINNED%%.*}/bin/clang-tidy" \
                     "$(brew --prefix llvm 2>/dev/null || true)/bin/clang-tidy"; do
        if is_pinned "$candidate"; then
            CLANG_TIDY="$candidate"
            break
        fi
        [ -z "$INSTALLED" ] && [ -x "$candidate" ] && INSTALLED="$candidate"
    done
    if [ -z "$CLANG_TIDY" ] && [ -n "$INSTALLED" ] && [ "$YES" = 0 ] && ! is_pinned "$VENV/bin/clang-tidy"; then
        echo "Found $INSTALLED, which is $("$INSTALLED" --version | sed -n 's/.*version \([0-9.]*\).*/\1/p' | head -n 1), not the $PINNED CI uses, so results may differ slightly."
        if ask "Use it anyway?"; then
            CLANG_TIDY="$INSTALLED"
        fi
    fi
fi
if [ -z "$CLANG_TIDY" ]; then
    CLANG_TIDY="$VENV/bin/clang-tidy"
    if ! is_pinned "$CLANG_TIDY"; then
        if ask "clang-tidy $PINNED (the version CI uses) is not installed. Install it into $VENV?"; then
            mkdir -p "$BUILD_DIR"
            if ! python3 -m venv "$VENV"; then
                echo "Could not create a Python virtual environment." >&2
                [ "$OS" = Linux ] && echo "On Debian or Ubuntu, install it with: sudo apt-get install -y python3-venv" >&2
                exit 1
            fi
            "$VENV/bin/pip" install --quiet --upgrade pip
            "$VENV/bin/pip" install --quiet -r "$REQUIREMENTS"
        else
            echo "To install it yourself, run:" >&2
            echo "    python3 -m venv $VENV && $VENV/bin/pip install -r scripts/clang_tidy_requirements.txt" >&2
            exit 1
        fi
    fi
fi

# --- Dependencies -------------------------------------------------------------

# A configured build directory remembers where its dependencies are.
if [ -z "$DEPS_DIR" ] && [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    DEPS_DIR=$(sed -n 's/^DEP_BUILD_DIR:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt")
fi
if [ "$OS" = Darwin ]; then
    ARCH=$(uname -m)
    DEPS_DIR="${DEPS_DIR:-deps/build/$ARCH}"
    BUILD_DEPS_CMD="./build_release_macos.sh -d -a $ARCH"
else
    DEPS_DIR="${DEPS_DIR:-deps/build}"
    BUILD_DEPS_CMD="./build_linux.sh -d"
fi
case "$DEPS_DIR" in
    /*) ;;
    *) DEPS_DIR="$ROOT/$DEPS_DIR" ;;
esac

if [ ! -d "$DEPS_DIR/OrcaSlicer_dep/usr/local" ]; then
    echo "OrcaSlicer's dependencies are not built in $DEPS_DIR."
    if ask "Build them now with $BUILD_DEPS_CMD? This takes a while."; then
        $BUILD_DEPS_CMD
    else
        echo "Build them with $BUILD_DEPS_CMD, or point to an existing build with --deps-dir." >&2
        exit 1
    fi
fi

# --- Compile database ---------------------------------------------------------

# The same configure as CI. DEP_BUILD_DIR is named outright: CMake would otherwise
# derive it from the build directory's name.
cmake_args=(-S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSLIC3R_PCH=OFF -DORCA_TOOLS=ON -DBUILD_TESTS=ON
    -DDEP_BUILD_DIR="$DEPS_DIR")
if [ "$OS" = Darwin ]; then
    cmake_args+=(-DCMAKE_OSX_ARCHITECTURES="$ARCH" "-DCMAKE_IGNORE_PREFIX_PATH=/opt/local;/usr/local;/opt/homebrew")
else
    cmake_args+=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
fi
echo "Configuring $BUILD_DIR"
mkdir -p "$BUILD_DIR"
if ! cmake "${cmake_args[@]}" >"$BUILD_DIR/configure.log" 2>&1; then
    tail -n 20 "$BUILD_DIR/configure.log" >&2
    echo "Configuring failed; the full log is in $BUILD_DIR/configure.log." >&2
    [ "$OS" = Linux ] && echo "Missing system libraries? Install them with: ./build_linux.sh -u" >&2
    exit 1
fi
cmake --build "$BUILD_DIR" --target git_commit_hash_header >/dev/null

# --- Base revision ------------------------------------------------------------

if [ -z "$BASE" ]; then
    REMOTE=$(git remote -v | awk '/github\.com[:\/]OrcaSlicer\/OrcaSlicer(\.git)? \(fetch\)/ { print $1; exit }')
    if [ -z "$REMOTE" ]; then
        # Against a fork's main that already has the commits, nothing is checked.
        echo "Warning: no remote points at github.com/OrcaSlicer/OrcaSlicer, so this compares against origin/main." >&2
        echo "If origin is your fork, add the upstream remote (git remote add upstream https://github.com/OrcaSlicer/OrcaSlicer.git) or pass --base." >&2
        REMOTE=origin
    fi
    if [ "$FETCH" = 1 ]; then
        git fetch --quiet "$REMOTE" main
    fi
    BASE="$REMOTE/main"
fi
echo "Comparing against $BASE"

# --- Run ----------------------------------------------------------------------

args=(-p "$BUILD_DIR" --base "$BASE" --clang-tidy "$CLANG_TIDY")
[ -n "$JOBS" ] && args+=(-j "$JOBS")
[ "$FIX" = 1 ] && args+=(-- --fix)
exec python3 scripts/clang_tidy_diff.py "${args[@]}"
