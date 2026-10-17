if (EMSCRIPTEN)
    set(_extra_cmake_args --use-port=zlib)
else ()
    set(DEP_Blosc_DEPENDS ZLIB zstd)
endif ()

# Common CMake arguments for Blosc
set(_blosc_cmake_args
        -DCMAKE_C_FLAGS="-std=gnu17"
        -DCMAKE_CXX_FLAGS="-std=gnu++17"
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_SHARED=${_build_shared}
        -DBUILD_STATIC=${_build_static}
        -DBUILD_TESTS=OFF
        -DBUILD_BENCHMARKS=OFF
        -DPREFER_EXTERNAL_ZLIB=ON
        -DPREFER_EXTERNAL_ZSTD=ON
        -DCMAKE_POLICY_VERSION_MINIMUM=3.10
)

# Platform-specific arguments
if(APPLE)
    # SSE2 support is dropped in Clang of newer Xcode (16.3+) versions
    list(APPEND _blosc_cmake_args -DDEACTIVATE_SSE2=ON)
endif()

orcaslicer_add_cmake_project(Blosc
        URL https://github.com/Blosc/c-blosc/archive/refs/tags/v1.21.6.zip
        URL_HASH SHA256=1919c97d55023c04aa8771ea8235b63e9da3c22e3d2a68340b33710d19c2a2eb
        EMSCRIPTEN_EXCLUDED OFF
        CMAKE_ARGS
        ${_blosc_cmake_args}
)
