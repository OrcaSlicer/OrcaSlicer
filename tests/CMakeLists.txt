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

# Shipped vendor profiles, so tests can exercise the real machine/filament gcode.
set(PROFILES_DIR ${CMAKE_SOURCE_DIR}/resources/profiles)
file(TO_NATIVE_PATH "${PROFILES_DIR}" PROFILES_DIR)

include(CTest)
include(Catch)

set(CATCH_EXTRA_ARGS "" CACHE STRING "Extra arguments ctest passes to every Catch2 test case, as a command line.")
separate_arguments(CATCH_EXTRA_ARGS_LIST NATIVE_COMMAND "${CATCH_EXTRA_ARGS}")

add_library(test_common INTERFACE)
target_compile_definitions(test_common INTERFACE TEST_DATA_DIR=R"\(${TEST_DATA_DIR}\)" PROFILES_DIR=R"\(${PROFILES_DIR}\)" CATCH_CONFIG_FAST_COMPILE)
target_include_directories(test_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if (APPLE)
    target_link_libraries(test_common INTERFACE "-liconv -framework IOKit" "-framework CoreFoundation" -lc++)
endif()

include(OrcaSlicerRuntimeDeps)

# Register Catch2 tags as CTest labels so `ctest -L`/`-LE` can filter by tag, passing any further
# arguments, such as PROPERTIES, to catch_discover_tests. A case or section that runs no assertion fails.
function(orcaslicer_discover_tests TARGET)
    catch_discover_tests(${TARGET} ADD_TAGS_AS_LABELS EXTRA_ARGS --warn NoAssertions ${CATCH_EXTRA_ARGS_LIST} ${ARGN})
endfunction()

add_subdirectory(libnest2d)
add_subdirectory(libslic3r)
add_subdirectory(automation)
add_subdirectory(slic3rutils)
add_subdirectory(fff_print)
add_subdirectory(sla_print)
add_subdirectory(filament_group)

# End-to-end checks of the orca-slicer binary. Linux only: they read result.json, which the CLI
# writes on Linux only. src/ is added before tests/, so the target is known here.
if (UNIX AND NOT APPLE AND TARGET OrcaSlicer)
    add_subdirectory(cli)
endif ()

if (ORCA_BENCHMARKS)
    add_subdirectory(orcabench)
endif ()
