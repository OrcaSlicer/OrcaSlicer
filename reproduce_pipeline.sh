#!/bin/bash
#
# reproduce_pipeline.sh - Reproduce CI pipeline locally as closely as possible
#
# This script builds OrcaSlicer from source in an Ubuntu environment, mimicking
# the CI pipeline steps. It installs system dependencies, builds required
# third-party libraries (Eigen3 5.0.1, LibDataChannel, Boost, Python 3.12.13,
# OpenVDB chain), configures with CMake, builds with Ninja, and runs unit tests.
#
# Usage: ./reproduce_pipeline.sh [clean]
#   clean  - Remove previous build directories before starting
#
set -euo pipefail

REPO_DIR="${REPO_DIR:-$(cd "$(dirname "$0")" && pwd)}"
BUILD_DIR="${BUILD_DIR:-$REPO_DIR/build}"
NUM_JOBS="${NUM_JOBS:-$(nproc)}"

echo "=============================================="
echo "OrcaSlicer CI Pipeline Reproduction"
echo "=============================================="
echo "Repo dir:  $REPO_DIR"
echo "Build dir: $BUILD_DIR"
echo "Jobs:      $NUM_JOBS"
echo ""

# ---- Parse arguments ----
if [[ "${1:-}" == "clean" ]]; then
    echo ">> Cleaning previous build directories..."
    rm -rf "$BUILD_DIR"
    rm -rf /tmp/eigen-install /tmp/Python-3.12.13 /tmp/libdatachannel
    rm -rf /tmp/boost_1_83_0 /tmp/OpenEXR-* /tmp/openvdb-*
    echo "   Done."
    echo ""
fi

# ---- Step 1: Install system dependencies via apt ----
echo ">> Step 1: Installing system dependencies via apt..."
apt-get update -qq
apt-get install -y -qq \
    cmake ninja-build g++ git ccache wget unzip bzip2 xz-utils \
    libssl-dev libdbus-1-dev \
    qtbase5-dev qtdeclarative5-dev \
    libboost-dev libboost-filesystem-dev libboost-thread-dev \
    libboost-log-dev libboost-locale-dev libboost-regex-dev \
    libboost-chrono-dev libboost-atomic-dev libboost-date-time-dev \
    libboost-iostreams-dev libboost-program-options-dev libboost-nowide-dev \
    libboost-system-dev \
    libtbb-dev \
    zlib1g-dev libbz2-dev liblzma-dev \
    libpng-dev libjpeg-dev libtiff-dev \
    libxml2-dev libsqlite3-dev libcurl4-openssl-dev \
    libexpat1-dev \
    libgl1-mesa-dev libglu1-mesa-dev \
    libglfw3-dev \
    libcereal-dev \
    libnlopt-dev \
    libassimp-dev \
    libnoise-dev \
    libocct-foundation-dev \
    libocct-modeling-algorithms-dev \
    libjpeg-dev \
    libdraco-dev \
    libwxgtk3.2-dev \
    libwxgtk-media3.2-dev \
    libwxgtk-webview3.2-dev \
    libgtk-3-dev \
    libsecret-1-dev \
    libwebkit2gtk-4.1-dev \
    libopencv-dev \
    python3-dev \
    2>&1 | tail -5

# nlopt.hpp symlink (libnlopt-dev installs nlopt.h, but libnest2d includes nlopt.hpp)
ln -sf /usr/include/nlopt.h /usr/include/nlopt.hpp 2>/dev/null || true

echo "   System dependencies installed."
echo ""

# ---- Step 2: Build Eigen3 5.0.1 from source ----
echo ">> Step 2: Building Eigen3 5.0.1 from source..."
if [[ ! -d /tmp/eigen-install/usr/local/include/eigen3/Eigen/Dense ]]; then
    cd /tmp
    rm -rf eigen-5.0.1 eigen-install eigen-5.0.1.zip
    wget -q https://gitlab.com/libeigen/eigen/-/archive/5.0.1/eigen-5.0.1.zip -O eigen-5.0.1.zip
    unzip -q eigen-5.0.1.zip
    cd eigen-5.0.1
    mkdir -p /tmp/eigen-install/usr/local/include/eigen3
    cp -r Eigen /tmp/eigen-install/usr/local/include/eigen3/
    cp -r unsupported /tmp/eigen-install/usr/local/include/eigen3/ 2>/dev/null || true
    mkdir -p /tmp/eigen-install/usr/local/lib/cmake/Eigen3
    cat > /tmp/eigen-install/usr/local/lib/cmake/Eigen3/Eigen3Config.cmake << 'INNER'
set(EIGEN3_INCLUDE_DIR /tmp/eigen-install/usr/local/include/eigen3)
set(EIGEN3_VERSION 5.0.1)
add_library(Eigen3::Eigen INTERFACE IMPORTED)
set_target_properties(Eigen3::Eigen PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${EIGEN3_INCLUDE_DIR}")
INNER
    cat > /tmp/eigen-install/usr/local/lib/cmake/Eigen3/Eigen3ConfigVersion.cmake << 'INNER'
set(PACKAGE_VERSION 5.0.1)
set(PACKAGE_VERSION_COMPATIBLE TRUE)
INNER
    echo "   Eigen3 5.0.1 installed to /tmp/eigen-install"
else
    echo "   Eigen3 5.0.1 already installed, skipping."
fi
echo ""

# ---- Step 3: Build Boost 1.83.0 from source (with ALL required compiled libs) ----
# CMakeLists.txt line 769 requires: system filesystem thread log log_setup locale regex chrono atomic date_time iostreams program_options nowide
echo ">> Step 3: Building Boost 1.83.0 from source..."
BOOST_NEEDS_REBUILD=0
if [[ ! -d /usr/local/include/boost/version.hpp ]]; then
    BOOST_NEEDS_REBUILD=1
else
    # Check if all required components are installed
    for comp in log_setup locale chrono atomic nowide; do
        if [[ ! -f /usr/local/lib/cmake/Boost-1.83.0/boost_${comp}-config.cmake ]] && \
           [[ ! -f /usr/local/lib/cmake/Boost-1.83.0/BoostConfig.cmake ]]; then
            BOOST_NEEDS_REBUILD=1
            break
        fi
    done
fi
if [[ $BOOST_NEEDS_REBUILD -eq 1 ]]; then
    cd /tmp
    rm -rf boost_1_83_0 boost_1_83_0.tar.bz2
    wget -q https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2
    tar xjf boost_1_83_0.tar.bz2
    cd boost_1_83_0
    ./bootstrap.sh --prefix=/usr/local \
        --with-libraries=system,filesystem,thread,log,locale,regex,chrono,atomic,date_time,iostreams,program_options,nowide 2>&1 | tail -3
    ./b2 install -j"$NUM_JOBS" --prefix=/usr/local 2>&1 | tail -5
    echo "   Boost 1.83.0 installed to /usr/local (all components)"
else
    echo "   Boost 1.83.0 already installed with all components, skipping."
fi
echo ""

# ---- Step 4: Build LibDataChannel from source ----
echo ">> Step 4: Building LibDataChannel from source..."
if [[ ! -d /usr/local/lib/cmake/LibDataChannel ]]; then
    cd /tmp
    rm -rf libdatachannel
    git clone --depth 1 --branch v0.24.5 https://github.com/paullouisageneau/libdatachannel.git
    cd libdatachannel
    git submodule update --init --recursive
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DNO_EXAMPLES=ON \
        -DNO_TESTS=ON \
        -DNO_WEBSOCKET=ON \
        -DNO_MEDIA=ON \
        -DUSE_NICE=OFF \
        -DUSE_SYSTEM_JUICE=OFF \
        -DUSE_SYSTEM_USRSCTP=OFF \
        -DOPENSSL_ROOT_DIR=/usr \
        -DOPENSSL_USE_STATIC_LIBS=OFF \
        2>&1 | tail -5
    ninja -C build -j"$NUM_JOBS" install 2>&1 | tail -5
    echo "   LibDataChannel installed to /usr/local"
else
    echo "   LibDataChannel already installed, skipping."
fi
echo ""

# ---- Step 5: Build Python 3.12.13 from source (exact version required) ----
echo ">> Step 5: Checking Python version..."
PYTHON_VERSION=$(python3 --version 2>/dev/null || echo "none")
if [[ "$PYTHON_VERSION" != "Python 3.12.13" ]]; then
    echo "   Current Python: $PYTHON_VERSION (need 3.12.13)"
    echo ">> Building Python 3.12.13 from source..."
    cd /tmp
    rm -rf Python-3.12.13 Python-3.12.13.tar.xz
    wget -q https://www.python.org/ftp/python/3.12.13/Python-3.12.13.tar.xz
    tar xf Python-3.12.13.tar.xz
    cd Python-3.12.13
    ./configure --prefix=/usr/local --enable-optimizations 2>&1 | tail -3
    make -j"$NUM_JOBS" 2>&1 | tail -5
    make install 2>&1 | tail -3
    echo "   Python 3.12.13 installed to /usr/local"
else
    echo "   Python 3.12.13 already installed, skipping."
fi
echo ""

# ---- Step 6: Install OpenVDB/Blosc from apt + create missing CMake configs ----
echo ">> Step 6: Installing OpenVDB/Blosc from apt and creating CMake configs..."
apt-get install -y -qq libopenvdb-dev libblosc-dev 2>&1 | tail -3

# apt packages don't install CMake config files, create them manually
mkdir -p /usr/local/lib/cmake/OpenVDB /usr/local/lib/cmake/IlmBase /usr/local/lib/cmake/Blosc

cat > /usr/local/lib/cmake/OpenVDB/OpenVDBConfig.cmake << 'INNER'
set(OpenVDB_FOUND TRUE)
set(OpenVDB_VERSION 10.0.1)
set(OpenVDB_openvdb_FOUND TRUE)
add_library(OpenVDB::openvdb INTERFACE IMPORTED)
set_target_properties(OpenVDB::openvdb PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
    INTERFACE_LINK_LIBRARIES "/usr/lib/x86_64-linux-gnu/libopenvdb.so"
)
INNER
cat > /usr/local/lib/cmake/OpenVDB/OpenVDBConfigVersion.cmake << 'INNER'
set(PACKAGE_VERSION 10.0.1)
set(PACKAGE_VERSION_COMPATIBLE TRUE)
INNER

cat > /usr/local/lib/cmake/IlmBase/IlmBaseConfig.cmake << 'INNER'
set(IlmBase_FOUND TRUE)
set(IlmBase_VERSION 3.2.4)
add_library(IlmBase::Half INTERFACE IMPORTED)
set_target_properties(IlmBase::Half PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "/usr/local/include/OpenEXR;/usr/local/include/Imath"
    INTERFACE_LINK_LIBRARIES "/usr/local/lib/libOpenEXRCore.so"
)
INNER

cat > /usr/local/lib/cmake/Blosc/BloscConfig.cmake << 'INNER'
set(Blosc_FOUND TRUE)
set(Blosc_VERSION 1.21.5)
add_library(Blosc::blosc INTERFACE IMPORTED)
set_target_properties(Blosc::blosc PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "/usr/include/blosc"
    INTERFACE_LINK_LIBRARIES "/usr/lib/x86_64-linux-gnu/libblosc.so"
)
INNER

echo "   OpenVDB/Blosc CMake configs created."
echo ""

# ---- Step 6.7: Build CGAL 6.2 from source (Ubuntu 24.04 only has 5.6) ----
echo ">> Step 6.7: Building CGAL 6.2 from source..."
if [[ ! -d /usr/local/include/CGAL ]]; then
    cd /tmp
    rm -rf CGAL-6.2 CGAL-6.2.tar.xz
    wget -q https://github.com/CGAL/cgal/releases/download/v6.2/CGAL-6.2.tar.xz
    tar xf CGAL-6.2.tar.xz
    cd CGAL-6.2
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_BUILD_TYPE=Release \
        -DWITH_CGAL_Qt5=OFF \
        -DWITH_CGAL_Qt6=OFF \
        -DWITH_CGAL_ImageIO=OFF \
        -DWITH_CGAL_Scip=OFF \
        -DWITH_CGAL_Eigen3=ON \
        2>&1 | tail -5
    ninja -C build -j"$NUM_JOBS" install 2>&1 | tail -5
    echo "   CGAL 6.2 installed to /usr/local"
else
    echo "   CGAL 6.2 already installed, skipping."
fi
echo ""

# ---- Step 6.5: Build SLVS (SolveSpace constraint solver) ----
echo ">> Step 6.5: Building SLVS from source..."
if [[ ! -d /usr/local/include/slvs ]]; then
    cd /tmp
    rm -rf SolveSpaceLib-* SolveSpaceLib-*.zip
    wget -q https://github.com/JacobStoren/SolveSpaceLib/archive/4d8704523e4bf212fadf5189f92484244f670fea.zip -O slvs.zip
    unzip -q slvs.zip
    cd SolveSpaceLib-*
    # Copy the CMakeLists.txt.in from deps/SLVS
    cp /repo/deps/SLVS/CMakeLists.txt.in ./CMakeLists.txt
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        2>&1 | tail -5
    ninja -C build -j"$NUM_JOBS" install 2>&1 | tail -5
    echo "   SLVS installed to /usr/local"
else
    echo "   SLVS already installed, skipping."
fi
echo ""

# ---- Step 7: Configure OrcaSlicer with CMake ----
echo ">> Step 7: Configuring OrcaSlicer with CMake..."
STEP7_START=$(date +%s)
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake -S "$REPO_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=ON \
    -DCMAKE_PREFIX_PATH=/usr/local \
    -DEigen3_DIR=/tmp/eigen-install/usr/local/lib/cmake/Eigen3 \
    -DBoost_DIR=/usr/local/lib/cmake/Boost-1.83.0 \
    -DOpenVDB_DIR=/usr/local/lib/cmake/OpenVDB \
    -DIlmBase_DIR=/usr/local/lib/cmake/IlmBase \
    -DBlosc_DIR=/usr/local/lib/cmake/Blosc \
    -DOpenVDB_INCLUDE_DIR=/usr/include \
    -DOpenVDB_LIBRARY=/usr/lib/x86_64-linux-gnu/libopenvdb.so \
    -DOpenVDB_openvdb_LIBRARY=/usr/lib/x86_64-linux-gnu/libopenvdb.so \
    -DOpenVDB_openvdb_LIBRARY_RELEASE=/usr/lib/x86_64-linux-gnu/libopenvdb.so \
    -DOpenVDB_LIB_COMPONENTS=/usr/lib/x86_64-linux-gnu/libopenvdb.so \
    -DOpenVDB_openvdb_FOUND=TRUE \
    -DOpenVDB_VERSION=10.0.1 \
    -DIlmBase_FOUND=TRUE \
    -DIlmBase_INCLUDE_DIRS="/usr/local/include/OpenEXR;/usr/local/include/Imath" \
    -DIlmBase_LIBRARIES="/usr/local/lib/libOpenEXRCore.so" \
    -DTBB_FOUND=TRUE \
    -DZLIB_FOUND=TRUE \
    -DLIBNOISE_LIBRARY_RELEASE=/usr/lib/libnoise.a \
    2>&1 | tail -30
STEP7_END=$(date +%s)
STEP7_SECONDS=$((STEP7_END - STEP7_START))

echo ""
echo ">> Step 8: Building OrcaSlicer with Ninja..."
STEP8_START=$(date +%s)
ninja -C "$BUILD_DIR" -j"$NUM_JOBS" 2>&1 | tail -20
STEP8_END=$(date +%s)
STEP8_SECONDS=$((STEP8_END - STEP8_START))

echo ""
echo ">> Step 9: Running unit tests..."
STEP9_START=$(date +%s)
ctest --test-dir "$BUILD_DIR" -C Release --output-on-failure -j"$NUM_JOBS" 2>&1 | tail -30
STEP9_END=$(date +%s)
STEP9_SECONDS=$((STEP9_END - STEP9_START))

echo ""
echo "=============================================="
echo "Pipeline reproduction complete!"
echo "=============================================="
echo ""
echo "=== PERFORMANCE STATISTICS ==="
echo "| Step   | Description                   | Time (seconds) |"
echo "|--------|-------------------------------|----------------|"
echo "| Step 1 | Install system dependencies   | N/A (apt)      |"
echo "| Step 2 | Build Eigen3 5.0.1            | N/A (cached)   |"
echo "| Step 3 | Build Boost 1.83.0            | N/A (cached)   |"
echo "| Step 4 | Build LibDataChannel          | N/A (cached)   |"
echo "| Step 5 | Build Python 3.12.13          | N/A (cached)   |"
echo "| Step 6 | Install OpenVDB/Blosc (apt)   | N/A (cached)   |"
echo "| Step 7 | CMake configure               | ${STEP7_SECONDS}s    |"
echo "| Step 8 | Ninja build (${NUM_JOBS} jobs)| ${STEP8_SECONDS}s    |"
echo "| Step 9 | ctest unit tests              | ${STEP9_SECONDS}s    |"
echo "|--------|-------------------------------|----------------|"
echo "| Total  | Build + test (Steps 7-9)      | $((STEP7_SECONDS + STEP8_SECONDS + STEP9_SECONDS))s   |"
echo "=============================================="
echo ""
echo "Build artifacts location: $BUILD_DIR"
echo "Performance log saved to: $BUILD_DIR/../perf_stats.txt"
echo ""
# Save perf stats to file
{
    echo "=== OrcaSlicer Local Pipeline Performance Statistics ==="
    echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Repo: $REPO_DIR"
    echo "Build dir: $BUILD_DIR"
    echo "Jobs: $NUM_JOBS"
    echo ""
    echo "| Step   | Description                   | Time (seconds) |"
    echo "|--------|-------------------------------|----------------|"
    echo "| Step 1 | Install system dependencies   | N/A (apt)      |"
    echo "| Step 2 | Build Eigen3 5.0.1            | N/A (cached)   |"
    echo "| Step 3 | Build Boost 1.83.0            | N/A (cached)   |"
    echo "| Step 4 | Build LibDataChannel          | N/A (cached)   |"
    echo "| Step 5 | Build Python 3.12.13          | N/A (cached)   |"
    echo "| Step 6 | Install OpenVDB/Blosc (apt)   | N/A (cached)   |"
    echo "| Step 7 | CMake configure               | ${STEP7_SECONDS}s    |"
    echo "| Step 8 | Ninja build (${NUM_JOBS} jobs)| ${STEP8_SECONDS}s    |"
    echo "| Step 9 | ctest unit tests              | ${STEP9_SECONDS}s    |"
    echo "|--------|-------------------------------|----------------|"
    echo "| Total  | Build + test (Steps 7-9)      | $((STEP7_SECONDS + STEP8_SECONDS + STEP9_SECONDS))s   |"
    echo ""
    echo "Git commit: $(cd "$REPO_DIR" && git rev-parse HEAD 2>/dev/null || echo 'unknown')"
    echo "Git branch: $(cd "$REPO_DIR" && git branch --show-current 2>/dev/null || echo 'unknown')"
} > "$BUILD_DIR/../perf_stats.txt"

cat "$BUILD_DIR/../perf_stats.txt"

# ---- Append to master results log ----
RESULTS_LOG="$REPO_DIR/docs/build-performance-local-results.log"
{
    echo "=============================================="
    echo "Run: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Commit: $(cd "$REPO_DIR" && git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
    echo "Branch: $(cd "$REPO_DIR" && git branch --show-current 2>/dev/null || echo 'unknown')"
    echo "Jobs: $NUM_JOBS"
    echo "Step 7 (CMake configure): ${STEP7_SECONDS}s"
    echo "Step 8 (Ninja build ${NUM_JOBS} jobs): ${STEP8_SECONDS}s"
    echo "Step 9 (ctest unit tests): ${STEP9_SECONDS}s"
    echo "Total (Steps 7-9): $((STEP7_SECONDS + STEP8_SECONDS + STEP9_SECONDS))s"
    echo "Result: $([ -f "$BUILD_DIR/OrcaSlicer" ] || [ -f "$BUILD_DIR/src/OrcaSlicer" ] || [ -f "$BUILD_DIR/orcaslicer" ] && echo "SUCCESS - Build produced binary" || echo "PARTIAL - Check output above")"
    echo ""
} >> "$RESULTS_LOG"

echo ""
echo "Master results log appended to: $RESULTS_LOG"
