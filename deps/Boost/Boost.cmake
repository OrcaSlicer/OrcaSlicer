
set(_context_abi_line "")
set(_context_arch_line "")
if (APPLE AND CMAKE_OSX_ARCHITECTURES)
    if (CMAKE_OSX_ARCHITECTURES MATCHES "x86")
        set(_context_abi_line "-DBOOST_CONTEXT_ABI:STRING=sysv")
    elseif (CMAKE_OSX_ARCHITECTURES MATCHES "arm")
        set (_context_abi_line "-DBOOST_CONTEXT_ABI:STRING=aapcs")
    endif ()
    set(_context_arch_line "-DBOOST_CONTEXT_ARCHITECTURE:STRING=${CMAKE_OSX_ARCHITECTURES}")
endif ()

# Windows ARM64: Boost.Context's default fcontext implementation assembles .asm
# via armasm64, which trips a CMake ASM_ARMASM linker-module bug under the VS
# generator. The winfib implementation (Windows Fiber API) avoids assembly while
# keeping the Boost::context target that Boost.Asio's stackful coroutines need.
set(_context_impl_line "")
if (MSVC AND "${DEPS_ARCH}" STREQUAL "arm64")
    set(_context_impl_line "-DBOOST_CONTEXT_IMPLEMENTATION:STRING=winfib")
endif ()

set(_options "")
if (MSVC AND DEP_DEBUG)
    set(_options "FORWARD_CONFIG")
endif ()

orcaslicer_add_cmake_project(Boost
    ${_options}
    URL "https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.gz"
    URL_HASH SHA256=4d27e9efed0f6f152dc28db6430b9d3dfb40c0345da7342eaa5a987dde57bd95
    LIST_SEPARATOR |
    CMAKE_ARGS
        -DBOOST_EXCLUDE_LIBRARIES:STRING=contract|fiber|numpy|stacktrace|wave|test
        -DBOOST_LOCALE_ENABLE_ICU:BOOL=OFF # do not link to libicu, breaks compatibility between distros
        -DBUILD_TESTING:BOOL=OFF
        -DBOOST_IOSTREAMS_ENABLE_BZIP2:BOOL=OFF # avoid libbz2 soname differences in AppImage builds
        -DBOOST_IOSTREAMS_ENABLE_ZSTD:BOOL=OFF
        "${_context_abi_line}"
        "${_context_arch_line}"
        "${_context_impl_line}"
)

set(DEP_Boost_DEPENDS ZLIB)