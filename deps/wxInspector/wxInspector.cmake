orcaslicer_add_cmake_project(
    wxInspector
    GIT_REPOSITORY https://github.com/Noisyfox/wxInspector.git
    GIT_TAG        8569ad6401674d1375034321a1e7a93be242ddf7 
    GIT_SHALLOW ON
    DEPENDS dep_wxWidgets
    CMAKE_ARGS
        -DCMAKE_CXX_FLAGS="-DwxDEBUG_LEVEL=0"
)

if (MSVC)
    add_debug_dep(dep_wxInspector)
endif ()
