include_guard(GLOBAL)

set(_orca_semantic_native_default OFF)
set(_orca_semantic_x64 OFF)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
   NOT CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]|aarch64" AND
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]|aarch64")
    set(_orca_semantic_x64 ON)
    set(_orca_semantic_native_default ON)
endif()
set_property(GLOBAL PROPERTY ORCA_SEMANTIC_X64 ${_orca_semantic_x64})
option(ORCA_ENABLE_MEDIAPIPE_NATIVE "Enable the optional Windows x64 CPU semantic recognizers" ${_orca_semantic_native_default})
set(ORCA_SEMANTIC_RUNTIME_DIR "" CACHE PATH "Verified semantic runtime prepared by scripts/prepare_semantic_runtime.ps1; empty builds without staging")
set_property(GLOBAL PROPERTY ORCA_SEMANTIC_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Apply to the target which compiles MediaPipeRegionRecognizers.cpp. The DLL is
# loaded dynamically; no Python interpreter, import library or GPU is required.
function(orca_configure_semantic_recognizers target)
    get_property(_semantic_cmake_dir GLOBAL PROPERTY ORCA_SEMANTIC_CMAKE_DIR)
    get_property(_semantic_x64 GLOBAL PROPERTY ORCA_SEMANTIC_X64)
    get_filename_component(_semantic_source_dir "${_semantic_cmake_dir}/.." ABSOLUTE)
    if(ORCA_ENABLE_MEDIAPIPE_NATIVE)
        if(NOT _semantic_x64)
            message(FATAL_ERROR "ORCA_ENABLE_MEDIAPIPE_NATIVE currently supports Windows x64 only")
        endif()
        target_compile_definitions(${target} PRIVATE ORCA_ENABLE_MEDIAPIPE_NATIVE=1)
        target_include_directories(${target} PRIVATE "${_semantic_source_dir}/src/slic3r/AI/ModelGeneration/SemanticColoring/vendor/mediapipe-1.0.0")
        # The parent target uses CMake's plain link signature. Properties keep
        # this helper usable with either signature, including standalone tests.
        set_property(TARGET ${target} APPEND PROPERTY LINK_LIBRARIES bcrypt)
        get_target_property(_semantic_target_type ${target} TYPE)
        if(_semantic_target_type STREQUAL "STATIC_LIBRARY")
            set_property(TARGET ${target} APPEND PROPERTY INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:bcrypt>")
        endif()
    endif()
endfunction()

# Call in the directory that creates the final executable target. Staging is
# explicit, offline, and separate from the sidecar's source-resource junction.
function(orca_stage_semantic_runtime target)
    if(NOT ORCA_ENABLE_MEDIAPIPE_NATIVE OR NOT ORCA_SEMANTIC_RUNTIME_DIR)
        return()
    endif()
    get_property(_semantic_cmake_dir GLOBAL PROPERTY ORCA_SEMANTIC_CMAKE_DIR)
    set(SEMANTIC_RUNTIME_SOURCE "${ORCA_SEMANTIC_RUNTIME_DIR}")
    include("${_semantic_cmake_dir}/OrcaStageSemanticRuntime.cmake")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DSEMANTIC_RUNTIME_SOURCE=${ORCA_SEMANTIC_RUNTIME_DIR}"
            "-DSEMANTIC_RUNTIME_DESTINATION=$<TARGET_FILE_DIR:${target}>/ai/portrait_semantics"
            -P "${_semantic_cmake_dir}/OrcaStageSemanticRuntime.cmake"
        COMMENT "Verifying and staging the optional CPU semantic runtime"
        VERBATIM)
    foreach(_semantic_file IN LISTS _orca_semantic_runtime_files)
        install(FILES "${ORCA_SEMANTIC_RUNTIME_DIR}/${_semantic_file}" DESTINATION "ai/portrait_semantics")
    endforeach()
endfunction()
