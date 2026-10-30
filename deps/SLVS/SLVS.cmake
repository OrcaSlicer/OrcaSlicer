# libslvs — the geometric constraint solver behind the Design tab's sketch constraints.
# Extraction of solvespace.com's libslvs, taken verbatim from JacobStoren/SolveSpaceLib;
# only the CMakeLists is ours, because upstream's builds a demo and installs nothing.
# GPLv3, compatible with this fork's licence. Self-contained: no external dependencies.
orcaslicer_add_cmake_project(SLVS
    URL https://github.com/solvespace/solvespace/archive/refs/tags/v3.2.zip
    URL_HASH SHA256=3312fc9a692584bb5a0504f9268adfbc9c93cf07334dfba5fc61190d0e6c7838
)

if (MSVC)
    add_debug_dep(dep_SLVS)
endif ()
