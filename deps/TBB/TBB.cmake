if (FLATPAK AND "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(_patch_command ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/GNU.cmake ./cmake/compilers/GNU.cmake)
else()
    set(_patch_command "")
endif()

orcaslicer_add_cmake_project(
    TBB
    URL "https://github.com/uxlfoundation/oneTBB/archive/refs/tags/v2022.3.0.zip"
    URL_HASH SHA256=4f47379064f99cc50da8dde85e27651d3609ac6c3e0941b1c728a1b2dd1e4b68
    PATCH_COMMAND ${_patch_command}
    CMAKE_ARGS          
        -DTBB_BUILD_SHARED=OFF
        -DTBB_BUILD_TESTS=OFF
        -DTBB_TEST=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_DEBUG_POSTFIX=_debug
)

if (MSVC)
    add_debug_dep(dep_TBB)
endif ()


