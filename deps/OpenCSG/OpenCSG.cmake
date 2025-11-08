
orcaslicer_add_cmake_project(OpenCSG
    # GIT_REPOSITORY https://github.com/floriankirsch/OpenCSG.git
    # GIT_TAG 7b24d76556e885195c80952494454baf1544067c #v1.5.1
    URL https://github.com/floriankirsch/OpenCSG/archive/refs/tags/opencsg-1-5-1-release.zip
    URL_HASH SHA256=114450e3431189018a8f7cf460db440d5e3062a61b45bd2f91eb2adf0e8cf771
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt.in ./CMakeLists.txt
    DEPENDS dep_GLEW
)

if (TARGET ${ZLIB_PKG})
    add_dependencies(dep_OpenCSG ${ZLIB_PKG})
endif()

if (MSVC)
    add_debug_dep(dep_OpenCSG)
endif ()
