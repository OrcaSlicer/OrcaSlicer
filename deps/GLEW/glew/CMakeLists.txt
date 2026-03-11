cmake_minimum_required(VERSION 3.0)
project(GLEW)

find_package(OpenGL REQUIRED)

# Allow parent project to control EGL usage.
# Default to OFF since OrcaSlicer forces GDK_BACKEND=x11 (using GLX contexts).
# GLEW must use glXGetProcAddressARB (GLX) to match wxWidgets GL canvas.
# Using EGL function loading with GLX contexts causes rendering failures.
option(GLEW_USE_EGL "Use EGL instead of GLX for OpenGL function loading" OFF)

if(GLEW_USE_EGL)
    message(STATUS "Building GLEW with EGL support")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DGLEW_EGL")
else()
    message(STATUS "Building GLEW with GLX support")
endif()

add_library(GLEW src/glew.c)
target_include_directories(GLEW PRIVATE include/)
target_link_libraries(GLEW PUBLIC OpenGL::GL)

if (NOT BUILD_SHARED_LIBS)
    target_compile_definitions(GLEW PUBLIC GLEW_STATIC)
endif ()

include(GNUInstallDirs)

install( 
    FILES
        ${PROJECT_SOURCE_DIR}/include/GL/glew.h
        ${PROJECT_SOURCE_DIR}/include/GL/wglew.h
        ${PROJECT_SOURCE_DIR}/include/GL/glxew.h
        ${PROJECT_SOURCE_DIR}/include/GL/eglew.h
    DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}/GL
)

install(TARGETS GLEW GLEW
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} 
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)