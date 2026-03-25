set(_libgit2_platform_flags "")

if (WIN32)
    set(_libgit2_platform_flags
        -DUSE_HTTPS=Schannel
    )
elseif (APPLE)
    set(_libgit2_platform_flags
        -DUSE_HTTPS=SecureTransport
    )
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_libgit2_platform_flags
        -DUSE_HTTPS=OpenSSL
    )
endif ()

orcaslicer_add_cmake_project(LIBGIT2
    URL "https://github.com/libgit2/libgit2/archive/refs/tags/v1.9.2.tar.gz"
    URL_HASH SHA256=6f097c82fc06ece4f40539fb17e9d41baf1a5a2fc26b1b8562d21b89bc355fe6
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTS=OFF
        -DBUILD_CLI=OFF
        -DUSE_SSH=OFF
        -DUSE_BUNDLED_ZLIB=ON
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_EXAMPLES=OFF
        ${_libgit2_platform_flags}
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (NOT OPENSSL_FOUND)
        add_dependencies(dep_LIBGIT2 ${OPENSSL_PKG})
    endif ()
endif ()

if (MSVC)
    add_debug_dep(dep_LIBGIT2)
endif ()
