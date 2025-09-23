
orcaslicer_add_cmake_project(OpenCSG
    # GIT_REPOSITORY https://github.com/floriankirsch/OpenCSG.git
    # GIT_TAG 313018fbf997f484f66cb4a320bbd2abf79a4fc1 #v1.9.1
    URL https://github.com/floriankirsch/OpenCSG/archive/refs/tags/opencsg-1-8-1-release.zip
    URL_HASH SHA256=405ead7642b052d8ea0a7425d9f8f55dede093c5d3d4af067e94e43c43f5fa79
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt.in ./CMakeLists.txt
    DEPENDS dep_GLEW
)

if (TARGET ${ZLIB_PKG})
    add_dependencies(dep_OpenCSG ${ZLIB_PKG})
endif()

if (MSVC)
    add_debug_dep(dep_OpenCSG)
endif ()
