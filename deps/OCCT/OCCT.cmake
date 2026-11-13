# clang-cl cannot emit some OCCT sources for ARM64 (llvm/llvm-project#62081), and with OCCT's
# precompiled headers it can break exception copies (llvm/llvm-project#53486). cl and clang-cl
# share an ABI, so clang-cl builds compile OCCT with cl. The Visual Studio generator picks the
# compiler by toolset, so there OCCT may stay on clang-cl; keep precompiled headers off there.
set(_occt_compiler_args "")
set(_occt_pch ON)
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if ("${DEPS_ARCH}" STREQUAL "arm64" OR NOT CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_occt_compiler_args -DCMAKE_C_COMPILER:STRING=cl -DCMAKE_CXX_COMPILER:STRING=cl)
    endif ()
    if (CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_occt_pch OFF)
    endif ()
endif ()

if(WIN32)
    set(library_build_type "Shared")
else()
    set(library_build_type "Static")
endif()

# Only the toolkits libslic3r links are built, rather than whole modules. OCCT's module
# flags are all-or-nothing: the DataExchange module alone also builds the IGES, VRML, OBJ,
# STL and PLY readers and the XML/binary document formats, none of which Orca uses. With
# every module OFF, OCCT builds BUILD_ADDITIONAL_TOOLKITS plus their dependencies.
#
# This list must match OCCT_LIBS in src/libslic3r/CMakeLists.txt. A toolkit missing here
# fails the app configure or link; one left here after libslic3r stops using it only costs
# build time.
#
# SLIC3R_CAD (declared in deps/CMakeLists.txt) adds TKFillet (BRepFilletAPI), TKOffset
# (BRepOffsetAPI) and TKBool, which they need. Their only consumer is the parametric
# Design/CAD tab. On Windows OCCT builds shared and only the linked toolkits ship, so the
# tab adds those three DLLs. See docs/HLSD/design-tab.md.
set(_occt_toolkits
    TKDESTEP TKDE TKXCAF TKXSBase TKVCAF TKCAF TKLCAF TKCDF TKV3d TKService TKMesh TKBO
    TKPrim TKHLR TKShHealing TKTopAlgo TKGeomAlgo TKBRep TKGeomBase TKG3d TKG2d TKMath TKernel)
if (SLIC3R_CAD)
    list(APPEND _occt_toolkits TKFillet TKOffset TKBool)
endif ()
# OCCT reads BUILD_ADDITIONAL_TOOLKITS as a space-separated string.
string(REPLACE ";" " " _occt_toolkits "${_occt_toolkits}")

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
        -DUSE_FREETYPE=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_VTK=OFF
        -DBUILD_DOC_Overview=OFF
        -DBUILD_USE_PCH=${_occt_pch}
        -DBUILD_MODULE_ApplicationFramework=OFF
        -DBUILD_MODULE_DataExchange=OFF
        -DBUILD_MODULE_Draw=OFF
        -DBUILD_MODULE_FoundationClasses=OFF
        -DBUILD_MODULE_ModelingAlgorithms=OFF
        -DBUILD_MODULE_ModelingData=OFF
        -DBUILD_MODULE_Visualization=OFF
        "-DBUILD_ADDITIONAL_TOOLKITS=${_occt_toolkits}"
        ${_occt_compiler_args}
)

# The precompiled headers (~2.2 GB with cl) are only needed while OCCT compiles; none are installed.
if (_occt_pch)
    ExternalProject_Add_Step(dep_OCCT remove_pch
        DEPENDEES install
        COMMENT "Removing OCCT's precompiled headers"
        COMMAND ${CMAKE_COMMAND} -DDIR=<BINARY_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/remove_pch.cmake)
endif ()
