orcaslicer_add_cmake_project(PNG
        URL https://github.com/glennrp/libpng/archive/refs/tags/v1.6.59.zip
        URL_HASH SHA256=6fd97a1e5e2ac55cec0a538a9800a351eb3844086736291844ebd12db72ba433
        DEPENDS ${ZLIB_PKG}
        CMAKE_ARGS
            # libpng defaults this ON on Apple and installs libpng, its headers and
            # a pnglibconf.h a second time into lib/png.framework. Keep the flat
            # install the only one, so every consumer sees the same prefixed
            # pnglibconf.h the archive was built with.
            -DPNG_FRAMEWORK=OFF
            # The libpng build defaults to using a prebuilt pnglibconf.h that has
            # no PNG_PREFIX applied. Force generation from the .dfa so the prefix
            # mapping is baked into the installed pnglibconf.h.
            -DPNG_LIBCONF_HEADER_PREBUILT=OFF
            -DPNG_FRAMEWORK=OFF
            -DPNG_SHARED=OFF
            -DPNG_STATIC=ON
            # PNG_PREFIX is removed: the libpng prefix mechanism is broken in
            # 1.6.59 (prefixed symbols in lib, unprefixed refs from header).
            # Building without a prefix makes libpng export standard png_* names
            # which match the unprefixed calls in PNGReadWrite.cpp.
            # -DPNG_PREFIX=prusaslicer_
            -DPNG_TESTS=OFF
            -DDISABLE_DEPENDENCY_TRACKING=OFF
)

if (MSVC)
    add_debug_dep(dep_PNG)
endif ()
