
include(ProcessorCount)
ProcessorCount(NPROC)

set(_python_version "3.12.13")
string(REGEX REPLACE "^([0-9]+\\.[0-9]+)\\..*" "\\1" _python_version_short "${_python_version}")
set(_python_url "https://www.python.org/ftp/python/${_python_version}/Python-${_python_version}.tar.xz")
set(_python_sha256 "c08bc65a81971c1dd5783182826503369466c7e67374d1646519adf05207b684")


set(_patch_cmd "")
if(WIN32)

    # Fix python build failure on Windows if python is not available, due to wrong nuget download URL
    # See https://github.com/python/cpython/issues/153438
    # Patch from https://github.com/python/cpython/pull/153608
    # This patch has not been merged to 3.12 yet so we need to apply it manually
    set(_patch_cmd git init && ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/01-windows-nuget.patch)

    if(MSVC_VERSION EQUAL 1800)
        set(_python_platform_toolset v120)
    elseif(MSVC_VERSION EQUAL 1900)
        set(_python_platform_toolset v140)
    elseif(MSVC_VERSION LESS 1920)
        set(_python_platform_toolset v141)
    elseif(MSVC_VERSION LESS 1930)
        set(_python_platform_toolset v142)
    elseif(MSVC_VERSION LESS 1950)
        set(_python_platform_toolset v143)
    elseif(MSVC_VERSION LESS 1960)
        set(_python_platform_toolset v145)
    else()
        message(FATAL_ERROR "Unsupported MSVC version for CPython build: ${MSVC_VERSION}")
    endif()

    # 64-bit-hosted MSBuild selection (see the build-step comment below). Default
    # to the amd64 host + x64 tools; only the native ARM64 build differs.
    set(_python_msbuild_host amd64)
    set(_python_tool_arch x64)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_python_pcbuild_platform ARM64)
        set(_python_layout_arch arm64)
        set(_python_pcbuild_output_dir arm64)
        set(_python_msbuild_host arm64)   # native ARM64 MSBuild already hosts arm64 tools
        set(_python_tool_arch "")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_python_pcbuild_platform x64)
        set(_python_layout_arch amd64)
        set(_python_pcbuild_output_dir amd64)
    else()
        set(_python_pcbuild_platform Win32)
        set(_python_layout_arch win32)
        set(_python_pcbuild_output_dir win32)
    endif()

    set(_python_pcbuild_config Release)
    set(_python_layout_debug OFF)
    if(DEFINED DEP_DEBUG AND DEP_DEBUG)
        set(_python_pcbuild_config Debug)
        set(_python_layout_debug ON)
    endif()

    # CPython's PCbuild needs a 64-bit-hosted toolchain: find_msbuild.bat picks the
    # 32-bit Bin\MSBuild.exe, whose x86 cl.exe/link.exe run out of address space
    # (fatal C1002 "out of heap space") building the LTCG-optimized pythoncore.
    # That 32-bit MSBuild ignores PreferredToolArchitecture, so build.bat must be
    # pointed at a 64-bit MSBuild via the MSBUILD env var. The native arm64 MSBuild
    # then hosts arm64 tools on its own; the amd64 MSBuild still defaults to x86, so
    # PreferredToolArchitecture pins it to x64. Scoped to this build step, so
    # CPython's own sources stay untouched.
    set(_python_env_args "GIT_CEILING_DIRECTORIES=<SOURCE_DIR>/..")
    if(CMAKE_GENERATOR_INSTANCE)   # empty for non-VS generators (e.g. Ninja)
        set(_python_msbuild "${CMAKE_GENERATOR_INSTANCE}/MSBuild/Current/Bin/${_python_msbuild_host}/MSBuild.exe")
        if(EXISTS "${_python_msbuild}")
            file(TO_NATIVE_PATH "${_python_msbuild}" _python_msbuild_native)
            list(APPEND _python_env_args "MSBUILD=${_python_msbuild_native}")
        else()
            # Loud signal: a silent fall-through to the 32-bit MSBuild reintroduces C1002.
            message(WARNING "Bundled Python: 64-bit MSBuild not found at '${_python_msbuild}'. "
                "CPython will fall back to find_msbuild.bat's default (32-bit) MSBuild, which may "
                "fail with C1002 (out of heap space) building the optimized pythoncore.")
        endif()
    endif()
    if(_python_tool_arch)
        list(APPEND _python_env_args "PreferredToolArchitecture=${_python_tool_arch}")
    endif()

    set(_conf_cmd
        cmd /c "echo /p:PlatformToolset=${_python_platform_toolset}>PCbuild\\msbuild.rsp"
    )
    set(_build_cmd
        ${CMAKE_COMMAND} -E env ${_python_env_args}
        cmd /c PCbuild\\build.bat
            -p ${_python_pcbuild_platform}
            -c ${_python_pcbuild_config}
            --no-tkinter
    )
    set(_install_cmd
        ${CMAKE_COMMAND}
            -DPYTHON_SOURCE_DIR=<SOURCE_DIR>
            -DPYTHON_BUILD_DIR=<SOURCE_DIR>/PCbuild/${_python_pcbuild_output_dir}
            -DPYTHON_DEST_DIR=${DESTDIR}/libpython
            -DPYTHON_LAYOUT_ARCH=${_python_layout_arch}
            -DPYTHON_DEBUG=${_python_layout_debug}
            -P ${CMAKE_CURRENT_LIST_DIR}/stage_windows.cmake
    )
elseif(APPLE)
    # macOS configuration
    if(CMAKE_OSX_ARCHITECTURES)
        set(_python_target_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_python_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(_python_build_arch aarch64)
        set(_python_build_arch_flag "arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        set(_python_build_arch x86_64)
        set(_python_build_arch_flag "x86_64")
    else()
        set(_python_build_arch "${CMAKE_SYSTEM_PROCESSOR}")
        set(_python_build_arch_flag "${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(_python_target_arch MATCHES "ARM64|arm64|aarch64")
        set(_python_host_arch aarch64)
        set(_python_arch_flag "arm64")
    elseif(_python_target_arch MATCHES "x86_64|AMD64|amd64")
        set(_python_host_arch x86_64)
        set(_python_arch_flag "x86_64")
    else()
        message(FATAL_ERROR "Unsupported macOS Python target architecture: ${_python_target_arch}")
    endif()

    set(_python_arch_flags "-arch ${_python_arch_flag} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    # No -rpath: all other deps are static, so libpython has no shared
    # dependencies to find there. headerpad reserves load-command space for
    # the post-install -add_rpath below.
    set(_python_ldflags "${_python_arch_flags} -Wl,-headerpad_max_install_names")

    if(IS_CROSS_COMPILE)
        set(_python_build_tgt --build=${_python_build_arch}-apple-darwin --host=${_python_host_arch}-apple-darwin)
        set(_python_build_arch_flags "-arch ${_python_build_arch_flag} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        set(_python_build_ldflags "${_python_build_arch_flags} -Wl,-rpath,${DESTDIR}/lib")
        set(_python_build_python_dir "<SOURCE_DIR>/build-python-host")
        set(_python_build_python "${_python_build_python_dir}/python")
        set(_conf_cmd
            /bin/sh -c
            "rm -rf '${_python_build_python_dir}' && \
             mkdir -p '${_python_build_python_dir}' && \
             cd '${_python_build_python_dir}' && \
             env \
               CC='${CMAKE_C_COMPILER}' \
               CXX='${CMAKE_CXX_COMPILER}' \
               CFLAGS='${_python_build_arch_flags}' \
               CXXFLAGS='${_python_build_arch_flags}' \
               LDFLAGS='${_python_build_ldflags}' \
               MACOSX_DEPLOYMENT_TARGET='${CMAKE_OSX_DEPLOYMENT_TARGET}' \
               ../configure \
                 --prefix='${_python_build_python_dir}/install' \
                 --enable-shared \
                 --without-static-libpython \
                 --disable-test-modules \
                 --build=${_python_build_arch}-apple-darwin && \
             make -j${NPROC} python && \
             cd '<SOURCE_DIR>' && \
             env \
               CC='${CMAKE_C_COMPILER}' \
               CXX='${CMAKE_CXX_COMPILER}' \
               CFLAGS='${_python_arch_flags}' \
               CXXFLAGS='${_python_arch_flags}' \
               LDFLAGS='${_python_ldflags}' \
               MACOSX_DEPLOYMENT_TARGET='${CMAKE_OSX_DEPLOYMENT_TARGET}' \
               ./configure \
                 --prefix='${DESTDIR}/libpython' \
                 --enable-shared \
                 --enable-optimizations \
                 --without-static-libpython \
                 --with-openssl='${DESTDIR}' \
                 --disable-test-modules \
                 ${_python_build_tgt} \
                 --with-build-python='${_python_build_python}' \
                 py_cv_module__tkinter=n/a"
        )
    else()
        set(_python_build_tgt --build=${_python_host_arch}-apple-darwin)
        set(_conf_cmd
            env
            "CC=${CMAKE_C_COMPILER}"
            "CXX=${CMAKE_CXX_COMPILER}"
            "CFLAGS=${_python_arch_flags}"
            "CXXFLAGS=${_python_arch_flags}"
            "LDFLAGS=${_python_ldflags}"
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            ./configure
            --prefix=${DESTDIR}/libpython
            --enable-shared
            --enable-optimizations
            --without-static-libpython
            --with-openssl=${DESTDIR}
            --disable-test-modules
            ${_python_build_tgt}
            # Tcl/Tk 9.0 (e.g. from Homebrew) is incompatible with CPython 3.12's
            # _tkinter; OrcaSlicer's embedded Python does not need tkinter anyway.
            py_cv_module__tkinter=n/a
        )
    endif()
    set(_build_cmd make -j${NPROC})

    # CPython stamps libpython with an absolute install name ($prefix/lib/...),
    # which every consumer inherits at link time and which only exists on the
    # build host. Normalize once here, before anything links against the dep:
    # give the dylib an @rpath id and teach the interpreter to find it relative
    # to itself. Consumers then just need an rpath entry (src/CMakeLists.txt).
    # install_name_tool invalidates code signatures, so re-sign ad-hoc; CI
    # re-signs the whole bundle with the real identity later.
    # ld collapses '//' in -install_name (but not in -rpath) strings, while
    # ${DESTDIR} ends with a slash -- collapse slashes so -change matches the
    # recorded install name.
    string(REGEX REPLACE "/+" "/" _python_prefix "${DESTDIR}/libpython")
    set(_python_dylib "${_python_prefix}/lib/libpython${_python_version_short}.dylib")
    set(_python_bin "${_python_prefix}/bin/python${_python_version_short}")
    set(_install_cmd make install
        COMMAND install_name_tool -id "@rpath/libpython${_python_version_short}.dylib" "${_python_dylib}"
        COMMAND install_name_tool -change "${_python_dylib}" "@rpath/libpython${_python_version_short}.dylib" "${_python_bin}"
        COMMAND install_name_tool -add_rpath "@loader_path/../lib" "${_python_bin}"
        COMMAND codesign --force --sign - "${_python_dylib}"
        COMMAND codesign --force --sign - "${_python_bin}"
    )
else()
    # Linux/Unix
    # Kept verbatim, no slash normalization (unlike the macOS branch's
    # collapsed copy): the LDFLAGS rpath below is recorded byte-for-byte in
    # the ELF, and the OLD_RPATH handed to relocate_linux.cmake must match it
    # exactly -- both derive from this one variable to make that structural.
    set(_python_prefix "${DESTDIR}/libpython")
    # The rpath points at libpython's real install dir, so the interpreter runs
    # in-tree pre-relocation -- and, critically, it reserves enough RUNPATH
    # bytes for the in-place $ORIGIN rewrite at install time (Flatpak's
    # DESTDIR is the short /app) -- see relocate_linux.cmake.
    set(_conf_cmd ./configure
        --prefix=${_python_prefix}
        --enable-shared
        --enable-optimizations
        --with-openssl=${DESTDIR}
        --without-static-libpython
        --disable-test-modules
        # Tcl/Tk 9.0 is incompatible with CPython 3.12's _tkinter; not needed here.
        py_cv_module__tkinter=n/a
        LDFLAGS=-Wl,-rpath,${_python_prefix}/lib
    )
    set(_build_cmd make -j${NPROC})
    set(_install_cmd make install
        COMMAND ${CMAKE_COMMAND}
            "-DPYTHON_BIN=${_python_prefix}/bin/python${_python_version_short}"
            "-DOLD_RPATH=${_python_prefix}/lib"
            -P "${CMAKE_CURRENT_LIST_DIR}/relocate_linux.cmake"
    )
endif()

ExternalProject_Add(dep_python3
    URL "${_python_url}"
    URL_HASH SHA256=${_python_sha256}
    PATCH_COMMAND ${_patch_cmd}
    DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/python3
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ${_conf_cmd}
    BUILD_COMMAND ${_build_cmd}
    INSTALL_COMMAND ${_install_cmd}
)

# Python depends on OpenSSL and ZLIB
if(TARGET dep_OpenSSL)
    add_dependencies(dep_python3 dep_OpenSSL)
endif()
if(TARGET dep_ZLIB)
    add_dependencies(dep_python3 dep_ZLIB)
endif()
