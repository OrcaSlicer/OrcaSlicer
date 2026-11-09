
include(ProcessorCount)
ProcessorCount(NPROC)

set(_python_version "3.12.13")
string(REGEX REPLACE "^([0-9]+\\.[0-9]+)\\..*" "\\1" _python_version_short "${_python_version}")
set(_python_url "https://www.python.org/ftp/python/${_python_version}/Python-${_python_version}.tar.xz")
set(_python_sha256 "c08bc65a81971c1dd5783182826503369466c7e67374d1646519adf05207b684")


if(WIN32)
    # Build-time Python for PCbuild's scripts, pinned because 3.15 cannot run the 3.12 scripts.
    set(DEP_PYTHON_HOST "" CACHE FILEPATH
        "Python 3.10-3.13 that runs CPython's build scripts; empty downloads a pinned one from nuget.org")
    if(DEP_PYTHON_HOST)
        execute_process(
            COMMAND "${DEP_PYTHON_HOST}" -E -c "import sys; print('%d.%d' % sys.version_info[:2])"
            OUTPUT_VARIABLE _python_host_minor
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _python_host_result
        )
        if(NOT _python_host_result EQUAL 0 OR _python_host_minor VERSION_LESS 3.10 OR _python_host_minor VERSION_GREATER 3.13)
            message(FATAL_ERROR "DEP_PYTHON_HOST must be Python 3.10 to 3.13, got '${_python_host_minor}' from ${DEP_PYTHON_HOST}")
        endif()
        file(TO_NATIVE_PATH "${DEP_PYTHON_HOST}" _python_host)
    else()
        set(_python_host_version "3.13.16")
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_python_host_package pythonarm64)
            set(_python_host_sha256 bfd77aa5b0edff4a7936a3220e8b79e7c78f864ce13e8fa2c835d9946e11694a)
        else()
            set(_python_host_package pythonx86)
            set(_python_host_sha256 c65cf47664b33286dfc5f2a17303668dd59e057869815ed9b270fb3866c1dd15)
        endif()
        set(_python_host_dir "${CMAKE_CURRENT_BINARY_DIR}/dep_python3_host-prefix/src/dep_python3_host")
        ExternalProject_Add(dep_python3_host
            URL "https://api.nuget.org/v3-flatcontainer/${_python_host_package}/${_python_host_version}/${_python_host_package}.${_python_host_version}.nupkg"
            URL_HASH SHA256=${_python_host_sha256}
            DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/python3
            # Renames the .nupkg to .zip so ExternalProject extracts it.
            DOWNLOAD_NAME ${_python_host_package}.${_python_host_version}.zip
            SOURCE_DIR "${_python_host_dir}"
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ""
            INSTALL_COMMAND ""
        )
        file(TO_NATIVE_PATH "${_python_host_dir}/tools/python.exe" _python_host)
        set(_python_depends dep_python3_host)
    endif()

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

    # pybind11 undefines _DEBUG around Python.h so a debug build links the
    # release python3xx.lib; Py_DEBUG could not load release plugin modules.
    set(_python_pcbuild_config Release)

    # CPython's own libraries, fetched here instead of by get_externals.bat, which ignores failed
    # downloads. Must match PCbuild/get_externals.bat for _python_version.
    set(_python_externals
        # repo       folder                 commit                                    sha256 of the archive
        source-deps  bzip2-1.0.8            05301997b2f9590f49c672cf3dfd3d3dfa7ad521  44b15bede52b32f8673a2976621a847258b041243c003e30f4d9a05214785fe5
        source-deps  sqlite-3.49.1.0        cb7a3997059739fe9c63b2819aeda7c6907510ef  586fe469463f5b27b749f9eef1938baa8e8a12030341e96160a371356c2d6759
        source-deps  xz-5.2.5               c6bc0c612605622aaef101a33a751f9de2ecc193  d99882a8f9fd48be0e605e27e3c077e5eaab370f6f52ebcc8ff754480ffdf0bc
        source-deps  zlib-1.3.1             4dc98e1909830e2bdc2a9cc2236e3c5d5037335b  39d867c083ebc37c77cf03a442f87c0f83db006eb64343bd4a19f1c84c26f50c
        bin-deps     libffi-3.4.4           94cb9a1c7feb608adf2b9f8fe2dbd6925ffbf90d  6b0f53490e0dd958708bff32df689cf2163c214147fcb8edb76a403fa6a877ca
        bin-deps     openssl-bin-3.0.16.2   1174fd43d858425c345b16d631716fb98ba492ea  b741fd1411a36cb899fee2c39b70a4492f4bc1c6e2301d67df32e9754118a63b
    )
    set(_python_externals_dir "${CMAKE_CURRENT_BINARY_DIR}/dep_python3_externals")
    list(LENGTH _python_externals _python_externals_len)
    math(EXPR _python_externals_last "${_python_externals_len} - 1")
    foreach(_i RANGE 0 ${_python_externals_last} 4)
        math(EXPR _j "${_i} + 1")
        math(EXPR _k "${_i} + 2")
        math(EXPR _l "${_i} + 3")
        list(GET _python_externals ${_i} _ext_repo)
        list(GET _python_externals ${_j} _ext_name)
        list(GET _python_externals ${_k} _ext_commit)
        list(GET _python_externals ${_l} _ext_sha256)
        ExternalProject_Add(dep_python3_ext_${_ext_name}
            URL "https://github.com/python/cpython-${_ext_repo}/archive/${_ext_commit}.zip"
            URL_HASH SHA256=${_ext_sha256}
            DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/python3
            DOWNLOAD_NAME cpython-${_ext_repo}-${_ext_name}.zip
            SOURCE_DIR "${_python_externals_dir}/${_ext_name}"
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ""
            INSTALL_COMMAND ""
        )
        list(APPEND _python_depends dep_python3_ext_${_ext_name})
    endforeach()
    file(TO_NATIVE_PATH "${_python_externals_dir}" _python_externals_native)

    # PCbuild runs PYTHON without -E, so PYTHONHOME and PYTHONPATH are cleared.
    set(_python_env_args --unset=PYTHONHOME --unset=PYTHONPATH "PYTHON=${_python_host}"
        "EXTERNALS_DIR=${_python_externals_native}"
        "GIT_CEILING_DIRECTORIES=<SOURCE_DIR>/..")

    # CPython's PCbuild needs a 64-bit-hosted toolchain: find_msbuild.bat picks the
    # 32-bit Bin\MSBuild.exe, whose x86 cl.exe/link.exe run out of address space
    # (fatal C1002 "out of heap space") building the LTCG-optimized pythoncore.
    # That 32-bit MSBuild ignores PreferredToolArchitecture, so build.bat must be
    # pointed at a 64-bit MSBuild via the MSBUILD env var. The native arm64 MSBuild
    # then hosts arm64 tools on its own; the amd64 MSBuild still defaults to x86, so
    # PreferredToolArchitecture pins it to x64. Scoped to this build step, so
    # CPython's own sources stay untouched.
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

    # MSBuild reads extra switches from PCbuild/msbuild.rsp.
    set(_python_rsp "/p:PlatformToolset=${_python_platform_toolset}\n")
    # VS 2026's ARM64 code generator needs about 27 GB for one function in
    # Objects/unicodectype.c (python/cpython#153668); the property sheet compiles
    # that file without optimisation.
    if(_python_pcbuild_platform STREQUAL "ARM64")
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_LIST_DIR}/arm64-unicodectype.props" _python_arm64_props)
        string(APPEND _python_rsp "/p:ForceImportAfterCppTargets=\"${_python_arm64_props}\"\n")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/python3-msbuild.rsp" "${_python_rsp}")
    set(_conf_cmd
        ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/python3-msbuild.rsp" <SOURCE_DIR>/PCbuild/msbuild.rsp
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

    # The macOS 27 SDK declares pipe2() and dup3() as available from macOS 27, so
    # configure finds them and CPython 3.12 calls them without a runtime check.
    # Below a macOS 27 deployment target they are weak-linked and resolve to NULL
    # on older systems, where os.pipe() then segfaults -- in `make install`
    # (compileall, ensurepip) and in the shipped app alike. Every configure below
    # keeps the pipe()/dup2() fallbacks (python/cpython#153711).
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
                 --build=${_python_build_arch}-apple-darwin \
                 ac_cv_func_pipe2=no ac_cv_func_dup3=no && \
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
                 ac_cv_func_pipe2=no ac_cv_func_dup3=no \
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
            ac_cv_func_pipe2=no
            ac_cv_func_dup3=no
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
    DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/python3
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ${_conf_cmd}
    BUILD_COMMAND ${_build_cmd}
    INSTALL_COMMAND ${_install_cmd}
    DEPENDS ${_python_depends}
)

# Python depends on OpenSSL and ZLIB
if(TARGET dep_OpenSSL)
    add_dependencies(dep_python3 dep_OpenSSL)
endif()
if(TARGET dep_ZLIB)
    add_dependencies(dep_python3 dep_ZLIB)
endif()

if (NOT WIN32)
    # _ssl and _hashlib embed the static OpenSSL built in deps/ (Windows uses
    # CPython's prebuilt OpenSSL instead). Their Makefile rules depend only on
    # their own sources, not on the OpenSSL archives, so a rebuilt OpenSSL does
    # not make them relink and they keep the previous one. On an incremental tree,
    # drop the built modules and relink them against the current OpenSSL; a
    # fresh build is left alone (its PGO target builds them). "make" alone is a
    # no-op once PGO has run, so sharedmods is invoked explicitly.
    ExternalProject_Get_Property(dep_python3 SOURCE_DIR)
    file(GLOB _python_ssl_modules
        "${SOURCE_DIR}/Modules/_ssl*.so"
        "${SOURCE_DIR}/Modules/_hashlib*.so")
    if (_python_ssl_modules)
        ExternalProject_Add_Step(dep_python3 relink_ssl_extensions
            DEPENDEES configure
            DEPENDERS build
            COMMAND sh -c "rm -f '${SOURCE_DIR}'/Modules/_ssl*.so '${SOURCE_DIR}'/Modules/_hashlib*.so && make -j${NPROC} sharedmods"
            WORKING_DIRECTORY "${SOURCE_DIR}"
            COMMENT "CPython: relinking _ssl/_hashlib against the current OpenSSL"
            DEPENDS "${CMAKE_CURRENT_LIST_FILE}"
                    "${CMAKE_CURRENT_LIST_DIR}/../OpenSSL/OpenSSL.cmake"
        )
    endif ()
endif ()
