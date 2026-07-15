# We have to check for OpenGL to compile GLEW
set(OpenGL_GL_PREFERENCE "LEGACY") # to prevent a nasty warning by cmake
find_package(OpenGL QUIET REQUIRED)

orcaslicer_add_cmake_project(
  GLEW
  SOURCE_DIR  ${CMAKE_CURRENT_LIST_DIR}/glew
  CMAKE_ARGS
    -DGLEW_USE_EGL=OFF
)

if (MSVC)
    add_debug_dep(dep_GLEW)
endif ()
