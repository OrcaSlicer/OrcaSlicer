set(_wx_toolkit "")
set(_wx_debug_postfix "")
set(_wx_shared -DwxBUILD_SHARED=OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_gtk_ver 2)

    if (DEP_WX_GTK3)
        set(_gtk_ver 3)
    endif ()

    set(_wx_toolkit "-DwxBUILD_TOOLKIT=gtk${_gtk_ver}")
    if (FLATPAK)
        set(_wx_debug_postfix "d")
        set(_wx_shared -DwxBUILD_SHARED=ON -DBUILD_SHARED_LIBS:BOOL=ON)
    endif ()
endif()

if (MSVC)
    set(_wx_edge "-DwxUSE_WEBVIEW_EDGE=ON")
else ()
    set(_wx_edge "-DwxUSE_WEBVIEW_EDGE=OFF")
endif ()

orcaslicer_add_cmake_project(
    wxWidgets
    GIT_REPOSITORY "https://github.com/SoftFever/Orca-deps-wxWidgets"
    GIT_TAG v3.3.2
    GIT_SHALLOW ON
    DEPENDS ${PNG_PKG} ${ZLIB_PKG} ${EXPAT_PKG} ${JPEG_PKG}
    CMAKE_ARGS
        -DwxBUILD_PRECOMP=ON
        ${_wx_toolkit}
        "-DCMAKE_DEBUG_POSTFIX:STRING=${_wx_debug_postfix}"
        -DwxBUILD_DEBUG_LEVEL=0
        -DwxBUILD_SAMPLES=OFF
        ${_wx_shared}
        -DwxUSE_MEDIACTRL=ON
        -DwxUSE_DETECT_SM=OFF
        -DwxUSE_PRIVATE_FONTS=ON
        -DwxUSE_OPENGL=ON
        -DwxUSE_GLCANVAS_EGL=ON
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
        -DwxUSE_LIBWEBP=builtin
        -DwxUSE_EXPAT=sys
        -DwxUSE_NANOSVG=OFF
)

# wxWidgets 3.3 cmake install doesn't include private headers.
# OrcaSlicer uses some of the private headers (for accessibility support).
# Copy the private headers directory after install.
if(MSVC)
    set(_wx_inc_dest ${DESTDIR}/include/wx)
else()
    set(_wx_inc_dest ${DESTDIR}/include/wx-3.3/wx)
endif()
ExternalProject_Add_Step(dep_wxWidgets copy_private_headers
    DEPENDEES install
    COMMENT "Copying wxWidgets private headers"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        <SOURCE_DIR>/include/wx/private
        ${_wx_inc_dest}/private
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        <SOURCE_DIR>/include/wx/generic/private
        ${_wx_inc_dest}/generic/private
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        <SOURCE_DIR>/include/wx/gtk/private
        ${_wx_inc_dest}/gtk/private
)

if (MSVC)
    add_debug_dep(dep_wxWidgets)
endif ()
