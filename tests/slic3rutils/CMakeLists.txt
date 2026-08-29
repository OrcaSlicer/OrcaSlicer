get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_dev_mapping.cpp
    test_filament_bitmap_utils.cpp
    test_device_progress.cpp
    test_network_versions.cpp
    test_action_source.cpp
    test_plugin_host_api.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_status.cpp
    test_printer_agent.cpp
    test_qidi_printer_agent.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    test_plugin_audit.cpp
    ../fff_print/test_helpers.cpp
    )

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r pybind11::embed Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_stage_test_python_runtime(${_TEST_NAME}_tests)

orcaslicer_discover_tests(${_TEST_NAME}_tests)

# why: the loader runs on a detached worker thread, so its Python interpreter
# ownership model cannot share the embedded interpreter in the main test binary.
add_executable(printer_agent_plugin_tests test_printer_agent_plugin.cpp)

if (MSVC)
    target_link_libraries(printer_agent_plugin_tests Setupapi.lib)
endif ()

target_link_libraries(printer_agent_plugin_tests test_common libslic3r_gui libslic3r pybind11::embed Catch2::Catch2)
set_property(TARGET printer_agent_plugin_tests PROPERTY FOLDER "tests")

# why: both targets' post-build steps stage/clear the same python/ directory
# beside the shared test output dir; serialize them so the copies can't race.
add_dependencies(printer_agent_plugin_tests ${_TEST_NAME}_tests)

orcaslicer_copy_test_dlls()

orcaslicer_stage_test_python_runtime(printer_agent_plugin_tests)

orcaslicer_discover_tests(printer_agent_plugin_tests)
