# Wasmtime — Bytecode Alliance WebAssembly runtime (C API).
#
# Phase 3.2.1 — pulls a pre-built Wasmtime C-API tarball matching the host
# platform, extracts it, and installs include/* + lib/* into ${DESTDIR}.
# No Rust toolchain or local CMake configure is required; the upstream
# release ships a fully self-contained static + shared library pair.
#
# Pinned to v45.0.0 (released 2026-05-21). When bumping, look up new SHA256
# digests with:
#   gh api repos/bytecodealliance/wasmtime/releases/tags/<tag> \
#     --jq '.assets[] | select(.name|test("-c-api\\.")) | "\(.name) \(.digest)"'
# and update the per-arch URL_HASH entries below. Never hand-edit the version
# anywhere else — bump it here and re-run the deps build.

set(_wasmtime_version "v45.0.0")
set(_wasmtime_url_base "https://github.com/bytecodealliance/wasmtime/releases/download/${_wasmtime_version}")

# -----------------------------------------------------------------------------
# Pick the tarball variant for the build host. Cross-compile (e.g. macOS
# universal2 builds requesting both arm64 + x86_64) is not supported here —
# users should produce per-arch builds and lipo-combine outside the dep step.
# -----------------------------------------------------------------------------
set(_wasmtime_archive "")
set(_wasmtime_sha256  "")
set(_wasmtime_is_zip  OFF)

if (WIN32)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-aarch64-windows-c-api.zip")
        set(_wasmtime_sha256  "4cd4afa9bf4b34af2b6d85f346319ec4ff10fa23f04e24f7d689ba3d21336344")
    elseif (MINGW)
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-x86_64-mingw-c-api.zip")
        set(_wasmtime_sha256  "0732b65e0052f078e2bbaa32aeb4fada2e4e4aee8c04a39de2cf8e4f8697a9f5")
    else ()
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-x86_64-windows-c-api.zip")
        set(_wasmtime_sha256  "d5ee516fc141576ccd6c43146aafee1074c3c26764cba73b3a97f599a3791f9c")
    endif ()
    set(_wasmtime_is_zip ON)
elseif (APPLE)
    if (CMAKE_OSX_ARCHITECTURES MATCHES "arm" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-aarch64-macos-c-api.tar.xz")
        set(_wasmtime_sha256  "43cd87ec7d398f2e799e81c7d4e143d930e0139953d3c5d2a9c4055789f29851")
    else ()
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-x86_64-macos-c-api.tar.xz")
        set(_wasmtime_sha256  "92d6b32a31711127fde10acbf5b984fa37b94052cec783a4fca6edd0bb8cdd6f")
    endif ()
else () # Linux / other Unix
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-aarch64-linux-c-api.tar.xz")
        set(_wasmtime_sha256  "59794105fcdcd3d5dd496acc63a78cefa5fad63662b3efb9bcd21ee0616f4944")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "armv7|arm")
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-armv7-linux-c-api.tar.xz")
        set(_wasmtime_sha256  "bbf19cfa300628f7b7daddc620965be4f0961c0eee83f441a4b94ed7446cc6d7")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "i.86")
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-i686-linux-c-api.tar.xz")
        set(_wasmtime_sha256  "514054e761d2edb9c4002bd41176c68d7af774a25d4d285fc009dc2cc87e2f2d")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-riscv64gc-linux-c-api.tar.xz")
        set(_wasmtime_sha256  "0207c1477392860b1614a2e01f9403d6db88aaf8dc0c4c88b60921a306486a82")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "s390x")
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-s390x-linux-c-api.tar.xz")
        set(_wasmtime_sha256  "4c78c55314e64a30d56a2efcea9ae418fb66f47934e2fc2378b28642a7a76b9c")
    else ()
        set(_wasmtime_archive "wasmtime-${_wasmtime_version}-x86_64-linux-c-api.tar.xz")
        set(_wasmtime_sha256  "95959e7a4cc4bfc12bbe45c9dea82cf45dd5b4321d9163e66343c50728429129")
    endif ()
endif ()

if (_wasmtime_archive STREQUAL "")
    message(FATAL_ERROR "Wasmtime: no pre-built tarball matches host "
                        "(system=${CMAKE_SYSTEM_NAME}, proc=${CMAKE_SYSTEM_PROCESSOR}). "
                        "Either add a mapping above or build Wasmtime from source.")
endif ()

# Strip the archive suffix to get the extracted root dir name.
string(REGEX REPLACE "\\.(tar\\.xz|zip)$" "" _wasmtime_root "${_wasmtime_archive}")

# -----------------------------------------------------------------------------
# ExternalProject_Add — download + extract only. The "install" step does a
# raw copy of include/ + lib/ into ${DESTDIR} so the tree matches what every
# other dep produces and downstream find_path / find_library calls just work.
# -----------------------------------------------------------------------------
ExternalProject_Add(dep_Wasmtime
    EXCLUDE_FROM_ALL ON
    URL "${_wasmtime_url_base}/${_wasmtime_archive}"
    URL_HASH SHA256=${_wasmtime_sha256}
    DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/Wasmtime
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E copy_directory
            <SOURCE_DIR>/include ${DESTDIR}/include
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            <SOURCE_DIR>/lib ${DESTDIR}/lib
)
