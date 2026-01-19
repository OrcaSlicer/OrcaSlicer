cmake_minimum_required(VERSION 3.0)
project(GLEW)

find_package(OpenGL REQUIRED)

if(OpenGL_EGL_FOUND)
    message(STATUS "building GLEW for EGL (hope that wxWidgets agrees, otherwise you won't have any output!)")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DGLEW_EGL")
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