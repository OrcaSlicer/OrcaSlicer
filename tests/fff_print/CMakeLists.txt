get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests 
	${_TEST_NAME}_tests.cpp
	test_data.cpp
	test_data.hpp
	test_extrusion_entity.cpp
	test_fill.cpp
	test_flow.cpp
	test_gcode.cpp
	test_gcodewriter.cpp
	test_model.cpp
	test_print.cpp
	test_printgcode.cpp
	test_printobject.cpp
	test_skirt_brim.cpp
	test_support_material.cpp
	test_trianglemesh.cpp
	)
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
