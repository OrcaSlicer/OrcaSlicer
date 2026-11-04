
# Assimp's bundled zlib (contrib/zlib) is too old to compile against the modern
# macOS SDK: its zutil.h takes the classic-Mac branch under TARGET_OS_MAC and
# does `#define fdopen(fd,mode) NULL`, which then clobbers the SDK's real
# `fdopen` prototype in <stdio.h> and breaks the build. On macOS use the system
# zlib (already found by find_package(ZLIB) in deps-unix-common) instead.
if(APPLE)
    set(_assimp_build_zlib "-DASSIMP_BUILD_ZLIB=OFF")
else()
    set(_assimp_build_zlib "-DASSIMP_BUILD_ZLIB=ON")
endif()

orcaslicer_add_cmake_project(Assimp
    URL https://github.com/assimp/assimp/archive/refs/tags/v6.0.5.zip
    URL_HASH 89e477e89df739ecc7dd21874897ac2e70c8944eb1074fbdbbdbf4dd2de6a403
    CMAKE_ARGS
        # Assimp's ccache support sets the global RULE_LAUNCH_COMPILE, which breaks
        # the Ninja RC rule. The superbuild forwards CMAKE_<LANG>_COMPILER_LAUNCHER.
        -DASSIMP_BUILD_USE_CCACHE=OFF
        -DASSIMP_BUILD_TESTS=OFF
        -DASSIMP_BUILD_SAMPLES=OFF
        -DASSIMP_BUILD_ASSIMP_TOOLS=OFF
        -DASSIMP_INSTALL_PDB=OFF
        -DASSIMP_NO_EXPORT=ON
        -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF
        -DASSIMP_BUILD_GLTF_IMPORTER=ON
        -DASSIMP_BUILD_OBJ_IMPORTER=ON
        -DASSIMP_BUILD_FBX_IMPORTER=ON
        ${_assimp_build_zlib}
        -DASSIMP_WARNINGS_AS_ERRORS=OFF
        -DBUILD_WITH_STATIC_CRT=OFF
)

if (MSVC)
    add_debug_dep(dep_Assimp)
endif ()
