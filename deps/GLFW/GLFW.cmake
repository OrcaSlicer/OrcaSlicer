if(BUILD_SHARED_LIBS)
    set(_build_shared ON)
    set(_build_static OFF)
else()
    set(_build_shared OFF)
    set(_build_static ON)
endif()

set(_glfw_platform_args "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_glfw_platform_args -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON)
endif()

orcaslicer_add_cmake_project(GLFW
    URL https://github.com/glfw/glfw/archive/refs/tags/3.5.1.zip
    URL_HASH SHA256=e9a80355e8a0c59b15ae8576c2c3aeae792c2b1082ec426dc93bde70d5017fda
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=${_build_shared}
        -DGLFW_BUILD_DOCS=OFF
        -DGLFW_BUILD_EXAMPLES=OFF
        -DGLFW_BUILD_TESTS=OFF
        ${_glfw_platform_args}
)

if (MSVC)
    add_debug_dep(dep_GLFW)
endif ()
