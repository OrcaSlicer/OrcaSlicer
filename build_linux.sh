#!/usr/bin/env bash
set -e # Exit immediately if a command exits with a non-zero status.
SECONDS=0

SCRIPT_NAME=$(basename "$0")
SCRIPT_PATH=$(dirname "$(readlink -f "${0}")")

pushd "${SCRIPT_PATH}" > /dev/null

function usage() {
    echo "Usage: ./${SCRIPT_NAME} [-1][-b][-c][-d][-D][-e][-F][-g][-h][-i][-j N][-p][-r][-s][-t][-u][-l][-L]"
    echo "   -1: limit builds to one core (where possible)"
    echo "   -j N: limit builds to N cores (where possible)"
    echo "   -b: build in Debug mode"
    echo "   -c: force a clean build"
    echo "   -C: enable ANSI-colored compile output (GNU/Clang only)"
    echo "   -d: download and build dependencies in ./deps/ (build prerequisite)"
    echo "   -D: dry run"
    echo "   -e: build in RelWithDebInfo mode"
    echo "   -F: rebuild the cached Docker/Podman runner image from scratch when used with -g"
    echo "   -g: run the requested build steps inside a Docker/Podman Ubuntu 24.04 container similar to the GitHub Actions Linux runner"
    echo "   -h: prints this help text"
    echo "   -i: build the Orca Slicer AppImage (optional)"
    echo "   -p: boost ccache hit rate by disabling precompiled headers (default: ON)"
    echo "   -r: skip RAM and disk checks (low RAM compiling)"
    echo "   -s: build the Orca Slicer (optional)"
    echo "   -t: build tests (optional), requires -s flag"
    echo "   -u: install system dependencies (asks for sudo password; build prerequisite)"
    echo "   -l: use Clang instead of GCC (default: GCC)"
    echo "   -L: use ld.lld as linker (if available)"
    echo "For a first use, you want to './${SCRIPT_NAME} -u'"
    echo "   and then './${SCRIPT_NAME} -dsi'"
    echo "For a GitHub Actions-like Linux build locally, use './${SCRIPT_NAME} -g -istrlL'"
    echo "Use './${SCRIPT_NAME} -gF -istrlL' to rebuild the cached runner image first."
    echo "Set ORCA_CONTAINER_CLI, ORCA_DOCKER_IMAGE, ORCA_DOCKER_BASE_IMAGE, or ORCA_DOCKER_CMAKE_VERSION to override the container runtime, cached image tag, base image, or CMake version."
}

SLIC3R_PRECOMPILED_HEADERS="ON"

unset name
BUILD_DIR=build
BUILD_CONFIG=Release
FORWARDED_ARGS=()
while getopts ":1j:bcCdDeFghiprstulL" opt ; do
  case ${opt} in
    1 )
        export CMAKE_BUILD_PARALLEL_LEVEL=1
        FORWARDED_ARGS+=("-1")
        ;;
    j )
        export CMAKE_BUILD_PARALLEL_LEVEL=$OPTARG
        FORWARDED_ARGS+=("-j" "$OPTARG")
        ;;
    b )
        BUILD_DIR=build-dbg
        BUILD_CONFIG=Debug
        FORWARDED_ARGS+=("-b")
        ;;
    c )
        CLEAN_BUILD=1
        FORWARDED_ARGS+=("-c")
        ;;
    C )
        COLORED_OUTPUT="-DCOLORED_OUTPUT=ON"
        FORWARDED_ARGS+=("-C")
        ;;
    d )
        BUILD_DEPS="1"
        FORWARDED_ARGS+=("-d")
        ;;
    D )
        DRY_RUN="1"
        FORWARDED_ARGS+=("-D")
        ;;
    e )
        BUILD_DIR=build-dbginfo
        BUILD_CONFIG=RelWithDebInfo
        FORWARDED_ARGS+=("-e")
        ;;
    F )
        CLEAN_DOCKER_IMAGE="1"
        ;;
    g )
        USE_DOCKER="1"
        ;;
    h ) usage
        exit 1
        ;;
    i )
        BUILD_IMAGE="1"
        FORWARDED_ARGS+=("-i")
        ;;
    p )
        SLIC3R_PRECOMPILED_HEADERS="OFF"
        FORWARDED_ARGS+=("-p")
        ;;
    r )
        SKIP_RAM_CHECK="1"
        FORWARDED_ARGS+=("-r")
        ;;
    s )
        BUILD_ORCA="1"
        FORWARDED_ARGS+=("-s")
        ;;
    t )
        BUILD_TESTS="1"
        FORWARDED_ARGS+=("-t")
        ;;
    u )
        export UPDATE_LIB="1"
        FORWARDED_ARGS+=("-u")
        ;;
    l )
        USE_CLANG="1"
        FORWARDED_ARGS+=("-l")
        ;;
    L )
        USE_LLD="1"
        FORWARDED_ARGS+=("-L")
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

if [[ -n "${CLEAN_DOCKER_IMAGE}" ]] && [[ -z "${USE_DOCKER}" ]] ; then
    echo "Error: -F requires -g."
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

function resolve_container_cli() {
    if [[ -n "${ORCA_CONTAINER_CLI}" ]] ; then
        if ! command -v "${ORCA_CONTAINER_CLI}" >/dev/null 2>&1 ; then
            echo "Error: container runtime '${ORCA_CONTAINER_CLI}' was not found." >&2
            exit 1
        fi

        echo "${ORCA_CONTAINER_CLI}"
        return
    fi

    if command -v docker >/dev/null 2>&1 ; then
        echo "docker"
        return
    fi

    if command -v podman >/dev/null 2>&1 ; then
        echo "podman"
        return
    fi

    echo "Error: neither docker nor podman is available. Install one of them or set ORCA_CONTAINER_CLI." >&2
    exit 1
}

function get_docker_runner_image() {
    local base_image
    local docker_cmake_version
    local recipe_hash
    local sanitized_base_image
    local sanitized_cmake_version

    if [[ -n "${ORCA_DOCKER_IMAGE}" ]] ; then
        echo "${ORCA_DOCKER_IMAGE}"
        return
    fi

    base_image="${ORCA_DOCKER_BASE_IMAGE:-ubuntu:24.04}"
    docker_cmake_version="${ORCA_DOCKER_CMAKE_VERSION-4.3.0}"
    recipe_hash=$(find "${SCRIPT_PATH}/build_linux.sh" "${SCRIPT_PATH}/scripts/linux.d" -type f -print0 | sort -z | xargs -0 cat | sha256sum | cut -c1-12)
    sanitized_base_image=$(echo "${base_image}" | tr '/:@' '---' | tr -cs 'A-Za-z0-9_.-' '-')
    sanitized_cmake_version=$(echo "${docker_cmake_version:-system}" | tr -cs 'A-Za-z0-9_.-' '-')
    echo "orcaslicer-linux-builder:${sanitized_base_image}-cmake-${sanitized_cmake_version}-${recipe_hash}"
}

function docker_runner_dockerfile() {
    cat <<'EOF'
ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ARG CMAKE_VERSION=4.3.0

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-c"]

RUN apt-get update && apt-get install -y sudo ca-certificates curl tar

COPY build_linux.sh /tmp/orcaslicer/build_linux.sh
COPY scripts/linux.d /tmp/orcaslicer/scripts/linux.d

WORKDIR /tmp/orcaslicer

RUN chmod +x ./build_linux.sh
RUN ./build_linux.sh -ur
RUN if [[ -n "${CMAKE_VERSION}" ]] ; then \
        case "$(uname -m)" in \
            x86_64|amd64) cmake_arch="x86_64" ;; \
            aarch64|arm64) cmake_arch="aarch64" ;; \
            *) cmake_arch="" ;; \
        esac ; \
        if [[ -n "${cmake_arch}" ]] ; then \
            cmake_root="/opt/cmake-${CMAKE_VERSION}-linux-${cmake_arch}" ; \
            if ! curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${cmake_arch}.tar.gz" | tar -xz -C /opt ; then \
                echo "Warning: failed to install CMake ${CMAKE_VERSION}; falling back to the distro cmake package." ; \
            elif [[ -d "${cmake_root}" ]] ; then \
                ln -sf "${cmake_root}/bin/"* /usr/local/bin/ ; \
            fi ; \
        else \
            echo "Skipping GitHub Actions CMake install for unsupported architecture $(uname -m)." ; \
        fi ; \
    fi
RUN rm -rf /var/lib/apt/lists/* /tmp/orcaslicer
EOF
}

function ensure_docker_runner_image() {
    local container_cli
    local base_image
    local runner_image
    local docker_cmake_version
    local image_exists="0"
    local force_rebuild="0"
    local -a build_cmd

    container_cli="$1"
    runner_image="$2"
    base_image="${ORCA_DOCKER_BASE_IMAGE:-ubuntu:24.04}"
    docker_cmake_version="${ORCA_DOCKER_CMAKE_VERSION-4.3.0}"

    if "${container_cli}" image inspect "${runner_image}" >/dev/null 2>&1 ; then
        image_exists="1"
    fi

    if [[ -n "${CLEAN_DOCKER_IMAGE}" ]] ; then
        force_rebuild="1"
        if [[ "${image_exists}" == "1" ]] ; then
            echo "Removing cached container image ${runner_image} ..."
            if [[ -z "${DRY_RUN}" ]] ; then
                "${container_cli}" image rm -f "${runner_image}" >/dev/null
            else
                printf '%q ' "${container_cli}" image rm -f "${runner_image}"
                echo
            fi
            image_exists="0"
        fi
    fi

    if [[ "${image_exists}" == "1" ]] ; then
        echo "Using cached container image ${runner_image}"
        return
    fi

    build_cmd=(
        "${container_cli}" build --pull
        -t "${runner_image}"
        --build-arg "BASE_IMAGE=${base_image}"
        --build-arg "CMAKE_VERSION=${docker_cmake_version}"
    )
    if [[ "${force_rebuild}" == "1" ]] ; then
        build_cmd+=(--no-cache)
    fi
    build_cmd+=(-f - "${SCRIPT_PATH}")

    printf '%q ' "${build_cmd[@]}"
    echo
    if [[ -n "${DRY_RUN}" ]] ; then
        return
    fi

    docker_runner_dockerfile | "${build_cmd[@]}"
}

function run_in_docker() {
    local container_cli
    local runner_image
    local container_workspace
    local host_uid
    local host_gid
    local host_user
    local -a build_args
    local -a container_env

    container_cli=$(resolve_container_cli)
    runner_image=$(get_docker_runner_image)
    host_uid=$(id -u)
    host_gid=$(id -g)
    host_user="${USER:-orca}"
    container_workspace="/__w/OrcaSlicer/OrcaSlicer"
    build_args=()
    for item in "${FORWARDED_ARGS[@]}" ; do
        if [[ "${item}" == "-u" ]] || [[ "${item}" == "-D" ]] ; then
            continue
        fi

        build_args+=("${item}")
    done

    container_env=(
        -e "CI=true"
        -e "GITHUB_ACTIONS=true"
        -e "GITHUB_WORKSPACE=${container_workspace}"
        -e "RUNNER_OS=Linux"
        -e "RUNNER_TEMP=/tmp"
        -e "HOST_UID=${host_uid}"
        -e "HOST_GID=${host_gid}"
        -e "HOST_USER=${host_user}"
    )
    if [[ -n "${CMAKE_BUILD_PARALLEL_LEVEL}" ]] ; then
        container_env+=( -e "CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL}" )
    fi
    if [[ -n "${ORCA_UPDATER_SIG_KEY}" ]] ; then
        container_env+=( -e "ORCA_UPDATER_SIG_KEY=${ORCA_UPDATER_SIG_KEY}" )
    fi

    ensure_docker_runner_image "${container_cli}" "${runner_image}"

    printf '%q ' "${container_cli}" run --rm -i \
        -v "${SCRIPT_PATH}:${container_workspace}" \
        -w "${container_workspace}" \
        "${container_env[@]}" \
        "${runner_image}" \
        bash -s -- "${build_args[@]}"
    echo
    if [[ -n "${DRY_RUN}" ]] ; then
        return
    fi

    "${container_cli}" run --rm -i \
        -v "${SCRIPT_PATH}:${container_workspace}" \
        -w "${container_workspace}" \
        "${container_env[@]}" \
        "${runner_image}" \
        bash -s -- "${build_args[@]}" <<'EOF'
set -e

function create_builder_user() {
    if [[ "${HOST_UID}" == "0" ]] ; then
        HOST_USER=root
        return
    fi

    if getent group "${HOST_GID}" >/dev/null 2>&1 ; then
        HOST_GROUP=$(getent group "${HOST_GID}" | cut -d: -f1)
    else
        HOST_GROUP="orca-builder"
        if getent group "${HOST_GROUP}" >/dev/null 2>&1 ; then
            HOST_GROUP="orca-builder-${HOST_GID}"
        fi
        groupadd -g "${HOST_GID}" "${HOST_GROUP}"
    fi

    if getent passwd "${HOST_UID}" >/dev/null 2>&1 ; then
        HOST_USER=$(getent passwd "${HOST_UID}" | cut -d: -f1)
        usermod -g "${HOST_GROUP}" "${HOST_USER}"
    elif id -u "${HOST_USER}" >/dev/null 2>&1 ; then
        usermod -u "${HOST_UID}" -g "${HOST_GROUP}" "${HOST_USER}"
    else
        useradd -m -u "${HOST_UID}" -g "${HOST_GROUP}" -s /bin/bash "${HOST_USER}"
    fi

    echo "${HOST_USER} ALL=(ALL) NOPASSWD:ALL" >/etc/sudoers.d/orcaslicer-builder
    chmod 0440 /etc/sudoers.d/orcaslicer-builder
}

create_builder_user
mkdir -p "${GITHUB_WORKSPACE}/deps/build/destdir"
chown -R "${HOST_UID}:${HOST_GID}" "${GITHUB_WORKSPACE}/deps/build"
if [[ -d "${GITHUB_WORKSPACE}/build" ]] ; then
    chown -R "${HOST_UID}:${HOST_GID}" "${GITHUB_WORKSPACE}/build"
fi
if [[ -d "${GITHUB_WORKSPACE}/build-dbg" ]] ; then
    chown -R "${HOST_UID}:${HOST_GID}" "${GITHUB_WORKSPACE}/build-dbg"
fi
if [[ -d "${GITHUB_WORKSPACE}/build-dbginfo" ]] ; then
    chown -R "${HOST_UID}:${HOST_GID}" "${GITHUB_WORKSPACE}/build-dbginfo"
fi

sudo -H -u "${HOST_USER}" env \
    CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL-}" \
    GITHUB_WORKSPACE="${GITHUB_WORKSPACE}" \
    ORCA_UPDATER_SIG_KEY="${ORCA_UPDATER_SIG_KEY-}" \
    bash -c '
        set -e
        cd "${GITHUB_WORKSPACE}"
        if [[ "$#" -gt 0 ]] ; then
            ./build_linux.sh "$@"
        else
            echo "No build steps were requested after container setup."
        fi
    ' bash "$@"
EOF
}

if [[ -n "${USE_DOCKER}" ]] ; then
    run_in_docker
    popd > /dev/null # ${SCRIPT_PATH}
    exit 0
fi

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
elif [[ "${DISTRIBUTION_LIKE}" == *"suse"* ]] ; then
    DISTRIBUTION="suse"
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
    print_and_run cmake --build deps/$BUILD_DIR -j1
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

elapsed=$SECONDS
printf "\nBuild completed in %dh %dm %ds\n" $((elapsed/3600)) $((elapsed%3600/60)) $((elapsed%60))

popd > /dev/null # ${SCRIPT_PATH}
