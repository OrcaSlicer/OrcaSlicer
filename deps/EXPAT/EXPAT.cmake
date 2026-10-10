orcaslicer_add_cmake_project(EXPAT
  SOURCE_DIR          ${CMAKE_CURRENT_LIST_DIR}/expat
)

if (MSVC)
    add_debug_dep(dep_EXPAT)
endif ()
