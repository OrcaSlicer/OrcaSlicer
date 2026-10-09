# clang-cl cannot emit some OCCT sources for ARM64 (llvm/llvm-project#62081).
# cl and clang-cl share an ABI.
set(_occt_compiler_args "")
if ("${DEPS_ARCH}" STREQUAL "arm64" AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    set(_occt_compiler_args -DCMAKE_C_COMPILER:STRING=cl -DCMAKE_CXX_COMPILER:STRING=cl)
endif ()

if(WIN32)
    set(library_build_type "Shared")
else()
    set(library_build_type "Static")
endif()

# SLIC3R_CAD (declared in deps/CMakeLists.txt) builds OCCT's ModelingAlgorithms module
# (fillet/offset/loft), whose only consumer is the parametric Design/CAD tab. With it OFF
# the deps prefix matches upstream exactly.
#
# With it ON OCCT also builds TKFillet (used via BRepFilletAPI), TKOffset (used via
# BRepOffsetAPI), and TKFeat, TKHelix, TKXMesh and TKExpress, which nothing here references
# but which the module flag builds anyway, since module flags are all-or-nothing. The
# module's other toolkits are built either way, because DataExchange (the STEP path)
# depends on them.
#
# On macOS/Linux OCCT links statically, so an unreferenced toolkit costs build time and no
# shipped bytes. Windows ships only the DLLs libslic3r links, so the tab adds the TKFillet,
# TKOffset and TKBool DLLs. See docs/HLSD/design-tab.md.

if (IN_GIT_REPO)
    set(OCCT_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_OCCT-prefix/src/dep_OCCT)
endif ()

orcaslicer_add_cmake_project(OCCT
    URL https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V8_0_1.zip
    URL_HASH SHA256=7c033d917ee8f040c0512d289dcc5f02c148889d5bac17c3e25639accb44f0da
    # Makes BRepMesh triangulate cone faces whose seam pcurve is slightly tilted
    # (Open-Cascade-SAS/OCCT#572); remove the patch once an OCCT release includes the fix.
    PATCH_COMMAND git apply ${OCCT_DIRECTORY_FLAG} --verbose --ignore-space-change --whitespace=fix ${CMAKE_CURRENT_LIST_DIR}/0001-BRepMesh-seam-pcurve-at-edge-parameter.patch
    #DEPENDS dep_Boost
    DEPENDS ${FREETYPE_PKG}
    CMAKE_ARGS
        -DCMAKE_CXX_STANDARD=17
        -DBUILD_LIBRARY_TYPE=${library_build_type}
        # With the Unix layout, OCCT's resources and licenses go under share/ and its scripts
        # into bin/occt on Windows too. libslic3r finds the CMake package in lib/cmake/occt.
        -DINSTALL_DIR_LAYOUT=Unix
        -DINSTALL_DIR_BIN=bin/occt
        -DINSTALL_DIR_LIB=lib/occt
        -DINSTALL_DIR_INCLUDE=include/occt
        -DINSTALL_DIR_CMAKE=lib/cmake/occt
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
        ${_occt_compiler_args}
)

# add_dependencies(dep_OCCT ${FREETYPE_PKG})
