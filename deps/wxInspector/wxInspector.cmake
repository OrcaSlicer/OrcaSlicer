orcaslicer_add_cmake_project(
    wxInspector
    GIT_REPOSITORY https://github.com/Noisyfox/wxInspector.git
    GIT_TAG        4e4ad7dcbaa77e1680e8a22eb66cbeac58013a4a 
    GIT_SHALLOW ON
    DEPENDS dep_wxWidgets
    CMAKE_ARGS
        -DCMAKE_CXX_FLAGS="-DwxDEBUG_LEVEL=0"
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

if (MSVC)
    add_debug_dep(dep_wxInspector)
endif ()
