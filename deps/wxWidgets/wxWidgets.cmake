set(_wx_toolkit "")
set(_wx_debug_postfix "")
set(_wx_shared -DwxBUILD_SHARED=OFF)
set(_wx_flatpak_patch "")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_gtk_ver 2)

    if (DEP_WX_GTK3)
        set(_gtk_ver 3)
    endif ()

    set(_wx_toolkit "-DwxBUILD_TOOLKIT=gtk${_gtk_ver}")
    if (FLATPAK)
        set(_wx_debug_postfix "d")
        set(_wx_shared -DwxBUILD_SHARED=ON -DBUILD_SHARED_LIBS:BOOL=ON)
        set(_wx_flatpak_patch PATCH_COMMAND ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-flatpak.patch)
    endif ()
endif()

if (MSVC)
    set(_wx_edge "-DwxUSE_WEBVIEW_EDGE=ON")
else ()
    set(_wx_edge "-DwxUSE_WEBVIEW_EDGE=OFF")
endif ()

set(_wx_opengl_override "")
if(APPLE AND CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(_wx_opengl_override
        -DOPENGL_gl_LIBRARY="-framework OpenGL"
        -DOPENGL_glu_LIBRARY="-framework OpenGL"
    )
endif()

orcaslicer_add_cmake_project(
    wxWidgets
    GIT_REPOSITORY "https://github.com/SoftFever/Orca-deps-wxWidgets"
    GIT_SHALLOW ON
    DEPENDS ${PNG_PKG} ${ZLIB_PKG} ${EXPAT_PKG} ${JPEG_PKG}
    ${_wx_flatpak_patch}
    CMAKE_ARGS
        ${_wx_opengl_override}
        -DwxBUILD_PRECOMP=ON
        ${_wx_toolkit}
        "-DCMAKE_DEBUG_POSTFIX:STRING=${_wx_debug_postfix}"
        -DwxBUILD_DEBUG_LEVEL=0
        -DwxBUILD_SAMPLES=OFF
        ${_wx_shared}
        -DwxUSE_MEDIACTRL=ON
        -DwxUSE_DETECT_SM=OFF
        -DwxUSE_UNICODE=ON
        -DwxUSE_PRIVATE_FONTS=ON
        -DwxUSE_OPENGL=ON
        -DwxUSE_WEBREQUEST=ON
        -DwxUSE_WEBVIEW=ON
        ${_wx_edge}
        -DwxUSE_WEBVIEW_IE=OFF
        -DwxUSE_REGEX=builtin
        -DwxUSE_LIBSDL=OFF
        -DwxUSE_XTEST=OFF
        -DwxUSE_STC=OFF
        -DwxUSE_AUI=ON
        -DwxUSE_LIBPNG=sys
        -DwxUSE_ZLIB=sys
        -DwxUSE_LIBJPEG=sys
        -DwxUSE_LIBTIFF=OFF
        -DwxUSE_EXPAT=sys
)

if (MSVC)
    add_debug_dep(dep_wxWidgets)
endif ()
