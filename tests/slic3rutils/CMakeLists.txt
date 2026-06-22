get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_plugin_installer.cpp
    )

# TEST_TMP_DIR — build-local scratch dir for tests that author + extract
# zip archives at runtime. Build-tree path so we never spew into /tmp.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tests/slic3rutils/tmp")
target_compile_definitions(${_TEST_NAME}_tests PRIVATE
    "TEST_TMP_DIR=\"${CMAKE_BINARY_DIR}/tests/slic3rutils/tmp\"")

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
