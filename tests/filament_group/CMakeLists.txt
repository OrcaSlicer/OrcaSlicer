get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

set(FG_GOLDEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/golden)
file(TO_NATIVE_PATH "${FG_GOLDEN_DIR}" FG_GOLDEN_DIR)

add_executable(${_TEST_NAME}_tests
    filament_group_regression_main.cpp
    )

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r nlohmann_json Catch2::Catch2WithMain)
# ${CMAKE_SOURCE_DIR}/deps_src puts <nlohmann/json.hpp> on the include path the same way the
# libslic3r translation units resolve it (via the admesh/.. system include); nlohmann_json's own
# interface dir only exposes <json.hpp>, so the source-tree include is required for the <nlohmann/…>
# spelling the serializers use.
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/deps_src)
target_compile_definitions(${_TEST_NAME}_tests PRIVATE
    FG_TEST_GOLDEN_DIR=R"\(${FG_GOLDEN_DIR}\)"
    )
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
