get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

# The automation core's suite. It links libslic3r_automation and nothing of libslic3r_gui, so it builds
# without wxWidgets and the embedded Python, and a core unit that calls into the GUI library fails to
# link here.
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    automation_test_utils.hpp
    test_command_catalog.cpp
    test_mcp_core.cpp
    test_mcp_http.cpp
    test_mcp_paths.cpp
    test_mcp_protocol.cpp
    test_mcp_replay.cpp
    test_mcp_replay_summary.cpp
    test_mcp_setup.cpp
    test_mcp_token.cpp
    )

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_automation libslic3r Catch2::Catch2WithMain)

set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
