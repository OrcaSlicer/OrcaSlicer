#!/usr/bin/env bash
set -e # Exit immediately if a command exits with a non-zero status.

SCRIPT_NAME=$(basename "$0")
SCRIPT_PATH=$(dirname "$(readlink -f "${0}")")

pushd "${SCRIPT_PATH}" > /dev/null

function usage() {
    echo "Usage: ./${SCRIPT_NAME} [-1][-b][-c][-d][-h][-i][-j N][-p][-r][-s][-t][-u][-l][-L]"
    echo "   -1: limit builds to one core (where possible)"
    echo "   -j N: limit builds to N cores (where possible)"
    echo "   -b: build in debug mode"
    echo "   -c: force a clean build"
    echo "   -C: enable ANSI-colored compile output (GNU/Clang only)"
    echo "   -d: download and build dependencies in ./deps/ (build prerequisite)"
    echo "   -h: prints this help text"
    echo "   -i: build the Orca Slicer AppImage (optional)"
    echo "   -p: boost ccache hit rate by disabling precompiled headers (default: ON)"
    echo "   -r: skip RAM and disk checks (low RAM compiling)"
    echo "   -s: build the Orca Slicer (optional)"
    echo "   -t: build tests (optional)"
    echo "   -u: install system dependencies (asks for sudo password; build prerequisite)"
    echo "   -l: use Clang instead of GCC (default: GCC)"
    echo "   -L: use ld.lld as linker (if available)"
    echo "For a first use, you want to './${SCRIPT_NAME} -u'"
    echo "   and then './${SCRIPT_NAME} -dsi'"
    echo "To build with tests: './${SCRIPT_NAME} -st' or './${SCRIPT_NAME} -dst'"
}

SLIC3R_PRECOMPILED_HEADERS="ON"

unset name
while getopts ":1j:bcCdhiprstulL" opt ; do
  case ${opt} in
    1 )
        export CMAKE_BUILD_PARALLEL_LEVEL=1
        ;;
    j )
        export CMAKE_BUILD_PARALLEL_LEVEL=$OPTARG
        ;;
    b )
        BUILD_DEBUG="1"
        ;;
    c )
        CLEAN_BUILD=1
        ;;
    C )
        COLORED_OUTPUT="-DCOLORED_OUTPUT=ON"
        ;;
    d )
        BUILD_DEPS="1"
        ;;
    h ) usage
        exit 1
        ;;
    i )
        BUILD_IMAGE="1"
        ;;
    p )
        SLIC3R_PRECOMPILED_HEADERS="OFF"
        ;;
    r )
        SKIP_RAM_CHECK="1"
        ;;
    s )
        BUILD_ORCA="1"
        ;;
    t )
        BUILD_TESTS="1"
        ;;
    u )
        export UPDATE_LIB="1"
        ;;
    l )
        USE_CLANG="1"
        ;;
    L )
        USE_LLD="1"
        ;;
    * )
	echo "Unknown argument '${opt}', aborting."
	exit 1
	;;
  esac
done

if [ ${OPTIND} -eq 1 ] ; then
    usage
    exit 1
fi

function check_available_memory_and_disk() {
    FREE_MEM_GB=$(free --gibi --total | grep 'Mem' | rev | cut --delimiter=" " --fields=1 | rev)
    MIN_MEM_GB=10

    FREE_DISK_KB=$(df --block-size=1K . | tail -1 | awk '{print $4}')
    MIN_DISK_KB=$((10 * 1024 * 1024))

    if [[ ${FREE_MEM_GB} -le ${MIN_MEM_GB} ]] ; then
        echo -e "\nERROR: Orca Slicer Builder requires at least ${MIN_MEM_GB}G of 'available' mem (system has only ${FREE_MEM_GB}G available)"
        echo && free --human && echo
        echo "Invoke with -r to skip RAM and disk checks."
        exit 2
    fi

    if [[ ${FREE_DISK_KB} -le ${MIN_DISK_KB} ]] ; then
        echo -e "\nERROR: Orca Slicer Builder requires at least $(echo "${MIN_DISK_KB}" |awk '{ printf "%.1fG\n", $1/1024/1024; }') (system has only $(echo "${FREE_DISK_KB}" | awk '{ printf "%.1fG\n", $1/1024/1024; }') disk free)"
        echo && df --human-readable . && echo
        echo "Invoke with -r to skip ram and disk checks."
        exit 1
    fi
}

# cmake 4.x compatibility workaround
export CMAKE_POLICY_VERSION_MINIMUM=3.5

DISTRIBUTION=$(awk -F= '/^ID=/ {print $2}' /etc/os-release | tr -d '"')
DISTRIBUTION_LIKE=$(awk -F= '/^ID_LIKE=/ {print $2}' /etc/os-release | tr -d '"')
# Check for direct distribution match to Ubuntu/Debian
if [ "${DISTRIBUTION}" == "ubuntu" ] || [ "${DISTRIBUTION}" == "linuxmint" ] ; then
    DISTRIBUTION="debian"
# Check if distribution is Debian/Ubuntu-like based on ID_LIKE
elif [[ "${DISTRIBUTION_LIKE}" == *"debian"* ]] || [[ "${DISTRIBUTION_LIKE}" == *"ubuntu"* ]] ; then
    DISTRIBUTION="debian"
elif [[ "${DISTRIBUTION_LIKE}" == *"arch"* ]] ; then
    DISTRIBUTION="arch"
fi

if [ ! -f "./scripts/linux.d/${DISTRIBUTION}" ] ; then
    echo "Your distribution \"${DISTRIBUTION}\" is not supported by system-dependency scripts in ./scripts/linux.d/"
    echo "Please resolve dependencies manually and contribute a script for your distribution to upstream."
    exit 1
else
    echo "resolving system dependencies for distribution \"${DISTRIBUTION}\" ..."
    # shellcheck source=/dev/null
    source "./scripts/linux.d/${DISTRIBUTION}"
fi

echo "FOUND_GTK3=${FOUND_GTK3}"
if [[ -z "${FOUND_GTK3_DEV}" ]] ; then
    echo "Error, you must install the dependencies before."
    echo "Use option -u with sudo"
    exit 1
fi

echo "Changing date in version..."
{
    # change date in version
    sed --in-place "s/+UNKNOWN/_$(date '+%F')/" version.inc
}
echo "done"


if [[ -z "${SKIP_RAM_CHECK}" ]] ; then
    check_available_memory_and_disk
fi

export CMAKE_C_CXX_COMPILER_CLANG=()
if [[ -n "${USE_CLANG}" ]] ; then
    export CMAKE_C_CXX_COMPILER_CLANG=(-DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++)
fi

# Configure use of ld.lld as the linker when requested
export CMAKE_LLD_LINKER_ARGS=()
if [[ -n "${USE_LLD}" ]] ; then
    if command -v ld.lld >/dev/null 2>&1 ; then
        LLD_BIN=$(command -v ld.lld)
        export CMAKE_LLD_LINKER_ARGS=(-DCMAKE_LINKER="${LLD_BIN}" -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld)
    else
        echo "Error: ld.lld not found. Please install the 'lld' package (e.g., sudo apt install lld) or omit -L."
        exit 1
    fi
fi

if [[ -n "${BUILD_DEPS}" ]] ; then
    echo "Configuring dependencies..."
    read -r -a BUILD_ARGS <<< "${DEPS_EXTRA_BUILD_ARGS}"
    BUILD_ARGS+=(-DDEP_WX_GTK3=ON)
    if [[ -n "${CLEAN_BUILD}" ]]
    then
        rm -fr deps/build
    fi
    mkdir -p deps/build
    if [[ -n "${BUILD_DEBUG}" ]] ; then
        # build deps with debug and release else cmake will not find required sources
        mkdir -p deps/build/release
	set -x
	cmake -S deps -B deps/build/release "${CMAKE_C_CXX_COMPILER_CLANG[@]}" "${CMAKE_LLD_LINKER_ARGS[@]}" -G Ninja \
	      -DSLIC3R_PCH="${SLIC3R_PRECOMPILED_HEADERS}" \
	      -DDESTDIR="${SCRIPT_PATH}/deps/build/destdir" \
	      -DDEP_DOWNLOAD_DIR="${SCRIPT_PATH}/deps/DL_CACHE" \
	      "${COLORED_OUTPUT}" \
	      "${BUILD_ARGS[@]}"
	set +x
        cmake --build deps/build/release
        BUILD_ARGS+=(-DCMAKE_BUILD_TYPE=Debug)
    fi

    set -x
    cmake -S deps -B deps/build "${CMAKE_C_CXX_COMPILER_CLANG[@]}" "${CMAKE_LLD_LINKER_ARGS[@]}" -G Ninja \
	  -DSLIC3R_PCH="${SLIC3R_PRECOMPILED_HEADERS}" \
	  -DDESTDIR="${SCRIPT_PATH}/deps/build/destdir" \
	  -DDEP_DOWNLOAD_DIR="${SCRIPT_PATH}/deps/DL_CACHE" \
	  "${COLORED_OUTPUT}" \
	  "${BUILD_ARGS[@]}"
    set +x
    cmake --build deps/build
fi

if [[ -n "${BUILD_ORCA}" ]] ; then
    echo "Configuring OrcaSlicer..."
    if [[ -n "${CLEAN_BUILD}" ]] ; then
        rm -fr build
    fi
    read -r -a BUILD_ARGS <<< "${ORCA_EXTRA_BUILD_ARGS}"
    if [[ -n "${FOUND_GTK3_DEV}" ]] ; then
        BUILD_ARGS+=(-DSLIC3R_GTK=3)
    fi
    if [[ -n "${BUILD_DEBUG}" ]] ; then
        BUILD_ARGS+=(-DCMAKE_BUILD_TYPE=Debug -DBBL_INTERNAL_TESTING=1)
    else
        BUILD_ARGS+=(-DBBL_RELEASE_TO_PUBLIC=1 -DBBL_INTERNAL_TESTING=0)
    fi
    if [[ -n "${BUILD_TESTS}" ]] ; then
        BUILD_ARGS+=(-DBUILD_TESTS=ON)
    fi
    if [[ -n "${ORCA_UPDATER_SIG_KEY}" ]] ; then
        BUILD_ARGS+=(-DORCA_UPDATER_SIG_KEY="${ORCA_UPDATER_SIG_KEY}")
    fi

    echo "Configuring OrcaSlicer..."
    set -x
    cmake -S . -B build "${CMAKE_C_CXX_COMPILER_CLANG[@]}" "${CMAKE_LLD_LINKER_ARGS[@]}" -G "Ninja Multi-Config" \
	  -DSLIC3R_PCH="${SLIC3R_PRECOMPILED_HEADERS}" \
	  -DCMAKE_PREFIX_PATH="${SCRIPT_PATH}/deps/build/destdir/usr/local" \
	  -DSLIC3R_STATIC=1 \
	  -DORCA_TOOLS=ON \
	  "${COLORED_OUTPUT}" \
	  "${BUILD_ARGS[@]}"
    set +x
    echo "done"
    echo "Building OrcaSlicer ..."
    if [[ -n "${BUILD_DEBUG}" ]] ; then
        cmake --build build --config Debug --target OrcaSlicer
    else
        cmake --build build --config Release --target OrcaSlicer
    fi
    echo "Building OrcaSlicer_profile_validator .."
    if [[ -n "${BUILD_DEBUG}" ]] ; then
        cmake --build build --config Debug --target OrcaSlicer_profile_validator
    else
        cmake --build build --config Release --target OrcaSlicer_profile_validator
    fi
    ./scripts/run_gettext.sh
    echo "done"
fi

if [[ -n "${BUILD_IMAGE}" || -n "${BUILD_ORCA}" ]] ; then
    pushd build > /dev/null
    echo "[9/9] Generating Linux app..."
    build_linux_image="./src/build_linux_image.sh"
    if [[ -e ${build_linux_image} ]] ; then
        extra_script_args=""
        if [[ -n "${BUILD_IMAGE}" ]] ; then
            extra_script_args="-i"
        fi
        ${build_linux_image} ${extra_script_args}

        echo "done"
    fi
    popd > /dev/null # build
fi

popd > /dev/null # ${SCRIPT_PATH}
