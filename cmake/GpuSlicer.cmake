# Shared optional geometry backend. CPU-only builds do not probe GPU toolchains.
option(SLIC3R_ENABLE_CUDA_SLICER "Build the optional CUDA geometry backend" OFF)
option(SLIC3R_ENABLE_VULKAN_SLICER "Build the optional Vulkan geometry backend" OFF)
option(SLIC3R_GPU_SELFTEST "Build the standalone GPU compute diagnostic" OFF)

if((SLIC3R_ENABLE_CUDA_SLICER OR SLIC3R_ENABLE_VULKAN_SLICER) AND CMAKE_VERSION VERSION_LESS 3.20)
    message(FATAL_ERROR "The optional GPU backends require CMake 3.20 or newer")
endif()

if(SLIC3R_ENABLE_CUDA_SLICER)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES "61;75;86;89" CACHE STRING "CUDA target architectures")
    endif()
    # Enable outside the function so compiler rule variables remain in scope.
    enable_language(CUDA)
endif()

# PRODUCT supplies the platform resource default; RESOURCE_DESTINATION overrides it.
function(slic3r_configure_gpu_backend target)
    if(NOT SLIC3R_ENABLE_CUDA_SLICER AND NOT SLIC3R_ENABLE_VULKAN_SLICER)
        return()
    endif()
    cmake_parse_arguments(GPU "PLAIN_LINK" "SOURCE_DIR;RESOURCE_DESTINATION;PRODUCT" "" ${ARGN})
    if(NOT GPU_SOURCE_DIR)
        set(GPU_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/libslic3r/Gpu")
    endif()
    set(backend "${target}_gpu_backend")
    add_library(${backend} STATIC
        "${GPU_SOURCE_DIR}/VulkanSlicer.cpp"
        "${GPU_SOURCE_DIR}/CudaSlicer.cpp")
    target_compile_features(${backend} PUBLIC cxx_std_17)
    find_package(Threads REQUIRED)
    target_link_libraries(${backend} PRIVATE Threads::Threads)
    set_target_properties(${backend} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${backend} PUBLIC "${GPU_SOURCE_DIR}" ${Boost_INCLUDE_DIRS})
    if(TARGET Boost::headers)
        target_link_libraries(${backend} PRIVATE Boost::headers)
    elseif(TARGET Boost::boost)
        target_link_libraries(${backend} PRIVATE Boost::boost)
    endif()
    if(GPU_PLAIN_LINK)
        target_link_libraries(${target} ${backend})
    else()
        target_link_libraries(${target} PRIVATE ${backend})
    endif()
    target_compile_definitions(${target} PRIVATE SLIC3R_GPU_ENABLED=1)

    if(SLIC3R_ENABLE_CUDA_SLICER)
        find_package(CUDAToolkit REQUIRED)
        if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL 13 AND "61" IN_LIST CMAKE_CUDA_ARCHITECTURES)
            message(FATAL_ERROR "Pascal sm_61 requires CUDA 12.x; select that toolkit or supported architectures")
        endif()
        # Isolate NVCC from directory-wide C++ compiler flags (/MP, /Zi, etc.).
        add_library(${backend}_cuda OBJECT "${GPU_SOURCE_DIR}/CudaSlicer.cu")
        set_property(TARGET ${backend}_cuda PROPERTY COMPILE_OPTIONS "")
        set_target_properties(${backend}_cuda PROPERTIES
            CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
            CUDA_RUNTIME_LIBRARY Shared POSITION_INDEPENDENT_CODE ON)
        target_include_directories(${backend}_cuda PRIVATE "${GPU_SOURCE_DIR}" ${Boost_INCLUDE_DIRS})
        target_compile_definitions(${backend}_cuda PRIVATE SLIC3R_ENABLE_CUDA_SLICER=1)
        if(TARGET Boost::headers)
            target_link_libraries(${backend}_cuda PRIVATE Boost::headers)
        elseif(TARGET Boost::boost)
            target_link_libraries(${backend}_cuda PRIVATE Boost::boost)
        endif()
        target_sources(${backend} PRIVATE $<TARGET_OBJECTS:${backend}_cuda>)
        target_compile_definitions(${backend} PRIVATE SLIC3R_ENABLE_CUDA_SLICER=1)
        target_link_libraries(${backend} PRIVATE CUDA::cudart)
        if(WIN32)
            find_file(cuda_runtime_dll NAMES "cudart64_${CUDAToolkit_VERSION_MAJOR}.dll"
                PATHS "${CUDAToolkit_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)
            install(FILES "${cuda_runtime_dll}" DESTINATION ".")
        endif()
    endif()

    if(SLIC3R_ENABLE_VULKAN_SLICER)
        find_package(Vulkan 1.2 REQUIRED)
        if(NOT Vulkan_GLSLC_EXECUTABLE)
            find_program(Vulkan_GLSLC_EXECUTABLE glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
        endif()
        target_link_libraries(${backend} PRIVATE Vulkan::Vulkan)
        if(WIN32)
            # Bundle the loader so unsupported machines can reach CPU fallback
            # even when they do not already have a Vulkan loader installed.
            set(SLIC3R_VULKAN_RUNTIME_DLL "" CACHE FILEPATH "Redistributable x64 Vulkan loader from the official runtime ZIP")
            if(NOT EXISTS "${SLIC3R_VULKAN_RUNTIME_DLL}")
                message(FATAL_ERROR "Set SLIC3R_VULKAN_RUNTIME_DLL to the x64 vulkan-1.dll from LunarG's runtime components ZIP")
            endif()
            install(FILES "${SLIC3R_VULKAN_RUNTIME_DLL}" DESTINATION ".")
        endif()
        target_compile_definitions(${backend} PRIVATE SLIC3R_ENABLE_VULKAN_SLICER=1)
        set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/resources/vulkan")
        target_compile_definitions(${backend} PRIVATE SLIC3R_VULKAN_BUILD_SHADER_DIR="${shader_dir}")
        set(shaders "")
        foreach(workgroup IN ITEMS 64 128 256 512 1024)
            set(output "${shader_dir}/perimeter_infill_candidates_${workgroup}.spv")
            add_custom_command(OUTPUT "${output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
                COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O
                    "-DORCA_WORKGROUP_SIZE=${workgroup}" -o "${output}"
                    "${GPU_SOURCE_DIR}/shaders/perimeter_infill_candidates.comp"
                DEPENDS "${GPU_SOURCE_DIR}/shaders/perimeter_infill_candidates.comp" VERBATIM)
            list(APPEND shaders "${output}")
        endforeach()
        set(output "${shader_dir}/tree_support_contour_candidates.spv")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O -o "${output}"
                "${GPU_SOURCE_DIR}/shaders/tree_support_contour_candidates.comp"
            DEPENDS "${GPU_SOURCE_DIR}/shaders/tree_support_contour_candidates.comp" VERBATIM)
        list(APPEND shaders "${output}")
        add_custom_target(${backend}_shaders DEPENDS ${shaders})
        add_dependencies(${backend} ${backend}_shaders)
        if(NOT GPU_RESOURCE_DESTINATION)
            if(NOT "${GPU_PRODUCT}" MATCHES "^(BambuStudio|QIDIStudio|OrcaSlicer)$")
                message(FATAL_ERROR "Provide PRODUCT (BambuStudio, QIDIStudio, OrcaSlicer) or an explicit RESOURCE_DESTINATION")
            endif()
            # Match the resource locations used by each product's root install rules.
            if(WIN32)
                set(GPU_RESOURCE_DESTINATION "resources")
            elseif(SLIC3R_FHS)
                set(GPU_RESOURCE_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${GPU_PRODUCT}")
            elseif(CMAKE_MACOSX_BUNDLE)
                set(GPU_RESOURCE_DESTINATION "${GPU_PRODUCT}.app/Contents/Resources")
            else()
                set(GPU_RESOURCE_DESTINATION "resources")
            endif()
        endif()
        install(FILES ${shaders} DESTINATION "${GPU_RESOURCE_DESTINATION}/vulkan")
    endif()

    if(SLIC3R_GPU_SELFTEST)
        add_executable(slic3r-gpu-compute-selftest "${GPU_SOURCE_DIR}/VulkanComputeSelfTest.cpp")
        target_link_libraries(slic3r-gpu-compute-selftest PRIVATE ${backend})
        install(TARGETS slic3r-gpu-compute-selftest RUNTIME DESTINATION ".")
    endif()
endfunction()
