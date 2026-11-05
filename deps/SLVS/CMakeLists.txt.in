# Replaces the upstream SolveSpaceLib CMakeLists, which builds a demo executable and
# has no install rules. The sources themselves are used verbatim.
cmake_minimum_required(VERSION 3.13)

project(SLVS VERSION 3.0)

add_library(slvs
    libslvs/constrainteq.cpp
    libslvs/entity.cpp
    libslvs/expr.cpp
    libslvs/system.cpp
    libslvs/util.cpp
    libslvs/platform/unixutil.cpp
    libslvs/lib.cpp
    libslvs/SolveSpaceSystem.cpp)

target_compile_features(slvs PUBLIC cxx_std_11)

# LIBRARY strips the solver core out of the SolveSpace application it was extracted from.
target_compile_definitions(slvs PRIVATE -DLIBRARY)
if (MSVC)
    target_compile_definitions(slvs PRIVATE -D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS)
endif ()

target_include_directories(slvs
    PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libslvs/include>
    PRIVATE ${PROJECT_SOURCE_DIR}/libslvs)

# libslic3r is linked into shared targets, so this has to be position independent.
set_target_properties(slvs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 2018 code, predating the project's warning settings; it is not ours to clean up.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(slvs PRIVATE -w -fno-strict-aliasing)
endif ()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion)

install(TARGETS slvs
    EXPORT ${PROJECT_NAME}Targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(ConfigPackageLocation ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

install(EXPORT ${PROJECT_NAME}Targets
    FILE "${PROJECT_NAME}Config.cmake"
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION ${ConfigPackageLocation})

install(FILES
        ${PROJECT_SOURCE_DIR}/libslvs/include/slvs.h
        ${PROJECT_SOURCE_DIR}/libslvs/include/SolveSpaceSystem.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    DESTINATION ${ConfigPackageLocation})
