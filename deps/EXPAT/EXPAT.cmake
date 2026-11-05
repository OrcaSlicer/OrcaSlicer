if (CMAKE_VERSION VERSION_LESS 3.17.0)
    message(FATAL_ERROR "expat 2.10.0 requires CMake 3.17 or newer")
endif ()

# Pinned revision: expat's entity-amplification guard is a security boundary, so
# the parser must not come from a distro or SDK copy that may predate it.
# The repository root has no CMakeLists.txt, hence SOURCE_SUBDIR expat.
# XML_DTD and XML_GE are passed explicitly: 2.4.0 gated the guard behind XML_DTD
# and 2.6.0 behind XML_GE, so neither may be left to the default.
#
# The flatpak sandbox has no network access during the build, so there the
# manifest pre-clones the repo and we build from that checkout.
if (FLATPAK)
    set(_expat_source
        SOURCE_DIR ${DEP_DOWNLOAD_DIR}/EXPAT
        SOURCE_SUBDIR expat
    )
else()
    set(_expat_source
        GIT_REPOSITORY https://github.com/libexpat/libexpat.git
        GIT_TAG a70841d9ffa64ddde29ab824ed6305035ebe4c47 # R_2_10_0
        SOURCE_SUBDIR expat
    )
endif()

orcaslicer_add_cmake_project(EXPAT
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=OFF
        -DEXPAT_SHARED_LIBS=OFF
        -DEXPAT_DTD=ON
        -DEXPAT_GE=ON
        -DEXPAT_BUILD_TOOLS=OFF
        -DEXPAT_BUILD_EXAMPLES=OFF
        -DEXPAT_BUILD_TESTS=OFF
        -DEXPAT_BUILD_DOCS=OFF
        -DEXPAT_BUILD_FUZZERS=OFF
        -DEXPAT_BUILD_PKGCONFIG=OFF
        -DEXPAT_ENABLE_INSTALL=ON
    ${_expat_source}
)

if (MSVC)
    add_debug_dep(dep_EXPAT)
endif ()
