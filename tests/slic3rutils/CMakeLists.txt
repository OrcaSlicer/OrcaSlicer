get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_bambu_filament_ids.cpp
    test_bed_exclusion_fields.cpp
    test_cloud_delete_queue.cpp
    test_config_value_formatter.cpp
    test_creality_cfs_match.cpp
    test_creality_print.cpp
    test_dev_mapping.cpp
    test_device_access.cpp
    test_moonraker_printer_agent.cpp
    test_network_discovery.cpp
    test_filament_bitmap_utils.cpp
    test_colour_picker_field.cpp
    test_filament_color_picker.cpp
    test_ams_materials_setting.cpp
    test_gcode_preview_colors.cpp
    test_device_progress.cpp
    test_device_manager_integration.cpp
    test_web_media_controller.cpp
    test_scene_raycaster.cpp
    test_scene_benchmark.cpp
    test_lazy.cpp
    test_prebuild_queue.cpp
    test_staged_build.cpp
    test_network_versions.cpp
    test_orca_cloud_agent.cpp
    test_action_source.cpp
    test_option_state_registry.cpp
    test_plugin_host_api.cpp
    # Exercise seam enums and predicates through the embedded Python host API.
    test_precise_seam_plugin.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_status.cpp
    test_printer_agent.cpp
    test_qidi_printer_agent.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_plugin_resource_texture.cpp
    test_plugin_printer_agent.cpp
    test_printhost.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    test_plugin_audit.cpp
    test_plugin_json_depth.cpp
    test_shortcuts.cpp
    test_texture_color_palette.cpp
    test_texture_island_view.cpp
    test_timelapse_codecs.cpp
    test_file_url.cpp
    test_imgui_wrapper.cpp
    test_user_manager.cpp
    test_sequential_clearance.cpp
    test_object_list.cpp
    ../fff_print/test_helpers.cpp
    )

if (SLIC3R_CAD)
    target_sources(${_TEST_NAME}_tests PRIVATE test_design_sketch_tool.cpp)
endif ()

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r pybind11::embed Catch2::Catch2WithMain)
if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(${_TEST_NAME}_tests ${CMAKE_SOURCE_DIR}/src/slic3r/pchheader.hpp FORCEINCLUDE)
endif ()

set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls(${_TEST_NAME}_tests)

# Native visibility regression for option-group ShowItems passes. No slicer app
# or user profile is started. Headless environments report an explicit skip.
# Resolve wx here as well: src/ discovers it in a sibling directory, so its
# module-mode include paths and definitions are not visible to this target.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(wxWidgets_CONFIG_OPTIONS "--toolkit=gtk${SLIC3R_GTK}")
    find_package(wxWidgets REQUIRED COMPONENTS core base)
else ()
    find_package(wxWidgets CONFIG QUIET COMPONENTS core base)
    if (NOT wxWidgets_FOUND)
        find_package(wxWidgets REQUIRED COMPONENTS core base)
    endif ()
endif ()
if (wxWidgets_USE_FILE)
    include(${wxWidgets_USE_FILE})
endif ()
add_executable(support_override_notice_tests test_support_override_notice.cpp)
target_include_directories(support_override_notice_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${wxWidgets_INCLUDE_DIRS})
target_compile_definitions(support_override_notice_tests PRIVATE ${wxWidgets_DEFINITIONS} wxDEBUG_LEVEL=0)
set(_SUPPORT_NOTICE_WX_LIBS ${wxWidgets_LIBRARIES})
if (APPLE)
    list(REMOVE_ITEM _SUPPORT_NOTICE_WX_LIBS OpenGL.framework)
endif()
find_package(OpenGL REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(support_override_notice_tests PRIVATE
    ${_SUPPORT_NOTICE_WX_LIBS} OpenGL::GL JPEG::JPEG PNG::PNG ZLIB::ZLIB ${EXPAT_LIBRARIES})
add_test(NAME support_override_notice COMMAND support_override_notice_tests)
set_tests_properties(support_override_notice PROPERTIES SKIP_RETURN_CODE 77)

orcaslicer_copy_test_dlls(support_override_notice_tests)

if (WIN32)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_PREFIX_PATH}/libpython" "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_PREFIX_PATH}/libpython/python${_bundled_python_abi}.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140_1.dll"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>"
        COMMENT "Copying Python runtime for slic3rutils plugin host API tests"
        VERBATIM
    )
elseif (NOT FLATPAK)
    # The CI unit-test runner only receives the build/tests tree, so both the
    # interpreter and the libpython the test binary links have to travel next to
    # the executable; find_bundled_python_home() picks the copy up from there.
    if (APPLE)
        target_link_options(${_TEST_NAME}_tests PRIVATE
            "LINKER:-rpath,@executable_path/python/lib")
    else ()
        set_property(TARGET ${_TEST_NAME}_tests APPEND PROPERTY BUILD_RPATH "$ORIGIN/python/lib")
    endif ()

    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Copying Python runtime for the plugin host API tests"
        VERBATIM
    )
elseif (FLATPAK)
    # Same <testdir>/python home as WIN32/APPLE; symlink since /app/libpython
    # already ships in the flatpak (the test exe links libpython3.12.so from it).
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Linking Python runtime for flatpak plugin host API tests"
        VERBATIM
    )
endif()

# scripts/run_unit_tests.sh installs the tests' numpy into that runtime with this uv,
# staged where the app build tree keeps it (<exe dir>/tools/uv, see src/CMakeLists.txt).
if (ORCA_BUNDLED_UV_EXECUTABLE)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORCA_BUNDLED_UV_EXECUTABLE}"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv/${ORCA_BUNDLED_UV_FILENAME}"
        COMMENT "Copying uv for the plugin host API tests"
        VERBATIM
    )
endif()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
