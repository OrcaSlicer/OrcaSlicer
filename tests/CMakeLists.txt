# Add individual tests as executables in separate directories at the bottom of this file

add_subdirectory(catch2)

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

add_subdirectory(libnest2d)
add_subdirectory(libslic3r)
add_subdirectory(slic3rutils)
add_subdirectory(fff_print)
add_subdirectory(sla_print)


