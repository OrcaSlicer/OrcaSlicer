# Vendored SolveSpace geometric constraint solver (libslvs), isolated solver core.
# Source: github.com/JacobStoren/SolveSpaceLib (extraction of solvespace.com libslvs).
# GPLv3 — compatible with this AGPL fork. Built as a self-contained STATIC lib; only
# include/slvs.h (+ SolveSpaceSystem.h C++ wrapper) is exposed to libslic3r.
# No external deps: it ships its own dense linear solver (no Eigen).

add_library(libslvs STATIC
    constrainteq.cpp
    entity.cpp
    expr.cpp
    system.cpp
    util.cpp
    platform/unixutil.cpp
    lib.cpp
    SolveSpaceSystem.cpp)

target_compile_definitions(libslvs PRIVATE -DLIBRARY)

# Public API headers (include/slvs.h, include/SolveSpaceSystem.h) propagate to consumers;
# the internal solvespace headers stay PRIVATE so they never leak into libslic3r TUs.
target_include_directories(libslvs
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# libslic3r is linked into shared targets → PIC required. Silence warnings for this
# vendored code (it predates the project's -Wall/-Werror=return-type cleanliness).
set_target_properties(libslvs PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(libslvs PRIVATE -w -fno-strict-aliasing)
endif()
