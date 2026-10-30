orcaslicer_add_cmake_project(
    TBB
    URL "https://github.com/uxlfoundation/oneTBB/archive/refs/tags/v2021.13.0.zip"
    URL_HASH SHA256=f8dba2602f61804938d40c24d8f9b1f1cc093cd003b24901d5c3cc75f3dbb952
    CMAKE_ARGS          
        -DTBB_BUILD_SHARED=OFF
        -DTBB_BUILD_TESTS=OFF
        -DTBB_TEST=OFF
        -DTBB_DISABLE_HWLOC_AUTOMATIC_SEARCH=ON
        -DTBB_ENABLE_IPO=OFF
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_DEBUG_POSTFIX=_debug
)

if (MSVC)
    add_debug_dep(dep_TBB)
endif ()


