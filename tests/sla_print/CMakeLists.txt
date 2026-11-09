get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests ${_TEST_NAME}_tests_main.cpp
    sla_print_tests.cpp
    sla_test_utils.hpp sla_test_utils.cpp
    sla_supptgen_tests.cpp
    sla_raycast_tests.cpp)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
# Hollowing needs OpenVDB; built with SLIC3R_OPENVDB=OFF it is a no-op.
if (TARGET OpenVDB::openvdb)
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE SLIC3R_HAS_OPENVDB)
endif ()
if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(${_TEST_NAME}_tests ${CMAKE_SOURCE_DIR}/src/libslic3r/pchheader.hpp FORCEINCLUDE)
endif ()

set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
