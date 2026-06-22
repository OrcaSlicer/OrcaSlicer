get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
  ${_TEST_NAME}_tests.cpp
  test_geometry.cpp
  test_nfp.cpp
  test_nfp_placer.cpp
  printer_parts.cpp
  printer_parts.hpp
  libnest2d_test_utils.hpp
)

# libnest2d is now an INTERFACE (header-only) library and no longer carries a
# transitive libslic3r dependency, so this test — which includes libslic3r.h
# via printer_parts.cpp — must link libslic3r explicitly for its headers.
target_link_libraries(${_TEST_NAME}_tests test_common libnest2d libslic3r Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
