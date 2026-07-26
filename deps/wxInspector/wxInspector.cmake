orcaslicer_add_cmake_project(
    wxInspector
    URL https://github.com/Noisyfox/wxInspector/archive/refs/tags/v1.0.0.zip
    URL_HASH SHA256=0ba163956f2d468b19a91b96c5aba66ee9610843ea41dda628ea44cdafde7db7
    DEPENDS ${WXWIDGETS_PKG}
    CMAKE_ARGS
        -DCMAKE_CXX_FLAGS="-DwxDEBUG_LEVEL=0"
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

if (MSVC)
    add_debug_dep(dep_wxInspector)
endif ()
