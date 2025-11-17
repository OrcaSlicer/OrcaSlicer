#!/bin/bash

# OrcaSlicer Flatpak Build Script
# This script builds and packages OrcaSlicer as a Flatpak package locally
# Based on the GitHub Actions workflow in .github/workflows/build_all.yml

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
ARCH=$(uname -m)
BUILD_DIR="build_flatpak"
CLEANUP=false
INSTALL_RUNTIME=false
JOBS=$(nproc)
FORCE_CLEAN=false
ENABLE_CCACHE=false
CACHE_DIR=".flatpak-builder"

# Help function
show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Build OrcaSlicer as a Flatpak package"
    echo ""
    echo "Options:"
    echo "  -a, --arch ARCH        Target architecture (x86_64, aarch64) [default: $ARCH]"
    echo "  -d, --build-dir DIR    Build directory [default: $BUILD_DIR]"
    echo "  -j, --jobs JOBS        Number of parallel build jobs for flatpak-builder and modules [default: $JOBS]"
    echo "  -c, --cleanup          Clean build directory before building"
    echo "  -f, --force-clean      Force clean build (disables caching)"
    echo "  --ccache               Enable ccache for faster rebuilds (requires ccache in SDK)"
    echo "  --cache-dir DIR        Flatpak builder cache directory [default: $CACHE_DIR]"
    echo "  -i, --install-runtime  Install required Flatpak runtime and SDK"
    echo "  -h, --help             Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                     # Build for current architecture with caching enabled"
    echo "  $0 -f                  # Force clean build (no caching)"
    echo "  $0 --ccache -j 8       # Use ccache and 8 parallel jobs for faster builds"
    echo "  $0 -a x86_64 -c       # Build for x86_64 and cleanup first"
    echo "  $0 -i -j 16 --ccache  # Install runtime, build with 16 jobs and ccache"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -a|--arch)
            ARCH="$2"
            shift 2
            ;;
        -d|--build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -c|--cleanup)
            CLEANUP=true
            shift
            ;;
        -f|--force-clean)
            FORCE_CLEAN=true
            shift
            ;;
        --ccache)
            ENABLE_CCACHE=true
            shift
            ;;
        --cache-dir)
            CACHE_DIR="$2"
            shift 2
            ;;
        -i|--install-runtime)
            INSTALL_RUNTIME=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}"
            show_help
            exit 1
            ;;
    esac
done

# Validate architecture
if [[ "$ARCH" != "x86_64" && "$ARCH" != "aarch64" ]]; then
    echo -e "${RED}Error: Unsupported architecture '$ARCH'. Supported: x86_64, aarch64${NC}"
    exit 1
fi

# Validate jobs parameter
if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo -e "${RED}Error: Jobs must be a positive integer, got '$JOBS'${NC}"
    exit 1
fi

echo -e "${BLUE}OrcaSlicer Flatpak Build Script${NC}"
echo -e "${BLUE}================================${NC}"
echo -e "Architecture: ${GREEN}$ARCH${NC}"
echo -e "Build directory: ${GREEN}$BUILD_DIR${NC}"
echo -e "Cache directory: ${GREEN}$CACHE_DIR${NC}"
echo -e "Parallel jobs: ${GREEN}$JOBS${NC}"
if [[ "$FORCE_CLEAN" == true ]]; then
    echo -e "Cache mode: ${RED}DISABLED (force clean)${NC}"
else
    echo -e "Cache mode: ${GREEN}ENABLED${NC}"
fi
if [[ "$ENABLE_CCACHE" == true ]]; then
    echo -e "Ccache: ${GREEN}ENABLED${NC}"
else
    echo -e "Ccache: ${YELLOW}DISABLED${NC}"
fi
echo ""

# Check available disk space (flatpak builds need several GB)
AVAILABLE_SPACE=$(df . | awk 'NR==2 {print $4}')
REQUIRED_SPACE=$((5 * 1024 * 1024))  # 5GB in KB

if [[ $AVAILABLE_SPACE -lt $REQUIRED_SPACE ]]; then
    echo -e "${YELLOW}Warning: Low disk space detected.${NC}"
    echo -e "Available: $(( AVAILABLE_SPACE / 1024 / 1024 ))GB, Recommended: 5GB+"
    echo -e "Continue anyway? (y/N)"
    read -r response
    if [[ ! "$response" =~ ^[Yy]$ ]]; then
        echo "Build cancelled by user"
        exit 1
    fi
fi

# Check if flatpak is installed
if ! command -v flatpak &> /dev/null; then
    echo -e "${RED}Error: Flatpak is not installed. Please install it first.${NC}"
    echo "On Ubuntu/Debian: sudo apt install flatpak"
    echo "On Fedora: sudo dnf install flatpak"
    echo "On Arch: sudo pacman -S flatpak"
    exit 1
fi

# Check if flatpak-builder is installed
if ! command -v flatpak-builder &> /dev/null; then
    echo -e "${RED}Error: flatpak-builder is not installed. Please install it first.${NC}"
    echo "On Ubuntu/Debian: sudo apt install flatpak-builder"
    echo "On Fedora: sudo dnf install flatpak-builder"
    echo "On Arch: sudo pacman -S flatpak-builder"
    exit 1
fi

# Check additional build dependencies
echo -e "${YELLOW}Checking build dependencies...${NC}"
MISSING_DEPS=()

if ! command -v cmake &> /dev/null; then
    MISSING_DEPS+=("cmake")
fi

if ! command -v ninja &> /dev/null && ! command -v make &> /dev/null; then
    MISSING_DEPS+=("ninja or make")
fi

if ! command -v pkg-config &> /dev/null; then
    MISSING_DEPS+=("pkg-config")
fi

if [ ${#MISSING_DEPS[@]} -ne 0 ]; then
    echo -e "${RED}Error: Missing required build dependencies: ${MISSING_DEPS[*]}${NC}"
    echo "On Ubuntu/Debian: sudo apt install cmake ninja-build pkg-config"
    echo "On Fedora: sudo dnf install cmake ninja-build pkgconfig"
    exit 1
fi

echo -e "${GREEN}All required dependencies found${NC}"

# Install runtime and SDK if requested
if [[ "$INSTALL_RUNTIME" == true ]]; then
    echo -e "${YELLOW}Installing GNOME runtime and SDK...${NC}"
    flatpak install --user -y flathub org.gnome.Platform//48
    flatpak install --user -y flathub org.gnome.Sdk//48
fi

# Check if required runtime is available
if ! flatpak info --user org.gnome.Platform//48 &> /dev/null; then
    echo -e "${RED}Error: GNOME Platform 48 runtime is not installed.${NC}"
    echo "Run with -i flag to install it automatically, or install manually:"
    echo "flatpak install --user flathub org.gnome.Platform//48"
    exit 1
fi

if ! flatpak info --user org.gnome.Sdk//48 &> /dev/null; then
    echo -e "${RED}Error: GNOME SDK 48 is not installed.${NC}"
    echo "Run with -i flag to install it automatically, or install manually:"
    echo "flatpak install --user flathub org.gnome.Sdk//48"
    exit 1
fi

# Get version information
echo -e "${YELLOW}Getting version information...${NC}"
if [[ -f "version.inc" ]]; then
    VER_PURE=$(grep 'set(SoftFever_VERSION' version.inc | cut -d '"' -f2)
    VER="V$VER_PURE"
    DATE=$(date +'%Y%m%d')
    echo -e "Version: ${GREEN}$VER${NC}"
    echo -e "Date: ${GREEN}$DATE${NC}"
else
    echo -e "${RED}Error: version.inc not found${NC}"
    exit 1
fi

# Cleanup build directory if requested
if [[ "$CLEANUP" == true ]]; then
    echo -e "${YELLOW}Cleaning up flatpak-specific build directories...${NC}"
    rm -rf deps/build_flatpak build_flatpak

    echo -e "${YELLOW}Cleaning up flatpak build directories...${NC}"
    rm -rf "$BUILD_DIR"
    
    # Only clean cache if force-clean is enabled
    if [[ "$FORCE_CLEAN" == true ]]; then
        echo -e "${YELLOW}Cleaning up flatpak build cache...${NC}"
        rm -rf "$CACHE_DIR"
    else
        echo -e "${BLUE}Preserving build cache at: $CACHE_DIR${NC}"
    fi
    
    echo -e "${BLUE}Note: Host build directories (deps/build, build) are preserved${NC}"
fi

# Create build directory
mkdir -p "$BUILD_DIR"
rm -rf "$BUILD_DIR/build-dir"

# Check if flatpak manifest exists
if [[ ! -f "./scripts/flatpak/io.github.softfever.OrcaSlicer.yml" ]]; then
    echo -e "${RED}Error: Flatpak manifest not found at scripts/flatpak/io.github.softfever.OrcaSlicer.yml${NC}"
    exit 1
fi

# Build the Flatpak
echo -e "${YELLOW}Building Flatpak package...${NC}"
echo -e "This may take a while (30+ minutes depending on your system)..."
echo ""

BUNDLE_NAME="OrcaSlicer-Linux-flatpak_${VER}_${ARCH}.flatpak"

# Remove any existing bundle
rm -f "$BUNDLE_NAME"

# Create necessary directories inside repo
mkdir -p "$BUILD_DIR/cache" "$BUILD_DIR/flatpak-builder"

# Set environment variables to match GitHub Actions
export FLATPAK_BUILDER_N_JOBS=$JOBS

echo -e "${BLUE}Running flatpak-builder...${NC}"
echo -e "Using $JOBS parallel jobs for flatpak-builder and $FLATPAK_BUILDER_N_JOBS for module builds"

# Check flatpak-builder version to determine available options
FLATPAK_BUILDER_VERSION=$(flatpak-builder --version 2>/dev/null | head -1 | awk '{print $2}' || echo "unknown")
echo -e "flatpak-builder version: $FLATPAK_BUILDER_VERSION"

# Build command with caching support
BUILDER_ARGS=(
    --arch="$ARCH"
    --user
    --install-deps-from=flathub
    --repo="$BUILD_DIR/repo"
    --verbose
    --state-dir="$CACHE_DIR"
    --jobs="$JOBS"
)

# Add force-clean only if explicitly requested (disables caching)
if [[ "$FORCE_CLEAN" == true ]]; then
    BUILDER_ARGS+=(--force-clean)
    echo -e "${YELLOW}Using --force-clean (caching disabled)${NC}"
else
    echo -e "${GREEN}Using build cache for faster rebuilds${NC}"
fi

# Add ccache if enabled
if [[ "$ENABLE_CCACHE" == true ]]; then
    BUILDER_ARGS+=(--ccache)
    echo -e "${GREEN}Using ccache for compiler caching${NC}"
fi

if ! flatpak-builder \
    "${BUILDER_ARGS[@]}" \
    "$BUILD_DIR/build-dir" \
    scripts/flatpak/io.github.softfever.OrcaSlicer.yml; then
    echo -e "${RED}Error: flatpak-builder failed${NC}"
    echo -e "${YELLOW}Check the build log above for details${NC}"
    exit 1
fi

# Create bundle
echo -e "${YELLOW}Creating Flatpak bundle...${NC}"
if ! flatpak build-bundle \
    "$BUILD_DIR/repo" \
    "$BUNDLE_NAME" \
    io.github.softfever.OrcaSlicer \
    --arch="$ARCH"; then
    echo -e "${RED}Error: Failed to create Flatpak bundle${NC}"
    exit 1
fi

# Success message
echo ""
echo -e "${GREEN}✓ Flatpak build completed successfully!${NC}"
echo -e "Bundle created: ${GREEN}$BUNDLE_NAME${NC}"
echo -e "Size: ${GREEN}$(du -h "$BUNDLE_NAME" | cut -f1)${NC}"
if [[ "$FORCE_CLEAN" != true ]]; then
    echo -e "Build cache: ${GREEN}$CACHE_DIR${NC} (preserved for faster future builds)"
fi
echo ""
echo -e "${BLUE}To install the Flatpak:${NC}"
echo -e "flatpak install --user $BUNDLE_NAME"
echo ""
echo -e "${BLUE}To run OrcaSlicer:${NC}"
echo -e "flatpak run io.github.softfever.OrcaSlicer"
echo ""
echo -e "${BLUE}To uninstall:${NC}"
echo -e "flatpak uninstall --user io.github.softfever.OrcaSlicer"
echo ""
if [[ "$FORCE_CLEAN" != true ]]; then
    echo -e "${BLUE}Cache Management:${NC}"
    echo -e "• Subsequent builds will be faster thanks to caching"
    echo -e "• To force a clean build: $0 -f"
    echo -e "• To clean cache manually: rm -rf $CACHE_DIR"
fi
