if(WIN32)
    set(library_build_type "Shared")
else()
    set(library_build_type "Static")
endif()

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
        -DBUILD_MODULE_ModelingAlgorithms=OFF
        -DBUILD_MODULE_ModelingData=OFF
        -DBUILD_MODULE_Visualization=OFF
)

# add_dependencies(dep_OCCT ${FREETYPE_PKG})
