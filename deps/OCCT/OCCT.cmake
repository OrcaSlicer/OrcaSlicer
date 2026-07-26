if(WIN32)
    set(library_build_type "Shared")
else()
    set(library_build_type "Static")
endif()

# The parametric Design/CAD tab is the only consumer of OCCT's ModelingAlgorithms
# module (fillet/offset/loft). With it OFF the deps prefix matches upstream exactly.
#
# With it ON the delta is THREE toolkits, not two: TKFillet (7.40 MiB archive, used via
# BRepFilletAPI), TKOffset (5.38 MiB, used via BRepOffsetAPI) and TKFeat (4.42 MiB), which
# nothing here references but which the module flag builds anyway -- it is all-or-nothing
# per module. The module's other nine toolkits are built either way, because DataExchange
# (the STEP path upstream already ships) depends on them.
#
# On macOS/Linux OCCT links statically, so an unreferenced toolkit costs build time and no
# shipped bytes. The Windows figure is a real DLL cost and has NOT been measured -- an
# earlier "3.77 MiB, Windows only" note here covered only two of the three toolkits and is
# not a number to quote. See docs/cad_dependency_weight.md.
#
# Must match the SLIC3R_CAD passed to the main project.
option(SLIC3R_CAD "Build OCCT's ModelingAlgorithms module (required by the Design/CAD tab)" ON)

if (IN_GIT_REPO)
    set(OCCT_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_OCCT-prefix/src/dep_OCCT)
endif ()

orcaslicer_add_cmake_project(OCCT
    URL https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V7_6_0.zip
    URL_HASH SHA256=28334f0e98f1b1629799783e9b4d21e05349d89e695809d7e6dfa45ea43e1dbc
    #PATCH_COMMAND ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-OCCT-fix.patch
    PATCH_COMMAND git apply ${OCCT_DIRECTORY_FLAG} --verbose --ignore-space-change --whitespace=fix ${CMAKE_CURRENT_LIST_DIR}/0001-OCCT-fix.patch
    #DEPENDS dep_Boost
    DEPENDS ${FREETYPE_PKG}
    CMAKE_ARGS
        -DCMAKE_CXX_STANDARD=17
        -DBUILD_LIBRARY_TYPE=${library_build_type}
        -DUSE_TK=OFF
        -DUSE_TBB=OFF
	#-DUSE_FREETYPE=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_VTK=OFF
        -DBUILD_DOC_Overview=OFF
        -DBUILD_MODULE_ApplicationFramework=OFF
        #-DBUILD_MODULE_DataExchange=OFF
        -DBUILD_MODULE_Draw=OFF
        -DBUILD_MODULE_FoundationClasses=OFF
        -DBUILD_MODULE_ModelingAlgorithms=${SLIC3R_CAD}
        -DBUILD_MODULE_ModelingData=OFF
        -DBUILD_MODULE_Visualization=OFF
)

# add_dependencies(dep_OCCT ${FREETYPE_PKG})
