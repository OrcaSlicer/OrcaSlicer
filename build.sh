#!/usr/bin/env bash
# OrcaSlicer Build Script (macOS)
# Handles the full build lifecycle: deps, configure, build
#
# Usage:
#   ./build.sh              # Incremental build (after initial setup)
#   ./build.sh --full       # Full build from scratch (deps + configure + build)
#   ./build.sh --deps       # Build dependencies only
#   ./build.sh --configure  # Run CMake configure only
#   ./build.sh --clean      # Remove build dir and rebuild
#   ./build.sh --help       # Show all options

set -e
set -o pipefail

# ── Configuration ──────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCH="${ARCH:-$(uname -m)}"
BUILD_CONFIG="${BUILD_CONFIG:-RelWithDebInfo}"
OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET:-11.3}"
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

BUILD_DIR="${SCRIPT_DIR}/build/${ARCH}"
DEPS_DIR="${SCRIPT_DIR}/deps"
DEPS_BUILD_DIR="${DEPS_DIR}/build/${ARCH}"
DEPS_INSTALL_DIR="${DEPS_BUILD_DIR}/OrcaSlicer_dep"

# CMake 4.x compatibility
CMAKE_VERSION=$(cmake --version 2>/dev/null | head -1 | sed 's/[^0-9]*\([0-9]*\).*/\1/')
CMAKE_COMPAT=""
if [ "${CMAKE_VERSION:-3}" -ge 4 ] 2>/dev/null; then
    CMAKE_COMPAT="-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
fi

# ── Colors ─────────────────────────────────────────────────────

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${BLUE}▸${NC} $*"; }
ok()    { echo -e "${GREEN}✓${NC} $*"; }
warn()  { echo -e "${YELLOW}⚠${NC} $*"; }
err()   { echo -e "${RED}✗${NC} $*"; }

# ── Parse Arguments ────────────────────────────────────────────

DO_DEPS=false
DO_CONFIGURE=false
DO_BUILD=true
DO_CLEAN=false
DO_FULL=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --full)       DO_FULL=true; shift ;;
        --deps)       DO_DEPS=true; DO_BUILD=false; shift ;;
        --configure)  DO_CONFIGURE=true; DO_BUILD=false; shift ;;
        --clean)      DO_CLEAN=true; shift ;;
        --arch)       ARCH="$2"; shift 2 ;;
        --config)     BUILD_CONFIG="$2"; shift 2 ;;
        --help|-h)
            echo "OrcaSlicer Build Script (macOS)"
            echo ""
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Build modes:"
            echo "  (no flags)     Incremental build (fastest, use after initial setup)"
            echo "  --full         Full build from scratch: deps → configure → build"
            echo "  --deps         Build dependencies only (takes 30-60 min first time)"
            echo "  --configure    Run CMake configure only"
            echo "  --clean        Remove build directory, then rebuild"
            echo ""
            echo "Options:"
            echo "  --arch ARCH    Architecture: arm64, x86_64 (default: $(uname -m))"
            echo "  --config CFG   Build config: Release, RelWithDebInfo, Debug"
            echo "                 (default: RelWithDebInfo)"
            echo ""
            echo "Examples:"
            echo "  $0 --full            # First-time build (do this first!)"
            echo "  $0                   # Quick rebuild after code changes"
            echo "  $0 --clean           # Clean rebuild if something is broken"
            echo ""
            echo "Prerequisites: Xcode Command Line Tools, CMake 3.13+"
            echo "  xcode-select --install"
            echo "  brew install cmake"
            exit 0
            ;;
        *)
            err "Unknown option: $1 (use --help)"
            exit 1
            ;;
    esac
done

# --full implies all steps
if [ "$DO_FULL" = true ]; then
    DO_DEPS=true
    DO_CONFIGURE=true
    DO_BUILD=true
fi

# ── Preflight Checks ──────────────────────────────────────────

echo ""
echo -e "${BOLD}OrcaSlicer Build${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Architecture:  ${ARCH}"
echo "  Config:        ${BUILD_CONFIG}"
echo "  CPU cores:     ${NCPU}"
echo "  CMake:         $(cmake --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
[ -n "$CMAKE_COMPAT" ] && echo "  CMake compat:  4.x → policy 3.5"
echo ""

# Check prerequisites
if ! command -v cmake &>/dev/null; then
    err "CMake not found. Install it with: brew install cmake"
    exit 1
fi

if ! command -v git &>/dev/null; then
    err "Git not found. Install Xcode Command Line Tools: xcode-select --install"
    exit 1
fi

if ! xcode-select -p &>/dev/null; then
    err "Xcode Command Line Tools not found. Install: xcode-select --install"
    exit 1
fi

# ── Clean ──────────────────────────────────────────────────────

if [ "$DO_CLEAN" = true ]; then
    warn "Removing build directory: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    ok "Clean complete"
    echo ""
fi

# ── Build Dependencies ─────────────────────────────────────────

if [ "$DO_DEPS" = true ]; then
    info "Building dependencies (this takes a while the first time)..."
    mkdir -p "${DEPS_INSTALL_DIR}"
    cd "${DEPS_BUILD_DIR}"

    cmake "${DEPS_DIR}" \
        -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE="${BUILD_CONFIG}" \
        -DCMAKE_OSX_ARCHITECTURES:STRING="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}" \
        ${CMAKE_COMPAT}

    cmake --build . --config "${BUILD_CONFIG}" --target deps -j"${NCPU}"

    ok "Dependencies built"
    echo ""
fi

# ── Configure ──────────────────────────────────────────────────

if [ "$DO_CONFIGURE" = true ] || { [ "$DO_BUILD" = true ] && [ ! -f "${BUILD_DIR}/Makefile" ] && [ ! -f "${BUILD_DIR}/build.ninja" ]; }; then
    info "Configuring CMake..."
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"

    cmake "${SCRIPT_DIR}" \
        -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE="${BUILD_CONFIG}" \
        -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}" \
        ${CMAKE_COMPAT}

    ok "CMake configured"
    echo ""
fi

# ── Build ──────────────────────────────────────────────────────

if [ "$DO_BUILD" = true ]; then
    if [ ! -d "${BUILD_DIR}" ]; then
        err "Build directory not found: ${BUILD_DIR}"
        err "Run '$0 --full' for a first-time build"
        exit 1
    fi

    info "Building OrcaSlicer..."
    cd "${SCRIPT_DIR}"

    cmake --build "${BUILD_DIR}" \
        --config "${BUILD_CONFIG}" \
        --target OrcaSlicer \
        -j"${NCPU}"

    echo ""
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    ok "Build complete!"
    echo ""
    echo "  App: ${BUILD_DIR}/src/${BUILD_CONFIG}/OrcaSlicer.app"
    echo ""
    echo "  Run: open \"${BUILD_DIR}/src/${BUILD_CONFIG}/OrcaSlicer.app\""
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
fi
