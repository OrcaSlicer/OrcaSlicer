#!/usr/bin/env bash
#
# Build OrcaSlicer Flatpak locally using Docker with the same container image
# as the CI (build_all.yml).
#
# Usage:
#   ./scripts/build_flatpak_with_docker.sh [--arch <x86_64|aarch64>] [--no-debug-info]
#
# Requirements:
#   - Docker (or Podman with docker compatibility)
#
# The resulting .flatpak bundle is placed in the project root.
# A persistent Docker volume "flatpak-builder-cache" is used to cache
# downloaded sources across builds. Remove it with:
#   docker volume rm flatpak-builder-cache

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------- defaults ----------
ARCH="$(uname -m)"
NO_DEBUG_INFO=false
NO_PULL=false
FORCE_CLEAN=true
CONTAINER_IMAGE="ghcr.io/flathub-infra/flatpak-github-actions:gnome-49"

# ---------- parse args ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)
            ARCH="$2"; shift 2 ;;
        --no-debug-info)
            NO_DEBUG_INFO=true; shift ;;
        --no-pull)
            NO_PULL=true; shift ;;
        --keep-build)
            FORCE_CLEAN=false; shift ;;
        --image)
            CONTAINER_IMAGE="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--arch <x86_64|aarch64>] [--no-debug-info] [--no-pull] [--keep-build] [--image <image>]"
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ---------- version & commit ----------
cd "$PROJECT_ROOT"

VER_PURE=$(grep 'set(SoftFever_VERSION' version.inc | cut -d '"' -f2)
if [ -z "$VER_PURE" ]; then
    echo "Error: could not extract version from version.inc" >&2
    exit 1
fi
VER="V${VER_PURE}"
GIT_COMMIT_HASH=$(git rev-parse HEAD)
BUNDLE_NAME="OrcaSlicer-Linux-flatpak_${VER}_${ARCH}.flatpak"

echo "=== OrcaSlicer Flatpak Build ==="
echo "  Version:    ${VER} (${VER_PURE})"
echo "  Commit:     ${GIT_COMMIT_HASH}"
echo "  Arch:       ${ARCH}"
echo "  Image:      ${CONTAINER_IMAGE}"
echo "  Bundle:     ${BUNDLE_NAME}"
echo "  Debug info: $([ "$NO_DEBUG_INFO" = true ] && echo "disabled" || echo "enabled")"
echo "  ccache:     enabled"
echo ""

# ---------- prepare manifest ----------
MANIFEST_SRC="scripts/flatpak/io.github.orcaslicer.OrcaSlicer.yml"
MANIFEST_DOCKER="scripts/flatpak/io.github.orcaslicer.OrcaSlicer.docker.yml"
cp "$MANIFEST_SRC" "$MANIFEST_DOCKER"

# Ensure cleanup on exit (success or failure)
trap 'rm -f "$PROJECT_ROOT/$MANIFEST_DOCKER"' EXIT

# Optionally strip debug info (matches CI behaviour for faster builds)
if [ "$NO_DEBUG_INFO" = true ]; then
    sed -i '/^build-options:/a\  no-debuginfo: true\n  strip: true' "$MANIFEST_DOCKER"
fi

# Inject git commit hash (same sed as CI)
sed -i "/name: OrcaSlicer/{n;s|buildsystem: simple|buildsystem: simple\n    build-options:\n      env:\n        git_commit_hash: \"$GIT_COMMIT_HASH\"|}" "$MANIFEST_DOCKER"

# ---------- run build in Docker ----------
DOCKER="${DOCKER:-docker}"

if [ "$NO_PULL" = false ]; then
    echo "=== Pulling container image ==="
    "$DOCKER" pull "$CONTAINER_IMAGE"
fi

FORCE_CLEAN_FLAG=""
if [ "$FORCE_CLEAN" = true ]; then
    FORCE_CLEAN_FLAG="--force-clean"
fi

# Pass build parameters as env vars so the inner script doesn't need
# variable expansion from the outer shell (avoids quoting issues).
echo "=== Starting Flatpak build inside container ==="
"$DOCKER" run --rm --privileged \
    -v "$PROJECT_ROOT":/src:Z \
    -v flatpak-builder-cache:/src/.flatpak-builder \
    -w /src \
    -e "BUILD_ARCH=$ARCH" \
    -e "BUNDLE_NAME=$BUNDLE_NAME" \
    -e "FORCE_CLEAN_FLAG=$FORCE_CLEAN_FLAG" \
    "$CONTAINER_IMAGE" \
    bash -c '
        set -euo pipefail

        # Install required SDK extensions (not pre-installed in the container image)
        flatpak install -y --noninteractive flathub \
            org.freedesktop.Sdk.Extension.llvm21//25.08 || true

        flatpak-builder $FORCE_CLEAN_FLAG \
            --ccache \
            --disable-rofiles-fuse \
            --arch="$BUILD_ARCH" \
            --repo=flatpak-repo \
            flatpak-build \
            scripts/flatpak/io.github.orcaslicer.OrcaSlicer.docker.yml

        flatpak build-bundle \
            --arch="$BUILD_ARCH" \
            flatpak-repo \
            "$BUNDLE_NAME" \
            io.github.orcaslicer.OrcaSlicer

        # Fix ownership so output files are not root-owned on the host
        chown "$(stat -c %u:%g /src)" "$BUNDLE_NAME" flatpak-build flatpak-repo

        echo "=== Build complete ==="
    '

echo ""
echo "=== Flatpak bundle ready ==="
echo "  ${PROJECT_ROOT}/${BUNDLE_NAME}"
echo ""
echo "Install with:"
echo "  flatpak install --user ${BUNDLE_NAME}"