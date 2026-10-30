if(BUILD_SHARED_LIBS)
    set(_build_shared ON)
    set(_build_static OFF)
else()
    set(_build_shared OFF)
    set(_build_static ON)
endif()

if(IS_CROSS_COMPILE AND APPLE)
    orcaslicer_add_cmake_project(Blosc
        URL https://github.com/Blosc/c-blosc/archive/refs/tags/v1.21.6.zip
        URL_HASH SHA256=1919c97d55023c04aa8771ea8235b63e9da3c22e3d2a68340b33710d19c2a2eb
        DEPENDS ${ZLIB_PKG}
        # Patching upstream does not work this way with git version 2.28 installed on mac worker
        # PATCH_COMMAND  ${GIT_EXECUTABLE} apply --ignore-space-change --whitespace=fix ${CMAKE_CURRENT_LIST_DIR}/blosc-mods.patch
        CMAKE_ARGS
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DBUILD_SHARED=${_build_shared} 
            -DBUILD_STATIC=${_build_static}
            -DBUILD_TESTS=OFF 
            -DBUILD_BENCHMARKS=OFF 
            -DPREFER_EXTERNAL_ZLIB=ON
            -DDEACTIVATE_SSE2=ON
            -DDEACTIVATE_AVX2=ON
    )
else()
    orcaslicer_add_cmake_project(Blosc
        URL https://github.com/Blosc/c-blosc/archive/refs/tags/v1.21.6.zip
        URL_HASH SHA256=1919c97d55023c04aa8771ea8235b63e9da3c22e3d2a68340b33710d19c2a2eb
        DEPENDS ${ZLIB_PKG}
        # Patching upstream does not work this way with git version 2.28 installed on mac worker
        # PATCH_COMMAND  ${GIT_EXECUTABLE} apply --ignore-space-change --whitespace=fix ${CMAKE_CURRENT_LIST_DIR}/blosc-mods.patch
        CMAKE_ARGS
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DBUILD_SHARED=${_build_shared} 
            -DBUILD_STATIC=${_build_static}
            -DBUILD_TESTS=OFF 
            -DBUILD_BENCHMARKS=OFF 
            -DPREFER_EXTERNAL_ZLIB=ON
    )
endif()
if (MSVC)
    add_debug_dep(dep_Blosc)
endif ()
