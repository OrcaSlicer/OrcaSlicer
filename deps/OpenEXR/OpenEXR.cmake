# Check if we're building for arm on x86_64 and just for OpenEXR, build fat
# binaries.  We need this because it compiles some code to generate other
# source and we need to be able to run the executables.  When we link the
# library, the x86_64 part will be ignored.
if (APPLE AND IS_CROSS_COMPILE)
    if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64" AND ${CMAKE_OSX_ARCHITECTURES} MATCHES "arm")
        set(_openexr_arch arm64^^x86_64)
        set(_openxr_list_sep LIST_SEPARATOR ^^)
        set(_cmake_openexr_arch -DCMAKE_OSX_ARCHITECTURES:STRING=${_openexr_arch})
    else()
        set(_openexr_arch ${CMAKE_OSX_ARCHITECTURES})
        set(_cmake_openexr_arch -DCMAKE_OSX_ARCHITECTURES:STRING=${_openexr_arch})
    endif()
    ExternalProject_Add(dep_OpenEXR
        EXCLUDE_FROM_ALL    ON
        URL https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v2.5.5.zip
        URL_HASH SHA256=0307a3d7e1fa1e77e9d84d7e9a8694583fbbbfd50bdc6884e2c96b8ef6b902de
        INSTALL_DIR         ${DESTDIR}
        DOWNLOAD_DIR        ${DEP_DOWNLOAD_DIR}/OpenEXR
        ${_openxr_list_sep}
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX:STRING=${DESTDIR}
            -DBUILD_SHARED_LIBS:BOOL=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DBUILD_TESTING=OFF 
            -DPYILMBASE_ENABLE:BOOL=OFF 
            -DOPENEXR_VIEWERS_ENABLE:BOOL=OFF
            -DOPENEXR_BUILD_UTILS:BOOL=OFF
            ${_cmake_openexr_arch}
    )
else()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_patch_cmd ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-OpenEXR-GCC13.patch)
else ()
    set(_patch_cmd "")
endif ()

orcaslicer_add_cmake_project(OpenEXR
    # GIT_REPOSITORY https://github.com/openexr/openexr.git
    URL https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v2.5.5.zip
    URL_HASH SHA256=0307a3d7e1fa1e77e9d84d7e9a8694583fbbbfd50bdc6884e2c96b8ef6b902de
    PATCH_COMMAND ${_patch_cmd}
    DEPENDS ${ZLIB_PKG}
    GIT_TAG v2.5.5
    CMAKE_ARGS
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_TESTING=OFF
        -DPYILMBASE_ENABLE:BOOL=OFF
        -DOPENEXR_VIEWERS_ENABLE:BOOL=OFF
        -DOPENEXR_BUILD_UTILS:BOOL=OFF
)
endif()

if (MSVC)
    add_debug_dep(dep_OpenEXR)
endif ()
