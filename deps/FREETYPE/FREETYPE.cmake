if(WIN32)
    set(library_build_shared "1")
else()
    set(library_build_shared "0")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ft_disable_zlib "-D FT_DISABLE_ZLIB=FALSE")
else()
    set(_ft_disable_zlib "-D FT_DISABLE_ZLIB=TRUE")
endif()

orcaslicer_add_cmake_project(FREETYPE
    URL https://github.com/SoftFever/orca_deps/releases/download/freetype-2.12.1.tar.gz/freetype-2.12.1.tar.gz
    URL_HASH SHA256=efe71fd4b8246f1b0b1b9bfca13cfff1c9ad85930340c27df469733bbb620938
    #DEPENDS ${ZLIB_PKG}
    #"${_patch_step}"
    CMAKE_ARGS
	-D BUILD_SHARED_LIBS=${library_build_shared}
	${_ft_disable_zlib}
        -D FT_DISABLE_BZIP2=TRUE
        -D FT_DISABLE_PNG=TRUE
        -D FT_DISABLE_HARFBUZZ=TRUE
        -D FT_DISABLE_BROTLI=TRUE
)

if(MSVC)
    add_debug_dep(dep_FREETYPE)
endif()
