
include(ProcessorCount)
ProcessorCount(NPROC)

if(DEFINED OPENSSL_ARCH)
    set(_cross_arch ${OPENSSL_ARCH})
else()
    if(WIN32)
        if("${DEPS_ARCH}" STREQUAL "arm64")
            set(_cross_arch "VC-WIN64-ARM")
        else()
            set(_cross_arch "VC-WIN64A")
        endif()
    elseif(APPLE)
        set(_cross_arch "darwin64-${CMAKE_OSX_ARCHITECTURES}-cc")
	endif()
endif()

set(_openssl_dir ${DESTDIR})
set(_openssl_extra_cflags "")

if(WIN32)
    set(_openssl_msvc_env CC=cl CXX=cl RC=rc CL=/FS)
    # OpenSSL's perl Configure honors the CC environment variable, but the
    # VC-WIN64A makefile only works with cl (an unquoted clang-cl path with
    # spaces, e.g. exported by CLion, silently produces no .obj files and the
    # lib step fails with LNK1181). Pin the upstream toolchain.
    # Keep rc.exe resolved from the MSVC developer environment as well. The
    # absolute Windows SDK path contains spaces and OpenSSL writes it to the
    # generated nmake file without quoting, which skips .res generation.
    # /FS serializes access to OpenSSL's shared generated PDB when cl is
    # driven through nmake from a Ninja configure step.
    set(_conf_cmd ${CMAKE_COMMAND} -E env ${_openssl_msvc_env} perl Configure )
    set(_cross_comp_prefix_line "")
    # 1.1.1's VC configs passed /Gs0, which needed a /Gs4096 override on ARM64
    # (MSVC 14.51/14.52 mis-emit the __chkstk probe). 3.x no longer sets /Gs.
    set(_make_cmd ${CMAKE_COMMAND} -E env ${_openssl_msvc_env} nmake)
    set(_install_cmd ${CMAKE_COMMAND} -E env ${_openssl_msvc_env} nmake install_sw )
else()
    if(APPLE)
        set(_conf_cmd export MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} && ./Configure -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
        # OPENSSLDIR is only where OpenSSL looks at runtime for cert.pem, certs/
        # and openssl.cnf; install_sw installs nothing there. Point it at the
        # system CA bundle, the one libcurl already uses here, so the bundled
        # Python's ssl module can verify HTTPS. Elsewhere it stays the prefix:
        # Windows Python reads the system certificate store, and on Linux, where
        # the bundle's location differs by distro, CurlGlobalInit points
        # SSL_CERT_FILE at it at startup.
        set(_openssl_dir /etc/ssl)
    else()
        # A static library that is embedded into a shared object must not export
        # its symbols. On Linux the running process also loads the system OpenSSL
        # (WebKitGTK/gnutls pull in libcrypto.so.3), and CPython's _ssl and
        # _hashlib are dlopened (RTLD_LOCAL) DSOs that each embed this OpenSSL.
        # With default visibility their unversioned OpenSSL references are
        # preempted by that global copy, mixing two OpenSSL builds and
        # corrupting the heap (ssl.create_default_context() aborts). Hidden
        # visibility makes each embedded copy self-contained. Linux-only: macOS
        # binds dylibs with a two-level namespace (no interposition) and ships no
        # OpenSSL, and Windows has no equivalent flag and no system OpenSSL to
        # collide with.
        set(_openssl_extra_cflags -fvisibility=hidden)
        set(_conf_cmd env "CC=${CMAKE_C_COMPILER}" "LDFLAGS=${CMAKE_EXE_LINKER_FLAGS}" "./config")
    endif()
    set(_cross_comp_prefix_line "")
    set(_make_cmd make -j${NPROC})
    set(_install_cmd make -j${NPROC} install_sw)
    if (CMAKE_CROSSCOMPILING)
        set(_cross_comp_prefix_line "--cross-compile-prefix=${TOOLCHAIN_PREFIX}-")

        if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "arm64")
            set(_cross_arch "linux-aarch64")
        elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armhf") # For raspbian
            # TODO: verify
            set(_cross_arch "linux-armv4")
        endif ()
    endif ()
endif()

ExternalProject_Add(dep_OpenSSL
    #EXCLUDE_FROM_ALL ON
    # 3.5 is the long-term-support series (supported until April 2030).
    URL "https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz"
    URL_HASH SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
    DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/OpenSSL
	CONFIGURE_COMMAND ${_conf_cmd} ${_cross_arch}
        "--openssldir=${_openssl_dir}"
        "--prefix=${DESTDIR}"
        # OpenSSL's linux-x86_64 target sets multilib=64, so it installs to
        # <prefix>/lib64 while every other dep uses <prefix>/lib. CPython's
        # --with-openssl only ever emits -L<dir>/lib, so it would miss the
        # bundled static libs and silently link the system OpenSSL instead.
        # Pin libdir so the prefix stays single-layout.
        "--libdir=lib"
        ${_cross_comp_prefix_line}
        ${_openssl_extra_cflags}
        # 3.x raises the default security level from 1 to 2. Keep 1, the level
        # 1.1.1 shipped with: it still accepts the 1024-bit RSA and DH that old
        # self-signed printer certificates and firmware TLS stacks use. TLS
        # 1.0 and 1.1 need level 0 in 3.x and stay refused at either level.
        -DOPENSSL_TLS_SECURITY_LEVEL=1
        no-shared
        no-asm
        no-dynamic-engine
        no-tests
    BUILD_IN_SOURCE ON
    BUILD_COMMAND ${_make_cmd}
    INSTALL_COMMAND ${_install_cmd}
)

if (CMAKE_GENERATOR MATCHES "Visual Studio")
    # OpenSSL builds with cl, but MSBuild runs nmake in this project's toolset
    # environment, and ClangCL's puts clang's headers first. Use the default.
    set_target_properties(dep_OpenSSL PROPERTIES VS_PLATFORM_TOOLSET "$(DefaultPlatformToolset)")
endif ()

if (NOT WIN32)
    # OpenSSL's object rules do not depend on the configured flags or paths, so
    # reconfiguring it (a new compiler flag, a different --openssldir) relinks
    # the archives from stale objects instead of recompiling them, and the
    # change silently has no effect. Drop the objects whenever this recipe
    # changes so the next build actually recompiles them.
    ExternalProject_Get_Property(dep_OpenSSL SOURCE_DIR)
    ExternalProject_Add_Step(dep_OpenSSL clean_objects
        DEPENDEES configure
        DEPENDERS build
        COMMAND make clean
        WORKING_DIRECTORY "${SOURCE_DIR}"
        DEPENDS "${CMAKE_CURRENT_LIST_FILE}"
        COMMENT "OpenSSL: cleaning objects after a recipe change"
    )
endif ()
