orcaslicer_add_cmake_project(PNG
        URL https://github.com/glennrp/libpng/archive/refs/tags/v1.6.59.zip
        URL_HASH SHA256=6fd97a1e5e2ac55cec0a538a9800a351eb3844086736291844ebd12db72ba433
        DEPENDS ${ZLIB_PKG}
        CMAKE_ARGS
            -DPNG_FRAMEWORK=OFF          # Without it on macOS: framework installs unprefixed header -> linker fails: undefined png_create_read_struct, png_read_info, png_read_image...
            -DPNG_LIBCONF_HEADER_PREBUILT=OFF  # Without it on all platforms: prebuilt header lacks PNG_PREFIX macros -> linker fails: undefined png_create_read_struct, png_read_info, png_read_image...
            -DPNG_STATIC=ON
            -DPNG_SHARED=OFF
            -DPNG_TESTS=OFF
            -DDISABLE_DEPENDENCY_TRACKING=OFF
)

if (MSVC)
    add_debug_dep(dep_PNG)
endif ()
