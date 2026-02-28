# We have to check for OpenGL to compile GLEW
set(OpenGL_GL_PREFERENCE "LEGACY") # to prevent a nasty warning by cmake
find_package(OpenGL QUIET REQUIRED)

orcaslicer_add_cmake_project(
  GLEW
  URL https://sourceforge.net/projects/glew/files/glew/2.3.1/glew-2.3.1.zip
  URL_HASH SHA256=09E0083AE46930ABA9B53E72C92EE1A557E24ED393526FEC26CB0EBABD834720
  CMAKE_ARGS
    -DBUILD_UTILS=OFF
    -DGLEW_USE_EGL=OFF
)

if (MSVC)
    add_debug_dep(dep_GLEW)
endif ()
