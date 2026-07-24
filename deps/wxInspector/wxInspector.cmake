orcaslicer_add_cmake_project(
    wxInspector
    GIT_REPOSITORY https://github.com/Noisyfox/wxInspector.git
    GIT_TAG        a27c643e8d5d0cf1c8181520c9bb2fd2690a920e 
    GIT_SHALLOW ON
    DEPENDS dep_wxWidgets
    CMAKE_ARGS
        -DCMAKE_CXX_FLAGS="-DwxDEBUG_LEVEL=0"
)

if (MSVC)
    add_debug_dep(dep_wxInspector)
endif ()
