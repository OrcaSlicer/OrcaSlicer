get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    test_3mf.cpp
    test_aabbindirect.cpp
    test_appconfig.cpp
    test_arachne_walls.cpp
    test_arrange.cpp
    test_bambu_networking.cpp
    test_buildvolume.cpp
    test_calib.cpp
    test_clipper_offset.cpp
    test_clipper_utils.cpp
    test_config.cpp
    test_config_variant_expansion.cpp
    test_toolordering_nozzle_group.cpp
    test_preset_bundle_loading.cpp
    test_preset_setting_id.cpp
    test_preset_diff.cpp
    test_vendor_cache.cpp
    test_elephant_foot_compensation.cpp
    test_fill_corner_smoothing.cpp
    test_filament_mixer.cpp
    test_fill_plane_path.cpp
    test_geometry.cpp
    test_localesutils.cpp
    test_multimaterial_segmentation.cpp
    test_placeholder_parser.cpp
    test_polygon.cpp
    test_mutable_polygon.cpp
    test_mutable_priority_queue.cpp
    test_nozzle_volume_type.cpp
    test_step.cpp
    test_stl.cpp
    test_triangle_selector.cpp
    test_meshboolean.cpp
    test_marchingsquares.cpp
    test_model.cpp
    test_utils.cpp
    test_timeutils.cpp
    test_voronoi.cpp
    test_optimizers.cpp
    test_ordering_strategies.cpp
    # test_png_io.cpp
    test_indexed_triangle_set.cpp
    ../libnest2d/printer_parts.cpp
    )

if (TARGET OpenVDB::openvdb)
    target_sources(${_TEST_NAME}_tests PRIVATE test_hollowing.cpp)
endif()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Each case needs a fresh process: Boost.Filesystem caches its path locale.
    add_test(NAME filesystem_locale_missing_measurement
        COMMAND ${_TEST_NAME}_tests "[LocalesUtils]")
    set_tests_properties(filesystem_locale_missing_measurement PROPERTIES
        ENVIRONMENT "LC_ALL=;LANG=C;LC_MEASUREMENT=orca_missing_locale.UTF-8"
        LABELS "LocalesUtils;Regression")

    add_test(NAME filesystem_locale_missing_override
        COMMAND ${_TEST_NAME}_tests "[LocalesUtils]")
    set_tests_properties(filesystem_locale_missing_override PROPERTIES
        ENVIRONMENT "LC_ALL=orca_missing_locale.UTF-8"
        LABELS "LocalesUtils;Regression")
endif()
