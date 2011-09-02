get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
  ${_TEST_NAME}_tests_main.cpp
  printer_parts.cpp
  printer_parts.hpp
)

target_link_libraries(${_TEST_NAME}_tests test_common libnest2d Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

catch_discover_tests(${_TEST_NAME}_tests)
