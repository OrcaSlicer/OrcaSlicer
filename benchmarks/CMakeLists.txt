add_library(orcabench_core STATIC
    core/BuildId.cpp
    core/Catalog.cpp
    core/Compare.cpp
    core/Document.cpp
    core/Host.cpp
    core/Measurement.cpp
    core/Options.cpp
    core/Policy.cpp
    core/Reporters.cpp
    core/Result.cpp
    core/Runner.cpp
    core/Sampler.cpp
    core/Summary.cpp
    core/Workload.cpp
    )

target_include_directories(orcabench_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The catalogs orca_bench lists and runs, which the tests read too.
set(_orcabench_catalog "${CMAKE_CURRENT_SOURCE_DIR}/catalog")
file(TO_NATIVE_PATH "${_orcabench_catalog}" _orcabench_catalog)
target_compile_definitions(orcabench_core PUBLIC ORCABENCH_CATALOG_DIR=R"\(${_orcabench_catalog}\)")

# The sampler reads the process on its own thread.
find_package(Threads REQUIRED)
target_link_libraries(orcabench_core PUBLIC Threads::Threads)

# json.hpp includes its siblings as <nlohmann/...>, which the nlohmann_json target's include
# directory cannot resolve.
target_link_libraries(orcabench_core PRIVATE nlohmann_json)
target_include_directories(orcabench_core SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/deps_src)

target_include_directories(orcabench_core PRIVATE ${ORCABENCH_GIT_COMMIT_HASH_DIR})
add_dependencies(orcabench_core git_commit_hash_header)

# Records the compiler and the flags code here compiles with, including the add_compile_options
# that cache options such as SLIC3R_ASAN turn on, except those written as generator expressions.
get_directory_property(_orcabench_options COMPILE_OPTIONS)
string(GENEX_STRIP "${_orcabench_options}" _orcabench_options)
list(JOIN _orcabench_options " " _orcabench_options)
set(_orcabench_configs ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
list(REMOVE_DUPLICATES _orcabench_configs)
foreach (_config IN LISTS _orcabench_configs)
    string(TOUPPER "${_config}" _config_upper)
    string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_config_upper}} ${_orcabench_options}" _flags)
    string(REPLACE "\\" "\\\\" _flags "${_flags}")
    string(REPLACE "\"" "\\\"" _flags "${_flags}")
    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_config}/orcabench_build.h"
        CONTENT "#pragma once
#define ORCABENCH_COMPILER \"${CMAKE_CXX_COMPILER_ID}\"
#define ORCABENCH_COMPILER_VERSION \"${CMAKE_CXX_COMPILER_VERSION}\"
#define ORCABENCH_FLAGS \"${_flags}\"
#define ORCABENCH_CONFIG \"${_config}\"
")
endforeach ()

target_include_directories(orcabench_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")

if (WIN32)
    # GetProcessMemoryInfo
    target_link_libraries(orcabench_core PRIVATE Psapi.lib)
endif ()

set_property(TARGET orcabench_core PROPERTY FOLDER "benchmarks")

# What slices, which only this library links, so orcabench_core and its tests never need libslic3r.
# Kinds add themselves to the registry during static initialization, and an object library puts
# every object in each executable, where a static library's linker would drop registrars nothing
# references.
file(GLOB _orcabench_kinds CONFIGURE_DEPENDS kinds/*.cpp)
add_library(orcabench_slicer OBJECT
    slicer/Environment.cpp
    slicer/Fixtures.cpp
    slicer/Nanosvg.cpp
    slicer/Output.cpp
    ${_orcabench_kinds}
    )
target_link_libraries(orcabench_slicer PUBLIC orcabench_core libslic3r)
# The resources orca_bench and the tests that slice read printer data from.
set(_orcabench_resources "${CMAKE_SOURCE_DIR}/resources")
file(TO_NATIVE_PATH "${_orcabench_resources}" _orcabench_resources)
target_compile_definitions(orcabench_slicer PUBLIC ORCABENCH_RESOURCES_DIR=R"\(${_orcabench_resources}\)")
set_property(TARGET orcabench_slicer PROPERTY FOLDER "benchmarks")

add_executable(orca_bench main.cpp)
target_link_libraries(orca_bench PRIVATE orcabench_slicer)
set_property(TARGET orca_bench PROPERTY FOLDER "benchmarks")
include(OrcaSlicerRuntimeDeps)
orcaslicer_copy_test_dlls(orca_bench)
