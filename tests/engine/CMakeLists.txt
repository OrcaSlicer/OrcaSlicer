get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

# Build the fixture plugin used by test_plugin_loader.cpp first, so the engine
# tests target can depend on it.
add_subdirectory(fixtures/loader_plugin)
add_subdirectory(fixtures/profile_pack)
add_subdirectory(fixtures/native_noop)
add_subdirectory(fixtures/webview_noop)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    test_session.cpp
    test_events_bus.cpp
    test_c_abi_events_bridge.cpp
    test_plugin_loader.cpp
    test_plugin_registry.cpp
    test_pipeline_events.cpp
    test_slot_pipeline.cpp
    test_slot_gcode_filter.cpp
    test_slot_placeholder_provider.cpp
    test_slot_printer_agent.cpp
    test_profile_pack.cpp
    test_plugin_reload.cpp
    test_wasm_host.cpp
    test_wasm_plugin.cpp
    test_wasm_dispatch.cpp
    test_runtime_fixtures.cpp
    test_ui_slot_abi.cpp
    )

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
target_include_directories(${_TEST_NAME}_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    # engine-internal headers (PluginRegistry, etc.) — needed by test_plugin_registry.cpp
    ${CMAKE_SOURCE_DIR}/engine/src
    )

# Phase 3.2.3 — test_wasm_host.cpp uses wasmtime_wat2wasm directly to author
# an inline WAT fixture, so engine_tests needs its own view of the wasmtime
# C API. libslic3r links wasmtime PRIVATE so consumers don't inherit it.
find_path(WASMTIME_INCLUDE_DIR_TESTS wasmtime.h)
find_library(WASMTIME_LIBRARY_TESTS NAMES wasmtime)
if (WASMTIME_INCLUDE_DIR_TESTS AND WASMTIME_LIBRARY_TESTS)
    target_include_directories(${_TEST_NAME}_tests SYSTEM PRIVATE
        ${WASMTIME_INCLUDE_DIR_TESTS})
    target_link_libraries(${_TEST_NAME}_tests ${WASMTIME_LIBRARY_TESTS})
else ()
    message(FATAL_ERROR "engine_tests needs wasmtime headers + lib; "
        "re-run `cmake --build deps/build --target dep_Wasmtime`.")
endif ()

# Where the fixture plugin's binary + manifest.json land after build. The loader
# test reads $TEST_FIXTURES_DIR/loader_plugin/{<id>.so, manifest.json}. Match
# the staged dir set in fixtures/loader_plugin/CMakeLists.txt.
#
# TEST_TMP_DIR is the scratch directory the canary tests write G-code into.
# Lives under the build tree so we never spew into /tmp. Created up-front so
# the tests don't have to mkdir at runtime.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tests/engine/tmp")
target_compile_definitions(${_TEST_NAME}_tests PRIVATE
    "TEST_FIXTURES_DIR=\"${CMAKE_BINARY_DIR}/tests/engine/fixtures\""
    "TEST_TMP_DIR=\"${CMAKE_BINARY_DIR}/tests/engine/tmp\""
    )

# Ensure the fixture binary + its manifest copy step run before the tests build.
add_dependencies(${_TEST_NAME}_tests
    test_fixture_loader_plugin
    test_fixture_native_noop
    test_fixture_webview_noop)

set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

catch_discover_tests(${_TEST_NAME}_tests)
