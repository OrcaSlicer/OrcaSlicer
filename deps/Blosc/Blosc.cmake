if(BUILD_SHARED_LIBS)
    set(_build_shared ON)
    set(_build_static OFF)
else()
    set(_build_shared OFF)
    set(_build_static ON)
endif()

if(IS_CROSS_COMPILE AND APPLE)
    orcaslicer_add_cmake_project(Blosc
        #URL https://github.com/Blosc/c-blosc/archive/refs/tags/v1.17.0.zip
        #URL_HASH SHA256=7463a1df566704f212263312717ab2c36b45d45cba6cd0dccebf91b2cc4b4da9
        URL https://github.com/tamasmeszaros/c-blosc/archive/refs/heads/v1.17.0_tm.zip
        URL_HASH SHA256=dcb48bf43a672fa3de6a4b1de2c4c238709dad5893d1e097b8374ad84b1fc3b3
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
        #URL https://github.com/Blosc/c-blosc/archive/refs/tags/v1.17.0.zip
        #URL_HASH SHA256=7463a1df566704f212263312717ab2c36b45d45cba6cd0dccebf91b2cc4b4da9
        URL https://github.com/tamasmeszaros/c-blosc/archive/refs/heads/v1.17.0_tm.zip
        URL_HASH SHA256=dcb48bf43a672fa3de6a4b1de2c4c238709dad5893d1e097b8374ad84b1fc3b3
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