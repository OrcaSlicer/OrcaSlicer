get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests ${_TEST_NAME}_tests_main.cpp
    sla_print_tests.cpp
    sla_test_utils.hpp sla_test_utils.cpp
    sla_supptgen_tests.cpp
    sla_raycast_tests.cpp)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
	if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
		orcaslicer_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
	elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
		orcaslicer_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
	else()
		orcaslicer_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
	endif()
endif()

catch_discover_tests(${_TEST_NAME}_tests)
