include(GNUInstallDirs)
orcaslicer_add_cmake_project(Qhull
    URL "https://github.com/qhull/qhull/archive/v8.0.2.zip"
    URL_HASH SHA256=a378e9a39e718e289102c20d45632f873bfdc58a7a5f924246ea4b176e185f1e
    CMAKE_ARGS 
        -DINCLUDE_INSTALL_DIR=${CMAKE_INSTALL_INCLUDEDIR}
)

if (MSVC)
    add_debug_dep(dep_Qhull)
endif ()
