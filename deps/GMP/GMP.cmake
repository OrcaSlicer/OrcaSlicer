
set(_srcdir ${CMAKE_CURRENT_LIST_DIR}/gmp)

if (IN_GIT_REPO)
    set(GMP_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_GMP-prefix/src/dep_GMP)
endif ()

if (MSVC)
    set(_output  ${DESTDIR}/include/gmp.h
                 ${DESTDIR}/lib/libgmp-10.lib
                 ${DESTDIR}/bin/libgmp-10.dll)

    add_custom_command(
        OUTPUT  ${_output}
        COMMAND ${CMAKE_COMMAND} -E copy ${_srcdir}/include/gmp.h ${DESTDIR}/include/
        COMMAND ${CMAKE_COMMAND} -E copy ${_srcdir}/lib/win-${DEPS_ARCH}/libgmp-10.lib ${DESTDIR}/lib/
        COMMAND ${CMAKE_COMMAND} -E copy ${_srcdir}/lib/win-${DEPS_ARCH}/libgmp-10.dll ${DESTDIR}/bin/
    )

    add_custom_target(dep_GMP SOURCES ${_output})

else ()
    set(_gmp_ccflags "-O2 -DNDEBUG -fPIC -DPIC -Wall -Wmissing-prototypes -Wpointer-arith -pedantic -fomit-frame-pointer -fno-common")
    set(_gmp_build_tgt "${CMAKE_SYSTEM_PROCESSOR}")

    if (APPLE)
        if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
            set(_gmp_build_arch aarch64)
        else ()
            set(_gmp_build_arch ${CMAKE_SYSTEM_PROCESSOR})
        endif()
        if (IS_CROSS_COMPILE)
            if (${CMAKE_OSX_ARCHITECTURES} MATCHES "arm")
                set(_gmp_host_arch aarch64)
                set(_gmp_host_arch_flags "-arch arm64")
            elseif (${CMAKE_OSX_ARCHITECTURES} MATCHES "x86_64")
                set(_gmp_host_arch x86_64)
                set(_gmp_host_arch_flags "-arch x86_64")
            endif()
            set(_gmp_ccflags "${_gmp_ccflags} ${_gmp_host_arch_flags} -mmacosx-version-min=${DEP_OSX_TARGET}")
            set(_gmp_build_tgt --build=${_gmp_build_arch}-apple-darwin --host=${_gmp_host_arch}-apple-darwin)
        else ()
            set(_gmp_ccflags "${_gmp_ccflags} -mmacosx-version-min=${DEP_OSX_TARGET}")
            set(_gmp_build_tgt "--build=${_gmp_build_arch}-apple-darwin")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
            set(_gmp_ccflags "${_gmp_ccflags} -march=armv7-a") # Works on RPi-4
            set(_gmp_build_tgt armv7)
        endif()
        set(_gmp_build_tgt "--build=${_gmp_build_tgt}-pc-linux-gnu")
    else ()
        set(_gmp_build_tgt "") # let it guess
    endif()

    set(_cross_compile_arg "")
    if (CMAKE_CROSSCOMPILING)
        # TOOLCHAIN_PREFIX should be defined in the toolchain file
        set(_cross_compile_arg --host=${TOOLCHAIN_PREFIX})
    endif ()

    # 6.3.0 is the first release whose arm64 assembly avoids x18, which macOS
    # reserves and zeroes on every return to user space; 6.2.1 crashes on Apple
    # silicon whenever CGAL falls back to exact arithmetic.
    ExternalProject_Add(dep_GMP
        URL https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
            https://github.com/SoftFever/OrcaSlicer_deps/releases/download/gmp-6.3.0/gmp-6.3.0.tar.xz
        URL_HASH SHA256=a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898
        DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/GMP
        PATCH_COMMAND git apply ${GMP_DIRECTORY_FLAG} --verbose ${CMAKE_CURRENT_LIST_DIR}/0001-GMP_GCC15.patch
        BUILD_IN_SOURCE ON
        CONFIGURE_COMMAND  env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}" "CFLAGS=${_gmp_ccflags}" "CXXFLAGS=${_gmp_ccflags}" "LDFLAGS=${CMAKE_EXE_LINKER_FLAGS}" ./configure ${_cross_compile_arg} --enable-shared=no --enable-cxx=yes --enable-static=yes "--prefix=${DESTDIR}" ${_gmp_build_tgt}
        BUILD_COMMAND     make -j
        INSTALL_COMMAND   make install
    )
endif ()
