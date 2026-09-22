# wxInspector finds wxWidgets through CMake's FindwxWidgets module, which only
# searches lib/vc*_lib because _WX_TOOL is hardcoded to "vc". A superbuild driven
# by clang-cl installs wxWidgets into lib/clang_x64_lib, so hand the module the
# directory wxWidgets actually used, derived the same way wxWidgetsConfig.cmake
# derives it.
set(_wxinspector_wx_hints "")
if (MSVC)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(_wx_compiler_prefix "clang")
    else ()
        set(_wx_compiler_prefix "vc")
    endif ()
    set(_wx_arch_suffix "")
    if (CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
        string(TOLOWER "_${CMAKE_GENERATOR_PLATFORM}" _wx_arch_suffix)
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_wx_arch_suffix "_x64")
    endif ()
    set(_wxinspector_wx_hints
        "-DwxWidgets_ROOT_DIR=${DESTDIR}"
        "-DwxWidgets_LIB_DIR=${DESTDIR}/lib/${_wx_compiler_prefix}${_wx_arch_suffix}_lib")
endif ()

orcaslicer_add_cmake_project(
    wxInspector
    URL https://github.com/Noisyfox/wxInspector/archive/refs/tags/v1.0.0.zip
    URL_HASH SHA256=0ba163956f2d468b19a91b96c5aba66ee9610843ea41dda628ea44cdafde7db7
    DEPENDS ${WXWIDGETS_PKG}
    CMAKE_ARGS
        -DCMAKE_CXX_FLAGS="-DwxDEBUG_LEVEL=0"
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        ${_wxinspector_wx_hints}
)

if (MSVC)
    add_debug_dep(dep_wxInspector)
endif ()
