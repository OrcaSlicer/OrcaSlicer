#!/usr/bin/env bash
#
# Build OrcaSlicer Flatpak locally using Docker with the same container image
# as the CI (build_all.yml).
#
# Usage:
#   ./scripts/build_flatpak_with_docker.sh [--arch <x86_64|aarch64>] [--no-debug-info] [--pull]
#
# Requirements:
#   - Docker (or Podman with docker compatibility)
#
# The resulting .flatpak bundle is placed in the project root.

set -euo pipefail
SECONDS=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------- defaults ----------
ARCH="$(uname -m)"
NO_DEBUG_INFO=false
FORCE_PULL=false
FORCE_CLEAN=true
CONTAINER_IMAGE="ghcr.io/flathub-infra/flatpak-github-actions:gnome-49"

normalize_arch() {
    case "$1" in
        arm64|aarch64)
            echo "aarch64"
            ;;
        x86_64|amd64)
            echo "x86_64"
            ;;
        *)
            echo "$1"
            ;;
    esac
}

# ---------- parse args ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)
            ARCH="$2"; shift 2 ;;
        --no-debug-info)
            NO_DEBUG_INFO=true; shift ;;
        --pull)
            FORCE_PULL=true; shift ;;
        --no-pull)
            FORCE_PULL=false; shift ;;  # kept for backward compat (now default)
        --keep-build)
            FORCE_CLEAN=false; shift ;;
        --image)
            CONTAINER_IMAGE="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--arch <x86_64|aarch64>] [--no-debug-info] [--pull] [--keep-build] [--image <image>]"
            echo "  --pull         Force pull the container image (default: use cached, auto-pull if missing)"
            echo "  --no-pull      Do not force pull (default, kept for backward compat)"
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

ARCH="$(normalize_arch "$ARCH")"

case "$ARCH" in
    x86_64|aarch64)
        ;;
    *)
        echo "Unsupported architecture: $ARCH. Supported: x86_64, aarch64" >&2
        exit 1
        ;;
esac

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
echo "  Pull mode:  $([ "$FORCE_PULL" = true ] && echo "force" || echo "auto (cached if available)")"
echo "  ccache:     enabled"
echo ""

# ---------- prepare manifest ----------
MANIFEST_SRC="scripts/flatpak/com.orcaslicer.OrcaSlicer.yml"
MANIFEST_DOCKER="scripts/flatpak/com.orcaslicer.OrcaSlicer.docker.yml"
# Ensure cleanup on exit (success or failure)
trap 'rm -f "$PROJECT_ROOT/$MANIFEST_DOCKER"' EXIT

# Build Docker-specific manifest with customizations (piped to avoid sed -i portability)
{
    if [ "$NO_DEBUG_INFO" = true ]; then
        sed '/^build-options:/a\
  no-debuginfo: true\
  strip: true
'
    else
        cat
    fi
} < "$MANIFEST_SRC" | \
sed "/name: OrcaSlicer/{
    n
    s|^\([[:space:]]*\)buildsystem: simple|\1buildsystem: simple\\
\1build-options:\\
\1  env:\\
\1    git_commit_hash: \"$GIT_COMMIT_HASH\"|
}" > "$MANIFEST_DOCKER"

# ---------- run build in Docker ----------
DOCKER="${DOCKER:-docker}"

if [ "$FORCE_PULL" = true ]; then
    echo "=== Pulling container image (--pull requested) ==="
    "$DOCKER" pull "$CONTAINER_IMAGE"
elif ! "$DOCKER" image inspect "$CONTAINER_IMAGE" &>/dev/null; then
    echo "=== Pulling container image (not found locally) ==="
    "$DOCKER" pull "$CONTAINER_IMAGE"
else
    echo "=== Using cached container image (use --pull to update) ==="
fi

FORCE_CLEAN_FLAG=""
if [ "$FORCE_CLEAN" = true ]; then
    FORCE_CLEAN_FLAG="--force-clean"
fi

DOCKER_RUN_ARGS=(run --rm -i --privileged)

# Pass build parameters as env vars so the inner script doesn't need
# variable expansion from the outer shell (avoids quoting issues).
echo "=== Starting Flatpak build inside container ==="
"$DOCKER" "${DOCKER_RUN_ARGS[@]}" \
    -v "$PROJECT_ROOT":/src:Z \
    -w /src \
    -e "BUILD_ARCH=$ARCH" \
    -e "BUNDLE_NAME=$BUNDLE_NAME" \
    -e "FORCE_CLEAN_FLAG=$FORCE_CLEAN_FLAG" \
    "$CONTAINER_IMAGE" \
    bash -s <<'EOF'
set -euo pipefail

format_duration() {
    local total_seconds="$1"
    local hours=$((total_seconds / 3600))
    local minutes=$(((total_seconds % 3600) / 60))
    local seconds=$((total_seconds % 60))

    printf "%02d:%02d:%02d" "$hours" "$minutes" "$seconds"
}

overall_start=$(date +%s)
install_start=$overall_start

# The workspace and .flatpak-builder cache are bind-mounted from the host.
# Git inside the container may reject cached source repos as unsafe due to
# ownership mismatch, which breaks flatpak-builder when it reuses git sources.
git config --global --add safe.directory /src
git config --global --add safe.directory '/src/.flatpak-builder/git/*'

# Install required SDK extensions (not pre-installed in the container image)
flatpak install -y --noninteractive --arch="$BUILD_ARCH" flathub \
    org.gnome.Platform//49 \
    org.gnome.Sdk//49 \
    org.freedesktop.Sdk.Extension.llvm21//25.08 || true

install_end=$(date +%s)
install_duration=$((install_end - install_start))

builder_start=$(date +%s)
flatpak-builder $FORCE_CLEAN_FLAG \
    --verbose \
    --ccache \
    --disable-rofiles-fuse \
    --state-dir=.flatpak-builder \
    --arch="$BUILD_ARCH" \
    --repo=flatpak-repo \
    flatpak-build \
    scripts/flatpak/com.orcaslicer.OrcaSlicer.docker.yml
builder_end=$(date +%s)
builder_duration=$((builder_end - builder_start))

bundle_start=$(date +%s)
flatpak build-bundle \
    --arch="$BUILD_ARCH" \
    flatpak-repo \
    "$BUNDLE_NAME" \
    com.orcaslicer.OrcaSlicer
bundle_end=$(date +%s)
bundle_duration=$((bundle_end - bundle_start))

# Fix ownership so output files are not root-owned on the host
owner="$(stat -c %u:%g /src)"
chown -R "$owner" .flatpak-builder flatpak-build flatpak-repo "$BUNDLE_NAME" 2>/dev/null || true

overall_end=$(date +%s)
overall_duration=$((overall_end - overall_start))

echo ""
echo "=== Build complete ==="
echo "=== Build Stats ==="
echo "  Runtime install: $(format_duration "$install_duration")"
echo "  flatpak-builder: $(format_duration "$builder_duration")"
echo "  Bundle export:   $(format_duration "$bundle_duration")"
echo "  Overall:         $(format_duration "$overall_duration")"
EOF

echo ""
echo "=== Flatpak bundle ready ==="
echo "  ${PROJECT_ROOT}/${BUNDLE_NAME}"
echo ""
echo "Install with:"
echo "  flatpak install --user ${BUNDLE_NAME}"

elapsed=$SECONDS
printf "\nBuild completed in %dh %dm %ds\n" $((elapsed/3600)) $((elapsed%3600/60)) $((elapsed%60))