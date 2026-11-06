cmake_minimum_required(VERSION 3.20)
project(slicer_gpu_safety_tests LANGUAGES CXX)

option(SLIC3R_ENABLE_CUDA_SLICER "Build CUDA geometry backend" OFF)
option(SLIC3R_ENABLE_VULKAN_SLICER "Build Vulkan geometry backend" OFF)
option(SLIC3R_BUILD_GPU_CONTRACT_TESTS "Build the hardware-independent Catch tests" ON)
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
set(GPU_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/libslic3r/Gpu")
add_library(slicer_gpu_core STATIC
    "${GPU_SOURCE_DIR}/CudaSlicer.cpp" "${GPU_SOURCE_DIR}/VulkanSlicer.cpp")
target_compile_features(slicer_gpu_core PUBLIC cxx_std_17)
target_include_directories(slicer_gpu_core PUBLIC "../../src" "${GPU_SOURCE_DIR}" ${Boost_INCLUDE_DIRS})
target_link_libraries(slicer_gpu_core PRIVATE Threads::Threads)

if(SLIC3R_ENABLE_CUDA_SLICER)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES 61 CACHE STRING "CUDA architectures; Pascal needs CUDA12.x")
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    # A separate object target keeps application CXX/MSVC flags out of NVCC.
    add_library(slicer_gpu_cuda OBJECT "${GPU_SOURCE_DIR}/CudaSlicer.cu")
    target_include_directories(slicer_gpu_cuda PRIVATE ${Boost_INCLUDE_DIRS})
    set_target_properties(slicer_gpu_cuda PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Shared)
    target_sources(slicer_gpu_core PRIVATE $<TARGET_OBJECTS:slicer_gpu_cuda>)
    target_compile_definitions(slicer_gpu_core PRIVATE SLIC3R_ENABLE_CUDA_SLICER=1)
    target_link_libraries(slicer_gpu_core PRIVATE CUDA::cudart)
endif()

if(SLIC3R_ENABLE_VULKAN_SLICER)
    find_package(Vulkan 1.2 REQUIRED COMPONENTS glslc)
    target_compile_definitions(slicer_gpu_core PRIVATE SLIC3R_ENABLE_VULKAN_SLICER=1)
    target_link_libraries(slicer_gpu_core PRIVATE Vulkan::Vulkan)
    add_executable(gpu_vulkan_readback_test test_vulkan_readback.cpp)
    target_compile_features(gpu_vulkan_readback_test PRIVATE cxx_std_17)
    target_compile_definitions(gpu_vulkan_readback_test PRIVATE SLIC3R_ENABLE_VULKAN_SLICER=1)
    target_include_directories(gpu_vulkan_readback_test PRIVATE "${GPU_SOURCE_DIR}" ${Vulkan_INCLUDE_DIRS})
    # This boundary contract must retain assertions in Release too.
    if(MSVC)
        target_compile_options(gpu_vulkan_readback_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(gpu_vulkan_readback_test PRIVATE -UNDEBUG)
    endif()
    enable_testing()
    add_test(NAME gpu_vulkan_readback COMMAND gpu_vulkan_readback_test)
    set(GPU_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources/vulkan")
    target_compile_definitions(slicer_gpu_core PRIVATE SLIC3R_VULKAN_BUILD_SHADER_DIR="${GPU_SHADER_DIR}")
    set(GPU_SHADERS)
    foreach(SIZE 64 128 256 512 1024)
        set(OUTPUT "${GPU_SHADER_DIR}/perimeter_infill_candidates_${SIZE}.spv")
        add_custom_command(OUTPUT "${OUTPUT}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${GPU_SHADER_DIR}"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O
                "-DORCA_WORKGROUP_SIZE=${SIZE}" -o "${OUTPUT}"
                "${GPU_SOURCE_DIR}/shaders/perimeter_infill_candidates.comp"
            DEPENDS "${GPU_SOURCE_DIR}/shaders/perimeter_infill_candidates.comp" VERBATIM)
        list(APPEND GPU_SHADERS "${OUTPUT}")
    endforeach()
    set(OUTPUT "${GPU_SHADER_DIR}/tree_support_contour_candidates.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${GPU_SHADER_DIR}"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O -o "${OUTPUT}"
            "${GPU_SOURCE_DIR}/shaders/tree_support_contour_candidates.comp"
        DEPENDS "${GPU_SOURCE_DIR}/shaders/tree_support_contour_candidates.comp" VERBATIM)
    list(APPEND GPU_SHADERS "${OUTPUT}")
    add_custom_target(slicer_gpu_shaders ALL DEPENDS ${GPU_SHADERS})
    add_dependencies(slicer_gpu_core slicer_gpu_shaders)
endif()

add_executable(slicer-gpu-selftest "${GPU_SOURCE_DIR}/VulkanComputeSelfTest.cpp")
target_link_libraries(slicer-gpu-selftest PRIVATE slicer_gpu_core)

if(SLIC3R_BUILD_GPU_CONTRACT_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG 9827c148c397289df17d3967692619a198032a24)
    FetchContent_MakeAvailable(Catch2)
    add_executable(gpu_safety_tests
        ../libslic3r/test_gpu_exact_geometry.cpp
        ../libslic3r/test_cuda_dispatch_buffers.cpp
        ../libslic3r/test_gpu_batch_preflight.cpp)
    target_link_libraries(gpu_safety_tests PRIVATE slicer_gpu_core Catch2::Catch2WithMain)
    enable_testing()
    add_test(NAME gpu_safety COMMAND gpu_safety_tests)
endif()

# This CPU-only child-process fixture includes the production recorder to check
# normal-exit lifetime, Unicode paths and failed writes without public test hooks.
add_executable(gpu_exit_diagnostics test_exit_diagnostics.cpp "${GPU_SOURCE_DIR}/CudaSlicer.cpp")
target_compile_features(gpu_exit_diagnostics PRIVATE cxx_std_17)
target_include_directories(gpu_exit_diagnostics PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(gpu_exit_diagnostics PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(gpu_exit_diagnostics PRIVATE /utf-8)
endif()
enable_testing()
add_test(NAME gpu_exit_diagnostics COMMAND gpu_exit_diagnostics)

# Stub only the external CUDA runtime boundary; the real facade and exact gate
# must account for a completed submission whose result is rejected.
add_executable(gpu_cuda_accounting test_cuda_submission_accounting.cpp
    "${GPU_SOURCE_DIR}/VulkanSlicer.cpp" "${GPU_SOURCE_DIR}/CudaSlicer.cpp")
target_compile_features(gpu_cuda_accounting PRIVATE cxx_std_17)
target_compile_definitions(gpu_cuda_accounting PRIVATE SLIC3R_ENABLE_CUDA_SLICER=1)
target_include_directories(gpu_cuda_accounting PRIVATE "${GPU_SOURCE_DIR}" ${Boost_INCLUDE_DIRS})
target_link_libraries(gpu_cuda_accounting PRIVATE Threads::Threads)
add_test(NAME gpu_cuda_accounting COMMAND gpu_cuda_accounting)
