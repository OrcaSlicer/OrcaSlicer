cmake_minimum_required(VERSION 3.2)
project(earcut_hpp LANGUAGES CXX C)

option(EARCUT_BUILD_TESTS "Build the earcut test program" ON)
option(EARCUT_BUILD_BENCH "Build the earcut benchmark program" ON)
option(EARCUT_BUILD_VIZ "Build the earcut visualizer program" ON)
option(EARCUT_WARNING_IS_ERROR "Treat warnings as errors" OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT GENERATOR_IS_MULTI_CONFIG)
    message(STATUS "No build type specified. Setting to 'Release'")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "The type of build." FORCE)
endif()


include(GNUInstallDirs)

add_library(earcut_hpp INTERFACE)
add_library(earcut_hpp::earcut_hpp ALIAS earcut_hpp)

target_include_directories(earcut_hpp INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set(CMAKE_CXX_STANDARD 11)

if("${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}" GREATER 3.7)
    # Allow C++11 requirements to propagate when using recent CMake versions
    target_compile_features(earcut_hpp INTERFACE cxx_std_11)
endif()

file(GLOB FIXTURE_SOURCE_FILES test/fixtures/*.cpp test/fixtures/*.hpp)
source_group(fixtures FILES ${FIXTURE_SOURCE_FILES})
add_library(fixtures OBJECT ${FIXTURE_SOURCE_FILES})
target_compile_options(fixtures PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Od>)

# In CMake 3.12, use target_link_libraries(fixtures PUBLIC earcut_hpp libtess2).
# Since we support down to CMake 3.2, we need to manually propagate usage requirements of earcut_hpp
target_include_directories(fixtures PRIVATE "$<TARGET_PROPERTY:earcut_hpp,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_features(fixtures PRIVATE "$<TARGET_PROPERTY:earcut_hpp,INTERFACE_COMPILE_FEATURES>")


file(GLOB COMPARISON_SOURCE_FILES test/comparison/*.cpp test/comparison/*.hpp)
source_group(comparison FILES ${COMPARISON_SOURCE_FILES})
# this is interface since there is no cpp files in the comparison directory
add_library(comparison INTERFACE)


file(GLOB LIBTESS2_SOURCE_FILES test/comparison/libtess2/*.c test/comparison/libtess2/*.h)
source_group(comparison/libtess2 FILES ${LIBTESS2_SOURCE_FILES})
add_library(libtess2 ${LIBTESS2_SOURCE_FILES})
target_compile_options(libtess2 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/wd4244 /wd4267>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>
)

add_library(common INTERFACE)
target_link_libraries(common INTERFACE libtess2 comparison)

# optional: -march=native (builds with the optimizations available on the build machine (only for local use!))
target_compile_options(common INTERFACE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-pipe -Wall -Wextra -Wconversion -Wpedantic>
)

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$" OR CMAKE_COMPILER_IS_GNUCXX)
    if ("${CMAKE_CXX_FLAGS}" MATCHES "--coverage")
        # We disable debug code for the coverage so it won't see assertion and other things only enabled for debugging
        target_compile_definitions(common INTERFACE NDEBUG)
    else()
        # Here we enable the undefined behavior sanitizer for the tests, benchmarks and the viz
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-fsanitize=undefined" HAVE_FLAG_SANITIZE_UNDEFINED)
        if(HAVE_FLAG_SANITIZE_UNDEFINED)
            target_compile_options(common INTERFACE $<$<CONFIG:Debug>:-fsanitize=undefined>)
            # TODO: Replace with target link option once we support CMake 3.13 
            target_link_libraries(common INTERFACE $<$<CONFIG:Debug>:-fsanitize=undefined>)
        endif()
    endif()
endif()

if (EARCUT_WARNING_IS_ERROR)
    target_compile_options(common INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>
    )
endif()

if (EARCUT_BUILD_TESTS)
    enable_testing()
    add_executable(tests test/tap.cpp test/tap.hpp test/test.cpp $<TARGET_OBJECTS:fixtures>)
    target_link_libraries(tests PRIVATE earcut_hpp common)
    add_test(NAME earcut_tests COMMAND tests)
endif()
if (EARCUT_BUILD_BENCH)
    add_executable(bench test/bench.cpp $<TARGET_OBJECTS:fixtures>)
    target_link_libraries(bench PRIVATE earcut_hpp common)
endif()
if (EARCUT_BUILD_VIZ)
    add_executable(viz test/viz.cpp $<TARGET_OBJECTS:fixtures>)

    # Setup viz target
    # OpenGL
    # linux: xorg-dev libgl1-mesa-glx libgl1-mesa-dev
    # windows: in the windows sdk
    find_package(OpenGL REQUIRED)

    # GLFW3
    find_package(glfw3 QUIET) # try to use the system default
    if (NOT glfw3_FOUND)
        if(EXISTS "${PROJECT_SOURCE_DIR}/.gitmodules")
            find_package(Git REQUIRED)
            execute_process(
                    COMMAND             ${GIT_EXECUTABLE} submodule update --init --recursive
                    WORKING_DIRECTORY   ${PROJECT_SOURCE_DIR}
                    OUTPUT_QUIET
                    ERROR_QUIET
            )
        endif()

        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build the GLFW example programs" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "Build the GLFW test programs" FORCE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "Generate installation target" FORCE)
        add_subdirectory(glfw)
    endif()
    
    target_compile_definitions(viz PRIVATE GL_SILENCE_DEPRECATION)
    
    # TODO: Using old variables for OpenGL package since they were added in CMake 3.8
    target_link_libraries(viz PRIVATE earcut_hpp common glfw ${OPENGL_LIBRARIES})
    target_include_directories(viz PRIVATE ${OPENGL_INCLUDE_DIR})
endif()

install(
  DIRECTORY include/mapbox
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp"
)

install(TARGETS earcut_hpp EXPORT earcut_hpp-config)

# Since there is two projects, we need to export into the parent directory
export(
  TARGETS earcut_hpp
  NAMESPACE earcut_hpp::
  FILE "${PROJECT_BINARY_DIR}/earcut_hpp-config.cmake"
)

install(EXPORT earcut_hpp-config
  DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/earcut_hpp"
  NAMESPACE earcut_hpp::
)
