#!/usr/bin/env bash
set -e # Exit immediately if a command exits with a non-zero status.

SCRIPT_NAME=$(basename "$0")
SCRIPT_PATH=$(dirname "$(readlink -f "${0}")")

pushd "${SCRIPT_PATH}" > /dev/null

function usage() {
    echo "Usage: ./${SCRIPT_NAME} [-1][-b][-c][-d][-D][-e][-h][-i][-j N][-p][-r][-s][-t][-u][-l][-L]"
    echo "   -1: limit builds to one core (where possible)"
    echo "   -j N: limit builds to N cores (where possible)"
    echo "   -b: build in Debug mode"
    echo "   -c: force a clean build"
    echo "   -C: enable ANSI-colored compile output (GNU/Clang only)"
    echo "   -d: download and build dependencies in ./deps/ (build prerequisite)"
    echo "   -D: dry run"
    echo "   -e: build in RelWithDebInfo mode"
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
}

SLIC3R_PRECOMPILED_HEADERS="ON"

unset name
BUILD_DIR=build
BUILD_CONFIG=Release
while getopts ":1j:bcCdDehiprstulL" opt ; do
  case ${opt} in
    1 )
        export CMAKE_BUILD_PARALLEL_LEVEL=1
        ;;
    j )
        export CMAKE_BUILD_PARALLEL_LEVEL=$OPTARG
        ;;
    b )
        BUILD_DIR=build-dbg
        BUILD_CONFIG=Debug
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
    D )
        DRY_RUN="1"
        ;;
    e )
        BUILD_DIR=build-dbginfo
        BUILD_CONFIG=RelWithDebInfo
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

function print_and_run() {
    cmd=()
    # Remove empty arguments, leading and trailing spaces
    for item in "$@" ; do
        if [[ -n $item ]]; then
            cmd+=( "$(echo "${item}" | xargs)" )
        fi
    done

    echo "${cmd[@]}"
    if [[ -z "${DRY_RUN}" ]] ; then
        "${cmd[@]}"
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

echo "FOUND_GTK3_DEV=${FOUND_GTK3_DEV}"
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
    if [[ -n "${CLEAN_BUILD}" ]]
    then
        print_and_run rm -fr deps/$BUILD_DIR
    fi
    mkdir -p deps/$BUILD_DIR
    if [[ $BUILD_CONFIG != Release ]] ; then
        BUILD_ARGS+=(-DCMAKE_BUILD_TYPE="${BUILD_CONFIG}")
    fi

    print_and_run cmake -S deps -B deps/$BUILD_DIR "${CMAKE_C_CXX_COMPILER_CLANG[@]}" "${CMAKE_LLD_LINKER_ARGS[@]}" -G Ninja "${COLORED_OUTPUT}" "${BUILD_ARGS[@]}"
    print_and_run cmake --build deps/$BUILD_DIR
fi

if [[ -n "${BUILD_ORCA}" ]] || [[ -n "${BUILD_TESTS}" ]] ; then
    echo "Configuring OrcaSlicer..."
    if [[ -n "${CLEAN_BUILD}" ]] ; then
        print_and_run rm -fr $BUILD_DIR
    fi
    read -r -a BUILD_ARGS <<< "${ORCA_EXTRA_BUILD_ARGS}"
    if [[ $BUILD_CONFIG != Release ]] ; then
        BUILD_ARGS+=(-DCMAKE_BUILD_TYPE="${BUILD_CONFIG}")
    fi
    if [[ -n "${BUILD_TESTS}" ]] ; then
        BUILD_ARGS+=(-DBUILD_TESTS=ON)
    fi
    if [[ -n "${ORCA_UPDATER_SIG_KEY}" ]] ; then
        BUILD_ARGS+=(-DORCA_UPDATER_SIG_KEY="${ORCA_UPDATER_SIG_KEY}")
    fi

    print_and_run cmake -S . -B $BUILD_DIR "${CMAKE_C_CXX_COMPILER_CLANG[@]}" "${CMAKE_LLD_LINKER_ARGS[@]}" -G "Ninja Multi-Config" \
-DSLIC3R_PCH=${SLIC3R_PRECOMPILED_HEADERS} \
-DORCA_TOOLS=ON \
"${COLORED_OUTPUT}" \
"${BUILD_ARGS[@]}"
    echo "done"
    if [[ -n "${BUILD_ORCA}" ]]; then
	echo "Building OrcaSlicer ..."
	print_and_run cmake --build $BUILD_DIR --config "${BUILD_CONFIG}" --target OrcaSlicer
	echo "Building OrcaSlicer_profile_validator .."
	print_and_run cmake --build $BUILD_DIR --config "${BUILD_CONFIG}" --target OrcaSlicer_profile_validator
	./scripts/run_gettext.sh
    fi
    if [[ -n "${BUILD_TESTS}" ]] ; then
	echo "Building tests ..."
	print_and_run cmake --build ${BUILD_DIR} --config "${BUILD_CONFIG}" --target tests/all
    fi
    echo "done"
fi

if [[ -n "${BUILD_IMAGE}" || -n "${BUILD_ORCA}" ]] ; then
    pushd $BUILD_DIR > /dev/null
    build_linux_image="./src/build_linux_image.sh"
    if [[ -e ${build_linux_image} ]] ; then
        extra_script_args=""
        if [[ -n "${BUILD_IMAGE}" ]] ; then
            extra_script_args="-i"
        fi
        print_and_run ${build_linux_image} ${extra_script_args} -R "${BUILD_CONFIG}"

        echo "done"
    fi
    popd > /dev/null # build
fi

popd > /dev/null # ${SCRIPT_PATH}
