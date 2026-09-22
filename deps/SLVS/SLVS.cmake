# libslvs — the geometric constraint solver behind the Design tab's sketch constraints.
# Extraction of solvespace.com's libslvs, taken verbatim from JacobStoren/SolveSpaceLib;
# only the CMakeLists is ours, because upstream's builds a demo and installs nothing.
# GPLv3, compatible with this fork's licence. Self-contained: no external dependencies.
orcaslicer_add_cmake_project(SLVS
    URL https://github.com/JacobStoren/SolveSpaceLib/archive/4d8704523e4bf212fadf5189f92484244f670fea.zip
    URL_HASH SHA256=1c4bdde9c3c6ef20ea4b50b73601de56769f2eb131b36927d7c6489f102e6c30
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt.in ./CMakeLists.txt
)

if (MSVC)
    add_debug_dep(dep_SLVS)
endif ()
