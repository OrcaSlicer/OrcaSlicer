# Pinned release: expat's entity-amplification guard is a security boundary, so
# the parser must not come from a distro or SDK copy that may predate it.
# XML_DTD and XML_GE are passed explicitly: 2.4.0 gated the guard behind XML_DTD
# and 2.6.0 behind XML_GE, so neither may be left to the default.
orcaslicer_add_cmake_project(EXPAT
    URL https://github.com/libexpat/libexpat/releases/download/R_2_10_0/expat-2.10.0.tar.gz
    URL_HASH SHA256=5466a341f9c692ef7a958a6881311be597f8e1ce345984cf78414f54793dea4c
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
)

if (MSVC)
    add_debug_dep(dep_EXPAT)
    # A deps prefix built before this pin still has lib/expat.lib from the
    # bundled 2.5.0, and FindEXPAT matches that name before libexpatMD.lib, so
    # overwrite it with the pinned build. This also covers a cached EXPAT_LIBRARY.
    set(_expat_legacy_libs
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${DESTDIR}/lib/libexpatMD.lib ${DESTDIR}/lib/expat.lib
    )
    if (DEP_DEBUG)
        list(APPEND _expat_legacy_libs
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DESTDIR}/lib/libexpatdMD.lib ${DESTDIR}/lib/expatd.lib
        )
    endif ()
    ExternalProject_Add_Step(dep_EXPAT copy_legacy_lib
        DEPENDEES install
        ${_expat_legacy_libs}
    )
endif ()
