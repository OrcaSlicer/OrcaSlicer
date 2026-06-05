# Add individual tests as executables in separate directories at the bottom of this file

add_subdirectory(catch2)

# The top-level project defines _UNICODE globally, which makes Catch2's bundled
# main emit wmain instead of main. Test executables link with the default
# /SUBSYSTEM:CONSOLE entry point (mainCRTStartup, which calls main), so force
# Catch2 to emit main to keep the CRT entry point happy.
if(MSVC)
    target_compile_definitions(Catch2WithMain PRIVATE DO_NOT_USE_WMAIN)
endif()

set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
file(TO_NATIVE_PATH "${TEST_DATA_DIR}" TEST_DATA_DIR)

include(CTest)
include(Catch)

set(CATCH_EXTRA_ARGS "" CACHE STRING "Extra arguments for catch2 test suites.") # Unknown if this still works and/or should be replaced with something else.

add_library(test_common INTERFACE)
target_compile_definitions(test_common INTERFACE TEST_DATA_DIR=R"\(${TEST_DATA_DIR}\)" CATCH_CONFIG_FAST_COMPILE)
target_include_directories(test_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if (APPLE)
    target_link_libraries(test_common INTERFACE "-liconv -framework IOKit" "-framework CoreFoundation" -lc++)
endif()

# Copies runtime DLLs next to each test executable. Handles both single-config
# generators (CMAKE_BUILD_TYPE set) and multi-config generators (Ninja
# Multi-Config, Visual Studio) where CMAKE_BUILD_TYPE is empty and DLLs must
# land in every per-config output directory.
function(orcaslicer_copy_test_dlls)
    if (NOT WIN32)
        return()
    endif()
    set(_configs ${CMAKE_CONFIGURATION_TYPES})
    if (NOT _configs)
        set(_configs "${CMAKE_BUILD_TYPE}")
    endif()
    foreach(_cfg IN LISTS _configs)
        if (_cfg STREQUAL "Debug")
            orcaslicer_copy_dlls(COPY_DLLS "Debug" "d" _unused_dlls)
        else()
            orcaslicer_copy_dlls(COPY_DLLS "${_cfg}" "" _unused_dlls)
        endif()
    endforeach()
endfunction()

add_subdirectory(libnest2d)
add_subdirectory(libslic3r)
add_subdirectory(slic3rutils)
add_subdirectory(fff_print)
add_subdirectory(sla_print)


